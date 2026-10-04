# Deploying KUDA-Lite on a Raspberry Pi 5 Cluster

This guide takes you from a pile of Raspberry Pi 5 boards to a running cluster with the dashboard, and then covers day-to-day operation. For building from source by hand, see [BUILD.md](BUILD.md).

**Summary:**

1. Flash a 64-bit Linux on every Pi, with hostnames and SSH keys.
2. Give each Pi a fixed address.
3. Copy `deploy/cluster.inventory.example` to `deploy/cluster.inventory` and list your Pis in it.
4. Optionally run `deploy/deploy-cluster.sh render` and review the generated per-node configuration.
5. Run `deploy/deploy-cluster.sh install`.
6. Run `deploy/deploy-cluster.sh verify`.
7. Open `http://<controller>:3000/`.

## 1. Topology

```
                         ┌──────────────────────── Gigabit switch (cluster network) ─────────────────────────┐
                         │                                                                                    │
 your Linux/macOS  ──────┤      pi5-ctl                   pi5-w0         pi5-w1         pi5-w2   …   pi5-wN   │
 machine (host)          │  kudalite-controller          kudalite-      kudalite-      kudalite-     kudalite- │
  • your programs        │  kudalite-dashboard-api/web   worker         worker         worker        worker    │
  • deploy scripts       │  :7070 hosts  :7071 workers   :7100          :7100          :7100         :7100     │
  • browser → :3000      │  :3000 dashboard                                                                   │
                         └────────────────────────────────────────────────────────────────────────────────────┘
```

- **One controller Pi.** It runs `cl-controller`, and usually the dashboard as well. Its CPU is mostly idle; it's the network hub for host copies.
- **N worker Pis.** Each runs `cl-worker`, which contributes about 60% of its RAM to global memory.
- **Your development machine** (Linux or macOS) runs KUDA-Lite programs and deploys the cluster over SSH. It needs to reach the controller's port 7070, and your browser needs port 3000.

## 2. Hardware checklist

