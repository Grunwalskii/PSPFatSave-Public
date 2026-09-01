#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "corevolt.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"
#include "screen_tuning.h"

// ── Live FPS overlay (optional, drawn during actual gameplay — menu closed) ──
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
// A "tick" (fps_tick(), one countable frame-advance event) comes from a
// 3-tier cascade, each tier only ticking once the one above it has gone quiet:
//   Tier 1 — SetFrameBuf: ticks on EVERY call, no address-change requirement.
//     Updates g_fps_recent_setfb_time/_count, read by tiers 2 and 3 below.
//   Tier 2 — WaitVblankStart/StartCB: ticks only when SetFrameBuf hasn't
//     fired at least 3 times in the trailing ~200ms (g_fps_recent_setfb_time/
//     _count) — lets a game that's stopped calling SetFrameBuf still get
//     ticks from vblank instead. Updates g_fps_recent_vblank_time, read by
//     tier 3.
//   Tier 3 — sceGeListEnQueue/EnQueueHead: universal fallback for a game gone
//     silent on BOTH tiers 1 and 2. Only ticks once tier 1 AND tier 2 have
//     both been quiet for a while (see fps_maybe_tick_from_ge) — a game can
//     legitimately submit several GE lists per visual frame, so this must
//     never fire alongside a working tier 1/2 or it over-counts.
// Each tick feeds a sliding window whose length is the FPS update rate
// (g_fps_rate, 0.1..1.0s; see fps_window_us()): once now - windowStart >= that length,
// fps = ticksInWindow * 1,000,000 / elapsed, then the window resets. Deltas
// closer together than FPS_REPRESENT_FLOOR_US are discarded as the same frame
// presented twice (see fps_tick).
//
// 1% Low (g_fps_show_lows): every accepted tick's frametime (the delta itself,
// in microseconds) is pushed into a 256-entry ring buffer. Sorting/scanning
// that buffer every single tick would cost real CPU, so it's only rescanned
// once per real second (g_fps_lows_last_calc_us), independent of the main FPS
// display's own window length. Each rescan pulls out the slowest 1% of
// recorded frametimes (~3 of 256), averages THOSE frametimes, then converts
// that average back to an FPS figure — the standard "worst 1% of frames"
// stutter metric.
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
// 1px baseline and frametime ABOVE it grows the bar at 1.25ms/px. A 1px minimum
// keeps every column visible (no 0px gaps). g_ft_chart[0] is leftmost/oldest,
// [FT_CHART_W-1] is newest; each tick shifts left and appends.
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

// Bars grow AWAY from whichever edge they're anchored to, one column at a
// time — drawn fresh every call (only the bar pixels are written, so the
// game shows through everywhere else, same as the rest of this overlay).
// Anchor mirrors g_overlay_pos but on the OPPOSITE edge from the text block:
// Up modes (text at top) anchor the chart to the bottom edge, baseline row
// 271, bars growing upward — the original layout. Down modes (text at
// bottom) anchor the chart to the TOP edge instead, baseline row 0, bars
// growing downward (flipped) — this keeps the chart clear of the bottom-
// anchored text without needing to reserve space for it there. Colored per
// column by g_ft_chart_bound: OVERLAY_BLUE (CPU-bound) / OVERLAY_ORANGE
// (GPU-bound) / OVERLAY_FG (no data — CPU Usage was off) — see its own
// comment above ft_chart_tick.
void ft_chart_draw(void)
{
	int x, y, w = (FT_CHART_W < dbg_bufw) ? FT_CHART_W : dbg_bufw;
	int flip = (g_overlay_pos >= 2);   // Down: chart anchors the top edge, bars grow downward
	int baseline = flip ? 0 : 271;
	int dir = flip ? 1 : -1;           // row step per unit of bar height
	if (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
		volatile u32 *base = (volatile u32 *)dbg_fb + baseline * dbg_bufw;
		for (x = 0; x < w; x++) {
			int h = g_ft_chart[x];
			u32 c = (g_ft_chart_bound[x] == FT_BOUND_GPU) ? OVERLAY_ORANGE
			      : (g_ft_chart_bound[x] == FT_BOUND_CPU) ? OVERLAY_BLUE : OVERLAY_FG;
			for (y = 0; y < h; y++) base[x + dir * y * dbg_bufw] = c;
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
			for (y = 0; y < h; y++) base[x + dir * y * dbg_bufw] = c;
		}
	}
}

