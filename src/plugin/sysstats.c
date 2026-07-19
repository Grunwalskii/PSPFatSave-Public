#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// ── Live FPS overlay (optional, drawn during actual gameplay — menu closed) ──
// Timing/counting design reverse-engineered from a comparable third-party
// FPS-counter plugin by disassembling its binary directly with psp-objdump/
// prxtool — NID lookups resolved against
// PSP_References/ARK-4-main/contrib/psplibdoc_660.xml. The prior sceRtc-based
// version of this hook left g_fps_value stuck at 0 in testing; this replaces
// the timing source and counting logic to match a scheme confirmed to work
// via that disassembly, not what the old comment here assumed.
//
// Hooks FIVE kernel functions by NID via sctrlHENFindFunction +
// sctrlHENPatchSyscall, tail-calling through to the real originals afterward:
//   module "sceDisplay_Service", library "sceDisplay":
//     0x289D82FE sceDisplaySetFrameBuf        — tier 1 tick source (below);
//                                                also gives the exact buffer/
//                                                stride/pixelformat the game
//                                                itself just passed, for
//                                                drawing at the right place.
//     0x984C27E7 sceDisplayWaitVblankStart    — tier 2 tick source.
//     0x46F186C3 sceDisplayWaitVblankStartCB  — tier 2 tick source (CB variant).
//   module "sceGE_Manager", library "sceGe_user":
//     0xAB49E76A sceGeListEnQueue             — tier 3 tick source.
//     0x1C0D95A6 sceGeListEnQueueHead         — tier 3 tick source.
// This is a fundamentally different mode from the rest of this file:
// everything else only draws while the game is FROZEN (the menu/confirm
// banner); this draws on every REAL frame while the game keeps running
// normally, so it must stay cheap.
//
// FPS timing uses sceKernelGetSystemTimeLow() (confirmed via the decompiled
// reference plugin's import table: it calls ThreadManForKernel NID
// 0x369ED59D for every tick), NOT sceRtc.
//
// A "tick" (fps_tick(), one countable frame-advance event) comes from a
// 3-tier cascade, each tier only ticking once the one above it has gone
// quiet — confirmed on real hardware that a game can silently stop calling
// EITHER SetFrameBuf (adaptive single-buffering under heavy render load —
// nothing to swap, so no reason to call it) or the vblank waits, while still
// rendering normally, so relying on just one signal isn't reliable:
//   Tier 1 — SetFrameBuf: ticks on EVERY call, no address-change requirement
//     (confirmed via the reference plugin's own decompiled hook: it stores
//     the incoming address unconditionally and ticks every call — some games
//     call this every frame with the SAME address, which an earlier
//     "only tick if the address changed" version of this hook silently
//     discarded as "not a new frame"). Updates g_fps_recent_setfb_time/
//     _count, read by tiers 2 and 3 below.
//   Tier 2 — WaitVblankStart/StartCB: ticks only when SetFrameBuf hasn't
//     fired at least 3 times in the trailing ~200ms (g_fps_recent_setfb_time/
//     _count) — lets a game that's stopped calling SetFrameBuf still get
//     ticks from vblank instead. Updates g_fps_recent_vblank_time, read by
//     tier 3.
//   Tier 3 — sceGeListEnQueue/EnQueueHead: the one kernel primitive EVERY
//     rendering technique must call to get anything drawn at all (sceGu is a
//     pure user-mode wrapper around it — no way to bypass it), so it's the
//     universal fallback for a game that's gone silent on BOTH tiers 1 and 2.
//     Only ticks once tier 1 AND tier 2 have both been quiet for a while (see
//     fps_maybe_tick_from_ge) — a game can legitimately submit several GE
//     lists per visual frame, so this must never fire alongside a working
//     tier 1/2 or it over-counts.
// Each tick feeds a sliding window whose length is g_show_fps_overlay's mode
// (1/0.5/0.2s, see fps_window_us()): once now - windowStart >= that length,
// fps = ticksInWindow * 1,000,000 / elapsed, then the window resets. Deltas
// closer together than FPS_REPRESENT_FLOOR_US are discarded as the same frame
// presented twice (see fps_tick) — this filters GTA's 3-5ms glitch re-present and
// Tomb Raider's 3-11ms thread/interrupt present pair.
//
// 1% Low (g_fps_show_lows): every accepted tick's frametime (the delta itself,
// in microseconds) is pushed into a 256-entry ring buffer — big enough to
// cover several real seconds of gameplay at 30-60fps while staying tiny in
// RAM. Sorting/scanning that buffer every single tick would cost real CPU, so
// it's only rescanned once per real second (g_fps_lows_last_calc_us),
// independent of the main FPS display's own window length. Each rescan pulls
// out the slowest 1% of recorded frametimes (~3 of 256), averages THOSE
// frametimes, then converts that average back to an FPS figure — the
// standard "worst 1% of frames" stutter metric (same idea as MSI Afterburner's
// 1% Low, just working in microsecond frametimes instead of milliseconds).
//
// All state explicitly zeroed in install_fps_overlay_hook() below, NOT trusted
// to "= 0" static initializers: GCC commonly folds explicit-zero statics into
// .bss as a size optimization, and this kernel PRX's .bss is NOT zeroed by the
// loader (see the "MUST be explicitly initialized" note at the very top of
// this file).
static int (*g_real_display_set_frame_buf)(void *, int, int, int) = NULL;
static int (*g_real_wait_vblank_start)(void) = NULL;
static int (*g_real_wait_vblank_start_cb)(void) = NULL;
static int (*g_real_ge_list_enqueue)(const void *, void *, int, void *) = NULL;
static int (*g_real_ge_list_enqueue_head)(const void *, void *, int, void *) = NULL;

u32  g_fps_last_tick_us;        // fps_tick(): last accepted tick timestamp (0 = none yet)
u32  g_fps_window_start_us;     // fps_tick(): start of the current averaging window
u32  g_fps_window_count;        // fps_tick(): ticks counted in the current window
static int  g_fps_value;               // final computed FPS — what gets drawn
u32  g_fps_recent_setfb_count;  // SetFrameBuf hook: consecutive-calls-within-200ms counter (cap 10)
static u32  g_fps_recent_setfb_time;   // SetFrameBuf hook: timestamp of its last call
u32  g_fps_recent_vblank_time;  // vblank fallback tick: timestamp of its last ACCEPTED tick (0 = none yet) —
                                        // gates the GE-list tier below it in the cascade (see fps_maybe_tick_from_ge)

#define FPS_FT_BUF_SIZE 128                          // ~2-4s of history at 30-60fps (shorter
                                                      // lookback than a 256 buffer -> 1% Low
                                                      // reacts to a recent stutter faster and
                                                      // forgets old ones sooner, at the cost of
                                                      // a smaller sample count per average)
#define FPS_LOW_K       ((FPS_FT_BUF_SIZE + 99) / 100) // slowest 1% of the buffer (2 of 128)
static u32 g_fps_ft_buf[FPS_FT_BUF_SIZE];             // ring buffer of recent tick frametimes (us)
int g_fps_ft_idx;                              // next write position (wraps)
u32 g_fps_lows_last_calc_us;                   // fps_calc_1pct_low(): last recalculation time
static int g_fps_low1_value;                          // final computed 1% Low FPS — what gets drawn

// Frametime histogram: one column per frame, full display width. 8.333ms is the
// 1px baseline and frametime ABOVE it grows the bar at 1.25ms/px. Examples:
// 70fps(14.3ms)->~6px, 60fps->~7px, 30fps->21px; ~107ms pegs.
// NOTE the baseline is now a pure display scale, NOT the arrival floor: it used to
// equal fps_tick's 8333us discard, so 1px really was the fastest frame that could
// reach the chart. FPS_REPRESENT_FLOOR_US has since moved to 12ms, so the shortest
// delta that arrives here is ~12ms (~4px) and 1px-3px columns no longer occur. The
// scale is deliberately left at 8333/1.25ms-per-px so bar heights stay as tuned.
// A 1px minimum keeps every column visible (no 0px gaps). g_ft_chart[0] is
// leftmost/oldest, [FT_CHART_W-1] is newest; each tick shifts left and appends.
#define FT_CHART_W     480    // PSP's fixed display width (not the buffer stride)
#define FT_CHART_MAX_H 80     // clamp — a stall past this (~107ms) still visibly "pegged"
static u8 g_ft_chart[FT_CHART_W];

// Which resource was the bottleneck at each frame pushed: 0=CPU, 1=GPU, 2=NO DATA.
// Only assigned a real bound while CPU Usage is on; otherwise FT_BOUND_NONE.
#define FT_BOUND_CPU  0
#define FT_BOUND_GPU  1
#define FT_BOUND_NONE 2
static u8 g_ft_chart_bound[FT_CHART_W];

// -1=unknown, 0=CPU-bound, 1=GPU-bound — whichever of CPU%/GPU% read higher.
// Used by fps_draw (text highlight) and ft_chart_tick (bar color).
static int g_perf_bound = -1;

void ft_chart_tick(u32 delta_us)
{
	// 120fps floor at 1px; each ms over 8.33ms adds height at 1.25ms/px.
	int h = (delta_us <= 8333) ? 1 : 1 + (int)((delta_us - 8333) / 1250);
	u8 bound = (g_show_cpu_usage && g_perf_bound >= 0) ? (u8)g_perf_bound : FT_BOUND_NONE;
	if (h > FT_CHART_MAX_H) h = FT_CHART_MAX_H;
	memmove(&g_ft_chart[0], &g_ft_chart[1], FT_CHART_W - 1);
	memmove(&g_ft_chart_bound[0], &g_ft_chart_bound[1], FT_CHART_W - 1);
	g_ft_chart[FT_CHART_W - 1] = (u8)h;
	g_ft_chart_bound[FT_CHART_W - 1] = bound;
}

