<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# Deploying KUDA-Lite on a Raspberry Pi 5 or Orange Pi 6 Plus Cluster

This guide takes you from a pile of Raspberry Pi 5 or Orange Pi 6 Plus boards to a running cluster. Where the boards differ it gives both, and [PLATFORMS.md](PLATFORMS.md) compares them. It then covers day-to-day operation. For building from source by hand, see [BUILD.md](BUILD.md).

**Summary:**

1. Flash a 64-bit Linux on every board, with hostnames and SSH keys.
2. Give each board a fixed address.
3. Copy `deploy/cluster.inventory.example` (or `cluster.inventory.opi6plus.example`) to `deploy/cluster.inventory` and list your boards in it.
4. Optionally run `deploy/deploy-cluster.sh render` and review the generated per-node configuration.
5. Run `deploy/deploy-cluster.sh install`.
6. Run `deploy/deploy-cluster.sh verify`.
7. Open `http://<controller>:3000/`.

## 1. Topology

KUDA-Lite supports two board types, **Raspberry Pi 5** and **Orange Pi 6 Plus**, and clusters may mix them ([PLATFORMS.md](PLATFORMS.md)). The layout is the same either way:

```
                         ┌──────────── Cluster switch: 1 GbE (Pi 5) or 5 GbE multi-gig (Orange Pi 6 Plus) ─────────┐
                         │                                                                                         │
 your Linux/macOS  ──────┤     controller                 worker 0       worker 1       worker 2   …   worker N      │
 machine (host)          │  kudalite-controller          kudalite-      kudalite-      kudalite-     kudalite-      │
  • your programs        │  kudalite-dashboard-api/web   worker         worker         worker        worker         │
  • deploy scripts       │  :7070 hosts  :7071 workers   :7100          :7100          :7100         :7100          │
  • browser → :3000      │  :3000 dashboard                                                                        │
                         └─────────────────────────────────────────────────────────────────────────────────────────┘
```

- **One controller board.** It runs `cl-controller`, and usually the dashboard as well. Its CPU is mostly idle; it's the network hub for host copies.
- **N worker boards.** Each runs `cl-worker`, which contributes about 60 % of its RAM to global memory.
- **Your development machine** (Linux or macOS) runs KUDA-Lite programs and deploys the cluster over SSH. It needs to reach the controller's port 7070, and your browser needs port 3000.

Example hostnames in this guide: `pi5-ctl`, `pi5-w0`, … for a Raspberry Pi 5 cluster, and `opi6-ctl`, `opi6-w0`, … for an Orange Pi 6 Plus cluster.

## 2. Hardware checklist

### 2.1 Raspberry Pi 5 cluster