// Averaging window = g_fps_rate in 0.1s units (1..10 = 0.1..1.0s), set with
// Triangle/X on the settings menu's Show FPS row. Out-of-range (corrupt
// settings.cfg) falls back to 1s — fps_tick() is only reached at all when the
// overlay is on.
u32 fps_window_us(void)
{
	int r = (g_fps_rate >= 1 && g_fps_rate <= 10) ? g_fps_rate : 10;
	return (u32)r * 100000;   // 0.1s units -> µs (0.1..1.0s)
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
// frame shown twice, not a new frame.
#define FPS_REPRESENT_FLOOR_US 12000

void fps_tick(u32 now)
{
	if (g_fps_last_tick_us != 0) {
		u32 delta = now - g_fps_last_tick_us;
		// Discard a re-present of the frame already counted (FPS_REPRESENT_FLOOR_US).
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
// sceKernelReferSystemStatus() reports idleClocks, the running total of CPU
// clocks spent in the kernel's idle thread; comparing its delta against the
// elapsed wall-clock delta (sceKernelGetSystemTimeLow(), the same timer
// fps_tick already uses) over a >=1s window gives load% without ever touching
// a hardware register — cheap enough to run inline on fps_poll_thread.
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
	if (g_cpu_last_clock_us != 0 && (now - g_cpu_last_clock_us) < fps_window_us()) return;   // update at the FPS window rate (0.1..1.0s), not a fixed 1s
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
// Percent (0-100) and LifeTime (minutes) come from scePower* calls;
// RemainCapacity and FullCapacity are in mAh. Power (mW) is OUR OWN derived
// value: Volt_mV * Current_mA / 1000, not a separate query.
static int g_batt_exists   = 0;
static int g_batt_ac       = 0;
static int g_batt_charging = 0;
static int g_batt_low      = 0;
static int g_batt_percent  = -1;   // -1 = not read yet / no battery
static int g_batt_life_min = -1;
static int g_batt_remain_mah = 0;
static int g_batt_full_mah   = 0;
static int g_batt_temp_c     = 0;    // battery temp in °C
static int g_batt_volt_mv    = 0;    // battery voltage in mV
static int g_batt_current_ma = 0;    // via sceSysconBatteryGetElec; negative = discharging

// sceSyscon_driver battery NIDs — resolved once via sctrlHENFindFunction.
static int (*g_syscon_batt_current)(int *) = NULL;
// Resolved at runtime rather than linked, so neither depends on what a given
// toolchain's static import library has.
static int (*g_power_remain_cap)(void) = NULL;
static int (*g_power_full_cap)(void)   = NULL;
// Charge gate (Stop-Charging setting): scePowerBatteryForbidCharging /
// PermitCharging (scePower_Service/scePower_driver NIDs from ARK-4's nidmap).
// These are the firmware's own charge on/off switches (they end up in
// sceSysconCtrlCharge, syscon cmd 0x56 — the ONLY charge control the syscon
// protocol exposes; there is no "set charge current" command). Resolved lazily
// alongside the other battery functions.
static int (*g_power_forbid_chg)(void) = NULL;
static int (*g_power_permit_chg)(void) = NULL;
// Tracks what we last TOLD the firmware, so the poll thread only writes on a
// change (each call is a syscon transaction) and so a threshold change or a
// re-permit is a deliberate transition, not a per-poll hammering. -1 = unknown
// (nothing sent yet this session).
static int g_chg_gate_sent = -1;

static void battery_resolve_syscon(void)
{
	static int resolved;
	if (resolved) return;
	resolved = 1;
	g_syscon_batt_current = (int (*)(int *))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0x483088B0);
	g_power_remain_cap    = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0x94F5A53F);
	g_power_full_cap      = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0xFD18A0FF);
	g_power_forbid_chg    = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0x166922EC);
	g_power_permit_chg    = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0xDD3D4DAC);
}