// Bars grow up from the bottom row of the display, one column at a time —
// drawn fresh every call (only the bar pixels are written, so the game shows
// through everywhere else, same as the rest of this overlay). Colored per
// column by g_ft_chart_bound: OVERLAY_BLUE (CPU-bound) / OVERLAY_ORANGE
// (GPU-bound) / OVERLAY_FG (no data — CPU Usage was off) — see its own
// comment above ft_chart_tick.
void ft_chart_draw(void)
{
	int x, y, w = (FT_CHART_W < dbg_bufw) ? FT_CHART_W : dbg_bufw;
	int baseline = 271;
	if (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
		volatile u32 *base = (volatile u32 *)dbg_fb + baseline * dbg_bufw;
		for (x = 0; x < w; x++) {
			int h = g_ft_chart[x];
			u32 c = (g_ft_chart_bound[x] == FT_BOUND_GPU) ? OVERLAY_ORANGE
			      : (g_ft_chart_bound[x] == FT_BOUND_CPU) ? OVERLAY_BLUE : OVERLAY_FG;
			for (y = 0; y < h; y++) base[x - y * dbg_bufw] = c;
		}
	} else {
		u16 cpu16 = pack16_fmt(OVERLAY_BLUE, dbg_pfmt);
		u16 gpu16 = pack16_fmt(OVERLAY_ORANGE, dbg_pfmt);
		u16 none16 = pack16_fmt(OVERLAY_FG, dbg_pfmt);
		volatile u16 *base = (volatile u16 *)dbg_fb + baseline * dbg_bufw;
		for (x = 0; x < w; x++) {
			int h = g_ft_chart[x];
			u16 c = (g_ft_chart_bound[x] == FT_BOUND_GPU) ? gpu16
			      : (g_ft_chart_bound[x] == FT_BOUND_CPU) ? cpu16 : none16;
			for (y = 0; y < h; y++) base[x - y * dbg_bufw] = c;
		}
	}
}

// g_show_fps_overlay: 1/2/3 -> 1s/0.5s/0.2s averaging window (see the global's
// own comment). Anything else (0, or an out-of-range settings.cfg value) falls
// back to 1s — fps_tick() is only reached at all when the overlay is on.
u32 fps_window_us(void)
{
	if (g_show_fps_overlay == 3) return 200000;
	if (g_show_fps_overlay == 2) return 500000;
	return 1000000;
}

// Pulls the slowest FPS_LOW_K frametimes out of the ring buffer (a running
// top-K via insertion into a small sorted array — cheaper than a full sort
// since K is tiny) and converts their average back to an FPS figure. Unfilled
// buffer slots are 0, which can never be among the SLOWEST entries, so this
// is correct even before the buffer's first full lap.
void fps_calc_1pct_low(u32 now)
{
	u32 top[FPS_LOW_K];
	int i, j, n = 0;
	u64 sum = 0;

	for (i = 0; i < FPS_FT_BUF_SIZE; i++) {
		u32 v = g_fps_ft_buf[i];
		if (v == 0) continue;
		if (n < FPS_LOW_K) {
			top[n++] = v;
			for (j = n - 1; j > 0 && top[j] < top[j - 1]; j--) {
				u32 t = top[j]; top[j] = top[j - 1]; top[j - 1] = t;
			}
		} else if (v > top[0]) {
			top[0] = v;
			for (j = 1; j < FPS_LOW_K && top[j] < top[j - 1]; j++) {
				u32 t = top[j]; top[j] = top[j - 1]; top[j - 1] = t;
			}
		}
	}
	g_fps_lows_last_calc_us = now;
	if (n == 0) return;
	for (i = 0; i < n; i++) sum += top[i];
	g_fps_low1_value = (int)(((u64)n * 1000000ULL) / sum);   // FPS = n*1,000,000 / sum(frametimes_us)
}

// Re-present floor: a present closer than this to the last counted one is the SAME
// frame shown twice, not a new frame. Sized from measured re-present gaps, and bounded
// above by the fastest real frametime any game here produces:
//   GTA        glitch re-present  3-5ms   -> caught
//   Tomb Raider re-present        3-11ms  (v650 [FLD]: max bucket 12, nothing at 14)
//                                         -> caught; it presents each frame twice, once
//                                            from its thread and once from a vblank
//                                            interrupt, and the gap grows with render time
//   real frames                   60fps = 16.7ms, Tron ~70fps = 14.3ms -> counted
// 8333 (a 120fps frametime) was the old value and was chosen as a round number, not from
// GTA's actual 3-5ms glitch — it sat BELOW Tomb Raider's re-present gap, so whenever
// render time pushed that gap past 8.33ms both presents counted and a capped 30fps game
// read 51 (v650, ~10% of seconds). CEILING: this undercounts any game faster than 83fps
// (12ms frames). Tron at ~70fps is the fastest measured here, leaving ~19% margin.
#define FPS_REPRESENT_FLOOR_US 12000

void fps_tick(u32 now)
{
	if (g_fps_last_tick_us != 0) {
		u32 delta = now - g_fps_last_tick_us;
		// Discard a re-present of the frame already counted — see
		// FPS_REPRESENT_FLOOR_US for how the threshold is sized and what it costs.
		// Returning before g_fps_last_tick_us is updated merges the re-present into
		// its real frame rather than restarting the clock from it.
		if (delta < FPS_REPRESENT_FLOOR_US) return;

		g_fps_ft_buf[g_fps_ft_idx] = delta;
		g_fps_ft_idx = (g_fps_ft_idx + 1) % FPS_FT_BUF_SIZE;
		if (g_fps_show_lows && (now - g_fps_lows_last_calc_us) >= 1000000)
			fps_calc_1pct_low(now);
		if (g_show_ft_chart) ft_chart_tick(delta);
	}
	g_fps_last_tick_us = now;
	g_fps_window_count++;
	{
		u32 elapsed = now - g_fps_window_start_us;
		if (elapsed < fps_window_us()) return;
		g_fps_value = (int)(((u64)g_fps_window_count * 1000000ULL) / (u64)elapsed);
		g_fps_window_start_us = now;
		g_fps_window_count = 0;
	}
}

// ── CPU Usage overlay ───────────────────────────────────────────────────────
// Same technique as PSP-HUD's getCpuUsage() (PSP_References/PSP-HUD-master/
// hud/main.c): sceKernelReferSystemStatus() reports idleClocks, the running
// total of CPU clocks spent in the kernel's idle thread; comparing its delta
// against the elapsed wall-clock delta (sceKernelGetSystemTimeLow(), the same
// timer fps_tick already uses) over a >=1s window gives load% without ever
// touching a hardware register — cheap enough to run inline on
// fps_poll_thread rather than needing battery_poll_thread's dedicated thread.
static u32 g_cpu_last_clock_us;   // 0 = no baseline yet
static u32 g_cpu_last_idle_us;
int g_cpu_usage_pct = -1;   // -1 = not yet sampled (first ~1s after enabling)

// ── GPU Usage (busy-duty-cycle estimate) ──────────────────────────────────
// GE has no hardware busy-time counter — sceGeDrawSync(1) polls instantaneous
// state. Only PSP_GE_LIST_DRAWING_DONE (value 2) counts as busy; all other
// states (DONE/QUEUED/STALL_REACHED/CANCEL_DONE) are idle. Sampled once per
// vblank, averaged over the same window as CPU% (1/0.5/0.2s). Result is a
// duty-cycle estimate, not a real load measurement.
static u32 g_gpu_busy_samples, g_gpu_total_samples;
static int g_gpu_usage_pct = -1;

void gpu_usage_sample(void)
{
	if (sceGeDrawSync(1) == PSP_GE_LIST_DRAWING_DONE) g_gpu_busy_samples++;
	g_gpu_total_samples++;
}

void cpu_usage_tick(u32 now)
{
	SceKernelSystemStatus status;
	u32 idle_now;
	if (g_cpu_last_clock_us != 0 && (now - g_cpu_last_clock_us) < fps_window_us()) return;   // update at the FPS window rate (1/0.5/0.2s), not a fixed 1s
	status.size = sizeof(status);
	sceKernelReferSystemStatus(&status);
	idle_now = status.idleClocks.low;
	if (g_cpu_last_clock_us != 0 && g_cpu_last_idle_us != 0 &&
	    now - g_cpu_last_clock_us > 0 && idle_now - g_cpu_last_idle_us <= now - g_cpu_last_clock_us) {
		u32 el_clock = now - g_cpu_last_clock_us;
		u32 el_idle  = idle_now - g_cpu_last_idle_us;
		int usage = 100 - (int)(((u64)el_idle * 100ULL) / (u64)el_clock);
		if (usage < 0) usage = 0;
		if (usage > 100) usage = 100;
		g_cpu_usage_pct = usage;
	}
	g_cpu_last_clock_us = now;
	g_cpu_last_idle_us  = idle_now;

	if (g_gpu_total_samples > 0) {
		g_gpu_usage_pct = (int)((g_gpu_busy_samples * 100) / g_gpu_total_samples);
		g_gpu_busy_samples = 0;
		g_gpu_total_samples = 0;
	}

	// Bottleneck guess for this window: whichever of CPU%/GPU% read higher -
	// see g_perf_bound's own comment (drives the fps_draw text highlight and
	// the frametime-chart bar color). A tie stays CPU-bound (0), arbitrarily.
	if (g_cpu_usage_pct >= 0 && g_gpu_usage_pct >= 0)
		g_perf_bound = (g_gpu_usage_pct > g_cpu_usage_pct) ? 1 : 0;
}

