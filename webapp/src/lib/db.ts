import Database from "better-sqlite3";
import path from "path";
import fs from "fs";

const DB_PATH = process.env.DB_PATH || "./data/readings.db";
const DB_MAX_SIZE_MB = parseInt(process.env.DB_MAX_SIZE_MB || "200", 10);

let _db: Database.Database | null = null;

export function getDb(): Database.Database {
  if (_db) return _db;

  const dbPath = path.resolve(DB_PATH);
  const dir = path.dirname(dbPath);
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir, { recursive: true });
  }

  _db = new Database(dbPath);
  _db.pragma("journal_mode = WAL");
  _db.pragma("synchronous = NORMAL");

  _db.exec(`
    CREATE TABLE IF NOT EXISTS readings (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      ts INTEGER NOT NULL,
      grid_import_w REAL NOT NULL DEFAULT 0,
      export_w REAL NOT NULL DEFAULT 0,
      heater_w REAL NOT NULL DEFAULT 0,
      avg_export_w REAL NOT NULL DEFAULT 0,
      auto_mode INTEGER NOT NULL DEFAULT 1,
      p_target REAL NOT NULL DEFAULT 0,
      p_applied REAL NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts);
  `);

  return _db;
}

export interface Reading {
  id?: number;
  ts: number;
  grid_import_w: number;
  export_w: number;
  heater_w: number;
  avg_export_w: number;
  auto_mode: boolean;
  p_target: number;
  p_applied: number;
}

const INSERT_SQL = `
  INSERT INTO readings (ts, grid_import_w, export_w, heater_w, avg_export_w, auto_mode, p_target, p_applied)
  VALUES (?, ?, ?, ?, ?, ?, ?, ?)
`;

export function insertReading(r: Reading): void {
  const db = getDb();
  db.prepare(INSERT_SQL).run(
    r.ts,
    r.grid_import_w,
    r.export_w,
    r.heater_w,
    r.avg_export_w,
    r.auto_mode ? 1 : 0,
    r.p_target,
    r.p_applied
  );
}

export function getLatestReading(): Reading | null {
  const db = getDb();
  const row = db
    .prepare("SELECT * FROM readings ORDER BY ts DESC LIMIT 1")
    .get() as Record<string, unknown> | undefined;
  if (!row) return null;
  return {
    ts: row.ts as number,
    grid_import_w: row.grid_import_w as number,
    export_w: row.export_w as number,
    heater_w: row.heater_w as number,
    avg_export_w: row.avg_export_w as number,
    auto_mode: row.auto_mode === 1,
    p_target: row.p_target as number,
    p_applied: row.p_applied as number,
  };
}

interface HistoryOptions {
  from?: number;
  to?: number;
  limit?: number;
  resolution?: number; // seconds between points (for downsampling)
}

export function getHistory(opts: HistoryOptions = {}): Reading[] {
  const db = getDb();
  const to = opts.to || Date.now();
  const from = opts.from || to - 3600_000; // default: last hour
  const limit = opts.limit || 10000;

  if (opts.resolution && opts.resolution > 1) {
    const bucketMs = opts.resolution * 1000;
    const rows = db
      .prepare(
        `SELECT
          (ts / ? * ?) as ts,
          AVG(grid_import_w) as grid_import_w,
          AVG(export_w) as export_w,
          AVG(heater_w) as heater_w,
          AVG(avg_export_w) as avg_export_w,
          ROUND(AVG(auto_mode)) as auto_mode,
          AVG(p_target) as p_target,
          AVG(p_applied) as p_applied
        FROM readings
        WHERE ts >= ? AND ts <= ?
        GROUP BY ts / ?
        ORDER BY ts ASC
        LIMIT ?`
      )
      .all(bucketMs, bucketMs, from, to, bucketMs, limit) as Record<string, unknown>[];

    return rows.map((r) => ({
      ts: r.ts as number,
      grid_import_w: r.grid_import_w as number,
      export_w: r.export_w as number,
      heater_w: r.heater_w as number,
      avg_export_w: r.avg_export_w as number,
      auto_mode: r.auto_mode === 1,
      p_target: r.p_target as number,
      p_applied: r.p_applied as number,
    }));
  }

  const rows = db
    .prepare(
      `SELECT * FROM readings WHERE ts >= ? AND ts <= ? ORDER BY ts ASC LIMIT ?`
    )
    .all(from, to, limit) as Record<string, unknown>[];

  return rows.map((r) => ({
    ts: r.ts as number,
    grid_import_w: r.grid_import_w as number,
    export_w: r.export_w as number,
    heater_w: r.heater_w as number,
    avg_export_w: r.avg_export_w as number,
    auto_mode: r.auto_mode === 1,
    p_target: r.p_target as number,
    p_applied: r.p_applied as number,
  }));
}

export function getDbStats(): {
  totalRows: number;
  oldestTs: number | null;
  newestTs: number | null;
  fileSizeMb: number;
} {
  const db = getDb();
  const count = db.prepare("SELECT COUNT(*) as c FROM readings").get() as {
    c: number;
  };
  const oldest = db
    .prepare("SELECT MIN(ts) as ts FROM readings")
    .get() as { ts: number | null };
  const newest = db
    .prepare("SELECT MAX(ts) as ts FROM readings")
    .get() as { ts: number | null };

  const dbPath = path.resolve(DB_PATH);
  let fileSizeMb = 0;
  if (fs.existsSync(dbPath)) {
    fileSizeMb =
      Math.round((fs.statSync(dbPath).size / (1024 * 1024)) * 100) / 100;
  }

  return {
    totalRows: count.c,
    oldestTs: oldest.ts,
    newestTs: newest.ts,
    fileSizeMb,
  };
}

export function pruneIfNeeded(): { deleted: number; vacuumed: boolean } {
  const stats = getDbStats();
  if (stats.fileSizeMb < DB_MAX_SIZE_MB) {
    return { deleted: 0, vacuumed: false };
  }

  const db = getDb();
  const deleteCount = Math.floor(stats.totalRows * 0.1);
  db.prepare(
    `DELETE FROM readings WHERE id IN (SELECT id FROM readings ORDER BY ts ASC LIMIT ?)`
  ).run(deleteCount);
  db.exec("VACUUM");

  return { deleted: deleteCount, vacuumed: true };
}
