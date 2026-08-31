#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// ── Intro Video Skip (PER-GAME, gameset.cfg) ───────────────────────────────
// Skips game intro videos by one of two paths:
//   scePsmfPlayer — the game-bundled "play this PMF" API.
//                   Skip = force GetCurrentStatus to report PLAYING_FINISHED.
//   sceMpeg       — the low-level decode API (firmware "sceMpeg_library").
//                   Skip = pulse the game's OWN skip button while a video is loaded.
#define PSMF_PLAYER_STATUS_PLAYING_FINISHED 0x200
#define PSMF_GET_CURRENT_STATUS_NID         0xF8EF08A6
#define MPEG_GET_AVC_AU_NID                 0xFE246728
#define MPEG_GET_ATRAC_AU_NID               0xE1CE83A7
#define MPEG_CREATE_NID                     0xD8C5F121
// jr ra / li v0, PLAYING_FINISHED — ARK's MAKE_DUMMY_FUNCTION_RETURN_0 shape
// (common/include/macros.h: JR_RA 0x03E00008, LI_V0(n) (0x2402<<16)|(n&0xFFFF)).
#define VSKIP_OP_JR_RA 0x03E00008u
#define VSKIP_OP_LI_V0 (0x24020000u | PSMF_PLAYER_STATUS_PLAYING_FINISHED)

// Patch sceMpegCreate's entry so it fails up front with SCE_MPEG_ERROR_OUT_OF_MEMORY
// (0x80610022). 3-word entry patch: lui v0,0x8061 / jr ra / ori v0,v0,0x22 (the ori is
// the delay slot).
#define MPEG_CREATE_OP0 0x3C028061u   // lui v0, 0x8061
#define MPEG_CREATE_OP1 0x03E00008u   // jr  ra
#define MPEG_CREATE_OP2 0x34420022u   // ori v0, v0, 0x22   (delay slot) -> v0 = 0x80610022

// ── THE LEARNED TIME WINDOW ────────────────────────────────────────────────
// The skip window is not detected automatically; it is LEARNED per game from the user.
//   CAPTURE run - armed at boot: the user holds D-pad Right (VSKIP_HOLD_BTN) for the
//                 whole intro. Firing happens strictly while Right is held; when the
//                 user RELEASES it, elapsed minus VSKIP_REACTION_MS (their reaction lag)
//                 is the learned window, saved per game, and the setting flips to TIMED.
//   TIMED runs  - fire everything from the anchor for the learned ms. No detector, no arm.
//
// The anchor is this thread's start (the game's first controller read), stable per game.
#define VSKIP_REACTION_MS 500                 // capture: trimmed off (hold btn released AFTER the intro ends)

// Buttons pulsed into the game's own controller reads while the window is open. Pulsed
// ALTERNATELY, one at a time, never together (see vskip_phase_mask).
#define VSKIP_INJECT_MASK  (PSP_CTRL_START | PSP_CTRL_CROSS)
// Press/release cadence is WALL-CLOCK, not call-count: X held for VSKIP_PULSE_US, released
// for the same, phase derived from sceKernelGetSystemTimeLow(), so every reader at the
// same instant sees the same held state.
#define VSKIP_PULSE_US 120000                  // 120ms held, then 120ms released
#define VSKIP_POLL_US      100000              // watcher poll period
// The button the USER holds to drive CAPTURE (the arm gate + the fire window). Deliberately
// NOT one of the injected buttons (VSKIP_INJECT_MASK) so the injector can pulse a clean
// X/START while the user holds Right.
// Outer safety bound: guarantees the watcher thread always exits.
#define VSKIP_CAP_US (120 * 1000 * 1000)

int g_video_skip = VSKIP_OFF;              // PER-GAME (gameset.cfg word 4): OFF / CAPTURE / TIMED
int g_video_skip_ms = 0;                   // PER-GAME (gameset.cfg word 5): the learned window, ms
volatile int g_vskip_inject = 0;           // watcher -> controller hooks: window open, pulse buttons
volatile int g_vskip_banner = 0;           // watcher -> display hook: draw the capture banner
volatile int g_vskip_window = 0;           // watcher -> probe: the fire window is open right now
volatile int g_vskip_active = 0;           // watcher lifetime (CAPTURE/TIMED) -> frame limiter: OFF the
                                            // WHOLE run. Not the momentary fire window (g_vskip_window):
                                            // the limiter alters frame timing, and CAPTURE measures the
                                            // intro's wall-clock length while TIMED replays it, so pacing
                                            // ANY part of the watcher run (including the gaps between fire
                                            // pulses) mis-calibrates the learned window. Off start-to-end.
