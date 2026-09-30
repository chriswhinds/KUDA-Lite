"use client";

import { useEffect, useState } from "react";
import type { ClusterView, Level } from "@/lib/types";
import { ago } from "@/lib/format";
import { usePoll } from "@/lib/usePoll";
import { NodeCard } from "./NodeCard";
import { NodeDetail } from "./NodeDetail";
import { NodeTable } from "./NodeTable";
import { SummaryTiles } from "./SummaryTiles";
import { ThemeToggle } from "./ThemeToggle";

type View = "cards" | "table";

function readPref<T extends string>(key: string, allowed: readonly T[], fallback: T): T {
  try {
    const v = localStorage.getItem(key);
    return v && (allowed as readonly string[]).includes(v) ? (v as T) : fallback;
  } catch {
    return fallback;
  }
}

function writePref(key: string, value: string) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // storage unavailable (private window); the preference just is not remembered
  }
}

export function Dashboard() {
  const { data, error, fetchedAt } = usePoll<ClusterView>("/api/cluster", 2000);
  const [view, setView] = useState<View>("cards");
  const [selected, setSelected] = useState<string | null>(null);
  const [seconds, setSeconds] = useState(300);
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => setView(readPref("kudalite.view", ["cards", "table"] as const, "cards")), []);
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  const nodes = data?.nodes ?? [];
  // Default selection: the first worker, else the controller.
  const selectedKey = selected ?? nodes.find((n) => n.role === "worker")?.key ?? nodes[0]?.key ?? null;
  const selectedNode = nodes.find((n) => n.key === selectedKey) ?? null;

  const backendDown = error != null && data == null;
  const controllerDown = data != null && !data.connected;
  const stale = backendDown || controllerDown || (error != null && data != null);
  const t = data?.thresholds;
  const maxTemp = data?.summary.maxTempC ?? null;
  const tempLevel: Level | null =
    maxTemp == null || !t ? null : maxTemp >= t.tempCriticalC ? "critical" : maxTemp >= t.tempWarningC ? "warning" : "good";

  const chooseView = (v: View) => {
    setView(v);
    writePref("kudalite.view", v);
  };

  return (
    <div className="page">
      <header className="topbar">
        <div>
          <h1>KUDA-Lite cluster</h1>
          <div className="muted small">{data ? `Controller ${data.controller}` : "Connecting to the dashboard API…"}</div>
        </div>
        <div className="topbar-right">
          <span className={`live ${stale ? "live-off" : "live-on"}`} role="status">
            <span className="live-dot" aria-hidden="true" />
            {backendDown
              ? "Dashboard API unreachable"
              : controllerDown
                ? "Controller unreachable"
                : fetchedAt
                  ? `Live · updated ${ago(now - fetchedAt)}`
                  : "Connecting…"}
          </span>
          <ThemeToggle />
        </div>
      </header>

      {(backendDown || controllerDown) && (
        <p className="notice" role="alert">
          {backendDown
            ? `Cannot reach the dashboard API (${error}). Is the FastAPI backend running?`
            : `The backend cannot reach the controller at ${data?.controller} (${data?.error ?? "unknown error"}). Showing the last data received.`}
        </p>
      )}

      {data && (
        <main className={stale ? "stale" : undefined}>
          <SummaryTiles s={data.summary} tempLevel={tempLevel} />

          <div className="section-head">
            <h2>
              Raspberry Pi nodes <span className="muted">· {nodes.length}</span>
            </h2>
            <div className="segmented" role="group" aria-label="Layout">
              {(["cards", "table"] as const).map((v) => (
                <button key={v} type="button" aria-pressed={view === v} onClick={() => chooseView(v)}>
                  {view === v && <span aria-hidden="true">✓ </span>}
                  {v === "cards" ? "Cards" : "Table"}
                </button>
              ))}
            </div>
          </div>

          {nodes.length === 0 ? (
            <p className="muted">No nodes have reported yet. Start cl-worker on each Pi.</p>
          ) : view === "cards" ? (
            <div className="node-grid">
              {nodes.map((n) => (
                <NodeCard key={n.key} node={n} selected={n.key === selectedKey} onSelect={() => setSelected(n.key)} />
              ))}
            </div>
          ) : (
            <NodeTable nodes={nodes} selected={selectedKey} onSelect={setSelected} />
          )}

          {selectedNode && t && (
            <NodeDetail
              node={selectedNode}
              seconds={seconds}
              onSeconds={setSeconds}
              tempWarning={t.tempWarningC}
              tempCritical={t.tempCriticalC}
            />
          )}
        </main>
      )}
    </div>
  );
}
