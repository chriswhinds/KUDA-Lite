"use client";

import { useEffect, useState } from "react";

export interface PollState<T> {
  data: T | null;
  /** URL the current `data` was fetched from (lags `url` briefly after it changes). */
  dataUrl: string | null;
  error: string | null;
  fetchedAt: number | null;
}

/**
 * Fetches `url` now and then every `intervalMs` after each response. Previous data is kept while
 * refetching or after an error, so charts hold their frame instead of flashing.
 */
export function usePoll<T>(url: string | null, intervalMs: number): PollState<T> {
  const [state, setState] = useState<PollState<T>>({ data: null, dataUrl: null, error: null, fetchedAt: null });

  useEffect(() => {
    if (!url) return;
    let stopped = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const abort = new AbortController();

    const tick = async () => {
      try {
        const res = await fetch(url, { cache: "no-store", signal: abort.signal });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const data = (await res.json()) as T;
        if (!stopped) setState({ data, dataUrl: url, error: null, fetchedAt: Date.now() });
      } catch (e) {
        if (stopped || (e instanceof DOMException && e.name === "AbortError")) return;
        setState((s) => ({ ...s, error: e instanceof Error ? e.message : String(e) }));
      } finally {
        if (!stopped) timer = setTimeout(tick, intervalMs);
      }
    };
    tick();
    return () => {
      stopped = true;
      clearTimeout(timer);
      abort.abort();
    };
  }, [url, intervalMs]);

  return state;
}