// Applies the Stop-Charging threshold (g_batt_stop_charge: 0=OFF, 100=ON/never
// charge, else 70..95). Called ONLY from battery_poll_thread (the calls are real
// syscon transactions). OFF -> always permit (and only if we previously forbade).
// ON -> always forbid. Threshold set -> forbid at/above it, permit below it.
// g_chg_gate_sent makes each transition a single write, and lets a threshold
// change re-evaluate immediately.
static void battery_charge_gate(void)
{
	int want;
	if (!g_power_forbid_chg || !g_power_permit_chg) return;
	if (g_batt_stop_charge <= 0) {
		want = 0;   // permit
	} else if (g_batt_stop_charge >= 100) {
		want = 1;   // ON: never charge
	} else {
		want = (g_batt_percent >= g_batt_stop_charge) ? 1 : 0;   // 1 = forbid
	}
	if (want == g_chg_gate_sent) return;
	if (want) g_power_forbid_chg(); else g_power_permit_chg();
	g_chg_gate_sent = want;
}

// Queries the firmware/syscon directly — real hardware bus transactions, not
// cheap RAM reads, so this only ever runs from battery_poll_thread's own slow
// loop (see there), never from anything drawing-related. remain_mah/full_mah/
// current_ma are queried from the Percent+Time tier up (g_show_battery>=2);
// temp/volt stay gated to g_show_battery==3 only.
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
// battery_refresh's sceSyscon* calls are real hardware bus transactions, so
// they run here at this thread's own pace and cache the results into plain
// ints. battery_draw (called from fps_draw) only ever reads those cached
// values — it never queries anything itself.
int g_battery_poll_started = 0;
int battery_poll_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	while (1) {
		// This thread already does syscon transactions at its own pace, so it is a
		// legal place to put a non-stock core-voltage step back after a firmware
		// resume (ProcessSignals only raises the flag — see corevolt.c). The menu
		// thread does the same on wake, for when this thread isn't running.
		cv_poll_reapply();
		// Exit only when there is NOTHING to do: overlay off AND no Stop-Charging
		// threshold. A threshold alone keeps this thread alive so the gate keeps
		// being enforced even with the battery overlay off.
		if (!g_show_battery && g_batt_stop_charge <= 0) {
			// Leaving with charging forbidden would leave the PSP never charging
			// after the overlay is turned off — always re-permit on the way out.
			if (g_chg_gate_sent == 1 && g_power_permit_chg) { g_power_permit_chg(); g_chg_gate_sent = 0; }
			g_battery_poll_started = 0;   // mirrors fps_poll_thread's own clean-exit pattern
			sceKernelExitDeleteThread(0);
			return 0;
		}
		battery_refresh();
		battery_charge_gate();
		// Reuses the Show FPS interval rather than a separate battery-only
		// setting. This thread just sleeps between queries.
		sceKernelDelayThread(fps_window_us());
	}
	return 0;
}

// Lazily starts battery_poll_thread — mirrors fps_poll_ensure_started exactly
// (see its own comment), just for the battery thread instead of the overlay
// draw thread. Low priority (64, well below the overlay thread's 32).
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
// touches the glyph via 8-neighbour dilation. Kept SEPARATE from the glyph
// table because the two carry different colours (glyph = runtime dbg_fg;
// halo = always black).
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
// Every fg pixel gets a 1px black halo (from the pre-baked g_small_halo mask)
// so the text stays readable over any game background; halo pixels are forced
// black even when dbg_transparent is set (true background pixels, i.e. neither
// fg nor halo, still honor dbg_transparent/dbg_bg as before). Draw region is
// therefore 7x9 (one halo pixel beyond each glyph edge), each pixel
// bounds-checked individually rather than aborting the whole glyph near screen
// edges.
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

// ── Overlay anchor helpers (g_overlay_pos: 0=Up Left .. 3=Down Right) ──────
// Down modes anchor the WHOLE block on the bottom edge, keeping the same
// top-to-bottom order as Up (FPS first, battery last); Right modes right-align
// each line on the 480px display.
static int ov_x(const char *s)
{
	int x = (g_overlay_pos & 1) ? 480 - (int)strlen(s) * 6 : 0;
	return (x < 0) ? 0 : x;   // clamp: an over-long line never starts past the edge
}
// Line count battery_draw will emit — needed to size the block BEFORE drawing
// it (bottom anchoring). Must mirror battery_draw's own branch structure.
static int batt_line_count(void)
{
	if (!g_show_battery) return 0;
	if (!g_batt_exists) return 1;
	return (g_show_battery >= 3) ? 3 : 1;
}

