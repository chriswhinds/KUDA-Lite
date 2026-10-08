# Copyright 2026 Christopher Hinds, Stratum Labs llc
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""A stand-in KUDA-Lite controller that serves synthetic telemetry.

For developing the dashboard without hardware, and for the backend tests. It speaks the real
wire protocol (Hello, GetTelemetry, Ping), so the backend cannot tell it from a controller.

    python -m app.mock_controller --port 7070 --workers 6 [--offline 1] [--platform opi6plus]

--platform picks the simulated boards: pi5 (Raspberry Pi 5, default), opi6plus (Orange Pi 6
Plus), or mixed (a Raspberry Pi 5 controller with Orange Pi 6 Plus workers).

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
from dataclasses import dataclass

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


@dataclass(frozen=True)
class Board:
    """Simulation profile of one board type (figures follow docs/POWER*.md and docs/PLATFORMS.md)."""

    name: str
    prefix: str  # hostname prefix
    cores: int
    mem_gib: float
    arena_gib: float
    cache_gib: float
    idle_used_gib: float
    busy_used_gib: float
    temp_idle_c: float
    temp_gain_c: float  # added at 100 % load
    freq_mhz: int
    rx_mib_s: tuple[float, float]  # data-plane receive rate while busy
    blocks_per_s: tuple[int, int]
    threads: int  # OS threads in cl-worker


BOARDS = {
    "pi5": Board("Raspberry Pi 5 Model B Rev 1.0", "pi5", 4, 8, 4.8, 1.2, 1.2, 3.1, 44, 38, 2400,
                 (8, 14), (6, 10), 11),
    "opi6plus": Board("Orange Pi 6 Plus", "opi6", 12, 64, 38.4, 9.6, 2.5, 22.0, 40, 22, 2600,
                      (60, 110), (40, 70), 19),
}


class SimNode:
    def __init__(self, node_id: int, role: NodeRole, hostname: str, address: str, heat_bias: float,
                 board: Board) -> None:
        self.board = board
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
        b = self.board
        target = b.temp_idle_c + self.heat_bias + b.temp_gain_c * load
        self.temp += (target - self.temp) * min(1.0, dt / 20.0)  # thermal lag
        if busy and worker:
            self.blocks += random.randint(*b.blocks_per_s)
            self.execs += 2
            self.busy_ns += int(dt * 1e9 * 0.95)
            self.rx += int(random.uniform(*b.rx_mib_s) * MIB * dt)
            self.tx += int(random.uniform(*b.rx_mib_s) * 0.3 * MIB * dt)
            self.hits += random.randint(900, 1200)
            self.misses += random.randint(10, 40)
        else:
            self.rx += int(2_000 * dt)
            self.tx += int(2_000 * dt)
        mem_total = int(b.mem_gib * GIB)
        base_used = (b.idle_used_gib if worker else b.idle_used_gib * 0.7) * GIB
        busy_used = b.busy_used_gib * GIB if busy and worker else 0.0
        self.samples.append(
            TelemetrySample(
                timestamp_ms=int(time.time() * 1000),
                uptime_ms=int((now - self.start) * 1000),
                mem_total_bytes=mem_total,
                mem_available_bytes=int(mem_total - base_used - busy_used - random.uniform(0, 60) * MIB),
                rss_bytes=int((40 + (b.busy_used_gib * 780 if busy and worker else 0)) * MIB + random.uniform(0, 8) * MIB),
                process_threads=b.threads if worker else 9,
                arena_bytes=int(b.arena_gib * GIB) if worker else 0,
                cache_bytes=int(b.cache_gib * 0.2 * GIB) if busy and worker else 0,
                cache_capacity_bytes=int(b.cache_gib * GIB) if worker else 0,
                cache_hits=self.hits,
                cache_misses=self.misses,
                compute_threads=b.cores if worker else 0,
                busy_threads=(random.choice([b.cores - 1, b.cores, b.cores, b.cores]) if busy else 0) if worker else 0,
                cpu_cores=b.cores,
                cpu_percent=100.0 * load,
                load1=b.cores * load,
                cpu_temp_c=self.temp,
                cpu_freq_mhz=b.freq_mhz if self.temp < 80 else int(b.freq_mhz * 0.75),
                queue_depth=random.choice([0, 1]) if busy and worker else 0,
                executing=1 if busy else 0,
                execs_completed=self.execs,
                blocks_executed=self.blocks,
                exec_busy_ns=self.busy_ns,
                net_rx_bytes=self.rx,
                net_tx_bytes=self.tx,
                current_kernel="cl_sgemm" if busy and worker else "",
                board=b.name,
            )
        )


class MockCluster:
    def __init__(self, workers: int, offline: int, platform: str = "pi5") -> None:
        if platform not in ("pi5", "opi6plus", "mixed"):
            raise ValueError(f"unknown platform {platform!r}")
        ctl_board = BOARDS["pi5" if platform == "mixed" else platform]
        worker_board = BOARDS["opi6plus" if platform == "mixed" else platform]
        self.start = time.monotonic()
        self.nodes = [SimNode(CONTROLLER_NODE_ID, NodeRole.CONTROLLER, f"{ctl_board.prefix}-ctl", "0.0.0.0:7070",
                              0.0, ctl_board)]
        for i in range(workers):
            # Worker 1 runs hot (e.g. a missing heatsink) so the warning state is visible.
            bias = (9.0 if worker_board.prefix == "pi5" else 14.0) if i == 1 else random.uniform(-2.0, 2.0)
            self.nodes.append(SimNode(i, NodeRole.WORKER, f"{worker_board.prefix}-w{i}", f"10.0.0.{11 + i}:7100",
                                      bias, worker_board))
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

        def arena_used(n: SimNode) -> int:
            return int(n.board.arena_gib * 0.16 * GIB) if busy else 0

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
                    arena_used_bytes=arena_used(n) if n.role == NodeRole.WORKER and n.alive else 0,
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
            allocated_bytes=sum(arena_used(n) for n in workers),
            arena_total_bytes=sum(int(n.board.arena_gib * GIB) for n in workers),
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
    parser.add_argument("--platform", choices=["pi5", "opi6plus", "mixed"], default="pi5",
                        help="simulated boards (mixed = Raspberry Pi 5 controller, Orange Pi 6 Plus workers)")
    args = parser.parse_args()
    cluster = MockCluster(args.workers, args.offline, args.platform)
    print(f"mock KUDA-Lite controller on {args.host}:{args.port}: {args.workers} simulated {args.platform} workers")
    asyncio.run(serve(args.host, args.port, cluster))


if __name__ == "__main__":
    main()
