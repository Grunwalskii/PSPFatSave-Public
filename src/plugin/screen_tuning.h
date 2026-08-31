#ifndef FS_SCREEN_TUNING_H
#define FS_SCREEN_TUNING_H

#include "pspfatsave.h"

// IPS screen tuning (screen_tuning.c): midtone gamma + colour temperature, both
// applied by ONE GE pass over each presented frame —
//   out = C · [ Cd + t·(Cd - Cd²) ],  t signed from gamma (see st_gamma_t100),
//   C from g_st_temp. g_st_gamma = gamma·100: 100 = 1.00 = off, 50..99 darken
//   (0.50..0.99, 50 = full Cd²), 101..200 brighten (1.01..2.00, 200 = full 2Cd-Cd²).
// Per-game: loaded/saved from SAVESTATE/<gid>/screen.cfg (st_load_game_gamma /
// st_save_game_gamma) — NOT settings.cfg and NOT gameset.cfg.

// Worker-thread stack (menu.c ram_usage_kb accounts for it, like the poll threads).
#define ST_WORKER_STACK_BYTES 4096

// Colour temperature, 0..200; 100 = neutral (C = white). <100 = warmer (cut
// blue), >100 = cooler (cut red). Applied in the same blend pass as gamma.
extern int g_st_temp;

// 1 = running under the POPS PS1 emulator (main.c runlevel gate). Declared
// ahead of st_active(), which reads it.
extern int g_is_pops;

// 1 = there is something to apply. Gamma and temperature are independent — the
// pass runs if EITHER is off its neutral value (gamma 1.0, temp 100), so
// temperature does not require a non-neutral gamma. Every gate in the module
// funnels through this.
static inline int st_active(void)
{
	return g_st_gamma != 100 || g_st_temp != 100;
}

void st_init(void);                 // module_start runtime init (kernel PRX .bss is not zeroed)
void st_ensure_started(void);       // resolve GE funcs + create worker/event flag (thread ctx only)
void st_prealloc(void);            // reserve the user-RAM GE buffers at boot (before the game exhausts its heap)
// Per-game gamma (screen.cfg in the RUNNING game's SAVESTATE folder): loaded once
// per game boot from the menu thread's boot auto-open block, saved when the HUD
// or the test screen edited the value.
void st_load_game_gamma(void);
void st_save_game_gamma(void);
// Present-hook entry (sysstats.c fps_display_set_frame_buf_patched). Applies the
// pass inline in thread context; defers to the worker from interrupt context.
void st_on_present(void *topaddr, int bufferwidth, int pixelformat);
// Menu test-screen entry (menu.c run_st_test): applies the pass in place to an
// already-drawn buffer, bypassing the menu gate + double-apply guards.
// Returns 1 if it ran, 0 if skipped (GE not idle — the game is frozen).
int st_apply_test(void *topaddr, int bufw, int pfmt);
void st_guard_reset(void);   // forget "already corrected" state (test-screen exit)
// Patch the savedata/message utility InitStart+ShutdownStart syscalls, so the
// pass can stand down while a dialog owns the screen and the GE. Installed
// unconditionally at boot, so the nesting count tracks dialogs from the start.
void st_install_dialog_hooks(void);
// Stop the pass and tear the worker down. Called from the game-exit hook in
// overclock.c. module_stop() is empty.
void st_stop(void);

// Injection pair, called from BOTH sceGeListEnQueue hooks. The "on" half runs
// BEFORE the real enqueue and only decides; the "after" half runs once the
// game's list is queued and does the enqueue, so our correction lands after
// the game's SCENE list and before its composite. Enqueue-only — never blocks.
void st_ge_on_submit(const void *list);
void st_ge_after_submit(void);

// Undo the POPS flip-hook patch. MUST run before this module unloads, so the
// patch must not outlive the module. Safe to call unconditionally; it no-ops
// when nothing is installed.
void st_pops_remove_flip(void);

// When the POPS double-buffer is redirecting the scanout, returns the front
// shadow buffer (where the HUD/overlay must draw to be visible). Otherwise
// returns topaddr unchanged — safe to call unconditionally.
void *st_pops_hud_fb(void *topaddr);

// 1 = an overlay (FPS/battery/CPU/FT chart) is on and needs the POPS scanout
// stabilized even when gamma/temp are neutral (POPS is single-buffered).
int st_pops_overlay_on(void);

// Bumped by the sceGeListEnQueue/EnQueueHead hooks (sysstats.c) on every GAME GE
// submit — the "something was drawn since last apply" signal for the double-apply
// guard. (Our own list bypasses the patched syscall, so it never bumps this.)
extern volatile u32 g_st_ge_seq;
extern volatile int g_st_worker_started;

#endif