// Draws the battery block starting at pixel row `y`; returns the next free
// y (so fps_draw can stack FPS lines above it without a fixed offset), 8px
// per line (the small font is 7px tall; +1px gap).
// Percent tier: 1 line ("<pct>%"). +Time tier: same line, time appended
// ("<pct>% <time>") — no separate line, no "Batt:"/"Time:" prefixes; AC/
// charging is conveyed by the time showing "--:--" rather than a separate
// AC/Chg suffix. ALL tier: +2 more lines — capacity+temp, then a bare
// volt/current/power line with no field labels at all (unit suffixes alone
// identify each number, e.g. "3745 MV 450 MA 1.73 W"). Pure cache read: no
// query, no gating, drawn every call in whatever mode the caller (fps_draw)
// already set — the actual refresh happens on the fully separate
// battery_poll_thread, never on this draw path.
int battery_draw(int y)
{
	char buf[40];
	if (!g_show_battery) return y;
	if (!g_batt_exists) { small_text(ov_x("BATT: NONE"), y, OVERLAY_FG, BR_BG, "BATT: NONE"); return y + 8; }

	{
		u32 fg = g_batt_low ? 0xFF0000FF : OVERLAY_FG;   // red when the firmware itself flags low battery
		// Scale the BMS minutes by real/full (1x when no override or full unknown).
		// Overflow-safe: minutes*8000 fits u32.
		int sc_num = 1, sc_den = 1;
		if (g_batt_real_mah > 0 && g_batt_full_mah > 0) { sc_num = g_batt_real_mah; sc_den = g_batt_full_mah; }
		if (g_show_battery == 1) {
			sprintf(buf, "%d%%", g_batt_percent);
		} else if (g_batt_ac || g_batt_charging) {
			// Charging: time-to-full = remaining capacity gap / charge current
			int target_mah = g_batt_full_mah - (g_batt_full_mah / 10);
			if (g_batt_current_ma > 0 && target_mah > g_batt_remain_mah) {
				int min_left = ((target_mah - g_batt_remain_mah) * 60) / g_batt_current_ma;
				min_left = (int)((u32)min_left * (u32)sc_num / (u32)sc_den);   // scale to the real cell
				sprintf(buf, "%d%% %d:%02d", g_batt_percent, min_left / 60, min_left % 60);
			} else {
				sprintf(buf, "%d%% --:--", g_batt_percent);
			}
		} else if (g_batt_life_min < 0) {
			sprintf(buf, "%d%% --:--", g_batt_percent);
		} else {
			int life = (int)((u32)g_batt_life_min * (u32)sc_num / (u32)sc_den);   // scale to the real cell
			sprintf(buf, "%d%% %d:%02d", g_batt_percent, life / 60, life % 60);
		}
		small_text(ov_x(buf), y, fg, BR_BG, buf); y += 8;

		// Charge-direction triangle, driven by the ACTUAL battery current
		// (sceSysconBatteryGetElec, mA; negative = discharging): >0 = charging
		// (green up), <0 = discharging (orange down), 0 = no arrow at all
		// (AC plugged in but battery idle/full, or current not readable).
		// Deliberately NOT keyed off scePowerIsBatteryCharging(), which reports
		// the firmware's charge state and can disagree with the real current
		// flow. g_batt_current_ma is refreshed in the same tier (>=2) that
		// draws this arrow, so the two always stay in sync.
		if (g_show_battery >= 2 && g_batt_current_ma != 0) {
			int arrow_x = ov_x(buf) + (int)strlen(buf) * 6 + 1;
			int charging = g_batt_current_ma > 0;
			dbg_fg = charging ? OVERLAY_GREEN : OVERLAY_ORANGE;
			dbg_bg = BR_BG;
			small_putchar(arrow_x, y - 8, charging ? '^' : '_');
		}
	}
	if (g_show_battery <= 2) return y;   // Percent, or Percent + Time

	{
		// Scale mAh to the real cell: show full = real capacity and
		// remain = reported_remain * real / reported_full (percentage-preserving).
		int disp_full   = g_batt_full_mah;
		int disp_remain = g_batt_remain_mah;
		if (g_batt_real_mah > 0 && g_batt_full_mah > 0) {
			// 32-bit math (no 64-bit divide): remain*real <= ~2000*8000 fits in u32.
			disp_full   = g_batt_real_mah;
			disp_remain = (int)((u32)g_batt_remain_mah * (u32)g_batt_real_mah / (u32)g_batt_full_mah);
		}
		sprintf(buf, "%d/%d MAH %d C", disp_remain, disp_full, g_batt_temp_c);
	}
	small_text(ov_x(buf), y, OVERLAY_FG, BR_BG, buf); y += 8;
	{
		// Power (mW) is derived: Volt_mV * Current_mA / 1000. Sign follows Current
		// (negative while discharging). Displayed as W with 2 decimals
		// (hundredths of a watt = mW/10), fixed-point integer math, not %f.
		int power_mw = (g_batt_volt_mv * g_batt_current_ma) / 1000;
		int neg = power_mw < 0;
		u32 p100 = (u32)((neg ? -power_mw : power_mw) / 10);
		sprintf(buf, "%d MV %d MA %s%u.%02u W", g_batt_volt_mv, g_batt_current_ma,
		        neg ? "-" : "", p100 / 100, p100 % 100);
		small_text(ov_x(buf), y, OVERLAY_FG, BR_BG, buf); y += 8;
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
		int nl = 0;   // overlay line count (8px/line) — sizes the block BEFORE drawing
		int y;
		if (g_show_fps_overlay) { nl++; if (g_fps_show_lows) nl++; }
		if (g_show_cpu_usage && g_cpu_usage_pct >= 0) nl++;
		nl += batt_line_count();
		// Anchor (g_overlay_pos): Up modes start at row 0; Down modes sit the block
		// on the bottom edge in the SAME order (FPS first, battery last). The
		// frametime chart anchors the OPPOSITE edge from the text (see
		// ft_chart_draw), so the two never share screen space and no extra
		// offset is needed here to keep them apart.
		y = (g_overlay_pos >= 2) ? 272 - nl * 8 : 0;
		dbg_transparent = 1;   // glyph pixels only — bg param below is otherwise an opaque box
		if (g_show_fps_overlay) {
			sprintf(buf, "%d FPS", g_fps_value);
			small_text(ov_x(buf), y, OVERLAY_FG, BR_BG, buf); y += 8;
			if (g_fps_show_lows) {
				sprintf(buf, "%d 1%%", g_fps_low1_value);
				small_text(ov_x(buf), y, OVERLAY_FG, BR_BG, buf); y += 8;
			}
		}
		if (g_show_cpu_usage && g_cpu_usage_pct >= 0) {
			// Two separate calls so the bottleneck (g_perf_bound) can be
			// highlighted (OVERLAY_BLUE=CPU, OVERLAY_ORANGE=GPU) while the
			// other stays OVERLAY_FG. Right modes align the COMBINED line: x0
			// spans CPU + GPU text together (6px/char advance).
			int x0;
			sprintf(buf, "%d%% CPU", g_cpu_usage_pct);
			x0 = ov_x(buf);
			if (g_gpu_usage_pct >= 0) {
				char g2[24];
				int off = (int)strlen(buf) * 6;
				sprintf(g2, " %d%% GPU", g_gpu_usage_pct);
				if (g_overlay_pos & 1) x0 = 480 - (off + (int)strlen(g2) * 6);
				if (x0 < 0) x0 = 0;
				small_text(x0 + off, y, (g_perf_bound == FT_BOUND_GPU) ? OVERLAY_ORANGE : OVERLAY_FG, BR_BG, g2);
			}
			small_text(x0, y, (g_perf_bound == FT_BOUND_CPU) ? OVERLAY_BLUE : OVERLAY_FG, BR_BG, buf);
			y += 8;
		}
		battery_draw(y);
		if (g_show_ft_chart) ft_chart_draw();
		dbg_transparent = 0;   // restore: menu/panel drawing elsewhere expects this default
	}
}


