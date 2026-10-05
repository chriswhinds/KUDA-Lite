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

"""End-to-end API tests: the backend polls the mock controller over real TCP."""

import asyncio
import socket
import threading
import time

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app
from app.mock_controller import MockCluster, serve


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def mock_controller():
    port = free_port()
    ready = threading.Event()

    def run() -> None:
        async def main() -> None:
            started = asyncio.Event()
            task = asyncio.create_task(serve("127.0.0.1", port, MockCluster(workers=4, offline=1), started))
            await started.wait()
            ready.set()
            await task

        asyncio.run(main())

    threading.Thread(target=run, daemon=True).start()
    assert ready.wait(5)
    return f"127.0.0.1:{port}"


def wait_connected(client: TestClient) -> None:
    deadline = time.time() + 10
    while time.time() < deadline:
        if client.get("/api/health").json()["connected"]:
            return
        time.sleep(0.1)
    pytest.fail("backend never connected to the mock controller")


def test_cluster_view(mock_controller):
    app = create_app(Settings(controller=mock_controller, poll_seconds=0.2))
    with TestClient(app) as client:
        wait_connected(client)
        body = client.get("/api/cluster").json()
        assert body["connected"] is True
        nodes = body["nodes"]
        assert [n["key"] for n in nodes] == ["controller", "worker-0", "worker-1", "worker-2", "worker-3"]
        assert nodes[0]["role"] == "controller"
        assert nodes[-1]["status"] == "offline"
        w0 = nodes[1]
        assert w0["status"] == "online"
        assert w0["computeThreads"] == 4
        assert 0 <= w0["busyThreads"] <= 4
        assert w0["memTotalBytes"] == 8 << 30
        assert 0 < w0["memUsedPct"] < 100
        assert w0["health"]["memory"] in ("good", "warning", "critical")
        assert w0["spark"]["cpuPercent"]
        s = body["summary"]
        assert s["workersTotal"] == 4 and s["workersOnline"] == 3
        assert s["computeThreads"] == 12
        assert s["maxTempC"] is not None


def test_history_and_unknown_node(mock_controller):
    app = create_app(Settings(controller=mock_controller, poll_seconds=0.2))
    with TestClient(app) as client:
        wait_connected(client)
        time.sleep(1.2)  # let the mock produce another sample
        body = client.get("/api/nodes/worker-0/history", params={"seconds": 300}).json()
        assert body["hostname"] == "pi5-w0"
        points = body["points"]
        assert len(points) >= 2
        assert points == sorted(points, key=lambda p: p["t"])
        assert {"memUsedBytes", "cpuPercent", "busyThreads", "netRxBytesPerSec"} <= points[-1].keys()
        assert client.get("/api/nodes/worker-99/history").status_code == 404


def test_disconnected_state_is_reported():
    app = create_app(Settings(controller=f"127.0.0.1:{free_port()}", poll_seconds=0.2))
    with TestClient(app) as client:
        time.sleep(0.5)
        health = client.get("/api/health").json()
        assert health["connected"] is False and health["error"]
        body = client.get("/api/cluster").json()
        assert body["connected"] is False and body["nodes"] == []
