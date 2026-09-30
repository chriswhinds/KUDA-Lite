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