// ── Frame limiter (PER-GAME, g_frame_limit) ─────────────────────────────────
// Paces the game by sleeping in the GE-ENQUEUE hook: a frame is PRODUCED by
// the game's thread submitting a render (sceGeListEnQueue), whereas
// sceDisplaySetFrameBuf is only PRESENTATION.
//
// THE FRAME BOUNDARY IS THE GE SUBMIT — not the present. A GE submit sets
// g_fl_pending and that is the only thing that ever does.
//
// TWO sleep sites, one pending flag: the GE site marks the frame and tries
// first; if its sleep is refused the frame stays pending and the present site
// sleeps for it instead. A refused sleep is a total no-op — it must not touch
// the anchor either, or the grid the other site inherits is corrupt.
//
// g_fl_anchor_us is the ideal grid point the cadence locks to. It advances by
// exactly target_us whenever we waited, so DelayThread's overshoot is absorbed
// rather than accumulating into drift; if the game is already slower than the
// cap there is nothing to do and it resets to now.
//
// Panel timing: the LCD is 59.94Hz (pspsdk pspdisplay.h), so g_fl_vblank_us
// takes the period from sceDisplayGetFramePerSec() instead (read once at
// install, kept as an int: no float math on the game's thread).
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

// Period for a target rate, snapped to a whole-vblank multiple only when that
// rate really is a refresh divisor (60 -> 16683, 30 -> 33367; also 20/15 if
// ever offered). The 2.5% tolerance rejects any snap that would change the
// rate the user asked for. Rates with no whole-vblank period keep their exact
// period; their frames alternate refresh counts (40fps is 1.5 vblanks, so
// 2/1/2/1).
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
			// Refused, not taken (interrupts or dispatch disabled). A TOTAL no-op on
			// purpose: leave pending set so the other site sleeps for this same frame,
			// and leave the anchor alone so that site inherits the same grid.
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
	// otherwise this is another GE list of the frame already in flight. Games
	// legitimately submit several lists per visual frame, and pacing each would
	// advance the grid far ahead of real time, stalling the game. g_fl_saw_present
	// is a FLAG, so multiple presents per frame collapse into a single mark.
	if (g_fl_saw_present) { g_fl_saw_present = 0; g_fl_pending = 1; }
	if (!g_fl_pending) { g_fl_extra++; return; }   // another list of the current frame
	// Whether a game's GE site can sleep is a FIXED property of that game, so stop
	// re-deriving it every frame. Once latched, skip straight to the present site
	// and re-probe only every 256th submit (~4/sec at 60fps) in case the game's
	// context changes. The probe counter is used instead of a clock so the skip
	// path costs no kernel call.
	if (g_fl_ge_refused >= GE_REFUSE_LATCH) {
		if (++g_fl_reprobe & 0xFF) return;
		g_fl_ge_refused = 0;
	}
	frame_limit_pace(1);
}