static u32  g_psmf_status_fn = 0;          // scePsmfPlayerGetCurrentStatus (USER addr), 0 = not seen
static char g_psmf_modname[32] = {0};      // which of the five names this game shipped
static u32  g_vskip_saved[2] = {0, 0};     // the two instructions we overwrote
static int  g_vskip_patched = 0;
int  g_mpeg_load_seq = 0;           // ++ on every sceMpeg_library load (probe)
static u32  g_mpeg_create_fn = 0;          // sceMpegCreate (USER addr) of the CURRENT codec load
static u32  g_mpeg_create_save[3];         // the three instructions the create-fail patch overwrote
static int  g_mpeg_create_patched = 0;

// The CAPTURE banner is drawn by fps_poll_thread, so the watcher has to spin that thread
// up. Defined far below; forward-declared here.
void fps_poll_ensure_started(void);

// Kernel PRX .bss is NOT zeroed at load, so the globals above must be reset here
// (g_psmf_status_fn and g_vskip_inject are read before the watcher writes them). Called
// from module_start with the rest of the BSS init.
void video_skip_init(void)
{
	g_video_skip = VSKIP_OFF;
	g_video_skip_ms = 0;
	g_vskip_banner = 0;
	g_vskip_window = 0;
	g_vskip_inject = 0;
	g_vskip_active = 0;
	g_psmf_status_fn = 0;
	g_psmf_modname[0] = '\0';
	g_vskip_saved[0] = g_vskip_saved[1] = 0;
	g_vskip_patched = 0;
	g_mpeg_load_seq = 0;
	g_mpeg_create_fn = 0;
	g_mpeg_create_save[0] = g_mpeg_create_save[1] = g_mpeg_create_save[2] = 0;
	g_mpeg_create_patched = 0;
}

// Overwrite sceMpegCreate's entry so it fails with OUT_OF_MEMORY.
// g_mpeg_create_fn must already point at the current codec's export.
static void vskip_create_patch(void)
{
	u32 a = g_mpeg_create_fn;
	if (a == 0 || g_mpeg_create_patched) return;
	g_mpeg_create_save[0] = *(volatile u32 *)a;
	g_mpeg_create_save[1] = *(volatile u32 *)(a + 4);
	g_mpeg_create_save[2] = *(volatile u32 *)(a + 8);
	*(volatile u32 *)a       = MPEG_CREATE_OP0;
	*(volatile u32 *)(a + 4) = MPEG_CREATE_OP1;
	*(volatile u32 *)(a + 8) = MPEG_CREATE_OP2;
	ClearCaches();
	g_mpeg_create_patched = 1;
}

// Restore sceMpegCreate: only write back if OUR three words are still present AND the
// address still belongs to sceMpeg_library.
static void vskip_create_restore(void)
{
	u32 a = g_mpeg_create_fn;
	if (a == 0 || !g_mpeg_create_patched) return;
	g_mpeg_create_patched = 0;
	if (*(volatile u32 *)a == MPEG_CREATE_OP0 &&
	    *(volatile u32 *)(a + 4) == MPEG_CREATE_OP1 &&
	    *(volatile u32 *)(a + 8) == MPEG_CREATE_OP2) {
		SceModule2 *m = (SceModule2 *)sceKernelFindModuleByAddress(a);
		if (m != NULL && strcmp(m->modname, "sceMpeg_library") == 0) {
			*(volatile u32 *)a       = g_mpeg_create_save[0];
			*(volatile u32 *)(a + 4) = g_mpeg_create_save[1];
			*(volatile u32 *)(a + 8) = g_mpeg_create_save[2];
			ClearCaches();
		}
	}
}

