<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# KUDA-Lite Roadmap and Known Limitations

## Next: Orange Pi 6 Plus

Release 0.3 adds the Orange Pi 6 Plus as a supported platform alongside the Raspberry Pi 5 ([PLATFORMS.md](PLATFORMS.md)). Planned follow-ups:

1. **Hardware validation:** run the acceptance plan ([TESTING.md §4](TESTING.md#4-hardware-acceptance-plan)) on real Orange Pi 6 Plus boards, and replace the estimates in [POWER_OPI6PLUS.md](POWER_OPI6PLUS.md) and [MATMUL.md §4](MATMUL.md#4-performance-model) with measurements.
2. **NPU back end** (the CIX P1's NPU, up to 45 TOPS). Proposed design:
   - a per-worker *accelerator* interface the executor can offer to kernels (`ctx.npu()`), present only on boards where the vendor NPU runtime is installed and detected;
   - an `npu` variant of selected kernels, starting with INT8/FP16 GEMM, chosen per launch by a kernel attribute or by the host (`clLaunchKernel` flag);
   - the worker still owns global-memory access: it gathers tiles into NPU-visible buffers, runs the NPU job, and writes results back, so the memory model is unchanged;
   - telemetry gains NPU utilisation, and the dashboard shows it.

   This depends on CIX's NPU SDK and runtime, whose licensing and Linux packaging need checking first. Until then all kernels run on the CPU. Full decision record, design and phased plan: [NPU.md](NPU.md).
3. **Thread placement on mixed cores:** optionally pin the kernel thread pool to the A720 clusters, and report per-cluster utilisation.
4. **SVE2/NEON GEMM micro-kernel** tuned for the Cortex-A720 (and a NEON one for the Pi 5's A76).
5. **Second 5 GbE port:** optional link aggregation, or a separate host-copy network.

## Known limitations in v1

| Limitation | Impact | Planned remedy |
|---|---|---|
| Host copies go through the controller | Host bandwidth is capped by the controller's single NIC | Direct host ↔ worker data path: the controller returns the extent plan and the host talks to the owners |
| Worker loss loses its slice of global memory | Allocations touching it become unusable | Optional replication (`clMallocEx` flag), or checkpoint to the controller's disk |
| No authentication or encryption | Anyone on the network can use the cluster | Mutual TLS with a pre-shared cluster CA, and a session token |
| No liveness heartbeats | A hung (but connected) worker stalls launches | Periodic `Ping` with a timeout; reassign that worker's chunks |
| Kernels must be compiled into `cl-worker` | Adding a kernel means redeploying workers | Signed kernel plug-ins (`dlopen`) distributed by the controller |
| No global atomics | No reductions or counters across blocks | `MemAtomicV` data-plane op (add, min, max, CAS) executed by the owning worker |
| GEMM kernel is auto-vectorised C++ | Roughly 10–20 % of the CPU's FP32 peak | Register-blocked micro-kernels (NEON on the Pi 5, SVE2/NEON on the Orange Pi 6 Plus) and packed panels |
| Blocks fetch, then compute, then write, serially | Network latency is not overlapped with compute | Prefetch the next block's panels while computing the current one |
| Scheduling ignores data location | Remote reads that owner-computes could avoid | Locality-aware chunk assignment for `clDistBlocked` data |
| CPU only; the Orange Pi 6 Plus NPU is unused | Accelerator capacity left idle | NPU back end (above) |
| Default stream does not synchronise with other streams | Differs from CUDA's legacy stream | Optional legacy mode |
| Single host per allocation (no sharing between processes) | No multi-process pipelines | IPC handles (`clIpcGetMemHandle` analogue) |
| Dashboard and telemetry are unauthenticated | Anyone on the network can view cluster state | Put the dashboard behind the site's SSO / reverse proxy; token on `GetTelemetry` |
| Telemetry lives in memory only (15 min in the dashboard) | No long-term trends | Prometheus `/metrics` endpoint on the backend (and Grafana), or a small time-series store |
| Throttling is inferred from frequency and temperature | Undervoltage and throttle history are not shown | Read firmware throttle flags where available (`vcgencmd get_throttled` on the Pi 5; the CIX P1's equivalent once documented) |
| No alerting | Someone must be watching | Threshold alerts (webhook or e-mail) from the backend |
| Controller is a single point of failure | Cluster restarts if it dies | Out of scope for these clusters; documented |