// From the END of the present hook — never before fps_tick, which needs
// untouched arrival timestamps. Does nothing unless the GE site's sleep was
// refused. NOTE it must not mark frames: multiple presents per frame would
// otherwise double-pace.
static void frame_limit_present(void)
{
	if (g_menu_open) return;                         // frame_limit_ge owns the reset
	// Mark that something reached the screen — from ANY context. This is what lets
	// the next GE submit tell a NEW frame from another list of this one.
	g_fl_saw_present = 1;
	if (!g_fl_pending) return;                       // the GE site already paced this frame
	if (sceKernelIsIntrContext()) return;            // interrupt flip — unpaceable
	frame_limit_pace(0);
}

int fps_display_set_frame_buf_patched(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
	// IPS gamma pass FIRST, before the CPU overlays below — the GE pass would
	// re-curve their pixels if drawn under it. Handles its own enable/menu/intr
	// gating (interrupt-context presents are deferred to its worker thread).
	st_on_present(topaddr, bufferwidth, pixelformat);

	// Intro-skip CAPTURE banner — see vskip_banner_draw. Drawn here for games that DO call
	// SetFrameBuf, and from fps_poll_thread for games that don't.
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

			// Tick on every real call — address-independent (some games present the
			// SAME buffer every frame). A phantom re-present is filtered downstream by
			// fps_tick's FPS_REPRESENT_FLOOR_US discard, not here.
			//
			// This MUST be reached with the ARRIVAL timestamp — the sleep site below is
			// deliberately after this block. FPS_REPRESENT_FLOOR_US pairs a re-present
			// with its frame correctly on arrival times.
			fps_tick(now);
		}
		fps_draw(topaddr, bufferwidth, pixelformat);
	}
	// Live gamma HUD on top of everything, and after st_on_present above so the
	// banner itself never gets curved (see st_hud_draw).
	if (g_st_hud && !g_menu_open && topaddr && bufferwidth > 0)
		st_hud_draw(st_pops_hud_fb(topaddr), bufferwidth, pixelformat);
	// Fallback sleep site, only for a frame whose GE submit refused to sleep. Runs
	// after fps_tick above, never before it — fps_tick needs untouched ARRIVAL
	// timestamps for FPS_REPRESENT_FLOOR_US to pair a re-present with its frame.
	// This does NOT mark a frame boundary: only the GE submit marks frames. Inline
	// OFF test, same reason as the GE call site. Skip during intro video skip to
	// preserve video timing.
	if (g_frame_limit > 0 && !g_vskip_active) frame_limit_present();
	return g_real_display_set_frame_buf ? g_real_display_set_frame_buf(topaddr, bufferwidth, pixelformat, sync) : 0;
}

