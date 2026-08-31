#ifndef __CHEATS_H__
#define __CHEATS_H__

// ── Lightweight per-game FPS unlock (v1) ─────────────────────────────────────
// Streams the CWCheat text DB in OUR data folder, extracts just the running
// game's FPS-named cheats, and re-applies the selected one every vblank via a
// minimal CWCheat interpreter (see cheats.c for the opcode spec).
//
// The menu owns a single "FPS:" selector row: option 0 = "Stock" (apply nothing),
// options 1..g_fps_opt_count = the DB's FPS cheats for this game.

// Number of FPS options found for the running game (0 = none). With 0 the
// menu row shows an empty-state label instead of "Stock" — see g_fps_db_found.
// Selector length = g_fps_opt_count + 1 (Stock at index 0).
extern int g_fps_opt_count;
// 1 = a cheat DB file was opened this boot (cheats_load_for_game). With
// g_fps_opt_count == 0 it picks the row label: DB found -> "No FPS unlock in
// Cheat.db", no DB -> "No Cheat.db found".
extern int g_fps_db_found;
// Active selection: 0 = Stock, 1..g_fps_opt_count = FPS option (name index-1).
extern int g_fps_active;

// Boot: build the option list for `umdid` from the DB and load the saved
// selection. MS-safe context only (menu thread) — reads the Memory Stick.
void cheats_load_for_game(void);

// Display name for selector index (0 -> "Stock", 1..count -> the cheat name;
// with no options at all the empty-state labels above).
const char *cheats_fps_name(int idx);

// D-pad Left/Right on the FPS row: dir -1/+1, wraps over 0..g_fps_opt_count.
// Persists the new selection (per-game) and (re)starts the apply thread.
void cheats_fps_cycle(int dir);

// Lazily start the per-vblank apply thread (no-op if already running or Stock).
void cheats_ensure_started(void);

// Stop the apply thread and wait for it to exit. Call from the game-exit hook
// BEFORE game RAM is torn down (prevents the crash-on-exit).
void cheats_stop(void);

#endif
