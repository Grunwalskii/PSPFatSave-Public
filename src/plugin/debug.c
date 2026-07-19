#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// VRAM row for the NEXT checkpoint printed — a plain sequential
// counter, deliberately independent of the checkpoint number `cp`
// (which is just a numeric tag for the RAM log/file log and differs
// per test function). Reset to 4 at the start of each top-level test
// function, right after dbg_capture_both_bufs().
int ms_test_row = 0;
// When set, the menu-open path skips ALL its Memory-Stick debug writes. Those writes
// are not just slow — the pad hook's WriteDebugLog runs on the GAME thread and parks it
// inside an MS I/O (holding the FATMS lock) right as we suspend it, which deadlocks the
// next log write. Save/load leave this 0 so their diagnostics are untouched.
int g_menu_quiet = 0;

#if DEBUG_BUILD
// Gate-log RAM buffer: a MENU freeze must do ZERO MS I/O between the first thread
// suspend and the MS probe — the fast gate can freeze a game thread in READY while
// it's INSIDE the FAT driver holding its lock, and the next [SUS] log append then
// deadlocks on that lock before the probe can detect it and retry (observed: menu
// open on GTA, last line "[PRE] threadmain st=0x2", hard freeze). With the Debug MS
// bit on, [PRE]/[SUS] lines from quiet (menu) freezes are buffered here and flushed
// by run_save_browser only AFTER the probe confirms the MS is usable (or after the
// game is resumed on abort).
static char g_gatelog[4096];
static int  g_gatelog_len = 0;
void gatelog_line(const char *line)
{
	int n = (int)strlen(line);
	if (g_gatelog_len + n + 1 >= (int)sizeof(g_gatelog)) return;   // full -> drop
	memcpy(g_gatelog + g_gatelog_len, line, n);
	g_gatelog_len += n;
	g_gatelog[g_gatelog_len++] = '\n';
}
void gatelog_flush(void)
{
	SceUID fd;
	if (!g_gatelog_len) return;
	fd = sceIoOpen("ms0:/pspstates_debug.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd >= 0) { sceIoWrite(fd, g_gatelog, g_gatelog_len); sceIoClose(fd); }
	g_gatelog_len = 0;
}
void gatelog_reset(void) { g_gatelog_len = 0; }

// Case-insensitive "does label contain 'fail'" check — used to color
// checkpoint lines green (expected/informational) vs red (unexpected).
// Stops at the label's null terminator, never reads past it.
static int label_has_fail(const char *label)
{
	int i;
	for (i = 0; label[i]; i++) {
		int j, match = 1;
		for (j = 0; j < 4; j++) {
			char c = label[i + j];
			if (!c) { match = 0; break; }
			if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
			if (c != "fail"[j]) { match = 0; break; }
		}
		if (match) return 1;
	}
	return 0;
}

// ── Non-intrusive per-checkpoint timing trace ──
// Two sinks, chosen by the Debug setting:
//   - UART on (DBG_UART): trace_record emits the line LIVE to the UART FIFO
//     immediately (register-only, a few µs — does NOT distort the timed window
//     the way an MS open/append/close per line would). Each line already carries
//     the logger's own [+µs] anchor prefix, so timing survives; and a mid-save
//     hang leaves the last checkpoint on the wire (better hang-localization than
//     a burst-after-the-fact). No RAM buffer, so NO 96-entry truncation on the
//     wire — the concern that motivated this change.
//   - UART off but MS on (DBG_MS): fall back to the old behavior — buffer
//     {elapsed-µs, cp, retval, label} into a small RAM array (no sceIo per line
//     in the frozen window) and dump it in ONE burst at trace_flush() AFTER the
//     timed section. This keeps crash-safe MS timing without per-line MS I/O.
// trace_start/record/flush are independent of DBG_SCR.
u64 now_us(void);   // defined below; used by the trace functions here
#define TRACE_CAP 64
static struct { u32 dt_us; u32 cp; u32 retval; const char *label; } g_trace[TRACE_CAP];
static int g_trace_n = 0;
static u64 g_trace_t0 = 0;
static int g_trace_active = 0;

void trace_start(void)
{
	g_trace_n = 0;
	g_trace_t0 = now_us();
	g_trace_active = 1;
}

static void trace_record(u32 cp, u32 retval, const char *label)
{
	if (!g_trace_active) return;
	if (DBG_UART()) {
		// Live UART: emit now, no buffering (no truncation, hang-localizing).
		char buf[96];
		u32 dt = (u32)(now_us() - g_trace_t0);
		sprintf(buf, "TRACE t=%uus CP%u %s ret=%d", (unsigned)dt, (unsigned)cp, label, (int)retval);
		uart_puts(buf);
		return;
	}
	// MS burst path: buffer to RAM (drops silently past TRACE_CAP — MS-only,
	// see trace_flush; the live UART path above has no such cap).
	if (g_trace_n >= TRACE_CAP) return;
	g_trace[g_trace_n].dt_us = (u32)(now_us() - g_trace_t0);
	g_trace[g_trace_n].cp = cp;
	g_trace[g_trace_n].retval = retval;
	g_trace[g_trace_n].label = label;
	g_trace_n++;
}

// Dump the buffered trace to the MS log in one burst (MS path only — the UART
// path already emitted each line live in trace_record, so there is nothing to
// flush there). Each line shows absolute elapsed (t=) and per-step delta (step=).
// Safe to call even if trace_start was never reached (n=0, no-op).
void trace_flush(const char *tag)
{
	int i;
	u32 prev = 0;
	if (!g_trace_active) return;
	g_trace_active = 0;
	if (g_trace_n == 0) { return; }   // UART path (or nothing recorded) -> no MS burst
	WriteDebugLogRaw(tag);
	for (i = 0; i < g_trace_n; i++) {
		char buf[96];
		u32 step = g_trace[i].dt_us - prev;
		prev = g_trace[i].dt_us;
		sprintf(buf, "TRACE t=%uus step=%uus CP%u %s ret=%d",
			(unsigned)g_trace[i].dt_us, (unsigned)step,
			(unsigned)g_trace[i].cp, g_trace[i].label, (int)g_trace[i].retval);
		WriteDebugLogRaw(buf);
	}
	g_trace_n = 0;
}

void ms_test_cp(u32 cp, u32 retval, const char *label)
{
	char buf[80];
	int fail;
	if (g_menu_quiet) return;          // menu open: no per-checkpoint MS write/pause
	// Feed the MS-burst trace buffer. Skip when UART is on: this function already
	// emits the CP line live to UART below, and trace_record would ALSO emit it
	// live on the UART path -> a duplicate wire line. (The IOBUF-flush records at
	// CP60/62/63 call trace_record directly, not through here, so they still reach
	// the live UART trace.)
	if (!DBG_UART()) trace_record(cp, retval, label);

	// Routed by the Debug setting (see pspfatsave.h): bit0 = MS log, bit1 =
	// screen. FAILURE checkpoints still get a chance to reach UART below even
	// with MS/Screen off (rare, and a live UART capture needs the last reached
	// step) — but the MS file itself stays gated on DBG_MS(), no exceptions
	// (ms_write_line() enforces this regardless of what calls it).
	fail = label_has_fail(label);
	// UART is an independent sink: emit if UART logging is on, even when MS and
	// Screen are both off.
	if (!DBG_MS() && !DBG_SCR() && !DBG_UART() && !fail) return;
	sprintf(buf, "CP%u %s ret=%d", cp, label, (int)retval);

	if (DBG_SCR()) {
		dbg_col = 0; dbg_row = ms_test_row;
		ms_test_row = (ms_test_row < 33) ? ms_test_row + 1 : 0;
		// Green = expected result/information, red = unexpected.
		dbg_fg = fail ? 0xFF0000FF : 0xFF00FF00;
		dbg_bg = 0xFF000000;
		dbg_print_all(buf);
	}

	// Crash-safe logging: write EACH checkpoint straight to the MS log
	// immediately (open/append/close per line) — plain sceIo* works under
	// the partial freeze used here, and with DBG_MS() on, a post-power-cycle
	// read of the file shows exactly the last reached step. WriteDebugLogRaw
	// also mirrors to UART (when DBG_UART) — so only emit UART separately
	// when the raw path is NOT taken, to avoid a double UART line. (When MS
	// is off, the "fail" branch below still calls WriteDebugLogRaw so a
	// failure reaches UART if that's on; ms_write_line() itself is what
	// keeps the MS file untouched while DBG_MS() is off, not this check.)
	if (DBG_MS() || fail)
		WriteDebugLogRaw(buf);          // MS write only if DBG_MS() (+ UART if on)
	else if (DBG_UART())
		uart_puts(buf);                 // UART-only checkpoint (MS off, not a failure)
}
// Per-op profile: volatile lock word (0x880E59C0) + DMAC channel-enable (in-flight
// game DMA). Both are plain MMIO reads that can NEVER fault — safe pre-freeze,
// frozen, and post-resume.
// NOTE: the volatile-region CONTENT fingerprint (CPU-reading 0x88400000) was REMOVED
// — it CRASHED a Pirates save (v485): a CPU read of the volatile region while the
// DDR-MPU has it locked faults (kernel exception), and the lock word is NOT a reliable
// proxy for MPU state, so there is no safe pre-check. vlock already tells us the lock
// state (the key GTA-vs-others signal); the content hash was only a nice-to-have.
// (with_vcrc kept as a no-op parameter so call sites/macros don't churn.)
void diag_profile_ex(const char *phase, int with_vcrc)
{
	u32 en0, en1, lockw;
	char b[80];
	(void)with_vcrc;
	if (!DBG_UART()) return;
	lockw = *(volatile u32 *)0x880E59C0;
	en0   = *(volatile u32 *)(0xBC900000 + 0x1C);
	en1   = *(volatile u32 *)(0xBCA00000 + 0x1C);
	sprintf(b, "[DIAG:%s] vlock=0x%X dmac=0x%X", phase, (unsigned)lockw, (unsigned)(en0 | en1));
	uart_puts(b);
}

// Dump every game thread's name/status/waitType/waitId over UART — the per-op
// thread profile for comparing a failing game's thread set + wait states against
// GTA's. UART-only, register reads (sceKernelReferThreadStatus is safe frozen).
void diag_threads(const char *phase, const SceUID *tids, int tcount)
{
	int t;
	if (!DBG_UART()) return;
	for (t = 0; t < tcount; t++) {
		SceKernelThreadInfo ti;
		char b[112];
		memset(&ti, 0, sizeof(ti)); ti.size = sizeof(ti);
		if (sceKernelReferThreadStatus(tids[t], &ti) < 0)
			sprintf(b, "[THR:%s] tid%d refer-FAILED", phase, t);
		else
			// (A frozen ME-RPC mutex holder is NOT identifiable from this dump —
			// it can be suspended READY inside the RPC with wt=0 wid=0, observed
			// on Duodecim. me_rpc_probe reports the owner directly instead.)
			sprintf(b, "[THR:%s] %.14s st=0x%X wt=0x%X wid=0x%X pri=%d",
			        phase, ti.name, (unsigned)ti.status, (unsigned)ti.waitType,
			        (unsigned)ti.waitId, ti.currentPriority);
		uart_puts(b);
	}
}
#endif // DEBUG_BUILD
