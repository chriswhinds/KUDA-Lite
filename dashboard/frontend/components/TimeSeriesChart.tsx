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

"use client";

import { useEffect, useMemo, useRef, useState, type KeyboardEvent, type PointerEvent } from "react";
import { clock } from "@/lib/format";

export interface Series {
  key: string;
  name: string;
  /** CSS color, normally a var(--series-N) token. */
  color: string;
  values: (number | null)[];
}

export interface Reference {
  value: number;
  label: string;
}

interface Props {
  title: string;
  subtitle?: string;
  t: number[];
  series: Series[];
  format: (v: number) => string;
  /** Lower bound of the y axis (default 0). */
  yMin?: number;
  /** Upper bound of the y axis; defaults to the data (and references) maximum. */
  yMax?: number;
  references?: Reference[];
  height?: number;
  /** Whole-number quantity (threads, counts): ticks never fall between integers. */
  integer?: boolean;
}

const M = { top: 12, right: 64, bottom: 24, left: 48 };

/** Rounds a span to 1, 2 or 5 × 10^n so axis ticks land on clean numbers. */
function niceStep(span: number, target: number): number {
  const raw = span / Math.max(1, target);
  const mag = 10 ** Math.floor(Math.log10(raw || 1));
  const norm = raw / mag;
  return (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
}

/**
 * Single-axis time-series chart: 2px lines, a 10% area wash for single-series charts, hairline
 * grid, end-of-line value labels, legend for two or more series, and a crosshair that snaps to
 * the nearest sample with a tooltip listing every series (pointer and arrow keys).
 */
export function TimeSeriesChart({ title, subtitle, t, series, format, yMin = 0, yMax, references = [], height = 180, integer = false }: Props) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(480);
  const [hover, setHover] = useState<number | null>(null);

  useEffect(() => {
    const el = wrapRef.current;
    if (!el) return;
    const ro = new ResizeObserver(([entry]) => setWidth(Math.max(240, entry.contentRect.width)));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const n = t.length;
  const innerW = width - M.left - M.right;
  const innerH = height - M.top - M.bottom;

  const geom = useMemo(() => {
    let hi = yMax ?? -Infinity;
    if (yMax === undefined) {
      for (const s of series) for (const v of s.values) if (v != null && v > hi) hi = v;
      for (const r of references) hi = Math.max(hi, r.value);
      if (!Number.isFinite(hi) || hi <= yMin) hi = yMin + 1;
    }
    const step = integer ? Math.max(1, niceStep(hi - yMin, 4)) : niceStep(hi - yMin, 4);
    const top = yMax ?? Math.ceil(hi / step) * step;
    const ticks: number[] = [];
    for (let v = yMin; v <= top + step * 1e-9; v += step) ticks.push(v);
    const t0 = n > 0 ? t[0] : 0;
    const t1 = n > 1 ? t[n - 1] : t0 + 1;
    const x = (ti: number) => M.left + ((ti - t0) / (t1 - t0 || 1)) * innerW;
    const y = (v: number) => M.top + innerH - ((Math.min(v, top) - yMin) / (top - yMin || 1)) * innerH;
    return { ticks, x, y, t0, t1 };
  }, [series, references, yMin, yMax, integer, t, n, innerW, innerH]);

  const paths = series.map((s) => {
    let d = "";
    let pen = false;
    s.values.forEach((v, i) => {
      if (v == null) {
        pen = false;
        return;
      }
      d += `${pen ? "L" : "M"}${geom.x(t[i]).toFixed(1)},${geom.y(v).toFixed(1)}`;
      pen = true;
    });
    return d;
  });

  const area =
    series.length === 1 && n > 1 && series[0].values.every((v) => v != null)
      ? `${paths[0]}L${geom.x(t[n - 1]).toFixed(1)},${geom.y(yMin)}L${geom.x(t[0]).toFixed(1)},${geom.y(yMin)}Z`
      : null;

  // End labels: only when there is room for all of them without overlapping (else legend + tooltip).
  const ends = series
    .map((s) => {
      let i = s.values.length - 1;
      while (i >= 0 && s.values[i] == null) i--;
      return i >= 0 ? { s, i, v: s.values[i] as number } : null;
    })
    .filter((e): e is { s: Series; i: number; v: number } => e !== null);
  const endYs = ends.map((e) => geom.y(e.v)).sort((a, b) => a - b);
  const labelEnds = series.length <= 4 && endYs.every((yy, k) => k === 0 || yy - endYs[k - 1] >= 14);

  const xTicks = n > 1 ? [0, 1, 2, 3].map((k) => geom.t0 + ((geom.t1 - geom.t0) * k) / 3) : [];

  const nearest = (clientX: number) => {
    const rect = wrapRef.current?.getBoundingClientRect();
    if (!rect || n === 0) return null;
    const px = clientX - rect.left;
    const ti = geom.t0 + ((px - M.left) / innerW) * (geom.t1 - geom.t0);
    let lo = 0;
    let hi = n - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (t[mid] < ti) lo = mid + 1;
      else hi = mid;
    }
    return lo > 0 && Math.abs(t[lo - 1] - ti) < Math.abs(t[lo] - ti) ? lo - 1 : lo;
  };

  const onMove = (e: PointerEvent<HTMLDivElement>) => setHover(nearest(e.clientX));
  const onKey = (e: KeyboardEvent<HTMLDivElement>) => {
    if (n === 0) return;
    if (e.key === "ArrowLeft") setHover((h) => Math.max(0, (h ?? n - 1) - 1));
    else if (e.key === "ArrowRight") setHover((h) => Math.min(n - 1, (h ?? n - 1) + 1));
    else if (e.key === "Escape") setHover(null);
    else return;
    e.preventDefault();
  };

  const hx = hover != null ? geom.x(t[hover]) : 0;
  const tooltipLeft = hover != null && hx > width - 200 ? hx - 12 : hx + 12;
  const tooltipShift = hover != null && hx > width - 200 ? "translateX(-100%)" : "none";
  const last = ends.length > 0 ? ends.map((e) => `${e.s.name} ${format(e.v)}`).join(", ") : "no data";

  return (
    <figure className="chart">
      <figcaption className="chart-head">
        <span className="chart-title">{title}</span>
        {subtitle && <span className="chart-subtitle">{subtitle}</span>}
      </figcaption>
      {series.length >= 2 && (
        <ul className="legend" aria-hidden="true">
          {series.map((s) => (
            <li key={s.key}>
              <span className="line-key" style={{ background: s.color }} />
              {s.name}
            </li>
          ))}
        </ul>
      )}
      <div
        ref={wrapRef}
        className="chart-plot"
        style={{ height }}
        tabIndex={0}
        role="img"
        aria-label={`${title}. Latest: ${last}. Use left and right arrow keys to inspect samples.`}
        onPointerMove={onMove}
        onPointerLeave={() => setHover(null)}
        onBlur={() => setHover(null)}
        onKeyDown={onKey}
      >
        <svg width={width} height={height} aria-hidden="true">
          {geom.ticks.map((v) => (
            <g key={v}>
              <line className="grid" x1={M.left} x2={M.left + innerW} y1={geom.y(v)} y2={geom.y(v)} />
              <text className="tick" x={M.left - 8} y={geom.y(v)} dy="0.32em" textAnchor="end">
                {format(v)}
              </text>
            </g>
          ))}
          {xTicks.map((ti, k) => (
            <text
              key={k}
              className="tick"
              x={geom.x(ti)}
              y={height - 6}
              textAnchor={k === 0 ? "start" : k === 3 ? "end" : "middle"}
            >
              {clock(ti)}
            </text>
          ))}
          {references.map((r) => (
            <g key={r.label}>
              <line className="reference" x1={M.left} x2={M.left + innerW} y1={geom.y(r.value)} y2={geom.y(r.value)} />
              <text className="tick" x={M.left + innerW - 4} y={geom.y(r.value) - 4} textAnchor="end">
                {r.label}
              </text>
            </g>
          ))}
          {area && <path d={area} style={{ fill: series[0].color, opacity: 0.1 }} />}
          {series.map((s, k) => (
            <path key={s.key} d={paths[k]} className="series-line" style={{ stroke: s.color }} />
          ))}
          {ends.map((e) => (
            <g key={e.s.key}>
              <circle className="end-dot" cx={geom.x(t[e.i])} cy={geom.y(e.v)} r={4} style={{ fill: e.s.color }} />
              {labelEnds && (
                <text className="end-label" x={geom.x(t[e.i]) + 8} y={geom.y(e.v)} dy="0.32em">
                  {format(e.v)}
                </text>
              )}
            </g>
          ))}
          {hover != null && (
            <g>
              <line className="crosshair" x1={hx} x2={hx} y1={M.top} y2={M.top + innerH} />
              {series.map((s) =>
                s.values[hover] != null ? (
                  <circle
                    key={s.key}
                    className="end-dot"
                    cx={hx}
                    cy={geom.y(s.values[hover] as number)}
                    r={4}
                    style={{ fill: s.color }}
                  />
                ) : null,
              )}
            </g>
          )}
        </svg>
        {hover != null && (
          <div className="tooltip" style={{ left: tooltipLeft, top: M.top, transform: tooltipShift }}>
            <div className="tooltip-time">{clock(t[hover])}</div>
            {series.map((s) => (
              <div key={s.key} className="tooltip-row">
                <span className="line-key" style={{ background: s.color }} />
                <strong>{s.values[hover] == null ? "–" : format(s.values[hover] as number)}</strong>
                <span className="tooltip-name">{s.name}</span>
              </div>
            ))}
          </div>
        )}
        {n === 0 && <div className="chart-empty">Waiting for samples…</div>}
      </div>
    </figure>
  );
}