// ── Battery Status overlay ──────────────────────────────────────────────────
// Values grounded in PSP_References/pspsdk-master/src/power/psppower.h (the
// high-level scePower* calls, already linked via -lpsppower and declared
// through pspfatsave.h's <psppower.h> include — no runtime resolution needed)
// and PSP_References/uofw-master/include/syscon.h (the lower-level
// sceSyscon_driver battery commands, resolved at runtime like every other raw
// NID lookup in this file). Percent (0-100) and LifeTime (minutes) are the
// only two fields psppower.h documents a concrete unit for; RemainCapacity/
// FullCapacity are documented in mAh. Neither reference tree documents a unit
// for Temp/Volt/Current/Cycle/LimitTime/ChargeTime — the units below (mV, °C,
// mA) are the USER's own hardware-observed readings on their console, not
// something confirmed against uofw/pspsdk source; Cycle/LimitTime/ChargeTime
// still have no confirmed unit at all, shown as plain numbers. Power (mW) is
// OUR OWN derived value (Volt_mV * Current_mA / 1000), not a separate query.
// scePowerGetBatteryElec is deliberately NOT called: pspsdk's own header
// documents it as "crashes PSP in usermode" (psppower.h). The ALL tier uses
// sceSysconBatteryGetElec instead (a different code path, not flagged as
// crash-prone) as a best-effort substitute for current-draw telemetry.
static int g_batt_exists   = 0;
static int g_batt_ac       = 0;
static int g_batt_charging = 0;
static int g_batt_low      = 0;
static int g_batt_percent  = -1;   // -1 = not read yet / no battery
static int g_batt_life_min = -1;
static int g_batt_remain_mah = 0;
static int g_batt_full_mah   = 0;
static int g_batt_temp_c     = 0;    // user-confirmed on their hardware, not documented in the refs
static int g_batt_volt_mv    = 0;    // user-confirmed on their hardware, not documented in the refs
static int g_batt_current_ma = 0;    // via sceSysconBatteryGetElec, best-effort; negative = discharging

// sceSyscon_driver battery NIDs (uofw syscon.h/exports.exp) — resolved once,
// same sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", ...) pair
// uart.c's g_hrpower already uses successfully in this codebase.
static int (*g_syscon_batt_current)(int *) = NULL;
// scePowerGetBatteryRemainCapacity/FullCapacity ARE declared in psppower.h and
// documented there (mAh) — but this toolchain's installed libpsppower.a turned
// out to only carry a stub for RemainCapacity, not FullCapacity (link error on
// the direct call). Resolved at runtime instead, like the syscon calls above,
// so neither depends on what a given toolchain's static import library has.
static int (*g_power_remain_cap)(void) = NULL;
static int (*g_power_full_cap)(void)   = NULL;

static void battery_resolve_syscon(void)
{
	static int resolved;
	if (resolved) return;
	resolved = 1;
	g_syscon_batt_current = (int (*)(int *))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0x483088B0);
	g_power_remain_cap    = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0x94F5A53F);
	g_power_full_cap      = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0xFD18A0FF);
}

// Queries the firmware/syscon directly — real hardware bus transactions, not
// cheap RAM reads, so this only ever runs from battery_poll_thread's own slow
// loop (see there), never from anything drawing-related. remain_mah/full_mah/
// current_ma are queried from the Percent+Time tier up (g_show_battery>=2) —
// the Time slot's charge-time-to-full calc in battery_draw needs them; temp/
// volt stay gated to g_show_battery==3 only, since nothing below that tier
// displays them. (chgtime — a second raw syscon bus call, same cost class as
// current — was removed here: it was never displayed by anything, confirmed
// as the actual source of a ~10% CPU cost the user measured specifically on
// the ALL tier, since it was the one extra hardware-bus call ALL added over
// Percent+Time; temp/volt are cheap high-level scePower calls, not the cause.)
void battery_refresh(void)
{
	g_batt_exists = scePowerIsBatteryExist();
	if (!g_batt_exists) { g_batt_percent = -1; return; }
	g_batt_ac       = scePowerIsPowerOnline();
	g_batt_charging = scePowerIsBatteryCharging();
	g_batt_low      = scePowerIsLowBattery();
	g_batt_percent  = scePowerGetBatteryLifePercent();
	g_batt_life_min = scePowerGetBatteryLifeTime();
	if (g_show_battery >= 2) {
		if (g_power_remain_cap) g_batt_remain_mah = g_power_remain_cap();
		if (g_power_full_cap)   g_batt_full_mah   = g_power_full_cap();
		if (g_syscon_batt_current) g_syscon_batt_current(&g_batt_current_ma);
	}
	if (g_show_battery >= 3) {
		g_batt_temp_c  = scePowerGetBatteryTemp();
		g_batt_volt_mv = scePowerGetBatteryVolt();
	}
}

// ── Dedicated battery-poll thread — fully decoupled from fps_poll_thread ──
// battery_refresh's sceSyscon* calls are real hardware bus transactions to the
// battery's fuel-gauge chip (not cheap RAM reads like fps_tick/fps_value) —
// the ALL tier fires 6+ of them back-to-back, and each can plausibly cost
// tens of milliseconds. Calling that from ANYWHERE inside fps_poll_thread
// blocks the OVERLAY DRAW thread itself for that whole stretch, no matter
// which side of the vblank-sync call it sits on — the thread can't service
// its precise vblank timing while it's stuck waiting on the battery chip, so
// the overlay visibly stalls/blinks once per refresh regardless of the exact
// call site (both tried, both still stalled). The actual fix is what the user
// suggested from the start: capture the values on a genuinely SEPARATE
// thread, at its own pace, into plain cached ints; battery_draw (called from
// fps_draw, same as always) only ever reads those, same as it reads
// g_fps_value — it never queries anything itself, so it can never block.
int g_battery_poll_started = 0;
int battery_poll_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	while (1) {
		if (!g_show_battery) {
			g_battery_poll_started = 0;   // mirrors fps_poll_thread's own clean-exit pattern
			sceKernelExitDeleteThread(0);
			return 0;
		}
		battery_refresh();
		// Reuses the Show FPS interval (1s/0.5s/0.2s, falls back to 1s with FPS
		// off) rather than a separate battery-only setting — no real reason for
		// the two to differ. This thread just sleeps between queries; it isn't
		// on anyone's timing budget, so blocking here for however long the
		// hardware transactions take is completely harmless.
		sceKernelDelayThread(fps_window_us());
	}
	return 0;
}

// Lazily starts battery_poll_thread — mirrors fps_poll_ensure_started exactly
// (see its own comment), just for the battery thread instead of the overlay
// draw thread. Low priority (64, well below the overlay thread's 32) and a
// bit more stack (3KB: the syscon/scePower call chains aren't ours to verify)
// since this thread never needs to keep up with anything time-critical.
void battery_poll_ensure_started(void)
{
	if (g_battery_poll_started) return;
	g_battery_poll_started = 1;
	{
		SceUID thid = sceKernelCreateThread("pspstates_battery_poll", battery_poll_thread, 64, BATTERY_POLL_STACK_BYTES, 0, NULL);
		if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
	}
}

// ── Small HUD font (5x7, packed 1bpp) — used ONLY by the FPS/Battery overlay
// below, NOT the menu (which keeps the full 8x8 font8x8/dbg_text).
//
// Sparse/curated set (35 glyphs, not a contiguous ASCII range), uppercase
// only — small_font_index() maps a character to its row here, falling back
// to the space glyph (index 0) for anything not covered (safe blank, never
// garbage). Row format: each byte's top 5 bits (128>>j for j=0..4) are that
// row's 5 pixels, MSB = leftmost; the bottom 3 bits are always 0 (unused).
//
// Every glyph is transcribed from PSP-HUD's small_font[][10]
// (PSP_References/PSP-HUD-master/hud/draw.c; that tree has no LICENSE file,
// no copyright header in any source file, and no license mention anywhere
// in its README) — its 7-wide cell's columns 1-5 (the outer border columns
// are always background) mapped 'x'->1 bit, else->0. An earlier draft used
// an independently hand-drawn font, then hand-drew lowercase l/m/n/o/s/t/y
// once PSP-HUD's own small font turned out to stop at 'k' — both looked
// wrong on hardware (baseline misaligned, "Limit" read as garbled). Rather
// than keep guessing at glyph shapes with no source to check against, every
// HUD string is now uppercase-only so every glyph here traces back to real
// PSP-HUD data — no hand-drawn shapes left in this table at all.
static const u8 g_small_font[37][7] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' (0)
	{0x00,0x00,0x90,0x20,0x40,0x90,0x00}, // '%' (1)
	{0x00,0x00,0x00,0xF8,0x00,0x00,0x00}, // '-' (2)
	{0x00,0x08,0x10,0x20,0x40,0x80,0x00}, // '/' (3)
	{0x00,0x00,0x20,0x00,0x20,0x00,0x00}, // ':' (4)
	{0x00,0xF8,0x88,0x88,0x88,0xF8,0x00}, // '0' (5)
	{0x00,0x20,0x20,0x20,0x20,0x20,0x00}, // '1' (6)
	{0x00,0xF8,0x08,0xF8,0x80,0xF8,0x00}, // '2' (7)
	{0x00,0xF8,0x08,0xF8,0x08,0xF8,0x00}, // '3' (8)
	{0x00,0x88,0x88,0xF8,0x08,0x08,0x00}, // '4' (9)
	{0x00,0xF8,0x80,0xF8,0x08,0xF8,0x00}, // '5' (10)
	{0x00,0xF8,0x80,0xF8,0x88,0xF8,0x00}, // '6' (11)
	{0x00,0xF8,0x08,0x08,0x08,0x08,0x00}, // '7' (12)
	{0x00,0xF8,0x88,0xF8,0x88,0xF8,0x00}, // '8' (13)
	{0x00,0xF8,0x88,0xF8,0x08,0xF8,0x00}, // '9' (14)
	{0x00,0xF8,0x88,0xF8,0x88,0x88,0x00}, // 'A' (15)
	{0x00,0xF0,0x88,0xF0,0x88,0xF0,0x00}, // 'B' (16)
	{0x00,0xF8,0x80,0x80,0x80,0xF8,0x00}, // 'C' (17)
	{0x00,0xF8,0x80,0xF8,0x80,0xF8,0x00}, // 'E' (18)
	{0x00,0xF8,0x80,0xF8,0x80,0x80,0x00}, // 'F' (19)
	{0x00,0x88,0x88,0xF8,0x88,0x88,0x00}, // 'H' (20)
	{0x00,0x20,0x20,0x20,0x20,0x20,0x00}, // 'I' (21)
	{0x00,0x80,0x80,0x80,0x80,0xF8,0x00}, // 'L' (22)
	{0x00,0xF8,0xA8,0xA8,0xA8,0xA8,0x00}, // 'M' (23)
	{0x00,0x88,0xC8,0xA8,0x98,0x88,0x00}, // 'N' (24)
	{0x00,0xF8,0x88,0x88,0x88,0xF8,0x00}, // 'O' (25)
	{0x00,0xF8,0x88,0xF8,0x80,0x80,0x00}, // 'P' (26)
	{0x00,0xF8,0x80,0xF8,0x08,0xF8,0x00}, // 'S' (27)
	{0x00,0xF8,0x20,0x20,0x20,0x20,0x00}, // 'T' (28)
	{0x00,0x88,0x88,0x88,0x50,0x20,0x00}, // 'V' (29)
	{0x00,0xA8,0xA8,0xA8,0xA8,0xF8,0x00}, // 'W' (30)
	{0x00,0x88,0x88,0x70,0x20,0x20,0x00}, // 'Y' (31)
	{0x00,0x88,0x88,0x88,0x88,0xF8,0x00}, // 'U' (32)
	{0x00,0xF8,0x80,0xB8,0x88,0xF8,0x00}, // 'G' (33)
	{0x00,0x00,0x00,0x00,0x00,0x20,0x00}, // '.' (34)
	// Charge-direction triangles, used next to the battery Time slot.
	{0x00,0x00,0x20,0x70,0xF8,0x00,0x00}, // '^' up triangle (35)
	{0x00,0x00,0xF8,0x70,0x20,0x00,0x00}, // '_' down triangle (36)
};

