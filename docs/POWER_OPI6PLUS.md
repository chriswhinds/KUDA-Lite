<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# Power Requirements: 5 × Orange Pi 6 Plus Cluster

Estimated electrical power for a KUDA-Lite cluster of **1 controller and 4 workers, all Orange Pi 6 Plus, with a multi-gig (5 GbE-capable) switch.** A mixed variant with a Raspberry Pi 5 controller is in §6. For the Raspberry Pi 5 cluster, see [POWER.md](POWER.md).

> **These are estimates**, built from published reviews and manufacturer specifications (sources at the end), not from measurements of this cluster. Published Orange Pi 6 Plus figures vary a lot with peripherals, OS image, CPU governor and how they were measured. The Linpack peak comes from the Orange Pi 6, which has the same CIX P1 SoC. **Measure your own cluster** (§8) before relying on these numbers.

## 1. Summary

| Cluster state | Power at the wall (est.) | Notes |
|---|---|---|
| **Idle** (services running, no jobs) | **≈ 100 W** | The CIX P1 idles high for an SBC (≈ 15 W per board), and multi-gig switches draw more than gigabit ones |
| **Typical full compute** (sustained kernel on all workers) | **≈ 180 W** | All 48 worker cores busy |
| **Peak** (HPL-class load such as large SGEMM, everything busy) | **≈ 250 W** | Worst realistic sustained draw |
| **Rated supply capacity** | **≈ 540 W** | 5 × 100 W USB-C PD supplies + switch adapter. Size circuits and UPS for the actual draw, not this. |

Rule of thumb: **plan for about 250 W continuous, and about 100 W whenever the cluster is on.** That's about 4× the Raspberry Pi 5 cluster. In return it delivers roughly 5× the Linpack throughput (≈ 540 vs ≈ 110 GFLOPS) and 8× the global memory (≈ 154 GB vs ≈ 19 GB), so compute per watt is better (§5).

## 2. Per-device figures

### Orange Pi 6 Plus (CIX P1, 12 cores)

| State | Board draw | Basis |
|---|---|---|
| Idle | ≈ 15 W (published: 13.3–16 W) | Reviews of the 6 Plus (sbc.compare, interfacinglinux.com); Orange Pi 6 measured 13.7 W (bret.dk). One much lower figure (4.2 W, minimal peripherals) is treated as an outlier. |
| Sustained all-core CPU load | ≈ 27–30 W | Orange Pi 6: 26.7 W (bret.dk); 6 Plus: 26.6 W (sbc.compare), "in the 30s" (interfacinglinux.com) |
| Peak (Linpack/HPL) | ≈ 41–42 W | Orange Pi 6: 41.4 W at 135 GFLOPS (bret.dk); the Plus's 5 GbE PHYs add an estimated ≈ 1 W |
| Active cooler | included in the figures above | Reviews tested with the stock or a stronger cooler |

This document uses **15 W idle, 30 W typical and 42 W peak** per worker. The controller is mostly idle with bursts: **16 W idle, 18 W typical, 25 W peak** (estimate).

### Power supply (one per board)

| Spec | Value |
|---|---|
| Required | USB-C Power Delivery, **20 V / 5 A (100 W)** |
| Official Orange Pi adapter | PD 5 V/3 A, 9 V/3 A, 12 V/3 A, 15 V/3 A, 20 V/5 A; 100 W max |
| Efficiency | Not published; **88 % assumed** (typical for 100 W GaN/PD adapters) |

At a 42 W peak the board draws about 2.1 A at 20 V, under half the supply's rating.

### Network switch (5 GbE-capable)

The Orange Pi 6 Plus's 5 GbE ports only pay off with a switch that runs them at 5 Gb/s. Five boards plus an uplink need **6 ports**:

| Option | Speed to each board | Max power | Note |
|---|---|---|---|
| **8-port 10G/multi-gig** (e.g. TP-Link TL-SX1008: 8 × 10GBASE-T, negotiates 5 G) | 5 Gb/s | 31.2 W (manufacturer spec) | Recommended. Estimated ≈ 15 W idle, ≈ 25 W loaded. |
| 8-port 2.5 GbE (unmanaged) | 2.5 Gb/s | typically 8–12 W | Cheaper and cooler, but halves the boards' network speed |

## 3. Cluster budget (all Orange Pi 6 Plus)

Board figures are treated as DC and divided by 0.88 for supply losses, which is conservative if a reviewer measured at the wall. The switch figure is at the wall.

| Device | Qty | Idle | Typical compute | Peak |
|---|---|---|---|---|
| Worker Orange Pi 6 Plus | 4 | 4 × 15 = 60 W | 4 × 30 = 120 W | 4 × 42 = 168 W |
| Controller Orange Pi 6 Plus | 1 | 16 W | 18 W | 25 W |
| **Boards subtotal** | | **76 W** | **138 W** | **193 W** |
| Boards at the wall (÷ 0.88) | | 86 W | 157 W | 219 W |
| Multi-gig switch (at the wall) | 1 | ≈ 15 W | ≈ 25 W | 31.2 W |
| **Cluster total at the wall** | | **≈ 100 W** | **≈ 180 W** | **≈ 250 W** |

## 4. Energy use and running cost

kWh/year = watts × 8,760 ÷ 1,000. The example rate is **$0.17 per kWh**; substitute your own.

