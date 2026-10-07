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

"""In-memory telemetry history and the derived views served by the API.

The controller already keeps ~10 minutes per node; the store keeps its own (longer) window so
the dashboard survives controller restarts and can compute rates between consecutive samples.
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field

from .config import Settings
from .models import (
    ClusterSummary,
    ClusterView,
    HistoryPoint,
    NodeHealth,
    NodeView,
    Sparklines,
    Status,
    Thresholds,
)
from .protocol import ClusterSnapshot, NodeRole, NodeSnapshot, TelemetrySample

SPARK_POINTS = 60


def node_key(node: NodeSnapshot) -> str:
    return "controller" if node.role == NodeRole.CONTROLLER else f"worker-{node.id}"


@dataclass
class Derived:
    """Rates computed from a sample and the one before it."""

    blocks_per_sec: float = 0.0
    net_rx_bytes_per_sec: float = 0.0
    net_tx_bytes_per_sec: float = 0.0
    utilization_pct: float = 0.0
    cache_hit_rate: float | None = None


def derive(prev: TelemetrySample | None, cur: TelemetrySample) -> Derived:
    if prev is None:
        return Derived()
    dt = (cur.uptime_ms - prev.uptime_ms) / 1000.0
    if dt <= 0:
        return Derived()

    def rate(a: int, b: int) -> float:
        return max(0.0, (b - a) / dt)

    hits = cur.cache_hits - prev.cache_hits
    misses = cur.cache_misses - prev.cache_misses
    return Derived(
        blocks_per_sec=rate(prev.blocks_executed, cur.blocks_executed),
        net_rx_bytes_per_sec=rate(prev.net_rx_bytes, cur.net_rx_bytes),
        net_tx_bytes_per_sec=rate(prev.net_tx_bytes, cur.net_tx_bytes),
        utilization_pct=min(100.0, max(0.0, (cur.exec_busy_ns - prev.exec_busy_ns) / (dt * 1e9) * 100.0)),
        cache_hit_rate=hits / (hits + misses) if hits + misses > 0 else None,
    )


def mem_used(s: TelemetrySample) -> int:
    return max(0, s.mem_total_bytes - s.mem_available_bytes) if s.mem_available_bytes else 0


@dataclass
class NodeState:
    meta: NodeSnapshot
    samples: deque[TelemetrySample] = field(default_factory=deque)
    derived: deque[Derived] = field(default_factory=deque)


class TelemetryStore:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.nodes: dict[str, NodeState] = {}
        self.cluster: ClusterSnapshot | None = None
        self.connected = False
        self.error: str | None = None
        self.session_id: int | None = None
        self.updated_at_ms: int | None = None

    # -- ingestion -----------------------------------------------------------------------------

    def ingest(self, snap: ClusterSnapshot) -> None:
        self.cluster = snap
        self.connected = True
        self.error = None
        self.updated_at_ms = int(time.time() * 1000)
        for node in snap.nodes:
            key = node_key(node)
            state = self.nodes.get(key)
            if state is None:
                state = self.nodes[key] = NodeState(meta=node)
            state.meta = node
            for s in node.samples:
                last = state.samples[-1] if state.samples else None
                if last is not None and s.uptime_ms <= last.uptime_ms:
                    if s.uptime_ms + 1000 < last.uptime_ms and s.timestamp_ms > last.timestamp_ms:
                        # The daemon restarted (uptime went backwards): start a fresh history.
                        state.samples.clear()
                        state.derived.clear()
                        last = None
                    else:
                        continue  # already have it
                state.derived.append(derive(last, s))
                state.samples.append(s)
            self._trim(state)

    def _trim(self, state: NodeState) -> None:
        if not state.samples:
            return
        horizon = state.samples[-1].timestamp_ms - self.settings.history_seconds * 1000
        while state.samples and state.samples[0].timestamp_ms < horizon:
            state.samples.popleft()
            state.derived.popleft()

    def mark_disconnected(self, error: str) -> None:
        self.connected = False
        self.error = error
        self.session_id = None

    # -- views ---------------------------------------------------------------------------------

    def _status(self, meta: NodeSnapshot, has_samples: bool) -> str:
        if not meta.alive:
            return "offline"
        if not self.connected and has_samples:
            return "stale"  # we lost the controller; this is the last data we have
        if not has_samples or meta.last_seen_age_ms is None:
            return "waiting"
        if meta.last_seen_age_ms > self.settings.stale_seconds * 1000:
            return "stale"
        return "online"

    def _level(self, value: float, warning: float, critical: float) -> Status:
        if value >= critical:
            return "critical"
        if value >= warning:
            return "warning"
        return "good"

    def node_view(self, key: str, state: NodeState) -> NodeView:
        meta = state.meta
        role = "controller" if meta.role == NodeRole.CONTROLLER else "worker"
        status = self._status(meta, bool(state.samples))
        base = dict(
            key=key,
            id=meta.id,
            role=role,
            hostname=meta.hostname,
            address=meta.address,
            status=status,
            last_seen_age_ms=meta.last_seen_age_ms,
            arena_used_bytes=meta.arena_used_bytes,
        )
        if not state.samples:
            return NodeView(**base, health=NodeHealth(memory="good"), spark=Sparklines(cpu_percent=[], mem_used_pct=[]))

        s = state.samples[-1]
        d = state.derived[-1]
        used = mem_used(s)
        used_pct = 100.0 * used / s.mem_total_bytes if s.mem_total_bytes else 0.0
        temp = s.cpu_temp_c if s.cpu_temp_c >= 0 else None
        recent = list(state.samples)[-SPARK_POINTS:]
        return NodeView(
            **base,
            board=s.board or None,
            timestamp_ms=s.timestamp_ms,
            uptime_ms=s.uptime_ms,
            mem_total_bytes=s.mem_total_bytes,
            mem_used_bytes=used,
            mem_used_pct=used_pct,
            rss_bytes=s.rss_bytes,
            process_threads=s.process_threads,
            arena_bytes=s.arena_bytes,
            arena_used_pct=100.0 * meta.arena_used_bytes / s.arena_bytes if s.arena_bytes else 0.0,
            cache_bytes=s.cache_bytes,
            cache_capacity_bytes=s.cache_capacity_bytes,
            cache_hit_rate=d.cache_hit_rate,
            compute_threads=s.compute_threads,
            busy_threads=s.busy_threads,
            cpu_cores=s.cpu_cores,
            cpu_percent=s.cpu_percent,
            load1=s.load1,
            cpu_temp_c=temp,
            cpu_freq_mhz=s.cpu_freq_mhz or None,
            queue_depth=s.queue_depth,
            executing=s.executing,
            current_kernel=s.current_kernel or None,
            execs_completed=s.execs_completed,
            blocks_executed=s.blocks_executed,
            blocks_per_sec=d.blocks_per_sec,
            utilization_pct=d.utilization_pct,
            net_rx_bytes_per_sec=d.net_rx_bytes_per_sec,
            net_tx_bytes_per_sec=d.net_tx_bytes_per_sec,
            health=NodeHealth(
                temperature=None
                if temp is None
                else self._level(temp, self.settings.temp_warning_c, self.settings.temp_critical_c),
                memory=self._level(used_pct, self.settings.mem_warning_pct, self.settings.mem_critical_pct),
            ),
            spark=Sparklines(
                cpu_percent=[round(x.cpu_percent, 1) for x in recent],
                mem_used_pct=[
                    round(100.0 * mem_used(x) / x.mem_total_bytes, 1) if x.mem_total_bytes else 0.0 for x in recent
                ],
            ),
        )

    def cluster_view(self) -> ClusterView:
        def order(item: tuple[str, NodeState]) -> tuple[int, int]:
            meta = item[1].meta
            return (0 if meta.role == NodeRole.CONTROLLER else 1, meta.id)

        nodes = [self.node_view(k, st) for k, st in sorted(self.nodes.items(), key=order)]
        summary = ClusterSummary()
        if self.cluster is not None:
            c = self.cluster
            summary = ClusterSummary(
                controller_uptime_ms=c.controller_uptime_ms,
                sessions=c.sessions,
                active_launches=c.active_launches,
                launches_total=c.launches_total,
                allocations=c.allocations,
                allocated_bytes=c.allocated_bytes,
                arena_total_bytes=c.arena_total_bytes,
            )
        for n in nodes:
            summary.nodes_total += 1
            live = n.status in ("online", "stale")
            summary.nodes_online += live
            if n.role == "worker":
                summary.workers_total += 1
                summary.workers_online += live
                if live:
                    summary.compute_threads += n.compute_threads
                    summary.busy_threads += n.busy_threads
                    summary.arena_used_bytes += n.arena_used_bytes
            if live:
                summary.mem_total_bytes += n.mem_total_bytes
                summary.mem_used_bytes += n.mem_used_bytes
                if n.cpu_temp_c is not None and (summary.max_temp_c is None or n.cpu_temp_c > summary.max_temp_c):
                    summary.max_temp_c = n.cpu_temp_c
                    summary.max_temp_node = n.hostname
        return ClusterView(
            controller=self.settings.controller,
            connected=self.connected,
            error=self.error,
            updated_at_ms=self.updated_at_ms,
            summary=summary,
            thresholds=Thresholds(
                temp_warning_c=self.settings.temp_warning_c,
                temp_critical_c=self.settings.temp_critical_c,
                mem_warning_pct=self.settings.mem_warning_pct,
                mem_critical_pct=self.settings.mem_critical_pct,
            ),
            nodes=nodes,
        )

    def history(self, key: str, seconds: float) -> tuple[str, list[HistoryPoint]] | None:
        state = self.nodes.get(key)
        if state is None:
            return None
        points: list[HistoryPoint] = []
        if state.samples:
            horizon = state.samples[-1].timestamp_ms - seconds * 1000
            for s, d in zip(state.samples, state.derived):
                if s.timestamp_ms < horizon:
                    continue
                points.append(
                    HistoryPoint(
                        t=s.timestamp_ms,
                        mem_used_bytes=mem_used(s),
                        mem_total_bytes=s.mem_total_bytes,
                        rss_bytes=s.rss_bytes,
                        cpu_percent=round(s.cpu_percent, 2),
                        cpu_temp_c=round(s.cpu_temp_c, 2) if s.cpu_temp_c >= 0 else None,
                        busy_threads=s.busy_threads,
                        compute_threads=s.compute_threads,
                        process_threads=s.process_threads,
                        queue_depth=s.queue_depth,
                        blocks_per_sec=round(d.blocks_per_sec, 2),
                        net_rx_bytes_per_sec=round(d.net_rx_bytes_per_sec, 1),
                        net_tx_bytes_per_sec=round(d.net_tx_bytes_per_sec, 1),
                    )
                )
        return state.meta.hostname, points