static int small_font_index(char c)
{
	switch (c) {
	case ' ': return 0;  case '%': return 1;  case '-': return 2;
	case '/': return 3;  case ':': return 4;
	case '0': return 5;  case '1': return 6;  case '2': return 7;
	case '3': return 8;  case '4': return 9;  case '5': return 10;
	case '6': return 11; case '7': return 12; case '8': return 13;
	case '9': return 14;
	case 'A': return 15; case 'B': return 16; case 'C': return 17;
	case 'E': return 18; case 'F': return 19; case 'H': return 20;
	case 'I': return 21; case 'L': return 22; case 'M': return 23;
	case 'N': return 24; case 'O': return 25; case 'P': return 26;
	case 'S': return 27; case 'T': return 28; case 'V': return 29;
	case 'W': return 30; case 'Y': return 31; case 'U': return 32;
	case 'G': return 33; case '.': return 34;
	case '^': return 35; case '_': return 36;
	default:  return 0;   // uncovered character -> blank, never garbage
	}
}

// 1 if glyph row r, column c (r=0..6, c=0..4) is a lit (fg) pixel; 0 for any
// out-of-range r/c too, so the halo dilation below can probe one pixel past
// every edge without special-casing the glyph border.
static int small_bit(const u8 *glyph, int r, int c)
{
	if (r < 0 || r > 6 || c < 0 || c > 4) return 0;
	return (glyph[r] & (128 >> c)) != 0;
}

// Pre-baked black-halo mask, one per glyph, over the same 9-row (r=-1..7) x
// 7-col (c=-1..5) draw region small_putchar rasterizes. Bit (128>>(c+1)) of
// row [r+1] is set iff pixel (r,c) is a halo pixel — a background pixel that
// touches the glyph via 8-neighbour dilation. The dilation is a pure function
// of the fixed g_small_font shapes above, so it's computed OFFLINE (scratchpad
// bake_halo.py, mirroring the exact dilation the old inline loop did) and stored
// here as constant data — the PSP never computes it. This replaces a per-pixel
// 8-neighbour probe inside small_putchar that dominated the overlay-draw cost;
// each pixel is now a single mask-bit test. Kept SEPARATE from the glyph table
// because the two carry different colours (glyph = runtime dbg_fg, e.g. red on
// low battery; halo = always black) and so can't share one bitmap.
static const u8 g_small_halo[37][9] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' (0)
	{0x00,0x00,0xFC,0xB4,0xEC,0xDC,0xB4,0xFC,0x00}, // '%' (1)
	{0x00,0x00,0x00,0xFE,0x82,0xFE,0x00,0x00,0x00}, // '-' (2)
	{0x00,0x0E,0x1A,0x36,0x6C,0xD8,0xB0,0xE0,0x00}, // '/' (3)
	{0x00,0x00,0x38,0x28,0x38,0x28,0x38,0x00,0x00}, // ':' (4)
	{0x00,0xFE,0x82,0xBA,0xAA,0xBA,0x82,0xFE,0x00}, // '0' (5)
	{0x00,0x38,0x28,0x28,0x28,0x28,0x28,0x38,0x00}, // '1' (6)
	{0x00,0xFE,0x82,0xFA,0x82,0xBE,0x82,0xFE,0x00}, // '2' (7)
	{0x00,0xFE,0x82,0xFA,0x82,0xFA,0x82,0xFE,0x00}, // '3' (8)
	{0x00,0xEE,0xAA,0xBA,0x82,0xFA,0x0A,0x0E,0x00}, // '4' (9)
	{0x00,0xFE,0x82,0xBE,0x82,0xFA,0x82,0xFE,0x00}, // '5' (10)
	{0x00,0xFE,0x82,0xBE,0x82,0xBA,0x82,0xFE,0x00}, // '6' (11)
	{0x00,0xFE,0x82,0xFA,0x0A,0x0A,0x0A,0x0E,0x00}, // '7' (12)
	{0x00,0xFE,0x82,0xBA,0x82,0xBA,0x82,0xFE,0x00}, // '8' (13)
	{0x00,0xFE,0x82,0xBA,0x82,0xFA,0x82,0xFE,0x00}, // '9' (14)
	{0x00,0xFE,0x82,0xBA,0x82,0xBA,0xAA,0xEE,0x00}, // 'A' (15)
	{0x00,0xFC,0x86,0xBA,0x86,0xBA,0x86,0xFC,0x00}, // 'B' (16)
	{0x00,0xFE,0x82,0xBE,0xA0,0xBE,0x82,0xFE,0x00}, // 'C' (17)
	{0x00,0xFE,0x82,0xBE,0x82,0xBE,0x82,0xFE,0x00}, // 'E' (18)
	{0x00,0xFE,0x82,0xBE,0x82,0xBE,0xA0,0xE0,0x00}, // 'F' (19)
	{0x00,0xEE,0xAA,0xBA,0x82,0xBA,0xAA,0xEE,0x00}, // 'H' (20)
	{0x00,0x38,0x28,0x28,0x28,0x28,0x28,0x38,0x00}, // 'I' (21)
	{0x00,0xE0,0xA0,0xA0,0xA0,0xBE,0x82,0xFE,0x00}, // 'L' (22)
	{0x00,0xFE,0x82,0xAA,0xAA,0xAA,0xAA,0xFE,0x00}, // 'M' (23)
	{0x00,0xEE,0xBA,0x9A,0xAA,0xB2,0xBA,0xEE,0x00}, // 'N' (24)
	{0x00,0xFE,0x82,0xBA,0xAA,0xBA,0x82,0xFE,0x00}, // 'O' (25)
	{0x00,0xFE,0x82,0xBA,0x82,0xBE,0xA0,0xE0,0x00}, // 'P' (26)
	{0x00,0xFE,0x82,0xBE,0x82,0xFA,0x82,0xFE,0x00}, // 'S' (27)
	{0x00,0xFE,0x82,0xEE,0x28,0x28,0x28,0x38,0x00}, // 'T' (28)
	{0x00,0xEE,0xAA,0xAA,0xBA,0xD6,0x6C,0x38,0x00}, // 'V' (29)
	{0x00,0xFE,0xAA,0xAA,0xAA,0xAA,0x82,0xFE,0x00}, // 'W' (30)
	{0x00,0xEE,0xAA,0xBA,0xC6,0x6C,0x28,0x38,0x00}, // 'Y' (31)
	{0x00,0xEE,0xAA,0xAA,0xAA,0xBA,0x82,0xFE,0x00}, // 'U' (32)
	{0x00,0xFE,0x82,0xBE,0xA2,0xBA,0x82,0xFE,0x00}, // 'G' (33)
	{0x00,0x00,0x00,0x00,0x00,0x38,0x28,0x38,0x00}, // '.' (34)
	{0x00,0x00,0x38,0x6C,0xC6,0x82,0xFE,0x00,0x00}, // '^' up triangle (35)
	{0x00,0x00,0xFE,0x82,0xC6,0x6C,0x38,0x00,0x00}, // '_' down triangle (36)
};

// Draws one small-font glyph at absolute pixel (px,py) — NOT the char-cell
// (dbg_col/dbg_row) system dbg_putchar uses, since this font's cells are a
// different, smaller size (5x7, not 8x8). Reads the SAME dbg_fb/dbg_bufw/
// dbg_pfmt/dbg_fg/dbg_bg/dbg_transparent globals every other draw primitive
// in this file does, just with its own geometry.
//
// Every fg pixel gets a 1px black halo (8-neighbor dilation of the glyph's
// own lit pixels, computed fresh per draw so it always matches whatever the
// glyph shape actually is) so the text stays readable over any game
// background — this is what makes this readable-over-gameplay overlay work
// regardless of dbg_transparent; halo pixels are forced black even when
// dbg_transparent is set (true background pixels, i.e. neither fg nor halo,
// still honor dbg_transparent/dbg_bg as before). Draw region is therefore
// 7x9 (one halo pixel beyond each glyph edge), each pixel bounds-checked
// individually rather than aborting the whole glyph near screen edges.
static void small_putchar(int px, int py, char ch)
{
	int r, c;
	int gi = small_font_index(ch);
	const u8 *glyph = g_small_font[gi];
	const u8 *halo_mask = g_small_halo[gi];   // precomputed (see g_small_halo)
	int is8888 = (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888);
	u32 fg32 = dbg_fg, bg32 = dbg_bg;
	u16 fg16 = 0, bg16 = 0, black16 = 0;
	if (!is8888) {
		fg16 = pack16_fmt(dbg_fg, dbg_pfmt);
		bg16 = pack16_fmt(dbg_bg, dbg_pfmt);
		black16 = pack16_fmt(0xFF000000, dbg_pfmt);
	}

	for (r = -1; r <= 7; r++) {
		int py_r = py + r;
		u8 halo_row = halo_mask[r + 1];
		if (py_r < 0 || py_r >= 272) continue;
		for (c = -1; c <= 5; c++) {
			int px_c = px + c;
			int fgbit, draw_black;
			if (px_c < 0 || px_c >= dbg_bufw) continue;
			fgbit = small_bit(glyph, r, c);
			draw_black = (!fgbit && (halo_row & (128 >> (c + 1))));
			if (!fgbit && !draw_black && dbg_transparent) continue; // true background, left untouched

			if (is8888) {
				volatile u32 *px32 = (volatile u32 *)dbg_fb + px_c + py_r * dbg_bufw;
				*px32 = fgbit ? fg32 : (draw_black ? 0xFF000000 : bg32);
			} else {
				volatile u16 *px16 = (volatile u16 *)dbg_fb + px_c + py_r * dbg_bufw;
				*px16 = fgbit ? fg16 : (draw_black ? black16 : bg16);
			}
		}
	}
}

