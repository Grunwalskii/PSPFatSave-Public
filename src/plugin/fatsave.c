#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// global — MUST be explicitly initialized (kernel PRX BSS is not zeroed!)
int is_running = 0;			// 0x2FA8
int state_flag = 0;			// 0x2F50

// Save browser: the open combo (hook) sets g_menu_open + wakes the menu thread.
volatile int g_menu_open = 0;
SceUID g_menu_thid = -1;
// Real sceController_Service Peek/Read (set in main.c before the syscall patch);
// defined up here so both FreezeSave and the browser can read the pad directly.
int (*g_real_ctrl_peek)(SceCtrlData *, int) = NULL;
int (*g_real_ctrl_read)(SceCtrlData *, int) = NULL;
// NEGATIVE-format counterparts of the two above (set in main.c). Some games read
// input via these instead of the Positive family — see sceCtrlReadBufferNegativePatched
// for why they need the exact same post-resume treatment as Positive.
int (*g_real_ctrl_peek_neg)(SceCtrlData *, int) = NULL;
int (*g_real_ctrl_read_neg)(SceCtrlData *, int) = NULL;
// Real sceCtrl LATCH reads (set in main.c) + a flag that suppresses+drains the
// USER-mode latch across the menu->game resume window — the SECOND button leak
// (latch-reading games, e.g. Ratchet & Clank). See sceCtrlReadLatchPatched.
int (*g_real_ctrl_readlatch)(SceCtrlLatch *) = NULL;
int (*g_real_ctrl_peeklatch)(SceCtrlLatch *) = NULL;
volatile int g_suppress_latch = 0;
// THIRD button leak (buffer-backlog games, e.g. Tomb Raider Legends): counts down
// the game's own post-resume sceCtrlReadBufferPositive/Negative calls to suppress —
// see the block comment above sceCtrlReadBufferPositivePatched for the mechanism.
volatile int g_suppress_posbuf_calls = 0;
// Resume timestamp (controller sampler clock, sceKernelGetSystemTimeLow) captured when the
// buffer suppress is armed: a ring sample stamped at/before this is stale menu input to
// blank; the first sample stamped AFTER it is real post-resume input that must pass through.
// See suppress_posbuf_slots.
volatile u32 g_suppress_posbuf_ts = 0;
// sceImposeSetHomePopup, resolved in main.c. Called with 0 to disable the HOME
// popup while the browser is open (so HOME can't exit the game mid-freeze and
// hang the PSP), and 1 to re-enable on close.
int (*g_set_home_popup)(int) = NULL;

// ── Plugin settings (persisted to SAVESTATE/settings.cfg) ──
int g_show_debug   = 0;   // Debug routing: 0=OFF (default) 1=Log MS 2=Log Screen 3=Screen and MS
                          // (bit0 = MS log, bit1 = on-screen; see DBG_MS/DBG_SCR in pspfatsave.h).
                          // Merged with g_uart_log in the menu: modes 0-3 map to g_show_debug 0-3,
                          // mode 4 = UART (g_uart_log=1, g_show_debug=0).
int g_uart_log     = 0;   // UART logging: 0=OFF (default) 1=ON. Now part of the merged Debug Messages
                          // menu setting (see g_show_debug). Persisted separately for compatibility.
int g_iobuf_flush_kb = 1024;   // IOBUF flush threshold + Fast-mode chunk size (1MB fixed)
int g_compress = 1;       // PER-GAME (gameset.cfg via load_game_settings). 1 = Compact (FastLZ,
                          // default), 0 = Off (store chunks uncompressed). Compression is the dominant
                          // save cost (~4x the aligned write time), so Off trades ~2x file size for a
                          // much shorter freeze. Per-record + self-describing (encode_chunk/decode_chunk)
                          // — the loader reads the file's own flag, so any save loads under either mode.
int g_default_slot = 1;   // 0 = New, 1 = Last (default): browser's initial selection
int g_stage_spot   = 1;   // Which 8MB slot of vacated user RAM stages the kernel snapshot:
                          // 0=Low(0x88800000) 1=Mid(0x89000000, default) 2=High(0x89800000).
                          // Mid avoids BOTH GTA's low-RAM audio DMA and the firmware's top-RAM
                          // suspend bookkeeping. Low/High kept selectable for per-game tuning.
                          // (High overlaps the apply blob's top-64KB -> High is save-only.)
int g_autoload     = 0;   // PER-GAME Auto-Open-on-Boot (stored in the game's gameset.cfg,
                          // not the global settings.cfg): 1 = on boot, if this game has a
                          // save, auto-open the load menu so the user can load before the intro.
int g_frame_limit  = 0;   // PER-GAME frame cap (gameset.cfg). 0 = OFF, else a target FPS in
                          // 25..60 step 5. Paces the game by delaying inside the present hook
                          // (see frame_limit_wait) so a title that swings (e.g. Tomb Raider
                          // 30-60) holds a steady rate instead — a stable cap usually reads as
                          // smoother than a fluctuating higher number. NOTE the panel is 60Hz,
                          // so only clean divisors (60/30/20) are judder-free; the in-between
                          // steps (25/35/40/45/50/55) don't align to the refresh and will
                          // judder somewhat even while holding their average — offered anyway
                          // since the right trade is game- and user-specific.
int g_overclock_id = 0;   // GLOBAL. Index into g_oc_multipliers (0 = stock 333MHz, see the
                          // oc_* block below). Applied at every plugin boot and live from the
                          // Settings menu. ANY firmware suspend/resume cycle (native sleep, or
                          // our own save/load) reverts the raw PLL registers back to stock as a
                          // side effect (confirmed by the user) — utils.c's ProcessSignals
                          // re-applies it on RESUME_COMPLETED (see pspfatsave.h for the extern
                          // + oc_apply's prototype; declared non-static there for that reason).
int g_overclock_stable = 0;   // GLOBAL. 0 = ask "Apply Overclock?" every boot (safety for a
                          // step that hung last session). 1 = the user marked this step STABLE
                          // (Settings: Triangle on the Overclock row): skip the boot confirm and
                          // apply it directly, and show the value in RED "(Set as Stable)". X on
                          // that row clears it back to 0. Persisted in settings.cfg (buf[10]).
int g_show_fps_overlay = 0;   // GLOBAL. 0=Off (default) 1=On/1s 2=On/0.5s 3=On/0.2s — the
                          // number is the averaging-window length fps_tick() uses (see
                          // fps_window_us()); live FPS counter drawn during actual gameplay
                          // (menu closed) — see the sceDisplaySetFrameBuf hook further
                          // below. Set from the Settings menu.
int g_fps_show_lows = 0;      // GLOBAL. 0=Disabled (default) 1=On — also draw a 1% Low
                          // figure (average FPS of the slowest 1% of recent frames, the
                          // usual "worst-case stutter" metric) alongside the main FPS
                          // number. Independent of g_show_fps_overlay's window length —
                          // see fps_calc_1pct_low(), recalculated on its own fixed 1s
                          // cadence regardless.
int g_show_battery = 0;       // GLOBAL. 0=Off (default) 1=Percent 2=Percent+Time 3=ALL —
                          // live battery overlay, drawn by the same poll thread as the FPS
                          // counter (see battery_draw / fps_poll_thread). Set from the
                          // Settings menu.
int g_show_cpu_usage = 0;     // GLOBAL. 0=Off (default) 1=On — live CPU load % overlay,
                          // same idle-thread-clocks technique PSP-HUD uses (see
                          // cpu_usage_tick / getCpuUsage in PSP_References/PSP-HUD-master/
                          // hud/main.c). Set from the Settings menu.
int g_show_ft_chart = 0;      // GLOBAL. 0=Off (default) 1=On — full-width scrolling frametime
                          // histogram (one column per frame, height = frametime); see
                          // ft_chart_tick/ft_chart_draw. Independent of Show FPS: ticks and
                          // draws even if the numeric FPS overlay is off. Set from the
                          // Settings menu.

// Armed at menu-thread startup; the controller hook consumes it on the first read (display +
// game threads up, umdid known) to check the per-game flag and auto-open the menu once.
int g_autoload_armed = 0;

// Sleep-hybrid flags (see pspfatsave.h). utils.c::ProcessSignals arms the RTC
// auto-wake, runs the PHASE0_0 capture/apply action, and sets g_resumed (polled
// by wait_for_resume()).
int g_sleep_arm = 0;
int g_resumed = 0;
int g_sleep_mode = 0;        // PHASE0_0 action: 0 none, 9 capture (save), 10 apply (load)
// Kernel capture/apply window at Phase0_0: the real kernel (KERNEL_RAM_SIZE =
// lower 4MB; the upper-4MB volatile/vshell scratch is skipped — see pspfatsave.h).
volatile u32 g_kcap_size = KERNEL_RAM_SIZE;
volatile u32 g_kcap_off  = 0;
// Runtime base of the kernel-snapshot staging slot. Three non-overlapping 8MB
// slots span the 24MB vacated user RAM. g_stage_spot is now forced to Mid (1)
// and no longer user-selectable (see load_settings / draw_settings).
// CAVEAT: GAME_STAGE_APPLY (0x89800000, LOAD-only) is the START of the High slot,
// so the apply routine lands on High's first bytes -> High is unusable for LOAD.
// Low (0x88800000..0x89000000) and Mid (0x89000000..0x89800000) are clear; the
// apply routine sits just past Mid's end, not inside it.
static const u32 g_stage_bases[3] = { 0x88800000, 0x89000000, 0x89800000 };
volatile u32 g_stage_base = 0x89000000;   // = g_stage_bases[g_stage_spot] (default Mid)

// LOAD-only apply-blob (ApplyHandoff copy loop) location. FIXED at 0x89800000 — the
// START of the upper 8MB (High) slot, just past the Mid snapshot it reads and clear
// of the kernel (0x88000000..0x88800000) it writes. This was a 6-way user setting
// ("Apply blob spot") during the cross-session-crash hunt; removed and pinned to the
// proven default.
volatile u32 g_apply_base = 0x89800000;

// Capture/apply suspend phase. Always PHASE0_0 (the last, most-quiesced suspend
// event) — every call site below sets it unconditionally. SCE_SYSTEM_SUSPEND_
// EVENT_FREEZE (pspfatsave.h) is defined but never assigned to this.
volatile int g_cap_phase = SCE_SYSTEM_SUSPEND_EVENT_PHASE0_0;
// Load-carry block. The three fields MUST stay contiguous (op_mode at +0, the two
// rates at +4/+8): a cross-session LOAD patches them into the staged snapshot at
// header[9]+0/+4/+8 (the plugin loads at a different kernel address each boot, so it
// can't use its own addresses — it patches at the SAVE-TIME offset). op_mode: 0 =
// SAVE, 1 = LOAD — the SAVE snapshot captures it and a LOAD patches the copy to 1 so
// the resumed FreezeSave tail does a read-only restore (NEVER writes the savegame).
// verify_mbps/read_mbps: the two pre-suspend LOAD rates (MB/s*100) for the
// "Verifying save"/"Reading kernel" panel lines, carried across the kernel apply so
// the resumed tail can re-show them (the labels are fixed; only the rates vary).
// start_lo/start_hi: the LOAD's RTC start tick (now_us, split into two u32 to avoid
// u64 alignment worries in the byte-offset snapshot patch) — carried across the apply
// so the resumed tail can print the true end-to-end wall-clock "Load time" (the load
// begins in the load session but finishes in the resumed save session — its own start
// global would otherwise be clobbered by the snapshot; SAVE uses its live t_start).
// g_op_mode is a macro alias for the first field so its existing uses are unchanged.
// (struct load_carry is declared in pspfatsave.h so utils.c/ProcessSignals can read
// g_lc.op_mode to gate the LOAD-only flash1: reinit on RESUME_COMPLETED.)
volatile struct load_carry g_lc = { 0, 0, 0, 0, 0 };
#define g_op_mode (g_lc.op_mode)
// Save-time dispatcher context (filled by SysEventShim on every sysevent dispatch;
// restored by ApplyHandoff on a load — see pspfatsave.h). Explicitly initialized
// (kernel PRX BSS is not zeroed) and 64-byte aligned (one cache line).
volatile u32 g_handoff_ctx[16] __attribute__((aligned(64))) = {0};
// Save-time address of g_handoff_ctx for the armed load (from header[10]).
volatile u32 g_handoff_ctx_addr = 0;
// Mode-9 DMAC-drain residual: the channel-enable bits still set when the drain spin
// gave up (0 = fully drained). Set inside the sysevent handler (MS is dead there),
// logged by the poll thread AFTER resume.
volatile u32 g_dma_drain_left = 0;

// Work buffer for compression/decompression + MS transfer staging (in BSS).
// Avoids using VRAM, which would conflict with the VRAM we're saving/restoring.
u8 work_buf[COMPRESS_BUF_SIZE] __attribute__((aligned(64)));

// ── Minimal kernel-safe debug screen ────────────────────────
// pspDebugScreenPrintf needs vsnprintf (not in kernel libc),
// but sprintf IS available.  We use sprintf + direct VRAM writes.
// No display-mode stealing — writes to the game's real framebuffer.
// (Debug-screen primitives now live in gfx.c; checkpoint/trace API in debug.c/.h.)

// ────────────────────────────────────────────────────────────
// Partial freeze via thread suspension, NOT IE-bit clear.
//
// Leaves interrupts and the kernel scheduler/dispatcher fully alive
// so sceIo* can still block and complete. A blanket IE-bit clear
// stops the dispatcher too, which sceIo* needs to complete a transfer.
//
// sceKernelSuspendThread/sceKernelGetThreadmanIdList are unreversed
// stubs in the available uOFW checkout — this rests on the documented
// SDK contract plus Ark-4's own recovery menu precedent (which
// suspends named VSH threads the same way), not a verified kernel
// implementation. A hang here (if a suspended thread held a kernel
// lock something else needed) would be SILENT — execution never
// reaches resume_game_threads(). Power-cycle recovery if so.
// ────────────────────────────────────────────────────────────

#define MAX_SUSPEND_THREADS 64

static SceUID suspended_ids[MAX_SUSPEND_THREADS];
static int suspended_count;

// Enumerate the running game's USER threads into out[] (up to MAX_SUSPEND_THREADS),
// EXCLUDING this (menu) thread; if status_out != NULL, also record each thread's
// natural status (READY/WAITING/…) BEFORE any suspend. Returns the count. Ordinary
// enumeration with IRQs on — call BEFORE freezing. Shared by FreezeSave (wants the
// status, for context capture) and FreezeLoad (passes NULL). Filter: exact
// PSP_THREAD_ATTR_USER (top 3 attr bits) — kernel/system threads are not game state.
static int identify_game_threads(SceUID *out, int *status_out)
{
	SceUID ids[MAX_SUSPEND_THREADS];
	SceUID my_tid = sceKernelGetThreadId();
	int i, count = 0, n = 0;
	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, MAX_SUSPEND_THREADS, &count) >= 0) {
		for (i = 0; i < count && n < MAX_SUSPEND_THREADS; i++) {
			SceKernelThreadInfo info;
			if (ids[i] == my_tid) continue;
			memset(&info, 0, sizeof(info));
			info.size = sizeof(info);
			if (sceKernelReferThreadStatus(ids[i], &info) < 0) continue;
			if ((info.attr & 0xE0000000) != PSP_THREAD_ATTR_USER) continue;
			if (status_out) status_out[n] = info.status;   // natural status BEFORE we suspend
			out[n++] = ids[i];
		}
	}
	return n;
}

void resume_game_threads(void)
{
	int i;
	for (i = 0; i < suspended_count; i++) {
		sceKernelResumeThread(suspended_ids[i]);
	}
	suspended_count = 0;
}

// Resume ONLY the suspended threads that are NOT in the game set — i.e. the
// VSH/system user threads — so the firmware suspend handshake can complete
// while the GAME threads stay parked (frozen).
// (all user threads suspended, none resumed) blocks scePowerRequestSuspend
// from sleeping, but the game must stay frozen so it doesn't run on
// the clobbered staging RAM after resume. The game threads remain in
// suspended_ids[] and are resumed later by resume_game_threads() (resuming an
// already-running non-game thread there is a harmless no-op error).
static void resume_nongame_threads(const SceUID *game_tids, int tcount)
{
	int i, j;
	for (i = 0; i < suspended_count; i++) {
		int is_game = 0;
		for (j = 0; j < tcount; j++) {
			if (suspended_ids[i] == game_tids[j]) { is_game = 1; break; }
		}
		if (!is_game)
			sceKernelResumeThread(suspended_ids[i]);
	}
}

// Block until the firmware suspend completes a full sleep+resume cycle (set by
// RESUME_COMPLETED via g_resumed). The calling thread is itself frozen+resumed
// by the firmware across this loop, so this budget is ACTIVE-thread time (the
// frozen sleep doesn't count). It does NOT set the suspend's wall-clock (that's
// the +2s RTC auto-wake alarm in utils.c). The budget DOES burn while the
// firmware's pre-descent 0x401/0x402 suspend handshake is still bouncing (a
// vetoer reports BUSY -> retry loop; system stays awake). Observed (Ridge Racer
// v470): a save hit that veto loop for >2s and the old 2s budget aborted a
// suspend that may still have gone through — and the abort itself is racy, since
// scePowerRequestSuspend can't be canceled (the pending suspend then fired
// mid-game once the abort resumed the game threads). 5s rides out transient
// vetoes (e.g. the game mid-MS-audio-load at freeze: kernel-side I/O drains on
// its own — kernel threads aren't frozen) while still bounding a permanent one.
// Safe for SAVE (the PHASE0_0 action only READS the kernel, so this thread
// survives).
static int wait_for_resume(void)
{
	int i;
	for (i = 0; i < 1000; i++) {        // ~5s ACTIVE budget (see note above); 5ms poll detects resume ~10x sooner
		if (g_resumed) { g_resumed = 0; return 1; }
		sceKernelDelayThread(5000); // 5ms
	}
	// Budget expired -> give up at 5s. (A +25s "descent-aware" extension for
	// g_sleep_arm==2 was tried in v471-477 and removed: observed transient vetoes
	// clear within ~1s (Pirates, ~16 bounces of 0x402), while a WEDGED veto — a
	// frozen thread holding whatever the 0x402 vetoer checks — never cleared even
	// after 30s (Ridge Racer, Ratchet & Clank rode the full extension to the end
	// every time). The extension bought zero rescues and cost 25s per failure.
	// NOTE the abort race remains: the pending scePowerRequestSuspend can't be
	// canceled, so the abort's thread-resume releases the veto and the suspend
	// then fires mid-game with capture disarmed — and the RTC alarm is past-due
	// by then (historically a lost wake), so the console sleeps until a manual
	// power press. A controlled abort (re-arm the alarm + ride that sleep out
	// before showing the notice) is the known follow-up if this bites.
	return 0;
}


// (The LOAD veto rescue is GONE entirely: v524's name-free rescue resumed every
// frozen thread with a stack outside the staged window, but waking a RUNNABLE
// thread onto RAM already clobbered by the staged snapshot hard-froze Dissidia
// Duodecim. The ME veto is now PREVENTED instead of rescued: me_rpc_probe at
// every save/load freeze guarantees no frozen thread owns the SceMediaEngineRpc
// mutex, so the 0x402 descent can only meet LIVE holders — native-sleep
// behavior. A residual non-ME veto aborts gracefully.)

// Wall-clock microseconds via the RTC (monotonic; advances across the firmware
// suspend, unlike the system tick). Used for the speed instrumentation (W1).
u64 now_us(void)
{
	u64 t = 0;
	sceRtcGetCurrentTick(&t);
	return t;
}
// Log a phase duration in milliseconds, then reset the marker to "now".
// Release build: no-op (reads t0 so the marker vars don't warn as set-unused).
#if DEBUG_BUILD
#define TLOG(label, t0) do { u64 _n = now_us(); WriteDebugLogHex(label, (u32)(_n - (t0)) / 1000); (t0) = _n; } while (0)
#else
#define TLOG(label, t0) do { (void)(t0); } while (0)
#endif

// Split-phase timing: separate the CPU part (compress/decompress) from the MS
// part (write/read) inside a stream loop, to size the pipelining win. TM_BEGIN
// declares two µs accumulators + a lap marker; TM_LAP folds the elapsed time
// since the last lap into an accumulator and re-marks; TM_LOG prints one total
// in ms. All three vanish entirely in a release build (no per-chunk now_us()
// cost), so the accumulator/marker names must be UNIQUE per loop.
#if DEBUG_BUILD
#define TM_BEGIN(a, b, mk)   u64 a = 0, b = 0, mk = now_us()
#define TM_BEGIN1(a, mk)     u64 a = 0, mk = now_us()   // single-accumulator variant (Fast-mode branches: no compress/decompress split)
#define TM_LAP(acc, mk)      do { u64 _n = now_us(); (acc) += _n - (mk); (mk) = _n; } while (0)
#define TM_LOG(label, acc)   WriteDebugLogHex(label, (u32)(acc) / 1000)
#else
#define TM_BEGIN(a, b, mk)   do { } while (0)
#define TM_BEGIN1(a, mk)     do { } while (0)
#define TM_LAP(acc, mk)      do { } while (0)
#define TM_LOG(label, acc)   do { } while (0)
#endif

