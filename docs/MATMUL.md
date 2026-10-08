<!--
Copyright 2026 Christopher Hinds, Stratum Labs llc
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# Test Case 1: Matrix Multiplication

`C = α·A·B + β·C`, with A (M×K), B (K×N) and C (M×N) row-major, in `float` (`cl_sgemm`) or `double` (`cl_dgemm`).

Sources: kernel [`blas_kernels.cpp`](../src/worker/kernels/blas_kernels.cpp), host wrapper [`clblas.cpp`](../src/host/clblas.cpp), test [`test_matmul.cpp`](../tests/test_matmul.cpp).

## 1. Decomposition

C is cut into **T×T output tiles** (T = 128 when M, N ≥ 512, otherwise 64; override with `$KUDALITE_GEMM_TILE`). One tile is one block:

```
grid  = (ceil(N/T), ceil(M/T))        blockIdx.x → tile column, blockIdx.y → tile row
block = (T, T)                        tile width, tile height
```

```
            B  (K × N)
          ┌────┬────┬────┐
          │    │ ▒▒ │    │  ← B column panel (K × T), fetched once per worker and cached
          └────┴────┴────┘
 A (M×K)          C (M × N)
┌────────┐      ┌────┬────┬────┐
│▓▓▓▓▓▓▓▓│  →   │    │ ██ │    │  ← block (x=1, y=1) computes this T×T tile
└────────┘      └────┴────┴────┘
 ↑ A row panel (T × K), reused by consecutive blocks of the same row
```

## 2. Per-block algorithm

1. `read2D` the A row panel (`rows × K`) and the B column panel (`K × cols`) into scratch. Both reads are **cached**. If `β ≠ 0`, also read the C tile, **uncached** since it is used once.
2. `parallelFor` over the tile's rows, so each core owns a contiguous band of rows:
   - `k` runs in panels of 128 (`kKBlock`), so a 128×T slice of B (64 KiB for T=128 floats) stays in the Cortex-A76's 512 KiB L2 while every row in the band streams over it;
   - the inner loop `c[j] += a_ik · b_kj` is contiguous in `j`, so GCC vectorises it (NEON on Arm boards, SSE/AVX on x86);
   - epilogue: `C = α·acc + β·C`. When β = 0, C is never read, which matches BLAS semantics (NaNs in C are ignored).
3. `write2D` the tile back. Each row segment goes to the worker(s) that own it, in one batched message per owner.

## 3. Data movement

With striped 64 KiB pages and N = 4096 (16 KiB per B row), each B page holds 4 full rows. A B column panel therefore touches **every** page of B. The effect of the page cache:

- the **first** block on a worker pulls all of B into its cache (one batched transfer from all owners in parallel);
- every later block on that worker gets B from the cache, and only the A panel for each new tile row is fetched;
- consecutive blocks in a chunk share a tile row, so they reuse the A panel.

Per launch, each of the W workers receives approximately:

| Data | Bytes received by one worker |
|---|---|
| B (whole, minus its own 1/W) | `(1 − 1/W)·K·N·s` |
| A rows for its tiles | `≈ (1 − 1/W)·M·K·s / W` |
| C tiles written to remote owners (sent) | `≈ (1 − 1/W)·M·N·s / W` |

where `s` is the element size. B is replicated to every worker, which is the classic trade-off for 1-D distribution. It costs `K·N·s` of network per worker and makes all the compute local afterwards.

The **cache budget** must hold B plus the A panels in flight. The default (15 % of RAM: ≈ 1.2 GB on an 8 GB Pi 5, ≈ 9.6 GB on a 64 GB Orange Pi 6 Plus) handles `float` B up to about 17k × 17k and 49k × 49k respectively. If B does not fit, pages are evicted FIFO and refetched. That is still correct, just slower.

## 4. Performance model