// Draws a string in the small font at absolute pixel (px,py), advancing by
// 6px per character (5px glyph + 1px gap). fg/bg set once for the call,
// same signature/behavior as dbg_text, just pixel- instead of cell-addressed.
static void small_text(int px, int py, u32 fg, u32 bg, const char *s)
{
	dbg_fg = fg; dbg_bg = bg;
	while (*s) {
		small_putchar(px, py, *s);
		px += 6;
		s++;
	}
}

// Draws the battery block starting at pixel row `y`; returns the next free
// y (so fps_draw can stack FPS lines above it without a fixed offset), 8px
// per line (the small font is 7px tall; +1px gap).
// Percent tier: 1 line ("<pct>%"). +Time tier: same line, time appended
// ("<pct>% <time>") — no separate line, no "Batt:"/"Time:" prefixes; AC/
// charging is conveyed by the time showing "--:--" rather than a separate
// AC/Chg suffix. ALL tier: +2 more lines — capacity+temp, then a bare
// volt/current/power line with no field labels at all (unit suffixes alone
// identify each number, e.g. "3745 MV 450 MA 1.73 W"). Charging-timer line removed
// (see the user request this matches). Pure cache read: no query, no
// gating, drawn every call in whatever mode the caller (fps_draw) already
// set — the actual refresh happens on the fully separate battery_poll_thread,
// never on this draw path.
int battery_draw(int y)
{
	char buf[40];
	if (!g_show_battery) return y;
	if (!g_batt_exists) { small_text(0, y, OVERLAY_FG, BR_BG, "BATT: NONE"); return y + 8; }

	{
		u32 fg = g_batt_low ? 0xFF0000FF : OVERLAY_FG;   // red when the firmware itself flags low battery
		if (g_show_battery == 1) {
			sprintf(buf, "%d%%", g_batt_percent);
		} else if (g_batt_ac || g_batt_charging) {
			// Charging: time-to-full = remaining capacity gap / charge current
			int target_mah = g_batt_full_mah - (g_batt_full_mah / 10);
			if (g_batt_current_ma > 0 && target_mah > g_batt_remain_mah) {
				int min_left = ((target_mah - g_batt_remain_mah) * 60) / g_batt_current_ma;
				sprintf(buf, "%d%% %d:%02d", g_batt_percent, min_left / 60, min_left % 60);
			} else {
				sprintf(buf, "%d%% --:--", g_batt_percent);
			}
		} else if (g_batt_life_min < 0) {
			sprintf(buf, "%d%% --:--", g_batt_percent);
		} else {
			sprintf(buf, "%d%% %d:%02d", g_batt_percent, g_batt_life_min / 60, g_batt_life_min % 60);
		}
		small_text(0, y, fg, BR_BG, buf); y += 8;

		// Charge-direction triangle: green up while charging, orange down otherwise.
		if (g_show_battery >= 2) {
			int arrow_x = (int)strlen(buf) * 6 + 1;
			dbg_fg = g_batt_charging ? OVERLAY_GREEN : OVERLAY_ORANGE;
			dbg_bg = BR_BG;
			small_putchar(arrow_x, y - 8, g_batt_charging ? '^' : '_');
		}
	}
	if (g_show_battery <= 2) return y;   // Percent, or Percent + Time

	sprintf(buf, "%d/%d MAH %d C", g_batt_remain_mah, g_batt_full_mah, g_batt_temp_c);
	small_text(0, y, OVERLAY_FG, BR_BG, buf); y += 8;
	{
		// Power (mW) is OUR OWN derived value, not a separate query — see the
		// block comment above battery_refresh. Sign follows Current (negative
		// while discharging), matching the user's own hardware observation.
		// Displayed as W with 2 decimals (hundredths of a watt = mW/10);
		// fixed-point integer math, not %f — see the MB/s panel for why.
		int power_mw = (g_batt_volt_mv * g_batt_current_ma) / 1000;
		int neg = power_mw < 0;
		u32 p100 = (u32)((neg ? -power_mw : power_mw) / 10);
		sprintf(buf, "%d MV %d MA %s%u.%02u W", g_batt_volt_mv, g_batt_current_ma,
		        neg ? "-" : "", p100 / 100, p100 % 100);
		small_text(0, y, OVERLAY_FG, BR_BG, buf); y += 8;
	}
	return y;   // ALL
}

// Shared by the SetFrameBuf hook and fps_poll_thread() below — draws straight
// into whichever buffer/stride/format is passed, never a stale/guessed one.
void fps_draw(void *topaddr, int bufferwidth, int pixelformat)
{
	dbg_fb   = (void *)(0xA0000000 | (u32)topaddr);
	dbg_bufw = bufferwidth;
	dbg_pfmt = pixelformat;
	{
		char buf[40];
		int y = 0;   // pixel row, 8px/line (small font is 7px tall + 1px gap) — see small_text
		dbg_transparent = 1;   // glyph pixels only — bg param below is otherwise an opaque box
		if (g_show_fps_overlay) {
			sprintf(buf, "%d FPS", g_fps_value);
			small_text(0, y, OVERLAY_FG, BR_BG, buf); y += 8;
			if (g_fps_show_lows) {
				sprintf(buf, "%d 1%%", g_fps_low1_value);
				small_text(0, y, OVERLAY_FG, BR_BG, buf); y += 8;
			}
		}
		if (g_show_cpu_usage && g_cpu_usage_pct >= 0) {
			// Two separate calls so the bottleneck (g_perf_bound) can be
			// highlighted (OVERLAY_BLUE=CPU, OVERLAY_ORANGE=GPU) while the
			// other stays OVERLAY_FG. 6px/char advance positions the GPU text
			// right after the CPU text.
			sprintf(buf, "%d%% CPU", g_cpu_usage_pct);
			small_text(0, y, (g_perf_bound == FT_BOUND_CPU) ? OVERLAY_BLUE : OVERLAY_FG, BR_BG, buf);
			if (g_gpu_usage_pct >= 0) {
				int cpu_px = (int)strlen(buf) * 6;
				sprintf(buf, " %d%% GPU", g_gpu_usage_pct);
				small_text(cpu_px, y, (g_perf_bound == FT_BOUND_GPU) ? OVERLAY_ORANGE : OVERLAY_FG, BR_BG, buf);
			}
			y += 8;
		}
		battery_draw(y);
		if (g_show_ft_chart) ft_chart_draw();
		dbg_transparent = 0;   // restore: menu/panel drawing elsewhere expects this default
	}
}


// NOTE: overlay draws are deliberately NOT throttled to the display rate. A
// ~60Hz cap was tried (v640) and made GTA's overlay flicker: GTA is single-
// buffered and re-presents the same live buffer (plus a ~4-7ms phantom present),
// so the overlay only stays solid if it's redrawn on essentially every present —
// capping the redraw rate lets the game overwrite it between draws. So every
// present and both poll passes redraw; the cost is accepted for a stable overlay.

