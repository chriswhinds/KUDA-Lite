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

"use client";

import type { HistoryView, NodeView } from "@/lib/types";
import { gib, pct, rate } from "@/lib/format";
import { usePoll } from "@/lib/usePoll";
import { TimeSeriesChart } from "./TimeSeriesChart";

const RANGES = [
  { seconds: 300, label: "Last 5 min" },
  { seconds: 900, label: "Last 15 min" },
];

const GIB = 1024 ** 3;

export function NodeDetail({
  node,
  seconds,
  onSeconds,
  tempWarning,
  tempCritical,
}: {
  node: NodeView;
  seconds: number;
  onSeconds: (s: number) => void;
  tempWarning: number;
  tempCritical: number;
}) {
  const url = `/api/nodes/${encodeURIComponent(node.key)}/history?seconds=${seconds}`;
  const { data, dataUrl, error } = usePoll<HistoryView>(url, 2000);
  const pts = data?.points ?? [];
  const t = pts.map((p) => p.t);
  const refreshing = dataUrl !== url;
  const worker = node.role === "worker";
  const memTotal = pts.length ? pts[pts.length - 1].memTotalBytes : node.memTotalBytes;

  return (
    <section className="detail" aria-labelledby="detail-title">
      <div className="section-head">
        <h2 id="detail-title">
          {node.hostname} <span className="muted">· {worker ? `worker ${node.id}` : "controller"}</span>
        </h2>
        <div className="segmented" role="group" aria-label="Time range">
          {RANGES.map((r) => (
            <button key={r.seconds} type="button" aria-pressed={seconds === r.seconds} onClick={() => onSeconds(r.seconds)}>
              {seconds === r.seconds && <span aria-hidden="true">✓ </span>}
              {r.label}
            </button>
          ))}
        </div>
      </div>
      {error && <p className="notice">History unavailable: {error}</p>}
      <div className={`chart-grid${refreshing ? " refreshing" : ""}`}>
        <TimeSeriesChart
          title="CPU utilisation"
          subtitle="whole board, %"
          t={t}
          yMax={100}
          format={(v) => pct(v)}
          series={[{ key: "cpu", name: "CPU", color: "var(--series-1)", values: pts.map((p) => p.cpuPercent) }]}
        />
        <TimeSeriesChart
          title="Memory in use"
          subtitle={`system RAM, GiB of ${gib(memTotal)}`}
          t={t}
          yMax={memTotal / GIB}
          format={(v) => `${v.toFixed(1)} GiB`}
          series={[{ key: "mem", name: "Used", color: "var(--series-1)", values: pts.map((p) => p.memUsedBytes / GIB) }]}
        />
        {worker ? (
          <TimeSeriesChart
            title="Busy compute threads"
            subtitle={`of ${node.computeThreads} in the kernel pool`}
            t={t}
            yMax={Math.max(1, node.computeThreads)}
            integer
            format={(v) => v.toFixed(0)}
            series={[{ key: "busy", name: "Busy", color: "var(--series-1)", values: pts.map((p) => p.busyThreads) }]}
          />
        ) : (
          <TimeSeriesChart
            title="Daemon threads"
            subtitle="OS threads in cl-controller"
            t={t}
            integer
            format={(v) => v.toFixed(0)}
            series={[{ key: "proc", name: "Threads", color: "var(--series-1)", values: pts.map((p) => p.processThreads) }]}
          />
        )}
        <TimeSeriesChart
          title="SoC temperature"
          subtitle="°C"
          t={t}
          yMin={20}
          yMax={Math.max(90, ...pts.map((p) => p.cpuTempC ?? 0))}
          format={(v) => `${v.toFixed(0)} °C`}
          references={[
            { value: tempWarning, label: `Warm ${tempWarning} °C` },
            { value: tempCritical, label: `Throttle risk ${tempCritical} °C` },
          ]}
          series={[{ key: "temp", name: "SoC", color: "var(--series-1)", values: pts.map((p) => p.cpuTempC) }]}
        />
        <TimeSeriesChart
          title="KUDA-Lite network traffic"
          subtitle="all connections of the daemon"
          t={t}
          format={(v) => rate(v)}
          series={[
            { key: "rx", name: "In", color: "var(--series-1)", values: pts.map((p) => p.netRxBytesPerSec) },
            { key: "tx", name: "Out", color: "var(--series-2)", values: pts.map((p) => p.netTxBytesPerSec) },
          ]}
        />
        {worker && (
          <TimeSeriesChart
            title="Blocks executed"
            subtitle="per second"
            t={t}
            format={(v) => v.toFixed(0)}
            series={[{ key: "bps", name: "Blocks/s", color: "var(--series-1)", values: pts.map((p) => p.blocksPerSec) }]}
          />
        )}
      </div>
    </section>
  );
}
