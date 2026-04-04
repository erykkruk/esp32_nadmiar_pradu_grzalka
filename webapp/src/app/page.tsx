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
} from "recharts";

interface Status {
  mode: "auto" | "manual";
  p_applied: number;
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
  avg_export_w: number; // gross export in DB
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
  if (s < 86400) return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
  return `${Math.floor(s / 86400)}d ${Math.floor((s % 86400) / 3600)}h`;
}

export default function Dashboard() {
  const [status, setStatus] = useState<Status | null>(null);
  const [history, setHistory] = useState<HistoryReading[]>([]);
  const [stats, setStats] = useState<DbStats | null>(null);
  const [range, setRange] = useState<Range>("1h");
  const [sliderDuty, setSliderDuty] = useState(0);
  const [sliderActive, setSliderActive] = useState(false);

  const fetchStatus = useCallback(async () => {
    try {
      const res = await fetch("/api/status");
      if (res.ok) {
        const data = await res.json();
        setStatus(data);
        if (!sliderActive) {
          setSliderDuty(data.manual_duty);
        }
      }
    } catch {
      /* ignore */
    }
  }, [sliderActive]);

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

  useEffect(() => {
    fetchStatus();
    fetchHistory();
    fetchStats();

    const statusInterval = setInterval(fetchStatus, 2000);
    const historyInterval = setInterval(fetchHistory, 15000);
    const statsInterval = setInterval(fetchStats, 60000);

    return () => {
      clearInterval(statusInterval);
      clearInterval(historyInterval);
      clearInterval(statsInterval);
    };
  }, [fetchStatus, fetchHistory, fetchStats]);

  const chartData = history.map((r) => ({
    ...r,
    time: formatTime(r.ts, range),
    gross_export_w: r.avg_export_w, // avg_export_w stores gross in DB
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

      {/* Main chart: heater + export + gross */}
      <div className="chart-card">
        <div className="chart-header">
          <span className="chart-title">Moc grzalki, eksport i brutto</span>
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
        <ResponsiveContainer width="100%" height={300}>
          <LineChart data={chartData}>
            <CartesianGrid strokeDasharray="3 3" stroke="#262626" />
            <XAxis
              dataKey="time"
              stroke="#a1a1aa"
              fontSize={11}
              tickCount={8}
            />
            <YAxis stroke="#a1a1aa" fontSize={11} />
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
            <Line
              type="monotone"
              dataKey="gross_export_w"
              stroke="#3b82f6"
              strokeWidth={1}
              strokeDasharray="5 5"
              dot={false}
              name="gross_export_w"
            />
          </LineChart>
        </ResponsiveContainer>
      </div>

      {/* Grid import chart */}
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
            <YAxis stroke="#a1a1aa" fontSize={11} />
            <Tooltip
              contentStyle={{
                background: "#1a1a1a",
                border: "1px solid #333",
                borderRadius: 8,
                fontSize: 13,
              }}
              formatter={(value: number) => [formatW(value), "Pobor"]}
            />
            <ReferenceLine y={0} stroke="#555" />
            <Line
              type="monotone"
              dataKey="grid_import_w"
              stroke="#ef4444"
              strokeWidth={2}
              dot={false}
            />
          </LineChart>
        </ResponsiveContainer>
      </div>

      <div className="footer">
        <span>
          {stats?.oldestTs
            ? `Dane od: ${new Date(stats.oldestTs).toLocaleDateString("pl-PL")}`
            : "Brak danych"}
        </span>
        <span>ESP32 Heater Control v2 (brutto eksport)</span>
      </div>
    </div>
  );
}