// ── Frame limiter (PER-GAME, g_frame_limit) ─────────────────────────────────
// Paces the game by sleeping in the GE-ENQUEUE hook, not the present hook: a frame is
// PRODUCED by the game's thread submitting a render (sceGeListEnQueue), whereas
// sceDisplaySetFrameBuf is only PRESENTATION and may not be the game's work at all.
//
// v643-v648 paced the present and it could not cap Tomb Raider, for a reason that is
// now measured rather than guessed: TR presents heavily from INTERRUPT context (v646
// [FLTID], via sceKernelGetThreadId returning 0x80020064 =
// SCE_ERROR_KERNEL_CANNOT_BE_CALLED_FROM_INTERRUPT) — a vblank handler flipping the
// buffer on the DISPLAY's schedule. A syscall-table patch (sctrlHENPatchSyscall) is
// reachable that way; an earlier comment here claimed it could not be, which was wrong.
// An interrupt cannot be slept, so those presents are unpaceable — and the ratio is
// scene-dependent (TR logs 30 thread : 30 intr in one view, 20 : 39 in another), so
// pacing the present gave a cap that came and went with the camera angle. Note the 20:39
// case also disproves "an interrupt present is a re-present": there are MORE of them than
// thread presents, so they carry real frames. Only the unsleepable part is true.
// sceGeListEnQueue has neither problem — every rendering path must call it (sceGu is a
// pure user-mode wrapper), and it is the game's own thread doing the work.
//
// THE FRAME BOUNDARY IS THE GE SUBMIT — not the present. "A present begins a new frame"
// is false for Tomb Raider, which presents each frame TWICE (v653 [FLTID]: 10 thread +
// 20 interrupt per second). v653 armed on every present and TR double-paced: the GE paced
// the frame, the interrupt flip then re-armed mid-frame, and the thread present paced the
// same frame again — cap=30 ge=15 paced=30 fps=14, i.e. a 30 cap delivering 15, worse than
// no cap at all. The signal that is genuinely once-per-frame in both games is the render:
// Tron uncapped ran ge=76 / fps=77, TR ran ge=30 / fps=29. So a GE submit sets g_fl_pending
// and that is the only thing that ever does.
//
// TWO sleep sites, one pending flag, because neither reaches both games and each fails for
// exactly the game the other handles:
//   Tomb Raider presents from a vblank INTERRUPT, which can never be slept — but its GE
//               submits sleep fine (v650: ge=30 paced=30, a real 30fps). Paced at the GE.
//   Tron        submits its GE lists with interrupts or dispatch disabled, so every sleep
//               there is REFUSED — v653 measured fail=30 of ge=30, err=0x800201A7
//               (SCE_ERROR_KERNEL_WAIT_CAN_NOT_WAIT, uofw common/errors.h:332). But it
//               presents from its THREAD, and v644 paced it correctly from the present
//               hook. Paced at the present.
// So: the GE site marks the frame and tries first; if its sleep is refused the frame stays
// pending and the present site sleeps for it instead. A refused sleep is a total no-op —
// it must not touch the anchor either, or the grid the other site inherits is corrupt.
//
// Neither the present hook nor the vblank hooks mark frames any more (the v652 vblank arm
// is gone with them): a present cannot, per the TR result above, and the vblank arm existed
// only to reach Tron's no-present windows, which is moot now that Tron's GE cannot sleep
// anyway. Known limit: a game submitting SEVERAL GE lists per visual frame would pace once
// per list. Not observed — both measured games are 1 GE per frame (see the ge=/fps= pairs
// above) — but nothing here guards it.
//
// g_fl_anchor_us is the ideal grid point the cadence locks to. It advances by exactly
// target_us whenever we waited, so DelayThread's overshoot is absorbed rather than
// accumulating into drift; if the game is already slower than the cap there is nothing
// to do and it resets to now.
//
// Panel timing: the LCD is NOT 60.000Hz. pspsdk documents the mode as 59.94005995Hz
// (pspsdk-master/src/display/pspdisplay.h:44-52) => 16683.33us/vblank, so a 30 cap
// of 1000000/30 = 33333us runs 33.67us/frame fast against the real 2-vblank grid of
// 33366.67us and slips a whole vblank roughly every 496 frames — a hitch about every
// 16s. g_fl_vblank_us takes the period from sceDisplayGetFramePerSec() instead (read
// once at install, kept as an int: no float math on the game's thread).
#define GE_REFUSE_LATCH 8        // consecutive GE-site refusals before we stop attempting there
u32 g_fl_anchor_us;       // ideal grid point (pacing only)
static int g_fl_pending;         // a GE submit began a frame that has not been paced yet
static int g_fl_saw_present;     // a present has happened since the last frame we marked
u32 g_fl_vblank_us;       // real refresh period in us, resolved at install (0 = unresolved)
static int g_fl_cached_for;      // the g_frame_limit that g_fl_target_us was computed for
static u32 g_fl_target_us;       // cached period for g_fl_cached_for
static int g_fl_ge_refused;      // consecutive GE-site sleep refusals; latches at GE_REFUSE_LATCH
static u32 g_fl_reprobe;         // submit counter for re-probing a latched GE site
static u32 g_fl_seen;            // [FLIMIT]: GE submits the hook saw        } drained + reset
static u32 g_fl_paced;           // [FLIMIT]: frames we actually slept on    } once/sec by
static u32 g_fl_extra;           // [FLIMIT]: GE submits that were another list of the same
                                  //           frame — >0 means a MULTI-LIST game } fps_poll_thread
u32 g_fl_intr;            // [FLIMIT]: submits from interrupt context (unpaceable)
static u32 g_fl_failed;          // [FLIMIT]: sleeps that returned an error instead of blocking
static u32 g_fl_last_err;        // [FLIMIT]: last such error code
static u32 g_fl_log_last_us;     // [FLIMIT]: last emit time

// Period for a target rate, snapped to a whole-vblank multiple only when that rate
// really is a refresh divisor (60 -> 16683, 30 -> 33367; also 20/15 if ever offered).
// The 2.5% tolerance is load-bearing: it must reject any snap that would change the
// rate the user asked for. 55 and 50 both sit within ONE vblank of 60fps, so a looser
// window would silently turn either into a 60 cap.
//
// Rates with no whole-vblank period keep their exact period. Their frames must alternate
// refresh counts — 40fps is 1.5 vblanks, so 2/1/2/1 — which is a real property of a
// fixed-refresh panel, NOT a defect in the pacing. How visible that is on the PSP's 4.3"
// 60Hz LCD is a separate question: tested by hand across the 20..60 range and reported as
// feeling fine, so do not assume the non-divisor steps are unusable.
u32 frame_limit_target_us(int fps)
{
	u32 vb    = g_fl_vblank_us ? g_fl_vblank_us : 16683;
	u32 ideal = 1000000u / (u32)fps;
	u32 n     = (ideal + vb / 2) / vb;          // nearest whole vblank count
	u32 grid, diff;
	if (n < 1) return ideal;
	grid = n * vb;
	diff = (grid > ideal) ? grid - ideal : ideal - grid;
	return (diff <= ideal / 40) ? grid : ideal;
}

// Sleeps this frame onto the grid. Callers must have established that a frame is pending
// (see g_fl_pending) and that we are on a thread. at_ge tells us which site we are, so a
// refusal can be latched (see GE_REFUSE_LATCH). See the block comment above for why there
// are two call sites.
static void frame_limit_pace(int at_ge)
{
	u32 now, elapsed;
	now = sceKernelGetSystemTimeLow();
	if (g_fl_anchor_us == 0) { g_fl_anchor_us = now; g_fl_pending = 0; return; }
	elapsed = now - g_fl_anchor_us;
	if (elapsed >= g_fl_target_us) {            // already below the cap — nothing to pace
		g_fl_anchor_us = now;
		g_fl_pending = 0;
		return;
	}
	{
		int r = sceKernelDelayThread(g_fl_target_us - elapsed);
		if (r < 0) {
			// Refused, not taken (Tron's GE submits: err=0x800201A7 WAIT_CAN_NOT_WAIT —
			// interrupts or dispatch disabled). A TOTAL no-op on purpose: leave pending
			// set so the other site sleeps for this same frame, and leave the anchor
			// alone so that site inherits the same grid. Touching either here corrupts
			// it — v652 advanced the anchor on a refused sleep, which pushed it ahead of
			// real time, underflowed the next unsigned now-anchor, and produced the
			// paced=ge/2 alternation with no actual delay at all.
			g_fl_failed++;
			g_fl_last_err = (u32)r;
			if (at_ge && g_fl_ge_refused < GE_REFUSE_LATCH) g_fl_ge_refused++;
			return;
		}
		if (at_ge) g_fl_ge_refused = 0;         // this site works after all — unlatch
		g_fl_pending = 0;
		g_fl_anchor_us += g_fl_target_us;       // lock to the ideal grid (no drift)
		g_fl_paced++;
	}
}

// From the GE-enqueue hooks: the game's thread submitting a render. This is THE frame
// boundary (see the block comment above) and the first choice of sleep site — it throttles
// production itself, and it is what reaches Tomb Raider. Owns the state reset.
void frame_limit_ge(void)
{
	if (g_menu_open) { g_fl_anchor_us = 0; g_fl_pending = 0; g_fl_saw_present = 0; return; }
	if (g_frame_limit != g_fl_cached_for) {     // recompute only on change, never per submit
		g_fl_cached_for = g_frame_limit;
		g_fl_target_us  = frame_limit_target_us(g_frame_limit);
	}
	g_fl_seen++;
	// An interrupt cannot be slept in ANY game — a no-op where rendering is thread work.
	if (sceKernelIsIntrContext()) { g_fl_intr++; return; }
	// A NEW frame only if something has been presented since the last one we marked;
	// otherwise this is another GE list of the frame already in flight. Games legitimately
	// submit several lists per visual frame — a GE-heavy phase logged ge=4658 in one second
	// (v652) — and pacing each would advance the grid ~155s per real second, stalling the
	// game outright. g_fl_saw_present is a FLAG, so Tomb Raider's two presents per frame
	// collapse into a single mark.
	if (g_fl_saw_present) { g_fl_saw_present = 0; g_fl_pending = 1; }
	if (!g_fl_pending) { g_fl_extra++; return; }   // another list of the current frame
	// Whether a game's GE site can sleep is a FIXED property of that game, so stop
	// re-deriving it every frame. Tron refuses every single time (v653: fail=30 of ge=30,
	// err=0x800201A7), and paying a clock read plus a doomed DelayThread per submit is
	// pure waste — its GE-heavy phases hit ge=4658/sec (v652), i.e. ~9300 useless kernel
	// calls in one second. Once latched, skip straight to the present site and re-probe
	// only every 256th submit (~4/sec at 60fps) in case the game's context changes. The
	// probe counter is used instead of a clock so the skip path costs no kernel call.
	if (g_fl_ge_refused >= GE_REFUSE_LATCH) {
		if (++g_fl_reprobe & 0xFF) return;
		g_fl_ge_refused = 0;
	}
	frame_limit_pace(1);
}

// From the END of the present hook — never before fps_tick, which needs untouched arrival
// timestamps (v645 slept ahead of it and the counter read double). Does nothing unless the
// GE site's sleep was refused, i.e. this is Tron. NOTE it must not mark frames: Tomb Raider
// presents twice per frame, so treating a present as a boundary double-paced it (v653).
static void frame_limit_present(void)
{
	if (g_menu_open) return;                         // frame_limit_ge owns the reset
	// Mark that something reached the screen — from ANY context, since Tomb Raider's
	// interrupt flip is still a real frame boundary even though it cannot be slept. This
	// is what lets the next GE submit tell a NEW frame from another list of this one.
	g_fl_saw_present = 1;
	if (!g_fl_pending) return;                       // the GE site already paced this frame
	if (sceKernelIsIntrContext()) return;            // Tomb Raider's flip — unpaceable
	frame_limit_pace(0);
}

