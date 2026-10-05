// Copyright 2026 Christopher Hinds, Stratum Labs
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Mirrors dashboard/backend/app/models.py (JSON is camelCase).

export type Level = "good" | "warning" | "critical";
export type NodeStatus = "online" | "stale" | "waiting" | "offline";

export interface NodeView {
  key: string;
  id: number;
  role: "worker" | "controller";
  hostname: string;
  address: string;
  status: NodeStatus;
  lastSeenAgeMs: number | null;
  timestampMs: number | null;
  uptimeMs: number | null;
  memTotalBytes: number;
  memUsedBytes: number;
  memUsedPct: number;
  rssBytes: number;
  processThreads: number;
  arenaBytes: number;
  arenaUsedBytes: number;
  arenaUsedPct: number;
  cacheBytes: number;
  cacheCapacityBytes: number;
  cacheHitRate: number | null;
  computeThreads: number;
  busyThreads: number;
  cpuCores: number;
  cpuPercent: number;
  load1: number;
  cpuTempC: number | null;
  cpuFreqMhz: number | null;
  queueDepth: number;
  executing: number;
  currentKernel: string | null;
  execsCompleted: number;
  blocksExecuted: number;
  blocksPerSec: number;
  utilizationPct: number;
  netRxBytesPerSec: number;
  netTxBytesPerSec: number;
  health: { temperature: Level | null; memory: Level };
  spark: { cpuPercent: number[]; memUsedPct: number[] };
}

export interface ClusterSummary {
  controllerUptimeMs: number;
  sessions: number;
  activeLaunches: number;
  launchesTotal: number;
  allocations: number;
  allocatedBytes: number;
  arenaTotalBytes: number;
  arenaUsedBytes: number;
  nodesOnline: number;
  nodesTotal: number;
  workersOnline: number;
  workersTotal: number;
  computeThreads: number;
  busyThreads: number;
  memTotalBytes: number;
  memUsedBytes: number;
  maxTempC: number | null;
  maxTempNode: string | null;
}

export interface ClusterView {
  controller: string;
  connected: boolean;
  error: string | null;
  updatedAtMs: number | null;
  summary: ClusterSummary;
  thresholds: { tempWarningC: number; tempCriticalC: number; memWarningPct: number; memCriticalPct: number };
  nodes: NodeView[];
}

export interface HistoryPoint {
  t: number;
  memUsedBytes: number;
  memTotalBytes: number;
  rssBytes: number;
  cpuPercent: number;
  cpuTempC: number | null;
  busyThreads: number;
  computeThreads: number;
  processThreads: number;
  queueDepth: number;
  blocksPerSec: number;
  netRxBytesPerSec: number;
  netTxBytesPerSec: number;
}

export interface HistoryView {
  key: string;
  hostname: string;
  points: HistoryPoint[];
}
