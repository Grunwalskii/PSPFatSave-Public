#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// ── Intro Video Skip (PER-GAME, gameset.cfg) ───────────────────────────────
// Games play their own intro/publisher-logo reels; the Sony boot logo is the XMB's
// game_plugin_module animation and is CFW's business, NOT this (ARK patches it in
// core/compat/psp/syspatch.c). Games reach video by one of two paths, and the v656
// probe found one real game of each shape, so BOTH are handled:
//
//   scePsmfPlayer — the high-level "play this PMF" API. Game-bundled PRX (see below).
//                   Skip = force GetCurrentStatus to report PLAYING_FINISHED.
//                   CONFIRMED WORKING on hardware (v658, Pirates): instant and clean.
//   sceMpeg       — the low-level decode API (firmware, flash0:/kd/mpeg.prx,
//                   "sceMpeg_library"), normally paired with scePsmf_library as the
//                   demuxer plus the game's own decode loop.
//                   Skip = pulse the game's OWN skip button while a video is loaded.
//
// Measured (v656/v658 probe, UART):
//   Tron    — sceMpeg_library + scePsmf_library, NO player at all. Note scePsmf_library
//             is the DEMUXER; the player is scePsmfP_library ('P'), which is why the
//             five-name list correctly does not match it. Imports 25 sceMpeg functions
//             including both AU getters.
//   Pirates — sceMpeg_library + scePsmf_library + scePsmfP_library, GetCurrentStatus
//             resolved at 0x09E53BFC. Has the player.
//
// MEASURED RESULT (v659, Tron, reproduced 2/2): the intro movie is GONE 304ms after
// injection starts, against a natural length of >=15s. The Disney logo that follows
// ignores START+X for its whole ~6.0s and plays out; that 6s is the floor and is all
// that remains of the intro. See the "BUTTONS ARE THE ONLY LEVER" block below for the
// three builds that tried to force such a video and why none of them survived.
//
// A game with BOTH (Pirates) gets the PLAYER treatment and no injection: the player is
// the precise lever, it is confirmed, and it is instant.
//
// scePsmfPlayer is NOT firmware: absent from both psplibdoc_660.xml and the 1.50 doc,
// and PPSSPP HLE-replaces it under five GAME-BUNDLED module names (sceKernelModule.cpp:
// 1580). It ships on the UMD, its name varies per game, and it does not exist until the
// game loads it — hence the probe rather than a fixed lookup at boot. sceMpeg is the
// opposite: firmware, so its module name is the same for every game — which is what
// makes it usable as a "a video is on screen right now" flag.
//
// Injection cannot beat a game's own lockout (Tron's logo gates skips for ~5s), so this
// shortens an intro rather than removing it. That is the trade for using the game's
// intended path: no black frames, no faked state, and a wrong guess just does nothing.
#define PSMF_PLAYER_STATUS_PLAYING_FINISHED 0x200
#define PSMF_GET_CURRENT_STATUS_NID         0xF8EF08A6
#define MPEG_GET_AVC_AU_NID                 0xFE246728
#define MPEG_GET_ATRAC_AU_NID               0xE1CE83A7
#define MPEG_CREATE_NID                     0xD8C5F121
// jr ra / li v0, PLAYING_FINISHED — ARK's MAKE_DUMMY_FUNCTION_RETURN_0 shape
// (common/include/macros.h: JR_RA 0x03E00008, LI_V0(n) (0x2402<<16)|(n&0xFFFF)).
#define VSKIP_OP_JR_RA 0x03E00008u
#define VSKIP_OP_LI_V0 (0x24020000u | PSMF_PLAYER_STATUS_PLAYING_FINISHED)

// CREATE-FAIL, an EXPERIMENT distinct from the abandoned EOF stub (see below). Instead of
// faking end-of-stream mid-playback, make sceMpegCreate itself fail up front with
// SCE_MPEG_ERROR_OUT_OF_MEMORY (0x80610022, uofw lib_mpeg.h; PPSSPP sceMpeg.cpp:497 is
// its NO_MEMORY return). The reasoning that makes it worth trying where the stub failed:
// video INIT failing is a real runtime condition a game must cope with (low memory,
// bad disc), so a game plausibly has a tested "create failed -> skip the video" branch,
// where it had no branch at all for "stream ended on the first frame". 3-word entry
// patch: lui v0,0x8061 / jr ra / ori v0,v0,0x22 (the ori is the delay slot). If the game
// wedges instead of skipping, this is as dead as the stub and injection-only is the ship.
#define MPEG_CREATE_OP0 0x3C028061u   // lui v0, 0x8061
#define MPEG_CREATE_OP1 0x03E00008u   // jr  ra
#define MPEG_CREATE_OP2 0x34420022u   // ori v0, v0, 0x22   (delay slot) -> v0 = 0x80610022

