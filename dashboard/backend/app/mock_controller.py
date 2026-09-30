"""A stand-in KUDA-Lite controller that serves synthetic telemetry.

For developing the dashboard without a Pi cluster, and for the backend tests. It speaks the real
wire protocol (Hello, GetTelemetry, Ping), so the backend cannot tell it from a controller.

    python -m app.mock_controller --port 7070 --workers 6 [--offline 1]

The simulated cluster alternates between idle periods and "matmul" launches: during a launch
the workers' compute threads go busy, CPU and temperature climb, blocks and traffic accumulate.
"""

from __future__ import annotations

import argparse
import asyncio
import math
import random
import time
from collections import deque

from .protocol import (
    CONTROLLER_NODE_ID,
    HEADER,
    ClusterSnapshot,
    MsgType,
    NodeRole,
    NodeSnapshot,
    Reader,
    TelemetrySample,
    Writer,
    decode_header,
    encode_frame,
    encode_telemetry_response,
    FLAG_RESPONSE,
)

GIB = 1 << 30
MIB = 1 << 20
LAUNCH_PERIOD_S = 30.0
LAUNCH_LENGTH_S = 14.0


class SimNode:
    def __init__(self, node_id: int, role: NodeRole, hostname: str, address: str, heat_bias: float) -> None:
        self.id = node_id
        self.role = role
        self.hostname = hostname
        self.address = address
        self.heat_bias = heat_bias
        self.alive = True
        self.start = time.monotonic()
        self.temp = 45.0 + heat_bias
        self.samples: deque[TelemetrySample] = deque(maxlen=600)
        self.blocks = 0
        self.execs = 0
        self.busy_ns = 0
        self.rx = 0
        self.tx = 0
        self.hits = 0
        self.misses = 0

    def tick(self, now: float, busy: bool, dt: float) -> None:
        worker = self.role == NodeRole.WORKER
        load = (0.88 + random.uniform(-0.05, 0.05)) if busy and worker else random.uniform(0.02, 0.08)
        if not worker:
            load = random.uniform(0.15, 0.3) if busy else random.uniform(0.02, 0.06)
        target = 44.0 + self.heat_bias + 38.0 * load
        self.temp += (target - self.temp) * min(1.0, dt / 20.0)  # thermal lag
        if busy and worker:
            self.blocks += random.randint(6, 10)
            self.execs += 2
            self.busy_ns += int(dt * 1e9 * 0.95)
            self.rx += int(random.uniform(8, 14) * MIB * dt)
            self.tx += int(random.uniform(2, 4) * MIB * dt)
            self.hits += random.randint(900, 1200)
            self.misses += random.randint(10, 40)
        else:
            self.rx += int(2_000 * dt)
            self.tx += int(2_000 * dt)
        mem_total = 8 * GIB
        base_used = (1.2 if worker else 0.8) * GIB
        busy_used = 3.1 * GIB if busy and worker else 0.0
        self.samples.append(
            TelemetrySample(
                timestamp_ms=int(time.time() * 1000),
                uptime_ms=int((now - self.start) * 1000),
                mem_total_bytes=mem_total,
                mem_available_bytes=int(mem_total - base_used - busy_used - random.uniform(0, 60) * MIB),
                rss_bytes=int((40 + (2400 if busy and worker else 0)) * MIB + random.uniform(0, 8) * MIB),
                process_threads=11 if worker else 9,
                arena_bytes=int(4.8 * GIB) if worker else 0,
                cache_bytes=int(260 * MIB) if busy and worker else 0,
                cache_capacity_bytes=int(1.2 * GIB) if worker else 0,
                cache_hits=self.hits,
                cache_misses=self.misses,
                compute_threads=4 if worker else 0,
                busy_threads=(random.choice([3, 4, 4, 4]) if busy else 0) if worker else 0,
                cpu_cores=4,
                cpu_percent=100.0 * load,
                load1=4.0 * load,
                cpu_temp_c=self.temp,
                cpu_freq_mhz=2400 if self.temp < 80 else 1800,
                queue_depth=random.choice([0, 1]) if busy and worker else 0,
                executing=1 if busy else 0,
                execs_completed=self.execs,
                blocks_executed=self.blocks,
                exec_busy_ns=self.busy_ns,
                net_rx_bytes=self.rx,
                net_tx_bytes=self.tx,
                current_kernel="cl_sgemm" if busy and worker else "",
            )
        )