int fps_display_set_frame_buf_patched(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
	// Intro-skip CAPTURE banner — see vskip_banner_draw. Drawn here for games that DO call
	// SetFrameBuf, and from fps_poll_thread for games that don't (GTA/Pirates).
	if (g_vskip_banner && !g_menu_open && topaddr && bufferwidth > 0)
		vskip_banner_draw(topaddr, bufferwidth, pixelformat);

	if ((g_show_fps_overlay || g_show_battery || g_show_cpu_usage || g_show_ft_chart) && !g_menu_open && topaddr && bufferwidth > 0) {
		if (g_show_fps_overlay || g_show_ft_chart) {
			u32 now = sceKernelGetSystemTimeLow();

			// Shared burst counter the vblank hooks read to avoid double-counting
			// frames that also flip the buffer through this hook.
			if (now - g_fps_recent_setfb_time < 200000) {
				if (g_fps_recent_setfb_count < 10) g_fps_recent_setfb_count++;
			} else {
				g_fps_recent_setfb_count = 0;
			}
			g_fps_recent_setfb_time = now;

			// Tick on every real call — address-independent (some games, e.g. GTA,
			// present the SAME buffer every frame; an earlier "tick only if topaddr
			// changed" version wrongly dropped those). GTA's ~4-7ms phantom re-present
			// is filtered downstream by fps_tick's 120fps discard floor, not here.
			//
			// This MUST be reached with the ARRIVAL timestamp — see frame_limit_wait's
			// call site below, which is deliberately after this block. An interrupt
			// present is NOT filtered here: v648 tried that and undercounted by half.
			// Tomb Raider's low-fps view logs 20 thread presents against 39 interrupt
			// ones ([FLTID], v648) — 2:1, so most of its REAL frames are presented from
			// the vblank handler, and dropping them read 19 for a ~40fps game. An
			// interrupt present being a mere re-present is false in general; it is only
			// true that it cannot be SLEPT (which is the limiter's problem, not this
			// counter's). FPS_REPRESENT_FLOOR_US is what pairs a re-present with its
			// frame, and on arrival times it does that correctly.
			fps_tick(now);
		}
		fps_draw(topaddr, bufferwidth, pixelformat);
	}
	// Fallback sleep site, only for a frame whose GE submit refused to sleep (Tron). Runs
	// after fps_tick above, never before it — fps_tick needs untouched ARRIVAL timestamps
	// for FPS_REPRESENT_FLOOR_US to pair a re-present with its frame (v645 slept ahead of
	// it and the counter read double). This does NOT mark a frame boundary: Tomb Raider
	// presents twice per frame, so arming here double-paced it (v653: ge=15 paced=30
	// fps=14 under a 30 cap). Only the GE submit marks frames. Inline OFF test, same
	// reason as the GE call site. Skip during intro video skip to preserve video timing.
	if (g_frame_limit > 0 && !g_vskip_active) frame_limit_present();
	return g_real_display_set_frame_buf ? g_real_display_set_frame_buf(topaddr, bufferwidth, pixelformat, sync) : 0;
}

// Drawing from the SetFrameBuf/vblank HOOKS still didn't show the overlay
// during actual GTA gameplay (only its pause menu) — those hooks only fire
// when the game calls the specific syscall being hooked, and GTA apparently
// doesn't call any of the three during real 3D gameplay. This thread doesn't
// depend on the game calling anything: it asks the display controller
// directly for whatever buffer is CURRENTLY live.
//
// A fixed ~16ms sceKernelDelayThread poll (matching a comparable reference
// plugin's own binary) still flickered in testing — confirmed NOT just the
// opaque background (a separate, already-fixed bug), the digits themselves
// were unstable. Draw TIMING (not FPS math — that stays exactly as
// implemented above; a reference HUD-overlay plugin's own FPS number is
// wrong, do not port that part) is adapted from that same reference plugin's
// own draw loop: instead of a fixed delay, block on the REAL display vblank
// via g_real_wait_vblank_start() directly (the resolved original, NOT the
// hooked syscall — calling the hooked sceDisplayWaitVblankStart here would
// re-enter fps_wait_vblank_start_patched and double-tick the FPS counter),
// draw immediately, then measure how long that draw took and sleep the REST
// of the vblank period (clamped to 10%-90% of it, same ratio that reference
// plugin uses) before drawing a SECOND time just before the next real flip.
// Two draws spread across the vblank period make it far more likely at least
// one lands after the game's own GE has finished refreshing that buffer for
// this cycle, instead of racing it once on a fixed timer.
//
// g_fps_poll_started tracks whether this thread currently exists at all (see
// fps_poll_ensure_started() below) - declared up here since this loop clears
// it itself right before self-exiting.
int g_fps_poll_started = 0;
int fps_poll_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	while (1) {
		if (!g_show_fps_overlay && !g_show_battery && !g_show_cpu_usage && !g_show_ft_chart && !g_vskip_banner) {
			// Turned off (all overlays AND the intro-skip banner): exit CLEANLY at this natural
			// point in our own loop (never mid-syscall, unlike an external
			// sceKernelTerminateThread would risk) so the 2KB stack is actually
			// reclaimed rather than just idling forever. Clear the started-flag
			// so a later re-enable (fps_poll_ensure_started) spins up a fresh
			// thread.
			g_fps_poll_started = 0;
			sceKernelExitDeleteThread(0);
			return 0;
		}
		if (g_menu_open) {
			sceKernelDelayThread(16000);   // still enabled, just paused while the menu draws
			continue;
		}

		// Battery's own hardware queries do NOT happen on this thread at all —
		// see battery_poll_thread — this loop only ever reads the cached values
		// battery_draw (via fps_draw below) picks up, same as it reads g_fps_value.

		// CPU usage IS queried right here, unlike Battery — sceKernelReferSystemStatus
		// is a plain kernel status read (no hardware bus transaction), so it's cheap
		// enough to self-throttle to 1/sec (see cpu_usage_tick) inline on this thread
		// rather than needing battery_poll_thread's dedicated one.
		if (g_show_cpu_usage) cpu_usage_tick(sceKernelGetSystemTimeLow());

		if (g_real_wait_vblank_start) g_real_wait_vblank_start();
		else sceKernelDelayThread(16000);

		// One GPU sample per loop (~vblank rate), not once/sec like CPU.
		if (g_show_cpu_usage) gpu_usage_sample();

		if ((g_show_fps_overlay || g_show_battery || g_show_cpu_usage || g_show_ft_chart || g_vskip_banner) && !g_menu_open) {
			void *topaddr = NULL;
			int bufferwidth = 0, pixelformat = 0;
			u32 t0 = sceKernelGetSystemTimeLow();
			int got = (sceDisplayGetFrameBuf(&topaddr, &bufferwidth, &pixelformat, PSP_DISPLAY_SETBUF_IMMEDIATE) >= 0
			           && topaddr && bufferwidth > 0);
			// No tick here — this thread only draws whatever fps_tick (now
			// correctly firing on every real SetFrameBuf call, see the hook
			// above) has already computed. An address-based tick attempt here
			// was tried and removed: it was solving the wrong problem
			// (address-change detection) instead of the actual bug (the hook
			// requiring an address change at all).
			if (got) fps_draw(topaddr, bufferwidth, pixelformat);
			if (got && g_vskip_banner) vskip_banner_draw(topaddr, bufferwidth, pixelformat);
			{
				u32 t1 = sceKernelGetSystemTimeLow();
				u32 draw_us = t1 - t0;
				float fps = sceDisplayGetFramePerSec();
				u32 vblank_us = (fps > 0.0f) ? (u32)(1000000.0f / fps) : 16666;
				u32 vblank_min = vblank_us / 10;
				u32 vblank_max = vblank_us - vblank_min;
				u32 delay = (draw_us * 3 < vblank_us) ? (vblank_us - draw_us * 3) : 0;
				if (delay < vblank_min) delay = vblank_min;
				if (delay > vblank_max) delay = vblank_max;
				sceKernelDelayThread(delay);
			}
			if (got && (g_show_fps_overlay || g_show_battery || g_show_cpu_usage || g_show_ft_chart) && !g_menu_open)
				fps_draw(topaddr, bufferwidth, pixelformat);
			if (got && g_vskip_banner && !g_menu_open)
				vskip_banner_draw(topaddr, bufferwidth, pixelformat);
		}

		// [FLIMIT] frame-limiter counters — drained HERE, on the poll thread, and never
		// from the present or GE hooks: uart_puts is a blocking register write (~5.5ms
		// for a 64-char line at 115200), which on the game's thread would perturb the
		// very pacing it measures. Emitted after both draws above so the write cost
		// lands between loop iterations rather than between the two draws (which is what
		// the second draw exists to avoid — see above).
		//
		// How to read it for a game that will not cap:
		//   ge=0            the GE hook never fires — nothing to pace against.
		//   extra > 0       a MULTI-LIST game: it submits several GE lists per visual
		//                   frame. Expected to be harmless (those lists are skipped), but
		//                   this is the shape that would stall a game if the guard broke.
		//   paced > ge      something marks frames more than once each — the cap would
		//                   deliver target/N. Should be impossible now; if it appears,
		//                   something other than a present is marking frames.
		//   paced ~= 0, fail ~= ge   every sleep refused AND no thread presents to fall
		//                   back on: that game cannot be paced by sleeping at all.
		//   paced ~= 0, fail = 0     already under the cap; nothing to do.
		//   fail ~= ge      normal and healthy for Tron: its GE sleeps always refuse and
		//                   the present site does the work. err= names the reason.
		if (DBG_UART() && g_frame_limit > 0 && !g_vskip_active) {   // silent during Intro Video Skip — limiter is fully off
			u32 tnow = sceKernelGetSystemTimeLow();
			if (tnow - g_fl_log_last_us >= 1000000) {
				char b[160];
				sprintf(b, "[FLIMIT] cap=%d ge=%u paced=%u extra=%u intr=%u fail=%u err=%08x fps=%d target=%uus",
				        g_frame_limit, (unsigned)g_fl_seen, (unsigned)g_fl_paced,
				        (unsigned)g_fl_extra, (unsigned)g_fl_intr, (unsigned)g_fl_failed,
				        (unsigned)g_fl_last_err, g_fps_value, (unsigned)g_fl_target_us);
				uart_puts(b);
				g_fl_log_last_us = tnow;
				g_fl_seen = 0; g_fl_paced = 0; g_fl_extra = 0; g_fl_intr = 0; g_fl_failed = 0;
			}
		}
	}
	return 0;
}

