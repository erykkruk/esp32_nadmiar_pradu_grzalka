import { processReport } from "./heater-logic";
import { getStore, resetStore } from "./store";
import { deleteSince } from "./db";

let demoInterval: ReturnType<typeof setInterval> | null = null;
let simTime = 0;
let demoStartedAt = 0;

// Simulation parameters
let solarBase = 1800;
let houseBase = 600;
let noiseAmp = 0;
let spikeAmp = 50;

function getSolar(t: number): number {
  return (
    solarBase +
    Math.sin(t * 0.02) * 150 + // slow cloud drift
    Math.sin(t * 0.1) * 60 + // medium variation
    Math.sin(t * 0.33) * 30 // fast flicker
  );
}

function getHouse(t: number): number {
  let h = houseBase + Math.sin(t * 0.05) * 50;
  if (Math.sin(t * 0.21) > 0.85) h += spikeAmp;
  if (Math.sin(t * 0.37) > 0.9) h += spikeAmp * 0.6;
  return h;
}

function getNoise(): number {
  return (Math.random() - 0.5) * 2 * noiseAmp;
}

function demoTick() {
  simTime++;

  const store = getStore();
  const solar = getSolar(simTime);
  const house = getHouse(simTime);
  const noise = getNoise();

  // Meter sees ALL loads including heater: house + heater - solar
  // positive = import, negative = export (same as DTSU666)
  const netPower = house + store.pApplied - solar + noise;

  processReport({
    power_w: netPower,
    uptime_s: simTime,
    wifi_rssi: -45,
    free_heap: 32000,
    current_duty_pct: 0,
    safe_mode: false,
    seconds_since_last_response: 0,
  });
}

export function startDemo(params?: {
  solar?: number;
  house?: number;
  noise?: number;
  spike?: number;
}): void {
  stopDemo();
  simTime = 0;

  if (params?.solar !== undefined) solarBase = params.solar;
  if (params?.house !== undefined) houseBase = params.house;
  if (params?.noise !== undefined) noiseAmp = params.noise;
  if (params?.spike !== undefined) spikeAmp = params.spike;

  demoStartedAt = Date.now();

  // Run at 5 ticks per second for smooth charts
  demoInterval = setInterval(demoTick, 200);
  // First tick immediately
  demoTick();
}

export function stopDemo(): void {
  if (demoInterval) {
    clearInterval(demoInterval);
    demoInterval = null;
  }

  // Clean up demo data from DB and memory
  if (demoStartedAt > 0) {
    deleteSince(demoStartedAt);
  }
  resetStore();

  simTime = 0;
  demoStartedAt = 0;
}

export function isDemoRunning(): boolean {
  return demoInterval !== null;
}

export function updateDemoParams(params: {
  solar?: number;
  house?: number;
  noise?: number;
  spike?: number;
}): void {
  if (params.solar !== undefined) solarBase = params.solar;
  if (params.house !== undefined) houseBase = params.house;
  if (params.noise !== undefined) noiseAmp = params.noise;
  if (params.spike !== undefined) spikeAmp = params.spike;
}

export function getDemoParams() {
  return {
    running: isDemoRunning(),
    simTime,
    solarBase,
    houseBase,
    noiseAmp,
    spikeAmp,
  };
}