// Drawing from the SetFrameBuf/vblank HOOKS only fires when the game calls
// the specific syscall being hooked. This thread doesn't depend on the game
// calling anything: it asks the display controller directly for whatever
// buffer is CURRENTLY live.
//
// Block on the REAL display vblank via g_real_wait_vblank_start() directly
// (the resolved original, NOT the hooked syscall — calling the hooked
// sceDisplayWaitVblankStart here would re-enter the patch and double-tick the
// FPS counter), draw immediately, then measure how long that draw took and
// sleep the REST of the vblank period (clamped to 10%-90% of it) before
// drawing a SECOND time just before the next real flip. Two draws spread
// across the vblank period make it more likely at least one lands after the
// game's own GE has finished refreshing that buffer for this cycle.
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
			// No tick here — this thread only draws whatever fps_tick (firing on
			// every real SetFrameBuf call, see the hook above) has already computed.
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
		// from the present or GE hooks: uart_puts is a blocking register write, which
		// on the game's thread would perturb the very pacing it measures. Emitted after
		// both draws above so the write cost lands between loop iterations.
		//
		// How to read it for a game that will not cap:
		//   ge=0            the GE hook never fires — nothing to pace against.
		//   extra > 0       a MULTI-LIST game: it submits several GE lists per visual
		//                   frame. Expected to be harmless (those lists are skipped).
		//   paced > ge      something marks frames more than once each — the cap would
		//                   deliver target/N. Should be impossible now.
		//   paced ~= 0, fail ~= ge   every sleep refused AND no thread presents to fall
		//                   back on: that game cannot be paced by sleeping at all.
		//   paced ~= 0, fail = 0     already under the cap; nothing to do.
		//   fail ~= ge      normal and healthy for a game whose GE sleeps always refuse
		//                   and the present site does the work. err= names the reason.
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
// rendering technique must call to get anything drawn at all. Only ticks once
// BOTH higher tiers have gone quiet (a game can legitimately submit several GE
// lists per visual frame, so this must never fire alongside a working tier 1/2
// or it over-counts) — 200ms since the last SetFrameBuf activity, same
// threshold tier 2 uses, and 50ms since the last accepted vblank-tier tick.
static void fps_maybe_tick_from_ge(void)
{
	if ((g_show_fps_overlay || g_show_ft_chart) && !g_menu_open) {
		u32 now = sceKernelGetSystemTimeLow();
		if ((now - g_fps_recent_setfb_time >= 200000 || g_fps_recent_setfb_count < 3) &&
		    (g_fps_recent_vblank_time == 0 || now - g_fps_recent_vblank_time >= 50000))
			fps_tick(now);
	}
}

