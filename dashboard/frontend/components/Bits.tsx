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

// Small presentational pieces shared by the cards, tiles and table.
import type { Level, NodeStatus } from "@/lib/types";

const LEVEL_TEXT: Record<Level, string> = { good: "Normal", warning: "Warning", critical: "Critical" };
const LEVEL_ICON: Record<Level, string> = { good: "✓", warning: "!", critical: "✕" };

/** Status is always icon + label, never colour alone. */
export function LevelBadge({ level, label }: { level: Level; label?: string }) {
  return (
    <span className={`badge badge-${level}`}>
      <span className="badge-icon" aria-hidden="true">
        {LEVEL_ICON[level]}
      </span>
      {label ?? LEVEL_TEXT[level]}
    </span>
  );
}

const NODE_STATUS: Record<NodeStatus, { level: Level | "muted"; icon: string; text: string }> = {
  online: { level: "good", icon: "●", text: "Online" },
  stale: { level: "warning", icon: "▲", text: "Stale" },
  waiting: { level: "muted", icon: "○", text: "Waiting" },
  offline: { level: "critical", icon: "✕", text: "Offline" },
};

export function NodeStatusBadge({ status }: { status: NodeStatus }) {
  const s = NODE_STATUS[status];
  return (
    <span className={`badge badge-${s.level}`}>
      <span className="badge-icon" aria-hidden="true">
        {s.icon}
      </span>
      {s.text}
    </span>
  );
}

/** Horizontal meter: the fill carries severity, the track is a lighter step of the same ramp. */
export function Meter({ value, level = "good", label }: { value: number; level?: Level; label: string }) {
  const clamped = Math.max(0, Math.min(100, value));
  return (
    <div
      className={`meter meter-${level}`}
      role="meter"
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(clamped)}
    >
      <div className="meter-fill" style={{ width: `${clamped}%` }} />
    </div>
  );
}

/** One square per compute thread; filled = currently running kernel work. */
export function ThreadCells({ busy, total }: { busy: number; total: number }) {
  if (total === 0) return null;
  return (
    <span className="threads" role="img" aria-label={`${busy} of ${total} compute threads busy`}>
      {Array.from({ length: total }, (_, i) => (
        <span key={i} className={i < busy ? "thread busy" : "thread"} />
      ))}
    </span>
  );
}

/** Trend line for a card; values are 0..100. Non-interactive: the number beside it is the value. */
export function Sparkline({ values, label }: { values: number[]; label: string }) {
  if (values.length < 2) return <svg className="spark" viewBox="0 0 100 28" aria-hidden="true" />;
  const d = values
    .map((v, i) => `${i === 0 ? "M" : "L"}${((i / (values.length - 1)) * 100).toFixed(2)},${(26 - (Math.min(100, v) / 100) * 24).toFixed(2)}`)
    .join("");
  return (
    <svg className="spark" viewBox="0 0 100 28" preserveAspectRatio="none" role="img" aria-label={label}>
      <path d={`${d}L100,28L0,28Z`} className="spark-area" />
      <path d={d} className="spark-line" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}