// ── CRC32 (standard IEEE poly 0xEDB88320) — savefile integrity ──
// Table-based (1KB in BSS, lazily built once). Use the conventional
// init = 0xFFFFFFFF and final XOR 0xFFFFFFFF (see crc32_init/finish).
// We CRC the payload (everything after the header) so a corrupt save is
// rejected BEFORE the kernel apply (which overwrites live kernel RAM).
static u32 g_crc_table[256];
static int g_crc_table_ready = 0;
static void crc32_build_table(void)
{
	u32 c; int n, k;
	for (n = 0; n < 256; n++) {
		c = (u32)n;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		g_crc_table[n] = c;
	}
	g_crc_table_ready = 1;
}
static u32 crc32_update(u32 crc, const void *buf, u32 len)
{
	const u8 *p = (const u8 *)buf;
	if (!g_crc_table_ready) crc32_build_table();
	while (len--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}
#define CRC32_INIT   0xFFFFFFFFu
#define crc32_finish(crc) ((crc) ^ 0xFFFFFFFFu)

// Read the next 64KB-cluster-aligned slice of the file into work_buf, tracking the
// running byte offset in *off. The FIRST read is short — only up to the next 64KB
// boundary — so every following read starts exactly on a cluster. The MS driver
// serves an aligned 64KB read ~2x faster than a straddled COMPRESS_BUF_SIZE (68KB)
// read (which spans two clusters). Returns bytes read (<=0 at EOF/error). Same total
// byte stream either way, so the CRC value is unchanged. work_buf must be >= 64KB.
static int crc_read_next(int fd, SceOff *off)
{
	u32 want = SAVE_CHUNK_SIZE - ((u32)*off & (SAVE_CHUNK_SIZE - 1));   // shrink the first read to the boundary, then 64KB
	int n = sceIoRead(fd, work_buf, (int)want);
	if (n > 0) *off += n;
	return n;
}

// ── ME-RPC mutex probe: is the ME's RPC mutex held by a thread we froze? ──
// me_wrapper.c (uofw 6.60 clone, PSP_References): every ME RPC
// (sceMeCore_driver_FA398D71) LOCKS the mutex named "SceMediaEngineRpc", kicks
// the ME, blocks on the "SceMediaEngineRpcWait" eventflag, then UNLOCKS. The
// firmware's 0x402 suspend handshake (me_wrapper sub_1026) TryLocks that mutex —
// held ⇒ veto. Per the audited suspend map, this is the ONLY descent veto whose
// condition a FROZEN game thread can wedge (syscon/ctrl/idstorage vetoes are
// kernel-side and self-clear; kernel threads are never frozen). So the single
// invariant that makes our suspend behave like a native sleep is: no game thread
// left frozen while OWNING that mutex. This probe checks the invariant directly
// via sceKernelReferMutexStatus (owner + lock count, pure READ — no lock/unlock
// side effects, no waiter handoff). Ownership is the correct test, NOT the
// thread's wait state: a holder can be frozen INSIDE the RPC while READY (wait
// already satisfied, unlock not yet run — observed on Duodecim: AUDIO MIXER TH
// frozen at st=0x8 wt=0 wid=0 still owning the mutex), which any waitId-based
// check misses. Same pattern as ms_probe_after_freeze (the MS FAT lock): probe
// after the freeze, and if a frozen thread holds the lock the caller unfreezes
// and retries — the released holder unlocks within a schedule slice (the ME
// finished long ago; only the software unlock is pending).
// Resolution at runtime (kernel object identity, no hardcoded UID — the eventflag
// UID differed across boots: 0x181CF19 vs 0x3D25533):
//   sceKernelSearchUIDbyName  SysMemForKernel  6.60/61 NID 0xE3F9C38E
//   sceKernelReferMutexStatus ThreadManForKernel 6.60/61 NID 0xA9C2CB9A
// (ARK-4 nid_660_data.c / psplibdoc_660.xml; both absent from the SDK stub libs,
// resolved via sctrlHENFindFunction like the ctrl/hprm/iofilemgr hooks.)
typedef struct {
	SceSize size;                 // set to sizeof before the call
	char    name[32];             // SCE_UID_NAME_LEN(31)+1 (uofw threadman_kernel.h)
	u32     attr;
	s32     initCount;
	s32     currentCount;         // 0 = free
	SceUID  currentOwner;         // holder thread when currentCount > 0
	s32     numWaitThreads;
} MeMutexInfo;

// Sysmem UID control block, 6.60 layout (uofw include/sysmem_kernel.h
// SceSysmemUidCB, size 36). The +12 uid / +16 name fields are hardware-proven
// on this console (v528's eventflag name-match read them via
// sceKernelGetUIDcontrolBlock); the ring fields (+4 instance ring, +8 type,
// +24 type ring) come from the same authoritative struct.
typedef struct MeUidCB {
	struct MeUidCB *parent0;      // +0  previous sibling in the instance ring
	struct MeUidCB *nextChild;    // +4  next sibling; ring is anchored at the TYPE CB
	struct MeUidCB *meta;         // +8  the TYPE control block
	SceUID          uid;          // +12
	char           *name;         // +16
	u8 childSize, size;           // +20, +21
	u16 attr;                     // +22
	struct MeUidCB *next;         // +24 type-list ring (meaningful on TYPE CBs)
	struct MeUidCB *parent1;      // +28
	void           *funcTable;    // +32
} __attribute__((packed)) MeUidCB;

// InterruptManagerForKernel (pspsdk -lpspkernel stubs; classic stable NIDs).
// The UID-tree walk below runs with interrupts suspended — that stops the
// scheduler, so no create/delete can tear the rings mid-traversal (the
// firmware's own UID search does exactly this). Prototypes match the SDK's
// pspintrman.h exactly (harmless duplicate if that header is already in).
unsigned int sceKernelCpuSuspendIntr(void);
void sceKernelCpuResumeIntr(unsigned int flags);

#define ME_EVF_CAP 64   // eventflag enumeration cap (SceMediaEngineRpcWait search) - was 256
static SceUID g_me_mutex_uid = -1;
static int (*g_refer_mutex)(SceUID mutexId, MeMutexInfo *info) = NULL;
static int g_me_probe_state = 0;   // 0 = unresolved, 1 = ok, -1 = unavailable (probe off)
SceUID g_me_evf_ids[ME_EVF_CAP];   // eventflag enumeration buffer (file-scope: keep it off the thread stack)

// Returns 0 = safe to proceed (mutex free, or probe unavailable = old behavior),
// -1 = the mutex is owned by a thread in suspended_ids[] (a frozen holder — the
// descent WOULD wedge); the caller must resume_game_threads() and retry the freeze.
// A LIVE holder (kernel/VSH thread) is waited out here (~50ms max; it clears in
// µs–ms since the threads are running) and only escalates to -1 if it persists.
int me_rpc_probe(void)
{
	int t;
	if (g_me_probe_state == 0) {
		// v531 resolution. The firmware's own sceKernelSearchUIDbyName returned
		// NOT_EXIST_ID (0x800200CD) for "SceMediaEngineRpc" on real 6.61 hardware
		// (v530 wire log) even though the object exists — v528 read the SIBLING
		// eventflag's CB name via GetUIDcontrolBlock. So by-name search is a dead
		// end (the uofw transcription of its inner walk also looks defective).
		// Walk the UID tree ourselves, from anchors that are all PROVEN on this
		// console:
		//   1. enumerate eventflags (sceKernelGetThreadmanIdList, TMID 3 — the
		//      same call this file already makes for threads with TMID 1),
		//   2. name-match "SceMediaEngineRpcWait" via sceKernelGetUIDcontrolBlock
		//      (SysMemForKernel 6.60/61 NID 0xC90B0992 — worked in v528),
		//   3. hop to its TYPE CB (+8) and iterate the type ring (+24), scanning
		//      each type's instance ring (+4) for a CB named "SceMediaEngineRpc",
		//   4. validate the hit with ReferMutexStatus echoing the name back.
		int (*getcb)(SceUID uid, MeUidCB **cb);
		u32 fgetcb, frefer;
		fgetcb = sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", 0xC90B0992);
		frefer = sctrlHENFindFunction("sceThreadManager", "ThreadManForKernel", 0xA9C2CB9A);
		if (!frefer)    // same function exported to user land; we run kernel-mode so the raw address is callable either way
			frefer = sctrlHENFindFunction("sceThreadManager", "ThreadManForUser", 0xA9C2CB9A);
		WriteDebugLogRawF("[MEPROBE] fn getcb=0x%08X refer=0x%08X", (unsigned)fgetcb, (unsigned)frefer);
		getcb = (int (*)(SceUID, MeUidCB **))fgetcb;
		g_refer_mutex = (int (*)(SceUID, MeMutexInfo *))frefer;
		if (getcb && g_refer_mutex) {
			MeUidCB *evfcb = NULL;
			int i, n = 0;
			sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_EventFlag, g_me_evf_ids, ME_EVF_CAP, &n);
			if (n > ME_EVF_CAP) n = ME_EVF_CAP;
			for (i = 0; i < n; i++) {
				MeUidCB *cb = NULL;
				if (getcb(g_me_evf_ids[i], &cb) == 0 && cb != NULL && cb->name != NULL &&
				    strcmp(cb->name, "SceMediaEngineRpcWait") == 0) { evfcb = cb; break; }
			}
			WriteDebugLogRawF("[MEPROBE] evf cb=0x%08X (of %d evfs)", (unsigned)(u32)evfcb, n);
			if (evfcb != NULL) {
				// Atomic ring traversal: interrupts off = scheduler off = no
				// concurrent UID create/delete can tear the rings under us.
				SceUID found = 0;
				unsigned int intr = sceKernelCpuSuspendIntr();
				MeUidCB *t0 = evfcb->meta, *ty = t0;
				int hops = 0, kids = 0;
				do {
					MeUidCB *c;
					for (c = ty->nextChild; c != ty && kids < 8192 && found == 0; c = c->nextChild, kids++)
						if (c->name != NULL && strcmp(c->name, "SceMediaEngineRpc") == 0)
							found = c->uid;
					ty = ty->next;
				} while (found == 0 && ty != NULL && ty != t0 && ++hops < 64);
				sceKernelCpuResumeIntr(intr);
				WriteDebugLogRawF("[MEPROBE] walk -> uid=0x%08X (hops=%d kids=%d)", (unsigned)found, hops, kids);
				if (found > 0) {
					// Validate the hit really is the ME RPC mutex (refer must
					// succeed on it as a MUTEX and echo the name back).
					MeMutexInfo mi;
					int rr;
					memset(&mi, 0, sizeof(mi)); mi.size = sizeof(mi);
					rr = g_refer_mutex(found, &mi);
					WriteDebugLogRawF("[MEPROBE] refer ret=0x%08X cnt=%d name=%.20s", (unsigned)rr, (int)mi.currentCount, mi.name);
					if (rr == 0 && strcmp(mi.name, "SceMediaEngineRpc") == 0) {
						g_me_mutex_uid = found;
						g_me_probe_state = 1;
					}
				}
			}
		}
		if (g_me_probe_state != 1) {
			g_me_probe_state = -1;
			WriteDebugLogRaw("[MEPROBE] resolve FAILED - ME mutex probe off");
		} else {
			WriteDebugLogRaw("[MEPROBE] resolved OK - probe active");
		}
	}
	if (g_me_probe_state < 0) return 0;
	for (t = 0; t < 25; t++) {                 // ~50ms grace for a LIVE holder
		MeMutexInfo mi;
		int i, frozen_owner = 0;
		memset(&mi, 0, sizeof(mi)); mi.size = sizeof(mi);
		if (g_refer_mutex(g_me_mutex_uid, &mi) < 0) return 0;   // refer failed -> don't block the op
		if (mi.currentCount == 0) return 0;                     // free -> frozen threads can't take it
		for (i = 0; i < suspended_count; i++)
			if (suspended_ids[i] == mi.currentOwner) { frozen_owner = 1; break; }
		if (frozen_owner) {
			SceKernelThreadInfo ti;
			memset(&ti, 0, sizeof(ti)); ti.size = sizeof(ti);
			sceKernelReferThreadStatus(mi.currentOwner, &ti);
			WriteDebugLogRawF("[MEPROBE] held by FROZEN %.14s tid=0x%X cnt=%d -> unfreeze+retry",
			                  ti.name, (unsigned)mi.currentOwner, (int)mi.currentCount);
			return -1;                          // caller resumes + retries the freeze
		}
		sceKernelDelayThread(2000);             // live holder: give it 2ms to unlock
	}
	WriteDebugLogRaw("[MEPROBE] LIVE holder persisted ~50ms -> retry");
	return -1;                                  // odd, but retrying is the safe move
}

// ── Per-op diagnostic profile (UART-only, DEBUG + DBG_UART gated) ──
// Emits a compact per-game snapshot so different games can be COMPARED against the
// GTA baseline directly from the wire (tested off-PC, read later). Everything here
// is register/RAM reads only — no sceIo, safe even in the frozen window. Called at
// op start (phase="op") and again right after the freeze (phase="frozen"), so the
// before/after of the game's I/O + thread state is visible.
//   [DIAG] volatile lock word (0x880E59C0): !=0 = game holds volatile (GTA does;
//          non-lockers get only the blind SUSPENDING notify).
//   [DIAG] DMAC channel-enable (PL080 EnbldChns, 0xBC900000+0x1C | 0xBCA00000+0x1C):
//          !=0 = a DMA transfer is IN FLIGHT — the game's streaming/MS DMA still
//          running while we freeze/write (the MS-contention lead).
//   [DIAG] volatile CRC over three 64KB probes (base / +2MB / +4MB-64KB): cheap
//          fingerprint to see whether a non-locking game has live volatile content
//          we don't capture (candidate cause of the "runs a few seconds then stuck").
// (diag_profile/diag_profile_frozen/diag_threads are declared in debug.h.)

// CRC32 the whole file EXCEPT the header (offset sizeof-header .. EOF). Used
// identically on save (to store) and load (to verify) — the same byte range
// either side, so any stale tail from an in-place overwrite is covered the
// same way. Leaves the file position at EOF. Returns the finished CRC.
static void prog_tick(void);   // defined with the progress panel below; ticks the bar from IO loops

static u32 crc32_file_after_header(int fd, u32 header_size)
{
	u32 crc = CRC32_INIT;
	SceOff off = (SceOff)header_size;
	int n;
	sceIoLseek(fd, off, PSP_SEEK_SET);
	while ((n = crc_read_next(fd, &off)) > 0) {
		crc = crc32_update(crc, work_buf, (u32)n);
		prog_tick();   // animate the "Verifying save" bar during the whole-file re-read
	}
	return crc32_finish(crc);
}

// Suspend ONLY the game's non-kernel (user/VSH/USB-WLAN, attr bit31) threads;
// kernel threads + dispatcher + interrupts stay ON. Returns the number of target
// threads it FAILED to freeze (0 = all frozen). The caller MUST check this and NOT
// proceed on a non-zero result — a game thread still running while we do frozen MS
// I/O is a bug, not a fallback.
//
// stable_gate: 1 = require a STABLE idle wait (same waitId twice) before suspending each
// thread, and FAIL a thread that never settles within cap_ms; avoids freezing one mid-MS-read.
// 0 = freeze on the FIRST WAITING (dispatch-off makes a READY freeze safe; used by the menu).
// cap_ms: per-thread best-effort wait budget in ms (1ms/sample). The SAVE wraps this in a
// retry loop so it uses a SHORT budget (50) and fails-fast/retries rather than stalling up to
// 4s black-screen on a threadmain busy in the game's suspend handler; the LOAD has no retry
// loop so it keeps the longer budget.
int suspend_escalating(int stable_gate, int cap_ms)
{
	SceUID ids[MAX_SUSPEND_THREADS];
	SceUID my_tid;
	int count = 0, i;
	int fails = 0;

	suspended_count = 0;
	my_tid = sceKernelGetThreadId();

	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, MAX_SUSPEND_THREADS, &count) < 0) {
		ms_test_cp(1, 0xFFFFFFFF, "GetThreadmanIdList-fail");
		return 1;   // enumeration failed: we froze NOTHING -> report a failure so callers abort (never 0)
	}

	for (i = 0; i < count; i++) {
		SceKernelThreadInfo info;
		int sret;

		if (ids[i] == my_tid)
			continue;

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferThreadStatus(ids[i], &info) < 0)
			continue;

		if (info.attr & 0x80000000) {
			// DORMANT (0x10: created-but-not-started, or finished): nothing to freeze —
			// it cannot run. sceKernelSuspendThread on it errors (0x80020198), which
			// would count as a failed freeze and abort the whole open (the modmgr
			// worker "SceModmgrStart" idles DORMANT as a user-attr thread).
			if (info.status & 0x10)
				continue;
			// Non-kernel (user/VSH/USB-WLAN) — always suspend, but only once the
			// thread has reached a STABLE idle wait (see the gate + NOTE below); else we FAIL it.
			// sceKernelSuspendThread(id) on a thread in READY (status 0x2 — just woken,
			// about to run kernel/driver code) can hang; a thread in WAITING (0x4) does
			// not have this problem. So only suspend threads observed WAITING, never READY.
			{
				int w;
				int stable = 0, cap = cap_ms;   // per-thread best-effort budget (1ms/sample); caller-provided
					u32 wst = info.status; u32 wtype = (u32)info.waitType, wid = (u32)info.waitId, prev_wid = 0xFFFFFFFF;
				// Mark which thread we're about to gate+suspend, BEFORE any of it. Safe
					// here (a re-check follows in the loop, so this MS write can't cause the
					// wake-to-READY hang described above). If a freeze deadlocks the [SUS] write below,
					// THIS [PRE] line is the last one on MS -> names the exact culprit thread
					// and the wait state we entered on. Behind the Show-Debug setting: two MS
					// appends per thread cost ~50-150ms per freeze; enable in Settings to diagnose.
					// Menu freezes (g_menu_quiet): NO MS write here — a frozen lock-holder
					// deadlocks the append itself; buffer to RAM instead and let
					// run_save_browser flush after the MS probe passes.
					if (DBG_MS()) { char pbuf[96]; sprintf(pbuf, "[PRE] %.14s st=0x%X wt=0x%X wid=0x%X", info.name, (u32)info.status, (u32)info.waitType, (u32)info.waitId); if (g_menu_quiet) gatelog_line(pbuf); else WriteDebugLogRaw(pbuf); }  // last [PRE] before a hang = the culprit thread
					// NOTE (possible improvement): the STABLE-waitId test below is a HEURISTIC for
					// "this thread is NOT mid-MS-read" (mid-read wait is brief and
					// on a different object, but it CAN be fooled by a single read that lasts longer
					// than the ~1ms sample gap (both samples land on the same read-completion object)
					// -> we could still freeze a thread holding the msstor lock and deadlock our
					// frozen sceIo*. A TRUE fix (we started digging: see EcsUmd9660DeviceFile / the
					// FATMS-msstor driver) would enumerate the actual device lock (semaphore/mutex),
					// check it is free (or find its owner), and freeze only when the MS is genuinely
					// idle -- deterministic, not a guess. Left as a heuristic for now (stable in test).
					for (w = 0; w < cap && !stable; w++) {   // poll for a stable idle wait (menu) / first WAITING (save/load)
					SceKernelThreadInfo wi;
					sceKernelDelayThread(1000);   // 1ms between samples
					memset(&wi, 0, sizeof(wi));
					wi.size = sizeof(wi);
					if (sceKernelReferThreadStatus(ids[i], &wi) < 0) break;
					wst = wi.status; wtype = (u32)wi.waitType; wid = (u32)wi.waitId;
						// A thread caught mid-MS-read is only briefly WAITING on the driver's read-completion
						// object; requiring the SAME waitId across two samples means we suspend it only on a
						// steady idle wait (no device lock held), never mid-read — the frozen-menu deadlock.
						if (!(wst & 0x04)) { prev_wid = 0xFFFFFFFF; continue; }  // not WAITING yet
						if (!stable_gate) { stable = 1; break; }                 // save/load: first WAITING is enough (game I/O already stopped, no mid-read to dodge)
						if (wid == prev_wid) { stable = 1; break; }              // menu: same wait object twice = steady idle (NOT the brief mid-read wait) -> safe to suspend
						prev_wid = wid;
				}
				// Suspend IMMEDIATELY after confirming WAITING — NO I/O between the
				// check and the call. WriteDebugLog (MS I/O, ~ms) would give the thread
				// a window to wake to READY and hang the suspend (see the READY/WAITING
				// note above). Log only after the call returns.
				// Suspend with the DISPATCHER held off (sceKernelSuspendDispatchThread):
					// sceKernelSuspendThread only HANGS when it catches a thread mid-executing
					// kernel/driver code. With dispatch off NOTHING runs but us, so the target
					// can't be running, so the suspend cannot hang mid-execution, and
					// the wake race between the check and the call is gone atomically. Interrupts
					// stay ON, so DMA/MS completion still works for our frozen sceIo*. Nothing
					// blocking may run inside this window (no sceIo/WriteDebugLog/DelayThread).
					if (stable || !stable_gate) {   // menu: only when stable. save/load: always (dispatch-off makes a READY freeze safe; I/O already stopped)
						int ds = sceKernelSuspendDispatchThread();
						sret = sceKernelSuspendThread(ids[i]);
						sceKernelResumeDispatchThread(ds);
					} else {
						sret = -1;   // menu only: never reached a STEADY idle wait (busy/streaming) -> fail; caller aborts (no mid-read freeze)
					}
				if (DBG_MS()) { char ubuf[96]; sprintf(ubuf, "[SUS] %.14s w=%d st=0x%X wt=0x%X wid=0x%X r=%d", info.name, w, (u32)wst, wtype, wid, sret); if (g_menu_quiet) gatelog_line(ubuf); else WriteDebugLogRaw(ubuf); }  // behind the Debug setting's MS bit; buffered when quiet (see [PRE])
			}
			if (sret >= 0 && suspended_count < MAX_SUSPEND_THREADS) {
				suspended_ids[suspended_count++] = ids[i];
			} else {
				fails++;   // this game thread was NOT frozen -> caller must abort, not proceed
			}
		}
		// Kernel threads are left running (dispatcher + interrupts stay ON).
	}

	// (CP1/CP2/CP4 found/suspended-count checkpoints removed — the [THR:...] dump
	// now lists every thread, which subsumes the counts. The CP1 fail variant above
	// stays for the GetThreadmanIdList error case.)
	return fails;
}