All figures are **estimates**, to be replaced with measurements once the code runs on real hardware ([TESTING.md §4](TESTING.md#4-hardware-acceptance-plan)). Platform details are in [PLATFORMS.md](PLATFORMS.md).

| Quantity | Raspberry Pi 5 | Orange Pi 6 Plus |
|---|---|---|
| Cores | 4 × Cortex-A76 @ 2.4 GHz | 8 × Cortex-A720 @ 2.4–2.6 GHz + 4 × Cortex-A520 @ 1.8 GHz |
| Peak FP32 (2 × 128-bit FMA per big core) | ≈ 150 GFLOP/s | ≈ 380 GFLOP/s (≈ 320 from the A720s) |
| Measured Linpack (FP64, one board) | ≈ 27 GFLOP/s | ≈ 135 GFLOP/s (Orange Pi 6, same SoC) |
| This kernel (auto-vectorised, not hand-tuned), expected | ≈ 8–20 GFLOP/s per board | ≈ 40–80 GFLOP/s per board |
| Network per node | 1 GbE ≈ 110 MB/s per direction | 5 GbE ≈ 550 MB/s per direction |
| Remote-page cache (default 15 % of RAM) | 1.2 GB (8 GB board) | 9.6 GB (64 GB board) |

### 4.1 Raspberry Pi 5 cluster (4 workers at ~10 GFLOP/s each, 1 GbE)

| n | FLOPs | Compute | B broadcast per worker | Host copies via controller (A, B in; C out) | Kernel time estimate |
|---|---|---|---|---|---|
| 1024 | 2.1 G | ~55 ms | 3 MB → ~30 ms | 12 MB → ~0.1 s | ~0.1 s |
| 2048 | 17 G | ~0.45 s | 12 MB → ~0.11 s | 48 MB → ~0.45 s | ~0.6 s |
| 4096 | 137 G | ~3.4 s | 48 MB → ~0.45 s | 192 MB → ~1.8 s | ~4 s |
| 8192 | 1.1 T | ~27 s | 192 MB → ~1.8 s | 768 MB → ~7 s | ~30 s |

### 4.2 Orange Pi 6 Plus cluster (4 workers at ~50 GFLOP/s each, 5 GbE)

| n | FLOPs | Compute | B broadcast per worker | Host copies via controller | Kernel time estimate |
|---|---|---|---|---|---|
| 1024 | 2.1 G | ~11 ms | 3 MB → ~5 ms | 12 MB → ~22 ms | ~20 ms |
| 2048 | 17 G | ~86 ms | 12 MB → ~22 ms | 48 MB → ~87 ms | ~0.11 s |
| 4096 | 137 G | ~0.7 s | 48 MB → ~87 ms | 192 MB → ~0.35 s | ~0.8 s |
| 8192 | 1.1 T | ~5.5 s | 192 MB → ~0.35 s | 768 MB → ~1.4 s | ~6 s |
| 16384 | 8.8 T | ~44 s | 768 MB → ~1.4 s | 3.2 GB → ~6 s | ~46 s |

The host-copy column assumes the host machine and the controller both have a ≥ 5 GbE link. With a 1 GbE host link (or a Raspberry Pi 5 controller in a mixed cluster), host copies take the 1 GbE times from §4.1.

### 4.3 What this shows

- Arithmetic intensity grows with n, so large problems are **compute-bound** and scale with the number of workers. Small problems (n ≲ 1024 on the Pi 5, n ≲ 2048 on the Orange Pi) are latency- and network-bound.
- **The Orange Pi 6 Plus is about 5× faster per board and has 5× the network.** Compute and communication grow by similar factors, so the compute-to-communication balance is similar to the Pi 5's. Its much larger memory and cache (B up to ≈ 49k × 49k floats fits in one worker's cache) let it take problems the Pi cluster cannot hold.
- **Host transfers through the controller** are the other big cost on both platforms. A direct host→worker data path is on the roadmap.
- **Hand-written micro-kernels** could reach 30–60 GFLOP/s per Pi 5 (NEON) or 150+ GFLOP/s per Orange Pi (SVE2/NEON on the A720s), which would make the network matter more. On the Orange Pi, low-precision GEMM could also go to the NPU ([PLATFORMS.md §6](PLATFORMS.md#6-the-npu)).
- **Mixed fast and slow cores:** `parallelFor` hands each worker thread small chunks of rows, so the A520 cores contribute without holding up the A720s ([PLATFORMS.md §3.1](PLATFORMS.md#31-heterogeneous-cores)).

## 5. Verification method

`test_matmul` compares every element (or 64 random rows once M·N·K > 2³¹) with a double-precision reference, using the standard forward error bound for a length-K dot product:

```
|C_ij − C_ref_ij|  ≤  4·(K+2)·ε · ( |α|·Σ_k |a_ik·b_kj| + |β·C_ij| )
```

The test reports the worst ratio `error / bound` and passes if it is ≤ 1. Typical observed ratios are 0.000–0.005, far inside the bound. Cases covered:

- square sizes 64, 512, 1024 (and any `--sizes` you pass);
- non-tile-multiple shapes (`257×129×300`), which exercise partial edge tiles;
- `α=1, β=0` and `α=0.5, β=2`, which exercise the β path and the uncached C read;
- `dgemm`, to check that the same template works for `double`.

## 6. Tuning knobs

| Knob | Where | Effect |
|---|---|---|
| Tile size T | `$KUDALITE_GEMM_TILE` | Larger T: fewer blocks, more reuse per fetch, but less load balancing |
| Page size | `cl-controller --page-size` or `clMallocEx` | Larger pages mean fewer, bigger messages, but coarser caching |
| Chunk factor | `cl-controller --chunk-factor` | More chunks balance better but cost more RPCs |
| Cache size | `cl-worker --cache` | Must hold B for the one-fetch behaviour |
| Threads | `cl-worker --threads` | Leave at all cores (4 on a Pi 5, 12 on an Orange Pi 6 Plus) |
