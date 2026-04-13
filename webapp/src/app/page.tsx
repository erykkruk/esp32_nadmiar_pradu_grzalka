"use client";

import { useState, useEffect, useCallback } from "react";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ResponsiveContainer,
  ReferenceLine,
  Legend,
} from "recharts";

const APP_VERSION = "2.5.1";

interface DemoState {
  running: boolean;
  simTime: number;
  solarBase: number;
  houseBase: number;
  noiseAmp: number;
  spikeAmp: number;
}

interface Status {
  mode: "auto" | "manual";
  p_applied: number;
  p_max: number;
  duty_pct: number;
  manual_duty: number;
  export_w: number;
  grid_import_w: number;
  heater_w: number;
  gross_export_w: number;
  esp_online: boolean;
  esp_safe_mode: boolean;
  esp_rssi: number;
  esp_uptime_s: number;
  last_report_at: number | null;
}

interface HistoryReading {
  ts: number;
  grid_import_w: number;
  export_w: number;
  heater_w: number;
  avg_export_w: number;
  p_target: number;
  p_applied: number;
}

interface DbStats {
  totalRows: number;
  oldestTs: number | null;
  newestTs: number | null;
  fileSizeMb: number;
}

const RANGES = ["10m", "1h", "6h", "24h", "7d", "30d"] as const;
type Range = (typeof RANGES)[number];

function formatTime(ts: number, range: Range): string {
  const d = new Date(ts);
  if (range === "10m" || range === "1h") {
    return d.toLocaleTimeString("pl-PL", {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
    });
  }
  if (range === "6h" || range === "24h") {
    return d.toLocaleTimeString("pl-PL", {
      hour: "2-digit",
      minute: "2-digit",
    });
  }
  return d.toLocaleDateString("pl-PL", {
    day: "2-digit",
    month: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  });
}

function formatW(v: number): string {
  return `${Math.round(v)} W`;
}

function timeSince(ts: number): string {
  const diff = Date.now() - ts;
  if (diff < 60_000) return `${Math.round(diff / 1000)}s temu`;
  if (diff < 3600_000) return `${Math.round(diff / 60_000)}min temu`;
  if (diff < 86400_000) return `${Math.round(diff / 3600_000)}h temu`;
  return `${Math.round(diff / 86400_000)}d temu`;
}