// extern
extern char umdid[24];				// disc id + "_" + 8-hex ISO-master hash

// The running game's display name (ISO volume label), filled at boot in main.c's
// OnModuleStart from the already-in-memory PVD — NEVER read from disc at menu time
// (a menu-path disc0: read hangs the freeze). Written into the game's save folder as
// title.txt so the game-folder browser can show it in front of the ID. "" if none.
char g_game_title[64] = {0};

// Capture the current framebuffer into "<path minus .bin>.thb": downsample
// 480x272 by THUMB_DS (nearest-neighbor) to THUMB_W x THUMB_H, stored 16-bit 565.
// Reuses work_buf (free here, before the RAM compress loop). Called from FreezeSave
// on a real save, while the clean game frame is still on screen.
static void write_thumbnail(const char *bin_path)
{
	void *addr; int bufw, pfmt, x, y, len;
	char thb[160];   // holds bin_path (up to ~127) + NUL; guarded below
	u16 *out = (u16 *)work_buf;
	SceUID fd;

	sceDisplayGetFrameBuf(&addr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
	if (!addr || bufw <= 0) return;

	if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
		for (y = 0; y < THUMB_H; y++) {
			volatile u32 *src = (volatile u32 *)(0xA0000000 | (u32)addr) + (y * THUMB_DS) * bufw;
			for (x = 0; x < THUMB_W; x++) out[y * THUMB_W + x] = to565(src[x * THUMB_DS]);
		}
	} else if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_5551) {
		for (y = 0; y < THUMB_H; y++) {
			volatile u16 *src = (volatile u16 *)(0xA0000000 | (u32)addr) + (y * THUMB_DS) * bufw;
			for (x = 0; x < THUMB_W; x++) out[y * THUMB_W + x] = c5551_to565(src[x * THUMB_DS]);
		}
	} else if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_4444) {
		for (y = 0; y < THUMB_H; y++) {
			volatile u16 *src = (volatile u16 *)(0xA0000000 | (u32)addr) + (y * THUMB_DS) * bufw;
			for (x = 0; x < THUMB_W; x++) out[y * THUMB_W + x] = c4444_to565(src[x * THUMB_DS]);
		}
	} else {   // 565: already the stored format
		for (y = 0; y < THUMB_H; y++) {
			volatile u16 *src = (volatile u16 *)(0xA0000000 | (u32)addr) + (y * THUMB_DS) * bufw;
			for (x = 0; x < THUMB_W; x++) out[y * THUMB_W + x] = src[x * THUMB_DS];
		}
	}

	len = (int)strlen(bin_path);
	if (len < 4 || len + 1 > (int)sizeof(thb)) return;   // guard: never overrun thb
	memcpy(thb, bin_path, len + 1);
	memcpy(thb + len - 4, ".thb", 5);   // ".bin" -> ".thb"
	fd = sceIoOpen(thb, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, out, THUMB_W * THUMB_H * 2); sceIoClose(fd); }
}

// Capture the FULL framebuffer (480x272) into "<name>.scr" as 16-bit 565, for the
// fullscreen preview. Streamed through work_buf (the full image is ~255KB > work_buf,
// so flush per batch of rows). SAVE-only, same point as the thumbnail.
static void write_screenshot(const char *bin_path)
{
	void *addr; int bufw, pfmt, x, y, len;
	char scr[160];   // holds bin_path (up to ~127) + NUL; guarded below
	u16 *out = (u16 *)work_buf;
	int cap = (int)(COMPRESS_BUF_SIZE / 2);   // u16 capacity of work_buf
	int n = 0;
	SceUID fd;

	sceDisplayGetFrameBuf(&addr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
	if (!addr || bufw <= 0) return;

	len = (int)strlen(bin_path);
	if (len < 4 || len + 1 > (int)sizeof(scr)) return;   // guard: never overrun scr
	memcpy(scr, bin_path, len + 1);
	memcpy(scr + len - 4, ".scr", 5);
	fd = sceIoOpen(scr, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0) return;

	for (y = 0; y < 272; y++) {
		if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
			volatile u32 *src = (volatile u32 *)(0xA0000000 | (u32)addr) + y * bufw;
			for (x = 0; x < 480; x++) out[n++] = to565(src[x]);
		} else if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_5551) {
			volatile u16 *src = (volatile u16 *)(0xA0000000 | (u32)addr) + y * bufw;
			for (x = 0; x < 480; x++) out[n++] = c5551_to565(src[x]);
		} else if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_4444) {
			volatile u16 *src = (volatile u16 *)(0xA0000000 | (u32)addr) + y * bufw;
			for (x = 0; x < 480; x++) out[n++] = c4444_to565(src[x]);
		} else {   // 565: already the stored format
			volatile u16 *src = (volatile u16 *)(0xA0000000 | (u32)addr) + y * bufw;
			for (x = 0; x < 480; x++) out[n++] = src[x];
		}
		if (n + 480 > cap) { sceIoWrite(fd, out, n * 2); n = 0; }
	}
	if (n) sceIoWrite(fd, out, n * 2);
	sceIoClose(fd);
}

// Copy this save's preview thumbnail to <gamedir>/Game.thb so the game-folder list has
// an instant per-game image — always the newest save's thumb (overwritten each save).
// Real-save only. Uses work_buf (free here). No-op if the .thb is missing.
void copy_game_thumb(const char *bin_path)
{
	char thb[128], game[128]; int len, i; SceUID fs, fd; int n;
	len = (int)strlen(bin_path);
	if (len < 5 || len + 1 > (int)sizeof(thb)) return;   // guard: never overrun thb/game
	memcpy(thb, bin_path, len + 1);
	memcpy(thb + len - 4, ".thb", 5);                   // <save>.bin -> <save>.thb
	memcpy(game, bin_path, len + 1);
	for (i = len - 1; i >= 0 && game[i] != '/'; i--) ;  // find the dir separator
	if (i < 0) return;
	strcpy(game + i + 1, "Game.thb");                   // <dir>/Game.thb
	fs = sceIoOpen(thb, PSP_O_RDONLY, 0);
	if (fs < 0) return;
	n = sceIoRead(fs, work_buf, THUMB_W * THUMB_H * 2);
	sceIoClose(fs);
	if (n <= 0) return;
	fd = sceIoOpen(game, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, work_buf, n); sceIoClose(fd); }
}

// ── Shared kernel-mode controller polling (all menu/prompt UI) ──
// One unmasked (k1=0) peek through the real controller service — the single
// wrapper every menu/prompt pad read goes through. Returns the peek's own
// result (>0 = a sample was read).
int kpeek(SceCtrlData *pad)
{
	int (*peek)(SceCtrlData *, int) = g_real_ctrl_peek ? g_real_ctrl_peek
	                                                    : sceCtrlPeekBufferPositive;
	int k1 = pspSdkSetK1(0);
	int res = peek(pad, 1);
	pspSdkSetK1(k1);
	return res;
}

// Block until a button in `mask` shows a rising edge; returns the masked pressed
// bits. ~25Hz poll (40ms), the cadence all the prompt loops use. Always-set level
// bits (e.g. PSP_CTRL_MS in the unmasked read) can't false-trigger: only edges
// against the entry state count.
u32 wait_button_edge(u32 mask)
{
	SceCtrlData pad;
	u32 prev, pressed;
	kpeek(&pad); prev = pad.Buttons;
	for (;;) {
		sceKernelDelayThread(40000);
		kpeek(&pad);
		pressed = (pad.Buttons & ~prev) & mask;
		prev = pad.Buttons;
		if (pressed) return pressed;
	}
}

// Wait for the given buttons to be fully released — confirm prompts use this so
// the answer press's stale state doesn't leak into the caller's next edge poll.
void wait_release(u32 mask)
{
	SceCtrlData pad;
	do { kpeek(&pad); sceKernelDelayThread(20000); } while (pad.Buttons & mask);
}

// Poll until no controller buttons are held. Called BEFORE resuming the game on a
// menu close / continue so the closing button (O, X) isn't still down when the game
// wakes — otherwise the game sees that press as its own input. (NOTE is masked from
// the game, but we wait for it too.) Game is frozen here, so this can't stall input.
void wait_buttons_up(void)
{
	SceCtrlData pad;
	// Only real buttons — the unmasked (k1=0) read also carries status bits, and
	// PSP_CTRL_MS (Memory Stick present) is ALWAYS set (we run from the MS!), so a
	// plain Buttons==0 test would never clear and hang here.
	const u32 mask = PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_RIGHT |
	                 PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER |
	                 PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE |
	                 PSP_CTRL_NOTE;
	for (;;) {
		kpeek(&pad);
		if ((pad.Buttons & mask) == 0) break;
		sceKernelDelayThread(15000);
	}
	// NOTE: the SECOND button leak (latch-reading games, e.g. Ratchet & Clank) is
	// NOT handled here. A kernel-side sceCtrlReadLatch (k1=0) drains the KERNEL latch,
	// but the game reads the USER latch (uofw ctrl.c: sceCtrlReadLatch picks user- vs
	// kernel-mode state by pspK1IsUserMode). The user latch can only be drained from
	// the game's own user-mode call — so it's handled by hooking sceCtrlReadLatch/
	// PeekLatch (sceCtrl*LatchPatched) + the g_suppress_latch window at the save trigger.
}

// ── Live status/progress panel (always shown, independent of the debug setting) ──
// A status panel sitting at the old banner row ("Saving"/"Loading" title at row 17)
// plus a growing list of steps below it, each shown as "<step> ...." while running
// and rewritten to "<step>  X.XX MB/s" when it finishes — a running progress view
// that doubles as a free MS speed readout. The background is SEMI-TRANSPARENT: the
// game band is dimmed ~50% (see dbg_dim_band) rather than blacked out, so the game
// stays faintly visible behind it. Drawn to BOTH captured framebuffers via
// dbg_print_all (the path that's safe pre- AND post-suspend). State lives in RAM and
// the text is REDRAWN on every change; the band is re-dimmed only when the panel is
// re-established on a fresh frame (prog_begin / after the VRAM restore repaints over
// it) so it never darkens progressively. Compiled in for every build.
#define PROG_MAX    8
#define PANEL_TOP   14           // top row of the card (title lands on PANEL_TOP+1). Card sits low,
                                 // near the screen bottom rather than mid-screen (see CARD_BOTTOM_Y).
#define STEP_GAP    2            // rows per step line (2 = one blank row between lines for breathing room)
#define STEP_TOP    3            // rows below PANEL_TOP where the first step sits (title +1, one blank row).
#define STEP_AREA_ROWS 15        // step lines + the CRC-verify line must land within PANEL_TOP..+15,
                                 // clear of the gap above the bottom bar (6 steps max: 3+5*2=13, CRC=+15).
static char g_prog_title[16] = "";
static char g_prog_label[PROG_MAX][40];
int  g_prog_mbps[PROG_MAX];   // -2 = running ("...."), >=0 = MB/s * 100
static int  g_prog_n = 0;
static u64  g_prog_t0 = 0;
// ── Determinate progress-bar state (weighted-segment model) ──
// Each VISIBLE step owns a fixed PERMILLE WEIGHT (0..1000 per op) reflecting its real share
// of elapsed time — NOT an equal 1/line split (that put RAM+VRAM save, "most of the visible
// step lines" but only ~25% of real time, at ~67-75% before the suspend even began). The
// CPU-off firmware-suspend window itself is NOT a segment and earns NO weight of its own —
// it can't tick (no CPU) or be meaningfully animated, so giving it a weight only means a
// jarring jump the instant it ends. Instead the pre-suspend steps are weighted to land
// around 500 (50%) and simply HOLD there for the whole suspend (nothing runs, nothing
// changes the fraction); the post-suspend steps then continue smoothly from 500 to 1000.
// g_prog_base_pm = permille already earned (segments finished). While a segment runs,
// g_prog_seg_pm is its weight, time-interpolated toward base+seg over a FIXED budget
// (g_prog_seg_ms), driven by prog_tick() calls in the IO loops. Runs LONGER than budget ->
// bar clamps just short of the next checkpoint and waits; finishes EARLY -> prog_seg_done
// snaps it forward.
//   On LOAD the kernel apply rolls plugin BSS back to save-time, so the resume tail can't
//   reuse a live segment across the suspend — it re-seeds g_prog_base_pm directly to
//   "verify + read-kernel already earned" (like the g_lc rate carry does for the MB/s
//   figures), landing at the same ~50% the SAVE side settles at pre-suspend.
static int  g_prog_active = 0;     // 1 once prog_begin has armed the bar for this op
static int  g_prog_base_pm = 0;    // permille (0..1000) already earned (segments finished)
static int  g_prog_seg_pm = 0;     // permille weight of the currently-running segment (0 = none)
static u64  g_prog_seg_t0 = 0;     // wall-clock at the running segment's start
static u32  g_prog_seg_ms = 0;     // that segment's fixed time budget (ms)
static u64  g_prog_bar_last = 0;   // prog_tick throttle: last bar redraw (µs)

// Per-step fixed time budgets (ms), rounded from measured GTA:VCS timings (uart_log). The
// bar paces to these; real runs that differ just wait/jump at the checkpoint.
#define PB_RAM_MS      2400   // Game RAM compress+write (~2.4s)
#define PB_VRAM_MS      250   // VRAM compress+write (~0.19s)
#define PB_KERNEL_MS    500   // kernel snapshot compress+write (~0.46s)
#define PB_RESTORE_MS  2400   // RAM+VRAM restore read+decompress (~2.4s)
#define PB_VERIFY_MS   1600   // load CRC verify re-read (~1.6s)
#define PB_READKN_MS    350   // load kernel read+decompress (~0.33s)
#define PROG_TICK_US  33000   // bar redraw throttle (~30 Hz)

// Segment weights, permille of the WHOLE operation (each table sums to 1000, split evenly
// 500/500 across the suspend so pre-suspend steps land around 50% and hold, post-suspend
// steps fill the rest — no weight is assigned to the suspend itself, see above). Ratios
// WITHIN each half are kept proportional to the real measured shares (uart_log).
#define W_SAVE_RAM       460   // RAM compress+write — pre-suspend
#define W_SAVE_VRAM       40   // VRAM compress+write — pre-suspend (RAM+VRAM = 500)
#define W_SAVE_KERNEL     90   // kernel compress+write — post-suspend
#define W_SAVE_RESTORE   410   // RAM+VRAM restore — post-suspend (kernel+restore = 500)
#define W_LOAD_VERIFY    410   // whole-file CRC verify — pre-suspend
#define W_LOAD_READKN     90   // kernel read+decompress — pre-suspend (verify+read = 500)
#define W_LOAD_RESTORE   500   // RAM+VRAM restore — post-suspend (the only post-suspend step)

// Centered operation "card" (over the frozen game frame): x70..410 (~15% narrower than the
// original w400). Sits near the SCREEN BOTTOM: CARD_BOTTOM_Y below places the progress bar
// (with the op-time text) just above the screen edge, fused to the card's own bottom edge
// (no separate border between them).
#define CARD_X   70
#define CARD_W   340
// Bottom progress bar: sized to just fit its ONE text row (8px) + BAR_PAD px above/below —
// no more empty band than that. BAR_TEXT_ROW is the absolute row (the same 8px font grid
// every dbg_text call is quantized to) the centered op-time text lands on; the bar's pixel
// box is built AROUND it with BAR_PAD margin, so the box itself isn't grid-locked, only the
// text inside it is.
#define BAR_PAD       3                          // px padding above/below the op-time text
#define BAR_H         (8 + 2 * BAR_PAD)          // 14px: just the text row + a few px each side
#define BAR_TEXT_ROW  (PANEL_TOP + 17)           // row the centered op-time text sits on
#define CARD_TOP_Y    (PANEL_TOP * 8)
#define CARD_BOTTOM_Y (BAR_TEXT_ROW * 8 + 8 + BAR_PAD)   // bar bottom = card bottom (seamless);
                                 // lands ~13px above the 272px screen edge — the closest the
                                 // 8px text-row grid allows to the requested 10px gap.

// Begin the currently-running segment: `weight_pm` permille of the whole op, paced to
// `budget_ms`. weight_pm=0 is valid (the suspend window ticks nothing — see prog_seg_done).
static void prog_seg_begin(int weight_pm, u32 budget_ms)
{
	g_prog_seg_pm = weight_pm;
	g_prog_seg_ms = budget_ms;
	g_prog_t0 = g_prog_seg_t0 = now_us();   // g_prog_t0 also feeds the MB/s readout
}
// Finish the running segment: its weight becomes earned (passed the checkpoint).
static void prog_seg_done(void)
{
	g_prog_base_pm += g_prog_seg_pm;
	g_prog_seg_pm   = 0;
}
// Bar fill as permille (0..1000) of the whole op: finished segments' weights plus the
// running segment's time-interpolated portion, capped at 0.97 of ITS weight so the bar
// visibly WAITS just short of the next checkpoint until prog_seg_done snaps it across.
static int prog_frac_permille(void)
{
	int num;
	if (!g_prog_active) return 0;
	num = g_prog_base_pm;
	if (g_prog_seg_pm > 0 && g_prog_seg_ms > 0) {
		u32 ms = (u32)((now_us() - g_prog_seg_t0) / 1000);
		u32 pk = (ms >= g_prog_seg_ms) ? 970u : (ms * 970u / g_prog_seg_ms);   // per-mille of the segment's OWN weight
		num += g_prog_seg_pm * (int)pk / 1000;
	}
	return num > 1000 ? 1000 : num;
}

// (dbg_dim_band — the old 50%-dim semi-transparent backdrop — was removed: the
// panel is now always the opaque menu color BR_BG via dbg_fill_band_color.)