// ── THE LEARNED TIME WINDOW (v666) ────────────────────────────────────────────
// Every automatic "is a video playing" signal was tried and every one failed, in
// OPPOSITE directions, because "codec loaded" is simply not the same event as "video
// on screen":
//   Field Commander - keeps sceMpeg_library RESIDENT into its menu, so the presence gate
//                     stayed true and pulsed START+X into the menu, reaching a save
//                     dialog 6.75s in (v663). Capped at 2s (v664) - a guess.
//   Valkyria Chr. II - loads the codec at boot INIT and plays its video much later. The
//                     presence gate armed at the load, the 2s cap shut the window at
//                     +2.1s, and create-fail was RESTORED at stand-down - so when the
//                     video actually ran, nothing was armed at all (v665).
//   Tomb Raider: Ann - no sceMpeg at ALL (decodes via sceVideocodec directly), so the
//                     gate never fires. avcodec never drops either (v665 measured it).
// There is no signal that separates an intro video from a menu/gameplay video - they are
// the same thing technically. So stop guessing and ASK THE USER once, per game:
//
//   CAPTURE run - ARMED at boot by boot_frozen_prompts: the game is frozen, the banner
//                 "Hold RIGHT until Intro skipped" is shown, and the user holds D-pad Right
//                 (VSKIP_HOLD_BTN) for 1s. The game then resumes with Right already held, so
//                 there is NO forced window — the skip fires strictly while Right is held
//                 (this is what stops a psmf-patch game from skipping its intro before the
//                 user can react). Right is deliberately NOT an injected button, so the
//                 injector can pulse a clean X/START. When the user RELEASES Right, the
//                 elapsed time minus VSKIP_REACTION_MS (their reaction lag) is the learned
//                 window, saved per game; the setting flips to VSKIP_TIMED.
//   TIMED runs  - fire everything from the anchor for the learned ms. No detector, no arm.
//
// The anchor is this thread's start (the game's first controller read; for CAPTURE, right
// after the arm resume), which is stable per game. Path-agnostic by construction: a timer
// does not care whether a game uses psmf, sceMpeg, a resident codec, or sceVideocodec
// directly - which is exactly why it covers the games no signal could.
#define VSKIP_REACTION_MS 500                 // capture: trimmed off (hold btn released AFTER the intro ends)

// Buttons pulsed into the game's own controller reads while the window is open. Pulsed
// ALTERNATELY, one at a time, never together (see vskip_phase_mask) - simultaneous START+X
// broke Tron's confirmation screen where X-alone works.
#define VSKIP_INJECT_MASK  (PSP_CTRL_START | PSP_CTRL_CROSS)
// Press/release cadence is WALL-CLOCK, not call-count: X held for VSKIP_PULSE_US, released
// for the same, phase derived from sceKernelGetSystemTimeLow(). A call-count tick assumed
// one read per frame and broke on any loop that polls at a different rate (a confirmation
// screen reading the pad on its own loop could land every sample in the release half and
// never see a press). Time-based, every reader at the same instant sees the same held state.
#define VSKIP_PULSE_US 120000                  // 120ms held, then 120ms released
#define VSKIP_POLL_US      100000              // watcher poll period
// The button the USER holds to drive CAPTURE (the arm gate + the fire window). Deliberately
// NOT one of the injected buttons (VSKIP_INJECT_MASK): X used to be both, which forced the
// injector to "own"/mask the user's held X and left the game seeing a muddied X — a final
// "press X/START" prompt after Tron's videos never registered. D-pad Right is unused during
// an intro, so the injector can pulse a CLEAN X/START while the user holds Right.
// SCEMPEG LEVERS FOR A PLAYER-LESS GAME. Two are live: button injection (confirmed on
// Tron) and, as of v663, the create-fail experiment above. What is ABANDONED and must NOT
// be revived without new evidence is the end-of-stream stub - a different thing from
// create-fail (it fakes EOF mid-playback; create-fail refuses the video up front):
//   v657/v658  Overwriting sceMpegGetAvcAu/GetAtracAu with an end-of-stream stub (dts =
//              -1 + 0x80618001, what the real functions do at EOF). Killed the picture
//              but not the wait: Tron's first video became a BLACK SCREEN needing a
//              manual X. Also inconsistent run to run - no effect in v657, clear effect
//              in v658, with only logging changed between them. Never explained.
//   v660       Same stub as an escalation for a video that ignores the button, aimed at
//              Tron's Disney logo. Far worse: the logo went from a clean 6.0s to 34.2s of
//              black that needed a CONSOLE SUSPEND to escape.
// The pattern across BOTH stub attempts: killing the decode removes the picture and leaves
// whatever the game is actually waiting on untouched, or wedges it. A video that refuses
// the skip button is refusing deliberately, and we have no honest way to satisfy its
// gate. Tron's Disney logo is unskippable with every lever we have - v659 pulsed START+X
// across its entire 6s and it ignored them - and that 6s is the floor.
// (v660 was also mistuned to 100ms, below the 304ms video 1 needs to button-skip, so it
// stubbed the one video that already worked. That was a separate bug and fixing it to 1s
// would not have saved the logo.)
// Outer safety bound on the watcher thread. The learned window (or the user releasing X)
// is what actually ends it; this only guarantees the thread always exits, e.g. if a
// capture run is left with X never pressed.
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