| Operating pattern | Average draw | kWh / year | Cost / year at $0.17 |
|---|---|---|---|
| Always on, idle | 100 W | ≈ 876 | ≈ $149 |
| Always on, busy 25 % of the time | ≈ 120 W | ≈ 1,051 | ≈ $179 |
| Always on, busy 100 % of the time | 180 W | ≈ 1,577 | ≈ $268 |
| Powered only during jobs (8 h/day, busy) | 180 W × 8 h | ≈ 526 | ≈ $89 |

Because idle draw is high, **powering the cluster down between job runs saves most of the cost**. Every daemon restarts cleanly on boot.

## 5. Compute per watt vs the Raspberry Pi 5 cluster

| 5-node cluster (1 controller + 4 workers) | Peak power (wall) | Worker Linpack (sum) | GFLOPS per wall watt | Global memory |
|---|---|---|---|---|
| Raspberry Pi 5 (8 GB) + 1 GbE switch | ≈ 67 W | ≈ 4 × 27 = 108 GFLOPS | ≈ 1.6 | ≈ 19 GB |
| Orange Pi 6 Plus (64 GB) + multi-gig switch | ≈ 250 W | ≈ 4 × 135 = 540 GFLOPS | ≈ 2.2 | ≈ 154 GB |

These are per-board Linpack figures. KUDA-Lite adds network and scheduling overhead, so expect lower cluster throughput. The faster network narrows that gap on the Orange Pi ([MATMUL.md §4](MATMUL.md#4-performance-model)).

## 6. Mixed variant: Raspberry Pi 5 controller, Orange Pi 6 Plus workers

The controller does little computing, so a Pi 5 saves about 12 W idle. Its 1 GbE port then limits host copies into the cluster, but not worker-to-worker traffic.

| | Idle | Typical | Peak |
|---|---|---|---|
| 4 × Orange Pi 6 Plus workers (at the wall) | 68 W | 136 W | 191 W |
| Raspberry Pi 5 controller (at the wall, [POWER.md](POWER.md)) | 4 W | 6 W | 8 W |
| Multi-gig switch | 15 W | 25 W | 31 W |
| **Total** | **≈ 87 W** | **≈ 167 W** | **≈ 230 W** |

## 7. Practical power setup

- **Supplies:** one genuine **100 W USB-C PD** supply per board. Laptop chargers work only if they reliably deliver 20 V / 5 A on that port; multi-port chargers usually share their wattage, so they won't. An underpowered board may boot and then reset under load.
- **Outlets and circuits:** six devices (5 board supplies and the switch). 250 W is about 2 A at 120 V (1 A at 230 V), easily within one household circuit. Use a surge-protected strip with room for 100 W bricks.
- **UPS:** size it for at least 250–300 W, i.e. **≥ 1,000 VA** class. Expect roughly 10–20 minutes at full load from a typical 1,000–1,500 VA unit, and longer at idle; check the maker's runtime chart. Losing power clears global memory ([DEPLOYMENT.md §8.3](DEPLOYMENT.md#83-what-happens-when-things-fail)).
- **Heat:** about 180 W ≈ 615 BTU/h under load, comparable to a few people in the room. Give the stack front-to-back airflow and keep the switch out of the boards' exhaust. Multi-gig switches run warm themselves.

## 8. Measuring your own cluster

1. **Wall:** a plug-in energy meter on the power strip gives the true total.
2. **Per board:** an inline USB-C PD power meter that supports 20 V shows each board's draw.
3. **From software:** the dashboard shows temperature and clock speed per node (no wattage on this board). Watch for the clock dropping under load, which means throttling.
4. **Reference loads:** idle with services running, then on the controller:

   ```bash
   cl-test-matmul --controller 127.0.0.1 --sizes 8192 --repeat 5
   ```

Write the measured numbers into §3.

## Sources

- bret.dk, [Orange Pi 6 initial thoughts: CIX P1 benchmarks, power draw and temperatures](https://bret.dk/orange-pi-6-initial-thoughts/): Orange Pi 6 idle 13.7 W, load 26.7 W, Linpack 41.4 W at 135 GFLOPS, temperatures.
- sbc.compare, [Orange Pi 6 Plus specs, benchmarks and price](https://sbc.compare/orange-pi-6-plus): 6 Plus idle 13.3 W, sustained load 26.6 W.
- interfacinglinux.com, [Orange Pi 6 Plus hands-on and setup](https://interfacinglinux.com/2025/11/10/orangepi-6-plus/): about 15 W idle, "in the 30s" under load.
- CNX Software, [Orange Pi 6 Plus: CIX P1 SBC with up to 64 GB LPDDR5 and 45 TOPS](https://www.cnx-software.com/2025/10/15/orange-pi-6-plus-cix-p1-sbc-64gb-lpddr5-45-tops-ai-performance/): SoC, memory, 2 × 5 GbE, NPU.
- Orange Pi, [Orange Pi 6 Plus product page](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-6-Plus.html), and the official 100 W supply listing (PD 20 V/5 A, 100 W): power input.
- TP-Link, [TL-SX1008 specifications](https://www.tp-link.com/us/business-networking/soho-switch-unmanaged/tl-sx1008/): 8 × 10G multi-gig, 31.2 W maximum.
- Jeff Geerling, [Pi 5 power and HPL efficiency](https://www.jeffgeerling.com/blog/2024/new-2gb-pi-5-has-33-smaller-die-30-idle-power-savings/): Pi 5 8 GB at 2.75 GFLOPS/W, used for the comparison in §5.
