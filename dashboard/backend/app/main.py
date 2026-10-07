# Copyright 2026 Christopher Hinds, Stratum Labs
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""FastAPI application: polls the KUDA-Lite controller and serves cluster telemetry as JSON.

Run:  uvicorn app.main:app --host 0.0.0.0 --port 8000
API docs are generated at /docs.
"""

from __future__ import annotations

import asyncio
import logging
import math
from contextlib import asynccontextmanager, suppress

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware

from .client import ControllerClient, ControllerError, parse_endpoint
from .config import Settings
from .models import ClusterView, Health, HistoryView
from .protocol import ProtocolError
from .store import TelemetryStore

log = logging.getLogger("kudalite.dashboard")

# The controller keeps up to 600 samples per node; ask for all of them once to backfill charts.
BACKFILL_SAMPLES = 600


class Poller:
    """Keeps one controller session open and feeds the store; reconnects with backoff."""

    def __init__(self, settings: Settings, store: TelemetryStore) -> None:
        self.settings = settings
        self.store = store
        self.host, self.port = parse_endpoint(settings.controller)

    async def run(self) -> None:
        backoff = 1.0
        # Samples that can arrive between two polls (1 s telemetry period) plus slack.
        per_poll = max(2, math.ceil(self.settings.poll_seconds) + 2)
        while True:
            client = ControllerClient(self.host, self.port)
            try:
                await client.connect()
                self.store.session_id = client.session_id
                log.info("connected to controller %s:%d (session %s)", self.host, self.port, client.session_id)
                self.store.ingest(await client.get_telemetry(BACKFILL_SAMPLES))
                backoff = 1.0
                while True:
                    await asyncio.sleep(self.settings.poll_seconds)
                    self.store.ingest(await client.get_telemetry(per_poll))
            except asyncio.CancelledError:
                await client.close()
                raise
            except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError, ProtocolError, ControllerError) as e:
                message = f"{type(e).__name__}: {e}" if str(e) else type(e).__name__
                if self.store.connected or self.store.error != message:
                    log.warning("controller %s:%d unavailable: %s", self.host, self.port, message)
                self.store.mark_disconnected(message)
                await client.close()
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 10.0)


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings()
    store = TelemetryStore(settings)
    poller = Poller(settings, store)

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        task = asyncio.create_task(poller.run())
        try:
            yield
        finally:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task

    app = FastAPI(
        title="KUDA-Lite Dashboard API",
        version="0.1.0",
        summary="Live telemetry for every board (Raspberry Pi 5, Orange Pi 6 Plus) in a KUDA-Lite cluster.",
        lifespan=lifespan,
    )
    app.state.store = store
    app.add_middleware(
        CORSMiddleware, allow_origins=settings.cors_origins, allow_methods=["GET"], allow_headers=["*"]
    )

    @app.get("/api/health", response_model=Health)
    def health() -> Health:
        return Health(
            controller=settings.controller,
            connected=store.connected,
            session_id=store.session_id,
            last_update_ms=store.updated_at_ms,
            error=store.error,
        )

    @app.get("/api/cluster", response_model=ClusterView)
    def cluster() -> ClusterView:
        """Cluster summary plus the latest telemetry of every node (controller first)."""
        return store.cluster_view()

    @app.get("/api/nodes/{key}/history", response_model=HistoryView)
    def history(key: str, seconds: float = Query(300.0, gt=0, le=86400)) -> HistoryView:
        """Time series for one node: `controller` or `worker-<id>`."""
        found = store.history(key, seconds)
        if found is None:
            raise HTTPException(status_code=404, detail=f"unknown node '{key}'")
        hostname, points = found
        return HistoryView(key=key, hostname=hostname, points=points)

    return app


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s [%(name)s] %(message)s")
app = create_app()
