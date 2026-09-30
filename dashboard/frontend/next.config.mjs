// /api/* is proxied to the FastAPI backend by app/api/[...path]/route.ts (configured at runtime
// with DASHBOARD_API_URL), so there is nothing environment-specific here.

/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // Self-contained server (`node .next/standalone/server.js`) for deployment on the Pis.
  output: "standalone",
};

export default nextConfig;