// The REAL sceDisplayWaitVblankStart, for callers that need the display cadence
// without re-entering our own syscall patch (that would double-tick the FPS
// counter). Returns at the START of vblank. Used by the gamma worker to lock
// its pass to the display in games that never present. The delay fallback is
// only for the case where the NID never resolved.
int fps_wait_vblank_real(void)
{
	if (g_real_wait_vblank_start) return g_real_wait_vblank_start();
	sceKernelDelayThread(16000);
	return 0;
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
	// so OFF costs one load+branch and no call at all: this fires on EVERY GE submit.
	if (g_frame_limit > 0 && !g_vskip_active) frame_limit_ge();
	fps_maybe_tick_from_ge();
	// "game drew something" signal for the screen-tuning double-apply guard
	// (screen_tuning.c). Only GAME submits land here — our own list calls the
	// real function directly, bypassing this patch. Non-atomic ++ is fine: only
	// changed-vs-unchanged is read, a lost increment still reads as changed.
	g_st_ge_seq++;
	st_ge_on_submit(list);    // decide BEFORE the real enqueue (see screen_tuning.h)
	{
		int r = g_real_ge_list_enqueue ? g_real_ge_list_enqueue(list, stall, cbid, arg) : 0;
		st_ge_after_submit();   // ...and queue our correction BEHIND the game's list
		return r;
	}
}

static int fps_ge_list_enqueue_head_patched(const void *list, void *stall, int cbid, void *arg)
{
	// per-game frame cap — paces frame PRODUCTION here. The g_frame_limit test is inline
	// so OFF costs one load+branch and no call at all: this fires on EVERY GE submit.
	if (g_frame_limit > 0 && !g_vskip_active) frame_limit_ge();
	fps_maybe_tick_from_ge();
	g_st_ge_seq++;   // same signal as the tail-enqueue hook above
	st_ge_on_submit(list);    // decide BEFORE the real enqueue (see screen_tuning.h)
	{
		int r = g_real_ge_list_enqueue_head ? g_real_ge_list_enqueue_head(list, stall, cbid, arg) : 0;
		st_ge_after_submit();   // ...and queue our correction BEHIND the game's list
		return r;
	}
}

// Lazily creates+starts fps_poll_thread the FIRST time it's needed - either at
// boot (if Show FPS was already on from settings.cfg, see install_fps_overlay_hook
// below) or later from the Settings menu the moment the user turns Show FPS OR
// Battery Status on mid-session. If the user turns BOTH back off, the thread
// notices at its own next loop iteration and exits itself (see fps_poll_thread),
// clearing g_fps_poll_started so THIS function spins up a fresh thread again
// next time.
void fps_poll_ensure_started(void)
{
	if (g_fps_poll_started) return;
	g_fps_poll_started = 1;
	// Priority 32, not the menu thread's priority 16 (which runs more eagerly than
	// the game's own threads typically do). Stack is 2048: every draw is a SEQUENCE
	// of shallow sprintf/dbg_text/dbg_print/dbg_putchar calls, each one's frame
	// freed before the next starts, so peak stack depth doesn't grow with how many
	// lines get drawn — only wall-clock cost does. Keeps headroom for the real
	// kernel calls in this thread (sceDisplayGetFrameBuf, WaitVblankStart,
	// GetFramePerSec, the battery queries).
	{
		SceUID thid = sceKernelCreateThread("pspstates_overlay_poll", fps_poll_thread, 32, FPS_POLL_STACK_BYTES, 0, NULL);
		if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
	}
	// POPS: an overlay drawn into its single-buffered scanout flickers. Start the
	// screen-tuning worker too, which keeps the overlay steady via the ping-pong
	// shadow. Idempotent: st_ensure_started returns early if already running.
	if (g_is_pops) st_ensure_started();
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
		// Real panel period for the frame limiter — the LCD is 59.94Hz, not 60.000.
		// Read ONCE here and kept as an int so the present hook never does float math
		// on the game's thread.
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

	// Tier 3 tick source (see fps_maybe_tick_from_ge's block comment).
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
	// Battery thread also runs for a Stop-Charging threshold alone (overlay off).
	if (g_show_battery || g_batt_stop_charge > 0) battery_poll_ensure_started();
	// Runs AFTER load_settings (menu.c) — the persisted settings are already live
	// here. Logged unconditionally (not DBG_UART-routed): "did the screen-tuning
	// pass run at all" is the first question after any in-game hang, and with the
	// Debug setting off the log otherwise contains nothing but the boot banner.
	{
		char b[64];
		sprintf(b, "[ST] settings gam=%d tmp=%d", g_st_gamma, g_st_temp);
		uart_puts(b);
	}
	st_install_dialog_hooks();   // unconditional — see the declaration in screen_tuning.h
	// A saved temperature-only config must start the worker just as a gamma one does.
	if (st_active()) st_ensure_started();
}