void video_skip_probe(SceModule2 *module)
{
	// The five game-bundled psmf player module names PPSSPP knows (sceKernelModule.cpp:1580).
	static const char *psmf_mods[5] = {
		"scePsmfP_library", "scePsmfPlayer", "libpsmfplayer", "psmf_jk", "jkPsmfP_library"
	};
	// Video libraries worth knowing the caller of. A module's import table lists the
	// exact NIDs it calls, so this answers "which entry points does this game actually
	// use" as fact instead of by assumption.
	static const char *dump_libs[3] = { "sceMpeg", "sceMpegbase", "scePsmf" };
	int i;
	// Only LOG when Intro Video Skip is enabled for this game; with it OFF the module dump
	// is noise. The functional detection below still runs unconditionally. On a release
	// build DBG_UART() is 0 anyway.
	int vlog = (DBG_UART() && g_video_skip != VSKIP_OFF);
	if (module == NULL) return;
	// With UART on, name every module the game loads on the wire.
	if (vlog) { char b[80]; sprintf(b, "[VSKIP] mod %s", module->modname); uart_puts(b); }

	// Read the module's own import table (the NID list lives in the module image) to see
	// which entry points this game actually uses.
	if (vlog) {
		for (i = 0; i < 3; i++) {
			SceLibraryStubTable *imp = sctrlFindImportLib(module, (char *)dump_libs[i]);
			int k, n;
			char b[96];
			if (imp == NULL || imp->nidtable == NULL) continue;
			n = (int)imp->stubcount;
			sprintf(b, "[VSKIP] %s imports %s x%d:", module->modname, dump_libs[i], n);
			uart_puts(b);
			for (k = 0; k < n; k++) {
				u32 nid = imp->nidtable[k];
				const char *tag = "";
				if      (nid == MPEG_GET_AVC_AU_NID)   tag = "  <- sceMpegGetAvcAu";
				else if (nid == MPEG_GET_ATRAC_AU_NID) tag = "  <- sceMpegGetAtracAu";
				sprintf(b, "[VSKIP]   %08X%s", (unsigned)nid, tag);
				uart_puts(b);
			}
		}
	}
	for (i = 0; i < 5; i++) {
		if (strcmp(module->modname, psmf_mods[i]) != 0) continue;
		// Latch only; the arming decision needs the per-game setting, which this thread
		// must not touch.
		g_psmf_status_fn = sctrlHENFindFunction(module->modname, "scePsmfPlayer",
		                                        PSMF_GET_CURRENT_STATUS_NID);
		strncpy(g_psmf_modname, module->modname, sizeof(g_psmf_modname) - 1);
		g_psmf_modname[sizeof(g_psmf_modname) - 1] = '\0';
		if (vlog) {
			char b[96];
			sprintf(b, "[VSKIP] psmf module '%s' GetCurrentStatus=%08X",
			        module->modname, (unsigned)g_psmf_status_fn);
			uart_puts(b);
		}
		// This game has a player, so undo any create-fail patch from an earlier bare
		// sceMpeg load; the sceMpeg branch below gates on !g_psmf_status_fn.
		vskip_create_restore();
		return;
	}
	if (strcmp(module->modname, "sceMpeg_library") == 0) {
		g_mpeg_load_seq++;
		// Patch CREATE-FAIL here, not in the watcher. Patch dies on unload; reset each load.
		// Gate: window open AND no psmf player (the player is the better lever).
		g_mpeg_create_patched = 0;
		if (g_vskip_window && g_psmf_status_fn == 0) {
			g_mpeg_create_fn = sctrlHENFindFunction(module->modname, "sceMpeg", MPEG_CREATE_NID);
			vskip_create_patch();
		}
		if (vlog) {
			char b[80];
			sprintf(b, "[VSKIP] sceMpeg_library load #%d%s", g_mpeg_load_seq,
			        g_mpeg_create_patched ? " (sceMpegCreate -> fail)" : "");
			uart_puts(b);
		}
	}
}

// The ONE skip button to inject this instant, or 0 during a release gap. Wall-clock phase
// (VSKIP_PULSE_US buckets): even bucket = a button held, odd = release gap; the next bit
// alternates WHICH button. So the cadence is  X held / gap / START held / gap / repeat.
// The two buttons are pulsed ALTERNATELY, never together. Each button fires every 4th
// phase (~480ms).
static u32 vskip_phase_mask(void)
{
	u32 bucket = sceKernelGetSystemTimeLow() / VSKIP_PULSE_US;
	if (bucket & 1) return 0;                                       // release gap
	return ((bucket >> 1) & 1) ? PSP_CTRL_START : PSP_CTRL_CROSS;
}

