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

"""API response models (JSON uses camelCase for the TypeScript front end)."""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict
from pydantic.alias_generators import to_camel

Status = Literal["good", "warning", "critical"]
NodeStatus = Literal["online", "stale", "waiting", "offline"]


class ApiModel(BaseModel):
    model_config = ConfigDict(alias_generator=to_camel, populate_by_name=True)


class Health(ApiModel):
    controller: str
    connected: bool
    session_id: int | None = None
    last_update_ms: int | None = None  # wall clock of the last successful poll
    error: str | None = None


class NodeHealth(ApiModel):
    temperature: Status | None = None  # None when the node has no temperature sensor
    memory: Status


class Sparklines(ApiModel):
    """The most recent samples (oldest first) for the small in-card charts."""

    cpu_percent: list[float]
    mem_used_pct: list[float]


class NodeView(ApiModel):
    key: str  # "controller" or "worker-<id>"
    id: int
    role: Literal["worker", "controller"]
    hostname: str
    address: str
    board: str | None = None  # hardware model reported by the node (KUDA-Lite >= 0.3)
    status: NodeStatus
    last_seen_age_ms: int | None

    timestamp_ms: int | None = None
    uptime_ms: int | None = None
    mem_total_bytes: int = 0
    mem_used_bytes: int = 0
    mem_used_pct: float = 0.0
    rss_bytes: int = 0
    process_threads: int = 0
    arena_bytes: int = 0
    arena_used_bytes: int = 0
    arena_used_pct: float = 0.0
    cache_bytes: int = 0
    cache_capacity_bytes: int = 0
    cache_hit_rate: float | None = None  # 0..1 over the latest interval, None if no reads
    compute_threads: int = 0
    busy_threads: int = 0
    cpu_cores: int = 0
    cpu_percent: float = 0.0
    load1: float = 0.0
    cpu_temp_c: float | None = None
    cpu_freq_mhz: int | None = None
    queue_depth: int = 0
    executing: int = 0
    current_kernel: str | None = None
    execs_completed: int = 0
    blocks_executed: int = 0
    blocks_per_sec: float = 0.0
    utilization_pct: float = 0.0  # share of the latest interval spent executing
    net_rx_bytes_per_sec: float = 0.0
    net_tx_bytes_per_sec: float = 0.0
    health: NodeHealth
    spark: Sparklines


class ClusterSummary(ApiModel):
    controller_uptime_ms: int = 0
    sessions: int = 0
    active_launches: int = 0
    launches_total: int = 0
    allocations: int = 0
    allocated_bytes: int = 0
    arena_total_bytes: int = 0
    arena_used_bytes: int = 0
    nodes_online: int = 0
    nodes_total: int = 0
    workers_online: int = 0
    workers_total: int = 0
    compute_threads: int = 0
    busy_threads: int = 0
    mem_total_bytes: int = 0
    mem_used_bytes: int = 0
    max_temp_c: float | None = None
    max_temp_node: str | None = None


class Thresholds(ApiModel):
    temp_warning_c: float
    temp_critical_c: float
    mem_warning_pct: float
    mem_critical_pct: float


class ClusterView(ApiModel):
    controller: str
    connected: bool
    error: str | None = None
    updated_at_ms: int | None = None
    summary: ClusterSummary
    thresholds: Thresholds
    nodes: list[NodeView]


class HistoryPoint(ApiModel):
    t: int  # node wall clock, Unix epoch ms
    mem_used_bytes: int
    mem_total_bytes: int
    rss_bytes: int
    cpu_percent: float
    cpu_temp_c: float | None
    busy_threads: int
    compute_threads: int
    process_threads: int
    queue_depth: int
    blocks_per_sec: float
    net_rx_bytes_per_sec: float
    net_tx_bytes_per_sec: float


class HistoryView(ApiModel):
    key: str
    hostname: str
    points: list[HistoryPoint]