// Redraw the panel from RAM state. redim: 0 = text only (per-step updates, which must
// NOT re-establish the backdrop or the band would darken each time); 1 = re-dim the
// game band first (a fresh semi-transparent backdrop); 2 = fill the band SOLID black
// Progress panel draw: title centered on row 17, transparent text.
// Uses stable column (widest state "<label> XX.XX MB/s") so value digits paint over old text.
// Determinate progress bar: a band fused flush to the card's bottom edge, filled left->right
// by prog_frac_permille (earned segment weight + a time-interpolated slice of the running
// one). Accent tracks the op (bright blue Save / bright green Load); the final op-time text is
// centered on it. Drawn to every captured buffer (pre-/post-suspend safe). No-op until prog_begin.
static u32 prog_accent(void)
{
	return BR_AZURE;   // Save AND Load both bright blue, so the bright-green save/load-time
	                    // text (drawn straight on the bar, no backing chip) always has contrast.
}
// The operation "card": BR_CARD panel with a per-op accent left-stripe + hairline border,
// centered over the frozen game frame. `accent` overrides the op color (failure = red).
static void prog_card(u32 accent)
{
	int x = CARD_X, y = CARD_TOP_Y, w = CARD_W, h = CARD_BOTTOM_Y - CARD_TOP_Y;   // <=272, no fb overrun
	// Flat drop-shadow: a 3px near-black band peeking out below + right, offset so the card
	// reads as lifted off the frozen game frame (shadow fits: x+w+3=443<480, y+h+3=243<272).
	dbg_fill_rect_all(x + 3, y + h, w, 3, BR_SHADOW);         // bottom
	dbg_fill_rect_all(x + w, y + 3, 3, h, BR_SHADOW);         // right
	// Card surface + full gray hairline outline.
	dbg_fill_rect_all(x, y, w, h, BR_CARD);
	dbg_fill_rect_all(x, y, w, 1, BR_LINE);                   // top
	dbg_fill_rect_all(x, y + h - 1, w, 1, BR_LINE);           // bottom
	dbg_fill_rect_all(x + w - 1, y, 1, h, BR_LINE);           // right
	dbg_fill_rect_all(x, y, 4, h, accent);                    // left accent stripe (over the left edge)
}
static void prog_bar(void)
{
	int barTop = BAR_TEXT_ROW * 8 - BAR_PAD;   // BAR_H-tall band centered on the op-time text row
	int bx = CARD_X, bw = CARD_W, fw;
	if (!g_prog_active) return;
	dbg_fill_rect_all(bx, barTop, bw, 1, BR_LINE);              // separator hairline above the band
	dbg_fill_rect_all(bx, barTop + 1, bw, BAR_H - 1, BR_BG);    // dark track (full card width)
	fw = bw * prog_frac_permille() / 1000;
	if (fw > bw) fw = bw;
	if (fw > 0) dbg_fill_rect_all(bx, barTop + 1, fw, BAR_H - 1, prog_accent());   // accent fill
}
// Called from the IO loops to move the bar between checkpoints. Throttled to ~PROG_TICK_US
// so most calls are just a time compare + return; only every ~33ms does it repaint the bar.
static void prog_tick(void)
{
	u64 nowu;
	if (!g_prog_active) return;
	nowu = now_us();
	if (nowu - g_prog_bar_last < PROG_TICK_US) return;
	g_prog_bar_last = nowu;
	prog_bar();
}
static void prog_draw(int redim)
{
	int i;
	// Backdrop is ALWAYS the opaque menu color (BR_BG) — same for the live progress
	// (redim 1) and the final readout (redim 2). (Was: redim 1 = 50% dim game, redim
	// 2 = solid black.) redim 0 = text-only refresh, no backdrop repaint.
	if (redim == 1 || redim == 2) prog_card(prog_accent());   // centered card over the frozen game
	dbg_transparent = 1;
	dbg_fg = prog_accent();                                   // accent title — bright blue, both ops
	dbg_row = PANEL_TOP + 1;
	dbg_col = (60 - (int)strlen(g_prog_title)) / 2; if (dbg_col < 0) dbg_col = 0;
	dbg_print_all(g_prog_title);
	dbg_fg = 0xFFFFFFFF;                                       // step lines stay white
	for (i = 0; i < g_prog_n && (STEP_TOP + i * STEP_GAP) < STEP_AREA_ROWS; i++) {
		char val[16];
		int row  = PANEL_TOP + STEP_TOP + i * STEP_GAP;
		int lcol = CARD_X / 8 + 4;              // label: left-aligned, generous margin past the accent stripe
		int rend = (CARD_X + CARD_W) / 8 - 3;   // MB/s: right-aligned, generous margin from the card edge
		if (g_prog_mbps[i] == -2) strcpy(val, "....");
		else sprintf(val, "%d.%02d MB/s", g_prog_mbps[i] / 100, g_prog_mbps[i] % 100);
		// Clear the row's whole text span first: "...." -> "X.XX MB/s" is a DIFFERENT glyph
		// shape, not a superset of the dots' lit pixels, so a transparent redraw alone left
		// leftover dot pixels on screen wherever the new glyphs don't happen to cover them.
		dbg_fill_rect_all(lcol * 8, row * 8, (rend - lcol) * 8, 8, BR_CARD);
		dbg_row = row; dbg_col = lcol; dbg_print_all(g_prog_label[i]);           // label left
		dbg_row = row; dbg_col = rend - (int)strlen(val); dbg_print_all(val);    // MB/s right
	}
	dbg_transparent = 0;
	prog_bar();
}
static void prog_begin(const char *title)   // start a fresh panel; dims the band once
{
	strncpy(g_prog_title, title, sizeof(g_prog_title) - 1); g_prog_title[sizeof(g_prog_title) - 1] = 0;
	g_prog_n = 0;
	g_prog_active   = 1;
	g_prog_base_pm  = 0;
	g_prog_seg_pm   = 0;
	g_prog_bar_last = 0;
	prog_draw(1);
}
static void prog_step(const char *label, int weight_pm, u32 budget_ms)   // begin a 1-line step
{
	if (g_prog_n >= PROG_MAX) return;
	strncpy(g_prog_label[g_prog_n], label, 39); g_prog_label[g_prog_n][39] = 0;
	g_prog_mbps[g_prog_n] = -2;
	g_prog_n++;
	prog_seg_begin(weight_pm, budget_ms);
	prog_draw(0);
}
// MB/s * 100 from bytes / elapsed-µs, in pure 32-bit math (no 64-bit divide ->
// no __udivdi3/__clz_tab linked, ~1.7KB saved). The exact rate is
// bytes*1e8 / (us*1048576); reworked to stay in u32 as (bytes>>10) * 97656 / us:
// bytes*1e8/(us*2^20) = (bytes/1024)*(1e8/1024)/us, and (bytes>>10)*97656 peaks at
// 24576*97656 ~ 2.4e9 < 2^32. The sub-KB truncation + 97656-vs-97656.25 rounding are
// negligible for a displayed rate. (elapsed µs of any section fits u32: a whole save
// is ~13e6 µs << 4.29e9.)
static int prog_mbps100(u32 bytes, u32 us)
{
	return us ? (int)(((u32)(bytes >> 10) * 97656u) / us) : 0;
}
static void prog_done(u32 bytes)             // finish the current step with a bytes/elapsed MB/s rate
{
	if (g_prog_n == 0) return;
	g_prog_mbps[g_prog_n - 1] = prog_mbps100(bytes, (u32)(now_us() - g_prog_t0));
	prog_seg_done();
	prog_draw(0);
}
// Like prog_done but re-dims the band first — used after the VRAM restore, which
// repaints the framebuffer over the panel; this re-establishes the backdrop and
// redraws every step line on top of the freshly-restored game frame.
static void prog_done_redim(u32 bytes)
{
	if (g_prog_n == 0) return;
	g_prog_mbps[g_prog_n - 1] = prog_mbps100(bytes, (u32)(now_us() - g_prog_t0));
	prog_seg_done();
	prog_draw(1);
}
// Begin a step that will be shown as TWO lines (e.g. a compress line + a write
// line), BOTH as running "<label> ...." right away — so the labels the user sees
// during the wait are the SAME as the finished ones (no jump from "Saving X" to
// the split). prog_fill2 fills in the two rates when the step completes.
static void prog_step2(const char *lblA, const char *lblB, int weight_pm, u32 budget_ms)
{
	if (g_prog_n + 2 > PROG_MAX) return;
	strncpy(g_prog_label[g_prog_n], lblA, 39); g_prog_label[g_prog_n][39] = 0; g_prog_mbps[g_prog_n] = -2; g_prog_n++;
	strncpy(g_prog_label[g_prog_n], lblB, 39); g_prog_label[g_prog_n][39] = 0; g_prog_mbps[g_prog_n] = -2; g_prog_n++;
	prog_seg_begin(weight_pm, budget_ms);
	prog_draw(0);
}
// Fill in the rates of the two lines a prog_step2 opened (compress on the
// UNCOMPRESSED input; write on the bytes actually written — the honest throughput
// for each), keeping the labels put.
static void prog_fill2(u32 bytesA, u32 usA, u32 bytesB, u32 usB)
{
	if (g_prog_n < 2) return;
	g_prog_mbps[g_prog_n - 2] = prog_mbps100(bytesA, usA);
	g_prog_mbps[g_prog_n - 1] = prog_mbps100(bytesB, usB);
	prog_seg_done();
	prog_draw(0);
}
// Finish the current step with an explicit byte count / elapsed-µs (single line,
// no split — the Fast/uncompressed path where there's nothing to split off).
static void prog_done_us(const char *label, u32 bytes, u32 us)
{
	if (g_prog_n == 0) return;
	strncpy(g_prog_label[g_prog_n - 1], label, 39); g_prog_label[g_prog_n - 1][39] = 0;
	g_prog_mbps[g_prog_n - 1] = prog_mbps100(bytes, us);
	prog_seg_done();
	prog_draw(0);
}

// ────────────────────────────────────────────────────────────
// ────────────────────────────────────────────────────────────
// Stage 1 — SAVE: capture Main RAM + VRAM + GE context + game
// thread contexts, streamed to MS via plain sceIo* under the
// partial freeze from suspend_escalating() (thread suspension, not an
// IE-bit clear — see that function's header comment).
//
// suspend_escalating() suspends every non-kernel (user/VSH) thread; the
// kernel-thread branches in its switch are currently unreachable (see the
// note above suspend_escalating). Thread CONTEXT capture is narrower still
// (exact PSP_THREAD_ATTR_USER) — kernel/system threads aren't game state,
// we only need them schedulable, not captured.
//
// This function only reads each thread's context via ThreadManForKernel_2D69D086
// and writes the blob to the save file. Nothing reads it back on a load anymore:
// the M5 kernel apply restores the save-time thread states wholesale (the TCBs
// live in the applied kernel image), so the blob is diagnostic/forward-compat
// data. (The old baseline load's per-thread thContext round-trip went with the
// removed restore_kernel==0 path — see git history.)
// ────────────────────────────────────────────────────────────

// Run the firmware's suspend QUERY poll ourselves — the exact first check the
// power thread does before it sleeps (reversed from scePower: it dispatches
// SCE_SUSPEND_EVENTS/0x100 with break_nonzero=1, and a handler returning nonzero
// cancels the suspend). Returns 1 if any registered handler VETOES (a driver is
// busy — e.g. the UMD mid-stream), 0 if everyone is ready. We always follow with
// CANCELLATION (0x101) so handlers don't think a real suspend is in flight.
static int probe_suspend_query(void)
{
	u8 payload[64];
	int result = 0, r;
	memset(payload, 0, sizeof(payload));
	*(u32 *)payload = sizeof(payload);   // SceSysEventSuspendPayload.size
	r = sceKernelSysEventDispatch(0xFF00, 0x100, "q", payload, &result, 1, NULL); // QUERY
	sceKernelSysEventDispatch(0xFF00, 0x101, "q", payload, NULL, 0, NULL);        // CANCELLATION
	return (r < 0 || result != 0) ? 1 : 0;
}

// ── Cooperative volatile-memory release via the game's own power callback ──
// Instead of force-unlocking volatile memory ourselves, ASK the game to do it: fire its
// power callback with SUSPENDING (exactly what the firmware does for a real sleep). A
// cooperating game unlocks volatile inside its handler -> scePower's +36 flag clears and
// the suspend can sleep, AND the game knows to rebuild that scratch on resume — so we
// never have to preserve its 4MB. Must run BEFORE we freeze the game (its callback thread
// has to be alive to process the notification). Power callbacks live in scePower's table
// at 0x880E5780 (32 slots, 16 bytes each, +0 = callback UID).
static SceUID g_game_pcbs[8];
int    g_game_pcb_count;

#define HWREG(a) (*(volatile u32 *)(a))

// Returns 0 if the game was notified / volatile released (or there was nothing to
// do), -1 on 5s volatile-release timeout (=> abort).
int cooperative_volmem_release(const SceUID *game_tids, int tcount)
{
	int slot, i, j;

	// v522: REVERTED v472's unconditional SUSPENDING notify — back to the v471 rule.
	// v472 sent PSP_POWER_CB_SUSPENDING to EVERY game with a power callback, incl.
	// non-volatile-locking games. That BROKE Pirates (bisected: v471 works, v472 breaks):
	// its ME/audio half-parks on SUSPENDING and our freeze ~300ms later catches it mid-
	// park -> dead sound + game freeze after save. So a non-locking game (Pirates / Ridge
	// Racer) is now left alone (no SUSPENDING); only a game that LOCKED volatile memory
	// (GTA, Dissidia) gets the release protocol below. The ME-RPC veto (any game) is
	// prevented at freeze time by me_rpc_probe (the v524 rescue is gone).
	g_game_pcb_count = 0;                              // reset every attempt (also gates the 4MB kernel window)
	if (*(volatile u32 *)0x880E59C0 == 0) return 0;   // volatile not locked -> nothing to do

	// Collect the power callbacks owned by a game thread.
	for (slot = 0; slot < 32; slot++) {
		volatile SceUID *entry = (volatile SceUID *)(0x880E5780 + slot * 16);
		SceUID cbid = entry[0];
		SceKernelCallbackInfo info;
		if (cbid <= 0) continue;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferCallbackStatus(cbid, &info) < 0) continue;
		for (j = 0; j < tcount; j++) {
			if (info.threadId == game_tids[j]) {
				if (g_game_pcb_count < 8) g_game_pcbs[g_game_pcb_count++] = cbid;
				break;
			}
		}
	}
	ms_test_cp(9, (u32)g_game_pcb_count, "game-power-cbs-found");

	// Volatile MPU nibbles around the release — pins down who zeroes them and
	// when (v396 probe: they read 0x00000000 = all-denied by save time, which
	// was the CP13 write freeze; boot state is 0xF/0xC). Ad-hoc diagnostic, not
	// routed through DBG_MS (WriteDebugLogHexRaw gates on DBG_MS() itself).
	WriteDebugLogHexRaw("[VOLMEM] pre-release BC000008=", HWREG(0xBC000008));

	// Ask the game to suspend (release volatile), then poll +36 until it clears.
	for (i = 0; i < g_game_pcb_count; i++)
		sceKernelNotifyCallback(g_game_pcbs[i], PSP_POWER_CB_SUSPENDING);
	for (i = 0; i < 1000; i++) {                       // 1000 * 5ms = 5s budget; 5ms poll detects release ~10x sooner
		if (*(volatile u32 *)0x880E59C0 == 0) break;
		sceKernelDelayThread(5000);
	}
	WriteDebugLogHexRaw("[VOLMEM] post-release BC000008=", HWREG(0xBC000008));
	ms_test_cp(10, (u32)i, (i < 1000) ? "volmem-released-by-game(*5ms)" : "volmem-NOT-released-ABORT");
	return (i < 100) ? 0 : -1;
}

// ────────────────────────────────────────────────────────────
// Checked write: returns 1 only if the FULL length was written. A short write
// (Memory Stick full, card error) must not be silently treated as success — the
// save path ANDs this into io_ok and refuses to certify (CRC) the file if it fails.
static int io_write_all(SceUID fd, const void *buf, int len)
{
	return sceIoWrite(fd, buf, len) == len;
}

// ── Cluster-aligned flush ───────────────────────────────────────────────────
// The Memory Stick's FAT allocation unit is 64KB (== SAVE_CHUNK_SIZE): a write
// whose start offset OR length isn't a 64KB multiple forces the card into a
// read-modify-write on the straddled cluster. Because compressed records pack
// back-to-back, an unaligned flush length also shifts the NEXT flush's start
// offset, so the penalty cascades through the whole section. Pad each flush up
// to the next 64KB so every flush both starts and ends on a cluster boundary
// (the section start is aligned separately). The pad is a self-describing
// record — [uncomp_size=0][comp_size=pad_bytes] then pad_bytes of junk — which
// the restore reader recognises by uncomp_size==0 and skips, so the extra bytes
// are never parsed as real data. Returns 1 only on a full write (io_write_all).
// The caller MUST keep fill <= IOBUF_SIZE - SAVE_CHUNK_SIZE (clamp the flush
// threshold) so the padded length can't overflow the buffer.
#define SAVE_ALIGN       SAVE_CHUNK_SIZE                 // 64KB cluster/alignment unit
#define SAVE_ALIGN_MASK  ((u32)(SAVE_ALIGN - 1))
static int iobuf_flush_padded(SceUID fd, u8 *iobuf, u32 fill)
{
	// Round (fill + 8-byte pad header) up to the next 64KB; the pad record's
	// data fills the remainder. fill+8 already on a boundary -> zero-length skip.
	u32 aligned = (fill + 8 + SAVE_ALIGN_MASK) & ~SAVE_ALIGN_MASK;
	if (aligned > IOBUF_SIZE) aligned = fill;   // safety net: never overflow the buffer
	if (aligned > fill) {
		u32 zero = 0, pad = aligned - fill - 8;
		memcpy(iobuf + fill + 0, &zero, 4);     // uncomp_size = 0  -> padding marker
		memcpy(iobuf + fill + 4, &pad,  4);     // comp_size  = bytes to skip
	}
	return io_write_all(fd, iobuf, (int)aligned);
}

// File offset of the first data (RAM) section: the header is padded up to the
// next 64KB so the section — and therefore its first flush — starts on a cluster
// boundary. header[11] carries the true VRAM offset for these aligned saves and
// is 0 in older saves, so it doubles as the "aligned format" discriminator on
// load (see FreezeLoad / FreezeSave-tail read paths).
#define RAM_SECTION_OFFSET \
	((SceOff)((SAVE_HEADER_WORDS * (u32)sizeof(u32) + SAVE_ALIGN_MASK) & ~SAVE_ALIGN_MASK))

// ── Per-chunk encode / decode (compression optional) ────────────────────────
// A record is [uncomp_size=SAVE_CHUNK_SIZE][comp_size][data]. comp_size ==
// SAVE_CHUNK_SIZE marks the chunk stored RAW (Fast mode, or a chunk FastLZ
// couldn't shrink); comp_size < SAVE_CHUNK_SIZE is FastLZ output. The two never
// collide — FastLZ output is < 64KB only when a match saved bytes, and we force
// raw whenever it doesn't shrink — so decode needs no mode flag and any save
// loads on any build. encode also stops the old waste of storing an EXPANDED
// (>64KB) copy of an incompressible chunk. g_compress selects the mode.
static u32 encode_chunk(const void *src, void *dst)   // returns stored size (== SAVE_CHUNK_SIZE means raw)
{
	if (g_compress) {
		u32 cs = (u32)fastlz_compress_level(1, src, (int)SAVE_CHUNK_SIZE, dst);
		if (cs > 0 && cs < SAVE_CHUNK_SIZE) return cs;   // shrank -> keep compressed
	}
	memcpy(dst, src, SAVE_CHUNK_SIZE);                    // Fast mode OR incompressible -> raw
	return SAVE_CHUNK_SIZE;
}
static int decode_chunk(const void *src, u32 comp_size, void *dst)   // returns bytes produced
{
	if (comp_size == SAVE_CHUNK_SIZE) { memcpy(dst, src, SAVE_CHUNK_SIZE); return (int)SAVE_CHUNK_SIZE; }
	return fastlz_decompress(src, (int)comp_size, dst, (int)SAVE_CHUNK_SIZE);
}

// ── Direct (uncompressed) region I/O — Fast mode skips the IOBUF entirely ────
// With compression off there's nothing to transform, so instead of copying game
// RAM/VRAM/staging into the IOBUF and writing records, DMA the region straight
// to/from the file. The region is already contiguous and 64KB-aligned, so every
// write is cluster-aligned — no IOBUF, no per-chunk headers, no padding. sceIo*
// handles the transfer buffer's cache the same way it does for work_buf; the one
// extra call is a WritebackInvalidate (game RAM can hold DIRTY lines from still-running game).
// DMA write reads live data, and drop stale lines around a DMA read so a later
// ClearCaches writeback can't clobber the freshly-read bytes.
static int write_region_direct(SceUID fd, u32 base, u32 size)
{
	u32 off = 0, chunk = (u32)g_iobuf_flush_kb * 1024;   // large aligned writes; setting-sweepable
	sceKernelDcacheWritebackInvalidateRange((void *)base, size);   // flush dirty -> RAM for the DMA read
	while (off < size) {
		u32 n = size - off; if (n > chunk) n = chunk;
		if (!io_write_all(fd, (const void *)(base + off), (int)n)) return 0;
		off += n;
	}
	return 1;
}
static int read_region_direct(SceUID fd, u32 base, u32 size)
{
	u32 off = 0, chunk = (u32)g_iobuf_flush_kb * 1024;
	sceKernelDcacheWritebackInvalidateRange((void *)base, size);   // drop dirty old lines BEFORE the DMA
	while (off < size) {
		u32 n = size - off; if (n > chunk) n = chunk;
		int got = sceIoRead(fd, (void *)(base + off), (int)n);
		if (got <= 0) return 0;
		off += (u32)got;
	}
	sceKernelDcacheWritebackInvalidateRange((void *)base, size);   // CPU/game must see the DMA'd data
	return 1;
}

// VRAM (0x84000000) is the GE's eDRAM — the Memory Stick DMA engine cannot reach
// it, so a direct sceIoWrite/Read to/from VRAM HANGS (froze at CP15). Stage each
// 64KB chunk through work_buf (DDR) with a CPU copy instead — still uncompressed,
// still 64KB-aligned, just DMA'd from/to DDR (a CPU bounce copy, then a DDR DMA).
static int write_vram_staged(SceUID fd)
{
	u32 off = 0;
	while (off < VRAM_SIZE) {
		memcpy(work_buf, (const void *)(VRAM_BASE_CACHED + off), SAVE_CHUNK_SIZE);
		if (!io_write_all(fd, work_buf, (int)SAVE_CHUNK_SIZE)) return 0;
		off += SAVE_CHUNK_SIZE;
	}
	return 1;
}
static int read_vram_staged(SceUID fd)
{
	u32 off = 0;
	while (off < VRAM_SIZE) {
		if (sceIoRead(fd, work_buf, (int)SAVE_CHUNK_SIZE) != (int)SAVE_CHUNK_SIZE) return 0;
		memcpy((void *)(VRAM_BASE_CACHED + off), work_buf, SAVE_CHUNK_SIZE);
		off += SAVE_CHUNK_SIZE;
	}
	return 1;
}

// ── DDR memory-protection window for IOBUF (the volatile region) ──
// Programs the DDR MPU nibbles (regs @0xBC000000, 4 bits per 256KB part) for
// the volatile 4MB via sceKernelSetDdrMemoryProtection. The nibble is a
// per-MASTER access mask, and the observed states line up with that:
//  - 0xF = value while a game HOLDS the volatile lock ([VOLMEM] pre-release)
//    and the boot/vsh state — everything works there incl. the savedata
//    utility's Memory-Stick DMA.
//  - 0x0 = value power.prx programs when the lock is RELEASED ([VOLMEM]
//    post-release) — any access hard-stalls (the v391 CP13 freeze).
//  - 0xC = the kernel-partition value (loadexec/rebootex set it, then do CPU
//    writes only). v396: CPU R/W pass under a verified 0xC. v397: froze with
//    a verified 0xC once the batched save handed IOBUF to sceIoWrite — i.e.
//    the MS DMA master is NOT in 0xC's mask.
// So the IOBUF window opens with 0xF, the exact value the region has under a
// legitimate lock holder. Needs k1=0 (as ARK's kuKernelSetDdrMemoryProtection
// does). Return is logged (RAW).
#define DDR_PROT_OPEN          0xF          // all masters — the lock-holder state
#define DDR_PROT_OPEN_NIBBLES  0xFFFFFFFF   // DDR_PROT_OPEN replicated across a reg
static void ddr_protect_range(u32 base, u32 size, u32 val)
{
	int k1 = pspSdkSetK1(0);
	int r = sceKernelSetDdrMemoryProtection((void *)base, (int)size, (int)val);
	pspSdkSetK1(k1);
	WriteDebugLogHexRaw("[DDR] SetDdrMemoryProtection val<<16|ret=", (val << 16) | ((u32)r & 0xFFFF));
}
static void ddr_protect(u32 val) { ddr_protect_range(IOBUF_BASE, IOBUF_SIZE, val); }

// ── Verified MPU open for IOBUF use ──
// power.prx zeroes the volatile nibbles (0xBC000008/0C, parts 16-31) the
// moment the game releases the volatile lock ([VOLMEM] post-release = 0x0),
// and any access under 0x0 hangs the machine (the v391 CP13 freeze). Open
// the window with DDR_PROT_OPEN, verify by readback, require the value to
// survive a settle delay, re-set on failure, bounded tries. Callers use the
// per-chunk work_buf path when this returns 0 — a save/load always completes
// either way.
static int iobuf_mpu_open(void)
{
	return HWREG(IOBUF_MPU_REG_LO) == DDR_PROT_OPEN_NIBBLES
	    && HWREG(IOBUF_MPU_REG_HI) == DDR_PROT_OPEN_NIBBLES;
}

