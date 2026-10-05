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

// Proxies /api/* to the FastAPI backend. The browser only talks to Next.js, so no CORS setup is
// needed and the backend can stay on a private address. DASHBOARD_API_URL is read per request,
// so one build works in every environment.

export const dynamic = "force-dynamic";

function backend(): string {
  return (process.env.DASHBOARD_API_URL ?? "http://127.0.0.1:8000").replace(/\/$/, "");
}

export async function GET(request: Request, { params }: { params: Promise<{ path: string[] }> }) {
  const { path } = await params;
  const search = new URL(request.url).search;
  const target = `${backend()}/api/${path.map(encodeURIComponent).join("/")}${search}`;
  try {
    const res = await fetch(target, { cache: "no-store", signal: AbortSignal.timeout(5000) });
    return new Response(res.body, {
      status: res.status,
      headers: {
        "content-type": res.headers.get("content-type") ?? "application/json",
        "cache-control": "no-store",
      },
    });
  } catch {
    return Response.json({ detail: `dashboard API unreachable at ${backend()}` }, { status: 502 });
  }
}