function formatUptime(s: number): string {
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m`;
  if (s < 86400)
    return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
  return `${Math.floor(s / 86400)}d ${Math.floor((s % 86400) / 3600)}h`;
}

// ==================== LOGIN SCREEN ====================

function LoginScreen({ onLogin }: { onLogin: () => void }) {
  const [password, setPassword] = useState("");
  const [error, setError] = useState(false);
  const [loading, setLoading] = useState(false);

  const submit = async (e: React.FormEvent) => {
    e.preventDefault();
    setLoading(true);
    setError(false);

    const res = await fetch("/api/auth", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password }),
    });

    if (res.ok) {
      onLogin();
    } else {
      setError(true);
    }
    setLoading(false);
  };

  return (
    <div
      style={{
        display: "flex",
        justifyContent: "center",
        alignItems: "center",
        minHeight: "100vh",
      }}
    >
      <div className="card" style={{ width: 320, textAlign: "center" }}>
        <h2 style={{ marginBottom: 16 }}>ESP32 Heater Control</h2>
        <form onSubmit={submit}>
          <input
            type="password"
            placeholder="Haslo"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoFocus
            style={{
              width: "100%",
              padding: "10px 12px",
              borderRadius: 8,
              border: `1px solid ${error ? "var(--red)" : "var(--border)"}`,
              background: "var(--bg)",
              color: "var(--text)",
              fontSize: "1rem",
              marginBottom: 12,
            }}
          />
          {error && (
            <p style={{ color: "var(--red)", fontSize: "0.85rem", marginBottom: 8 }}>
              Nieprawidlowe haslo
            </p>
          )}
          <button
            type="submit"
            disabled={loading}
            style={{
              width: "100%",
              padding: "10px",
              borderRadius: 8,
              border: "none",
              background: "var(--blue)",
              color: "white",
              fontSize: "1rem",
              cursor: "pointer",
            }}
          >
            {loading ? "..." : "Zaloguj"}
          </button>
        </form>
        <p
          style={{
            marginTop: 16,
            fontSize: "0.7rem",
            color: "var(--muted)",
          }}
        >
          v{APP_VERSION}
        </p>
      </div>
    </div>
  );
}

// ==================== DASHBOARD ====================

export default function Dashboard() {
  const [authed, setAuthed] = useState<boolean | null>(null);
  const [status, setStatus] = useState<Status | null>(null);
  const [history, setHistory] = useState<HistoryReading[]>([]);
  const [stats, setStats] = useState<DbStats | null>(null);
  const [range, setRange] = useState<Range>("1h");
  const [sliderDuty, setSliderDuty] = useState(0);
  const [sliderActive, setSliderActive] = useState(false);
  const [demo, setDemo] = useState<DemoState | null>(null);
  const [demoSolar, setDemoSolar] = useState(1800);
  const [demoHouse, setDemoHouse] = useState(600);
  const [demoNoise, setDemoNoise] = useState(0);
  const [demoSpike, setDemoSpike] = useState(50);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [pMaxInput, setPMaxInput] = useState("2000");
  const [pMaxSaving, setPMaxSaving] = useState(false);
  const [showGrossExport, setShowGrossExport] = useState(false);

  // Check if already authenticated
  useEffect(() => {
    fetch("/api/status")
      .then((res) => setAuthed(res.ok))
      .catch(() => setAuthed(false));
  }, []);

  const fetchStatus = useCallback(async () => {
    try {
      const res = await fetch("/api/status");
      if (res.status === 401) {
        setAuthed(false);
        return;
      }
      if (res.ok) {
        const data = await res.json();
        setStatus(data);
        if (!sliderActive) {
          setSliderDuty(data.manual_duty);
        }
        if (!settingsOpen) {
          setPMaxInput(String(data.p_max));
        }
      }
    } catch {
      /* ignore */
    }
  }, [sliderActive, settingsOpen]);

  const fetchHistory = useCallback(async () => {
    try {
      const res = await fetch(`/api/history?range=${range}`);
      if (res.ok) {
        const json = await res.json();
        setHistory(json.data);
      }
    } catch {
      /* ignore */
    }
  }, [range]);

  const fetchStats = useCallback(async () => {
    try {
      const res = await fetch("/api/stats");
      if (res.ok) setStats(await res.json());
    } catch {
      /* ignore */
    }
  }, []);

  const setMode = async (mode: "auto" | "manual") => {
    await fetch("/api/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode }),
    });
    fetchStatus();
  };

  const setDuty = async (duty: number) => {
    await fetch("/api/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ duty }),
    });
    fetchStatus();
  };

  const fetchDemo = useCallback(async () => {
    try {
      const res = await fetch("/api/demo");
      if (res.ok) setDemo(await res.json());
    } catch {
      /* ignore */
    }
  }, []);

  const startDemo = async () => {
    await fetch("/api/demo", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        solar: demoSolar,
        house: demoHouse,
        noise: demoNoise,
        spike: demoSpike,
      }),
    });
    setRange("10m");
    fetchDemo();
  };

  const stopDemo = async () => {
    await fetch("/api/demo", { method: "DELETE" });
    fetchDemo();
  };

  const updateDemoParams = async (params: {
    solar?: number;
    house?: number;
    noise?: number;
    spike?: number;
  }) => {
    await fetch("/api/demo", {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(params),
    });
  };

  const savePMax = async () => {
    const value = Number(pMaxInput);
    if (isNaN(value) || value < 100 || value > 10000) return;
    setPMaxSaving(true);
    await fetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ p_max: value }),
    });
    setPMaxSaving(false);
    fetchStatus();
  };

  const demoRunning = demo?.running ?? false;

  useEffect(() => {
    if (!authed) return;
    fetchStatus();
    fetchHistory();
    fetchStats();
    fetchDemo();

    const historyMs =
      range === "10m" ? 1000 : range === "1h" ? 5000 : 15000;
    const statusInterval = setInterval(fetchStatus, 200);
    const historyInterval = setInterval(fetchHistory, historyMs);
    const statsInterval = setInterval(fetchStats, 60000);
    const demoInterval = setInterval(fetchDemo, 3000);

    return () => {
      clearInterval(statusInterval);
      clearInterval(historyInterval);
      clearInterval(statsInterval);
      clearInterval(demoInterval);
    };
  }, [authed, range, demoRunning, fetchStatus, fetchHistory, fetchStats, fetchDemo]);

  // Loading
  if (authed === null) return null;

  // Login screen
  if (!authed) {
    return (
      <LoginScreen
        onLogin={() => {
          setAuthed(true);
        }}
      />
    );
  }

  const chartData = history.map((r) => ({
    ...r,
    time: formatTime(r.ts, range),
    gross_export_w: r.avg_export_w,
  }));

  const isAuto = status?.mode === "auto";

  return (
    <div className="container">
      <h1>Sterowanie grzalka - ESP32</h1>

      {/* Status bar */}
      <div className="status-bar">
        <span
          className={`badge ${status?.esp_online ? "badge-green" : "badge-red"}`}
        >
          {status?.esp_online ? "ESP32 online" : "ESP32 offline"}
        </span>
        {status?.esp_safe_mode && (
          <span className="badge badge-red">SAFE MODE</span>
        )}
        {status && (
          <span className={`badge ${isAuto ? "badge-blue" : "badge-amber"}`}>
            {isAuto ? "AUTO" : "MANUAL"}
          </span>
        )}
        {status?.last_report_at && (
          <span className="badge badge-green">
            {timeSince(status.last_report_at)}
          </span>
        )}
        {status?.esp_rssi !== 0 && (
          <span className="badge badge-blue">
            WiFi: {status?.esp_rssi} dBm
          </span>
        )}
        {status && status.esp_uptime_s > 0 && (
          <span className="badge badge-blue">
            Uptime: {formatUptime(status.esp_uptime_s)}
          </span>
        )}
        {stats && (
          <span className="badge badge-blue">
            DB: {stats.fileSizeMb} MB /{" "}
            {stats.totalRows.toLocaleString("pl-PL")} rek.
          </span>
        )}
      </div>

      {/* Mode control */}
      <div className="card" style={{ marginBottom: 16 }}>
        <div
          style={{
            display: "flex",
            gap: 16,
            alignItems: "center",
            flexWrap: "wrap",
          }}
        >
          <label style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <input
              type="checkbox"
              checked={isAuto}
              onChange={() => setMode(isAuto ? "manual" : "auto")}
            />
            <b>Automatycznie</b>
          </label>

          {!isAuto && (
            <>
              <input
                type="range"
                min={0}
                max={100}
                step={1}
                value={sliderDuty}
                onChange={(e) => {
                  setSliderActive(true);
                  setSliderDuty(Number(e.target.value));
                }}
                onMouseUp={() => {
                  setSliderActive(false);
                  setDuty(sliderDuty);
                }}
                onTouchEnd={() => {
                  setSliderActive(false);
                  setDuty(sliderDuty);
                }}
                style={{ width: 200 }}
              />
              <b style={{ fontVariantNumeric: "tabular-nums" }}>
                {sliderDuty}%
              </b>
            </>
          )}

          {isAuto && status && (
            <span style={{ color: "var(--muted)", fontSize: "0.9rem" }}>
              Brutto eksport: {formatW(status.gross_export_w)} | Duty:{" "}
              {status.duty_pct}%
            </span>
          )}
        </div>
      </div>

      {/* Settings */}
      <div className="card" style={{ marginBottom: 16 }}>
        <div
          style={{
            display: "flex",
            alignItems: "center",
            justifyContent: "space-between",
            cursor: "pointer",
          }}
          onClick={() => setSettingsOpen(!settingsOpen)}
        >
          <b>Ustawienia</b>
          <span
            style={{
              color: "var(--muted)",
              fontSize: "0.85rem",
              userSelect: "none",
            }}
          >
            {settingsOpen ? "▲" : "▼"}
          </span>
        </div>
        {settingsOpen && (
          <div style={{ marginTop: 12 }}>
            <div
              style={{
                display: "flex",
                gap: 12,
                alignItems: "center",
                flexWrap: "wrap",
              }}
            >
              <label
                style={{
                  fontSize: "0.85rem",
                  color: "var(--muted)",
                  whiteSpace: "nowrap",
                }}
              >
                Moc grzalki (W):
              </label>
              <input
                type="number"
                min={100}
                max={10000}
                step={100}
                value={pMaxInput}
                onChange={(e) => setPMaxInput(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") savePMax();
                }}
                style={{
                  width: 120,
                  padding: "6px 10px",
                  borderRadius: 8,
                  border: "1px solid var(--border)",
                  background: "var(--bg)",
                  color: "var(--text)",
                  fontSize: "0.95rem",
                  fontVariantNumeric: "tabular-nums",
                }}
              />
              <button
                onClick={savePMax}
                disabled={pMaxSaving}
                style={{
                  padding: "6px 16px",
                  borderRadius: 8,
                  border: "none",
                  background: "var(--blue)",
                  color: "white",
                  fontSize: "0.85rem",
                  cursor: "pointer",
                  fontWeight: 600,
                }}
              >
                {pMaxSaving ? "..." : "Zapisz"}
              </button>
              {status && (
                <span
                  style={{
                    fontSize: "0.8rem",
                    color: "var(--muted)",
                  }}
                >
                  Aktualna: {status.p_max} W
                </span>
              )}
            </div>
          </div>
        )}
      </div>

      {/* Value cards */}
      <div className="grid-4">
        <div className="card">
          <div className="card-label">Pobor z sieci</div>
          <div className="card-value" style={{ color: "var(--red)" }}>
            {status ? formatW(status.grid_import_w) : "-- W"}
          </div>
        </div>
        <div className="card">
          <div className="card-label">Eksport (zmierzony)</div>
          <div className="card-value" style={{ color: "var(--green)" }}>
            {status ? formatW(status.export_w) : "-- W"}
          </div>
        </div>
        <div className="card">
          <div className="card-label">Moc grzalki</div>
          <div className="card-value" style={{ color: "var(--amber)" }}>
            {status ? formatW(status.heater_w) : "-- W"}
          </div>
        </div>
        <div className="card">
          <div className="card-label">Brutto eksport</div>
          <div className="card-value" style={{ color: "var(--blue)" }}>
            {status ? formatW(status.gross_export_w) : "-- W"}
          </div>
        </div>
      </div>

      {/* Demo panel - only visible when demo is running */}
      {demo?.running && (
        <div className="card" style={{ marginBottom: 16, borderColor: "var(--amber)" }}>
          <div
            style={{
              display: "flex",
              gap: 16,
              alignItems: "center",
              flexWrap: "wrap",
              marginBottom: 12,
            }}
          >
            <span className="badge badge-amber">DEMO</span>
            <span
              style={{
                fontVariantNumeric: "tabular-nums",
                fontSize: "0.85rem",
                color: "var(--muted)",
              }}
            >
              Czas symulacji: {demo.simTime}s
            </span>
            <button
              onClick={stopDemo}
              style={{
                padding: "6px 16px",
                borderRadius: 8,
                border: "none",
                background: "var(--red)",
                color: "white",
                cursor: "pointer",
                fontWeight: 600,
                marginLeft: "auto",
              }}
            >
              STOP DEMO
            </button>
          </div>
          <div
            style={{
              display: "grid",
              gridTemplateColumns: "repeat(auto-fit, minmax(180px, 1fr))",
              gap: 12,
            }}
          >
            <label style={{ fontSize: "0.8rem", color: "var(--muted)" }}>
              Produkcja solarna: {demoSolar} W
              <input
                type="range"
                min={500}
                max={3000}
                step={50}
                value={demoSolar}
                onChange={(e) => {
                  setDemoSolar(Number(e.target.value));
                  updateDemoParams({ solar: Number(e.target.value) });
                }}
                style={{ width: "100%", marginTop: 4 }}
              />
            </label>
            <label style={{ fontSize: "0.8rem", color: "var(--muted)" }}>
              Zuzycie domu: {demoHouse} W
              <input
                type="range"
                min={200}
                max={1500}
                step={50}
                value={demoHouse}
                onChange={(e) => {
                  setDemoHouse(Number(e.target.value));
                  updateDemoParams({ house: Number(e.target.value) });
                }}
                style={{ width: "100%", marginTop: 4 }}
              />
            </label>
            <label style={{ fontSize: "0.8rem", color: "var(--muted)" }}>
              Szum odczytu: +/-{demoNoise} W
              <input
                type="range"
                min={0}
                max={200}
                step={10}
                value={demoNoise}
                onChange={(e) => {
                  setDemoNoise(Number(e.target.value));
                  updateDemoParams({ noise: Number(e.target.value) });
                }}
                style={{ width: "100%", marginTop: 4 }}
              />
            </label>
            <label style={{ fontSize: "0.8rem", color: "var(--muted)" }}>
              Skoki zuzycia: +/-{demoSpike} W
              <input
                type="range"
                min={0}
                max={500}
                step={50}
                value={demoSpike}
                onChange={(e) => {
                  setDemoSpike(Number(e.target.value));
                  updateDemoParams({ spike: Number(e.target.value) });
                }}
                style={{ width: "100%", marginTop: 4 }}
              />
            </label>
          </div>
        </div>
      )}

      {/* Chart 1: Heater + export + gross */}
      <div className="chart-card">
        <div className="chart-header">
          <span className="chart-title">Moc grzalki i eksport</span>
          <div style={{ display: "flex", gap: 8, alignItems: "center", flexWrap: "wrap" }}>
            <label
              style={{
                display: "flex",
                alignItems: "center",
                gap: 4,
                fontSize: "0.75rem",
                color: "var(--muted)",
                cursor: "pointer",
              }}
            >
              <input
                type="checkbox"
                checked={showGrossExport}
                onChange={(e) => setShowGrossExport(e.target.checked)}
              />
              Brutto eksport
            </label>
            <div className="range-buttons">
              {RANGES.map((r) => (
                <button
                  key={r}
                  className={`range-btn ${range === r ? "active" : ""}`}
                  onClick={() => setRange(r)}
                >
                  {r}
                </button>
              ))}
            </div>
          </div>
        </div>
        <ResponsiveContainer width="100%" height={300}>
          <LineChart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#262626" />
            <XAxis
              dataKey="time"
              stroke="#a1a1aa"
              fontSize={11}
              tickCount={8}
            />
            <YAxis stroke="#a1a1aa" fontSize={11} unit=" W" />
            <Tooltip
              contentStyle={{
                background: "#1a1a1a",
                border: "1px solid #333",
                borderRadius: 8,
                fontSize: 13,
              }}
              formatter={(value: number, name: string) => {
                const labels: Record<string, string> = {
                  heater_w: "Grzalka",
                  export_w: "Eksport",
                  gross_export_w: "Brutto eksport",
                };
                return [formatW(value), labels[name] || name];
              }}
            />
            <Legend
              formatter={(value: string) => {
                const labels: Record<string, string> = {
                  heater_w: "Grzalka",
                  export_w: "Eksport",
                  gross_export_w: "Brutto eksport",
                };
                return labels[value] || value;
              }}
            />
            <ReferenceLine
              y={100}
              stroke="#f59e0b"
              strokeDasharray="5 5"
              label={{
                value: "Rezerwa 100W",
                fill: "#f59e0b",
                fontSize: 11,
              }}
            />
            <Line
              type="monotone"
              dataKey="heater_w"
              stroke="#f59e0b"
              strokeWidth={2}
              dot={false}
              name="heater_w"
            />
            <Line
              type="monotone"
              dataKey="export_w"
              stroke="#22c55e"
              strokeWidth={2}
              dot={false}
              name="export_w"
            />
            {showGrossExport && (
              <Line
                type="monotone"
                dataKey="gross_export_w"
                stroke="#3b82f6"
                strokeWidth={1}
                strokeDasharray="5 5"
                dot={false}
                name="gross_export_w"
              />
            )}
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Chart 2: Grid import */}
      <div className="chart-card">
        <div className="chart-header">
          <span className="chart-title">Pobor z sieci</span>
        </div>
        <ResponsiveContainer width="100%" height={200}>
          <LineChart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#262626" />
            <XAxis
              dataKey="time"
              stroke="#a1a1aa"
              fontSize={11}
              tickCount={8}
            />
            <YAxis stroke="#a1a1aa" fontSize={11} unit=" W" />
            <Tooltip
              contentStyle={{
                background: "#1a1a1a",
                border: "1px solid #333",
                borderRadius: 8,
                fontSize: 13,
              }}
              formatter={(value: number) => [formatW(value), "Pobor z sieci"]}
            />
            <ReferenceLine y={0} stroke="#555" />
            <Line
              type="monotone"
              dataKey="grid_import_w"
              stroke="#ef4444"
              strokeWidth={2}
              dot={false}
              name="Pobor"
            />
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Footer */}
      <div className="footer">
        <span>
          {stats?.oldestTs
            ? `Dane od: ${new Date(stats.oldestTs).toLocaleDateString("pl-PL")}`
            : "Brak danych"}
        </span>
        {!demo?.running && (
          <button
            onClick={startDemo}
            style={{
              padding: "4px 12px",
              borderRadius: 6,
              border: "1px solid var(--border)",
              background: "transparent",
              color: "var(--muted)",
              cursor: "pointer",
              fontSize: "0.75rem",
            }}
          >
            Start Demo
          </button>
        )}
        <span>v{APP_VERSION}</span>
      </div>
    </div>
  );
}
