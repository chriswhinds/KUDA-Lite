// Copyright 2026 Christopher Hinds, Stratum Labs llc
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
import { LevelBadge, Meter, NodeStatusBadge, Sparkline, ThreadCells } from "./Bits";

const TEMP_LABEL = { good: "Normal", warning: "Warm", critical: "Hot" } as const;

export function NodeCard({ node, selected, onSelect }: { node: NodeView; selected: boolean; onSelect: () => void }) {
  const live = node.status === "online" || node.status === "stale";
  const worker = node.role === "worker";
  return (
    <article className={`card node-card${selected ? " selected" : ""}${live ? "" : " dim"}`}>
      <div className="card-head">
        <div>
          <h3 className="node-name">
            {/* Stretched over the whole card by CSS, so the card is one click/keyboard target. */}
            <button type="button" className="card-link" aria-pressed={selected} onClick={onSelect}>
              {node.hostname}
            </button>
          </h3>
          <div className="node-sub">
            {worker ? `Worker ${node.id}` : "Controller"} · {node.address}
          </div>
          {node.board && <div className="node-sub">{node.board}</div>}
        </div>
        <NodeStatusBadge status={node.status} />
      </div>

      {node.timestampMs == null ? (
        <p className="muted">No telemetry received yet.</p>
      ) : (
        <dl className="metrics">
          <div className="metric metric-wide">
            <dt>CPU</dt>
            <dd className="metric-cpu">
              <span className="big">{pct(node.cpuPercent)}</span>
              <Sparkline values={node.spark.cpuPercent} label={`CPU utilisation trend for ${node.hostname}`} />
            </dd>
          </div>

          <div className="metric metric-wide">
            <dt>Memory</dt>
            <dd>
              <div className="metric-line">
                <span>
                  {gib(node.memUsedBytes)} / {gib(node.memTotalBytes)} GiB
                </span>
                <span className="muted">{pct(node.memUsedPct)}</span>
              </div>
              <Meter value={node.memUsedPct} level={node.health.memory} label={`System memory used on ${node.hostname}`} />
              <div className="muted small">
                daemon RSS {bytes(node.rssBytes)}
                {node.health.memory !== "good" && (
                  <>
                    {" "}
                    · <LevelBadge level={node.health.memory} />
                  </>
                )}
              </div>
            </dd>
          </div>

          {worker && (
            <div className="metric metric-wide">
              <dt>Global memory</dt>
              <dd>
                <div className="metric-line">
                  <span>
                    {gib(node.arenaUsedBytes, 2)} / {gib(node.arenaBytes)} GiB arena
                  </span>
                  <span className="muted">{pct(node.arenaUsedPct)}</span>
                </div>
                <Meter value={node.arenaUsedPct} label={`Arena allocated on ${node.hostname}`} />
              </dd>
            </div>
          )}

          <div className="metric">
            <dt>Threads</dt>
            <dd>
              {worker ? (
                <>
                  <ThreadCells busy={node.busyThreads} total={node.computeThreads} />
                  <div className="small">
                    {node.busyThreads} of {node.computeThreads} busy
                  </div>
                </>
              ) : (
                <div className="small">no compute pool</div>
              )}
              <div className="muted small">{node.processThreads} OS threads</div>
            </dd>
          </div>

          <div className="metric">
            <dt>SoC temp</dt>
            <dd>
              <span className="big">{temp(node.cpuTempC)}</span>
              {node.health.temperature && (
                <div>
                  <LevelBadge level={node.health.temperature} label={TEMP_LABEL[node.health.temperature]} />
                </div>
              )}
              {node.cpuFreqMhz != null && <div className="muted small">{node.cpuFreqMhz} MHz</div>}
            </dd>
          </div>

          <div className="metric metric-wide">
            <dt>{worker ? "Executor" : "Launches"}</dt>
            <dd className="small">
              {worker ? (
                node.currentKernel ? (
                  <>
                    Running <code>{node.currentKernel}</code>
                    {node.queueDepth > 0 && ` · ${node.queueDepth} queued`} · {node.blocksPerSec.toFixed(0)} blocks/s
                  </>
                ) : (
                  <span className="muted">Idle · {node.blocksExecuted.toLocaleString("en-US")} blocks run</span>
                )
              ) : node.executing > 0 ? (
                `${node.executing} launch${node.executing === 1 ? "" : "es"} in progress`
              ) : (
                <span className="muted">No launch in progress</span>
              )}
              <div className="muted">
                Network in {rate(node.netRxBytesPerSec)} · out {rate(node.netTxBytesPerSec)}
              </div>
            </dd>
          </div>
        </dl>
      )}
    </article>
  );
}