int iobuf_open_verified(void)
{
	int tries;
	for (tries = 0; tries < 8; tries++) {
		ddr_protect(DDR_PROT_OPEN);
		if (iobuf_mpu_open()) {
			sceKernelDelayThread(50000);   // 50ms settle — outlive a racing re-zero
			if (iobuf_mpu_open()) {
				WriteDebugLogHexRaw("[IOBUF] MPU open verified, tries=", (u32)tries);
				return 1;
			}
		}
		sceKernelDelayThread(50000);
	}
	WriteDebugLogRaw("[IOBUF] MPU open did NOT hold -> per-chunk fallback");
	return 0;
}

// Cheap in-loop guard: re-read the nibbles before touching IOBUF and re-assert
// once if something flipped them (v397 froze mid-save with a previously
// VERIFIED open, so treat the state as revocable). Returns 0 when the re-assert
// doesn't stick — callers must ABORT the batched phase (clean fail beats a bus
// stall). Logs only when a re-assert actually happens, so the hot path is two
// sysreg reads.
static int iobuf_still_open(void)
{
	if (iobuf_mpu_open())
		return 1;
	WriteDebugLogHexRaw("[IOBUF] nibbles CHANGED mid-use! LO=", HWREG(IOBUF_MPU_REG_LO));
	WriteDebugLogHexRaw("[IOBUF] nibbles CHANGED mid-use! HI=", HWREG(IOBUF_MPU_REG_HI));
	ddr_protect(DDR_PROT_OPEN);
	return iobuf_mpu_open();
}

// ── Read-ahead reader over the save file (mirror of the batched write) ──
// Buffers big blocks into the 4MB IOBUF and hands out records from RAM, so the
// restore does a few large reads instead of two small reads per chunk. A single
// record (<= COMPRESS_BUF_SIZE) always fits, so ra_need always makes progress.
struct ra_reader { SceUID fd; u8 *buf; u32 have; u32 rp; };
static void ra_init(struct ra_reader *r, SceUID fd)
{
	r->fd = fd; r->buf = (u8 *)IOBUF_BASE; r->have = 0; r->rp = 0;
}
// Ensure >= n bytes are available at the parse cursor; refills from fd, moving
// the unconsumed tail to the front first. Returns 0 on success, -1 on short read.
//
// Refills in the SAME 1MB cluster-aligned unit the writer flushes in
// (g_iobuf_flush_kb — see save_region_records / write_region_direct), NOT an
// arbitrary "fill the rest of the 4MB buffer" gulp. The writer keeps the file on
// the 64KB cluster grid (sections start 64KB-aligned via RAM_SECTION_OFFSET, every
// flush padded up to 64KB by iobuf_flush_padded); a refill sized IOBUF_SIZE-have
// drifts the file offset off that grid after the first read (have carries a
// partial-record `left`), turning every subsequent read into a partial-cluster
// read-modify-write — the exact penalty the write-side padding removed. A fixed
// 1MB (64KB-multiple) read keeps the file offset aligned to EOF, so load reads
// match the write path's rate. `left` (already-read bytes in RAM) doesn't move the
// file pointer, so a 1MB read always advances it by exactly 1MB. n is at most one
// record (<= ~68KB), so a single 1MB read always satisfies it.
static int ra_need(struct ra_reader *r, u32 n)
{
	if (r->rp + n <= r->have) return 0;
	{
		u32 left = r->have - r->rp;
		u32 chunk = (u32)g_iobuf_flush_kb * 1024;   // 1MB — same unit as the writer
		if (left) memmove(r->buf, r->buf + r->rp, left);
		r->have = left; r->rp = 0;
		while (r->have < n) {
			int got;
			u32 want = chunk;
			if (want > IOBUF_SIZE - r->have)
				want = (IOBUF_SIZE - r->have) & ~SAVE_ALIGN_MASK;   // safety: never overflow the 4MB buffer, stay 64KB-aligned
			// Revocation guard (see iobuf_still_open): the refill is an MS DMA
			// WRITE into the region — fail the read instead of stalling the bus.
			if (!iobuf_still_open()) {
				WriteDebugLogRaw("[IOBUF] open revoked mid-restore-read -> fail");
				return -1;
			}
			got = sceIoRead(r->fd, r->buf + r->have, (int)want);
			if (got <= 0) return -1;
			r->have += (u32)got;
		}
	}
	return 0;
}

// ── Compress + write one region as packed records (the compressed-save loop) ──
// Encodes [base, base+size) in SAVE_CHUNK_SIZE records ([64KB][cs][data], packed
// back-to-back) at the current file position. use_iobuf: batch through the 4MB
// volatile IOBUF and flush in large writes (padded to 64KB when `padded`; VRAM case unpadded);
// else fall
// back to per-chunk work_buf writes (always unpadded records — the pad only exists
// to align iobuf flushes). Record headers can land at ANY byte offset (cs is rarely
// a multiple of 4), and a direct u32 store there is an unaligned access MIPS faults
// on (Address Error 5) — memcpy compiles to byte stores; this exact fault was the
// real cause of every v397-v399 freeze. Adds each record's stored size to
// *bytes_out and the summed write/flush µs to *wt_us (NULL = don't track; the
// panel's compress/write split needs it). Returns 1 only if every write succeeded.
static int save_region_records(SceUID fd, u32 base, u32 size, int use_iobuf,
                               int padded, u32 *bytes_out, u32 *wt_us,
                               const char *tag)
{
	u32 off = 0;
	int ok = 1;
	(void)tag;   // only used by the debug-build log sinks below
	TM_BEGIN(ct, wt, tm);   // compress-µs / write-µs split
	if (use_iobuf) {
		u8 *iobuf = (u8 *)IOBUF_BASE;
		u32 fill = 0;
		// Padded sections use the sweepable flush threshold (clamped so
		// iobuf_flush_padded's up-to-64KB pad can't overrun the 4MB buffer);
		// unpadded sections force a single tail flush (<=~2.2MB always fits).
		u32 flush_threshold = padded ? (u32)g_iobuf_flush_kb * 1024 : IOBUF_SIZE - SAVE_ALIGN;
		if (flush_threshold > IOBUF_SIZE - SAVE_ALIGN) flush_threshold = IOBUF_SIZE - SAVE_ALIGN;
		int first_flush = 1;
		while (off < size) {
			prog_tick();   // advance the compress/write bar
			// Revocation guard: abort the batched phase (caller sees io_ok=0 -> no
			// CRC certification, ABORT path) instead of stalling the bus.
			if (!iobuf_still_open()) {
				WriteDebugLogRawF("[IOBUF] open revoked mid-%s-save -> ABORT", tag);
				ok = 0;
				break;
			}
			u32 cs = encode_chunk((const void *)(base + off), iobuf + fill + 8);
			TM_LAP(ct, tm);
			{ u32 hdr = SAVE_CHUNK_SIZE; memcpy(iobuf + fill + 0, &hdr, 4); }   // unaligned header — see above
			memcpy(iobuf + fill + 4, &cs, 4);
			fill += cs + 8;
			*bytes_out += cs + 8;
			off += SAVE_CHUNK_SIZE;
			// Flush before the next chunk if a worst-case record might overrun.
			if (fill + COMPRESS_BUF_SIZE + 8 > flush_threshold) {
				u64 fw0 = now_us();
				// The flush is the first NON-CPU (MS DMA) access to the region —
				// breadcrumb it so a freeze here vs in the CPU fill is separable.
				if (first_flush) {
					WriteDebugLogHexRaw("[IOBUF] first-flush nibbles LO=", HWREG(IOBUF_MPU_REG_LO));
					first_flush = 0;
				}
				ok &= padded ? iobuf_flush_padded(fd, iobuf, fill)
				             : io_write_all(fd, iobuf, (int)fill);
				if (wt_us) *wt_us += (u32)(now_us() - fw0);
				TM_LAP(wt, tm);
				fill = 0;
			}
		}
		if (fill && ok) {
			u64 fw0 = now_us();
			ok &= padded ? iobuf_flush_padded(fd, iobuf, fill)
			             : io_write_all(fd, iobuf, (int)fill);
			if (wt_us) *wt_us += (u32)(now_us() - fw0);
			TM_LAP(wt, tm);
		}
	} else {
		while (off < size) {
			u32 cs;
			prog_tick();
			cs = encode_chunk((const void *)(base + off), work_buf + 8);
			TM_LAP(ct, tm);
			*(u32 *)(work_buf + 0) = SAVE_CHUNK_SIZE;   // work_buf is aligned; direct stores fine
			*(u32 *)(work_buf + 4) = cs;
			{ u64 w0 = now_us(); ok &= io_write_all(fd, work_buf, cs + 8); if (wt_us) *wt_us += (u32)(now_us() - w0); }
			TM_LAP(wt, tm);
			*bytes_out += cs + 8;
			off += SAVE_CHUNK_SIZE;
		}
	}
#if DEBUG_BUILD
	{
		char lb[48];
		sprintf(lb, "[TIME] %s compress ms=", tag); TM_LOG(lb, ct);
		sprintf(lb, "[TIME] %s write ms=", tag);    TM_LOG(lb, wt);
	}
#endif
	return ok;
}

// ── Read + decompress one region's records back (the restore loop) ──
// Mirror of save_region_records: parses records from the current file position
// into [base, base+size). use_iobuf: read-ahead through the 4MB IOBUF
// (ra_reader); else per-chunk work_buf reads. Skips the self-describing 64KB
// flush-pad records (uncomp_size==0). Every header field is UNTRUSTED and
// validated before use: uncomp_size becomes the decompressor's write bound, so
// it is pinned to SAVE_CHUNK_SIZE (every chunk was written exactly that size and
// all regions are 64KB-aligned), and the produced byte count is verified so a
// short/failed decompress isn't advanced over as success. Returns bytes
// produced (== size only on a complete restore).
static u32 restore_region_records(SceUID fd, u32 base, u32 size, int use_iobuf,
                                  const char *tag)
{
	u32 off = 0;
	(void)tag;   // only used by the debug-build log sinks below
	TM_BEGIN(rt, dt, tm);   // read-µs / decompress-µs split
	if (use_iobuf) {
		struct ra_reader ra;
		ra_init(&ra, fd);
		while (off < size) {
			u32 uncomp_size, comp_size;
			prog_tick();   // advance the restore bar
			if (ra_need(&ra, 8) < 0) break;
			// ra.rp advances by 8+comp_size each record, so this header can sit at
			// any byte offset — unaligned u32 loads fault on MIPS (see the writer).
			memcpy(&uncomp_size, ra.buf + ra.rp + 0, 4);
			memcpy(&comp_size,   ra.buf + ra.rp + 4, 4);
			if (uncomp_size == 0) {   // 64KB flush-pad record — skip, don't count
				if (comp_size >= SAVE_ALIGN || ra_need(&ra, 8 + comp_size) < 0) break;
				ra.rp += 8 + comp_size;
				continue;
			}
			if (comp_size == 0 || comp_size > COMPRESS_BUF_SIZE - 8) break;
			if (uncomp_size != SAVE_CHUNK_SIZE) break;   // pin untrusted write bound
			if (ra_need(&ra, 8 + comp_size) < 0) break;
			TM_LAP(rt, tm);
			if (decode_chunk(ra.buf + ra.rp + 8, comp_size, (void *)(base + off)) != (int)uncomp_size) break;
			TM_LAP(dt, tm);
			ra.rp += 8 + comp_size;
			off += uncomp_size;
		}
	} else {
		while (off < size) {
			u32 uncomp_size, comp_size;
			prog_tick();   // advance the restore bar
			if (sceIoRead(fd, work_buf, 8) != 8) break;
			uncomp_size = *(u32 *)(work_buf + 0);
			comp_size   = *(u32 *)(work_buf + 4);
			if (uncomp_size == 0) {   // 64KB flush-pad record — skip
				if (comp_size >= SAVE_ALIGN) break;
				sceIoLseek(fd, comp_size, PSP_SEEK_CUR);
				continue;
			}
			if (comp_size == 0 || comp_size > COMPRESS_BUF_SIZE - 8) break;
			if (uncomp_size != SAVE_CHUNK_SIZE) break;   // pin untrusted write bound
			if (sceIoRead(fd, work_buf + 8, comp_size) != (int)comp_size) break;
			TM_LAP(rt, tm);
			if (decode_chunk(work_buf + 8, comp_size, (void *)(base + off)) != (int)uncomp_size) break;
			TM_LAP(dt, tm);
			off += uncomp_size;
		}
	}
#if DEBUG_BUILD
	{
		char lb[48];
		sprintf(lb, "[TIME] %s read ms=", tag);       TM_LOG(lb, rt);
		sprintf(lb, "[TIME] %s decompress ms=", tag); TM_LOG(lb, dt);
	}
#endif
	return off;
}

// Advance the background save-file CRC by one 64KB-aligned chunk (crc_read_next
// pacing is the caller's: the frozen prompt loop runs it back-to-back, the
// post-resume loop sleeps between calls so the running game's own MS reads can
// interleave). On EOF the CRC is finished and stamped into header word 8 — the
// certify step a load's verify checks. Returns 1 while still hashing, 0 once
// finished+stamped.
static int bg_crc_step(SceUID fd, SceOff *off, u32 *crc, int *io_ok)
{
	int n = crc_read_next(fd, off);
	if (n > 0) { *crc = crc32_update(*crc, work_buf, (u32)n); return 1; }
	*crc = crc32_finish(*crc);
	sceIoLseek(fd, (SceOff)(8 * sizeof(u32)), PSP_SEEK_SET);
	*io_ok &= io_write_all(fd, crc, sizeof(*crc));
	return 0;
}

// Draw a centered red notice (msg1) + optional white sub-line (msg2) on the solid-black
// panel across the captured framebuffers. Shared by the abort helpers below.
static void draw_fail_panel(const char *msg1, const char *msg2)
{
	prog_card(BR_RED);                                // red-accented card = failure
	dbg_transparent = 0; dbg_bg = BR_CARD;            // red/white text reads clearly on the card
	dbg_fg = BR_RED;   dbg_row = PANEL_TOP + 1; dbg_col = (60 - (int)strlen(msg1)) / 2; if (dbg_col < 0) dbg_col = 0; dbg_print_all(msg1);
	if (msg2) { dbg_fg = 0xFFFFFFFF; dbg_row = PANEL_TOP + 4; dbg_col = (60 - (int)strlen(msg2)) / 2; if (dbg_col < 0) dbg_col = 0; dbg_print_all(msg2); }
}

// Centered accent-outlined prompt pill on the panel, drawn to every captured buffer
// (pre-/post-suspend safe). row = char-row of the label; fg = text, line = the pill's
// border + left stripe accent (green for a normal "continue", red for a failure).
static void prompt_pill(int row, const char *label, u32 fg, u32 line)
{
	int len = (int)strlen(label);
	int w   = len * 8 + 24;
	int px  = (480 - w) / 2;
	int py  = row * 8 - 4;
	dbg_fill_rect_all(px, py, w, 20, BR_CARD);
	dbg_fill_rect_all(px, py, w, 1, line);
	dbg_fill_rect_all(px, py + 19, w, 1, line);
	dbg_fill_rect_all(px, py, 3, 20, line);
	dbg_transparent = 1;
	dbg_fg = fg; dbg_row = row; dbg_col = (60 - len) / 2; if (dbg_col < 0) dbg_col = 0;
	dbg_print_all(label);
	dbg_transparent = 0;
}

// Forward decl: arm_input_suppress() lives with the ctrl-hook functions much
// further down (it arms the flags those hooks read), but op_abort's resume
// needs it here too — see arm_input_suppress's own comment for what it covers.
void arm_input_suppress(void);

// ── Unified abort of a frozen save/load (resumable) ──────────────────────────
// One path for the many failure points so they aren't a patchwork. It:
//   - closes the file, and for a SAVE deletes the incomplete file (save_path non-NULL;
//     loads pass NULL — a load makes no file);
//   - optionally shows a red notice the user confirms with X (msg1 non-NULL) — only
//     meaningful once the panel is up (after prog_begin), so pre-freeze opens pass NULL;
//   - resumes the game: wakes the VSH/controller so the prompt input works, resumes the
//     game threads, AND delivers the RESUMING/RESUME_COMPLETE the game's power callback
//     is owed. We send it SUSPENDING before freezing (cooperative_volmem_release), but an
//     abort never runs the firmware suspend that normally delivers the matching resume —
//     without it the game stays in its own suspended state even with its threads awake.
// ALWAYS returns -1. Call only AFTER SUSPENDING was sent (post cooperative_volmem_release);
// a pre-freeze open failure just closes the file and returns -1 itself.
static int op_abort(const SceUID *game_tids, int tcount, SceUID fd,
                    const char *save_path, const char *msg1, const char *msg2)
{
	int i;
	if (fd >= 0) sceIoClose(fd);
	if (msg1 && dbg_buf_count > 0) {
		if (game_tids) resume_nongame_threads(game_tids, tcount);   // wake controller service so X reads
		// Put the notice on the VISIBLE buffer: blanking games re-point at the pre-blank
		// capture; non-blanking games pin the frozen front buffer. Without this the panel
		// lands off-screen on a non-blanking game (was gated on g_game_pcb_count>0, so a
		// CRC-fail / "Load failed" notice was invisible on e.g. Pirates). Same fix as the
		// save/load banner — see pin_current_display().
		if (g_game_pcb_count > 0) reassert_display();
		else                      pin_current_display();
		draw_fail_panel(msg1, msg2);
		prompt_pill(PANEL_TOP + 7, "Press X to continue", BR_GREEN, BR_RED);
		wait_button_edge(PSP_CTRL_CROSS);
		wait_buttons_up();
	}
	arm_input_suppress();   // real button presses may have been sampled while the notice was up
	resume_game_threads();
	if (g_game_pcb_count > 0) reassert_display();
	for (i = 0; i < g_game_pcb_count; i++) sceKernelNotifyCallback(g_game_pcbs[i], PSP_POWER_CB_RESUMING);
	if (g_game_pcb_count > 0) sceKernelDelayThread(50000);        // let the game process RESUMING before RESUME_COMPLETE
	for (i = 0; i < g_game_pcb_count; i++) sceKernelNotifyCallback(g_game_pcbs[i], PSP_POWER_CB_RESUME_COMPLETE);
	if (save_path) sceIoRemove(save_path);
	return -1;
}

// Non-resumable failure: the game RAM was already clobbered (post-suspend), so we CANNOT
// resume — draw a red notice telling the user to power off and leave the game frozen.
static int op_frozen_fail(const char *msg1, const char *msg2)
{
	if (dbg_buf_count > 0) draw_fail_panel(msg1, msg2);
	return -1;
}

