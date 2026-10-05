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

import type { NodeView } from "@/lib/types";
import { bytes, gib, pct, rate, temp } from "@/lib/format";
import { LevelBadge, NodeStatusBadge } from "./Bits";

/** Every value on the dashboard, as a table (also the accessible view of the cards). */
export function NodeTable({ nodes, selected, onSelect }: { nodes: NodeView[]; selected: string | null; onSelect: (k: string) => void }) {
  return (
    <div className="table-wrap card">
      <table className="node-table">
        <thead>
          <tr>
            <th scope="col">Node</th>
            <th scope="col">Status</th>
            <th scope="col" className="num">CPU</th>
            <th scope="col" className="num">SoC temp</th>
            <th scope="col" className="num">Memory used</th>
            <th scope="col" className="num">Daemon RSS</th>
            <th scope="col" className="num">Arena allocated</th>
            <th scope="col" className="num">Compute threads</th>
            <th scope="col" className="num">OS threads</th>
            <th scope="col">Kernel</th>
            <th scope="col" className="num">Net in</th>
            <th scope="col" className="num">Net out</th>
          </tr>
        </thead>
        <tbody>
          {nodes.map((n) => (
            <tr key={n.key} className={n.key === selected ? "selected" : undefined}>
              <th scope="row">
                <button type="button" className="link" onClick={() => onSelect(n.key)}>
                  {n.hostname}
                </button>
                <div className="muted small">{n.role === "worker" ? `Worker ${n.id}` : "Controller"}</div>
              </th>
              <td>
                <NodeStatusBadge status={n.status} />
              </td>
              <td className="num">{pct(n.cpuPercent)}</td>
              <td className="num">
                {temp(n.cpuTempC)}
                {n.health.temperature && n.health.temperature !== "good" && (
                  <div>
                    <LevelBadge level={n.health.temperature} />
                  </div>
                )}
              </td>
              <td className="num">
                {gib(n.memUsedBytes)} / {gib(n.memTotalBytes)} GiB
                <div className="muted small">{pct(n.memUsedPct)}</div>
              </td>
              <td className="num">{bytes(n.rssBytes)}</td>
              <td className="num">{n.role === "worker" ? `${gib(n.arenaUsedBytes, 2)} / ${gib(n.arenaBytes)} GiB` : "–"}</td>
              <td className="num">{n.role === "worker" ? `${n.busyThreads} / ${n.computeThreads} busy` : "–"}</td>
              <td className="num">{n.processThreads}</td>
              <td>{n.currentKernel ? <code>{n.currentKernel}</code> : <span className="muted">idle</span>}</td>
              <td className="num">{rate(n.netRxBytesPerSec)}</td>
              <td className="num">{rate(n.netTxBytesPerSec)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
