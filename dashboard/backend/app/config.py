"""Dashboard backend settings, read from environment variables."""

from __future__ import annotations

import os
from dataclasses import dataclass, field


def _float(name: str, default: float) -> float:
    value = os.environ.get(name)
    return float(value) if value else default


@dataclass(frozen=True)
class Settings:
    # Controller host API endpoint, same variable the C++ host runtime uses.
    controller: str = field(default_factory=lambda: os.environ.get("KUDALITE_CONTROLLER", "127.0.0.1:7070"))
    # How often the controller is polled, seconds.
    poll_seconds: float = field(default_factory=lambda: _float("DASHBOARD_POLL_SECONDS", 1.0))
    # How much history is kept in memory per node, seconds.
    history_seconds: float = field(default_factory=lambda: _float("DASHBOARD_HISTORY_SECONDS", 900.0))
    # A node whose newest sample is older than this is shown as stale, seconds.
    stale_seconds: float = field(default_factory=lambda: _float("DASHBOARD_STALE_SECONDS", 5.0))
    # Health thresholds. The Pi 5 firmware starts throttling the CPU around 80-85 C.
    temp_warning_c: float = field(default_factory=lambda: _float("DASHBOARD_TEMP_WARNING_C", 70.0))
    temp_critical_c: float = field(default_factory=lambda: _float("DASHBOARD_TEMP_CRITICAL_C", 80.0))
    mem_warning_pct: float = field(default_factory=lambda: _float("DASHBOARD_MEM_WARNING_PCT", 80.0))
    mem_critical_pct: float = field(default_factory=lambda: _float("DASHBOARD_MEM_CRITICAL_PCT", 90.0))
    # Browser origins allowed to call the API directly (the Next.js dev server proxies instead).
    cors_origins: list[str] = field(
        default_factory=lambda: [
            o.strip()
            for o in os.environ.get("DASHBOARD_CORS_ORIGINS", "http://localhost:3000,http://127.0.0.1:3000").split(",")
            if o.strip()
        ]
    )