int FreezeSave(const char *path)
{
	SceUID fd;
	u32 offset;
	u32 header[SAVE_HEADER_WORDS];
	u32 ram_bytes = 0, vram_bytes = 0;
	int do_bg_crc = 0;   // SAVE only: compute the file CRC AFTER the game resumes
	int io_ok = 1;       // cleared by any short/failed sceIoWrite (MS full/card error) -> don't report success
	SceOff vram_pos = 0; // file offset of the VRAM section (for restore-after-staging)
	SceOff kernel_pos = 0; // file offset of the kernel section (M4: written after resume)
	PspGeContext ge_ctx;
	SceUID game_tids[MAX_SUSPEND_THREADS];
	int game_status[MAX_SUSPEND_THREADS]; // save-time status (READY/WAITING/...) per game thread
	int tcount = 0, i;
	u64 t_start, t_mark;  // W1 speed instrumentation (RTC wall-clock µs)
	int was_load = 0;     // captured from g_op_mode before it's reset mid-tail (for the "time:" line)
	u32 op_elapsed_ms = 0; // op wall-clock, captured ONCE at restore-done (CP25) so BOTH the
	                       // on-screen panel AND the UART log print the SAME value — NOT a
	                       // now_us() recomputed after the "Press X" wait (which would add the
	                       // user's read time; that was the "Save time: 88.887s" bug).

	WriteDebugLog("[FSAVE] Starting full save...");
#if DEBUG_BUILD
	if (DBG_UART()) { char gb[96]; sprintf(gb, "[GAME] %.40s id=%.16s", g_game_title[0] ? g_game_title : "?", umdid[0] ? umdid : "?"); uart_puts(gb); }
#endif
	diag_profile("save-op");   // pre-freeze: is the game holding volatile / running MS DMA?
	t_start = t_mark = now_us();
	trace_start();   // RAM-only per-checkpoint trace; flushed once after the restore below

	// O_RDWR (not O_WRONLY): after staging the kernel snapshot we re-READ the
	// VRAM section back from this same file to restore the game's framebuffer.
	// NO O_TRUNC: overwrite the existing slot file IN PLACE so its cluster chain
	// is reused (stays contiguous -> fast) instead of being freed + reallocated
	// from scattered free space on every save. We write from offset 0; a smaller
	// save leaves a stale tail (ignored — the header records every section's
	// offset/size), a larger save extends once and then re-stabilizes.
	//
	// Tested O_TRUNC (2026-07-04): inconsistent write-speed effect (sometimes
	// better than in-place at the same flush size, sometimes worse — best
	// 4.33 MB/s, worst 3.32 MB/s across several runs, no reliable win) and a
	// real downside: an interrupted/frozen save destroys the old save before
	// the new one is confirmed written, unlike in-place overwrite. Reverted.
	fd = sceIoOpen(path, PSP_O_RDWR | PSP_O_CREAT, 0777);
	if (fd < 0) {
		WriteDebugLogHexRaw("[FSAVE] FAILED to open save file, fd=", (u32)fd);
		return -1;
	}

	sceKernelDelayThread(100000); // ~100ms — let any in-flight I/O settle

	// Capture the preview thumbnail now, while the (clean) game frame is on
	// screen and before the RAM loop reuses work_buf. SAVE-only: on a LOAD the
	// system re-enters this function's TAIL via the snapshot, not the top.
	write_thumbnail(path);
	write_screenshot(path);   // full-res preview for the fullscreen view (Left in the browser)

	// No pre-freeze VRAM banner: the game is still running here and
	// overwrites it before it's ever readable. dbg_capture_both_bufs()
	// is still needed though — it captures the framebuffer pointers
	// that CP16 onward (already inside the freeze) write into.
	dbg_capture_both_bufs();
	dbg_fg = 0xFFFFFFFF; dbg_bg = 0xFF000000;
	ms_test_row = 0;

	// ── Identify the narrow "game thread" set for context capture,
	// BEFORE freezing (IRQs on, ordinary enumeration). game_status keeps each
	// thread's natural status for the context capture. ──
	tcount = identify_game_threads(game_tids, game_status);

	// Ask the game to release its volatile-memory lock (via its power callback) BEFORE we
	// freeze it — a frozen game can't run its handler, and the suspend won't sleep while
	// volatile is locked. Also necessary for sub_16389() (firmware PHASE0_5 ME halt sequence):
	// that handler sends a halt interrupt to the ME and spins until the ME acks; if GTA's ME
	// firmware is still running audio it never responds and the firmware hangs. The game's
	// callback gracefully stops the ME program, ensuring sub_16389() can complete and call
	// sceSysregMeBusClockDisable() before Phase0_0 fires. If it won't let go within 5s, abort.
	if (cooperative_volmem_release(game_tids, tcount) < 0) {
		WriteDebugLogRaw("[FSAVE] game never released volatile mem -> ABORT save");
		return op_abort(game_tids, tcount, fd, path, NULL, NULL);   // game still running (not frozen) -> silent; delete the empty file
	}

	// The game blanked the screen inside its SUSPENDING handler (above). Re-point
	// the display at the captured frame and show "Saving" NOW — BEFORE the freeze
	// loop, which can take a moment if a game thread is slow to reach a stable
	// wait. Otherwise the screen sits black (backlight on) for that whole window.
	// Reasserted again after the freeze (below) in case the still-running game
	// re-blanked in between.
	if (g_game_pcb_count > 0) { reassert_display(); prog_begin("Saving"); }

	// ── QUIESCENT-POINT FREEZE (Stage 3): per-thread suspend of ONLY the
	// game's user threads, tracking exactly what we suspend so resume is
	// symmetric (game-parked threads stay parked) — as opposed to a blanket
	// suspend-all-user-threads/resume-all-user-threads pair, which is not used
	// here. Kernel threads + dispatcher + interrupts stay ON. ──
	{
		// Freeze at a QUIESCENT point. The native suspend only sleeps when no
		// driver vetoes its QUERY (e.g. UMD mid-read). So: poll QUERY until it's
		// clear, freeze THERE, then re-poll to close the race (a read that started
		// while we were freezing) — retry if so. Frozen at a clear point the game
		// can't begin a new op, so the real suspend's QUERY stays clear and it
		// sleeps with the game fully frozen (consistent save, no divergence).
		int q, frozen = 0, fails = 1;
		for (q = 0; q < 1000 && !frozen; q++) {
			if (probe_suspend_query() == 0) {            // no driver mid-op right now
				fails = suspend_escalating(0, 50);    // save: FAST gate (freeze on first WAITING, else dispatch-off) — game I/O already stopped by cooperative_volmem_release, so no mid-read to dodge; freezes threadmain reliably instead of looping on a never-"stable" thread
				// Commit only when EVERY thread froze AND the query is still clear AND
				// no frozen thread owns the ME RPC mutex (me_rpc_probe) — a frozen
				// owner would wedge the 0x402 descent veto forever (the CP27 timeout
				// + phantom-sleep tail). On probe fail the unfreeze below releases
				// the holder; it unlocks within a schedule slice and the retry
				// re-freezes it clean. Same unfreeze-retry shape as the MS FAT-lock
				// probe at menu-open (ms_probe_after_freeze).
				if (fails == 0 && probe_suspend_query() == 0 && me_rpc_probe() == 0) frozen = 1;
				else resume_game_threads();              // a thread wouldn't freeze, race lost, or frozen ME-mutex holder -> unfreeze, retry
			}
			if (!frozen) sceKernelDelayThread(3000);     // ~3ms between polls
		}
		ms_test_cp(11, (u32)q, frozen ? "froze-at-quiescent" : "no-quiescent-fellback");
		if (frozen) {
			// Post-freeze snapshot: is game DMA still in flight (MS contention lead),
			// and what are the frozen game threads waiting on? Compare vs GTA.
			diag_profile_frozen("save-frozen");
			diag_threads("save-frozen", game_tids, tcount);
		}
		if (!frozen) {                                   // couldn't cleanly freeze EVERY thread -> do NOT write a corrupt save
			WriteDebugLogRaw("[FSAVE] freeze FAILED (a thread would not freeze) -> ABORT save");
			// Game is running again (freeze failed -> resumed), so no stable-frame prompt;
			// op_abort un-blanks, delivers the owed RESUMING, and deletes the empty file.
			return op_abort(game_tids, tcount, fd, path, NULL, NULL);
		}
	}

	// Make the save-phase overlay land on the VISIBLE buffer with a matching stride.
	// Blanking games (pcb>0): re-point the display at the pre-blank captured framebuffer.
	// Non-blanking games (pcb==0): the pre-freeze capture may be stale (game kept flipping,
	// possibly to a third buffer) -> adopt+pin the actual frozen front buffer now. This is
	// the fix for the intermittent banner-shift / no-banner (e.g. Pirates). See
	// pin_current_display().
	if (g_game_pcb_count > 0) reassert_display();
	else                      pin_current_display();

	prog_begin("Saving");   // weighted bar (W_SAVE_*): RAM+VRAM save, suspend, kernel save, restore

	ms_test_cp(12, (u32)tcount, "game-threads-for-context");

	header[0] = SAVESTATE_MAGIC;
	header[1] = SAVESTATE_VERSION;
	header[2] = MAIN_RAM_SIZE;
	header[3] = VRAM_SIZE;
	header[4] = (u32)sizeof(PspGeContext);
	header[5] = (u32)tcount;
	header[6] = KERNEL_RAM_SIZE; // kernel section appended after thread contexts
	header[7] = 0;               // kernel_pos     — patched in after capture (M4/M5)
	header[8] = 0;               // crc32(file after header) — patched in after the kernel write
	// Save-time address of g_op_mode. The plugin loads at a DIFFERENT kernel address
	// each boot, so a cross-session LOAD can't use its own &g_op_mode to find the flag
	// inside the saved snapshot — it must patch at the SAVE-TIME offset. Store it here.
	header[9] = (u32)&g_op_mode;
	// Save-time address of g_handoff_ctx (dispatcher context captured by SysEventShim;
	// same different-address-each-boot reason as header[9]). The LOAD's ApplyHandoff
	// restores this block from the applied image and jumps to its ra — the one-way
	// handoff that replaces returning through load-session code.
	header[10] = (u32)&g_handoff_ctx;
	header[11] = 0;   // reserved
	// Save-time address of g_overclock_id — same different-address-each-boot reason
	// as header[9]/[10]. A LOAD patches the CURRENT session's live value in at this
	// offset (see FreezeLoad) so the overclock setting reflects what the user has
	// NOW, not whatever it was when this particular save was made.
	header[12] = (u32)&g_overclock_id;
	io_ok &= io_write_all(fd, header, sizeof(header));

	{
	// Use volatile IOBUF if MPU open verifies; else fall back to per-chunk work_buf I/O.
	int use_iobuf = g_compress ? iobuf_open_verified() : 0;   // Fast mode writes direct from game RAM — no volatile IOBUF, no MPU unlock

	// ── Compress + write Main RAM (24MB) ──
	ms_test_cp(13, MAIN_RAM_SIZE, use_iobuf ? "starting-ram-save-IOBUF" : "starting-ram-save-perchunk");
	// Start the RAM section on a 64KB cluster boundary (pad past the header) so
	// each padded flush lands aligned — see iobuf_flush_padded / RAM_SECTION_OFFSET.
	sceIoLseek(fd, RAM_SECTION_OFFSET, PSP_SEEK_SET);
	u32 ram_wt_us = 0;   // write-only µs (sum of flush/write durations); compress = total - write, for the panel split
	if (g_compress) prog_step2("Game RAM compress", "Game RAM write", W_SAVE_RAM, PB_RAM_MS);   // both shown "...." during the wait
	else            prog_step("Saving Game RAM", W_SAVE_RAM, PB_RAM_MS);
	if (!g_compress) {
		// Fast mode: DMA the raw 24MB straight from game RAM to the card — no
		// IOBUF, no per-chunk records, no compress step. See write_region_direct.
		TM_BEGIN1(ram_wt, ram_tm);
		io_ok &= write_region_direct(fd, MAIN_RAM_BASE_CACHED, MAIN_RAM_SIZE);
		TM_LAP(ram_wt, ram_tm);
		TM_LOG("[TIME] RAM write ms=", ram_wt);
		ram_bytes = MAIN_RAM_SIZE;
	} else {
		// Compressed records, batched through IOBUF when the MPU open held (turns
		// ~384 small unaligned writes into a few large padded ones) — see
		// save_region_records for the record format + flush rules.
		io_ok &= save_region_records(fd, MAIN_RAM_BASE_CACHED, MAIN_RAM_SIZE,
		                             use_iobuf, 1, &ram_bytes, &ram_wt_us, "RAM");
	}
	{
		// Fill the compress + write rates (labels were shown up front by prog_step2).
		// total = whole section (from prog_step2's timestamp); write = summed flush/
		// write µs; compress = the rest. Compress rate is on the UNCOMPRESSED input;
		// write rate on the bytes written.
		u32 tot = (u32)(now_us() - g_prog_t0);
		if (g_compress)
			prog_fill2(MAIN_RAM_SIZE, tot > ram_wt_us ? tot - ram_wt_us : 0, ram_bytes, ram_wt_us);
		else
			prog_done_us("Saving Game RAM", MAIN_RAM_SIZE, tot);   // Fast mode: nothing to split off
	}
	ms_test_cp(14, ram_bytes, "ram-done(compressed-bytes)");

	// ── Compress + write VRAM ──
	// Record where the VRAM section starts so we can re-read+decompress it
	// back into VRAM after using VRAM to stage the kernel snapshot.
	vram_pos = sceIoLseek(fd, 0, PSP_SEEK_CUR);   // 64KB-aligned (RAM tail was padded)
	// Record the true VRAM offset in header[11] (reserved word). On load this
	// both lands the reader on the real VRAM start (past the RAM section's tail
	// padding) and flags the file as the 64KB-aligned format — old saves have 0.
	{ u32 vp = (u32)vram_pos | (g_compress ? 0 : SAVE_FLAG_UNCOMPRESSED);   // top bit = Fast (raw blob) format
	  sceIoLseek(fd, (SceOff)(11 * (u32)sizeof(u32)), PSP_SEEK_SET);
	  io_ok &= io_write_all(fd, &vp, sizeof(vp));
	  sceIoLseek(fd, vram_pos, PSP_SEEK_SET); }
	ms_test_cp(15, VRAM_SIZE, "starting-vram-save");
	u32 vram_wt_us = 0;   // write-only µs for the panel split (see RAM section)
	if (g_compress) prog_step2("VRAM compress", "VRAM write", W_SAVE_VRAM, PB_VRAM_MS);
	else            prog_step("Saving VRAM", W_SAVE_VRAM, PB_VRAM_MS);
	if (!g_compress) {
		// Fast mode: VRAM is eDRAM (no MS DMA) — stage through work_buf. See write_vram_staged.
		TM_BEGIN1(vr_wt, vr_tm);
		io_ok &= write_vram_staged(fd);
		TM_LAP(vr_wt, vr_tm);
		TM_LOG("[TIME] VRAM write ms=", vr_wt);
		vram_bytes = VRAM_SIZE;
	} else {
		// VRAM flushes are UNPADDED (padded=0, unlike the RAM section): the GE/thread
		// sections read straight after VRAM via the current file position, so a padded
		// VRAM tail would desync those reads. That makes a MID-loop flush harmful — it
		// ends on an unaligned offset, so the NEXT flush starts unaligned and every
		// cluster after it costs a card read-modify-write => the intermittent ~4MB/s
		// VRAM write. VRAM (<=2MB in, <=~2.2MB compressed) always fits the 4MB IOBUF,
		// so padded=0 forces a SINGLE tail flush: it starts on the aligned vram_pos, so
		// only its final cluster is unaligned (negligible). The file bytes are identical
		// either way (records pack back-to-back), so the load path is unchanged.
		io_ok &= save_region_records(fd, VRAM_BASE_CACHED, VRAM_SIZE,
		                             use_iobuf, 0, &vram_bytes, &vram_wt_us, "VRAM");
	}
	{
		u32 tot = (u32)(now_us() - g_prog_t0);   // whole VRAM section (from prog_step2)
		if (g_compress)
			prog_fill2(VRAM_SIZE, tot > vram_wt_us ? tot - vram_wt_us : 0, vram_bytes, vram_wt_us);
		else
			prog_done_us("Saving VRAM", VRAM_SIZE, tot);
	}
	}
	TLOG("[TIME] RAM+VRAM compress+write ms=", t_mark);
	ms_test_cp(16, vram_bytes, "vram-done(compressed-bytes)");

	// ── GE context (fixed size, uncompressed) ──
	ms_test_cp(17, (u32)sizeof(ge_ctx), "starting-ge-save");
	memset(&ge_ctx, 0, sizeof(ge_ctx));
	sceGeSaveContext(&ge_ctx);
	io_ok &= io_write_all(fd, &ge_ctx, sizeof(ge_ctx));
	ms_test_cp(18, (u32)sizeof(ge_ctx), "ge-context-done");

	// ── Game thread contexts: {name[32], status(u32), SceThreadContext}
	// per thread. status = the thread's natural state at save (READY/
	// WAITING/…) so the load can choose to restore only non-waiting
	// (READY/RUNNING) threads, avoiding the register-vs-kernel-wait-state
	// mismatch that crashes resume for WAITING threads. ──
	ms_test_cp(19, (u32)tcount, "starting-thread-ctx-save");
	for (i = 0; i < tcount; i++) {
		SceKernelThreadKInfo kinfo;
		u32 status = (u32)game_status[i];
		int ctxret;
		memset(&kinfo, 0, sizeof(kinfo));
		kinfo.size = sizeof(kinfo);

		ctxret = ThreadManForKernel_2D69D086(game_tids[i], &kinfo);
		if (ctxret >= 0 && kinfo.thContext) {
			io_ok &= io_write_all(fd, kinfo.name, sizeof(kinfo.name));
			io_ok &= io_write_all(fd, &status, sizeof(status));
			io_ok &= io_write_all(fd, kinfo.thContext, sizeof(struct SceThreadContext));
		} else {
			char blank_name[32];
			struct SceThreadContext blank_ctx;
			memset(blank_name, 0, sizeof(blank_name));
			memset(&blank_ctx, 0, sizeof(blank_ctx));
			io_ok &= io_write_all(fd, blank_name, sizeof(blank_name));
			io_ok &= io_write_all(fd, &status, sizeof(status));
			io_ok &= io_write_all(fd, &blank_ctx, sizeof(blank_ctx));
			// Log the ACTUAL return code (not the thread UID) so the
			// failure can be decoded via `pspsh -e "error 0x..."` —
			// don't guess at the cause, find out for real.
			ms_test_cp(20, (u32)ctxret, "thread-ctx-FAILED-ret");
		}
	}
	ms_test_cp(21, (u32)tcount, "thread-contexts-done");

	// GATE 1: if any pre-suspend write short-wrote (MS full / card error) the file is
	// already truncated. Do NOT proceed into the suspend/capture — abort via op_abort
	// (shows the notice + resumes the game; RAM is still intact, not yet staged over) and
	// delete the broken file. (The in-place overwrite already cost the previous save.)
	if (!io_ok) {
		ms_test_cp(21, 0, "save-write-SHORT-preSuspend-ABORT");
		WriteDebugLogRaw("[FSAVE] short write before suspend (MS full?) -> ABORT");
		return op_abort(game_tids, tcount, fd, path, "Save incomplete", "Not enough space");
	}

	// ── Kernel window capture via FIRMWARE SUSPEND, staged in GAME RAM (M4) ──
	// The phat has no spare bank, so stage the kernel snapshot in the game's own
	// RAM (vacated; already written to MS). Capture at
	// PHASE0_0 (the quiesced point, matching where LOAD applies it). Keep GAME
	// threads frozen across the suspend (resume only the VSH/system threads the
	// handshake needs) so the game RAM stays consistent with what we saved AND
	// game threads don't run on the clobbered staging after resume. Then drain
	// the snapshot to MS and restore the (clobbered) game RAM from MS.
	{
		// Align the kernel section to a 64KB boundary too (see RAM section): its
		// flushes are padded below, and the reader seeks straight to header[7], so
		// the small gap left after the thread contexts is never parsed.
		kernel_pos = sceIoLseek(fd, 0, PSP_SEEK_CUR);
		kernel_pos = (kernel_pos + SAVE_ALIGN_MASK) & ~(SceOff)SAVE_ALIGN_MASK;
		sceIoClose(fd);                                // sceIo is dead during suspend
		fd = -1;

		resume_nongame_threads(game_tids, tcount);     // wake VSH/system; game stays parked
		ms_test_cp(22, g_kcap_size, "kernel-capture-via-suspend");
		g_op_mode = 0;            // SAVE; the snapshot captures this. A LOAD patches the
		                          // snapshot's copy to 1 so its resume takes the LOAD branch.
		// No flash-driver repair on the SAVE path. A SAVE only READS kernel RAM
		// (no rollback), so like a native power-switch sleep the firmware keeps the
		// flash drivers coherent across its own suspend/resume — repairing is
		// redundant and (per the v510 bisection) froze Dissidia. The LOAD path does
		// need it (it rolls the kernel back to a stale flash state); it runs in
		// utils.c on RESUME_COMPLETED — in-place lflash df_exit/df_init (FTL rebuild)
		// bracketed by a flash1: remount, keyed on g_lc.op_mode == 1.
		g_resumed = 0;
		g_sleep_mode = 9;                              // capture kernel -> GAME_STAGE_KERNEL at g_cap_phase
		// Always capture the full 8MB kernel window.
		g_kcap_off = 0; g_kcap_size = KERNEL_RAM_SIZE;
		// Stage the snapshot in the user-RAM slot. g_stage_spot is permanently 1 (Mid) —
		// see its own comment — so this is just g_stage_bases[1], written this way only
		// to keep both call sites reading the array by the same (now-fixed) index.
		g_stage_base = g_stage_bases[1];
		// Capture at PHASE0_0 (fully quiesced) for all games.
		g_cap_phase = SCE_SYSTEM_SUSPEND_EVENT_PHASE0_0;
		g_sleep_arm = 1;
		ClearCaches();
		// The game already released its volatile-memory lock cooperatively (its power
		// callback, before the freeze), so scePower's +36 flag is clear and the native
		// suspend can FREEZE. No force-unlock needed here (the LOAD path runs the same way).
		// NOTE: do NOT sceUmdDeactivate here. It powers down the logical UMD drive,
		// which GTA's streaming engine sees as a disc-removal error ("Error reading
		// the UMD") and never recovers from. In-flight UMD DMA is instead drained at
		// the Phase0_0 capture point (DMAC EnbldChns spin in utils.c), which is enough.
		t_mark = now_us();
		// No progress-bar segment opens here: the CPU-off firmware suspend can't tick (no
		// CPU) and isn't given a weight of its own (see the progress-bar model comment
		// above) — the bar just holds at the pre-suspend ~50% for this whole window.
		scePowerRequestSuspend();
		if (!wait_for_resume()) {
			ms_test_cp(27, 0, "save-suspend-TIMEOUT");
			// Disarm: RESUME_COMPLETED never fired (it clears these), so without this
			// the mode-9 capture stays armed and the next NATIVE power-switch sleep would
			// run it against live game RAM.
			g_sleep_arm = 0; g_sleep_mode = 0;
			// Suspend never completed -> game RAM never staged over, still intact -> resumable.
			return op_abort(game_tids, tcount, fd, path, "Save failed", "Try again");
		}
		// Re-init UART post-resume so the tail is visible: the firmware suspend resets
		// the UART core / HPRM re-grabs the port, and the +0x44 status clear in
		// uart_init un-wedges the latched TXFULL (see HOWTO_UART.md §3, §7). Safe here:
		// wait_for_resume() returned -> RESUME_COMPLETED fired, the firmware+dispatcher
		// are fully alive and this (menu) thread is running normally. Covers both SAVE
		// and LOAD (LOAD re-enters this same tail). Gated on the UART setting so an
		// off setting makes no firmware calls / 100ms sleep here. No-op in a release build.
		if (DBG_UART()) {
			uart_init();   // (banner-line removed; the post-resume tail existing at all confirms the re-init)
		}
		// Overclock reapply is NOT done here — moved to utils.c's ProcessSignals on
		// RESUME_COMPLETED, which fires for EVERY firmware resume (native sleep too,
		// not just our own save/load), and runs BEFORE wait_for_resume() even returns
		// (it's what SETS g_resumed). So by the time this tail runs, it has already
		// happened.
		// LOAD continuation: the kernel rollback restored the save-time row counter and
		// the VRAM restore repaints the save's debug overlay across the TOP rows. We keep
		// that overlay and print the load's tail messages in the always-free lower half
		// (rows 18+) instead, so they never collide with the save lines. (SAVE keeps its
		// continuous live display — no rollback there.) The "Loading" panel is rebuilt a
		// few lines below (g_op_mode == 1 block); no separate transient banner here.
		if (g_op_mode == 1) {
			ms_test_row = 18;
		}
		// Stamp activity = NOW (new-session clock) so the power manager doesn't see the
		// stale save-time "last activity" tick and idle-off the display during the RAM/
		// VRAM restore. Without this, a load made long after the save (cross-session)
		// resumes to a blanked screen that only wakes on a button press. Fire it BEFORE
		// the restore so it beats the idle check.
		scePowerTick(PSP_POWER_TICK_ALL);
		TLOG("[TIME] suspend+kernel-capture ms=", t_mark);
		ms_test_cp(23, g_kcap_size, "snapshot-in-gameRAM-ok");
		// Logs when the mode-9 DMAC drain-before-read (utils.c) timed out with a
		// channel still enabled, logged here where MS is alive again (the drain
		// itself runs inside the sysevent handler, where sceIo is unusable).
		if (g_dma_drain_left) ms_test_cp(23, g_dma_drain_left, "WARN-DMAC-drain-timeout(kernel-read)");

		// g_op_mode tells whether this resumed continuation is a real SAVE (write
		// the snapshot to MS) or a LOAD that re-entered this same tail via the
		// kernel rollback (must NEVER touch the savegame — restore game RAM/VRAM
		// read-only, then resume).
		if (g_op_mode == 0) {
			// SAVE: write the kernel snapshot to MS + patch kernel_pos. Read the
			// snapshot from the UNCACHED game-RAM alias (mode 9 wrote it straight
			// to RAM; do NOT writeback-flush the stale cached view over it).
			fd = sceIoOpen(path, PSP_O_RDWR, 0777);
			if (fd < 0) {
				// Game RAM top 8MB holds the kernel snapshot (clobbered); nothing is
				// restored yet. Do NOT resume into corrupt RAM — leave the game frozen
				// for a clean power-cycle (baseline FreezeLoad policy).
				ms_test_cp(28, (u32)fd, "reopen-FAILED-frozen-poweroff");
				WriteDebugLogRaw("[FSAVE] reopen failed post-resume -> game left frozen; power off");
				return op_frozen_fail("Save failed", "Power off the PSP");   // RAM clobbered by kernel staging -> cannot resume
			}
			sceIoLseek(fd, kernel_pos, PSP_SEEK_SET);
			prog_step("Saving kernel", W_SAVE_KERNEL, PB_KERNEL_MS);
			{
				// Same batched-IOBUF path as RAM/VRAM (measured ~1.3 MB/s here
				// per-chunk vs up to ~4 MB/s batched) — safe to reuse post-resume:
				// the game threads are still parked (only VSH/system were woken
				// above), so nothing has re-locked volatile memory yet; the RAM/
				// VRAM restore right after this already re-verifies the same way.
				int use_iobuf_kn = g_compress ? iobuf_open_verified() : 0;   // Fast mode: direct, no MPU unlock
				u32 kbytes = 0;
				if (!g_compress) {
					// Fast mode: DMA the raw 4MB kernel snapshot straight from its
					// game-RAM staging slot (see write_region_direct).
					TM_BEGIN1(kn_wt, kn_tm);
					io_ok &= write_region_direct(fd, g_stage_base, KERNEL_RAM_SIZE);
					kbytes = KERNEL_RAM_SIZE;
					TM_LAP(kn_wt, kn_tm);
					TM_LOG("[TIME] kernel write ms=", kn_wt);
				} else {
					// Compress the snapshot like game RAM (uniform/consistent). Read
					// GAME_STAGE_KERNEL CACHED: post-resume the dcache is empty (power
					// cycle), so cached reads fetch the uncached-captured bytes from RAM.
					io_ok &= save_region_records(fd, g_stage_base, KERNEL_RAM_SIZE,
					                             use_iobuf_kn, 1, &kbytes, NULL, "kernel");
				}
				prog_done(kbytes);
				ms_test_cp(24, kbytes, use_iobuf_kn ? "kernel-saved-to-MS-IOBUF(compressed)" : "kernel-saved-to-MS-perchunk(compressed)");
			}
			TLOG("[TIME] kernel compress+write ms=", t_mark);

			// Patch kernel_pos now (cheap, one word). DEFER the whole-file CRC: it
			// re-reads the file (~1.2s) but only touches the finished MS file, not
			// any frozen game state — so we compute it AFTER resuming the game (see
			// below), off the visible freeze, the way the original V2 streamed its
			// buffer to MS with the game already running.
			{
				u32 kp = (u32)kernel_pos;
				sceIoLseek(fd, (SceOff)(7 * sizeof(u32)), PSP_SEEK_SET);
				io_ok &= io_write_all(fd, &kp, sizeof(kp));
			}

			// GATE 2: only certify the file (write its CRC below) if EVERY write
			// succeeded. If a kernel-section/patch write short-wrote, leave do_bg_crc
			// = 0 so no valid CRC is stamped -> next load's CRC check rejects the
			// truncated file instead of loading garbage. Report failure via the log.
			do_bg_crc = io_ok;
			if (!io_ok) WriteDebugLogRaw("[FSAVE] short write during kernel section (MS full?) -> file left uncertified");
		} else {
			// LOAD resume: do NOT write the savegame. Open read-only.
			fd = sceIoOpen(path, PSP_O_RDONLY, 0);
			if (fd < 0) {
				// The save-time kernel was already applied, so ALL game RAM is stale
				// relative to it and nothing is restored. Do NOT resume — leave frozen
				// for a clean power-cycle (baseline FreezeLoad policy).
				ms_test_cp(29, (u32)fd, "reopen-RO-FAILED-frozen-poweroff");
				WriteDebugLogRaw("[FLOAD] RO reopen failed post-resume -> game left frozen; power off");
				return op_frozen_fail("Load failed", "Power off the PSP");   // RAM clobbered -> cannot resume
			}
			ms_test_cp(30, 0, "LOAD-resume: NO MS write");
		}

		// LOAD: the kernel rollback wiped the pre-suspend panel (the resumed tail carries
		// the SAVE-TIME panel state). Rebuild it as "Loading" and re-show the two pre-
		// suspend steps: the labels are fixed and the rates rode across the apply in the
		// load-carry block (g_lc.verify_mbps/read_mbps, patched into the snapshot beside
		// the LOAD marker). "Restoring RAM/VRAM" is appended below. Uses the captured-
		// buffer path (prog_* -> dbg_print_all), safe post-apply.
		if (g_op_mode == 1) {
			strncpy(g_prog_title, "Loading", sizeof(g_prog_title) - 1); g_prog_title[sizeof(g_prog_title) - 1] = 0;
			strcpy(g_prog_label[0], "Verifying save"); g_prog_mbps[0] = g_lc.verify_mbps;
			strcpy(g_prog_label[1], "Reading kernel"); g_prog_mbps[1] = g_lc.read_mbps;
			g_prog_n = 2;
			// The apply rolled plugin BSS back to the SAVE-TIME values, so re-seed the bar
			// bookkeeping for the LOAD directly: verify + read-kernel (weighted to land at
			// 500/1000, matching where SAVE settles pre-suspend) are credited as earned;
			// "Restoring RAM/VRAM" (begun just below, weight 500) is the running one.
			g_prog_active = 1;
			g_prog_base_pm = W_LOAD_VERIFY + W_LOAD_READKN;
			g_prog_seg_pm = 0; g_prog_bar_last = 0;
			prog_draw(1);
		}

		// Restore game RAM (clobbered by staging) — read-only decompress, both paths.
		// Reset the timer marker with a FRESH timestamp: on a LOAD this tail runs
		// as the save-time continuation, so the carried-over t_mark is stale.
		t_mark = now_us();
		prog_step("Restoring RAM/VRAM", g_op_mode == 1 ? W_LOAD_RESTORE : W_SAVE_RESTORE, PB_RESTORE_MS);
		// Batch the restore reads through IOBUF only when the MPU open verifies
		// and holds (post-resume the nibble state is whatever resumeSysmem put
		// back); per-chunk work_buf reads otherwise.
		int use_iobuf = g_compress ? iobuf_open_verified() : 0;   // Fast restore reads direct into game RAM — no MPU unlock
		sceIoLseek(fd, RAM_SECTION_OFFSET, PSP_SEEK_SET);   // 64KB-aligned section start (see writer)
		offset = 0;
		// This tail only ever restores a file THIS same instance wrote, so g_compress
		// matches the file's format (on a load-resume it's the save-time value carried
		// in the kernel snapshot). Fast saves are raw blobs — DMA them straight back.
		if (!g_compress) {
			TM_BEGIN1(lr_rt, lr_tm);
			if (read_region_direct(fd, MAIN_RAM_BASE_CACHED, MAIN_RAM_SIZE)) offset = MAIN_RAM_SIZE;
			TM_LAP(lr_rt, lr_tm);
			TM_LOG("[TIME] RAM read ms=", lr_rt);
		} else {
			offset = restore_region_records(fd, MAIN_RAM_BASE_CACHED, MAIN_RAM_SIZE, use_iobuf, "RAM");
		}
		// Restore VRAM too (LOAD never decompressed it pre-suspend; on SAVE it is
		// the same data the game already has, so harmless).
		int restore_ok = (offset >= MAIN_RAM_SIZE);   // RAM loop reached the end?
		sceIoLseek(fd, vram_pos, PSP_SEEK_SET);
		if (!g_compress) {
			// Fast save: raw VRAM blob at vram_pos — stage back through work_buf (eDRAM, no DMA).
			TM_BEGIN1(lv_rt, lv_tm);
			if (!read_vram_staged(fd)) restore_ok = 0;
			TM_LAP(lv_rt, lv_tm);
			TM_LOG("[TIME] VRAM read ms=", lv_rt);
		} else {
			if (restore_region_records(fd, VRAM_BASE_CACHED, VRAM_SIZE, use_iobuf, "VRAM") < VRAM_SIZE)
				restore_ok = 0;   // VRAM restore incomplete
		}
		ClearCaches();
		was_load = (g_op_mode == 1);   // capture before the reset below (used by the "time:" line)
		// Freeze the op's end-to-end wall-clock NOW (restore done, game about to resume) —
		// before the "Press X" wait — so it excludes the user's read time. SAVE uses the
		// live t_start; LOAD uses the start tick carried across the apply (its own t_start
		// here is the abandoned save-time capture's, not the load's).
		{
			u64 op_start = was_load ? (((u64)g_lc.start_hi << 32) | (u64)g_lc.start_lo) : t_start;
			op_elapsed_ms = (u32)(now_us() - op_start) / 1000;   // cast to u32 BEFORE /1000 -> 32-bit divide (no __udivdi3); a whole op fits u32 µs
		}
		g_op_mode = 0; // reset for the next op
		if (restore_ok) prog_done_redim(MAIN_RAM_SIZE + VRAM_SIZE);   // re-dims + redraws the panel over the VRAM the restore just repainted
		ms_test_cp(25, offset, restore_ok ? "RAM+VRAM-restored-OK" : "gameRAM-restore-FAILED");
		TLOG("[TIME] RAM+VRAM restore (read+decompress) ms=", t_mark);
		// Flush the RAM trace HERE: everything from here on is either the "press
		// X to continue" prompt (waiting on the user) or background CRC overlapped
		// with the already-resumed game — not part of the freeze being timed.
		trace_flush("[TRACE] FreezeSave (start .. RAM+VRAM restore)");
		// GATE: a transient MS read error mid-restore leaves half-restored,
		// incoherent game RAM/VRAM. Resuming into it can crash the game or corrupt
		// its in-game save, so: do NOT resume — leave the game frozen for a clean
		// power-cycle and report failure. (For a SAVE the file on MS is complete
		// except its CRC; a re-save after power-cycle is the recovery. We
		// deliberately do not certify it here.)
		if (!restore_ok) {
			WriteDebugLogRaw("[FSAVE] RAM/VRAM restore incomplete post-resume -> game left frozen; power off");
			if (fd >= 0) sceIoClose(fd);
			return op_frozen_fail(g_op_mode ? "Load failed" : "Save failed", "Power off the PSP");   // half-restored RAM -> cannot resume
		}
	}

	// Save/load done; game still frozen. Show "Press X to continue" and, in the SAME
	// loop, run the finalize work that doesn't need the freeze — the Game.thb copy and
	// the file CRC — so it overlaps the user's read time instead of waiting for X. Resume
	// the instant X is pressed; finish any leftover CRC with the game running. LOAD
	// re-entry / a failed save (io_ok==0, e.g. the kernel-section short write reached
	// here post-resume) has do_bg_crc=0, so it just waits for X (no CRC, no Game.thb) and
	// shows the red failure notice below. (A PRE-suspend save failure never reaches here —
	// GATE 1 handles it via op_abort.)
	{
		SceCtrlData pad; int prev, xhit = 0;
		u32 crc = CRC32_INIT; int crc_active = (do_bg_crc && fd >= 0);
		SceOff crc_off = (SceOff)sizeof(header);   // running CRC read offset; aligned reads via crc_read_next
		if (g_game_pcb_count > 0) reassert_display();   // un-blank the screen for the prompt

		if (do_bg_crc) copy_game_thumb(path);           // newest save's thumb -> Game.thb

		// Final readout on a solid-black panel. On a short write (MS full / card error)
		// the file is uncertified (rejected on load), so show a RED "Save incomplete /
		// Not enough space" instead of the normal green prompt. Otherwise: the stats +
		// green prompt. Both wait for X, then resume below.
		if (!io_ok) {
			draw_fail_panel("Save incomplete", "Not enough space");
			prompt_pill(PANEL_TOP + 7, "Press X to continue", BR_GREEN, BR_RED);
		} else {
			// Repaint the card (accent title + steps + full bar), then swap the title line
			// for the green "done" prompt — inside the card, preserving the accent stripe.
			prog_draw(2);
			dbg_fill_rect_all(CARD_X + 6, (PANEL_TOP + 1) * 8, CARD_W - 12, 8, BR_CARD);
			dbg_transparent = 1; dbg_fg = BR_GREEN; dbg_bg = BR_CARD;
			dbg_row = PANEL_TOP + 1; dbg_col = (60 - 19) / 2; dbg_print_all("Press X to continue");
			// On-screen op time: bright green text drawn straight on top of the bar (which
			// prog_draw(2) above already filled to 100% with its accent color) — no backing
			// rect, nothing else added. SAME frozen op_elapsed_ms as the log line (captured
			// at restore-done, excludes the user's read time).
			{
				char tl[48];
				sprintf(tl, "%s time: %u.%03us", was_load ? "Load" : "Save",
				        (unsigned)(op_elapsed_ms / 1000), (unsigned)(op_elapsed_ms % 1000));
				dbg_transparent = 1; dbg_fg = BR_GREEN;
				dbg_row = BAR_TEXT_ROW; dbg_col = (60 - (int)strlen(tl)) / 2; if (dbg_col < 0) dbg_col = 0;
				dbg_print_all(tl);
				// Log the SAME line to UART HERE — before the "Press X" wait below — so it
				// appears on the wire when the op is actually done, not after the user
				// finally presses X (which was the "time line appears after X" complaint).
				WriteDebugLogRaw(tl);
			}
		}

		u64 crc_t0 = 0;   // CRC verify start (own var — TLOG below clobbers t_mark)
		if (crc_active) { t_mark = crc_t0 = now_us(); sceIoLseek(fd, crc_off, PSP_SEEK_SET); }
		kpeek(&pad); prev = pad.Buttons;
		while (!xhit) {
			if (crc_active) {                            // advance the CRC one 64KB-aligned chunk (no sleep)
				if (!bg_crc_step(fd, &crc_off, &crc, &io_ok)) {
					crc_active = 0;
					ms_test_cp(26, crc, "bg-file-CRC-written(during-prompt)");
					TLOG("[TIME] bg CRC (interleaved) ms=", t_mark);
					// The CRC finished WHILE the user is still on the prompt (hasn't pressed
					// X). Show a new line below the last stat with its verify MB/s, so the
					// user sees the background verify completed. (If X was pressed first,
					// the CRC finishes post-resume and this never runs — no panel then.)
					{
						u32 us = (u32)(now_us() - crc_t0);   // crc_t0, NOT t_mark (TLOG just reset t_mark)
						u32 crc_bytes = (u32)(crc_off - (SceOff)sizeof(header));   // bytes hashed (after header)
						u32 mbps100 = (u32)prog_mbps100(crc_bytes, us);   // 32-bit (no __udivdi3)
						char cl[48];
						int lastrow = PANEL_TOP + STEP_TOP + (g_prog_n > 0 ? (g_prog_n - 1) : 0) * STEP_GAP;
						int crow = lastrow + STEP_GAP;
						if (crow > PANEL_TOP + STEP_AREA_ROWS) crow = PANEL_TOP + STEP_AREA_ROWS;   // clamp: stay above the bottom progress bar
						sprintf(cl, "CRC verify complete  %u.%02u MB/s",
						        (unsigned)(mbps100 / 100), (unsigned)(mbps100 % 100));
						{
							char clv[16];
							int lcol = CARD_X / 8 + 4, rend = (CARD_X + CARD_W) / 8 - 3;
							sprintf(clv, "%u.%02u MB/s", (unsigned)(mbps100 / 100), (unsigned)(mbps100 % 100));
							dbg_transparent = 1; dbg_fg = BR_GREEN;
							dbg_row = crow; dbg_col = lcol; dbg_print_all("CRC verify complete");   // label left
							dbg_col = rend - (int)strlen(clv); dbg_print_all(clv);                  // MB/s right
							dbg_transparent = 0;
						}
						WriteDebugLogRaw(cl);
					}
				}
			} else {
				sceKernelDelayThread(40000);
			}
			kpeek(&pad);
			if ((pad.Buttons & ~prev) & PSP_CTRL_CROSS) xhit = 1;
			prev = pad.Buttons;
		}
		wait_buttons_up();                               // don't leak the continue-X into the game (BufferPositive)
		// Also suppress the continue-X across the latch AND buffer-backlog paths (see
		// arm_input_suppress) — both self-clear/self-drain on their own now, so the
		// 100ms delay + explicit clear below is just a belt-and-suspenders backstop
		// for latch specifically, not load-bearing. Snapshot already written (flag
		// was 0 at capture).
		arm_input_suppress();
		resume_game_threads();
		sceKernelDelayThread(100000);                    // ~100ms: game reads+drains the latch (latch input only)
		g_suppress_latch = 0;

		// Finish the CRC if X interrupted it mid-hash — the game is RUNNING now, so YIELD between
		// chunks. Reading the file back-to-back with no sleep (as during the frozen prompt) starves
		// the game's own Memory-Stick reads -> the massive lag. A short sleep per chunk lets the
		// game's reads interleave; the leftover is small (the prompt loop advanced most of it).
		while (crc_active) {
			sceKernelDelayThread(2000);                  // 2ms: hand the resumed game a Memory-Stick window
			if (!bg_crc_step(fd, &crc_off, &crc, &io_ok)) {
				crc_active = 0;
				ms_test_cp(26, crc, "bg-file-CRC-written(after-resume)");
			}
		}
	}

	if (fd >= 0)
		sceIoClose(fd);

	WriteDebugLogHex("[FSAVE] ram_bytes=", ram_bytes);
	WriteDebugLogHex("[FSAVE] vram_bytes=", vram_bytes);
	// Report failure if any write short-wrote (the file was left uncertified above,
	// so it will be rejected on load) — never a false "completed" to the caller/UI.
	if (!io_ok) {
		// Delete the incomplete file so it doesn't clutter the save list (it would be
		// rejected on load anyway). fd is already closed above. Safe: io_ok is only ever
		// cleared by a failed sceIoWrite, and a LOAD's resumed tail never writes the
		// savegame — so io_ok stays 1 there and this block is a real failed SAVE only,
		// with path = that save's own file.
		sceIoRemove(path);
		WriteDebugLogRaw("[FSAVE] Save FAILED (short write) — incomplete file deleted.");
		return -1;
	}
	// (The "Save/Load time:" UART line is emitted UP at the panel-draw point, before
	// the "Press X" wait — not here — so it lands on the wire when the op finishes.)
	// Post-op one-shot thread + DMA sample: the Class-B failure (loads that run a
	// few seconds then freeze) dies AFTER this point, so this snapshot of the
	// resumed game's threads is the last state we can log before the game runs.
	if (was_load) { diag_profile("post-load"); diag_threads("post-load", game_tids, tcount); }
	WriteDebugLog(was_load ? "[FLOAD] Full load completed." : "[FSAVE] Full save completed.");
	return 0;
}

