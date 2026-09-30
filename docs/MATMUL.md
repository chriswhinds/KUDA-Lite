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
   - the inner loop `c[j] += a_ik · b_kj` is contiguous in `j`, so GCC vectorises it (NEON on the Pi, SSE/AVX on x86);
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

The **cache budget** must hold B plus the A panels in flight. The default (15% of an 8 GB Pi, ≈1.2 GB) handles `float` B up to about 17k × 17k. If B does not fit, pages are evicted FIFO and refetched. That is still correct, just slower.

## 4. Performance model (Pi 5 cluster, 1 GbE)

All figures are **estimates**, to be replaced with measurements in the hardware phase.

| Quantity | Value |
|---|---|
| Pi 5 peak FP32 (4 × A76 @ 2.4 GHz, 2×128-bit FMA) | ≈ 150 GFLOP/s |
| This kernel (auto-vectorised, not hand-tuned), expected | ≈ 8–20 GFLOP/s per Pi |
| Effective 1 GbE throughput | ≈ 110 MB/s per direction |

For square n×n `float` GEMM on W = 4 workers at ~10 GFLOP/s each:

| n | FLOPs | Compute | B broadcast per worker | Host copies via controller (A, B in; C out) | Kernel time estimate |
|---|---|---|---|---|---|
| 1024 | 2.1 G | ~55 ms | 3 MB → ~30 ms | 12 MB → ~0.1 s | ~0.1 s |
| 2048 | 17 G | ~0.45 s | 12 MB → ~0.11 s | 48 MB → ~0.45 s | ~0.6 s |
| 4096 | 137 G | ~3.4 s | 48 MB → ~0.45 s | 192 MB → ~1.8 s | ~4 s |
| 8192 | 1.1 T | ~27 s | 192 MB → ~1.8 s | 768 MB → ~7 s | ~30 s |

What this shows:

- Arithmetic intensity grows with n, so large problems are **compute-bound** and scale with the number of workers. Small problems (n ≲ 1024) are latency- and network-bound and won't beat a single laptop.
- Host transfers through the controller are the other big cost. The roadmap's direct host→worker path and a 2.5 GbE HAT on the controller both address it.
- Hand-written NEON micro-kernels (register-blocked 8×12) could reach 30–60 GFLOP/s per Pi, which would make the network matter more. The model above suggests the design stays compute-dominated at n ≥ 4096 even then.

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
| Threads | `cl-worker --threads` | Leave at 4 on a Pi 5 |
