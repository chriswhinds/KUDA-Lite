# Power Requirements: 5 × Raspberry Pi 5 Cluster

Estimated electrical power for the reference KUDA-Lite cluster: **1 controller Pi 5, 4 worker Pi 5s, and one gigabit network switch.**

> **These are estimates**, built from published measurements and manufacturer specifications (sources at the end), not from measurements of this cluster. Real figures depend on board revision, RAM size, workload, ambient temperature and accessories. Measure your own cluster before relying on the numbers for anything critical (§7).

## 1. Summary

| Cluster state | Power at the wall (est.) | What it looks like |
|---|---|---|
| **Idle** (services running, no jobs) | **≈ 22 W** | Daemons and dashboard up, CPUs near idle, fans mostly off |
| **Typical full compute** (sustained kernel on all workers) | **≈ 56 W** | All 16 worker cores busy, fans spinning, network active |
| **Peak** (HPL-class load such as large SGEMM, everything busy) | **≈ 67 W** | Worst realistic sustained draw |
| **Rated supply capacity** (what the power supplies *can* deliver) | **≈ 138 W** | 5 × 27 W Pi supplies + 3 W switch adapter (5 V × 0.6 A). Size outlets and any UPS for the actual draw above, not this. |

Rule of thumb: **plan for about 70 W continuous, and expect around 25 W at idle.** That's roughly one bright incandescent bulb at full load, well within a single household outlet or power strip.

## 2. Per-device figures

### Raspberry Pi 5 (8 GB)

| State | Board draw (DC) | Basis |
|---|---|---|
| Idle, headless, Ethernet up | 3.2 W | Measured idle, 8 GB C1 board (Geerling) |
| Typical everyday CPU load | 6.5 W | Measured (bret.dk) |
| All 4 cores busy (stress-ng) | 9.8 W | Measured, 8 GB C1 board (Geerling) |
| Peak (Linpack/HPL) | 11.6 W | Measured (bret.dk) |
| Active Cooler fan | 0 W idle (fan off when cool), ≈ 0.5 W running | Third-party estimate; the official datasheet gives no power figure |

**Why the HPL figure matters here.** KUDA-Lite's `cl_sgemm` kernel is the same kind of dense floating-point work as Linpack. A worker running a large matrix multiplication will sit between the stress-ng and HPL figures, so this document uses **≈ 10.3 W typical** (9.8 + fan) and **≈ 12.1 W peak** (11.6 + fan) per busy worker.

The 2 GB Pi 5 (D0 silicon) uses less: about 2.4 W idle and 8.9 W under stress-ng. A cluster of 2 GB boards would land 10–15% below the figures here, but it would also have far less global memory.

### Controller Pi 5

The controller does little computing. It schedules work, proxies host copies, and runs the dashboard (Python and Node.js). Its CPU is mostly idle, with bursts during large copies.

| State | Estimate (DC) |
|---|---|
| Idle (controller + dashboard services) | 3.5 W |
| Busy (proxying copies, dashboard in use) | 5 W typical, 7.5 W peak including fan |

### Official 27 W USB-C power supply (one per Pi)

| Spec | Value |
|---|---|
| Output | 5.1 V, 5 A (25.5 W); USB-PD profiles to 27 W |
| Input | 100–240 V AC, 50/60 Hz |
| Average active efficiency | 89 % (87.9 % at 10 % load) |

Wall power for a Pi ≈ board power ÷ 0.89. At 12 W the Pi draws about 2.4 A from its 5 A supply, so the supply has plenty of headroom.

### Network switch

Five Pis plus an uplink to your network (for your development machine and the dashboard) need **6 ports**, so use an **8-port** unmanaged gigabit switch. A 5-port switch is one port short.

| Example | Max power (at the wall) | Typical |
|---|---|---|
| TP-Link TL-SG108 (8-port, unmanaged gigabit) | 3.75 W (manufacturer spec, 220 V) | ≈ 2–3 W: idle ports power down automatically |

Other 8-port unmanaged gigabit switches draw much the same, about 2–5 W.

## 3. Cluster budget

DC figures are at the devices. Wall figures add power-supply losses (Pi supplies at 89 %; the switch figure is already at the wall).

| Device | Qty | Idle (DC) | Typical compute (DC) | Peak (DC) |
|---|---|---|---|---|
| Worker Pi 5 + Active Cooler | 4 | 4 × 3.3 = 13.2 W | 4 × 10.3 = 41.2 W | 4 × 12.1 = 48.4 W |
| Controller Pi 5 + Active Cooler | 1 | 3.5 W | 5.0 W | 7.5 W |
| **Pi subtotal (DC)** | | **16.7 W** | **46.2 W** | **55.9 W** |
| Pi subtotal at the wall (÷ 0.89) | | 18.8 W | 51.9 W | 62.8 W |
| Switch (at the wall) | 1 | 2.5 W | 3.75 W | 3.75 W |
| **Cluster total at the wall** | | **≈ 21–22 W** | **≈ 56 W** | **≈ 67 W** |

## 4. Energy use and running cost

Energy (kWh/year) = watts × 8,760 h ÷ 1,000. Multiply by your electricity rate. The example rate below is **$0.17 per kWh**; substitute your own tariff.