| Item | Recommendation | Why |
|---|---|---|
| Raspberry Pi 5 | 8 GB (4 GB and 16 GB also work) | Global memory is roughly 4.8 GB per 8 GB worker |
| **Cooling** | **Official Active Cooler (or equivalent) on every Pi** | Sustained kernels run at 100% on all cores; without active cooling the SoC throttles at 80–85 °C (the dashboard shows this) |
| Power | Official 27 W USB-C PSU per Pi, or a PoE+ HAT | Under-voltage causes throttling and crashes. A 5-Pi cluster draws about 22 W idle and 56–67 W under load; see [POWER.md](POWER.md) |
| Storage | 32 GB+ A2 microSD, or NVMe via HAT | KUDA-Lite does not use disk at runtime; this is only for the OS |
| Network | Unmanaged **gigabit** switch (8-port for up to 6 workers plus an uplink), Cat 6 cables | All global-memory traffic crosses it |
| Optional | 2.5 GbE HAT on the controller | Host copies flow through the controller ([MATMUL.md §4](MATMUL.md#4-performance-model-pi-5-cluster-1-gbe)) |

## 3. Prepare each Pi

### 3.1 Operating system

Any **64-bit** Linux distribution works. `install.sh` supports the apt, dnf, pacman, zypper and apk package managers (only the build steps have been exercised so far, not each package manager), and the services need systemd. The easiest choices:

- **Raspberry Pi OS Lite (64-bit)**, or
- **Ubuntu Server 24.04 LTS (arm64)**.

A 32-bit OS is not supported.

With **Raspberry Pi Imager**, set these in the OS customisation dialog for each card:

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

### 3.2 Fixed addresses and names

Workers find each other through the addresses they register with, so every Pi needs a **stable address**: a DHCP reservation on your router, or a static IP. Example plan:

| Host | Address |
|---|---|
| pi5-ctl | 10.0.0.10 |
| pi5-w0 … pi5-w7 | 10.0.0.11 … 10.0.0.18 |

The names must resolve **on every Pi and on your machine**. Use local DNS / mDNS (`pi5-ctl.local`), or add the plan to `/etc/hosts` everywhere:

```
10.0.0.10  pi5-ctl
10.0.0.11  pi5-w0
10.0.0.12  pi5-w1
```

### 3.3 Time synchronisation

The dashboard plots each node's samples by that node's clock, so keep the clocks in sync. Raspberry Pi OS and Ubuntu enable `systemd-timesyncd` by default. Check with:

```bash
timedatectl
```

Look for `System clock synchronized: yes`. On an isolated network, point every Pi at the controller running `chrony` as a local time server.

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

`bash`, `ssh` and `rsync`. That's all: the build runs on the Pis. On macOS, the stock bash 3.2 is fine.

Check that you can reach every Pi without a password:

```bash
for h in pi5-ctl pi5-w0 pi5-w1 pi5-w2 pi5-w3; do ssh -o BatchMode=yes pi@$h hostname; done
```

If that prompts for a password, install your key first with `ssh-copy-id pi@<host>`.

### 4.2 Describe the cluster

```bash
cp deploy/cluster.inventory.example deploy/cluster.inventory
```

Edit `deploy/cluster.inventory`: one line per node, `role host [ssh-user]`:

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
   - builds KUDA-Lite tuned for the Pi 5 (`-mcpu=cortex-a76`), runs the unit tests, and installs to `/usr/local`;
   - creates the `kudalite` system user;
   - writes `/etc/kudalite/<role>.conf`, with the controller address filled in for workers (existing files are never overwritten);
   - installs and starts the systemd service.

`sudo` may prompt for a password on each node. With passwordless sudo on the workers, `-p` installs them in parallel. Useful extras go through `-x`:

```bash
deploy/deploy-cluster.sh -x "--tune-network --install-node" install
```

| `install.sh` option | Effect |
|---|---|
| `--tune-network` | Installs `/etc/sysctl.d/90-kudalite.conf` (bigger TCP buffers, for 1–2.5 GbE) |
| `--install-node` | For the dashboard: if the system Node.js is older than 20.9 (Raspberry Pi OS ships 18), downloads Node.js 22 LTS from nodejs.org into `/opt/kudalite/node` and verifies its checksum |

Expect a few minutes per Pi, most of it the compile. The dashboard's first build (npm) adds a few more. These are estimates; they have not been timed on Pi hardware yet.

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
scp dist/kudalite-0.2.0.tar.gz dist/kudalite-0.2.0.tar.gz.sha256 pi@pi5-w0:
```

On the node:

```bash
sha256sum -c kudalite-0.2.0.tar.gz.sha256 && tar xzf kudalite-0.2.0.tar.gz && cd kudalite-0.2.0
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

**Sizing a worker.** Defaults are `mem` = 60% and `cache` = 15% of RAM, which leaves about 25% for the OS. The page cache should hold the largest input that every block reads; for GEMM that's the B matrix ([MATMUL.md §3](MATMUL.md#3-data-movement)). On an 8 GB Pi:

| Workload | `mem` | `cache` |
|---|---|---|
| Default / mixed | *(unset → 4.8 GB)* | *(unset → 1.2 GB)* |
| Very large matrices, many allocations | `5500M` | `1000M` |
| Large shared inputs (e.g. GEMM with big B) | `4000M` | `2000M` |

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

To add one, prepare the Pi (§3), add a `worker` line to the inventory, and run `deploy/deploy-cluster.sh install`. Alternatively, run `sudo deploy/install.sh worker --controller pi5-ctl` on the new Pi alone. It joins immediately; new allocations use it, existing ones don't.

To remove one, run `sudo systemctl disable --now kudalite-worker` on it, between jobs. Its entry shows as offline in the dashboard until the controller restarts.

## 9. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `cl-info`: "could not connect to the KUDA-Lite controller" | Controller down, wrong address, or firewall on 7070 | `systemctl status kudalite-controller`; check `$KUDALITE_CONTROLLER`; §3.4 |
| Worker log repeats "controller … not reachable; retrying" | Wrong `controller =` in `worker.conf`, name doesn't resolve on the worker, or firewall on 7071 | Fix `worker.conf` / `/etc/hosts`; `nc -zv pi5-ctl 7071` from the worker |
| `cl-info` lists fewer workers than expected | Those workers are failing to register | `deploy/deploy-cluster.sh logs <worker>` |
| Copies or kernels fail with `clErrorNetwork` | Workers can't reach each other on 7100: firewall, or a wrong advertised address on multi-homed Pis | Open 7100; set `advertise = <cluster IP>` in `worker.conf` |
| `clErrorMemoryAllocation` although memory is free | A striped allocation needs space on *every* worker; one is full | `cl-info` (per-worker USED); free memory; raise `mem` |
| Dashboard shows "Controller unreachable" | Wrong `KUDALITE_CONTROLLER` in `dashboard.env` | Fix it, then `systemctl restart kudalite-dashboard-api` |
| Dashboard page doesn't load | Web service down, or port 3000 blocked | `systemctl status kudalite-dashboard-web`; §3.4 |
| `install.sh`: "cmake … is too old" | Old distribution | `pip install cmake`, or backports; re-run with `--no-deps` |
| `install.sh`: "the dashboard needs Node.js >= 20.9" | Distribution Node.js too old | Re-run with `--install-node` |
| Low GFLOP/s on one node | Thermal throttling | Dashboard temperature and MHz; fix cooling |

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
