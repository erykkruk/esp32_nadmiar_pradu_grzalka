import { GROSS_BUFFER_SIZE, HISTORY_MAX_MEMORY, P_MAX_DEFAULT } from "./constants";

export interface EspReport {
  power_w: number;
  uptime_s: number;
  wifi_rssi: number;
  free_heap: number;
  current_duty_pct: number;
  safe_mode: boolean;
  seconds_since_last_response: number;
}

export interface HistoryEntry {
  timestamp: number;
  power_w: number;
  export_w: number;
  gross_export_w: number;
  heater_w: number;
  duty_pct: number;
  grid_import_w: number;
}

export interface Store {
  // Gross export circular buffer
  grossBuffer: number[];
  bufferIndex: number;
  bufferFull: boolean;

  // AUTO logic state
  mode: "auto" | "manual";
  pApplied: number; // Power after EMA filter [W]
  lastRequestMs: number; // Timestamp of last ESP report

  // MANUAL mode
  manualDuty: number; // 0-100

  // Settings
  pMax: number; // Maximum heater power [W]

  // Last ESP report
  lastReport: {
    powerW: number;
    uptimeS: number;
    wifiRssi: number;
    freeHeap: number;
    currentDutyPct: number;
    safeMode: boolean;
    receivedAt: number;
  } | null;

  // In-memory history for fast dashboard access
  history: HistoryEntry[];
}

function createStore(): Store {
  return {
    grossBuffer: new Array(GROSS_BUFFER_SIZE).fill(0),
    bufferIndex: 0,
    bufferFull: false,
    mode: "auto",
    pApplied: 0,
    lastRequestMs: 0,
    manualDuty: 0,
    pMax: P_MAX_DEFAULT,
    lastReport: null,
    history: [],
  };
}

// Singleton: survives across API requests within the same Node.js process
const globalStore = globalThis as unknown as { __heaterStore?: Store };
if (!globalStore.__heaterStore) {
  globalStore.__heaterStore = createStore();
}

export function getStore(): Store {
  return globalStore.__heaterStore!;
}

export function addHistoryEntry(entry: HistoryEntry): void {
  const store = getStore();
  store.history.push(entry);
  if (store.history.length > HISTORY_MAX_MEMORY) {
    store.history.shift();
  }
}

export function resetStore(): void {
  const store = getStore();
  store.grossBuffer.fill(0);
  store.bufferIndex = 0;
  store.bufferFull = false;
  store.pApplied = 0;
  store.lastRequestMs = 0;
  store.lastReport = null;
  store.history = [];
}