// Called from the controller hooks on the game's own per-frame reads. Pulses the skip
// buttons into what the game sees, but ONLY while the watcher says a video is up.
void vskip_inject_buttons(SceCtrlData *pad_data, int count, int res, int negative)
{
	int i, n;
	u32 mask;
	if (!g_vskip_inject) return;
	if (!pad_data || res <= 0) return;
	// PULSE, do not hold: a game skips on the press EDGE. Clear BOTH skip buttons, then set the
	// single one for this phase (if any) - so the game sees a clean press/release EDGE and never
	// both buttons at once. The user's hold signal is a SEPARATE button (VSKIP_HOLD_BTN = D-pad
	// Right), read via kpeek, so X/START here are ours alone and never fight a held button.
	//
	// Negative format reports a PRESSED button as a 0 bit (uofw ctrl.c _sceCtrlReadBuf inverts
	// on the PEEK_BUFFER_NEGATIVE mode bit), so set/clear invert there.
	mask = vskip_phase_mask();
	n = (res < count) ? res : count;
	for (i = 0; i < n; i++) {
		if (negative) {
			pad_data[i].Buttons |=  VSKIP_INJECT_MASK;   // both up   (1 = released)
			pad_data[i].Buttons &= ~mask;                // this one down (0 = pressed)
		} else {
			pad_data[i].Buttons &= ~VSKIP_INJECT_MASK;   // both up   (0 = released)
			pad_data[i].Buttons |=  mask;                // this one down (1 = pressed)
		}
	}
}

// Latch counterpart of vskip_inject_buttons for games that read the controller LATCH
// (make/break/press/release edge bits) instead of the button buffer. Assert make+press
// for the ONE button pulsed this instant (same alternating cadence as the buffer path);
// do NOT fake break/release (a spurious START-release could toggle a pause menu).
void vskip_inject_latch(SceCtrlLatch *latch)
{
	u32 mask;
	if (!g_vskip_inject || !latch) return;
	mask = vskip_phase_mask();
	latch->uiMake    = (latch->uiMake  & ~VSKIP_INJECT_MASK) | mask;
	latch->uiPress   = (latch->uiPress & ~VSKIP_INJECT_MASK) | mask;
	latch->uiBreak   &= ~VSKIP_INJECT_MASK;
	latch->uiRelease &= ~VSKIP_INJECT_MASK;
}

static void vskip_patch(void)
{
	u32 a = g_psmf_status_fn;
	if (a == 0 || g_vskip_patched) return;
	g_vskip_saved[0] = *(volatile u32 *)a;
	g_vskip_saved[1] = *(volatile u32 *)(a + 4);
	*(volatile u32 *)a       = VSKIP_OP_JR_RA;
	*(volatile u32 *)(a + 4) = VSKIP_OP_LI_V0;   // delay slot
	ClearCaches();
	g_vskip_patched = 1;
}

static void vskip_unpatch(void)
{
	u32 a = g_psmf_status_fn;
	if (a == 0 || !g_vskip_patched) return;
	g_vskip_patched = 0;
	// Only write back if OUR two instructions are still there AND the address still
	// belongs to the module we patched.
	if (*(volatile u32 *)a == VSKIP_OP_JR_RA && *(volatile u32 *)(a + 4) == VSKIP_OP_LI_V0) {
		SceModule2 *m = (SceModule2 *)sceKernelFindModuleByAddress(a);
		if (m != NULL && strcmp(m->modname, g_psmf_modname) == 0) {
			*(volatile u32 *)a       = g_vskip_saved[0];
			*(volatile u32 *)(a + 4) = g_vskip_saved[1];
			ClearCaches();
			if (DBG_UART()) uart_puts("[VSKIP] window closed, GetCurrentStatus restored");
			return;
		}
	}
	if (DBG_UART()) uart_puts("[VSKIP] window closed, module gone/changed - left alone");
}

