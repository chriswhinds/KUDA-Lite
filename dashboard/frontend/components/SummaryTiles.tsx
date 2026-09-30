import type { ClusterSummary, Level } from "@/lib/types";
import { compact, duration, gib, temp } from "@/lib/format";
import { LevelBadge } from "./Bits";

function Tile({ label, value, detail, extra }: { label: string; value: string; detail?: string; extra?: React.ReactNode }) {
  return (
    <div className="tile">
      <div className="tile-label">{label}</div>
      <div className="tile-value">{value}</div>
      <div className="tile-detail">
        {extra}
        {detail}
      </div>
    </div>
  );
}

export function SummaryTiles({ s, tempLevel }: { s: ClusterSummary; tempLevel: Level | null }) {
  const offline = s.nodesTotal - s.nodesOnline;
  const threadPct = s.computeThreads ? Math.round((100 * s.busyThreads) / s.computeThreads) : 0;
  return (
    <section className="tiles" aria-label="Cluster summary">
      <Tile
        label="Pis online"
        value={`${s.nodesOnline} / ${s.nodesTotal}`}
        detail={offline > 0 ? `${offline} offline` : `controller up ${duration(s.controllerUptimeMs)}`}
      />
      <Tile
        label="Compute threads busy"
        value={`${s.busyThreads} / ${s.computeThreads}`}
        detail={`${threadPct}% of ${s.workersOnline} worker${s.workersOnline === 1 ? "" : "s"}`}
      />
      <Tile
        label="System memory in use"
        value={`${gib(s.memUsedBytes)} / ${gib(s.memTotalBytes)} GiB`}
        detail="all Pis, including the OS"
      />
      <Tile
        label="Global memory allocated"
        value={`${gib(s.allocatedBytes, 2)} / ${gib(s.arenaTotalBytes)} GiB`}
        detail={`${s.allocations} allocation${s.allocations === 1 ? "" : "s"}`}
      />
      <Tile
        label="Kernel launches"
        value={`${s.activeLaunches} running`}
        detail={`${compact(s.launchesTotal)} total · ${s.sessions} host session${s.sessions === 1 ? "" : "s"}`}
      />
      <Tile
        label="Hottest SoC"
        value={temp(s.maxTempC)}
        extra={tempLevel ? <LevelBadge level={tempLevel} /> : null}
        detail={s.maxTempNode ? ` ${s.maxTempNode}` : " no sensor data"}
      />
    </section>
  );
}
