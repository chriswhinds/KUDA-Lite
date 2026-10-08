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