// Persist the learned window to the RUNNING game's gameset.cfg (READ-MODIFY-WRITE, not
// save_game_settings(), which writes other games' globals). Only words 4/5 (our own) are
// touched; the rest are preserved as read. Runs on the watcher at capture end.
static void vskip_save_learned(void)
{
	char p[96], d[80]; SceUID fd; u32 buf[6]; int i;
	const char *gid = umdid[0] ? umdid : "globalstate";
	for (i = 0; i < 6; i++) buf[i] = 0;
	buf[0] = GAMESET_MAGIC; buf[2] = 1;          // defaults if there is no file yet (compress on)
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/gameset.cfg", gid);
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd >= 0) { sceIoRead(fd, buf, sizeof(buf)); sceIoClose(fd); }   // short file -> rest stay 0
	buf[0] = GAMESET_MAGIC;
	buf[4] = (u32)g_video_skip;
	buf[5] = (u32)g_video_skip_ms;
	sceIoMkdir("ms0:/seplugins/SAVESTATE", 0777);
	sprintf(d, "ms0:/seplugins/SAVESTATE/%s", gid);
	sceIoMkdir(d, 0777);
	fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, buf, sizeof(buf)); sceIoClose(fd); }
}

// Kernel thread. Runs only when the per-game setting is CAPTURE or TIMED. Everything is
// measured from t0 (this thread's start = the game's first controller read), which is the
// stable per-game anchor both the capture and the replay share.
int video_skip_thread(SceSize args, void *argp)
{
	u64 t0 = now_us();                 // THE ANCHOR - both capture and replay measure from here
	int capture = (g_video_skip == VSKIP_CAPTURE);
	int x_was_held = 0;                // capture: has the user held the hold button (Right) yet
	int avc_present = -1;              // DIAGNOSTIC ONLY: last-seen sceAvcodec_wrapper presence
	(void)args; (void)argp;

	if (DBG_UART()) {
		char b[64];
		if (capture) sprintf(b, "[VSKIP] CAPTURE run - hold RIGHT until the intro ends");
		else         sprintf(b, "[VSKIP] TIMED run - firing for %d.%03ds",
		                     g_video_skip_ms / 1000, g_video_skip_ms % 1000);
		uart_puts(b);
	}
	g_vskip_banner = capture;          // the display hook draws it while this is set
	g_vskip_active = 1;                // frame limiter OFF for the whole run (see the global)
	if (capture) fps_poll_ensure_started();   // banner is drawn by the poll thread (see vskip_banner_draw)

	while ((now_us() - t0) < VSKIP_CAP_US) {
		// DIAGNOSTIC ONLY, no behaviour change: logs sceAvcodec_wrapper presence up/gone
		// transitions; decides nothing.
		{
			int avc_now = (sceKernelFindModuleByName("sceAvcodec_wrapper") != NULL) ? 1 : 0;
			if (avc_now != avc_present) {
				avc_present = avc_now;
				if (DBG_UART()) uart_puts(avc_now ? "[VSKIP] avcodec up" : "[VSKIP] avcodec gone");
			}
		}

		{
			u64 elapsed = now_us() - t0;
			int fire;

			if (capture) {
				// Read the user's hold button (VSKIP_HOLD_BTN = D-pad Right) via kpeek
				// (kernel-mode read), not the game's pad, so it only sees what the user is
				// physically holding.
				SceCtrlData kpad;
				int x_held;
				int k1 = pspSdkSetK1(0);
				x_held = (kpeek(&kpad) > 0 && (kpad.Buttons & VSKIP_HOLD_BTN)) ? 1 : 0;
				pspSdkSetK1(k1);

				// Fire strictly while Right is held — releasing = "the intro ended here".
				fire = x_held;
				if (x_held) x_was_held = 1;

				// Right let go before the thread's first read: nothing to time, so stand down.
				if (!x_was_held && elapsed >= 2000000) {
					if (DBG_UART()) uart_puts("[VSKIP] hold button not held after arm - capture cancelled");
					break;
				}

				// RELEASED after having held = "the intro ended here". That instant, minus
				// their reaction lag, is the learned window.
				if (x_was_held && !x_held) {
					int ms = (int)(elapsed / 1000) - VSKIP_REACTION_MS;
					if (ms < 0) ms = 0;
					if (ms > VSKIP_LEARN_MAX_MS) ms = VSKIP_LEARN_MAX_MS;
					g_video_skip_ms = ms;
					g_video_skip = VSKIP_TIMED;
					vskip_save_learned();
					if (DBG_UART()) {
						char b[80];
						sprintf(b, "[VSKIP] captured %d.%03ds (RIGHT released at %d.%03ds - %dms lag)",
						        ms / 1000, ms % 1000,
						        (int)(elapsed / 1000000), (int)((elapsed / 1000) % 1000),
						        VSKIP_REACTION_MS);
						uart_puts(b);
					}
					break;
				}
			} else {
				// TIMED: fire for exactly the learned window, then stop. No detector at all.
				fire = (elapsed < (u64)g_video_skip_ms * 1000);
				if (!fire) {
					if (DBG_UART()) uart_puts("[VSKIP] learned window elapsed - standing down");
					break;
				}
			}

			// "Fire everything" for the window: psmf player patch where a game has one, plus
			// button injection and create-fail for everyone else. create-fail is also driven
			// here because a resident codec can create its video mid-window, not just at load.
			g_vskip_window = fire;   // the probe create-fails a codec that loads mid-window
			if (fire) {
				if (g_psmf_status_fn != 0 && !g_vskip_patched) {
					vskip_patch();
					if (DBG_UART()) uart_puts("[VSKIP] armed: psmf player (GetCurrentStatus -> PLAYING_FINISHED)");
				}
				if (g_psmf_status_fn == 0) {
					if (!g_vskip_inject) {
						g_vskip_inject = 1;
						if (DBG_UART()) uart_puts("[VSKIP] window open - injecting START+X");
					}
					if (!g_mpeg_create_patched &&
					    sceKernelFindModuleByName("sceMpeg_library") != NULL) {
						g_mpeg_create_fn = sctrlHENFindFunction("sceMpeg_library", "sceMpeg",
						                                        MPEG_CREATE_NID);
						vskip_create_patch();
						if (g_mpeg_create_patched && DBG_UART())
							uart_puts("[VSKIP] sceMpegCreate -> fail (window)");
					}
				}
			} else if (g_vskip_inject) {
				g_vskip_inject = 0;
			}
		}
		sceKernelDelayThread(VSKIP_POLL_US);
	}

	g_vskip_window = 0;
	g_vskip_banner = 0;
	g_vskip_inject = 0;
	g_vskip_active = 0;                // watcher done -> frame limiter resumes
	vskip_unpatch();
	vskip_create_restore();
	if (DBG_UART()) uart_puts("[VSKIP] done");
	return 0;
}

