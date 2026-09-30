# KUDA-Lite Memory Model

The RAM of every worker is pooled into one **global memory** that the host sees as device memory and that every kernel, on every worker, can read and write. This document defines how that works.

Sources: [`address_map.*`](../src/common/address_map.h), [`dsm_client.*`](../src/common/dsm_client.h), [`page_cache.*`](../src/worker/page_cache.h), [`controller.cpp`](../src/controller/controller.cpp) (`allocate`/`release`).

## 1. Building blocks

| Term | Definition |
|---|---|
| **Arena** | One contiguous anonymous mapping on each worker (`--mem`, default 60% of RAM). Physical pages are committed on first touch. |
| **Arena offset** | Byte offset inside one worker's arena. This is the only address the data plane understands. |
| **GVA** | Global virtual address, a 64-bit `clDevPtr`. The controller hands out GVAs starting at `0x1000_0000_0000`, so 0 and small integers are never valid. |
| **Allocation** | A contiguous GVA range `[base, base+size)` cut into fixed-size **pages**, each of which lives on one worker |
| **Allocation record** | `{base, size, pageSize, owners[n], localBase[n]}`, all that is needed to translate any address in the allocation |

## 2. Address translation

For an allocation with `n = owners.size()` stripe members, byte `a` (with `base ≤ a < base+size`) lives at:

```
off   = a - base
page  = off / pageSize
j     = page % n                          # stripe member
owner = owners[j]
arena = localBase[j] + (page / n) * pageSize + off % pageSize
```

Each owner holds its pages **packed contiguously** in one slice that starts at `localBase[j]`. The slice is `pagesOnOwner(j) × pageSize` bytes, where

```
pagesOnOwner(j) = P / n + (j < P % n ? 1 : 0),    P = ceil(size / pageSize)
```

### Worked example

Three workers (ids 7, 3, 5), `pageSize = 4096`, `size = 10·4096 + 100` (11 pages, the last one 100 bytes), `localBase = {0, 1 MiB, 2 MiB}`:

```
page:   0    1    2    3    4    5    6    7    8    9    10
owner:  7    3    5    7    3    5    7    3    5    7    3
slot:   0    0    0    1    1    1    2    2    2    3    3
```

Page 4 lives on worker 3 at arena offset `1 MiB + 1·4096`. Workers 7 and 3 each hold 4 pages and worker 5 holds 3. A 200-byte access at `base + 4000` splits into 96 bytes on worker 7 (offset 4000) and 104 bytes on worker 3 (offset 1 MiB). All of this is checked in `tests/test_unit.cpp`.

`AddressMap::resolve()` produces these *extents*. `coalesceExtents()` merges neighbours that are contiguous on the same worker, which with one worker or a blocked layout turns an entire copy into a single extent.

## 3. Distributions

| `clDistribution` | Page size | Use it when |
|---|---|---|
| `clDistStriped` (default) | `--page-size` (64 KiB) or the value passed to `clMallocEx` (a multiple of 64) | You don't know or don't care about the access pattern. Every large access draws on all workers' bandwidth in parallel. |
| `clDistBlocked` | `ceil(size / n)` rounded up to 4 KiB, so each worker holds one contiguous block | Work is partitioned the same way as the data (owner-computes), e.g. a row-block of a matrix |

The first owner rotates from one allocation to the next, so many small allocations spread across the cluster instead of all landing on worker 0.

## 4. Allocation and free

`clMalloc` → controller `allocate()` (under `memMu_`):

1. Choose the page size and the stripe order over all **live** workers.
2. For each owner, place its slice in that worker's first-fit free list (64-byte alignment). If any slice does not fit, roll back and return `clErrorMemoryAllocation`.
3. Assign the GVA range. GVAs are never reused, and every allocation is followed by a ≥1 MiB unmapped gap, so an overrun past the end hits "invalid device pointer" instead of the neighbouring allocation.
4. **Publish** the record to every worker (`AllocAdd`) and wait for all acks. Only then is the pointer returned, so any worker can translate it before the host can use it.

`clFree` synchronises the session's streams, then removes the record, releases the slices and broadcasts `AllocRemove`.

Allocations belong to the host session that made them, and are all freed when it disconnects.

**Capacity.** `totalGlobalMem` is the sum of the arenas. A striped allocation needs about `size / n` of free space on *every* worker, so the largest allocation you can make is roughly `n × (smallest largest-hole)`. `cl-info` shows per-worker usage.

## 5. Consistency model

KUDA-Lite provides the same guarantees as CUDA global memory, and no more:

1. **Stream order.** Every operation in a stream (copy, memset, launch) sees all memory effects of the operations before it in that stream. Operations complete before their reply is sent.
2. **Cross-stream order** only through events (`clStreamWaitEvent`) or host synchronisation.
3. **Within a launch**, a block sees its own writes (the page cache invalidates written pages). A block must **not** rely on seeing writes made by *other* blocks of the same launch. There is no inter-block synchronisation.
4. **Write granularity.** A `ctx.write` becomes one `MemWriteV` per owning worker. Concurrent writes to the *same* bytes by different blocks have no guaranteed winner. Disjoint writes never interfere, even within the same page.
5. There are no global atomics in v1 ([roadmap](ROADMAP.md)).

## 6. Caching

A worker can cache **remote** pages during a launch:

- `ctx.read(..., clCache::Cached)` (the default) fetches the whole page(s) covering the range from their owners and keeps them. Later reads by *any* block of the same launch on that worker are served locally.
- Pages owned by the worker itself are never cached. They are read straight from the arena.
- Concurrent readers of the same missing page share one fetch (an in-flight future).
- Budget: `--cache` (default 15% of RAM), with FIFO eviction.
- **Lifetime: one launch.** When an `ExecBlocks` request carries a new launch id, the cache is cleared. Because of that, rule 1 of §5 holds with no invalidation traffic.
- `ctx.write` invalidates the written pages locally (rule 3).

Use `clCache::Uncached` for data that is read exactly once (streaming inputs, `C` in `beta·C`), so it doesn't evict useful pages.

## 7. Data-plane mechanics

`DsmClient` (used by the controller for host copies and by every worker for kernels):

1. Translate each requested range into extents and coalesce them.
2. Split pieces larger than 16 MiB and group them by owner.
3. For each owner, send batched `MemReadV`/`MemWriteV` messages (≤16 MiB of data each), with **all owners in parallel**.
4. Serve extents owned by the local worker with `memcpy`.
5. Wait for every reply. The first error wins.

So a `read2D` of 128 rows spread over 4 workers costs **one round trip** (4 concurrent messages), not 128.
