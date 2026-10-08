<!--
Copyright 2026 Christopher Hinds, Stratum Labs llc
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# KUDA-Lite Wire Protocol (version 1)

Sources: [`protocol.h`](../src/common/protocol.h), [`rpc.cpp`](../src/common/rpc.cpp).

## 1. Transport and framing

- TCP with `TCP_NODELAY`. Every connection is full-duplex and **multiplexed**: either side may send requests at any time, and responses carry the request's id.
- All integers are **little-endian**. Both supported CPU families (x86-64, AArch64) are little-endian, and a `static_assert` enforces it.
- Each message is a 32-byte header followed by `payloadLen` bytes:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `0x31544C43` (bytes `C L T 1`) |
| 4 | 2 | `version` | `1`. A mismatch closes the connection. |
| 6 | 2 | `type` | `MsgType` (tables below) |
| 8 | 4 | `flags` | bit 0 = response |
| 12 | 4 | `status` | `clError_t` (responses) |
| 16 | 8 | `requestId` | assigned by the requester, echoed in the response |
| 24 | 8 | `payloadLen` | at most 1 GiB |

### Payload encoding primitives

| Notation | Encoding |
|---|---|
| `u8/u16/u32/u64`, `f64` | little-endian fixed width |
| `str` | `u32 len` + bytes (not NUL-terminated) |
| `blob` | `u32 len` + bytes |
| `dim3` | `u32 x, u32 y, u32 z` |
| `extent` | `u64 arenaOffset, u64 len` |

A response has the same `type` as its request, and its payload is only meaningful when `status == 0`. A truncated payload is answered with `clErrorProtocol`.

## 2. Host runtime → controller (port 7070)

| Type | Id | Request payload | Response payload | Stream op |
|---|---|---|---|---|
| `Ping` | 1 | – | – | |
| `Hello` | 100 | `str clientName` | `u64 sessionId` | |
| `GetDeviceProps` | 101 | – | `str name, u32 workers, u32 cores, u64 totalMem, u64 freeMem, u32 pageSize, u32 protoVersion` | |
| `GetWorkers` | 102 | – | `u32 n`, then n × `{u32 id, str hostname, str address, u16 dataPort, u32 cores, u64 memBytes, u64 memUsed}` | |
| `Malloc` | 103 | `u64 size, u32 distribution, u32 pageSize` | `u64 devPtr` | |
| `Free` | 104 | `u64 devPtr` | – | |
| `MemcpyH2D` | 110 | `u32 stream, u64 dst, u64 len, len bytes` | – | ✔ |
| `MemcpyD2H` | 111 | `u32 stream, u64 src, u64 len` | `len bytes` | ✔ |
| `MemcpyD2D` | 112 | `u32 stream, u64 dst, u64 src, u64 len` | – | ✔ |
| `Memset` | 113 | `u32 stream, u64 dst, u8 value, u64 len` | – | ✔ |
| `Launch` | 114 | `u32 stream, str kernel, dim3 grid, dim3 block, blob args` | `u64 blocks, u32 workersUsed, f64 wallMs` | ✔ |
| `EventRecord` | 115 | `u32 stream, u32 eventId` | `u64 timestampNs` (controller steady clock) | ✔ |
| `StreamWaitEvent` | 116 | `u32 stream, u32 eventId` | – | ✔ |
| `StreamDestroy` | 117 | `u32 stream` | – | |
| `GetTelemetry` | 118 | `u32 maxHistory` | cluster counters + per-node history ([OBSERVABILITY.md §4](OBSERVABILITY.md#4-wire-format)) | |

**Stream ops** are queued on the named stream and answered when they **complete**. Stream ids and event ids are chosen by the host and are local to its session. The controller creates a stream on first use, and stream 0 is the default stream.

The host runtime splits copies into requests of ≤ 8 MiB.

## 3. Worker ↔ controller control plane (port 7071)

The worker opens the connection. After that, requests flow in both directions.

| Type | Id | Direction | Request payload | Response payload |
|---|---|---|---|---|
| `Register` | 200 | W → C | `str hostname, str dataHost, u16 dataPort, u64 arenaBytes, u32 cores, u32 k, k × str kernelName` | `u32 workerId` |
| `ClusterMap` | 201 | C → W | `u32 n`, n × `{u32 id, str host, u16 port}` | – |
| `AllocAdd` | 202 | C → W | `u64 base, u64 size, u64 pageSize, u32 n, n × u32 owner, n × u64 localBase` | – |
| `AllocRemove` | 203 | C → W | `u64 base` | – |
| `ExecBlocks` | 204 | C → W | `u64 launchId, str kernel, dim3 grid, dim3 block, blob args, u64 blockBegin, u64 blockEnd` | `u64 elapsedNs, u64 blocksRun` |
| `Telemetry` | 205 | W → C | one telemetry sample blob (version 2 since 0.3: adds the board model), every `--telemetry-ms` ([OBSERVABILITY.md §4](OBSERVABILITY.md#4-wire-format)) | – |

Ordering guarantees the design relies on:

- After the `Register` reply the controller sends `AllocAdd` for every existing allocation, then `ClusterMap`, all on the same connection, so they are processed in that order.
- `ClusterMap` broadcasts are serialised, so every worker sees membership changes in the same order.
- `ExecBlocks` is answered only after the blocks finish. Error statuses: `clErrorKernelNotFound`, `clErrorInvalidValue`, `clErrorLaunchFailure`, or the memory error the kernel hit.

## 4. Data plane (port 7100)

Served by every worker. Clients are the controller (host copies) and peer workers (kernels). Offsets are **arena offsets**; translation happened on the client. Every extent is bounds-checked against the arena, and any out-of-range extent fails the whole request with `clErrorInvalidDevicePointer`.

| Type | Id | Request payload | Response payload |
|---|---|---|---|
| `MemReadV` | 300 | `u32 n, n × extent` | the n ranges concatenated |
| `MemWriteV` | 301 | `u32 n, n × extent`, then the n ranges' data concatenated | – |
| `MemFillV` | 302 | `u8 value, u32 n, n × extent` | – |

Clients batch at most 16 MiB of data per message.

## 5. Error codes

`status` carries a `clError_t` ([`cl_types.h`](../include/kudalite/cl_types.h)). If a connection is lost, every outstanding request on it completes locally with `clErrorNetwork`.

## 6. Versioning

The header `version` must match exactly. Any incompatible payload change bumps `kProtocolVersion`. Hosts and daemons from different KUDA-Lite versions refuse each other at the first frame instead of misinterpreting data.
