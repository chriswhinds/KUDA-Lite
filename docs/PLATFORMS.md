<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# Supported Platforms

KUDA-Lite has one codebase with two first-class hardware targets. Each target has its own build preset, CPU tuning, network profile, deployment notes, inventory example and power estimate:

| | **Raspberry Pi 5** | **Orange Pi 6 Plus** |
|---|---|---|
| SoC | Broadcom BCM2712 | CIX P1 (CD8180) |
| CPU | 4 × Cortex-A76 @ 2.4 GHz | 12 cores: 4 × Cortex-A720 @ 2.6 GHz, 4 × Cortex-A720 @ 2.4 GHz, 4 × Cortex-A520 @ 1.8 GHz |
| Architecture | Armv8.2-A | Armv9.2-A |
| RAM | 4 / 8 / 16 GB LPDDR4X | 16 / 32 / 64 GB LPDDR5 (128-bit) |
| Global memory per worker (60 % default) | 4.8 GB (8 GB board) | 38 GB (64 GB board) |
| Network | 1 × 1 GbE | 2 × 5 GbE |
| NPU | none | up to 45 TOPS (not used by KUDA-Lite yet; see [§6](#6-the-npu)) |
| Power input | USB-C, 5.1 V / 5 A (27 W supply) | USB-C PD, 20 V / 5 A (100 W supply) |
| Linpack (HPL), one board | ≈ 27 GFLOPS (≈ 2.75 GFLOPS/W) | ≈ 135 GFLOPS (≈ 3.25 GFLOPS/W) |
| Power, one board (idle / load / peak) | ≈ 3 / 10 / 12 W | ≈ 15 / 30 / 42 W |
| Build preset | `pi5` (`-mcpu=cortex-a76`) | `opi6plus` (`-mcpu=cortex-a720`, with fallbacks) |
| `install.sh` network profile | `deploy/sysctl/pi5.conf` | `deploy/sysctl/opi6plus.conf` |
| Inventory example | `deploy/cluster.inventory.example` | `deploy/cluster.inventory.opi6plus.example` |
| Power estimate | [POWER.md](POWER.md) | [POWER_OPI6PLUS.md](POWER_OPI6PLUS.md) |

Figures are from manufacturer specifications and published reviews (sources in the power documents). The Orange Pi Linpack and power figures come from the Orange Pi 6, which has the same SoC as the Plus. **Neither platform has been tested with KUDA-Lite on real hardware yet** ([TESTING.md](TESTING.md)).

Any other 64-bit Linux machine (x86-64 or AArch64) also works as a `generic` node. That's how the test suite runs a simulated cluster on one PC.

## 1. Choosing

- **Raspberry Pi 5:** cheap, low power (≈ 67 W peak for 5 nodes), well-supported OS. The 1 GbE link is the bottleneck for global memory, so it suits learning, development and smaller problems.
- **Orange Pi 6 Plus:** each worker has 3× the cores, about 5× the Linpack throughput, 8× the RAM and 5× the network bandwidth of a Pi 5. A 5-node cluster delivers around 540 GFLOPS of CPU compute and about 150 GB of global memory, at roughly 3–4× the power (≈ 250 W peak). It also has an NPU for future acceleration work.

**Mixed clusters work.** Every node detects its own board, and the protocol and memory model don't depend on it. A common split is an Orange Pi 6 Plus controller (its 5 GbE port speeds up host copies) with whatever workers you have. Keep in mind:

- A striped allocation is spread **evenly** over all workers, so its size is limited by the **smallest** worker arena. Use `mem =` in `worker.conf` to balance arenas, or keep workers alike.
- Blocks are scheduled dynamically, so faster workers simply take more of a launch.
- A 1 GbE node slows down the transfers it takes part in.

## 2. What is platform-specific in the code

| Area | How it adapts | Where |
|---|---|---|
| CPU tuning | CMake option `KUDALITE_CPU` = `pi5` or `opi6plus`. For the CIX P1 it probes the compiler: `-mcpu=cortex-a720` → `-mcpu=cortex-a710` → `-march=armv9-a` → `-march=armv8.2-a`, since GCC < 14 doesn't know the A720. | `CMakeLists.txt`, `CMakePresets.json` |
| Board detection | `install.sh` reads the device-tree model, else DMI (UEFI/ACPI boards), else the CPU part number (Cortex-A720 = `0xd81`). Override with `--platform`. | `deploy/install.sh` |
| Network tuning | Per-platform sysctl profile; the 5 GbE profile has 64 MB TCP windows and a larger receive backlog | `deploy/sysctl/*.conf` |
| Telemetry | Temperature is the **hottest** thermal zone, clock is the **fastest** cpufreq policy, and the board model is reported. On big.LITTLE parts `cpu0` is often a slow core, so reading only `cpu0` would under-report. | `src/common/telemetry.cpp` |
| Defaults | Worker threads = all cores (4 or 12); arena 60 % and cache 15 % of RAM; scaled automatically | `src/worker/main.cpp` |

Everything else (protocol, memory model, scheduler, kernels, dashboard) is shared.

## 3. Orange Pi 6 Plus notes

### 3.1 Heterogeneous cores

The CIX P1 has three clusters of different speed. A worker's thread pool uses all 12 cores. `parallelFor` hands out work in small chunks (about 4 per thread), so the A520 cores finish fewer chunks and nobody waits on them, and a kernel gets close to the full chip's throughput. If you'd rather leave the LITTLE cores to the OS and the network stack, set `threads = 8` in `worker.conf`. Linux usually schedules busy threads onto the big cores first, but this isn't guaranteed; pinning threads to clusters is on the [roadmap](ROADMAP.md).

### 3.2 Two 5 GbE ports

Use **one port for the cluster switch**. The second port can stay unused or connect to your normal LAN. If both are connected, set `advertise = <cluster-port address>` in `worker.conf`, so other nodes reach the worker over the cluster port. Check the negotiated speed with:

```bash
sudo ethtool eth0 | grep Speed
```

It should report `5000Mb/s`; a cable or switch fault often drops it to 1000.

### 3.3 Operating system

Use Orange Pi's official images: Debian 12 or Ubuntu with the vendor kernel, e.g. `6.6.x-cix` in published reviews. Mainline Linux support for the CIX P1 is still developing, so prefer the vendor image for now. Things to know:

- **Compiler:** Debian 12 ships GCC 12, which doesn't know `cortex-a720`. The build falls back to `-mcpu=cortex-a710` (same Armv9 family) automatically.
- **Node.js:** Debian 12's Node.js is version 18, too old for the dashboard. Install with `-x "--install-node"`.
- **Firmware:** the board boots with UEFI. If the image exposes ACPI rather than a device tree, `install.sh` falls back to DMI and the CPU part number to recognise it.
- **Default user:** Orange Pi images create `orangepi` (see the inventory example). Create your own user and SSH key as described in [DEPLOYMENT.md §3](DEPLOYMENT.md#3-prepare-each-node).

### 3.4 Cooling and power

Use the board's active cooler. Published tests saw at most about 52 °C (59 °C in the official case) under sustained load without throttling. The board needs a **20 V USB-C PD supply rated 100 W**; a phone charger will not do. Idle power is high for an SBC (≈ 15 W), so an always-on 5-node cluster idles near 100 W including a multi-gig switch ([POWER_OPI6PLUS.md](POWER_OPI6PLUS.md)).

## 4. Raspberry Pi 5 notes

Raspberry Pi OS Lite (64-bit) or Ubuntu Server. Use the official Active Cooler and the 27 W supply. The single 1 GbE port is the global-memory bottleneck ([MATMUL.md §4](MATMUL.md#4-performance-model)). Everything in [DEPLOYMENT.md](DEPLOYMENT.md) applies as written.

## 5. Build and deploy per platform

| Task | Raspberry Pi 5 | Orange Pi 6 Plus |
|---|---|---|
| Build on the board | `cmake --preset pi5 && cmake --build --preset pi5` | `cmake --preset opi6plus && cmake --build --preset opi6plus` |
| Cross-compile on x86-64 | `cmake --preset pi5-cross` | `cmake --preset opi6plus-cross` |
| Install one node | `sudo deploy/install.sh worker --controller HOST` (platform detected automatically) | same, ideally with `--tune-network` |
| Deploy a cluster | `deploy/deploy-cluster.sh install` | `deploy/deploy-cluster.sh -x "--tune-network --install-node" install` |
| Inventory example | `deploy/cluster.inventory.example` | `deploy/cluster.inventory.opi6plus.example` |

## 6. The NPU

The CIX P1's NPU (up to 45 TOPS, INT8-oriented) is a reason to pick the Orange Pi 6 Plus, but **KUDA-Lite does not use it yet**: every kernel runs on the CPU cores. Using it needs CIX's NPU SDK and runtime, and a kernel back end that can hand suitable work (low-precision GEMM, convolutions) to the NPU while the CPU handles the rest. The design is outlined in the [roadmap](ROADMAP.md). The current CPU kernels and the whole runtime work unchanged in the meantime.

## 7. Adding another board

1. Add a `KUDALITE_CPU` value and its compiler-flag candidates in `CMakeLists.txt`, plus native and cross presets in `CMakePresets.json`.
2. Teach `detect_platform()` in `deploy/install.sh` to recognise it (device-tree or DMI model, or CPU part number).
3. Add `deploy/sysctl/<platform>.conf` sized for its network speed.
4. Add a profile to the dashboard's mock controller (`BOARDS` in `mock_controller.py`).
5. Document it here, with a `POWER_<platform>.md` and an inventory example.