// Shared by both vblank-wait hooks below (StartCB is just the callback-polling
// variant of the same wait) — tier 2 of the tick cascade (see the block
// comment above install_fps_overlay_hook): only ticks once SetFrameBuf (tier
// 1) has gone quiet, and records when it does so tier 3 (GE-list, below)
// knows this tier is already covering it.
static void fps_maybe_tick_from_vblank(void)
{
	if ((g_show_fps_overlay || g_show_ft_chart) && !g_menu_open) {
		u32 now = sceKernelGetSystemTimeLow();
		if (now - g_fps_recent_setfb_time >= 200000 || g_fps_recent_setfb_count < 3) {
			fps_tick(now);
			g_fps_recent_vblank_time = now;
		}
	}
}

// Tier 3, the universal fallback: sceGeListEnQueue/EnQueueHead (see the block
// comment above install_fps_overlay_hook) is the one kernel primitive EVERY
// rendering technique must call to get anything drawn at all, regardless of
// buffering scheme — confirmed on real hardware that some games (GTA, in
// heavy-geometry view directions) stop calling BOTH SetFrameBuf and
// WaitVblankStart/StartCB entirely while still rendering normally, so tiers 1
// and 2 alone go completely silent for them. Only ticks once BOTH higher
// tiers have gone quiet (a game can legitimately submit several GE lists per
// visual frame, so this must never fire alongside a working tier 1/2 or it
// over-counts) — 200ms since the last SetFrameBuf activity, same threshold
// tier 2 uses, and 50ms since the last accepted vblank-tier tick (vblank
// itself is a fixed ~16.6ms cadence, so 50ms is several missed vblanks, a
// clear sign that tier is not the one covering this game right now).
static void fps_maybe_tick_from_ge(void)
{
	if ((g_show_fps_overlay || g_show_ft_chart) && !g_menu_open) {
		u32 now = sceKernelGetSystemTimeLow();
		if ((now - g_fps_recent_setfb_time >= 200000 || g_fps_recent_setfb_count < 3) &&
		    (g_fps_recent_vblank_time == 0 || now - g_fps_recent_vblank_time >= 50000))
			fps_tick(now);
	}
}

static int fps_wait_vblank_start_patched(void)
{
	fps_maybe_tick_from_vblank();
	return g_real_wait_vblank_start ? g_real_wait_vblank_start() : 0;
}

static int fps_wait_vblank_start_cb_patched(void)
{
	fps_maybe_tick_from_vblank();
	return g_real_wait_vblank_start_cb ? g_real_wait_vblank_start_cb() : 0;
}

static int fps_ge_list_enqueue_patched(const void *list, void *stall, int cbid, void *arg)
{
	// per-game frame cap — paces frame PRODUCTION here. The g_frame_limit test is inline
	// so OFF costs one load+branch and no call at all: this fires on EVERY GE submit, and
	// a GE-heavy phase can hit thousands per second (v652 logged ge=4658 in one second).
	if (g_frame_limit > 0 && !g_vskip_active) frame_limit_ge();
	fps_maybe_tick_from_ge();
	return g_real_ge_list_enqueue ? g_real_ge_list_enqueue(list, stall, cbid, arg) : 0;
}

static int fps_ge_list_enqueue_head_patched(const void *list, void *stall, int cbid, void *arg)
{
	// per-game frame cap — paces frame PRODUCTION here. The g_frame_limit test is inline
	// so OFF costs one load+branch and no call at all: this fires on EVERY GE submit, and
	// a GE-heavy phase can hit thousands per second (v652 logged ge=4658 in one second).
	if (g_frame_limit > 0 && !g_vskip_active) frame_limit_ge();
	fps_maybe_tick_from_ge();
	return g_real_ge_list_enqueue_head ? g_real_ge_list_enqueue_head(list, stall, cbid, arg) : 0;
}

// Lazily creates+starts fps_poll_thread the FIRST time it's needed - either at
// boot (if Show FPS was already on from settings.cfg, see install_fps_overlay_hook
// below) or later from the Settings menu the moment the user turns Show FPS OR
// Battery Status on mid-session. If the user turns BOTH back off, the thread
// notices at its own next loop iteration and exits itself (see fps_poll_thread),
// clearing g_fps_poll_started so THIS function spins up a fresh thread again
// next time. Shared with ram_usage_kb's Dynamic figure below (Settings footer)
// - keep in sync with the sceKernelCreateThread call this feeds, though "in
// sync" is now enforced by both sides reading this same constant instead of
// two magic numbers.
void fps_poll_ensure_started(void)
{
	if (g_fps_poll_started) return;
	g_fps_poll_started = 1;
	// Priority 32 matches a comparable reference plugin's own poll thread
	// exactly (confirmed via disassembly of its sceKernelCreateThread call) —
	// not the menu thread's priority 16, which runs far more eagerly than the
	// game's own threads typically do and caused a flicker bug when tried here.
	// (A CPU-load-correlated stall this thread appeared to have at this
	// priority turned out to be a tick-detection gap, not starvation — see the
	// 3-tier tick cascade above install_fps_overlay_hook — so this stays at
	// the reference plugin's own value rather than the more aggressive
	// priorities tried while that was still misdiagnosed.) Stack is 2048: even with the Battery ALL tier's extra lines,
	// every draw is still a SEQUENCE of shallow sprintf/dbg_text/dbg_print/
	// dbg_putchar calls (each one's frame freed before the next starts), never
	// a deeper call chain than the plain FPS-only case — so peak stack depth
	// doesn't grow with how many lines get drawn, only wall-clock cost does.
	// Keeps real headroom for the real kernel calls in this thread
	// (sceDisplayGetFrameBuf, WaitVblankStart, GetFramePerSec, the battery
	// queries) whose own internal stack usage isn't ours to verify.
	{
		SceUID thid = sceKernelCreateThread("pspstates_overlay_poll", fps_poll_thread, 32, FPS_POLL_STACK_BYTES, 0, NULL);
		if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
	}
}

void install_fps_overlay_hook(void)
{
	static int installed;
	if (installed) return;
	installed = 1;

	g_fps_last_tick_us = 0;
	g_fps_window_start_us = 0;
	g_fps_window_count = 0;
	g_fps_value = 0;
	g_fps_recent_setfb_count = 0;
	g_fps_recent_setfb_time = 0;
	g_fps_recent_vblank_time = 0;
	memset(g_fps_ft_buf, 0, sizeof(g_fps_ft_buf));
	g_fps_ft_idx = 0;
	g_fps_lows_last_calc_us = 0;
	g_fps_low1_value = 0;
	g_cpu_last_clock_us = 0;
	g_cpu_last_idle_us = 0;
	g_cpu_usage_pct = -1;
	g_gpu_busy_samples = 0;
	g_gpu_total_samples = 0;
	g_gpu_usage_pct = -1;
	memset(g_ft_chart, 0, sizeof(g_ft_chart));
	memset(g_ft_chart_bound, FT_BOUND_NONE, sizeof(g_ft_chart_bound));
	g_perf_bound = -1;

	g_fl_anchor_us = 0;
	g_fl_pending = 0;
	g_fl_saw_present = 0;
	g_fl_ge_refused = 0;
	g_fl_reprobe = 0;
	g_fl_cached_for = 0;
	g_fl_target_us = 0;
	g_fl_seen = 0;
	g_fl_paced = 0;
	g_fl_extra = 0;
	g_fl_intr = 0;
	g_fl_failed = 0;
	g_fl_last_err = 0;
	g_fl_log_last_us = 0;
	{
		// Real panel period for the frame limiter — the LCD is 59.94Hz, not 60.000
		// (see frame_limit_ge). Read ONCE here and kept as an int so the present
		// hook never does float math on the game's thread. Same derivation
		// fps_poll_thread already uses for its own draw budget.
		float hz = sceDisplayGetFramePerSec();
		g_fl_vblank_us = (hz > 0.0f) ? (u32)(1000000.0f / hz) : 16683;
	}

	g_real_display_set_frame_buf = (int (*)(void *, int, int, int))
		sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x289D82FE);
	if (g_real_display_set_frame_buf)
		sctrlHENPatchSyscall((void *)g_real_display_set_frame_buf, fps_display_set_frame_buf_patched);

	g_real_wait_vblank_start = (int (*)(void))
		sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x984C27E7);
	if (g_real_wait_vblank_start)
		sctrlHENPatchSyscall((void *)g_real_wait_vblank_start, fps_wait_vblank_start_patched);

	g_real_wait_vblank_start_cb = (int (*)(void))
		sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", 0x46F186C3);
	if (g_real_wait_vblank_start_cb)
		sctrlHENPatchSyscall((void *)g_real_wait_vblank_start_cb, fps_wait_vblank_start_cb_patched);

	// Tier 3 tick source (see fps_maybe_tick_from_ge's block comment) — module
	// "sceGE_Manager" confirmed via uofw's ge.c SCE_MODULE_INFO, library
	// "sceGe_user" confirmed via pspsdk's sceGe_user.S IMPORT_START, NIDs from
	// psplibdoc_660.xml.
	g_real_ge_list_enqueue = (int (*)(const void *, void *, int, void *))
		sctrlHENFindFunction("sceGE_Manager", "sceGe_user", 0xAB49E76A);
	if (g_real_ge_list_enqueue)
		sctrlHENPatchSyscall((void *)g_real_ge_list_enqueue, fps_ge_list_enqueue_patched);

	g_real_ge_list_enqueue_head = (int (*)(const void *, void *, int, void *))
		sctrlHENFindFunction("sceGE_Manager", "sceGe_user", 0x1C0D95A6);
	if (g_real_ge_list_enqueue_head)
		sctrlHENPatchSyscall((void *)g_real_ge_list_enqueue_head, fps_ge_list_enqueue_head_patched);

	battery_resolve_syscon();
	if (g_show_fps_overlay || g_show_battery || g_show_cpu_usage || g_show_ft_chart) fps_poll_ensure_started();
	if (g_show_battery) battery_poll_ensure_started();
}