// The CAPTURE banner is drawn by fps_poll_thread (the same path the FPS overlay uses to
// reach games that never call sceDisplaySetFrameBuf during play, e.g. GTA/Pirates), so the
// watcher has to spin that thread up. Defined far below; forward-declared here.
void fps_poll_ensure_started(void);

// Kernel PRX .bss is NOT zeroed at load, and every zero initializer above lands there.
// g_psmf_status_fn matters most: video_skip_probe only ever WRITES it, so on a game with
// no player the watcher would read boot garbage, pass the != 0 test and patch a wild
// address. g_vskip_inject matters too — it is read by the controller hooks on EVERY
// frame from the very first one, long before the watcher exists. Called from
// module_start with the rest of the BSS init.
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

// Overwrite sceMpegCreate's entry so it fails with OUT_OF_MEMORY (see the CREATE-FAIL
// note above). g_mpeg_create_fn must already point at the current codec's export.
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

// Restore sceMpegCreate. Guarded like every other revert here: the codec unloads between
// videos and a reload lands at the same address with its own code, so only write back if
// OUR three words are still present AND the address still belongs to sceMpeg_library.
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
	// Only LOG when Intro Video Skip is actually enabled for this game: with the feature
	// OFF the probe's module dump is pure noise (a game keeps loading sceChnnlsv/
	// sceVshSDAuto etc. for its whole run). The functional detection below still runs
	// unconditionally, so a CAPTURE/TIMED game that hasn't had its setting loaded yet at an
	// early psmf-player load is not missed. NOTE: on a release build DBG_UART() is 0 anyway.
	int vlog = (DBG_UART() && g_video_skip != VSKIP_OFF);
	if (module == NULL) return;
	// The probe half of this feature: with UART on, every module the game loads is
	// named on the wire. That is how we find out what a given game actually ships —
	// the five names above are PPSSPP's list, not an exhaustive one.
	if (vlog) { char b[80]; sprintf(b, "[VSKIP] mod %s", module->modname); uart_puts(b); }

	// v657 armed the end-of-stream stub across the whole of Tron's first video and the
	// game played it through regardless (15.5s, then straight on to video 2) — which
	// only adds up if the AU getters are never called, i.e. Tron reaches video by some
	// other entry point. So stop guessing which one and read it out of the module's own
	// import table. The NID list lives in the module image, so it is readable right
	// here, before the loader has even resolved the stubs, and needs nothing armed.
	// Costs nothing on a release build (DBG_UART() is 0) and nothing on modules that
	// import none of these libraries.
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
		// Exports are registered at load time, so the export walk resolves here even
		// though module_start has not run yet. Latch only — the arming decision needs
		// the per-game setting off the MS, which this thread must not touch.
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
		// This game has a player, which is the confirmed lever - so undo any create-fail
		// patch we may have applied for an earlier bare sceMpeg load and never apply it
		// again (the sceMpeg branch below gates on !g_psmf_status_fn). This is what keeps
		// create-fail from breaking a player that decodes through sceMpeg underneath, e.g.
		// Pirates - its player loads AFTER sceMpeg but its own create calls come later.
		vskip_create_restore();
		return;
	}
	if (strcmp(module->modname, "sceMpeg_library") == 0) {
		g_mpeg_load_seq++;
		// Patch CREATE-FAIL here (probe hook) not watcher. Patch dies on unload; reset each load.
		// Gate: window open AND no psmf player (player is better lever); window-gated outside learned
		//   is an in-game cutscene and must be left alone.
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
//
// The two buttons are pulsed ALTERNATELY, never together. Pressing START+X on the SAME frame
// broke Tron's post-video confirmation screen: a human presses X alone and it advances, but
// simultaneous START (which a Yes/No prompt can read as pause/cancel/selection-move) muddied
// it so the X never confirmed. Alternating guarantees the prompt sees clean X-alone presses,
// while a video that skips on START still gets clean START-alone presses. Each button fires
// every 4th phase (~480ms) - ~20 presses of each inside a 10s window.
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

// Latch counterpart of vskip_inject_buttons. Some games read their skip input from the
// controller LATCH (an edge accumulator: make/break/press/release bits) instead of the
// button buffer — e.g. Ratchet & Clank — so the buffer injection above never reaches them
// and X/START "don't register". A confirmation screen reads the MAKE edge ("just pressed"),
// so assert make+press for the ONE button pulsed this instant (same alternating cadence as
// the buffer path). We do NOT fake break/release: a spurious START-release could toggle a
// pause menu. Same g_vskip_inject gate + wall-clock phase, so the two stay in lock-step.
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
	// Games unload the psmf module once the intro is done, so by now this address may
	// be freed or re-used by something else — restoring blind would corrupt whatever
	// took it over. Only write back if OUR two instructions are still there AND the
	// address still belongs to the module we patched.
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

// Persist the learned window to the RUNNING game's gameset.cfg. READ-MODIFY-WRITE, and
// deliberately NOT save_game_settings(): that writes the g_autoload/g_compress/
// g_frame_limit globals, which belong to whatever game the BROWSER last looked at, not
// necessarily the running one — writing them here would copy another game's settings
// onto this one. Only words 4/5 (our own) are touched; the rest are preserved as read.
// Runs on the watcher (a kernel thread, MS-safe like the menu thread) at capture end,
// which is after the intro when the game's boot streaming has settled.
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
		// DIAGNOSTIC ONLY, no behaviour change. sceAvcodec_wrapper is the decode layer
		// UNDER sceMpeg, and games that bypass sceMpeg entirely (Tomb Raider: Anniversary
		// uses sceVideocodec directly) still load it - so its load/UNLOAD could be a
		// broader "video playing / intro over" signal than sceMpeg presence. The probe
		// only sees loads; this polls presence so we can finally see whether a game DROPS
		// it at the menu (usable stop signal) or keeps it resident (not usable). Logs the
		// up/gone transitions; decides nothing.
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
				// Read the user's REAL hold button (VSKIP_HOLD_BTN = D-pad Right), not the
				// game's pad: our own injection writes X/START into the game's buffer, and the
				// hold button is deliberately a DIFFERENT button so the two never collide.
				// kpeek is the kernel-mode (unmasked) hardware read the NOTE-tap detector
				// already uses, so it only ever sees what the user is physically holding.
				SceCtrlData kpad;
				int x_held;
				int k1 = pspSdkSetK1(0);
				x_held = (kpeek(&kpad) > 0 && (kpad.Buttons & VSKIP_HOLD_BTN)) ? 1 : 0;
				pspSdkSetK1(k1);

				// No forced window: the capture is ARMED by the boot Hold gate
				// (boot_frozen_prompts), so the user is already holding Right as the intro's
				// first frame plays. Fire strictly while Right is held — releasing = "the intro
				// ended here". This is what stops a psmf-patch game (Pirates) from skipping
				// the intro before the user can react.
				fire = x_held;
				if (x_held) x_was_held = 1;

				// Armed but Right let go before the thread's first read of it (released right
				// after the 1s arm): nothing to time -> stand down rather than spin the full
				// VSKIP_CAP_US. ~2s grace covers the resume -> thread-start -> first-poll gap.
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

			// "Fire everything" for the window: the psmf player patch where a game has one
			// (Pirates - precise and instant), plus button injection and create-fail for
			// everyone else. create-fail is driven from HERE, not only from the probe,
			// because a resident codec (VC2) loads at boot init and creates its video much
			// later - it has to be patched for the whole window, not just at load.
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
// Intro-skip CAPTURE banner. Same draw mechanic as fps_draw (into the live buffer), and
// called from the SAME two places — the SetFrameBuf hook AND fps_poll_thread — so it reaches
// games that never call sceDisplaySetFrameBuf during play (GTA/Pirates), exactly like the
// FPS overlay. Opaque band so it stays readable over a bright video. Own flag (g_vskip_banner):
// one test per draw when off.
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
