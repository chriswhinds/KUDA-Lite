"use client";

import { useEffect, useState } from "react";

type Theme = "system" | "light" | "dark";
const ORDER: Theme[] = ["system", "light", "dark"];
const LABEL: Record<Theme, string> = { system: "Theme: system", light: "Theme: light", dark: "Theme: dark" };

/** Cycles system → light → dark by stamping data-theme on <html>. */
export function ThemeToggle() {
  const [theme, setTheme] = useState<Theme>("system");

  useEffect(() => {
    try {
      const saved = localStorage.getItem("kudalite.theme") as Theme | null;
      if (saved && ORDER.includes(saved)) setTheme(saved);
    } catch {
      // storage unavailable: follow the system setting
    }
  }, []);

  useEffect(() => {
    const root = document.documentElement;
    if (theme === "system") delete root.dataset.theme;
    else root.dataset.theme = theme;
    try {
      localStorage.setItem("kudalite.theme", theme);
    } catch {
      // not remembered, still applied
    }
  }, [theme]);

  return (
    <button
      type="button"
      className="ghost-button"
      onClick={() => setTheme(ORDER[(ORDER.indexOf(theme) + 1) % ORDER.length])}
    >
      {LABEL[theme]}
    </button>
  );
}
