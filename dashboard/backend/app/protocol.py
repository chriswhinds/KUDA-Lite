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

"""KUDA-Lite wire protocol, the subset the dashboard needs: framing, Hello, GetTelemetry.

Mirrors src/common/protocol.h and src/common/telemetry.h; the authoritative description is
docs/PROTOCOL.md and docs/OBSERVABILITY.md. Encoders are included so tests and the mock
controller can produce byte-exact controller responses.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum

MAGIC = 0x31544C43  # bytes "CLT1"
PROTOCOL_VERSION = 1
FLAG_RESPONSE = 1
HEADER = struct.Struct("<IHHIiQQ")  # magic, version, type, flags, status, requestId, payloadLen
MAX_PAYLOAD = 1 << 30
TELEMETRY_VERSION = 2  # v2 appended `board`
CONTROLLER_NODE_ID = 0xFFFFFFFE
NEVER_SEEN = 0xFFFFFFFFFFFFFFFF


class MsgType(IntEnum):
    PING = 1
    HELLO = 100
    GET_TELEMETRY = 118


class NodeRole(IntEnum):
    WORKER = 0
    CONTROLLER = 1


class ProtocolError(Exception):
    """Malformed or unexpected bytes from the peer."""


# ---------------------------------------------------------------------------------------------
# Primitive readers / writers (little-endian)
# ---------------------------------------------------------------------------------------------


class Reader:
    def __init__(self, data: bytes) -> None:
        self._data = memoryview(data)
        self._pos = 0

    def _take(self, n: int) -> memoryview:
        if n > len(self._data) - self._pos:
            raise ProtocolError("truncated message")
        view = self._data[self._pos : self._pos + n]
        self._pos += n
        return view

    def _unpack(self, fmt: str) -> int | float:
        s = struct.Struct("<" + fmt)
        return s.unpack(self._take(s.size))[0]

    def u8(self) -> int:
        return int(self._unpack("B"))

    def u16(self) -> int:
        return int(self._unpack("H"))

    def u32(self) -> int:
        return int(self._unpack("I"))

    def u64(self) -> int:
        return int(self._unpack("Q"))

    def f32(self) -> float:
        return float(self._unpack("f"))

    def string(self) -> str:
        return bytes(self._take(self.u32())).decode("utf-8", errors="replace")

    def blob(self) -> bytes:
        return bytes(self._take(self.u32()))

    @property
    def remaining(self) -> int:
        return len(self._data) - self._pos


class Writer:
    def __init__(self) -> None:
        self._parts: list[bytes] = []

    def _pack(self, fmt: str, value: int | float) -> Writer:
        self._parts.append(struct.pack("<" + fmt, value))
        return self

    def u8(self, v: int) -> Writer:
        return self._pack("B", v)

    def u16(self, v: int) -> Writer:
        return self._pack("H", v)

    def u32(self, v: int) -> Writer:
        return self._pack("I", v)

    def u64(self, v: int) -> Writer:
        return self._pack("Q", v)

    def f32(self, v: float) -> Writer:
        return self._pack("f", v)

    def string(self, s: str) -> Writer:
        raw = s.encode("utf-8")
        self.u32(len(raw))
        self._parts.append(raw)
        return self

    def blob(self, b: bytes) -> Writer:
        self.u32(len(b))
        self._parts.append(b)
        return self

    def raw(self, b: bytes) -> Writer:
        self._parts.append(b)
        return self

    def bytes(self) -> bytes:
        return b"".join(self._parts)


# ---------------------------------------------------------------------------------------------
# Frames
# ---------------------------------------------------------------------------------------------


@dataclass
class FrameHeader:
    type: int
    flags: int
    status: int
    request_id: int
    payload_len: int

    @property
    def is_response(self) -> bool:
        return bool(self.flags & FLAG_RESPONSE)


def encode_frame(msg_type: int, request_id: int, payload: bytes = b"", *, flags: int = 0, status: int = 0) -> bytes:
    return HEADER.pack(MAGIC, PROTOCOL_VERSION, msg_type, flags, status, request_id, len(payload)) + payload


def decode_header(raw: bytes) -> FrameHeader:
    magic, version, msg_type, flags, status, request_id, payload_len = HEADER.unpack(raw)
    if magic != MAGIC:
        raise ProtocolError("bad frame magic")
    if version != PROTOCOL_VERSION:
        raise ProtocolError(f"unsupported protocol version {version}")
    if payload_len > MAX_PAYLOAD:
        raise ProtocolError("frame too large")
    return FrameHeader(msg_type, flags, status, request_id, payload_len)


# ---------------------------------------------------------------------------------------------
# Telemetry
# ---------------------------------------------------------------------------------------------


@dataclass
class TelemetrySample:
    timestamp_ms: int = 0
    uptime_ms: int = 0
    mem_total_bytes: int = 0
    mem_available_bytes: int = 0
    rss_bytes: int = 0
    process_threads: int = 0
    arena_bytes: int = 0
    cache_bytes: int = 0
    cache_capacity_bytes: int = 0
    cache_hits: int = 0
    cache_misses: int = 0
    compute_threads: int = 0
    busy_threads: int = 0
    cpu_cores: int = 0
    cpu_percent: float = 0.0
    load1: float = 0.0
    cpu_temp_c: float = -1.0
    cpu_freq_mhz: int = 0
    queue_depth: int = 0
    executing: int = 0
    execs_completed: int = 0
    blocks_executed: int = 0
    exec_busy_ns: int = 0
    net_rx_bytes: int = 0
    net_tx_bytes: int = 0
    current_kernel: str = ""
    board: str = ""  # v2: hardware model, e.g. "Orange Pi 6 Plus"


def decode_sample(r: Reader) -> TelemetrySample:
    body = Reader(r.blob())
    version = body.u16()
    if version < 1:
        raise ProtocolError("bad telemetry version")
    s = TelemetrySample(
        timestamp_ms=body.u64(),
        uptime_ms=body.u64(),
        mem_total_bytes=body.u64(),
        mem_available_bytes=body.u64(),
        rss_bytes=body.u64(),
        process_threads=body.u32(),
        arena_bytes=body.u64(),
        cache_bytes=body.u64(),
        cache_capacity_bytes=body.u64(),
        cache_hits=body.u64(),
        cache_misses=body.u64(),
        compute_threads=body.u32(),
        busy_threads=body.u32(),
        cpu_cores=body.u32(),
        cpu_percent=body.f32(),
        load1=body.f32(),
        cpu_temp_c=body.f32(),
        cpu_freq_mhz=body.u32(),
        queue_depth=body.u32(),
        executing=body.u32(),
        execs_completed=body.u64(),
        blocks_executed=body.u64(),
        exec_busy_ns=body.u64(),
        net_rx_bytes=body.u64(),
        net_tx_bytes=body.u64(),
        current_kernel=body.string(),
    )
    if version >= 2:
        s.board = body.string()
    # Fields appended by newer telemetry versions are ignored.
    return s


def encode_sample(w: Writer, s: TelemetrySample) -> None:
    body = (
        Writer()
        .u16(TELEMETRY_VERSION)
        .u64(s.timestamp_ms)
        .u64(s.uptime_ms)
        .u64(s.mem_total_bytes)
        .u64(s.mem_available_bytes)
        .u64(s.rss_bytes)
        .u32(s.process_threads)
        .u64(s.arena_bytes)
        .u64(s.cache_bytes)
        .u64(s.cache_capacity_bytes)
        .u64(s.cache_hits)
        .u64(s.cache_misses)
        .u32(s.compute_threads)
        .u32(s.busy_threads)
        .u32(s.cpu_cores)
        .f32(s.cpu_percent)
        .f32(s.load1)
        .f32(s.cpu_temp_c)
        .u32(s.cpu_freq_mhz)
        .u32(s.queue_depth)
        .u32(s.executing)
        .u64(s.execs_completed)
        .u64(s.blocks_executed)
        .u64(s.exec_busy_ns)
        .u64(s.net_rx_bytes)
        .u64(s.net_tx_bytes)
        .string(s.current_kernel)
        .string(s.board)
    )
    w.blob(body.bytes())


@dataclass
class NodeSnapshot:
    id: int
    role: NodeRole
    alive: bool
    hostname: str
    address: str
    arena_used_bytes: int
    last_seen_age_ms: int | None  # None = never reported
    samples: list[TelemetrySample] = field(default_factory=list)  # oldest first


@dataclass
class ClusterSnapshot:
    controller_uptime_ms: int
    sessions: int
    active_launches: int
    launches_total: int
    allocations: int
    allocated_bytes: int
    arena_total_bytes: int
    nodes: list[NodeSnapshot]


def decode_telemetry_response(payload: bytes) -> ClusterSnapshot:
    r = Reader(payload)
    uptime, sessions, active, total = r.u64(), r.u32(), r.u32(), r.u64()
    allocations, allocated, arena_total = r.u32(), r.u64(), r.u64()
    count = r.u32()
    if count > r.remaining:
        raise ProtocolError("bad node count")
    nodes: list[NodeSnapshot] = []
    for _ in range(count):
        node_id, role, alive = r.u32(), r.u8(), r.u8()
        hostname, address = r.string(), r.string()
        arena_used, age = r.u64(), r.u64()
        n = r.u32()
        samples = [decode_sample(r) for _ in range(n)]
        nodes.append(
            NodeSnapshot(
                id=node_id,
                role=NodeRole(role) if role in (0, 1) else NodeRole.WORKER,
                alive=bool(alive),
                hostname=hostname,
                address=address,
                arena_used_bytes=arena_used,
                last_seen_age_ms=None if age == NEVER_SEEN else age,
                samples=samples,
            )
        )
    return ClusterSnapshot(uptime, sessions, active, total, allocations, allocated, arena_total, nodes)


def encode_telemetry_response(snap: ClusterSnapshot) -> bytes:
    w = Writer()
    w.u64(snap.controller_uptime_ms).u32(snap.sessions).u32(snap.active_launches).u64(snap.launches_total)
    w.u32(snap.allocations).u64(snap.allocated_bytes).u64(snap.arena_total_bytes)
    w.u32(len(snap.nodes))
    for n in snap.nodes:
        w.u32(n.id).u8(int(n.role)).u8(1 if n.alive else 0).string(n.hostname).string(n.address)
        w.u64(n.arena_used_bytes)
        w.u64(NEVER_SEEN if n.last_seen_age_ms is None else n.last_seen_age_ms)
        w.u32(len(n.samples))
        for s in n.samples:
            encode_sample(w, s)
    return w.bytes()