| Item | Recommendation | Why |
|---|---|---|
| Raspberry Pi 5 | 8 GB (4 GB and 16 GB also work) | Global memory is roughly 4.8 GB per 8 GB worker |
| **Cooling** | **Official Active Cooler (or equivalent) on every Pi** | Sustained kernels run at 100% on all cores; without active cooling the SoC throttles at 80–85 °C (the dashboard shows this) |
| Power | Official 27 W USB-C PSU per Pi, or a PoE+ HAT | Under-voltage causes throttling and crashes. A 5-Pi cluster draws about 22 W idle and 56–67 W under load; see [POWER.md](POWER.md) |
| Storage | 32 GB+ A2 microSD, or NVMe via HAT | KUDA-Lite does not use disk at runtime; this is only for the OS |
| Network | Unmanaged **gigabit** switch (8-port for up to 6 workers plus an uplink), Cat 6 cables | All global-memory traffic crosses it |
| Optional | 2.5 GbE HAT on the controller | Host copies flow through the controller ([MATMUL.md §4](MATMUL.md#4-performance-model)) |

### 2.2 Orange Pi 6 Plus cluster

| Item | Recommendation | Why |
|---|---|---|
| Orange Pi 6 Plus | 32 GB or 64 GB (16 GB works) | About 38 GB of global memory per 64 GB worker; 12 cores per board |
| **Cooling** | The board's active cooler, with open airflow | Sustained all-core load; published tests stayed under 60 °C with it |
| **Power** | **USB-C PD supply, 20 V / 5 A (100 W)** per board, such as the official Orange Pi 100 W adapter | Each board peaks around 42 W; underpowered boards reset under load. A 5-board cluster draws about 100 W idle and 180–250 W under load; see [POWER_OPI6PLUS.md](POWER_OPI6PLUS.md) |
| Storage | NVMe SSD in an M.2 slot (recommended), or microSD / eMMC | Only for the OS |
| **Network** | **Multi-gig switch that runs ports at 5 GbE** (e.g. 8-port 10GBASE-T such as the TP-Link TL-SX1008), **Cat 6 or better** | To use the board's 5 GbE. A 2.5 GbE switch works, at half the speed; a 1 GbE switch wastes most of the board's network |
| Ports | One 5 GbE port per board to the cluster switch | The second port is optional; see [PLATFORMS.md §3.2](PLATFORMS.md#32-two-5-gbe-ports) |

## 3. Prepare each node

### 3.1 Operating system

Any **64-bit** Linux distribution works. `install.sh` supports the apt, dnf, pacman, zypper and apk package managers (only the build steps have been exercised so far, not each package manager), and the services need systemd. A 32-bit OS is not supported.

#### Raspberry Pi 5

Use **Raspberry Pi OS Lite (64-bit)** or **Ubuntu Server 24.04 LTS (arm64)**. With **Raspberry Pi Imager**, set these in the OS customisation dialog for each card:

| Setting | Value |
|---|---|
| Hostname | `pi5-ctl` for the controller; `pi5-w0`, `pi5-w1`, … for the workers |
| Username | e.g. `pi` (the same on every node keeps the inventory simple) |
| SSH | enabled, **public-key authentication only**, pasting your public key (`~/.ssh/id_ed25519.pub`) |
| Locale / time zone | yours |

Boot each Pi once, log in with SSH, and update:

```bash
sudo apt update && sudo apt full-upgrade -y && sudo reboot
```

#### Orange Pi 6 Plus

Use Orange Pi's **official Debian 12 or Ubuntu image** for the Orange Pi 6 Plus, from orangepi.org. It carries the vendor kernel with CIX P1 support; mainline support is still developing ([PLATFORMS.md §3.3](PLATFORMS.md#33-operating-system)).

1. Write the image to the NVMe/microSD with balenaEtcher, or `dd` on Linux.
2. Boot the board with a screen and keyboard, or find its address on your router, and log in with the image's default user (`orangepi`). Change the password at once.
3. Set the hostname and install your SSH key:

   ```bash
   sudo hostnamectl set-hostname opi6-w0
   ```

   ```bash
   mkdir -p ~/.ssh && echo "<your public key>" >> ~/.ssh/authorized_keys && chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys
   ```

4. Update, then confirm the cluster port negotiated 5 Gb/s (use your port's name from `ip link`):

   ```bash
   sudo apt update && sudo apt full-upgrade -y && sudo reboot
   ```

   ```bash
   sudo ethtool eth0 | grep Speed
   ```

The image's Node.js is too old for the dashboard, so add `--install-node` when installing it (§4.4).

### 3.2 Fixed addresses and names

Workers find each other through the addresses they register with, so every board needs a **stable address**: a DHCP reservation on your router, or a static IP. Example plan:

| Host | Address |
|---|---|
| pi5-ctl | 10.0.0.10 |
| pi5-w0 … pi5-w7 | 10.0.0.11 … 10.0.0.18 |

The names must resolve **on every node and on your machine**. Use local DNS / mDNS (`pi5-ctl.local`), or add the plan to `/etc/hosts` everywhere:

```
10.0.0.10  pi5-ctl
10.0.0.11  pi5-w0
10.0.0.12  pi5-w1
```

### 3.3 Time synchronisation

The dashboard plots each node's samples by that node's clock, so keep the clocks in sync. Most distributions, including Raspberry Pi OS and Ubuntu, enable `systemd-timesyncd` by default. Check with:

```bash
timedatectl
```

Look for `System clock synchronized: yes`. On an isolated network, point every node at the controller running `chrony` as a local time server.

### 3.4 Firewall

If a firewall is enabled, open these ports on the cluster interface:

| Node | Port | From | For |
|---|---|---|---|
| controller | 7070/tcp | your machine and anything running KUDA-Lite programs | host API |
| controller | 7071/tcp | workers | registration, telemetry |
| every worker | 7100/tcp | controller and other workers | global-memory data plane |
| dashboard node | 3000/tcp | browsers | dashboard |

With ufw, on the controller:

```bash
sudo ufw allow from 10.0.0.0/24 to any port 7070:7071 proto tcp && sudo ufw allow from 10.0.0.0/24 to any port 3000 proto tcp
```

With ufw, on each worker:

```bash
sudo ufw allow from 10.0.0.0/24 to any port 7100 proto tcp
```

With firewalld, use `sudo firewall-cmd --permanent --add-port=7100/tcp` and so on, then `sudo firewall-cmd --reload`.

**Do not expose these ports to the internet.** The v1 protocol has no authentication ([ARCHITECTURE.md §11](ARCHITECTURE.md#11-security)).

## 4. Deploy the whole cluster from your machine

### 4.1 Prerequisites on your machine

`bash`, `ssh` and `rsync`. That's all: the build runs on the boards. On macOS, the stock bash 3.2 is fine.

Check that you can reach every node without a password (Orange Pi example: `orangepi@opi6-w0`):

```bash
for h in pi5-ctl pi5-w0 pi5-w1 pi5-w2 pi5-w3; do ssh -o BatchMode=yes pi@$h hostname; done
```

If that prompts for a password, install your key first with `ssh-copy-id pi@<host>`.

### 4.2 Describe the cluster

Start from the example for your board:

```bash
cp deploy/cluster.inventory.example deploy/cluster.inventory
```

For an Orange Pi 6 Plus cluster, use the other example instead:

```bash
cp deploy/cluster.inventory.opi6plus.example deploy/cluster.inventory
```

Edit `deploy/cluster.inventory`: one line per node, `role host [ssh-user]`. The board type isn't listed; each node's board is detected when it's installed, so mixed clusters need nothing extra. Raspberry Pi 5 example:

```
controller  pi5-ctl  pi
worker      pi5-w0   pi
worker      pi5-w1   pi
worker      pi5-w2   pi
worker      pi5-w3   pi
dashboard   pi5-ctl  pi
```

### 4.3 Review the generated configuration (optional)

```bash
deploy/deploy-cluster.sh render
```

This writes, for every node in the inventory, the exact files `install.sh` will create there, under `deploy/rendered/<host>/`: the config files in `etc/kudalite/` with the controller address filled in, and the systemd units in `etc/systemd/system/`. It needs no SSH and touches no node. It's the place to check addresses and ports before installing, or to take files for an SD-card image. The output directory is git-ignored.

### 4.4 Install

```bash
deploy/deploy-cluster.sh install
```

For each node, in order (controller, then workers, then dashboard), this:

1. copies the source tree to `~/kudalite-src` on the node;
2. runs `sudo deploy/install.sh <role>` there, which:
   - installs the build tools (and, for the dashboard, Python) with the distribution's package manager;
   - detects the board and builds KUDA-Lite tuned for it (Raspberry Pi 5: `-mcpu=cortex-a76`; Orange Pi 6 Plus: `-mcpu=cortex-a720`, or `cortex-a710` with Debian 12's GCC 12; anything else: generic), runs the unit tests, and installs to `/usr/local`;
   - creates the `kudalite` system user;
   - writes `/etc/kudalite/<role>.conf`, with the controller address filled in for workers (existing files are never overwritten);
   - installs and starts the systemd service.

`sudo` may prompt for a password on each node. With passwordless sudo on the workers, `-p` installs them in parallel. Useful extras go through `-x`:

```bash
deploy/deploy-cluster.sh -x "--tune-network --install-node" install
```

**For an Orange Pi 6 Plus cluster, use exactly that command:** the 5 GbE profile and a newer Node.js are both needed there.

| `install.sh` option | Effect |
|---|---|
| `--tune-network` | Installs the board's network profile as `/etc/sysctl.d/90-kudalite.conf`: `deploy/sysctl/pi5.conf` (bigger TCP buffers for 1–2.5 GbE) or `deploy/sysctl/opi6plus.conf` (64 MB windows and a larger backlog for 5 GbE) |
| `--install-node` | For the dashboard: if the system Node.js is older than 20.9 (Raspberry Pi OS and Orange Pi's Debian 12 ship 18), downloads Node.js 22 LTS from nodejs.org into `/opt/kudalite/node` and verifies its checksum |
| `--platform NAME` | Overrides board detection: `pi5`, `opi6plus` or `generic` (normally not needed) |

Expect a few minutes per board, most of it the compile. The dashboard's first build (npm) adds a few more. These are estimates; they have not been timed on real hardware yet.

### 4.5 Verify

```bash
deploy/deploy-cluster.sh status
```

Every service should read `active`, and `cl-info` should list all workers with their arena sizes.

```bash
deploy/deploy-cluster.sh verify
```

This runs `cl-test-memory` and `cl-test-matmul` (up to 2048×2048) on the controller against the live cluster. It prints `PASS` and GFLOP/s per size. Record these figures; they are your baseline.

Then open the dashboard at `http://pi5-ctl:3000/`.

## 5. Use the cluster from your machine

Build and install the host library and tools on your machine:

```bash
cmake --preset host && cmake --build --preset host && sudo cmake --install build/host
```

(On Linux you can use `sudo deploy/install.sh host` instead.) Then point programs at the controller:

```bash
export KUDALITE_CONTROLLER=pi5-ctl:7070
```

```bash
cl-info && cl-top
```

Your own programs call `clInit(nullptr)` (which reads `$KUDALITE_CONTROLLER`) and link against `kudalite::kudalite` ([BUILD.md §5](BUILD.md#using-kuda-lite-from-your-own-cmake-project)).

## 6. Manual installation (one node at a time)

Everything `deploy-cluster.sh` does, done by hand. First get the source onto the node. The simplest way is the release tarball: build it on your machine, copy it over, and unpack it.

```bash
scripts/package-release.sh
```

```bash
scp dist/kudalite-0.3.0.tar.gz dist/kudalite-0.3.0.tar.gz.sha256 pi@pi5-w0:
```

On the node:

```bash
sha256sum -c kudalite-0.3.0.tar.gz.sha256 && tar xzf kudalite-0.3.0.tar.gz && cd kudalite-0.3.0
```

Then run the command for the node's role:

On the controller:

```bash
sudo deploy/install.sh controller
```

On each worker:

```bash
sudo deploy/install.sh worker --controller pi5-ctl
```

For the dashboard, usually on the controller:

```bash
sudo deploy/install.sh dashboard --install-node
```

`sudo deploy/install.sh --help` lists every option. The script is idempotent; re-running it is how you upgrade.

**Without systemd** (e.g. Alpine with OpenRC), `install.sh` installs the binaries and config and prints the commands to run. Start them with your init system:

```
/usr/local/bin/cl-controller --config /etc/kudalite/controller.conf
/usr/local/bin/cl-worker --config /etc/kudalite/worker.conf
```

Keep workers under a supervisor that restarts them (see §8.3).

## 7. Configuration

| File | Read by | Contents |
|---|---|---|
| `/etc/kudalite/controller.conf` | `kudalite-controller` | ports, page size, scheduling, telemetry, log level |
| `/etc/kudalite/worker.conf` | `kudalite-worker` | controller address, data port, **memory and cache sizes**, threads, telemetry |
| `/etc/kudalite/dashboard.env` | both dashboard services | controller endpoint, ports, health thresholds |

The files are commented; each key is the daemon's `--option` name (`cl-worker --help`). After editing, restart the service:

```bash
sudo systemctl restart kudalite-worker
```

When an upgrade ships a changed template, `install.sh` keeps your file and writes `<file>.new` next to it for you to merge.

**Sizing a worker.** Defaults are `mem` = 60% and `cache` = 15% of RAM, which leaves about 25% for the OS. The page cache should hold the largest input that every block reads; for GEMM that's the B matrix ([MATMUL.md §3](MATMUL.md#3-data-movement)).

| Workload | 8 GB Raspberry Pi 5: `mem` / `cache` | 64 GB Orange Pi 6 Plus: `mem` / `cache` |
|---|---|---|
| Default / mixed | *(unset → 4.8 GB / 1.2 GB)* | *(unset → 38 GB / 9.6 GB)* |
| Very large matrices, many allocations | `5500M` / `1000M` | `46G` / `8G` |
| Large shared inputs (e.g. GEMM with big B) | `4000M` / `2000M` | `32G` / `16G` |

In a **mixed cluster**, a striped allocation is limited by the smallest worker arena ([PLATFORMS.md §1](PLATFORMS.md#1-choosing)). Set `mem` on the larger boards if you want arenas to match.

The arena is reserved lazily: a large `mem` costs nothing until data is actually stored.

## 8. Operations

### 8.1 Everyday commands

From your machine:

| Task | Command |
|---|---|
| Health of every node | `deploy/deploy-cluster.sh status` |
| Preview config for every node | `deploy/deploy-cluster.sh render` |
| Acceptance test | `deploy/deploy-cluster.sh verify` |
| Follow one node's logs | `deploy/deploy-cluster.sh logs pi5-w2` |
| Restart everything | `deploy/deploy-cluster.sh restart` |
| Stop everything | `deploy/deploy-cluster.sh stop` |
| Live terminal view | `cl-top pi5-ctl` |

On a node:

| Task | Command |
|---|---|
| Service state | `systemctl status kudalite-worker` |
| Logs | `journalctl -u kudalite-worker -f` |
| More detailed logs | set `log-level = debug` in the conf file and restart |

### 8.2 Upgrading

Pull or copy the new source to your machine, then run:

```bash
deploy/deploy-cluster.sh install
```

Upgrading restarts every daemon, which ends all running host sessions and **clears global memory**. Stop your jobs first. The controller and workers must run the same version, because the protocol version is checked at the first message.

### 8.3 What happens when things fail

| Event | What KUDA-Lite does | What you do |
|---|---|---|
| A worker reboots or crashes | Its slice of every allocation is lost. Running jobs touching it fail with `clErrorWorkerLost`. It rejoins automatically, empty. | Re-run the affected job |
| The controller restarts | All sessions end and global memory is reset. Workers exit, systemd restarts them, and they re-register. | Re-run jobs |
| A node overheats | The clock drops (dashboard: Hot, lower MHz). Work continues, more slowly; dynamic scheduling hands it fewer blocks. | Check the cooler and airflow |
| Network cable pulled | Like a crash for that node | Reconnect; it rejoins |

KUDA-Lite keeps no persistent state, so there is nothing to back up except `/etc/kudalite/`.

### 8.4 Adding or removing a worker

To add one, prepare the board (§3), add a `worker` line to the inventory, and run `deploy/deploy-cluster.sh install`. Alternatively, run `sudo deploy/install.sh worker --controller pi5-ctl` on the new Pi alone. It joins immediately; new allocations use it, existing ones don't.

To remove one, run `sudo systemctl disable --now kudalite-worker` on it, between jobs. Its entry shows as offline in the dashboard until the controller restarts.

## 9. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `cl-info`: "could not connect to the KUDA-Lite controller" | Controller down, wrong address, or firewall on 7070 | `systemctl status kudalite-controller`; check `$KUDALITE_CONTROLLER`; §3.4 |
| Worker log repeats "controller … not reachable; retrying" | Wrong `controller =` in `worker.conf`, name doesn't resolve on the worker, or firewall on 7071 | Fix `worker.conf` / `/etc/hosts`; `nc -zv pi5-ctl 7071` from the worker |
| `cl-info` lists fewer workers than expected | Those workers are failing to register | `deploy/deploy-cluster.sh logs <worker>` |
| Copies or kernels fail with `clErrorNetwork` | Workers can't reach each other on 7100: firewall, or a wrong advertised address on multi-homed boards (Orange Pi 6 Plus with both ports connected) | Open 7100; set `advertise = <cluster IP>` in `worker.conf` |
| `clErrorMemoryAllocation` although memory is free | A striped allocation needs space on *every* worker; one is full | `cl-info` (per-worker USED); free memory; raise `mem` |
| Dashboard shows "Controller unreachable" | Wrong `KUDALITE_CONTROLLER` in `dashboard.env` | Fix it, then `systemctl restart kudalite-dashboard-api` |
| Dashboard page doesn't load | Web service down, or port 3000 blocked | `systemctl status kudalite-dashboard-web`; §3.4 |
| `install.sh`: "cmake … is too old" | Old distribution | `pip install cmake`, or backports; re-run with `--no-deps` |
| `install.sh`: "the dashboard needs Node.js >= 20.9" | Distribution Node.js too old | Re-run with `--install-node` |
| Low GFLOP/s on one node | Thermal throttling | Dashboard temperature and MHz; fix cooling |
| Orange Pi 6 Plus: slow transfers, cluster no faster than gigabit | Port negotiated 1 GbE (cable, switch port, or a gigabit switch) | `sudo ethtool <iface> \| grep Speed` should show `5000Mb/s`; use Cat 6 and a 5 GbE-capable switch port |
| Orange Pi 6 Plus: other nodes can't reach a worker that has both ports connected | Worker advertised its non-cluster port's address | Set `advertise = <cluster-port IP>` in `worker.conf` and restart the worker |
| Orange Pi 6 Plus: board resets under load | Supply can't hold 20 V / 5 A | Use the 100 W USB-C PD supply ([POWER_OPI6PLUS.md §7](POWER_OPI6PLUS.md#7-practical-power-setup)) |
| `install.sh` reports `platform=generic` on an Orange Pi | Image exposes neither the model nor the CPU part | Re-run with `--platform opi6plus` |

## 10. Uninstall

On each node:

```bash
sudo systemctl disable --now kudalite-controller kudalite-worker kudalite-dashboard-api kudalite-dashboard-web
```

```bash
sudo rm -f /etc/systemd/system/kudalite-*.service /usr/local/bin/cl-* /etc/sysctl.d/90-kudalite.conf && sudo rm -rf /etc/kudalite /opt/kudalite /usr/local/include/kudalite /usr/local/lib/cmake/kudalite /usr/local/share/kudalite /usr/local/share/doc/kudalite /usr/local/lib/libkudalite.* /var/tmp/kudalite-build-* && sudo systemctl daemon-reload
```

```bash
sudo userdel kudalite
```