// ────────────────────────────────────────────────────────────
// Stage 2 — LOAD (M5 kernel apply): verify the save file, stage its kernel
// snapshot + the ApplyHandoff blob in game RAM, and arm the firmware suspend
// that overwrites the live kernel at PHASE0_0. On success the resume enters
// the SAVE-TIME session (the save-time poll thread continues FreezeSave's
// tail, which restores game RAM/VRAM from this same file) — this function
// never returns then. Same freeze bracket as FreezeSave: suspend_escalating /
// resume_game_threads, plain sceIo* throughout.
// ────────────────────────────────────────────────────────────
int FreezeLoad(const char *path)
{
	SceUID fd;
	u32 header[SAVE_HEADER_WORDS];
	u32 kernel_size;
	u32 offset;
	SceUID game_tids[MAX_SUSPEND_THREADS];
	int live_tcount = 0;

	WriteDebugLog("[FLOAD] Opening file...");
#if DEBUG_BUILD
	if (DBG_UART()) { char gb[96]; sprintf(gb, "[GAME] %.40s id=%.16s", g_game_title[0] ? g_game_title : "?", umdid[0] ? umdid : "?"); uart_puts(gb); }
#endif
	diag_profile("load-op");   // pre-freeze game DMA / volatile state
	// Stamp the LOAD's wall-clock start (RTC µs, monotonic across the suspend). This
	// rides the load-carry block into the snapshot (patched below) so the resumed
	// save-session tail can print the true end-to-end "Load time".
	{ u64 ls = now_us(); g_lc.start_lo = (u32)ls; g_lc.start_hi = (u32)(ls >> 32); }
	fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd < 0) {
		WriteDebugLogHexRaw("[FLOAD] FAILED open, fd=", (u32)fd);
		return -1;
	}

	dbg_capture_both_bufs();
	ms_test_row = 0;

	// ── Identify the live "game thread" set BEFORE freezing (IRQs on, ordinary
	// enumeration), same filter as FreezeSave (no status needed here). Used to
	// match saved context entries to live threads for the round-trip restore. ──
	live_tcount = identify_game_threads(game_tids, NULL);

	// Ask the game to release its volatile-memory lock before we freeze it (same as the
	// save path) so the load's kernel-rollback suspend can sleep instead of stalling at
	// PHASE1_2. Abort the load cleanly if it won't let go within 5s.
	if (cooperative_volmem_release(game_tids, live_tcount) < 0) {
		WriteDebugLogRaw("[FLOAD] game never released volatile mem -> ABORT load");
		return op_abort(game_tids, live_tcount, fd, NULL, NULL, NULL);   // running (not frozen) -> silent; load makes no file
	}

	// Un-blank BEFORE the freeze (the game blanked in its SUSPENDING handler) so the
	// freeze wait doesn't sit on a black screen — see the matching FreezeSave note.
	if (g_game_pcb_count > 0) { reassert_display(); prog_begin("Loading"); }

	// ── QUIESCENT-POINT FREEZE (Stage 3): per-thread suspend of ONLY user
	// threads, tracked for symmetric resume (see FreezeSave note). Kernel
	// threads stay alive. ──
	{
		// If any game thread won't freeze, do NOT apply the loaded state over a running
		// thread (crash/corruption) — resume and abort the load cleanly. Retried with
		// me_rpc_probe() like the SAVE loop: a frozen ME-RPC mutex owner would wedge the
		// 0x402 descent veto (the old "[FLOAD] descent vetoed" hang). Probing HERE —
		// before the snapshot is staged over game RAM — means even the abort is fully
		// resumable, unlike the post-request frozen-fail ("power off") further down.
		{
			int attempt, ok = 0;
			for (attempt = 0; attempt < 20 && !ok; attempt++) {
				if (attempt) sceKernelDelayThread(3000);       // ~3ms: let a released holder unlock
				if (suspend_escalating(0, 50) != 0) { resume_game_threads(); continue; }
				if (me_rpc_probe() == 0) { ok = 1; break; }
				resume_game_threads();                          // frozen ME-mutex holder -> unfreeze, retry
			}
			if (!ok) {
				WriteDebugLogRaw("[FLOAD] freeze/ME-probe FAILED -> ABORT load");
				return op_abort(game_tids, live_tcount, fd, NULL, NULL, NULL);   // running (never committed a freeze) -> silent
			}
		}
		// Post-freeze snapshot (compare vs GTA): game DMA in flight? thread wait states?
		diag_profile_frozen("load-frozen");
		diag_threads("load-frozen", game_tids, live_tcount);
	}

	// Land the load-phase overlay on the VISIBLE buffer with a matching stride (see the
	// matching FreezeSave note + pin_current_display). Blanking games: re-point at the
	// pre-blank capture. Non-blanking games: adopt+pin the frozen front buffer (fixes the
	// intermittent banner-shift / no-banner).
	if (g_game_pcb_count > 0) reassert_display();
	else                      pin_current_display();

	prog_begin("Loading");   // weighted bar (W_LOAD_*): verify, read kernel, suspend, restore

	if (sceIoRead(fd, header, sizeof(header)) != sizeof(header)) {
		ms_test_cp(40, 0xFFFFFFFF, "header-read-FAILED");
		return op_abort(game_tids, live_tcount, fd, NULL, "Load failed", "Bad save file");
	}
	if (header[0] != SAVESTATE_MAGIC) {
		ms_test_cp(40, header[0], "BAD-MAGIC-FAILED");
		return op_abort(game_tids, live_tcount, fd, NULL, "Load failed", "Bad save file");
	}
	kernel_size   = header[6];   // game RAM/VRAM/GE/thread sizes are read by the resumed save-time tail, not here
	// Fast (uncompressed) format flag — top bit of header[11] (see SAVE_FLAG_UNCOMPRESSED).
	// Read the FILE's format, not the current g_compress, since a load may run under a
	// different setting than the save used.
	int uncompressed = (header[11] & SAVE_FLAG_UNCOMPRESSED) != 0;
	// Version mismatch is just logged, not rejected — we're actively
	// iterating and the version number bumps on every build.
	ms_test_cp(40, header[1], "header-validated(version)");

	// ── INTEGRITY CHECK: CRC32 the file (after the header) and compare to the
	// value stored at save time, BEFORE we commit to the load. Loading overwrites
	// the live kernel during the suspend, so applying a corrupt snapshot can
	// crash/corrupt the system — reject here instead. Same byte range as the save
	// computed (after the header .. EOF), so it matches regardless of any tail. ──
	{
		u32 want_crc = header[8];
		u32 crc_bytes = 0;
		SceOff flen = sceIoLseek(fd, 0, PSP_SEEK_END);   // bytes for the on-screen verify rate; crc32_file_after_header re-seeks itself
		if (flen > (SceOff)sizeof(header)) crc_bytes = (u32)(flen - (SceOff)sizeof(header));
		prog_step("Verifying save", W_LOAD_VERIFY, PB_VERIFY_MS);   // the CRC re-reads the whole file (~seconds) — the main pre-suspend wait
		u32 crc = crc32_file_after_header(fd, sizeof(header));
		if (crc != want_crc) {
			ms_test_cp(41, crc, "CRC-MISMATCH-ABORT");
			WriteDebugLogHexRaw("[FLOAD] CRC want=", want_crc);
			WriteDebugLogHexRaw("[FLOAD] CRC got =", crc);
			// Resumable (RAM still intact — the verify runs BEFORE any staging): show the
			// red CRC-fail notice + "Press X to continue", then resume the game unharmed.
			return op_abort(game_tids, live_tcount, fd, NULL, "Save CRC check failed", "Load aborted");
		}
		prog_done(crc_bytes);
		ms_test_cp(41, crc, "file-CRC-OK");
	}

	// ── M5 LOAD: apply the save-time kernel via firmware suspend, then let the
	// firmware RESUME + the save-time continuation bring the system to the save
	// point. We do NOT decompress game RAM/VRAM here: the save-time kernel
	// snapshot captured the poll thread mid-FreezeSave, so on resume that thread
	// runs FreezeSave's tail, which restores the full 24MB game RAM (+VRAM) from
	// THIS SAME file. So loading = "let the save finish." We only need to stage
	// the kernel snapshot + the apply routine in game RAM and overwrite the
	// kernel at PHASE0_0. ──
	{
		u32 kpos = header[7];
		// Load path stages ApplyHandoff (one-way: copy + cache flush + jump to
		// the save-time dispatcher context).
		const unsigned char *fnsrc = (const unsigned char *)((void *)ApplyHandoff);
		unsigned int fnlen = (unsigned int)(ApplyHandoffEnd - fnsrc);
		volatile unsigned char *slot;
		unsigned int k;

		// The apply-blob location is FIXED at g_apply_base (0x89800000) — mode 10 in
		// utils.c executes from this same address. (Was a user-swept setting; pinned.)
		slot = (volatile unsigned char *)(g_apply_base + KSEG1_ALIAS);
		ms_test_cp(38, g_apply_base, "apply-blob-base");

		if (kpos == 0 || kernel_size != KERNEL_RAM_SIZE) {
			ms_test_cp(42, kpos, "bad-kernel-pos/size-FAILED");
			return op_abort(game_tids, live_tcount, fd, NULL, "Load failed", "Corrupt save");   // pre-staging -> RAM intact
		}
		// Select the staging slot BEFORE decompressing/patching — the snapshot and
		// the g_op_mode patch below MUST land in the same slot the PHASE0_0 apply
		// reads from (mirrors the SAVE path, which sets g_stage_base up front).
		// g_stage_spot is permanently 1 (Mid) — see its own comment.
		g_stage_base = g_stage_bases[1];

		// Read the snapshot into the game-RAM staging slot (uncached alias, so
		// it lands in RAM for the apply routine). This clobbers the top of game
		// RAM — fine, the save-time tail restores all 24MB from the file.
		ms_test_cp(42, kernel_size, "kernel-read-to-gameRAM");
		prog_step("Reading kernel", W_LOAD_READKN, PB_READKN_MS);
		{
		u64 t_mark = now_us();
		sceIoLseek(fd, (SceOff)kpos, PSP_SEEK_SET);
		offset = 0;
		if (uncompressed) {
			// Fast save: raw kernel blob — DMA straight into the staging slot. The
			// WritebackInvalidate in read_region_direct lands it in RAM for the
			// uncached mode-10 apply (like the decompress+ClearCaches path does).
			TM_BEGIN1(lk_rt, lk_tm);
			if (read_region_direct(fd, g_stage_base, KERNEL_RAM_SIZE)) offset = KERNEL_RAM_SIZE;
			TM_LAP(lk_rt, lk_tm);
			TM_LOG("[TIME] LOAD kernel read ms=", lk_rt);
		} else {
			// Decompress to CACHED g_stage_base; the ClearCaches before the suspend
			// (below) flushes it to RAM so the uncached apply (mode 10) reads it.
			// Always per-chunk (no IOBUF read-ahead on this path).
			offset = restore_region_records(fd, g_stage_base, kernel_size, 0, "LOAD kernel");
		}
		ms_test_cp(43, offset, (offset < kernel_size) ? "kernel-decomp-INCOMPLETE-FAILED" : "kernel-decompressed");
		TLOG("[TIME] LOAD kernel read+decompress ms=", t_mark);
		}
		sceIoClose(fd);
		fd = -1;
		// Partial staging already clobbered game RAM (and there's no restore on this abort
		// path), so we CANNOT resume — leave frozen for a power-cycle.
		if (offset < kernel_size) return op_frozen_fail("Load failed", "Power off the PSP");
		prog_done(KERNEL_RAM_SIZE);   // effective read+decompress rate (4MB uncompressed); rate carried below via the marker patch

		// Copy the apply routine into game RAM (uncached alias).
		for (k = 0; k < fnlen; k++) slot[k] = fnsrc[k];

		// Mark this as a LOAD so the save-time continuation (which re-enters
		// FreezeSave's tail after the kernel rollback) does a READ-ONLY restore
		// and never writes the savegame. Set the live flag (covers g_op_mode
		// being outside the captured window) AND patch the staged snapshot's
		// copy (the value the restored kernel actually sees). The flash-driver
		// repair the rollback needs runs on RESUME_COMPLETED in utils.c (in-place
		// lflash df_exit/df_init + flash1: remount, keyed on the patched op_mode == 1).
		{
			// Use the SAVE-TIME &g_op_mode (header[9]) — NOT our own, which differs
			// every boot — so the patch lands on the flag's real offset in the saved
			// snapshot. Using the live address here corrupts an unrelated kernel word
			// (the plugin moved) and freezes the apply (observed cross-session).
			u32 opaddr = header[9];
			g_op_mode = 1;
			WriteDebugLogHex("[FLOAD] save-time &g_op_mode=", opaddr);
			if (opaddr >= KERNEL_RAM_BASE && opaddr + 16 < KERNEL_RAM_BASE + KERNEL_RAM_SIZE) {
				// Patch the CACHED snapshot (we decompressed into cached
				// GAME_STAGE_KERNEL) so ClearCaches below flushes the patch to RAM
				// together with the snapshot. (Patching the uncached alias would be
				// overwritten by that writeback -> reverting g_op_mode to SAVE.)
				u32 lc = g_stage_base + (opaddr - KERNEL_RAM_BASE);   // save-time g_lc in the staged snapshot
				*(volatile u32 *)(lc + 0) = 1;                        // op_mode = LOAD
				// Carry the two pre-suspend load rates (verify_mbps at +4, read_mbps at
				// +8, contiguous with op_mode in struct load_carry) into the snapshot so
				// the resumed tail can re-show "Verifying save"/"Reading kernel" with rates.
				*(volatile int *)(lc + 4) = (g_prog_n > 0) ? g_prog_mbps[0] : 0;
				*(volatile int *)(lc + 8) = (g_prog_n > 1) ? g_prog_mbps[1] : 0;
				// LOAD start tick (start_lo at +12, start_hi at +16) -> resumed tail
				// computes the end-to-end "Load time".
				*(volatile u32 *)(lc + 12) = g_lc.start_lo;
				*(volatile u32 *)(lc + 16) = g_lc.start_hi;
				// (CP44 success log removed — the handoff-ctx-addr-ok CP44 below covers it.)
			} else {
				// The save-time &g_op_mode lies outside the 8MB kernel window
				// (corrupt/hostile header, or plugin loaded elsewhere at save
				// time). We CANNOT patch the snapshot's LOAD flag, so on resume the
				// save-time tail would take its SAVE branch and overwrite + re-CRC
				// the user's save file (self-certifying corruption). ABORT instead
				// of arming the suspend.
				ms_test_cp(44, opaddr, "g_op_mode-OUTSIDE-window-FAILED-ABORT");
				return op_frozen_fail("Load failed", "Power off the PSP");   // snapshot already staged over game RAM -> cannot resume
			}
		}

		// Carry the CURRENT session's overclock setting through the kernel rollback:
		// without this, the restored kernel would keep whatever g_overclock_id this
		// PARTICULAR save was made with (the kernel rollback restores the plugin's
		// own globals to save-time state, same reason g_op_mode needs patching above)
		// — silently changing the user's overclock choice based on which save they
		// loaded. Best-effort, unlike g_op_mode/g_handoff_ctx above: a bad address
		// here just means the setting doesn't carry over (falls back to the save's
		// own value, i.e. pre-existing behavior) — NOT worth aborting an otherwise-
		// good load over.
		{
			u32 ocaddr = header[12];
			if (ocaddr >= KERNEL_RAM_BASE && ocaddr + 4 <= KERNEL_RAM_BASE + KERNEL_RAM_SIZE && (ocaddr & 3) == 0) {
				u32 oc = g_stage_base + (ocaddr - KERNEL_RAM_BASE);
				*(volatile u32 *)oc = (u32)g_overclock_id;
				WriteDebugLogHex("[FLOAD] carried current overclock id=", (u32)g_overclock_id);
			} else {
				WriteDebugLogHex("[FLOAD] overclock carry SKIPPED (bad save-time addr)=", ocaddr);
			}
		}

		// Save-time dispatcher-context address for the one-way handoff. ApplyHandoff
		// reads a 64-byte block here FROM THE APPLIED IMAGE and jumps to its ra, so a
		// bad address means jumping through garbage — validate it lies fully inside
		// the kernel window, like the g_op_mode patch above. An old 10-word save has
		// RAM-chunk data in this slot and is rejected here (re-save with this build).
		{
			u32 ctxaddr = header[10];
			if (ctxaddr < KERNEL_RAM_BASE ||
			    ctxaddr + sizeof(g_handoff_ctx) > KERNEL_RAM_BASE + KERNEL_RAM_SIZE ||
			    (ctxaddr & 3) != 0) {
				ms_test_cp(44, ctxaddr, "handoff-ctx-OUTSIDE-window-FAILED-ABORT");
				return op_frozen_fail("Load failed", "Power off the PSP");   // snapshot already staged over game RAM -> cannot resume
			}
			g_handoff_ctx_addr = ctxaddr;
			ms_test_cp(44, ctxaddr, "handoff-ctx-addr-ok");
		}

		// Keep game threads frozen; wake VSH/system for the suspend handshake.
		resume_nongame_threads(game_tids, live_tcount);
		ms_test_cp(45, 0, "arming-kernel-apply-suspend");
		g_resumed = 0;
		// Always apply the full 8MB kernel window.
		g_kcap_off = 0; g_kcap_size = KERNEL_RAM_SIZE;
		// g_stage_base was selected above (before decompress) — the apply reads the
		// same slot the snapshot was decompressed and g_op_mode-patched into.
		// Apply at PHASE0_0 (fully quiesced) — overwriting the live kernel pre-suspend is fatal.
		g_cap_phase = SCE_SYSTEM_SUSPEND_EVENT_PHASE0_0;
		g_sleep_mode = 10;   // apply routine overwrites the kernel at g_cap_phase
		g_sleep_arm = 1;
		ClearCaches();
		// (No ME 0x402-veto handling needed here: the me_rpc_probe at this load's
		// freeze guaranteed no FROZEN thread owns the SceMediaEngineRpc mutex, and
		// frozen threads can't acquire it afterwards — so the descent's veto can
		// only see LIVE holders, which release on their own, exactly like a native
		// sleep. The v528 mailbox bypass at 0xBFC00700 was removed with that.)
		scePowerRequestSuspend(); // async; on success the firmware freezes us here and resumes
		// into the SAVE-TIME kernel — the apply OVERWRITES this plugin, so we never return. (That's
		// why we can't use wait_for_resume() like SAVE, whose thread survives a read-only capture.)
		// FAILURE GUARD (descent-aware): on success we freeze inside the poll below and never
		// run past it; if we ARE still running, g_sleep_arm distinguishes the two failures:
		//  - still 1 after ~2s: the 0x401 power-lock never fired — the request never
		//    started; abort.
		//  - == 2: a descent IS in flight but the 0x401/0x402 handshake keeps retrying.
		//    A frozen ME-RPC mutex holder (the historic wedge) is prevented by the
		//    me_rpc_probe at this load's freeze, so a persistent BUSY means some OTHER
		//    vetoer (syscon packets / ctrl transfer — kernel-side, self-clearing).
		//    Ride the full ~5s cap, re-pushing the auto-wake alarm so a late descent
		//    still has a LIVE alarm (the +2s alarm armed at the 0x401 goes past-due
		//    during this wait, and a past-due alarm has historically been a lost wake:
		//    console asleep until a manual power press). Then abort gracefully. There
		//    is no thread-resume rescue anymore: v524's rescue woke runnable threads
		//    onto RAM already clobbered by the staged snapshot and hard-froze Dissidia
		//    Duodecim — a graceful "power off" beats a hard freeze.
		{
			int spin, waited = 0;
			for (spin = 0; spin < 100; spin++) {     // ~5s safety cap
				sceKernelDelayThread(50000);         // 50ms
				waited += 50;
				if (g_sleep_arm == 2) {
					if ((waited % 1000) == 0) {      // keep the wake alarm live while the descent can still fire
						u64 tick;
						sceRtcGetCurrentTick(&tick);
						sceRtcTickAddSeconds(&tick, &tick, 5);
						sceRtcSetAlarmTick(&tick);
					}
				} else if (waited >= 2000) {
					break;                           // request never started within 2s -> abort below
				}
			}
		}
		if (g_sleep_arm == 2)
			WriteDebugLogRaw("[FLOAD] descent still vetoed after ~5s (non-ME vetoer?) -> abort");
		// Disarm the mode-10 apply: the suspend never took, so if the user presses power
		// to SLEEP (instead of holding to power off) the native suspend would otherwise
		// apply the staged kernel snapshot outside the controlled flow (kernel overwrite).
		// Also drop the wake alarm — left registered it would wake the next native sleep
		// instantly (see the RESUME_COMPLETED handler, utils.c).
		g_sleep_arm = 0; g_sleep_mode = 0;
		sceRtcSetAlarmTick(NULL);
		WriteDebugLogRaw("[FLOAD] kernel-apply suspend NEVER FIRED -> ABORT (game left frozen; power off)");
		return op_frozen_fail("Load failed", "Power off the PSP");
	}
}



