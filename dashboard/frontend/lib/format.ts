const GIB = 1024 ** 3;
const MIB = 1024 ** 2;

export function gib(bytes: number, digits = 1): string {
  return (bytes / GIB).toFixed(digits);
}

export function bytes(n: number): string {
  if (n >= GIB) return `${(n / GIB).toFixed(1)} GiB`;
  if (n >= MIB) return `${(n / MIB).toFixed(0)} MiB`;
  if (n >= 1024) return `${(n / 1024).toFixed(0)} KiB`;
  return `${n.toFixed(0)} B`;
}

export function rate(bytesPerSec: number): string {
  if (bytesPerSec >= 1e6) return `${(bytesPerSec / 1e6).toFixed(1)} MB/s`;
  if (bytesPerSec >= 1e3) return `${(bytesPerSec / 1e3).toFixed(0)} kB/s`;
  return `${bytesPerSec.toFixed(0)} B/s`;
}

export function pct(v: number, digits = 0): string {
  return `${v.toFixed(digits)}%`;
}

export function temp(c: number | null): string {
  return c == null ? "–" : `${c.toFixed(0)} °C`;
}

export function compact(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)}B`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(1)}M`;
  if (n >= 1e4) return `${(n / 1e3).toFixed(1)}K`;
  return n.toLocaleString("en-US");
}

export function duration(ms: number): string {
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

export function ago(ms: number): string {
  if (ms < 1500) return "just now";
  return `${duration(ms)} ago`;
}

export function clock(t: number, withSeconds = true): string {
  return new Date(t).toLocaleTimeString("en-GB", {
    hour: "2-digit",
    minute: "2-digit",
    ...(withSeconds ? { second: "2-digit" } : {}),
  });
}