| Operating pattern | Average draw | kWh / year | Cost / year at $0.17 |
|---|---|---|---|
| Always on, idle | 22 W | ≈ 193 | ≈ $33 |
| Always on, busy 25 % of the time | ≈ 30 W | ≈ 263 | ≈ $45 |
| Always on, busy 100 % of the time | 56 W | ≈ 491 | ≈ $83 |
| Powered only during jobs (8 h/day, busy) | 56 W × 8 h | ≈ 164 | ≈ $28 |

## 5. Practical power setup

**Outlets.** Six devices need power: 5 Pi supplies and the switch adapter. Use a surge-protected power strip with at least 6 outlets, spaced widely enough for the bulky USB-C supplies (or use short extension leads). The whole cluster is far below the 1,800 W (US 15 A) or 2,300–3,000 W (EU/UK) a single household circuit provides.

**Use the official 27 W supply, or another genuine 5 V/5 A USB-PD supply.** Most phone and laptop chargers give at most 5 V/3 A (15 W) at 5 V. On those, the Pi 5 prints a power warning and limits its USB ports to 600 mA. A 15 W supply technically covers a 12 W compute load, but it leaves almost no margin for load spikes, and under-voltage causes throttling and instability. Undersized supplies are a common cause of "random" worker crashes.

**Multi-port USB-C chargers.** Only use one if *every* port can supply 5 V/5 A at the same time. Almost none can, so one supply per Pi is the reliable choice.

**Power over Ethernet (alternative).** Each Pi takes a Pi 5-compatible PoE+ HAT (IEEE 802.3at), and the switch becomes an 8-port PoE+ switch. That's one cable per Pi and no USB-C supplies. Budget about 15 W per Pi, so the switch needs **≥ 75 W of PoE budget** (higher is better), plus roughly 1–2 W of conversion loss per HAT. Expect total wall power about 10–15 % above the figures in §3 (estimate). Many PoE HATs also cover the CPU, so check cooler compatibility.

**UPS (optional).** At about 60 W, a small UPS rated 350–600 VA (roughly 80–100 Wh of battery) should give somewhere near 45–90 minutes. This varies a lot between models, so check the manufacturer's runtime chart at 60 W. A UPS is worth having because **losing power clears KUDA-Lite's global memory** ([DEPLOYMENT.md §8.3](DEPLOYMENT.md#83-what-happens-when-things-fail)); it lets jobs finish or be stopped cleanly.

**Heat.** Essentially all the power ends up as heat: about 56 W ≈ 190 BTU/h at full load. That's negligible for a room, but the Pis themselves need airflow. Without active coolers they throttle at 80–85 °C, which you'll see in the dashboard. Leave space between boards, and don't enclose the stack without ventilation.

## 6. Scaling to other cluster sizes

| Cluster | Idle (wall) | Typical compute (wall) | Peak (wall) |
|---|---|---|---|
| 1 controller + 2 workers | ≈ 14 W | ≈ 33 W | ≈ 39 W |
| **1 controller + 4 workers (this document)** | **≈ 22 W** | **≈ 56 W** | **≈ 67 W** |
| 1 controller + 8 workers (16-port switch) | ≈ 38 W | ≈ 104 W | ≈ 123 W |

These follow from §3: each extra busy worker adds about 11.6 W typical and 13.6 W peak at the wall. An 8-port switch covers up to 6 workers (6 + controller + uplink = 8 ports). From 7 workers you need a 16-port switch, assumed here at about 4 W idle and 6 W maximum.

## 7. Measuring your own cluster

1. **At the wall:** plug the power strip into a plug-in energy meter or smart plug with power monitoring. This gives the true total, including all losses.
2. **Per Pi:** an inline USB-C power meter between the supply and the Pi shows each board's DC draw.
3. **From software:** the Pi 5's power-management chip reports rail voltages and currents with `vcgencmd pmic_read_adc`. Summing current × voltage gives an approximate board power, which you can log while a job runs.
4. **Reproduce the reference loads:** measure idle with the services running, then run `cl-test-matmul --controller 127.0.0.1 --sizes 4096 --repeat 10` on the controller for a sustained full-compute figure.

Write the measured numbers into §3, replacing these estimates.

## Sources

- Jeff Geerling, [New 2GB Pi 5 has 33% smaller die, 30% idle power savings](https://www.jeffgeerling.com/blog/2024/new-2gb-pi-5-has-33-smaller-die-30-idle-power-savings/): idle and stress-ng power for the 2 GB, 4 GB and 8 GB Pi 5.
- bret.dk, [How to Power the Raspberry Pi 5: A Complete Guide](https://bret.dk/how-to-power-the-raspberry-pi-5-a-complete-guide/): typical, AI-inference and Linpack peak power.
- Raspberry Pi Ltd, [27W USB-C Power Supply product brief](https://datasheets.raspberrypi.com/power-supply/27w-usb-c-power-supply-product-brief.pdf): output, input and efficiency.
- Raspberry Pi Ltd, [Active Cooler product brief](https://datasheets.raspberrypi.com/cooling/raspberry-pi-active-cooler-product-brief.pdf): fan specification (no power figure published).
- TP-Link, [TL-SG108 specifications](https://www.tp-link.com/us/business-networking/soho-switch-unmanaged/tl-sg108/): maximum power consumption 3.75 W.
- raspberry.tips, [Raspberry Pi Power Consumption 2026](https://raspberry.tips/en/raspberrypi-tutorials/raspberry-pi-power-consumption-update-2026-all-models-compared): cross-check of Pi 5 idle and load figures; source of the about 0.5 W Active Cooler estimate.
