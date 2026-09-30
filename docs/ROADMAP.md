# KUDA-Lite Roadmap and Known Limitations

## Next phase (awaiting go-ahead)

- **Build system:** CMake producing `libkudalite` (static and shared), `cl-controller`, `cl-worker`, the tests, `cl-info` and the examples. Native builds on Linux/macOS hosts, and native or cross builds for AArch64 Pi nodes. `-O3 -mcpu=cortex-a76` for workers.
- **Configuration and deployment:** systemd units for the controller and workers (with restart-on-failure), a cluster config file (controller address, ports, memory and cache sizes), and provisioning scripts for a fresh Pi on any distribution.

## Known limitations in v1

| Limitation | Impact | Planned remedy |
|---|---|---|
| Host copies go through the controller | Host bandwidth is capped by the controller's single NIC | Direct host ↔ worker data path: the controller returns the extent plan and the host talks to the owners |
| Worker loss loses its slice of global memory | Allocations touching it become unusable | Optional replication (`clMallocEx` flag), or checkpoint to the controller's disk |
| No authentication or encryption | Anyone on the network can use the cluster | Mutual TLS with a pre-shared cluster CA, and a session token |
| No liveness heartbeats | A hung (but connected) worker stalls launches | Periodic `Ping` with a timeout; reassign that worker's chunks |
| Kernels must be compiled into `cl-worker` | Adding a kernel means redeploying workers | Signed kernel plug-ins (`dlopen`) distributed by the controller |
| No global atomics | No reductions or counters across blocks | `MemAtomicV` data-plane op (add, min, max, CAS) executed by the owning worker |
| GEMM kernel is auto-vectorised C++ | Roughly 10% of the Pi's FP32 peak | NEON register-blocked micro-kernel and packed panels |
| Blocks fetch, then compute, then write, serially | Network latency is not overlapped with compute | Prefetch the next block's panels while computing the current one |
| Scheduling ignores data location | Remote reads that owner-computes could avoid | Locality-aware chunk assignment for `clDistBlocked` data |
| Default stream does not synchronise with other streams | Differs from CUDA's legacy stream | Optional legacy mode |
| Single host per allocation (no sharing between processes) | No multi-process pipelines | IPC handles (`clIpcGetMemHandle` analogue) |
| Dashboard and telemetry are unauthenticated | Anyone on the network can view cluster state | Put the dashboard behind the site's SSO / reverse proxy; token on `GetTelemetry` |
| Telemetry lives in memory only (15 min in the dashboard) | No long-term trends | Prometheus `/metrics` endpoint on the backend (and Grafana), or a small time-series store |
| Throttling is inferred from frequency and temperature | Undervoltage and throttle history are not shown | Read the Pi firmware throttle flags (`vcgencmd get_throttled`, or the equivalent sysfs file where present) |
| No alerting | Someone must be watching | Threshold alerts (webhook or e-mail) from the backend |
| Controller is a single point of failure | Cluster restarts if it dies | Out of scope for the Pi cluster; documented |
