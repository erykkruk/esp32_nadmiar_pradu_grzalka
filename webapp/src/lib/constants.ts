export const P_MAX = 2000.0; // Maximum heater power [W]
export const EXPORT_RESERVE_W = 100.0; // Minimum export reserve [W]
export const GROSS_BUFFER_SIZE = 10; // Circular buffer size for gross export averaging
export const TAU_UP = 8.0; // EMA time constant for power increase [s]
export const TAU_DOWN = 5.0; // EMA time constant for power decrease [s]
export const HISTORY_MAX_MEMORY = 3600; // In-memory history entries (~1h at 1s intervals)