class MockCluster:
    def __init__(self, workers: int, offline: int) -> None:
        self.start = time.monotonic()
        self.nodes = [SimNode(CONTROLLER_NODE_ID, NodeRole.CONTROLLER, "pi5-ctl", "0.0.0.0:7070", 0.0)]
        for i in range(workers):
            # Worker 1 runs hot (e.g. a missing heatsink) so the warning state is visible.
            bias = 9.0 if i == 1 else random.uniform(-2.0, 2.0)
            self.nodes.append(SimNode(i, NodeRole.WORKER, f"pi5-w{i}", f"10.0.0.{11 + i}:7100", bias))
        for node in self.nodes[len(self.nodes) - offline :]:
            if node.role == NodeRole.WORKER:
                node.alive = False
        self.sessions = 0
        self.launches = 0
        self.last = time.monotonic()
        self.tick()

    def busy(self, now: float) -> bool:
        return (now - self.start) % LAUNCH_PERIOD_S < LAUNCH_LENGTH_S

    def tick(self) -> None:
        now = time.monotonic()
        dt = max(0.001, now - self.last)
        self.last = now
        busy = self.busy(now)
        self.launches = int((now - self.start) // LAUNCH_PERIOD_S) + (1 if busy else 0)
        for node in self.nodes:
            if node.alive:
                node.tick(now, busy, dt)

    def snapshot(self, max_history: int) -> ClusterSnapshot:
        now = time.monotonic()
        busy = self.busy(now)
        workers = [n for n in self.nodes if n.role == NodeRole.WORKER and n.alive]
        arena_used = int(0.75 * GIB) if busy else 0
        nodes = []
        for n in self.nodes:
            samples = list(n.samples)[-max(1, max_history) :]
            nodes.append(
                NodeSnapshot(
                    id=n.id,
                    role=n.role,
                    alive=n.alive,
                    hostname=n.hostname,
                    address=n.address,
                    arena_used_bytes=arena_used if n.role == NodeRole.WORKER and n.alive else 0,
                    last_seen_age_ms=int((now - self.last) * 1000) + random.randint(0, 300) if n.samples else None,
                    samples=samples,
                )
            )
        return ClusterSnapshot(
            controller_uptime_ms=int((now - self.start) * 1000),
            sessions=self.sessions,
            active_launches=1 if busy else 0,
            launches_total=self.launches,
            allocations=3 if busy else 0,
            allocated_bytes=arena_used * len(workers),
            arena_total_bytes=int(4.8 * GIB) * len(workers),
            nodes=nodes,
        )


async def serve(host: str, port: int, cluster: MockCluster, ready: asyncio.Event | None = None) -> None:
    async def ticker() -> None:
        while True:
            await asyncio.sleep(1.0)
            cluster.tick()

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        cluster.sessions += 1
        try:
            while True:
                header = decode_header(await reader.readexactly(HEADER.size))
                payload = await reader.readexactly(header.payload_len)
                if header.is_response:
                    continue
                if header.type == MsgType.HELLO:
                    body = Writer().u64(cluster.sessions).bytes()
                elif header.type == MsgType.GET_TELEMETRY:
                    body = encode_telemetry_response(cluster.snapshot(Reader(payload).u32()))
                elif header.type == MsgType.PING:
                    body = b""
                else:
                    writer.write(encode_frame(header.type, header.request_id, flags=FLAG_RESPONSE, status=11))
                    await writer.drain()
                    continue
                writer.write(encode_frame(header.type, header.request_id, body, flags=FLAG_RESPONSE))
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            cluster.sessions -= 1
            writer.close()

    server = await asyncio.start_server(handle, host, port)
    tick_task = asyncio.create_task(ticker())
    if ready is not None:
        ready.set()
    try:
        async with server:
            await server.serve_forever()
    finally:
        tick_task.cancel()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--offline", type=int, default=0, help="how many workers to show as disconnected")
    args = parser.parse_args()
    cluster = MockCluster(args.workers, args.offline)
    print(f"mock KUDA-Lite controller on {args.host}:{args.port} with {args.workers} simulated Pi 5 workers")
    asyncio.run(serve(args.host, args.port, cluster))


if __name__ == "__main__":
    main()
