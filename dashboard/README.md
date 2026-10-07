<!--
Copyright 2026 Christopher Hinds, Stratum Labs
SPDX-License-Identifier: Apache-2.0 (see the LICENSE file at the project root)
-->

# KUDA-Lite Cluster Dashboard

A small web service that shows live telemetry for every board (Raspberry Pi 5, Orange Pi 6 Plus, or mixed) in a KUDA-Lite cluster, including each node's board model: memory, compute and OS threads, CPU, SoC temperature, the kernel each worker is running, and network traffic.

```
 browser ──HTTP──▶ Next.js front end ──/api/* proxy──▶ FastAPI backend ──KUDA-Lite protocol :7070──▶ cl-controller
 (React UI)         frontend/                           backend/          (GetTelemetry, every 1 s)     (history of all nodes)
```

- **Backend** (`backend/`, Python 3.10+, FastAPI). Keeps one session to the controller and speaks the KUDA-Lite binary protocol directly (no C++ bindings needed). It polls `GetTelemetry` every second, keeps 15 minutes of history per node in memory, computes rates (network, blocks/s) and health levels, and serves JSON. It reconnects automatically when the controller restarts.
- **Front end** (`frontend/`, Next.js 16, React 19, TypeScript). One page: cluster summary tiles; a card per node with its board model, a CPU sparkline, memory and arena meters, busy compute-thread cells, OS thread count, temperature, executor state and network rates; a table view with every value; and 5 or 15 minutes of charts for the selected Pi (crosshair tooltip, keyboard navigable). Light and dark themes. No chart library: plain SVG.

Background and field definitions: [docs/OBSERVABILITY.md](../docs/OBSERVABILITY.md).

## Running it

> These are development instructions. For production, `deploy/install.sh dashboard` installs it as two systemd services; see [docs/DEPLOYMENT.md](../docs/DEPLOYMENT.md).

**1. Backend** (on any machine that can reach the controller's host port):

```bash
cd dashboard/backend && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

```bash
cd dashboard/backend && KUDALITE_CONTROLLER=pi5-ctl:7070 .venv/bin/uvicorn app.main:app --host 127.0.0.1 --port 8000
```

Interactive API docs are then at `http://127.0.0.1:8000/docs`.

**2. Front end:**

```bash
cd dashboard/frontend && npm install
```

```bash
cd dashboard/frontend && DASHBOARD_API_URL=http://127.0.0.1:8000 npm run dev
```

Open `http://localhost:3000`. For production use `npm run build`, then `DASHBOARD_API_URL=… npm start`. The backend URL is read at runtime, so one build works anywhere.

**No cluster yet?** Run the mock controller. It speaks the real protocol and simulates boards alternating between idle and matmul launches, with one worker running hot. `--platform` picks `pi5` (default), `opi6plus`, or `mixed` (a Pi 5 controller with Orange Pi 6 Plus workers):

```bash
cd dashboard/backend && .venv/bin/python -m app.mock_controller --port 7070 --workers 6 --offline 1 --platform opi6plus
```

## Configuration

| Variable | Used by | Default | Meaning |
|---|---|---|---|
| `KUDALITE_CONTROLLER` | backend | `127.0.0.1:7070` | Controller host API endpoint |
| `DASHBOARD_POLL_SECONDS` | backend | `1.0` | Poll period |
| `DASHBOARD_HISTORY_SECONDS` | backend | `900` | History kept per node |
| `DASHBOARD_STALE_SECONDS` | backend | `5` | Age after which a node is shown as stale |
| `DASHBOARD_TEMP_WARNING_C` / `_CRITICAL_C` | backend | `70` / `80` | SoC temperature levels |
| `DASHBOARD_MEM_WARNING_PCT` / `_CRITICAL_PCT` | backend | `80` / `90` | System memory levels |
| `DASHBOARD_CORS_ORIGINS` | backend | `http://localhost:3000,http://127.0.0.1:3000` | Only needed if browsers call the backend directly |
| `DASHBOARD_API_URL` | front end | `http://127.0.0.1:8000` | Where the Next.js server proxies `/api/*` |

## HTTP API

| Endpoint | Returns |
|---|---|
| `GET /api/health` | `{controller, connected, sessionId, lastUpdateMs, error}` |
| `GET /api/cluster` | Summary counters, health thresholds, and the latest view of every node (controller first) |
| `GET /api/nodes/{key}/history?seconds=300` | Time series for one node. `key` is `controller` or `worker-<id>`; 404 if unknown. |

JSON field names are camelCase. The full schemas are in `/docs` (OpenAPI), mirrored in `frontend/lib/types.ts`.

## Layout

```
backend/app/protocol.py        framing + telemetry decoders/encoders (mirrors src/common/*.h)
backend/app/client.py          asyncio controller client (Hello, GetTelemetry)
backend/app/store.py           history, rates, health levels, JSON views
backend/app/main.py            FastAPI app and the reconnecting poller
backend/app/models.py          response models
backend/app/config.py          environment settings
backend/app/mock_controller.py synthetic cluster for development and tests
backend/tests/                 protocol round trips; API end to end against the mock over TCP
frontend/app/                  layout, page, global styles (design tokens), /api proxy route
frontend/components/           Dashboard, SummaryTiles, NodeCard, NodeTable, NodeDetail, TimeSeriesChart, Bits
frontend/lib/                  API types, polling hook, formatting
```

## Tests

```bash
cd dashboard/backend && .venv/bin/pip install -r requirements-dev.txt && .venv/bin/python -m pytest -q
```

```bash
cd dashboard/frontend && npm run typecheck && npm run build
```