// Arm BOTH post-resume input-suppress mechanisms together — call right before
// resume_game_threads() at any point real button presses may have been sampled
// into the hardware's controller state while the game sat frozen (menu close,
// save/load trigger, an abort's notice-then-resume, or FreezeSave/FreezeLoad's own
// final Press-X resume). See the block comments on g_suppress_latch's hooks and
// suppress_posbuf_slots for what each one covers and how each clears itself.
void arm_input_suppress(void)
{
	g_suppress_latch = 1;
	g_suppress_posbuf_ts = sceKernelGetSystemTimeLow();   // boundary: samples after this are real
	g_suppress_posbuf_calls = SUPPRESS_POSBUF_CALLS;
}





// the game (the op finishes), reaps the probe, retries.
static SceUID g_probe_fd = -1;   // pending probe fd (reaped after an unfreeze)

// Returns 1 = MS usable (probe completed + closed). 0 = FAT lock held by a
// frozen thread (or probe error): caller MUST resume_game_threads() and then
// call ms_probe_reap() before retrying.
int ms_probe_after_freeze(void)
{
	SceInt64 res = 0;
	int i;
	g_probe_fd = sceIoOpenAsync("ms0:/SEPLUGINS/pspfatsave.prx", PSP_O_RDONLY, 0);
	if (g_probe_fd < 0) { g_probe_fd = -1; return 0; }   // couldn't even queue -> treat as busy
	for (i = 0; i < 10; i++) {                            // ~100ms budget (a free FAT open is ms-scale)
		int pr = sceIoPollAsync(g_probe_fd, &res);        // 0 = done, 1 = pending, <0 = error (uofw iofilemgr)
		if (pr == 0) {
			sceIoClose(g_probe_fd);                       // close the async fd (harmless if the open itself failed)
			g_probe_fd = -1;
			return 1;
		}
		if (pr < 0) return 0;                             // poll error: play safe = busy (reap handles the fd)
		sceKernelDelayThread(10000);                      // 10ms between polls
	}
	return 0;                                             // still pending: lock held by a frozen thread
}

// After resume_game_threads() the former lock holder runs on and the parked
// probe completes — wait for it and close the fd so the retry starts clean.
void ms_probe_reap(void)
{
	SceInt64 res = 0;
	if (g_probe_fd < 0) return;
	sceIoWaitAsync(g_probe_fd, &res);
	sceIoClose(g_probe_fd);
	g_probe_fd = -1;
}