// Read the RUNNING game's Intro Video Skip flag. Self-contained, same shape and same
// menu-thread-only constraint as game_frame_limit_load above.
void game_video_skip_load(void)
{
	char p[96]; SceUID fd; u32 buf[6]; int n;
	g_video_skip = VSKIP_OFF;              // default OFF (no file / pre-Video-Skip file)
	g_video_skip_ms = 0;
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/gameset.cfg", umdid[0] ? umdid : "globalstate");
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return;
	n = sceIoRead(fd, buf, sizeof(buf));
	if (n >= (int)(5 * sizeof(u32)) && buf[0] == GAMESET_MAGIC) {
		int m = (int)buf[4];
		g_video_skip = (m == VSKIP_CAPTURE || m == VSKIP_TIMED) ? m : VSKIP_OFF;
		if (n >= (int)(6 * sizeof(u32))) g_video_skip_ms = (int)buf[5];
		if (g_video_skip_ms < 0 || g_video_skip_ms > VSKIP_LEARN_MAX_MS) g_video_skip_ms = 0;
		// TIMED with no learned window is meaningless - fall back to capturing one.
		if (g_video_skip == VSKIP_TIMED && g_video_skip_ms <= 0) g_video_skip = VSKIP_CAPTURE;
	}
	sceIoClose(fd);
}
// Intro-skip CAPTURE banner, drawn into the live buffer like fps_draw from the same two
// places (SetFrameBuf hook and fps_poll_thread). Opaque band so it stays readable over a
// bright video.
void vskip_banner_draw(void *topaddr, int bufferwidth, int pixelformat)
{
	static const char *msg = "> SKIP INTRO - HOLD RIGHT";
	int len = (int)strlen(msg);
	int w   = len * 8 + 24;
	int px  = (480 - w) / 2;
	int py  = 16;
	dbg_fb   = (void *)(0xA0000000 | (u32)topaddr);
	dbg_bufw = bufferwidth;
	dbg_pfmt = pixelformat;
	dbg_fill_rect(px, py, w, 20, BR_AMBER);      // amber pill, readable over a bright video
	dbg_fill_rect(px, py, 3, 20, BR_BG);         // dark left stripe
	dbg_transparent = 1;                          // dark glyphs over the amber fill
	dbg_text((px + 12) / 8, (py + 6) / 8, BR_BG, BR_AMBER, msg);
	dbg_transparent = 0;
}
