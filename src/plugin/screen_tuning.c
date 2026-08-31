// ── IPS screen tuning: midtone gamma + colour temperature (screen_tuning.c) ──
// ONE GE pass applies both corrections to every presented frame:
//
//   out = C · [ Cd + t·(Cd - Cd²) ]
//
// t is SIGNED, derived from the displayed gamma value g_st_gamma (= gamma·100,
// 100 = 1.00 = off):  gamma > 1.0 -> t = +(gamma-1)   (brighten; t=+1 -> 2Cd-Cd²)
//                     gamma < 1.0 -> t = 2·(gamma-1)  (darken;   t=-1 -> out = Cd²)
// C is the per-channel colour-temperature gain from g_st_temp (100 = neutral).
//
// Mechanism: self-texture read with MODULATE blend. The framebuffer is drawn
// over itself as a texture, blend ADD. Brighten (t >= 0), src=ONE_MINUS_DST_COLOR,
// dst=FIX:  out = Cs·(1-Cd) + Cd·B, Cs = Cd·V, V = t·C, B = C.
// Darken (t < 0) mirrors it, src=OTHER_COLOR (=Cd), dst=FIX:
//   out = Cs·Cd + Cd·B, Cs = Cd·V, V = |t|·C, B = (1-|t|)·C  ->  C·[Cd + t·(Cd-Cd²)]
// (vertex colours are unsigned, so the negative t rides in V=|t|·C and the
// remaining (1-|t|) moves into FIXB — at t = -1 that is FIXB = 0, i.e. out=Cd².)
// The Cd² term requires the texture read. At t = 0 (temperature only) the
// texture fetch is skipped.
//
// The list is enqueued through the real sceGeListEnQueue with driver context
// save/restore, after the game's scene list and before its composite. Which
// submit is which comes from a parity latch (g_par_scene_next), anchored by an
// FBP read whenever the submit arrives in thread context.
//
// Present-less path: games that never call sceDisplaySetFrameBuf are handled by
// a worker thread that blocks on vblank and corrects the game's render target
// in place. A utility-dialog gate stands the pass down while savedata/message
// dialogs are open.

#include <pspsdk.h>
#include <pspintrman.h>   /* PSP_VBLANK_INT + sub-interrupt handler API (vblank flip) */
#include "screen_tuning.h"
#include "menu.h"      /* st_hud_draw + save_settings — the live HUD (see g_st_hud) */
#include "sysstats.h"  /* fps_wait_vblank_real — display cadence for present-less games */

#include <pspiofilemgr.h>   /* sceIo* for the per-game screen.cfg */

extern char umdid[24];   /* disc id + "_" + 8-hex ISO hash (main.c) — per-game key */

// uart_puts blocks ~5.5ms, so gate it behind DBG_UART().
#define ST_UART(s) do { if (DBG_UART()) uart_puts(s); } while(0)

// 1 Hz detailed UART stats (the [ST] stats line + the POPS [POPS]/[POPSDB]
// report). OFF by default: each [ST] line is ~245 chars of busy-polled UART
// (~21 ms/s of blocked worker at 115200 baud) plus a ~40-arg sprintf, and on
// POPS st_pops_report() adds several more lines on the same cadence — a
// measurable in-game slowdown. Flip to 1 only while tuning the pass.
#define ST_DETAILED_UART_STATS 1

// ── GE command ids (uofw ge_user.h / pspsdk guInternal.h GECommand) ──
#define GC_VADDR        0x01
#define GC_PRIM         0x04
#define GC_END          0x0C
#define GC_FINISH       0x0F
#define GC_BASE         0x10
#define GC_VTYPE        0x12
#define GC_REGION1      0x15
#define GC_REGION2      0x16
#define GC_OFFSETX      0x4C   /* pspsdk sceGuOffset: sendCommandi(76/77, v << 4) */
#define GC_OFFSETY      0x4D
#define GC_LIGHTING_EN  0x17
#define GC_CULL_EN      0x1D
#define GC_TEX_EN       0x1E
#define GC_FOG_EN       0x1F
#define GC_DITHER_EN    0x20
#define GC_BLEND_EN     0x21
#define GC_ATEST_EN     0x22
#define GC_ZTEST_EN     0x23
#define GC_STENCIL_EN   0x24
#define GC_AA_EN        0x25
#define GC_CTEST_EN     0x27
#define GC_LOGICOP_EN   0x28
#define GC_FBP          0x9C
#define GC_FBW          0x9D
#define GC_TBP0         0xA0
#define GC_TBW0         0xA8
#define GC_TSIZE0       0xB8
#define GC_TMAP         0xC0
#define GC_TMODE        0xC2
#define GC_TPF          0xC3
#define GC_TFILTER      0xC6
#define GC_TWRAP        0xC7
#define GC_TFUNC        0xC9
#define GC_TFLUSH       0xCB
#define GC_TSYNC        0xCC
#define GC_FPF          0xD2
#define GC_CLEARMODE    0xD3
#define GC_SCISSOR1     0xD4
#define GC_SCISSOR2     0xD5
#define GC_BLEND        0xDF
#define GC_FIXA         0xE0
#define GC_FIXB         0xE1
#define GC_DITH0        0xE2
#define GC_DITH1        0xE3
#define GC_DITH2        0xE4
#define GC_DITH3        0xE5
#define GC_ZMSK         0xE7
#define GC_PMSKC        0xE8
#define GC_PMSKA        0xE9

#define GE_OP(cmd, arg) ((((u32)(cmd)) << 24) | ((arg) & 0xFFFFFF))

// Minimum spacing between applies to the same buffer: a game can present the
// same buffer twice within one frame (thread + interrupt). Also bounds the poll
// fallback's apply rate.
#define ST_REPRESENT_FLOOR_US 12000
// Maximum age of a deferred (interrupt-context) present before the worker
// discards it: a present older than one frame (~33ms) describes a buffer that
// has flipped since, so applying it would correct the wrong buffer.
#define ST_DEFER_MAX_US 33000
// With no present through the hook for this long, the poll fallback arms (the
// "SetFrameBuf has gone quiet" threshold).
#define ST_POLL_QUIET_US      100000

// ── Present-less path waits for the game's frame to leave the GE ─────────────
// 40 x 200us = 8ms bound; a stuck GE costs one skipped tick.
#define ST_GE_WAIT_SPINS 40
#define ST_GE_WAIT_US    200
// ── The present-less path must see the absence SUSTAINED ─────────────────────
// `quiet` alone (no present) does not separate "never presents" from a pause;
// the path arms only after this many consecutive vblanks with NO present (a
// present resets the count).
#define ST_POLL_ARM_TICKS 8

// ── Hard floor on the present-less tick period ───────────────────────────────
// After the display is gone (game exit) sceDisplayWaitVblankStart returns
// immediately, so the loop would spin; the floor bounds that. Applied only to
// the present-less branch — the event-flag branch is woken by real presents.
#define ST_TICK_FLOOR_US 8000

// ── Tiling and draw order ────────────────────────────────────────────────────
// The coverage region is drawn as ST_STRIPS full-height strips, emitted LEFT TO
// RIGHT. Strip extents span g_cov_w x g_cov_h — the source region the game's
// composite reads, which is larger than the display when it scales. Only the
// strip count is fixed here.
#define ST_STRIPS   15
#define ST_VERTS    (ST_STRIPS * 2)   /* 30 */

// Phase knob for the present-less path: delay between the game's frame leaving
// the GE and our submit. 0 = submit as soon as the frame is done.
#define ST_PHASE_OFFSET_US 0

// ── State — ALL runtime-initialized in st_init(): kernel PRX .bss is not
// zeroed, and GCC folds explicit-zero statics into .bss (sysstats.c's note). ──
volatile u32 g_st_ge_seq;
volatile int g_st_worker_started;
int g_st_temp = 100;      // 100 = neutral, <100 warmer, >100 cooler
// Set by st_stop() at game exit: the worker ends itself and every submit path
// refuses, so nothing more reaches the GE.
static volatile int g_quit;
static SceUID g_evf;
static volatile int g_busy;               // single-flight: hook + worker may race
static u32 g_last_fb, g_last_seq, g_last_apply_us;
static volatile u32 g_last_present_us;    // last hook-path present (poll fallback arms when stale)
// A present made through the hook at least once. Used by st_pipeline_up().
static volatile int g_seen_present;
static int (*g_ge_enqueue)(const void *, void *, int, void *);
static int (*g_ge_sync)(int qid, int mode);
static int (*g_ge_getcmd)(u32 cmdOff);   // reads a GE hardware register
// Settings the vertex colours + FIXB were built for (-1 = never built).
static int g_vtx_gamma, g_vtx_temp;
// ...and the coverage those strips were built for — part of the same cache key.
static int g_vtx_cw, g_vtx_ch;
// Colour-temperature gain C, packed for FIXB (ABGR: R lo, G mid, B hi).
static u32 g_temp_fix;
// Darken-side FIXB: B = (1-|t|)·C, packed the same way (all zero at t = -1).
static u32 g_dark_fix;
// Interrupt-context present, handed to the worker.
static void * volatile g_pend_addr;
static volatile int g_pend_bufw, g_pend_pfmt;
static volatile u32 g_pend_time;
// ── Utility-dialog gate ──────────────────────────────────────────────────────
// The pass stands down while a system utility dialog (savedata / message) is up:
// a dialog is a second GE client that draws its own screen over the game's.
// Hooked on the InitStart/ShutdownStart syscall pairs.
static volatile int g_dlg_open;
static volatile u32 g_dlg_us;    // time of the 0->1 transition (staleness expiry)
static volatile int g_dlg_evt;   // pending trace event for the worker (see st_worker)
// Staleness bound on the gate: after this long the gate stops applying.
#define ST_DIALOG_MAX_US 60000000
// The close arms a cooldown needing BOTH wall-clock time AND ST_MIN_GE_SEQ of
// the game's own GE submits before the gate lifts.
#define ST_DIALOG_COOL_US 1000000
static volatile int g_dlg_cool;      // cooldown armed by the last dialog close
static volatile u32 g_dlg_cool_us;   // wall-clock end of that cooldown
static volatile u32 g_dlg_cool_seq;  // g_st_ge_seq sampled at the close
static int (*g_real_sd_init)(void *);
static int (*g_real_sd_shut)(void);
static int (*g_real_md_init)(void *);
static int (*g_real_md_shut)(void);

// ── Stats ────────────────────────────────────────────────────────────────────
// One array, one enum. The REJ_* block is contiguous and printed as rej=a/b/c/...
// in submit-guard order (see st_ge_on_submit).
enum {
	STAT_APPLY, STAT_POLL, STAT_INJECT,
	STAT_SKIP_STATIC, STAT_SKIP_REP, STAT_SKIP_EARLY, STAT_SKIP_GEBUSY,
	STAT_SKIP_DLG, STAT_BUSY, STAT_ERR, STAT_MAX_US, STAT_GTGT,
	STAT_POLL_NQUI,     /* presents flowing — the hook owns the game */
	STAT_POLL_NARM,     /* not armed yet */
	STAT_POLL_NOREND,   /* no new GE work since the last tick */
	STAT_POLL_GEBUSY,   /* frame still on the GE after the drain wait */
	STAT_REJ_ARM,       /* injection not armed — worker never handed over */
	STAT_REJ_OFF,       /* menu open (fully-off returns earlier, uncounted) */
	STAT_REJ_BUSY,      /* a submit already in flight */
	STAT_REJ_INTR,      /* interrupt context before any FBP anchor — nothing to extrapolate from */
	STAT_REJ_DISP,      /* displayed buffer not published yet */
	STAT_REJ_READ,      /* GE register read failed validation */
	STAT_REJ_FLOOR,     /* inside the once-per-frame floor */
	STAT_REJ_PAR,       /* the latch says composite — not our submit */
	/* st_apply's entry guards, split out of one folded condition. One slot each,
	 * printed as ap=a/b/c/d. */
	STAT_AP_INACT,      /* gamma 1.0 and temp 100 — nothing to apply */
	STAT_AP_MENU,       /* menu owns the screen */
	STAT_AP_ARG,        /* topaddr/bufw/pfmt outside the accepted range */
	STAT_AP_RES,        /* sceGe entry points not resolved */
	/* The deferred hand-off — what the worker actually picked up. */
	STAT_DWAKE,         /* worker woke on the present event flag */
	STAT_DNULL,         /* ...but the stashed address was NULL */
	STAT_PFLIP,         /* FBP anchor disagreed with the latch (alternation broke) */
	STAT_INJ_INTR,      /* injections enqueued from interrupt context */
	STAT_IIDLE,         /* GE reported idle at an intr injection */
	STAT_N
};
static u32 g_stat[STAT_N];
static u32 g_inject_total;   // running total, for one-shot triggers (not reset)
static u32 g_last_err;       // last enqueue error, kept for display
static u32 g_stat_log_us;    // wall clock of the last stats line

// ── Deterministic injection ahead of the game's composite ────────────────────
// A double-buffered game submits a SCENE list into the offscreen buffer, then a
// COMPOSITE list into the displayed buffer. FBP at submit time names the buffer
// the PREVIOUS list left, so at the SCENE submit it still names the DISPLAYED
// buffer — which identifies the submit without hardcoding either address. That
// reading is only available in thread context, so it serves as the latch's
// anchor; see g_par_scene_next below.
//
// Injecting at the scene submit puts our correction after the scene and before
// the composite, so the composite copies already-corrected pixels.
static volatile int g_inject_arm;    // present-less path is live (worker sets it)
static volatile u32 g_disp_fb;       // displayed buffer, cached by the worker
// The offscreen buffer the game renders its scene into, cached by the worker.
// The injection cannot read it at the scene submit (FBP still names the
// displayed buffer there), so the address must come from an earlier observation.
static volatile u32 g_offscreen_fb;
static volatile int g_offscreen_bw, g_offscreen_pf;
static volatile int g_inject_pending;   // set by the pre-hook, consumed by the post-hook
static u32 g_inject_last_us;         // once-per-frame guard
// ── Parity latch: which of the two per-frame submits this one is ─────────────
// sceGeGetCmd cannot be called from interrupt context (it toggles the A-bus
// clock), yet under load a game can submit both lists from interrupt context.
// So the role is tracked instead of read: the two submits strictly alternate,
// g_par_scene_next holds the expected role of the NEXT one, and every submit
// toggles it. A thread-context submit still reads FBP and re-anchors the latch;
// STAT_PFLIP counts anchor-vs-latch disagreements.
static volatile int g_par_scene_next;   // 1 = expect the next submit to be the scene
static volatile int g_par_valid;        // an FBP anchor has been taken at least once
// Minimum spacing between injections whose role was GUESSED by the latch (an
// interrupt-context submit, nothing verified it).
#define ST_INJECT_FLOOR_US 20000
// Floor for an FBP-VERIFIED submit (thread context). See the floor comment in
// st_ge_on_submit.
#define ST_VERIFIED_FLOOR_US 5000

// ── How much of the offscreen buffer the composite actually reads ────────────
// Sized from the GE drawing region (st_cov_from_buffer). The composite maps a
// source region onto the 480x272 display; correcting only 480x272 of the source
// leaves an uncorrected right edge and bottom band when the composite scales.
#define ST_COV_MAX_W 1024
#define ST_COV_MAX_H 1023   /* REGION/SCISSOR encode 10 bits per axis */
static int g_cov_w = 480, g_cov_h = 272;   /* identity until the offscreen stride is captured */
// Consecutive no-present vblanks; the arming counter.
static u32 g_poll_arm_ticks;
// Arming sweeps spent looking for the game's offscreen render target (see the
// sweep in st_worker). Bounded: a game that composites straight into the
// displayed buffer has no separate target to find.
#define ST_ARM_SWEEP_MAX 60
static u32 g_arm_sweeps;
// ── Arming is PROVISIONAL until the injection actually fires ─────────────────
// The injection identifies the scene submit by FBP still naming the DISPLAYED
// buffer; a game that renders into a back buffer and flips never satisfies that,
// so nothing is ever injected. Arming must therefore prove itself: if the game
// submits this many of its own lists after arming with nothing injected, the
// injection does not fit this game and the poll path takes it back.
#define ST_INJECT_PROVE_SEQ 16
static u32 g_arm_seq;        // g_st_ge_seq sampled when the injection armed
static int g_inject_unfit;   // proven unfit for this game — stay on the poll path
// The game must submit this many of its own GE lists before we hand the GE one
// of ours (until then it is not really rendering yet).
#define ST_MIN_GE_SEQ 8
// Startup trace, capped (uart_puts blocks ~5.5ms per line and some run on the
// game's own thread).
#define ST_TRACE_N 3
static int g_trace_sub, g_trace_poll, g_trace_arg;
// GE-submit count sampled at the previous fallback tick. The fallback only fires
// when this ADVANCES: a rendering-but-not-presenting game keeps submitting
// lists; a loading screen submits nothing.
static u32 g_poll_last_seq;

// ── Power (suspend/resume) coordination ──────────────────────────────────────
// The pass MUST NOT touch the GE/scanout across a native suspend/resume, and on
// resume it must re-acquire fresh state instead of riding the stale restored FBP
// register. st_pwr_suspend() stands the pass down; st_pwr_resume() arms a DELAYED
// re-enable so the game is shown uncorrected first and the correction snaps in
// only on a real, re-latched frame.
#define ST_RESUME_DELAY_US   150000    // show the game this long before re-applying
#define ST_RESUME_MIN_SEQ    6         // only once it has drawn this many frames
#define ST_SUSP_DRAIN_SPINS  8         // in-flight-submit drain in the suspend handler
#define ST_SUSP_DRAIN_US     1000      // (yields to the owning thread; usually 0 iters)
// After re-acquire, the present-hook applies stay NON-BLOCKING (wait=0) for this
// long while the GE re-settles after a resume. wait=0 enqueues and returns.
#define ST_RESUME_GRACE_US   500000    // non-blocking settle window
static volatile int g_st_suspended;       // 1 = stood down for a firmware suspend
static volatile int g_st_resume_pending;  // resume seen; waiting to re-acquire (show game first)
static u32 g_st_resume_at_us;             // earliest wall-clock to re-acquire
static u32 g_st_resume_anchor_seq;        // g_st_ge_seq at resume; +MIN_SEQ before re-acquire
static int g_st_susp_logged;              // one-shot "stood down" log per cycle (worker)
static int g_st_uart_revived;             // one-shot: UART re-init'd after this resume
static volatile int g_st_susp_drain_busy; // debug: g_busy still set after the suspend drain
static volatile int g_st_pwr_susp_ran;    // debug: st_pwr_suspend() actually ran this cycle
static u32 g_st_pwr_cycles;               // debug: suspend/resume cycles handled
static u32 g_st_grace_until_us;           // wait=0 grace deadline (0 = no grace)

int g_is_pops;   // set by main.c's runlevel gate (see screen_tuning.h)

static int st_read_game_target(u32 *fb, int *bufw, int *pfmt);   // defined below

// ── The display controller: what is ACTUALLY being scanned out ───────────────
// sceDisplayGetFrameBuf only reports what the user-mode API last set. The LCDC
// registers report the actual scanout regardless of which path set it, and
// reading them is pure MMIO — safe from any context.
//
// Layout from ARK iplsdk libs/iplsdk/dmacplus.c (DMACPLUS_LCDC_MMIO_BASE):
//   +0x00 framebuffer address, stored as a PHYSICAL address (addr & 0x1FFFFFFF)
//   +0x04 pixel format          +0x08 width
//   +0x0C stride (multiple of 64)
//   +0x10 config, bit0 = scanout enabled
#define ST_LCDC_BASE  0xBC800100

// The hardware format enum is NOT the GE one. dmacplus.h has 0=RGBA8888,
// 1=RGB565, 2=RGBA5551, 3=RGBA4444; the GE (and our list builder) uses 0=565,
// 1=5551, 2=4444, 3=8888. The raw value must be mapped.
static int st_lcdc_to_ge_pfmt(u32 f)
{
	switch (f & 3) {
	case 0:  return 3;   /* 8888 */
	case 1:  return 0;   /* 565  */
	case 2:  return 1;   /* 5551 */
	default: return 2;   /* 4444 */
	}
}

// Returns 1 when the controller is scanning a plausible VRAM framebuffer.
static int st_lcdc_read(u32 *addr, int *width, int *stride, int *ge_pfmt, int *enabled)
{
	volatile u32 *r = (volatile u32 *)ST_LCDC_BASE;
	u32 a = r[0], f = r[1], w = r[2], st = r[3], cfg = r[4];

	*enabled = (int)(cfg & 1);
	*width   = (int)w;
	*stride  = (int)st;
	*ge_pfmt = st_lcdc_to_ge_pfmt(f);
	*addr    = a;
	// MAIN RAM is a valid scanout source, not just VRAM.
	return st >= 64 && st <= 1024 &&
	       (((a & 0xFF000000) == 0x04000000 && a < 0x04200000) ||   /* VRAM  */
	        (a >= 0x08800000 && a <  0x0A000000));                  /* RAM   */
}

// ── POPS observation census (writes NOTHING to the GE) ──────────────────────
// FBP under POPS can name emulated PS1 VRAM rather than the displayed picture.
// This census records which GE render targets appear and which match the LCDC
// scanout — read-only, so it cannot corrupt POPS.
static u32 g_pops_seen[6];        // distinct GE render targets
static int g_pops_bw[6], g_pops_pf[6];
static int g_pops_nseen;
static u32 g_pops_hits;           // submits whose target IS the scanned-out buffer
static u32 g_pops_subs;           // submits seen

// ── POPS: the correction runs from the FRAME signal instead of the worker.
// POPS flips through the kernel sceDisplaySetFrameBufferInternal export, which a
// syscall patch cannot see, so ARK's hijack of that function is used instead: the
// HOOK runs before the real function, when the frame is complete and the flip has
// not yet happened.
#define ST_POPS_FLIP_NID 0xA38B3F89

// Davee's trampoline, as used by ARK's macros.h HIJACK_FUNCTION: copy the first
// two instructions aside, jump back to target+8 after them, and overwrite the
// entry with a jump to the hook. _lw/_sw live in ARK's kernel lib, which this
// plugin does not link, so they are spelled out here.
#define ST_LW(a)      (*(volatile u32 *)(a))
#define ST_SW(v, a)   (*(volatile u32 *)(a) = (u32)(v))

// Four args, matching ARK's call: (topaddr, bufferwidth, pixelformat, sync).
static int (*g_real_flip)(void *, int, int, int);
static u32 g_flip_tramp[5];
static u32 g_flip_calls;

// ── Sample the scanout address at tick rate ─────────────────────────────────
// st_lcdc_read returns the buffer being DISPLAYED at each tick; this records how
// many distinct buffers appear and whether the address changes between ticks.
#define ST_LA_SEEN 4
static u32 g_la_seen[ST_LA_SEEN];
static int g_la_nseen;
static u32 g_la_changes;   // how often it changed between consecutive ticks
static u32 g_la_last;

// Signed strength in percent, derived from the DISPLAYED gamma·100 (g_st_gamma,
// 50..200, 100 = 1.00 = off). Brighten (gamma > 1.0): t = gamma-1, so 2.00 -> +100
// (out = 2Cd - Cd²). Darken (gamma < 1.0): t = 2·(gamma-1), so 0.50 -> -100
// (out = Cd²) — the two extremes of the one curve family.
static int st_gamma_t100(int g100)
{
	return (g100 > 100) ? (g100 - 100) : 2 * (g100 - 100);
}

// Per-channel curve tables, pre-shifted into place so the inner loop is three
// lookups and two ORs. Rebuilt when the settings or the pixel format change.
static u32 g_lut[3][256];
static int g_lut_gamma = -1, g_lut_temp = -1, g_lut_pf = -1;

// PSP pixel layouts are RGBA with R in the LOW bits.
//   565  R:0-4 G:5-10 B:11-15           5551 R:0-4 G:5-9  B:10-14 A:15
//   4444 R:0-3 G:4-7  B:8-11  A:12-15   8888 R:0-7 G:8-15 B:16-23 A:24-31
static void st_lut_fmt(int pf, int *bits, int *shift, u32 *keep)
{
	switch (pf) {
	case 0:  bits[0]=5; bits[1]=6; bits[2]=5; shift[0]=0; shift[1]=5; shift[2]=11; *keep=0; break;
	case 1:  bits[0]=5; bits[1]=5; bits[2]=5; shift[0]=0; shift[1]=5; shift[2]=10; *keep=0x8000; break;
	case 2:  bits[0]=4; bits[1]=4; bits[2]=4; shift[0]=0; shift[1]=4; shift[2]=8;  *keep=0xF000; break;
	default: bits[0]=8; bits[1]=8; bits[2]=8; shift[0]=0; shift[1]=8; shift[2]=16; *keep=0xFF000000u; break;
	}
}

static void st_build_lut(int gamma, int temp, int pf)
{
	int bits[3], shift[3], gain[3], c, i;
	u32 keep;
	int d = temp - 100;
	int t100 = st_gamma_t100(gamma);

	// Same channel gains the GE path puts in FIXB (see st_build_vertices).
	gain[0] = 255; gain[1] = 255; gain[2] = 255;
	if (d < 0)      { gain[2] = 255 + d * 64 / 100; gain[1] = 255 + d * 12 / 100; }
	else if (d > 0) { gain[0] = 255 - d * 64 / 100; gain[1] = 255 - d * 12 / 100; }
	for (c = 0; c < 3; c++) if (gain[c] < 0) gain[c] = 0;

	st_lut_fmt(pf, bits, shift, &keep);
	for (c = 0; c < 3; c++) {
		int max = (1 << bits[c]) - 1;
		for (i = 0; i <= max; i++) {
			// out = C * (v + t*(v - v^2)), t signed (st_gamma_t100/100), computed
			// on the channel's own range so it can never leave it.
			int v    = i;
			int sq   = (v * v + max / 2) / max;
			int lift = ((v - sq) * t100) / 100;
			int o    = v + lift;
			o = (o * gain[c] + 127) / 255;
			if (o < 0)   o = 0;
			if (o > max) o = max;
			g_lut[c][i] = (u32)o << shift[c];
		}
	}
	g_lut_gamma = gamma; g_lut_temp = temp; g_lut_pf = pf;
}

// Out-of-place curve: read POPS's buffer (src), write the corrected pixels into
// a DIFFERENT buffer (dst). Per-row cache discipline: invalidate the src row
// before reading, write the dst row back after. Both src and dst are cached
// aliases.
static void st_pops_curve_copy(u32 dst, u32 src, int stride, int pf, int w)
{
	int bits[3], shift[3], x, y;
	u32 keep;

	if (g_lut_gamma != g_st_gamma || g_lut_temp != g_st_temp || g_lut_pf != pf)
		st_build_lut(g_st_gamma, g_st_temp, pf);
	st_lut_fmt(pf, bits, shift, &keep);

	if (pf == 3) {
		u32 *s = (u32 *)src, *d = (u32 *)dst;
		u32 rb = (u32)w * 4u;
		for (y = 0; y < 272; y++, s += stride, d += stride) {
			sceKernelDcacheWritebackInvalidateRange((void *)s, rb);
			for (x = 0; x < w; x++) {
				u32 v = s[x];
				d[x] = g_lut[0][v & 0xFF] | g_lut[1][(v >> 8) & 0xFF] |
				       g_lut[2][(v >> 16) & 0xFF] | (v & keep);
			}
			sceKernelDcacheWritebackRange((void *)d, rb);
		}
	} else {
		u16 *s = (u16 *)src, *d = (u16 *)dst;
		u32 rb = (u32)w * 2u;
		u32 m0 = (1u << bits[0]) - 1, m1 = (1u << bits[1]) - 1, m2 = (1u << bits[2]) - 1;
		for (y = 0; y < 272; y++, s += stride, d += stride) {
			sceKernelDcacheWritebackInvalidateRange((void *)s, rb);
			for (x = 0; x < w; x++) {
				u32 v = s[x];
				d[x] = (u16)(g_lut[0][v & m0] | g_lut[1][(v >> shift[1]) & m1] |
				             g_lut[2][(v >> shift[2]) & m2] | (v & keep));
			}
			sceKernelDcacheWritebackRange((void *)d, rb);
		}
	}
}

// ── POPS shadow buffer (see st_pops_tick) ────────────────────────────────────
#define ST_SHADOW_FB  0x04100000u
static u32 g_pops_src;            // POPS's own display buffer (its blit target)
static int g_pops_sbw, g_pops_spf;
static int g_pops_redirected;     // scanout currently pointed at the shadow
// The shadow region must be PROVEN free before anything is written to it: it is
// stamped with a pattern and re-checked for ST_SHADOW_PROVE ticks; anything
// overwriting it means POPS owns that memory and the shadow stays off.
#define ST_SHADOW_PROVE 90           // ~1.5s of vblank ticks
#define ST_SHADOW_MARK  0x5A3C0F69u
static int g_shadow_state;           // 0 = idle/probing, 3 = redirected/held, 9 = unavailable
static int g_shadow_ticks;
static u32 g_shadow_lost;            // times POPS re-pointed the scanout away
static u32 g_shadow_need;            // bytes per shadow buffer
static u32 g_flip_last_us;           // last flip-hook call time — in-game vs POPS-menu gate
// The shadow lives in EXCLUSIVELY-ALLOCATED RAM, not the VRAM gap; an allocation
// cannot collide with POPS's memory. Two buffers for the ping-pong.
#define ST_DB_PARTITION 1            // kernel partition
#define ST_DB_ALIGN     0x4000u      // 16KB
static SceUID g_shadow_memid = -1;
static u32 g_shadow_base;            // aligned base of the allocated block (both buffers)
// While POPS is FLIPPING (its own UI/menu), the flip hook corrects in place, so
// the worker must NOT also redirect. Engage the redirect only once flips have
// been absent this long (i.e. in-game, where POPS never calls SetFrameBuf).
#define ST_DB_IDLE_US   500000u
#define ST_DB_HOLD      120          // stage-1 freeze self-test hold (~2s of vblanks)
#define ST_DB_STABLE    30           // display must hold one config this long first (~0.5s)
#define ST_DB_VBL_SUBNO 0            // vblank sub-interrupt slot, 0 = fire LAST in
                                      // descending dispatch, after firmware display handler
static int g_db_vbl_on;              // vblank flip handler installed
static int g_in_pops_menu;           // POPS menu active — flip hook corrects in place,
                                      // ISR must NOT redirect scanout over it
static u32 g_db_last_a; static int g_db_last_s, g_db_last_p, g_db_stable;
static u32 g_shadow_buf[2];          // the two ping-pong buffers (cached virtual addrs)
static int g_db_front;               // buffer index currently scanned out (-1 = none yet)
static int g_db_ready;               // buffer filled last tick, to show at the next vblank
static u32 g_db_copies;              // corrected frames written this second (per-sec rate)
static int g_db_w;                   // visible width of POPS's buffer (latched at stability)

static void st_pops_observe(void)
{
	u32 fb; int bw, pf, i;
	u32 la; int lw, ls, lp, len;

	// FBP comes from sceGeGetCmd, which toggles the A-bus clock — thread only.
	if (sceKernelIsIntrContext()) return;
	if (!st_read_game_target(&fb, &bw, &pf)) return;
	g_pops_subs++;
	if (st_lcdc_read(&la, &lw, &ls, &lp, &len) && fb == la) g_pops_hits++;

	for (i = 0; i < g_pops_nseen; i++) if (g_pops_seen[i] == fb) return;
	if (g_pops_nseen < 6) {
		g_pops_seen[g_pops_nseen] = fb;
		g_pops_bw[g_pops_nseen]   = bw;
		g_pops_pf[g_pops_nseen]   = pf;
		g_pops_nseen++;
	}
}

// Emitted once a second from the worker alongside the [ST] line.
#if ST_DETAILED_UART_STATS
static void st_pops_report(void)
{
	char b[240];
	u32 la; int lw, ls, lp, len, ok, i, n;

	ok = st_lcdc_read(&la, &lw, &ls, &lp, &len);
	// Census (subs/hit/ntgt) + LCDC readback: confirms which buffer POPS is
	// actually scanning out and that this module is reading it correctly.
	sprintf(b, "[POPS] lcdc=%08x w=%d stride=%d gepf=%d ok=%d subs=%u hit=%u ntgt=%d",
	        (unsigned)la, lw, ls, lp, ok,
	        (unsigned)g_pops_subs, (unsigned)g_pops_hits, g_pops_nseen);
	ST_UART(b);
	// Double-buffer engine status (state/copies/lost/front/redir), the flip-hook
	// rate (calls = every SetFrameBuf POPS made), and the scanout-address sample
	// (nla/chg — nla>1 or chg>0 would mean POPS itself flips between buffers).
	sprintf(b, "[POPSDB] st=%d cpy=%u/s lost=%u front=%d redir=%d flips=%u/s nla=%d chg=%u src=%08x bw=%d pf=%d buf0=%08x",
	        g_shadow_state, (unsigned)g_db_copies, (unsigned)g_shadow_lost,
	        g_db_front, g_pops_redirected, (unsigned)g_flip_calls,
	        g_la_nseen, (unsigned)g_la_changes,
	        (unsigned)g_pops_src, g_pops_sbw, g_pops_spf,
	        (unsigned)g_shadow_buf[0]);
	ST_UART(b);
	g_db_copies = 0; g_la_changes = 0; g_flip_calls = 0;
	n = g_pops_nseen;
	for (i = 0; i < n; i++) {
		sprintf(b, "[POPS]  tgt%d=%08x bw=%d pf=%d %s", i,
		        (unsigned)g_pops_seen[i], g_pops_bw[i], g_pops_pf[i],
		        (ok && g_pops_seen[i] == la) ? "<== SCANNED OUT" : "");
		ST_UART(b);
	}
	g_pops_subs = 0; g_pops_hits = 0;
}
#endif // ST_DETAILED_UART_STATS

// Rotating list banks. uofw _sceGeListEnQueue (ge.c:3474) scans the ACTIVE queue
// and refuses a list whose address is already in it, returning SCE_ERROR_BUSY
// (0x80000021). The injection enqueues with wait=0 and never syncs, so a queued
// correction from a previous frame could still be present when the next one is
// built — a rotating bank avoids self-collision.
#define ST_LIST_BANKS 3
// 80 words: the in-place list uses 49, the POPS shadow list ~58 (two draws).
#define ST_LIST_BANK_WORDS 80
static u32 *g_list_bank[ST_LIST_BANKS];
static int s_bank_usr;   // 1 = the user block exists and g_list_bank[] point into it
static u32 g_banks_fail_us;   // alloc-failure backoff timestamp (retry at most once/sec)
static u32 *g_list;      // current bank; rotated in st_submit under g_busy
// Non-zero selects the POPS out-of-place pass in st_submit: copy from this
// source into the destination, then curve the destination. Zero for every other
// path, which stays the in-place self-texture pass.
static u32 g_shadow_src;
static int g_shadow_sbw, g_shadow_spf;
static int g_list_idx;
// Through-mode vertex, GU_COLOR_8888|GU_TEXTURE_16BIT|GU_VERTEX_16BIT:
// [u v][color][x y z], padded to 16 so the u32 color stays 4-aligned per vertex.
typedef struct { short u, v; u32 c; short x, y, z, pad; } StVtx;
// The GE fetches the vertices through VADDR, so g_vtx points into the user block
// (st_banks_ensure).
static StVtx *g_vtx;
// Driver-managed context save/restore target (PspGeContext-sized, 2KB). Also in
// the user block: the FINISH-time ctx restore REPLAYS a list built in this
// buffer through the GE.
static u32 *g_ge_ctx;
// Full 16-byte enqueue-args form: uofw ge.c reads numStacks/stacks when
// size==16; the installed SDK's PspGeListArgs is the older 8-byte {size,ctx}.
struct StGeListArgs { u32 size; void *ctx; u32 numStacks; void *stacks; };

// ── All GE-read buffers (list banks, ctx, vertices) in ONE user-partition ───
// block, allocated once at pass start and used for every submit, before and
// after suspends alike.
static void st_banks_ensure(void)
{
	if (s_bank_usr) return;
	{
		u32 now = sceKernelGetSystemTimeLow();
		// Backoff: retry (and log) at most once per second.
		if ((u32)(now - g_banks_fail_us) < 1000000) return;
		// SDK quirk: sceKernelAllocPartitionMemory's last arg is an ADDRESS HINT
		// (NULL = auto-allocate), NOT an out-param — the block address comes from
		// sceKernelGetBlockHeadAddr(uid).
		SceUID uid = sceKernelAllocPartitionMemory(2 /* PSP_MEMORY_PARTITION_USER */,
		                                           "STbuf", 0 /* PSP_SMEM_Low */,
		                                           (ST_LIST_BANKS * ST_LIST_BANK_WORDS + 512) * 4
		                                             + ST_VERTS * (int)sizeof(StVtx), NULL);
		if (uid >= 0) {
			u32 *blk = (u32 *)sceKernelGetBlockHeadAddr(uid);
			int i;
			for (i = 0; i < ST_LIST_BANKS; i++)
				g_list_bank[i] = blk + i * ST_LIST_BANK_WORDS;
			g_ge_ctx = blk + ST_LIST_BANKS * ST_LIST_BANK_WORDS;        // 16-aligned (240 words in)
			g_vtx = (StVtx *)(blk + ST_LIST_BANKS * ST_LIST_BANK_WORDS + 512);  // 16-aligned
			g_list = g_list_bank[0];
			g_vtx_gamma = -1;   // force st_build_vertices to fill the user buffer
			s_bank_usr = 1;
			ST_UART("[ST] GE buffers -> user (alloc ok)");
		} else {
			char b[64];
			g_banks_fail_us = now;
			sprintf(b, "[ST] user GE-buffer alloc FAILED %08x - pass disabled", (unsigned)uid);
			ST_UART(b);
		}
	}
}

void st_init(void)
{
	int i;
	g_st_gamma = 100;           // gamma·100, 100 = 1.00 = off; per-game load may change it
	g_st_temp = 100;
	g_st_hud = 0;
	g_st_hud_dirty = 0;
	g_st_ge_seq = 0;
	g_st_worker_started = 0;
	g_quit = 0;
	g_evf = -1;
	g_busy = 0;
	g_last_fb = 0; g_last_seq = 0; g_last_apply_us = 0; g_last_present_us = 0;
	g_seen_present = 0;
	g_trace_sub = 0; g_trace_poll = 0; g_trace_arg = 0; g_poll_last_seq = 0;
	g_inject_arm = 0; g_disp_fb = 0; g_inject_last_us = 0;
	g_par_scene_next = 0; g_par_valid = 0;
	g_list_idx = 0;
	g_list_bank[0] = g_list_bank[1] = g_list_bank[2] = NULL;
	g_list = NULL; g_vtx = NULL; g_ge_ctx = NULL;
	s_bank_usr = 0;
	g_offscreen_fb = 0; g_offscreen_bw = 0; g_offscreen_pf = 0; g_inject_pending = 0;
	g_poll_arm_ticks = 0; g_arm_sweeps = 0;
	g_arm_seq = 0; g_inject_unfit = 0;
	g_pops_nseen = 0; g_pops_subs = 0; g_pops_hits = 0;
	g_pops_src = 0; g_pops_sbw = 0; g_pops_spf = 0; g_pops_redirected = 0;
	g_shadow_src = 0; g_shadow_sbw = 0; g_shadow_spf = 0;
	g_shadow_state = 0; g_shadow_ticks = 0;
	g_shadow_lost = 0;
	g_shadow_need = 0; g_shadow_base = 0; g_flip_last_us = 0;
	g_db_last_a = 0; g_db_last_s = 0; g_db_last_p = 0; g_db_stable = 0;
	g_shadow_buf[0] = 0; g_shadow_buf[1] = 0; g_db_copies = 0; g_db_w = 0;
	g_db_front = -1; g_db_ready = -1;
	// g_shadow_memid keeps its load-time -1 (freed+cleared in st_stop); resetting
	// it here could orphan a live block.
	g_la_nseen = 0; g_la_changes = 0; g_la_last = 0;
	g_lut_gamma = -1; g_lut_temp = -1; g_lut_pf = -1;
	// NOT g_real_flip: the hijack patches firmware code in place and outlives a
	// re-init, so clearing it would install a second trampoline over the first.
	g_flip_calls = 0;
	g_dlg_open = 0; g_dlg_us = 0; g_dlg_evt = 0;
	g_dlg_cool = 0; g_dlg_cool_us = 0; g_dlg_cool_seq = 0;
	g_real_sd_init = NULL; g_real_sd_shut = NULL;
	g_real_md_init = NULL; g_real_md_shut = NULL;
	g_ge_enqueue = NULL; g_ge_sync = NULL; g_ge_getcmd = NULL;
	g_vtx_gamma = -1; g_vtx_temp = -1; g_vtx_cw = 0; g_vtx_ch = 0;
	g_temp_fix = 0xFFFFFF; g_dark_fix = 0xFFFFFF;
	g_pend_addr = NULL; g_pend_bufw = 0; g_pend_pfmt = 0; g_pend_time = 0;
	g_inject_total = 0; g_last_err = 0; g_stat_log_us = 0;
	g_st_suspended = 0; g_st_resume_pending = 0; g_st_resume_at_us = 0;
	g_st_resume_anchor_seq = 0; g_st_susp_logged = 0; g_st_uart_revived = 0;
	g_st_susp_drain_busy = 0; g_st_pwr_susp_ran = 0; g_st_pwr_cycles = 0;
	g_st_grace_until_us = 0;
	for (i = 0; i < STAT_N; i++) g_stat[i] = 0;
}

// Vertex strips + the FIXB constants. All pure functions of (gamma, temp), so
// they are built together and cached against the same key.
//
// Brighten (t >= 0): blend out = Cd·V·(1-Cd) + Cd·B, V = t·C, B = C.
// Darken  (t < 0):  blend out = Cd·V·Cd + Cd·B, V = |t|·C, B = (1-|t|)·C —
// vertex colours are unsigned, so the negative t rides in V=|t|·C and the
// remaining (1-|t|) moves into FIXB. At temp 100 (C = white) and t=±1 the pass
// is exactly gamma-only.
static void st_build_vertices(int gamma, int temp, int cw, int ch)
{
	int i, n = 0;
	int r = 255, g = 255, b = 255;
	int d = temp - 100;   // -100..+100
	int t100 = st_gamma_t100(gamma);
	int at100 = t100 < 0 ? -t100 : t100;
	u32 col;

	// Colour temperature: cut blue to warm, cut red to cool, with a smaller
	// counter-move on green.
	if (d < 0) {
		b = 255 + d * 64 / 100;   // d=-100 -> b=191
		g = 255 + d * 12 / 100;   // d=-100 -> g=243
	} else if (d > 0) {
		r = 255 - d * 64 / 100;   // d=+100 -> r=191
		g = 255 - d * 12 / 100;   // d=+100 -> g=243
	}
	if (r < 0) r = 0;
	if (g < 0) g = 0;
	if (b < 0) b = 0;
	g_temp_fix = (u32)r | ((u32)g << 8) | ((u32)b << 16);
	if (t100 < 0) {
		// B = (1-|t|)·C per channel — the darken blend's FIXB (0 at full darken).
		g_dark_fix = ((u32)((100 + t100) * b / 100) << 16)
		           | ((u32)((100 + t100) * g / 100) << 8)
		           |  (u32)((100 + t100) * r / 100);
	} else {
		g_dark_fix = g_temp_fix;   // brighten side: FIXB = C as before
	}
	// V = |t|·C per channel. Alpha unused (writes are masked).
	col = 0xFF000000
	    | ((u32)(at100 * b / 100) << 16)
	    | ((u32)(at100 * g / 100) << 8)
	    |  (u32)(at100 * r / 100);

	// Strips span the coverage region the CALLER asked for, not a fixed 480x272
	// and not a global — the injection corrects the composite's SOURCE, which is
	// larger than the display when the composite scales, while every submit that
	// targets a display buffer must stay inside 480x272. Widths are derived per
	// strip so rounding cannot leave a seam.
	for (i = 0; i < ST_STRIPS; i++) {
		short x0 = (short)(i * cw / ST_STRIPS);
		short x1 = (short)((i + 1) * cw / ST_STRIPS);
		// Sprite: v0 inclusive, v1 exclusive, UV 1:1 with the pixels; flat
		// shading takes the color from the pair's SECOND vertex — set both.
		g_vtx[n].u = x0; g_vtx[n].v = 0;
		g_vtx[n].c = col;
		g_vtx[n].x = x0; g_vtx[n].y = 0; g_vtx[n].z = 0;
		g_vtx[n].pad = 0;
		n++;
		g_vtx[n].u = x1; g_vtx[n].v = (short)ch;
		g_vtx[n].c = col;
		g_vtx[n].x = x1; g_vtx[n].y = (short)ch; g_vtx[n].z = 0;
		g_vtx[n].pad = 0;
		n++;
	}
	sceKernelDcacheWritebackInvalidateRange(g_vtx, sizeof(StVtx) * ST_VERTS);
	g_vtx_gamma = gamma; g_vtx_temp = temp; g_vtx_cw = cw; g_vtx_ch = ch;
}

// Pipeline state both list builders need: geometry source, render target, and
// every stage they rely on being OFF. The game's register state is unknown, so
// nothing may be inherited — the driver's context save/restore puts it all back
// afterwards. Returns the advanced write cursor. Callers set TEX_EN, BLEND_EN
// and DITHER_EN themselves, which is all they disagree about.
static u32 *st_emit_common(u32 *l, u32 vtx, u32 fb, int bufw, int pfmt, int w, int h)
{
	*l++ = GE_OP(GC_BASE, (vtx >> 8) & 0xF0000);
	*l++ = GE_OP(GC_VADDR, vtx);
	// through (1<<23) | color 8888 (7<<2) | vertex s16 (2<<7) | texcoord s16 (2)
	*l++ = GE_OP(GC_VTYPE, 0x80011E);
	*l++ = GE_OP(GC_REGION1, 0);
	*l++ = GE_OP(GC_REGION2, ((h - 1) << 10) | (w - 1));
	*l++ = GE_OP(GC_SCISSOR1, 0);
	*l++ = GE_OP(GC_SCISSOR2, ((h - 1) << 10) | (w - 1));
	// Vertex offset — zero it. The list inherits whatever the game left in every
	// register it does not set; the game renders with a non-zero offset, which
	// would translate our correction out of place.
	*l++ = GE_OP(GC_OFFSETX, 0);
	*l++ = GE_OP(GC_OFFSETY, 0);
	// Render target (high address byte rides in the width word —
	// sceGuDrawBuffer.c encoding). Display pixel formats 0-3 == GE fb formats.
	*l++ = GE_OP(GC_FPF, pfmt);
	*l++ = GE_OP(GC_FBP, fb);
	*l++ = GE_OP(GC_FBW, ((fb & 0xFF000000) >> 8) | (u32)bufw);
	*l++ = GE_OP(GC_LIGHTING_EN, 0);
	*l++ = GE_OP(GC_FOG_EN, 0);
	*l++ = GE_OP(GC_CULL_EN, 0);
	*l++ = GE_OP(GC_AA_EN, 0);
	*l++ = GE_OP(GC_ATEST_EN, 0);
	*l++ = GE_OP(GC_ZTEST_EN, 0);
	*l++ = GE_OP(GC_STENCIL_EN, 0);
	*l++ = GE_OP(GC_CTEST_EN, 0);
	*l++ = GE_OP(GC_LOGICOP_EN, 0);
	*l++ = GE_OP(GC_CLEARMODE, 0);
	// Depth writes masked, alpha writes masked (fb alpha is the game's stencil
	// storage).
	*l++ = GE_OP(GC_ZMSK, 1);
	*l++ = GE_OP(GC_PMSKC, 0);
	*l++ = GE_OP(GC_PMSKA, 0xFF);
	return l;
}

// Rebuilt per apply (framebuffer address/stride/format change per game and per
// flip). Returns the word count (49 of the 64 available).
static int st_build_list(u32 fb, int bufw, int pfmt, int cw, int ch)
{
	u32 *l = g_list;
	u32 vtx = (u32)g_vtx;

	l = st_emit_common(l, vtx, fb, bufw, pfmt, cw, ch);
	// The curve term needs the self-texture read; a temperature-only pass does
	// not (V is 0, so Cs is 0 either way) and skips the fetch entirely.
	*l++ = GE_OP(GC_TEX_EN, g_st_gamma != 100 ? 1 : 0);
	*l++ = GE_OP(GC_BLEND_EN, 1);
	// Dither the 16-bit formats (the curve otherwise bands in gradients after
	// the 5/6-bit quantize); libgu's default bayer matrix, nibble-packed.
	*l++ = GE_OP(GC_DITHER_EN, (pfmt != 3) ? 1 : 0);
	*l++ = GE_OP(GC_DITH0, 0x1D0C);
	*l++ = GE_OP(GC_DITH1, 0xF3E2);
	*l++ = GE_OP(GC_DITH2, 0x0C1D);
	*l++ = GE_OP(GC_DITH3, 0xE2F3);
	// Texture = the same buffer. 512x512 declared size (log2 9,9) over the
	// 480x272 UV range; through-mode UVs are raw texel units, no scale applied.
	*l++ = GE_OP(GC_TMAP, 0);                    // UV mapping
	*l++ = GE_OP(GC_TMODE, 0);                   // no swizzle, 1 level
	*l++ = GE_OP(GC_TPF, pfmt);                  // texture formats 0-3 == display formats
	*l++ = GE_OP(GC_TFILTER, 0);                 // nearest/nearest — exact texel reads
	*l++ = GE_OP(GC_TWRAP, 0x101);               // clamp u+v
	*l++ = GE_OP(GC_TFUNC, 0);                   // MODULATE, RGB: fragment = texel * V
	*l++ = GE_OP(GC_TBP0, fb);
	*l++ = GE_OP(GC_TBW0, ((fb >> 8) & 0x0F0000) | (u32)bufw);
	*l++ = GE_OP(GC_TSIZE0, (9 << 8) | 9);
	*l++ = GE_OP(GC_TFLUSH, 0);                  // last frame's texels may be cached at these addresses
	*l++ = GE_OP(GC_TSYNC, 0);
	// Brighten (t >= 0, gamma > 1.0): out = Cs*(1-Cd) + Cd*FIXB(C), Cs = t*C*Cd
	// Darken (t < 0, gamma < 1.0): src=OTHER_COLOR (=Cd), dst=FIX, op=ADD:
	//   out = Cs*Cd + Cd*FIXB = Cd²·V + Cd·B, V=|t|·C, B=(1-|t|)·C
	if (g_st_gamma < 100) {
		*l++ = GE_OP(GC_BLEND, 0 | (10 << 4) | (0 << 8));   // src=otherColor, dst=FIX, op=ADD
		*l++ = GE_OP(GC_FIXA, 0);
		*l++ = GE_OP(GC_FIXB, g_dark_fix);
	} else {
		*l++ = GE_OP(GC_BLEND, 1 | (10 << 4) | (0 << 8));   // src=1-otherColor, dst=FIX, op=ADD
		*l++ = GE_OP(GC_FIXA, 0);
		*l++ = GE_OP(GC_FIXB, g_temp_fix);
	}
	*l++ = GE_OP(GC_PRIM, (6 << 16) | ST_VERTS);        // sprites
	*l++ = GE_OP(GC_TFLUSH, 0);   // flush OUR write so a following composite (texture read) sees it

	*l++ = GE_OP(GC_FINISH, 0);
	*l++ = GE_OP(GC_END, 0);

	return (int)(l - g_list);
}

// ── Shadow pass: copy src -> dst, then curve dst in place ───────────────────
// For POPS: read POPS's display buffer, write ours, and point the scanout at
// ours. POPS's own buffer is never written.
//
// It needs TWO draws. The blend is out = Cs*(1-Cd) + Cd*B where Cd is the
// DESTINATION pixel, so the curve algebra only holds when the destination
// already holds the source image. Draw 1 is a plain REPLACE copy with blending
// off; draw 2 is the normal in-place curve, reading the shadow as its own
// texture. TFLUSH between them because draw 1's writes are draw 2's texels.
static int st_build_shadow_list(u32 dst, int dbw, int dpf,
                                u32 src, int sbw, int spf, int cw, int ch)
{
	u32 *l = g_list;
	u32 vtx = (u32)g_vtx;

	l = st_emit_common(l, vtx, dst, dbw, dpf, cw, ch);

	// ── draw 1: straight copy from POPS's buffer ──
	*l++ = GE_OP(GC_TEX_EN, 1);
	*l++ = GE_OP(GC_BLEND_EN, 0);
	*l++ = GE_OP(GC_DITHER_EN, 0);
	*l++ = GE_OP(GC_TMAP, 0);
	*l++ = GE_OP(GC_TMODE, 0);
	*l++ = GE_OP(GC_TPF, spf);
	*l++ = GE_OP(GC_TFILTER, 0);
	*l++ = GE_OP(GC_TWRAP, 0x101);
	*l++ = GE_OP(GC_TFUNC, 1);                   // REPLACE — fragment = texel
	*l++ = GE_OP(GC_TBP0, src);
	*l++ = GE_OP(GC_TBW0, ((src >> 8) & 0x0F0000) | (u32)sbw);
	*l++ = GE_OP(GC_TSIZE0, (9 << 8) | 9);
	*l++ = GE_OP(GC_TFLUSH, 0);
	*l++ = GE_OP(GC_TSYNC, 0);
	*l++ = GE_OP(GC_PRIM, (6 << 16) | ST_VERTS);

	// ── draw 2: the curve, in place on the shadow (self-texture) ──
	if (g_st_gamma != 100) {
		*l++ = GE_OP(GC_TFUNC, 0);               // MODULATE — fragment = texel * V
		*l++ = GE_OP(GC_TPF, dpf);
		*l++ = GE_OP(GC_TBP0, dst);
		*l++ = GE_OP(GC_TBW0, ((dst >> 8) & 0x0F0000) | (u32)dbw);
		*l++ = GE_OP(GC_TFLUSH, 0);              // draw 1 just wrote these texels
		*l++ = GE_OP(GC_TSYNC, 0);
	} else {
		*l++ = GE_OP(GC_TEX_EN, 0);              // temperature only: no fetch needed
	}
	*l++ = GE_OP(GC_BLEND_EN, 1);
	*l++ = GE_OP(GC_DITHER_EN, (dpf != 3) ? 1 : 0);
	*l++ = GE_OP(GC_DITH0, 0x1D0C);
	*l++ = GE_OP(GC_DITH1, 0xF3E2);
	*l++ = GE_OP(GC_DITH2, 0x0C1D);
	*l++ = GE_OP(GC_DITH3, 0xE2F3);
	if (g_st_gamma < 100) {
		*l++ = GE_OP(GC_BLEND, 0 | (10 << 4) | (0 << 8));   // src=otherColor, dst=FIX, op=ADD
		*l++ = GE_OP(GC_FIXA, 0);
		*l++ = GE_OP(GC_FIXB, g_dark_fix);
	} else {
		*l++ = GE_OP(GC_BLEND, 1 | (10 << 4) | (0 << 8));   // src=1-otherColor, dst=FIX, op=ADD
		*l++ = GE_OP(GC_FIXA, 0);
		*l++ = GE_OP(GC_FIXB, g_temp_fix);
	}
	*l++ = GE_OP(GC_PRIM, (6 << 16) | ST_VERTS);

	*l++ = GE_OP(GC_FINISH, 0);
	*l++ = GE_OP(GC_END, 0);
	return (int)(l - g_list);
}

// Resolve the REAL sceGe entry points (bypassing our own syscall patches on the
// enqueue pair — those are the frame-counter/seq hooks, not a path we want to
// re-enter). Idempotent; safe to call from any thread context.
static void st_resolve(void)
{
	if (!g_ge_enqueue)
		g_ge_enqueue = (int (*)(const void *, void *, int, void *))
			sctrlHENFindFunction("sceGE_Manager", "sceGe_user", 0xAB49E76A);   // sceGeListEnQueue
	if (!g_ge_sync)
		g_ge_sync = (int (*)(int, int))
			sctrlHENFindFunction("sceGE_Manager", "sceGe_user", 0x03444EB4);   // sceGeListSync
	if (!g_ge_getcmd)
		g_ge_getcmd = (int (*)(u32))
			sctrlHENFindFunction("sceGE_Manager", "sceGe_user", 0xDC93CFEF);   // sceGeGetCmd
}

// ── Which buffer the GAME is rendering into ──────────────────────────────────
// Read off the GE hardware. sceDisplayGetFrameBuf reports only the displayed
// buffer; the game's scene renders into a separate target the user-callable
// display library never names.
//
// Returns 1 and fills the outputs when the reading is a plausible VRAM
// framebuffer; 0 means don't trust it and the caller falls back to
// sceDisplayGetFrameBuf. If the address were a texture or depth target rather
// than a framebuffer, writing the curve into it would corrupt the game's data.
//
// No sign check on the return — sceGeGetCmd hands back the raw command word with
// the opcode in the top byte, so FBP reads as 0x9Cxxxxxx (negative as signed).
static int st_read_game_target(u32 *fb, int *bufw, int *pfmt)
{
	u32 a, w, f, addr, stride, fmt, bpp, need;

	if (!g_ge_getcmd) return 0;
	a = (u32)g_ge_getcmd(0x9C);   // FBP: address bits 0-23
	w = (u32)g_ge_getcmd(0x9D);   // FBW: address bits 24-31 in bits 16-23, + stride
	f = (u32)g_ge_getcmd(0xD2);   // FPF: framebuffer pixel format
	addr   = (a & 0x00FFFFFF) | ((w & 0x00FF0000) << 8);
	stride = w & 0xFFFF;
	fmt    = f & 0x3;
	// The game addresses VRAM eDRAM-RELATIVE (high byte 0). Our list builder
	// takes an ABSOLUTE address, so rebase; passing 0x00088000 through would
	// point the GE at main RAM.
	if ((addr & 0xFF000000) == 0) addr |= 0x04000000;

	if ((addr & 0xFF000000) != 0x04000000) return 0;      // outside VRAM
	if (stride < 64 || stride > 1024) return 0;
	bpp  = (fmt == 3) ? 4 : 2;
	need = (addr - 0x04000000) + stride * 272 * bpp;
	if (need > 0x00200000) return 0;                      // would run past 2MB of VRAM

	*fb = addr; *bufw = (int)stride; *pfmt = (int)fmt;
	return 1;
}

// Size the correction from the back buffer's stride, not the marker probe.
// Width = the stride (FBW). Height = the GE drawing region (REGION1/REGION2),
// which the game sets to its render area while drawing the scene — this handles
// non-uniform scaling (the PSP display is fixed 480x272, but a game's back
// buffer can be a different aspect, so the width ratio does NOT imply the
// height). Falls back to the width-ratio estimate when the region is implausible.
static void st_cov_from_buffer(u32 fb, int bw, int pf)
{
	int w = bw;
	int h = (bw * 272 + 479) / 480;   // fallback: uniform-scale estimate
	int from_region = 0;
	u32 reg1 = 0, reg2 = 0;
	if (g_ge_getcmd) {
		reg1 = (u32)g_ge_getcmd(0x15);   // REGION1: (y1<<10)|x1, origin
		reg2 = (u32)g_ge_getcmd(0x16);   // REGION2: (y2<<10)|x2, end (inclusive)
		int x1 = (int)(reg1 & 0x3FF), y1 = (int)((reg1 >> 10) & 0x3FF);
		int x2 = (int)(reg2 & 0x3FF), y2 = (int)((reg2 >> 10) & 0x3FF);
		int rh = y2 - y1 + 1;
		int rw = x2 - x1 + 1;
		if (rw >= 480 && rw <= 1024 && rh >= 272 && rh <= 512) {
			h = rh;
			from_region = 1;
		}
	}
	if (w > ST_COV_MAX_W) w = ST_COV_MAX_W;
	if (h > ST_COV_MAX_H) h = ST_COV_MAX_H;
	{
		u32 stride_b = (u32)bw * (u32)((pf == 3) ? 4 : 2);
		u32 room = 0x04200000u - fb;
		while (h > 272 && (u32)h * stride_b > room) h--;
	}
	if (w != g_cov_w || h != g_cov_h) {
		char b[96];
		g_cov_w = w; g_cov_h = h;
		g_vtx_gamma = -1;                          /* force the quad rebuild */
		sprintf(b, "[ST] coverage -> %dx%d (%s reg1=%04x reg2=%04x)",
		        w, h, from_region ? "region" : "stride-est", (unsigned)reg1, (unsigned)reg2);
		ST_UART(b);
	}
}

// Wait (bounded) for the game's frame to finish on the GE, so the pass lands at
// the earliest instant the frame exists to be corrected. Thread context only
// (it sleeps). Returns 1 if the GE went idle within the bound.
static int st_wait_ge_drain(void)
{
	int spins;
	for (spins = 0; spins < ST_GE_WAIT_SPINS; spins++) {
		if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) return 1;
		sceKernelDelayThread(ST_GE_WAIT_US);
	}
	return 0;
}

// Build + run the list. Caller owns the guards and the st_claim() single-flight.
// Returns 1 if the list actually ran.
//
// wait: 1 = block in sceGeListSync until the list has run; 0 = enqueue and
// return. The injection path MUST pass 0 — it runs inside the game's own
// sceGeListEnQueue syscall, where blocking deadlocks. Enqueuing there is safe.
//
// With wait=1 this is THREAD CONTEXT ONLY (sceGeListSync blocks). With wait=0 it
// is also reached from an interrupt handler — see st_ge_after_submit for why the
// enqueue is safe there, and note the `traceable` gate below: the UART trace must
// not run on that path.
static int st_submit(u32 fb, int bufw, int pfmt, int wait, int cw, int ch)
{
	int n, qid, k1, ok = 0;
	// The startup trace below is UART, and uart_puts blocks ~5.5ms per line —
	// unusable from the interrupt-context injection path, which now reaches here
	// (see st_ge_after_submit). Sampled once: the whole function is one context.
	int traceable = (g_trace_sub < ST_TRACE_N) && !sceKernelIsIntrContext();

	// Entry points resolved? st_resolve() runs from st_ensure_started(), i.e.
	// when the worker starts, so anything reaching here earlier would call
	// through a NULL pointer. Checked here, at the single point that
	// dereferences them.
	if (!g_ge_enqueue) return 0;

	// The GE buffers live in the user block (allocated at pass start). If it is
	// missing (thread context: allocate now; interrupt context: cannot alloc —
	// bail until the worker's path has ensured it).
	if (!s_bank_usr) {
		if (!sceKernelIsIntrContext()) st_banks_ensure();
		if (!s_bank_usr) return 0;
	}

	// Next bank, before the builders (they write through g_list). Caller owns
	// g_busy, so the rotation is single-flight with every other submit path.
	if (++g_list_idx >= ST_LIST_BANKS) g_list_idx = 0;
	g_list = g_list_bank[g_list_idx];

	if (g_shadow_src) {
		if (g_vtx_gamma != g_st_gamma || g_vtx_temp != g_st_temp ||
		    g_vtx_cw != cw || g_vtx_ch != ch)
			st_build_vertices(g_st_gamma, g_st_temp, cw, ch);
		n = st_build_shadow_list(fb, bufw, pfmt,
		                         g_shadow_src, g_shadow_sbw, g_shadow_spf, cw, ch);
	} else {
		if (g_vtx_gamma != g_st_gamma || g_vtx_temp != g_st_temp ||
		    g_vtx_cw != cw || g_vtx_ch != ch)
			st_build_vertices(g_st_gamma, g_st_temp, cw, ch);
		n = st_build_list(fb, bufw, pfmt, cw, ch);
	}
	// The GE reads the list through the uncached alias (uofw ge.c:3585) — flush
	// our cached stores first.
	sceKernelDcacheWritebackInvalidateRange(g_list, n * 4);

	k1 = pspSdkSetK1(0);   // kernel list/ctx buffers vs the driver's pspK1StaBufOk checks
	{
		struct StGeListArgs a;
		u32 t0 = sceKernelGetSystemTimeLow();
		a.size = 16; a.ctx = g_ge_ctx; a.numStacks = 0; a.stacks = NULL;
		if (traceable) {
			char b[80];
			sprintf(b, "[ST] sub%d fb=%08x bw=%d pf=%d n=%d", g_trace_sub, (unsigned)fb, bufw, pfmt, n);
			ST_UART(b);
		}
		qid = g_ge_enqueue(g_list, g_list + n, -1, &a);
		if (qid >= 0) {
			// Blocking sync makes the list buffer reusable afterwards, but skip
			// it once the game is exiting (nothing rebuilds g_list after quit).
			if (wait && !g_quit) g_ge_sync(qid, PSP_GE_LIST_DONE);
			if (traceable) {
				char b[64];
				sprintf(b, "[ST] sub%d done qid=%d", g_trace_sub, qid);
				ST_UART(b);
				g_trace_sub++;
			}
			{
				u32 us = sceKernelGetSystemTimeLow() - t0;
				if (us > g_stat[STAT_MAX_US]) g_stat[STAT_MAX_US] = us;
			}
			ok = 1;
		} else {
			g_stat[STAT_ERR]++; g_last_err = (u32)qid;
			if (traceable) {
				char b[64];
				sprintf(b, "[ST] sub%d ENQ FAIL %08x", g_trace_sub, (unsigned)qid);
				ST_UART(b);
				g_trace_sub++;
			}
		}
	}
	pspSdkSetK1(k1);
	return ok;
}

// 1 = keep off the GE entirely: a utility dialog is up, or one closed recently
// enough that the module teardown / the game's renderer rebuild may still be in
// flight (see ST_DIALOG_COOL_US). Called from the present hook AND from
// st_apply, covering both the inline path and the poll fallback. Clears the
// cooldown itself once both conditions are met and emits a trace event.
static int st_dlg_blocked(u32 now)
{
	if (g_dlg_open > 0 && (u32)(now - g_dlg_us) < ST_DIALOG_MAX_US) return 1;
	if (g_dlg_cool) {
		if ((int)(now - g_dlg_cool_us) < 0) return 1;                       // wall clock not up
		if ((u32)(g_st_ge_seq - g_dlg_cool_seq) < ST_MIN_GE_SEQ) return 1;   // game not rendering yet
		g_dlg_cool = 0;
		g_dlg_evt = 5;
	}
	return 0;
}

// ── Single-flight claim on the submit path ───────────────────────────────────
// The test-and-set must be ATOMIC against interrupts: a non-atomic
// `if (g_busy) ... g_busy = 1` could be preempted between the two by the
// injection interrupt (st_ge_after_submit), leaving two callers inside st_submit
// sharing the g_list bank pointer. Suspending interrupts across the two
// instructions closes it; every caller pairs this with st_release().
static int st_claim(void)
{
	unsigned int intr = sceKernelCpuSuspendIntr();
	int got = !g_busy;
	if (got) g_busy = 1;
	sceKernelCpuResumeIntr(intr);
	return got;
}

static void st_release(void)
{
	g_busy = 0;
}

// Stash one present for the worker's deferred apply (the FRONT-BUFFER FALLBACK
// in st_apply). Fields first, address last (the publish), all inside a
// suspended-interrupt window so the worker's snapshot can never tear. Wakes the
// worker immediately: its 16ms wait timeout would land the correction a full
// frame late, past the point where the buffer is still being scanned out.
static void st_stash(void *topaddr, int bufw, int pfmt, u32 now)
{
	unsigned int intr = sceKernelCpuSuspendIntr();
	g_pend_bufw = bufw; g_pend_pfmt = pfmt; g_pend_time = now;
	g_pend_addr = topaddr;
	sceKernelCpuResumeIntr(intr);
	if (g_evf >= 0) sceKernelSetEventFlag(g_evf, 1);
}

// The GE can only render into eDRAM (VRAM 0x04000000..0x04200000). Reject any
// framebuffer whose full frame extent is outside VRAM before we build a list.
static int st_fb_in_vram(u32 fb, int bufw, int pfmt)
{
	int bpp = (pfmt == 3) ? 4 : 2;
	u32 need;
	if (fb < 0x04000000 || fb >= 0x04200000) return 0;
	need = (fb - 0x04000000) + (u32)bufw * 272 * (u32)bpp;
	return need <= 0x00200000;
}

// Apply the pass to one presented buffer. Thread context only. `now` = the
// present's ARRIVAL timestamp (not apply time — the deferred path applies
// later, but the re-present window is between presents). The dialog gate is
// evaluated against LIVE time, not the arrival time: a dialog that opens after
// the present was stashed would underflow the subtraction and pass as not-
// blocked. `deferred`: 1 = called from the worker (its buffer is already
// presented; it skips the GE-idle gate and waits through the blocking sync on
// the worker thread instead); 0 = called inline from the present hook (buffer
// about to be presented, must sync for a flicker-free correction before the
// swap).
static void st_apply(void *topaddr, int bufw, int pfmt, u32 now, int deferred)
{
	u32 fb, seq;

	if (g_quit) return;   // game exiting — nothing more goes to the GE
	if (g_st_suspended) return;   // stood down for a firmware suspend (power coordination)
	// One counter per guard, so a skipped apply can say which guard stopped it.
	if (!st_active())                    { g_stat[STAT_AP_INACT]++; return; }
	if (g_menu_open || g_st_op_hold)     { g_stat[STAT_AP_MENU]++;  return; }   // g_st_op_hold: gamma held off for the whole save/load op
	if (!topaddr || bufw < 64 || bufw > 1024 || (unsigned)pfmt > 3) {
		g_stat[STAT_AP_ARG]++;
		// Log the values once, capped.
		if (g_trace_arg < ST_TRACE_N) {
			char b[80];
			sprintf(b, "[ST] apARG top=%08x bw=%d pf=%d", (unsigned)topaddr, bufw, pfmt);
			ST_UART(b);
			g_trace_arg++;
		}
		return;
	}
	if (!g_ge_enqueue || !g_ge_sync)     { g_stat[STAT_AP_RES]++;   return; }
	// A utility dialog owns the screen and the GE, or one just closed and things
	// are still settling — stay off it entirely (see st_dlg_blocked). Evaluated
	// on LIVE time, not the arrival time.
	if (st_dlg_blocked(sceKernelGetSystemTimeLow())) { g_stat[STAT_SKIP_DLG]++; return; }

	fb = (u32)topaddr & 0x1FFFFFFF;   // strip KSEG/uncached bits; GE takes the physical form
	if (!st_fb_in_vram(fb, bufw, pfmt)) {
		g_stat[STAT_AP_ARG]++;
		if (g_trace_arg < ST_TRACE_N) {
			char b[80];
			sprintf(b, "[ST] apNOVRAM fb=%08x bw=%d pf=%d", (unsigned)fb, bufw, pfmt);
			ST_UART(b);
			g_trace_arg++;
		}
		return;
	}
	seq = g_st_ge_seq;
	if (fb == g_last_fb) {
		if (seq == g_last_seq) { g_stat[STAT_SKIP_STATIC]++; return; }
		if ((u32)(now - g_last_apply_us) < ST_REPRESENT_FLOOR_US) { g_stat[STAT_SKIP_REP]++; return; }
	}
	// Two preconditions before any submit:
	// 1. The game must actually be rendering — g_st_ge_seq counts its GE submits,
	//    which are near-zero during the logo and first black frames.
	if (seq < ST_MIN_GE_SEQ) { g_stat[STAT_SKIP_EARLY]++; return; }
	// 2. The INLINE path must find the GE queue EMPTY: it tail-enqueues and then
	//    blocks in sceGeListSync, so anything queued ahead of it would be waited
	//    on indefinitely. The DEFERRED path targets the DISPLAYED buffer while
	//    the game renders the other one, so a busy GE is expected and harmless:
	//    it waits through the blocking sync below (on the worker thread, not the
	//    game's) instead.
	// FRONT-BUFFER FALLBACK (GTA Liberty City report; user-proposed, not yet
	// hardware-confirmed): when the inline path is rejected here the frame goes
	// out UNCORRECTED — the observed occasional "1 frame not applied" flicker.
	// Instead of dropping it, stash it for the worker: by the time the deferred
	// apply runs (GE-idle gate skipped, blocking sync on the worker thread) this
	// buffer has flipped and IS the displayed front buffer, so the correction
	// lands mid-scanout — a small transition band instead of a whole-screen
	// miss. ST_DEFER_MAX_US still discards a stash too old to describe the
	// buffer currently being scanned.
	if (!deferred && sceGeDrawSync(1) != PSP_GE_LIST_DONE) {
		g_stat[STAT_SKIP_GEBUSY]++;
		st_stash(topaddr, bufw, pfmt, now);
		return;
	}
	if (!st_claim()) { g_stat[STAT_BUSY]++; return; }   // single-flight; a lost frame, counted
	// BLOCKING submit (wait=1) on BOTH paths. On the inline path it runs inside
	// sceDisplaySetFrameBuf, so blocking means the buffer is fully corrected
	// before it becomes visible. On the DEFERRED path it runs on the worker
	// thread, so blocking just means our list is drained before the worker moves
	// on — no async in-flight list survives into the game's exit. 480x272, NOT
	// g_cov_w/g_cov_h: this path corrects a DISPLAY buffer, and the enlarged
	// coverage describes the composite's SOURCE. See st_apply_test.
	//
	// EXCEPT during the post-resume grace: enqueue NON-BLOCKING (wait=0) so the
	// game thread cannot deadlock in sceGeListSync while the GE re-settles after
	// a firmware resume. One frame may show pre-correction during the grace.
	if (st_submit(fb, bufw, pfmt,
	              (g_st_grace_until_us && (int)(now - g_st_grace_until_us) < 0) ? 0 : 1,
	              bufw < 480 ? bufw : 480, 272)) {
		g_last_fb = fb; g_last_seq = seq; g_last_apply_us = now;
		g_stat[STAT_APPLY]++;
	}
	st_release();
}

// Menu-screen entry point (the full-screen test pattern). Deliberately bypasses
// the g_menu_open gate and the double-apply guards — the caller redraws the
// pattern from scratch before every call. Returns 1 if the pass ran, 0 if it was
// skipped (GE unavailable/busy).
int st_apply_test(void *topaddr, int bufw, int pfmt)
{
	int ok;
	if (!topaddr || bufw < 64 || bufw > 1024 || (unsigned)pfmt > 3) return 0;
	if (g_st_suspended) return 0;   // stood down for a firmware suspend
	if (!st_active()) return 1;   // identity — the raw pattern IS the result
	st_resolve();
	st_banks_ensure();   // the menu test pattern needs the user block too
	if (!st_fb_in_vram((u32)topaddr & 0x1FFFFFFF, bufw, pfmt)) return 0;
	if (!g_ge_enqueue || !g_ge_sync) return 0;
	// Only submit when the queue is provably empty: the game's threads are
	// frozen behind the menu, so a stalled game list could never be advanced and
	// a tail-enqueued sync would block forever.
	if (sceGeDrawSync(1) != PSP_GE_LIST_DONE) return 0;
	if (!st_claim()) return 0;
	// 480x272 — the display, NOT g_cov_w/g_cov_h. st_cov_from_buffer sets the
	// global coverage to the region the GAME's composite reads (right for the
	// injection, which writes the offscreen SOURCE buffer), but the test screen
	// targets the MENU's 480x272 display buffer, so the enlarged coverage would
	// write past the end of the 272-row buffer.
	ok = st_submit((u32)topaddr & 0x1FFFFFFF, bufw, pfmt, 1,
	               bufw < 480 ? bufw : 480, 272);
	st_release();
	return ok;
}

// Drop the live path's "already corrected this buffer" state. Called when the
// test screen exits: it wrote the same buffer the game will present next, so
// the recorded fb/seq no longer describe what is on screen.
void st_guard_reset(void)
{
	g_last_fb = 0; g_last_seq = 0; g_last_apply_us = 0;
}

// Present-hook entry — see screen_tuning.h.
void st_on_present(void *topaddr, int bufferwidth, int pixelformat)
{
	u32 now;
	if (!topaddr) return;
	// Presence history is recorded UNCONDITIONALLY, ahead of the active and menu
	// gates — it describes what the GAME is doing, not what the pass is doing.
	// If the pass is off at boot and turned on later, presents made while it was
	// off still prove the display pipeline came up.
	now = sceKernelGetSystemTimeLow();
	g_last_present_us = now;   // presents are flowing — present-less path stays disarmed
	g_seen_present = 1;
	// Stood down for a firmware suspend: record that the game is presenting again
	// (the worker's re-acquire waits on this) but touch nothing on the GE.
	if (g_st_suspended) return;
	if (!st_active() || g_menu_open || g_st_op_hold || g_is_pops) return;   // g_st_op_hold: held off across a save/load op
	// Dialog up (or just closed): nothing goes to the GE. Placed after the two
	// lines above so the game keeps presenting behind the dialog and the quiet
	// clock keeps advancing (the poll fallback would otherwise arm mid-dialog).
	if (st_dlg_blocked(now)) return;
	// A real present just arrived, so this game is NOT present-less right now: the
	// injection is a present-LESS fallback and must stand down. It can arm during a
	// transient present-quiet window (e.g. Ys Seven's ~407ms loading pause in the
	// UART log armed it via poll0), and if it stays armed once presents resume BOTH
	// paths correct the same frame — the composite copies the injection-corrected
	// source, then this present hook curves the displayed copy again. That
	// double-apply washes out static frames and, frame-to-frame, flickers (see the
	// mutual-exclusion note in the worker's arming branch). Disarming here leaves
	// only this present-hook path — the same single-path state a suspend/resume
	// leaves behind, which is why the flicker "fixes after a suspend". The worker
	// re-arms only after presents go quiet for ST_POLL_QUIET_US again (g_last_present_us
	// is fresh here), so a presenting game cannot thrash arm/disarm.
	if (g_inject_arm) { g_inject_arm = 0; g_par_valid = 0; }
	if (sceKernelIsIntrContext()) {
		u32 fb  = (u32)topaddr & 0x1FFFFFFF;
		u32 seq = g_st_ge_seq;
		if (!st_fb_in_vram(fb, bufferwidth, pixelformat)) { g_stat[STAT_AP_ARG]++; return; }
		// A vblank-flip present is the instant this buffer becomes the displayed
		// frame — correct it NOW with a wait=0 enqueue (interrupt-safe, the same
		// mechanism the injection path uses) instead of stashing it to the worker.
		// At the flip the GE is normally idle (this frame just finished and the
		// next has not been submitted yet), so the list runs before the beam
		// gets far.
		if (fb == g_last_fb) {
			if (seq == g_last_seq) { g_stat[STAT_SKIP_STATIC]++; return; }
			if ((u32)(now - g_last_apply_us) < ST_REPRESENT_FLOOR_US) { g_stat[STAT_SKIP_REP]++; return; }
		}
		if (seq < ST_MIN_GE_SEQ)                  { g_stat[STAT_SKIP_EARLY]++;  return; }
		if (!st_claim()) { g_stat[STAT_BUSY]++; return; }
		if (st_submit(fb, bufferwidth, pixelformat, 0,
		              bufferwidth < 480 ? bufferwidth : 480, 272)) {
			g_last_fb = fb; g_last_seq = seq; g_last_apply_us = now;
			g_stat[STAT_APPLY]++;
		}
		st_release();
		return;
	}
	st_apply(topaddr, bufferwidth, pixelformat, now, 0);
}

// ── Utility-dialog hooks ─────────────────────────────────────────────────────
// Four syscall patches on the InitStart/ShutdownStart pairs. They only move the
// nesting count and tail-call the real function — no uart_puts here: these run
// on the game's own UI thread, so the trace is handed to the worker via
// g_dlg_evt instead.
static void st_dlg_enter(int evt)
{
	if (g_dlg_open <= 0) { g_dlg_open = 1; g_dlg_us = sceKernelGetSystemTimeLow(); }
	else g_dlg_open++;
	g_dlg_evt = evt;
}

static void st_dlg_leave(int evt)
{
	if (g_dlg_open > 0) g_dlg_open--;
	// Last dialog gone: arm the settling window (see ST_DIALOG_COOL_US).
	if (g_dlg_open == 0) {
		g_dlg_cool = 1;
		g_dlg_cool_us = sceKernelGetSystemTimeLow() + ST_DIALOG_COOL_US;
		g_dlg_cool_seq = g_st_ge_seq;
	}
	g_dlg_evt = evt;
}

static int st_sd_init_patched(void *param)
{
	st_dlg_enter(1);
	return g_real_sd_init ? g_real_sd_init(param) : 0;
}

static int st_sd_shut_patched(void)
{
	st_dlg_leave(2);
	return g_real_sd_shut ? g_real_sd_shut() : 0;
}

static int st_md_init_patched(void *param)
{
	st_dlg_enter(3);
	return g_real_md_init ? g_real_md_init(param) : 0;
}

static int st_md_shut_patched(void)
{
	st_dlg_leave(4);
	return g_real_md_shut ? g_real_md_shut() : 0;
}

// Installed at boot, unconditionally, so the nesting count tracks dialogs from
// the start. NIDs from PSP_References/ARK-4-main/contrib/psplibdoc_660.xml
// (sceUtility_Driver).
void st_install_dialog_hooks(void)
{
	g_real_sd_init = (int (*)(void *))
		sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x50C4CD57);   // sceUtilitySavedataInitStart
	if (g_real_sd_init)
		sctrlHENPatchSyscall((void *)g_real_sd_init, st_sd_init_patched);

	g_real_sd_shut = (int (*)(void))
		sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x9790B33C);   // sceUtilitySavedataShutdownStart
	if (g_real_sd_shut)
		sctrlHENPatchSyscall((void *)g_real_sd_shut, st_sd_shut_patched);

	g_real_md_init = (int (*)(void *))
		sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x2AD8E239);   // sceUtilityMsgDialogInitStart
	if (g_real_md_init)
		sctrlHENPatchSyscall((void *)g_real_md_init, st_md_init_patched);

	g_real_md_shut = (int (*)(void))
		sctrlHENFindFunction("sceUtility_Driver", "sceUtility", 0x67AF3428);   // sceUtilityMsgDialogShutdownStart
	if (g_real_md_shut)
		sctrlHENPatchSyscall((void *)g_real_md_shut, st_md_shut_patched);

	{
		char b[96];
		sprintf(b, "[ST] dlg hooks sd=%08x/%08x md=%08x/%08x",
		        (unsigned)g_real_sd_init, (unsigned)g_real_sd_shut,
		        (unsigned)g_real_md_init, (unsigned)g_real_md_shut);
		ST_UART(b);
	}
}

// ── Proof that the display pipeline is up ────────────────────────────────────
// The present-less path must not hand the GE work during the game's own GE
// init, so it needs evidence the pipeline is running first. A present through
// the hook is direct evidence, but it is not sufficient on its own: some games
// never call sceDisplaySetFrameBuf during play (they use the kernel-only
// sceDisplaySetFrameBufferInternal, see videoskip.c), so a present alone would
// leave the present-less path — built for exactly those games — unarmed. The
// game having submitted ST_MIN_GE_SEQ lists of its own is the same proof
// without needing a present.
static int st_pipeline_up(void)
{
	return g_seen_present || g_st_ge_seq >= ST_MIN_GE_SEQ;
}

// ── POPS: correct out of place and redirect the scanout ──────────────────────
// POPS's stretch into its display buffer does not go through the GE path this
// module can see, so there is no composite to ride. The correction is done OUT
// OF PLACE: POPS keeps its own buffer untouched, and the scanout is pointed at a
// shadow we fill ourselves.
//
// The redirect is a single register store (ARK iplsdk dmacplus.c,
// dmacplus_lcdc_set_base_addr). Reading back an address that is NOT our shadow
// is how we notice if POPS ever re-points, and that same read re-latches the
// source.
static void st_lcdc_set_addr(u32 addr)
{
	volatile u32 *r = (volatile u32 *)ST_LCDC_BASE;
	r[0] = addr & 0x1FFFFFFF;   // the register holds the PHYSICAL form
}

// Hand the display back to POPS. Called when the pass goes off, the menu opens,
// or the game exits — the shadow stops being refreshed then, so leaving the
// scanout on it would freeze the picture.
static void st_pops_restore(void)
{
	if (!g_pops_redirected) return;
	g_pops_redirected = 0;
	if (g_pops_src) st_lcdc_set_addr(g_pops_src);
}

// 1 = an on-screen overlay is active (FPS/battery/CPU/FT chart). On POPS these
// are drawn into its SINGLE-buffered scanout, which POPS re-blits every frame —
// so an overlay drawn straight into that buffer is wiped between vblanks and
// flickers. The ping-pong shadow (st_pops_tick) is what keeps it steady, so the
// worker must stay alive for it even when gamma/temp are neutral. FPS is a
// separate feature from screen tuning; this is what decouples them.
int st_pops_overlay_on(void)
{
	return g_show_fps_overlay || g_show_battery || g_show_cpu_usage || g_show_ft_chart;
}

// Allocate the shadow block (two buffers) from exclusive RAM, once. Returns 1 on
// success. Sized from the live stride/format so the scanout layout matches POPS's.
static int st_pops_db_alloc(int stride, int pf)
{
	char b[140];
	u32 head;
	g_shadow_need = (u32)stride * 272u * ((pf == 3) ? 4u : 2u);
	g_shadow_memid = sceKernelAllocPartitionMemory(ST_DB_PARTITION, "st_pops_db",
	                 PSP_SMEM_High, g_shadow_need * 2u + ST_DB_ALIGN, NULL);
	if (g_shadow_memid < 0) {
		sprintf(b, "[POPSDB] alloc %uKB failed (%08x) - redirect disabled",
		        (unsigned)((g_shadow_need * 2u) >> 10), (unsigned)g_shadow_memid);
		ST_UART(b);
		return 0;
	}
	head = (u32)sceKernelGetBlockHeadAddr(g_shadow_memid);
	g_shadow_base = (head + (ST_DB_ALIGN - 1)) & ~(ST_DB_ALIGN - 1);
	g_shadow_buf[0] = g_shadow_base;
	g_shadow_buf[1] = g_shadow_base + g_shadow_need;   // need is 16-aligned (stride*272*bpp)
	sprintf(b, "[POPSDB] alloc ok head=%08x buf0=%08x buf1=%08x need=%uKB/buf",
	        (unsigned)head, (unsigned)g_shadow_buf[0], (unsigned)g_shadow_buf[1],
	        (unsigned)(g_shadow_need >> 10));
	ST_UART(b);
	return 1;
}

// The FLIP, run from the VBLANK INTERRUPT (not the worker thread). Writes the
// scanout register here, at the true vblank, so the whole visible frame comes
// from the shadow. Register store only — no firmware calls, safe in interrupt
// context. The worker does the heavy copy and hands us the ready buffer index.
//
// Stands down while POPS is in its own menu (g_in_pops_menu) or our plugin
// menu is open (g_menu_open) — in both cases the shadow redirect would hide
// what the menu is drawing.
static int st_db_vblank_flip(int subno, void *arg)
{
	(void)subno; (void)arg;
	if (g_shadow_state == 3 && g_pops_redirected && g_db_ready >= 0 &&
	    !g_in_pops_menu && !g_menu_open && !g_st_op_hold && !g_st_suspended) {
		((volatile u32 *)ST_LCDC_BASE)[0] = g_shadow_buf[g_db_ready] & 0x1FFFFFFF;
		g_db_front = g_db_ready;
	}
	return -1;
}

// One tick of the POPS self-double-buffer, from the worker (thread context).
// Waits for a valid, STABLE in-game display, allocates two shadow buffers from
// exclusive RAM, then each vblank flips the scanout to the buffer filled last
// tick and copies+curves POPS's current frame into the other buffer. POPS's own
// buffer is never written.
static void st_pops_tick(void)
{
	u32 la; int lw, ls, lp, len, i, ok, back;
	u32 now = sceKernelGetSystemTimeLow();

	// Sample the scanout address EVERY tick, before any gate can return.
	ok = st_lcdc_read(&la, &lw, &ls, &lp, &len);   // 1 = valid stride/addr (rejects stride=0)
	if (ok && len) {
		if (g_la_last && la != g_la_last) g_la_changes++;
		g_la_last = la;
		for (i = 0; i < g_la_nseen; i++) if (g_la_seen[i] == la) break;
		if (i == g_la_nseen && g_la_nseen < ST_LA_SEEN) g_la_seen[g_la_nseen++] = la;
	}

	// Off / menu / dialog: hand the display back to POPS and stand down.
	// The live HUD still needs to draw even when gamma is OFF — it draws
	// directly into POPS's buffer (the scanout, since the double-buffer
	// is disengaged) without applying any gamma correction. An active overlay
	// keeps the double-buffer engaged (identity curve) for scanout stability.
	if (g_quit || g_st_suspended || (!st_active() && !st_pops_overlay_on()) || g_menu_open || g_st_op_hold || st_dlg_blocked(now)) {
		st_pops_restore();
		if (g_st_suspended) { g_db_front = -1; g_db_ready = -1; return; }
		if (g_st_hud && g_pops_src && g_pops_sbw > 0)
			st_hud_draw((void *)g_pops_src, g_pops_sbw, g_pops_spf);
		g_db_front = -1; g_db_ready = -1;
		return;
	}
	// Coexist with the menu flip hook: while POPS is FLIPPING (its own UI), the
	// flip hook corrects in place — do not redirect. Re-arm when flips stop.
	if ((u32)(now - g_flip_last_us) < ST_DB_IDLE_US) {
		st_pops_restore();
		g_db_front = -1; g_db_ready = -1;
		if (g_shadow_state != 9) g_shadow_state = 0;
		return;
	}
	g_in_pops_menu = 0;   // flips have been idle long enough — ISR can redirect

	if (g_shadow_state == 9) return;   // shadow unavailable — never redirect

	if (g_shadow_state == 3) {
		// Running: ping-pong. POPS keeps rendering into its own buffer (g_pops_src)
		// regardless of where the scanout points, so it always holds a fresh frame.
		// The scanout FLIP happens in st_db_vblank_flip (the vblank ISR).
		//
		// First tick after (re-)engagement (g_db_ready < 0): fill BOTH buffers
		// so neither holds stale pre-menu content. Otherwise the ISR's first
		// flip would briefly show an old frame — the "flicker squares" artifact.
		if (g_db_ready < 0) {
			st_pops_curve_copy(g_shadow_buf[0], g_pops_src, g_pops_sbw, g_pops_spf, g_db_w);
			st_pops_curve_copy(g_shadow_buf[1], g_pops_src, g_pops_sbw, g_pops_spf, g_db_w);
			fps_draw((void *)g_shadow_buf[0], g_pops_sbw, g_pops_spf);
			fps_draw((void *)g_shadow_buf[1], g_pops_sbw, g_pops_spf);
			if (g_st_hud) st_hud_draw((void *)g_shadow_buf[0], g_pops_sbw, g_pops_spf);
			g_pops_redirected = 1;   // re-arm the ISR — st_pops_restore() cleared it
			g_db_front = 0;
			g_db_ready = 1;
			((volatile u32 *)ST_LCDC_BASE)[0] = g_shadow_buf[0] & 0x1FFFFFFF;
			g_db_copies += 2;
			g_stat[STAT_APPLY] += 2;
			return;
		}
		// Normal ping-pong: fill the buffer NOT being displayed, then re-draw
		// the overlay into it. The ISR flips to it at the next vblank.
		u32 reg = ((volatile u32 *)ST_LCDC_BASE)[0] & 0x1FFFFFFF;
		if (reg != (g_shadow_buf[0] & 0x1FFFFFFF) &&
		    reg != (g_shadow_buf[1] & 0x1FFFFFFF)) {
			g_shadow_lost++;
			g_pops_src = reg;
		}
		back = (g_db_front == 0) ? 1 : 0;
		st_pops_curve_copy(g_shadow_buf[back], g_pops_src, g_pops_sbw, g_pops_spf, g_db_w);
		fps_draw((void *)g_shadow_buf[back], g_pops_sbw, g_pops_spf);
		if (g_st_hud && !g_menu_open) st_hud_draw((void *)g_shadow_buf[back], g_pops_sbw, g_pops_spf);
		g_db_ready = back;
		g_db_copies++;
		g_stat[STAT_APPLY]++;
		return;
	}

	// state 0: wait for a VALID, STABLE display before allocating/redirecting:
	// require the same address/stride/format for ST_DB_STABLE ticks.
	if (!ok || !len) { g_db_stable = 0; return; }
	if (la == g_db_last_a && ls == g_db_last_s && lp == g_db_last_p) {
		if (g_db_stable < ST_DB_STABLE) g_db_stable++;
	} else {
		g_db_last_a = la; g_db_last_s = ls; g_db_last_p = lp; g_db_stable = 0;
	}
	if (g_db_stable < ST_DB_STABLE) return;

	if (g_shadow_memid < 0 && !st_pops_db_alloc(ls, lp)) {
		g_shadow_state = 9;
		return;
	}

	// Latch POPS's buffer geometry and enter the running ping-pong. The vblank
	// ISR only writes the scanout while g_pops_redirected is set.
	g_pops_src = la; g_pops_sbw = ls; g_pops_spf = lp;
	g_db_w = (lw > 0 && lw < 512) ? lw : 480;
	g_db_front = -1; g_db_ready = -1;
	g_pops_redirected = 1;
	g_shadow_state = 3;
	{
		char b[150];
		sprintf(b, "[POPSDB] LIVE start src=%08x stride=%d pf=%d w=%d buf0=%08x buf1=%08x",
		        (unsigned)la, ls, lp, g_db_w,
		        (unsigned)g_shadow_buf[0], (unsigned)g_shadow_buf[1]);
		ST_UART(b);
	}
}

// The flip hook. POPS calls this from its own menu; in-game it never fires.
// Its ONLY job is signalling: update the menu/idle discriminator so the
// double-buffer ISR stands down while POPS owns the display. No correction is
// applied here. Signature matches the user call —
// DisplaySetFrameBuf(addr, PSP_SCREEN_LINE, PSP_DISPLAY_PIXEL_FORMAT_8888, sync).
static int st_flip_hook(void *topaddr, int width, int format, int sync)
{
	g_flip_calls++;
	g_flip_last_us = sceKernelGetSystemTimeLow();   // in-game vs POPS-menu discriminator
	// POPS is presenting its own menu — ISR must stand down. No gate on
	// st_dlg_blocked: a save dialog is a POPS UI frame just like the menu, and
	// the ISR must not redirect over it either.
	if (!g_menu_open)
		g_in_pops_menu = 1;
	return g_real_flip ? g_real_flip(topaddr, width, format, sync) : 0;
}

// Where the patch went and what was there before it, so it can be UNDONE. A
// patch into firmware code must have a restore path.
static u32 g_flip_at;
static u32 g_flip_orig[2];

// Install once, POPS only. Thread context (called from st_ensure_started).
static void st_pops_install_flip(void)
{
	u32 a = (u32)sctrlHENFindFunction("sceDisplay_Service", "sceDisplay_driver",
	                                  ST_POPS_FLIP_NID);
	char b[96];
	if (!a) { ST_UART("[POPS] flip hook: kernel sceDisplaySetFrameBuf NOT FOUND"); return; }

	g_flip_at      = a;
	g_flip_orig[0] = ST_LW(a);
	g_flip_orig[1] = ST_LW(a + 4);

	ST_SW(g_flip_orig[0], (u32)g_flip_tramp);        /* original instruction 0 */
	ST_SW(g_flip_orig[1], (u32)g_flip_tramp + 4);    /* ...and 1 (delay slot)  */
	ST_SW(0,              (u32)g_flip_tramp + 8);    /* nop                    */
	ST_SW(0x08000000 | (((a + 8) & 0x0FFFFFFC) >> 2), (u32)g_flip_tramp + 12);
	ST_SW(0,              (u32)g_flip_tramp + 16);   /* nop in the delay slot  */
	ST_SW(0x08000000 | (((u32)st_flip_hook >> 2) & 0x03FFFFFF), a);
	ST_SW(0, a + 4);
	g_real_flip = (int (*)(void *, int, int, int))g_flip_tramp;

	// Patched code must be visible to the instruction fetcher.
	sceKernelDcacheWritebackAll();
	sceKernelIcacheClearAll();
	sprintf(b, "[POPS] flip hook installed at %08x (kernel SetFrameBuf)", (unsigned)a);
	ST_UART(b);
}

// Put the firmware back exactly as it was. Called from st_stop (game exit) and
// from module_stop, so the patch never outlives this module.
void st_pops_remove_flip(void)
{
	// The vblank flip ISR goes first and UNCONDITIONALLY: it writes MMIO from an
	// interrupt and must never outlive this module (a stale handler would point at
	// freed code), and it can be installed even when the flip hook is not.
	// Clearing g_pops_redirected also makes it no-op instantly.
	g_pops_redirected = 0;
	if (g_db_vbl_on) {
		sceKernelDisableSubIntr(PSP_VBLANK_INT, ST_DB_VBL_SUBNO);
		sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, ST_DB_VBL_SUBNO);
		g_db_vbl_on = 0;
	}
	if (!g_flip_at) return;
	ST_SW(g_flip_orig[0], g_flip_at);
	ST_SW(g_flip_orig[1], g_flip_at + 4);
	g_flip_at = 0;
	g_real_flip = NULL;
	sceKernelDcacheWritebackAll();
	sceKernelIcacheClearAll();
	ST_UART("[POPS] flip hook removed");
}

// Returns the framebuffer where the HUD/overlay should draw. When the POPS
// double-buffer is active, the scanout reads from the shadow — drawing to
// POPS's buffer would be invisible. Returns the front shadow buffer in that
// case, otherwise topaddr unchanged.
void *st_pops_hud_fb(void *topaddr)
{
	if (g_shadow_state == 3 && g_pops_redirected && g_db_front >= 0)
		return (void *)g_shadow_buf[g_db_front];
	return topaddr;
}

// ── Per-game screen-tuning persistence (SAVESTATE/<gid>/screen.cfg) ──────────
// Gamma AND colour temperature are LIVE on the running game, unlike the
// gameset.cfg fields (which only take effect at that game's next boot) — so they
// deliberately do NOT ride in settings.cfg (global) or gameset.cfg (browsed
// folder). A separate file, keyed by umdid, read once at game boot and written
// when the HUD/test screen/main menu edits them. No legacy import from the old
// global settings.cfg field.
// Format: [0]=magic "GAMA", [1]=gamma·100 (50..200), [2]=temp (0..200). An
// older 2-word file loads gamma and leaves temp neutral (size-tolerant read).
#define ST_GAMMA_MAGIC 0x47414D41u   // "GAMA"
void st_load_game_gamma(void)
{
	char p[96]; SceUID fd; u32 buf[3]; int n;
	const char *gid = umdid[0] ? umdid : "globalstate";
	g_st_gamma = 100;   // default 1.00 = off per game
	g_st_temp  = 100;   // default neutral per game
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/screen.cfg", gid);
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return;
	n = sceIoRead(fd, buf, sizeof(buf));
	if (n >= (int)(2 * sizeof(u32)) && buf[0] == ST_GAMMA_MAGIC) {
		int g = (int)buf[1];
		if (g >= 50 && g <= 200) g_st_gamma = g;   // gamma·100 (0.50..2.00)
		if (n >= (int)(3 * sizeof(u32))) {
			int t = (int)buf[2];
			if (t >= 0 && t <= 200) g_st_temp = t;   // colour temp, 100 = neutral
		}
	}
	sceIoClose(fd);
}

void st_save_game_gamma(void)
{
	char p[96], d[80]; SceUID fd; u32 buf[3];
	const char *gid = umdid[0] ? umdid : "globalstate";
	buf[0] = ST_GAMMA_MAGIC; buf[1] = (u32)g_st_gamma; buf[2] = (u32)g_st_temp;
	sceIoMkdir("ms0:/seplugins/SAVESTATE", 0777);
	sprintf(d, "ms0:/seplugins/SAVESTATE/%s", gid);
	sceIoMkdir(d, 0777);
	sprintf(p, "%s/screen.cfg", d);
	fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, buf, sizeof(buf)); sceIoClose(fd); }
}

// ── Re-acquire after a firmware resume ───────────────────────────────────────
// Drop EVERY cached view of the display/GE and re-anchor, exactly as a fresh game
// would: the suspend rewrote all of VRAM, reset the GE from the SYSTEM context and
// re-latched the scanout, so nothing sampled before it can be trusted. Runs in the
// worker (thread context) when the delayed re-enable fires.
static void st_reacquire(void)
{
	g_last_fb = 0; g_last_seq = 0; g_last_apply_us = 0;
	g_disp_fb = 0;
	g_offscreen_fb = 0; g_offscreen_bw = 0; g_offscreen_pf = 0;
	g_inject_arm = 0; g_inject_pending = 0; g_inject_unfit = 0;
	g_par_scene_next = 0; g_par_valid = 0;
	g_arm_sweeps = 0; g_arm_seq = g_st_ge_seq;
	g_poll_arm_ticks = 0; g_poll_last_seq = g_st_ge_seq;
	g_last_present_us = sceKernelGetSystemTimeLow();
	// Coverage back to identity + force the vertex rebuild: the region is
	// re-read on a freshly re-latched frame, not the old one.
	g_cov_w = 480; g_cov_h = 272;
	g_vtx_gamma = -1; g_vtx_temp = -1;
	// POPS: re-stabilise on POPS's own buffer; the shadow's pixels are stale and the
	// scanout was re-latched by the firmware. Keep the allocated shadow block.
	g_pops_redirected = 0;
	g_db_front = -1; g_db_ready = -1; g_db_stable = 0;
	if (g_shadow_state != 9) g_shadow_state = 0;
	// Re-arm the existing sub/done UART trace so the FIRST few post-resume submits
	// are logged (it was capped out pre-suspend). This pinpoints the lock-up: a
	// "sub" line with no matching "sub done" == frozen inside sceGeListEnQueue;
	// both present then silence == the async GE list itself faulted.
	g_trace_sub = 0; g_trace_poll = 0; g_trace_arg = 0;
	st_resolve();   // GE entry points survive a resume, but this is cheap + idempotent
}

// ── Firmware suspend: stand the pass down ────────────────────────────────────
// Called from the plugin's sysevent handler (utils.c ProcessSignals) on the FIRST
// suspend-descent phase of the cycle; idempotent across the several phases in one
// suspend. NO UART/logging here: the suspend handshake must not be perturbed —
// the worker prints [ST-PWR] post-resume from thread context.
void st_pwr_suspend(void)
{
	int i;
	if (g_st_suspended) return;
	g_st_suspended = 1;
	g_st_susp_logged = 0;
	g_st_uart_revived = 0;   // arm a UART revive on the worker's first post-resume tick
	g_st_pwr_susp_ran = 1;   // debug: the handler actually engaged
	g_st_pwr_cycles++;
	// Stop the vblank ISR writing the scanout register and hand the display back to
	// POPS's own buffer (the shadow is not refreshed across the suspend, and all
	// VRAM is evacuated/restored by the firmware).
	g_pops_redirected = 0;
	st_pops_restore();
	g_db_front = -1; g_db_ready = -1;
	// Let an in-flight BLOCKING submit finish so the firmware's queue-suspend does
	// not catch one of our lists mid-flight. YIELD (never busy-spin — g_busy is
	// owned by another thread that needs the CPU). Bounded; usually 0 iterations
	// (g_busy is clear between frames). If threads are already frozen this just
	// times out and the firmware's _sceGeQueueSuspend handles the queued list.
	for (i = 0; i < ST_SUSP_DRAIN_SPINS && g_busy; i++)
		sceKernelDelayThread(ST_SUSP_DRAIN_US);
	g_st_susp_drain_busy = g_busy;   // debug: 0 = drained clean
}

// ── Firmware resume: arm a DELAYED re-enable ─────────────────────────────────
// Called from ProcessSignals on RESUME_COMPLETED (last resume event, thread ctx).
// Does NOT lift the stand-down here — the worker lifts it only after the game has
// been shown again for ST_RESUME_DELAY_US AND has submitted fresh GE work, so the
// correction re-appears on a real, re-latched frame rather than the stale restored
// FBP. If nothing re-enabled the pass before this (deferred case), the worker that
// starts on re-enable inherits this armed state.
void st_pwr_resume(void)
{
	if (!g_st_suspended) return;   // no suspend was recorded — nothing to re-arm
	g_st_resume_at_us = sceKernelGetSystemTimeLow() + ST_RESUME_DELAY_US;
	g_st_resume_anchor_seq = g_st_ge_seq;
	g_st_resume_pending = 1;
}

// Worker thread: applies deferred (interrupt-context) presents, runs the
// present-less poll fallback, emits the 1/s stats line. Self-exits when the
// pass is off or the game exits.
static int st_worker(SceSize args, void *argp)
{
	(void)args; (void)argp;
	while (1) {
		u32 out; SceUInt tmo = 16000;
		int waited;
		u32 use_fb = 0; int use_bw = 0, use_pf = 0, use_game = 0;   // correction target
		// Persist a setting the live HUD changed, once it has closed. Done here,
		// not in the controller hook that edits it (that runs on the game's
		// thread mid-syscall and must never touch the Memory Stick). Also before
		// the exit check below, which can end this thread.
		if (g_st_hud_dirty && !g_st_hud && !g_menu_open) {
			g_st_hud_dirty = 0;
			st_save_game_gamma();  // gamma + colour temp (per-game, screen.cfg)
		}
		// Arming has to prove itself — see ST_INJECT_PROVE_SEQ. Armed, rendering,
		// and still nothing injected means this game has no composite to ride, so
		// disarm and give the sweep a fresh chance (a mistimed early arm must not
		// strand the game in the poll fallback forever).
		if (g_inject_arm && g_inject_total == 0 &&
		    (u32)(g_st_ge_seq - g_arm_seq) >= ST_INJECT_PROVE_SEQ) {
			g_inject_arm = 0;
			g_arm_sweeps = 0;
			g_arm_seq = g_st_ge_seq;
			if (DBG_UART()) ST_UART("[ST] inject PROVE retry (re-arm later)");
		}
		// An unarmed game whose sweep budget burned out gets fresh sweep attempts
		// every prove window.
		if (!g_inject_arm && !g_inject_unfit && g_arm_sweeps >= ST_ARM_SWEEP_MAX &&
		    (u32)(g_st_ge_seq - g_arm_seq) >= ST_INJECT_PROVE_SEQ) {
			g_arm_sweeps = 0;
			g_arm_seq = g_st_ge_seq;
			if (DBG_UART()) ST_UART("[ST] arm sweep budget reset (retry)");
		}
		// Dialog trace, deferred off the game's UI thread by the hooks.
		if (g_dlg_evt) {
			int e = g_dlg_evt;
			char b[64];
			g_dlg_evt = 0;
			sprintf(b, "[ST] dlg %s n=%d",
			        e == 1 ? "savedata-in" : e == 2 ? "savedata-out" :
			        e == 3 ? "msg-in" : e == 4 ? "msg-out" :
			        "cool-end (pass re-armed)", g_dlg_open);
			ST_UART(b);
		}
		// Game is exiting — end unconditionally, ahead of every other check
		// (including the HUD one below, which would otherwise keep us alive).
		if (g_quit) {
			// Hand the display back before this thread stops refreshing the shadow,
			// or POPS would be left scanning a buffer nobody updates.
			st_pops_restore();
			g_st_worker_started = 0;
			sceKernelExitDeleteThread(0);
			return 0;
		}
		// Stay alive while the HUD is up even with nothing to apply — the HUD is
		// what turns the pass back on, and it still needs the save above. Stay
		// alive for a POPS overlay too: an overlay on POPS's single-buffered
		// scanout flickers without the ping-pong shadow (st_pops_tick).
		if (!st_active() && !g_st_hud && !(g_is_pops && st_pops_overlay_on())) {
			// Disarm on the way out. The injection hooks keep firing on every game
			// GE submit whether or not this thread exists, and g_inject_arm decides
			// how deep they go. It also forces a fresh capture if the pass is
			// switched back on, rather than reusing an offscreen address nobody has
			// re-observed since.
			g_inject_arm = 0;
			g_arm_sweeps = 0;
			g_par_valid = 0;   // re-enabling re-anchors instead of trusting a stale phase
			st_pops_restore();   // ...and stop scanning a shadow we no longer refresh
			g_st_worker_started = 0;
			sceKernelExitDeleteThread(0);
			return 0;
		}
		// Stood down for a firmware suspend: touch NOTHING on the GE/scanout until
		// the game has been shown again (uncorrected) for ST_RESUME_DELAY_US AND has
		// submitted ST_RESUME_MIN_SEQ fresh GE lists — "show the game first, then
		// re-apply". A re-enable long after the resume passes both instantly (no
		// artificial wait); one right after it waits out the delay.
		if (g_st_suspended) {
			// The firmware suspend reset the UART core / HPRM re-grabbed the port, so
			// the wire went dead. Revive it ONCE from here — a normal thread with the
			// firmware fully back up, the same context the save/load tail re-inits in
			// (fatsave.c) — so the [ST-PWR] and [ST] lines below reach the PC again.
			if (!g_st_uart_revived) {
				g_st_uart_revived = 1;
				if (DBG_UART()) uart_init();
			}
			if (g_st_resume_pending &&
			    (int)(sceKernelGetSystemTimeLow() - g_st_resume_at_us) >= 0 &&
			    (u32)(g_st_ge_seq - g_st_resume_anchor_seq) >= ST_RESUME_MIN_SEQ) {
				u32 nowr   = sceKernelGetSystemTimeLow();
				u32 waited = (nowr - g_st_resume_at_us) + ST_RESUME_DELAY_US;
				u32 nseq   = g_st_ge_seq - g_st_resume_anchor_seq;
				char b[140];
				st_reacquire();
				g_st_resume_pending = 0;
				g_st_suspended = 0;
				g_st_grace_until_us = nowr + ST_RESUME_GRACE_US;   // wait=0 grace starts now
				sprintf(b, "[ST-PWR] re-acquire: pass live (cyc=%u susp_ran=%d drain=%d waited=%ums +%useq)",
				        (unsigned)g_st_pwr_cycles, g_st_pwr_susp_ran, g_st_susp_drain_busy,
				        (unsigned)(waited / 1000), (unsigned)nseq);
				ST_UART(b);
				// fall through to normal operation this iteration
			} else {
				if (!g_st_susp_logged) {
					char b[120];
					g_st_susp_logged = 1;
					sprintf(b, "[ST-PWR] stood down for suspend (cyc=%u susp_ran=%d drain=%d) — showing game first",
					        (unsigned)g_st_pwr_cycles, g_st_pwr_susp_ran, g_st_susp_drain_busy);
					ST_UART(b);
				}
				sceKernelDelayThread(20000);   // idle ~20ms; do not spin, touch no GE
				continue;
			}
		}
		// Two modes, re-decided every iteration. A presenting game is driven by
		// the hook; this thread only services the interrupt-context presents
		// handed to it, and sleeps on the event flag. A game that does NOT present
		// has no frame event it can signal, so its only frame reference is the
		// display and this thread blocks on the real vblank instead.
		if (st_pipeline_up() && !g_menu_open &&
		    (u32)(sceKernelGetSystemTimeLow() - g_last_present_us) >= ST_POLL_QUIET_US) {
			u32 t0 = sceKernelGetSystemTimeLow(), el;
			fps_wait_vblank_real();   // returns at vblank START
			// ...but not once the display is gone, where it returns instantly and
			// this loop has nothing else pacing it (ST_TICK_FLOOR_US).
			el = sceKernelGetSystemTimeLow() - t0;
			{
				// A live vblank takes real time, so the short POPS floor still
				// catches a dead-display spin while letting a live tick through.
				u32 floor = g_is_pops ? 100u : ST_TICK_FLOOR_US;
				if (el < floor) sceKernelDelayThread(floor - el);
			}
			waited = -1;              // nothing was signalled; fall into the present-less path
		} else {
			waited = sceKernelWaitEventFlag(g_evf, 1, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &out, &tmo);
		}
		if (waited >= 0) {
			// deferred present — the FRONT-BUFFER FALLBACK stash from st_apply's
			// inline GE-busy reject. Snapshot + consume atomically (one-shot: a
			// stash describes ONE present; the next missed frame re-stashes
			// itself, so a rejected apply must not retry a stale one).
			void *a; int bw, pf; u32 t;
			unsigned int intr = sceKernelCpuSuspendIntr();
			a = g_pend_addr; bw = g_pend_bufw; pf = g_pend_pfmt; t = g_pend_time;
			g_pend_addr = NULL;
			sceKernelCpuResumeIntr(intr);
			g_stat[STAT_DWAKE]++;
			if (!a) g_stat[STAT_DNULL]++;
			// Discard a present older than ~33ms — it describes a buffer that has
			// flipped since, so applying it would correct the wrong buffer.
			if (a && (u32)(sceKernelGetSystemTimeLow() - t) >= ST_DEFER_MAX_US) {
				a = NULL;
			}
			if (a) st_apply(a, bw, pf, t, 1);
		} else if (!g_menu_open) {
			if (g_is_pops) {
				// POPS: self-double-buffer engine — the poll/inject fallback below
				// must NOT run for it (its st_read_game_target arming would target
				// PS1 VRAM).
				st_pops_tick();
			} else {
			// Present-less path, once per vblank. Each rejection is counted so a
			// skipped tick can say which gate stopped it.
			u32 now = sceKernelGetSystemTimeLow();
			u32 seq = g_st_ge_seq;
			int rendering = (seq != g_poll_last_seq);
			int quiet = st_pipeline_up() &&
			            (u32)(now - g_last_present_us) >= ST_POLL_QUIET_US;
			g_poll_last_seq = seq;
			// (POPS never reaches here — it is handled by st_pops_tick in the
			// g_is_pops branch above. This is the non-POPS present-less path.)
			// Arming: presents must be absent for ST_POLL_ARM_TICKS in a row
			// before anything is handed to the GE. Keyed on `quiet` ONLY — a present
			// resets it, a tick where the game happened to submit nothing does not
			// (`rendering` still gates the actual fire below, per-tick).
			if (quiet) {
				if (g_poll_arm_ticks < ST_POLL_ARM_TICKS) g_poll_arm_ticks++;
			} else {
				g_poll_arm_ticks = 0;
			}
			if (!quiet) {
				g_stat[STAT_POLL_NQUI]++;
			} else if (!rendering) {
				g_stat[STAT_POLL_NOREND]++;
			} else if (g_poll_arm_ticks < ST_POLL_ARM_TICKS) {
				g_stat[STAT_POLL_NARM]++;
			} else if (st_dlg_blocked(now)) {
				g_stat[STAT_SKIP_DLG]++;                 // checked BEFORE the 8ms wait, not after
			} else if (!st_wait_ge_drain()) {
				// UNARMED: wait, bounded, for the game's frame to leave the GE, then
				// correct immediately.
				//
				// ARMED: peek instead of wait — the correction comes from the
				// injection hook and this branch never reaches st_apply, so the
				// wait buys nothing. The idle TEST stays either way: the body still
				// republishes g_offscreen_fb from st_read_game_target, and FBP is
				// only trustworthy between lists (read mid-list it can name an
				// intermediate render target, which the injection would then write
				// the curve into).
				g_stat[STAT_POLL_GEBUSY]++;
			} else {
				void *topaddr = NULL; int bufw = 0, pfmt = 0;
				if (g_trace_poll < ST_TRACE_N) {
					char b[64];
					sprintf(b, "[ST] poll%d fire quiet=%ums", g_trace_poll,
					        (unsigned)((now - g_last_present_us) / 1000));
					ST_UART(b);
					g_trace_poll++;
				}
#if ST_PHASE_OFFSET_US > 0
				sceKernelDelayThread(ST_PHASE_OFFSET_US);   // phase knob, see the constant
#endif
				// Correct the buffer the GAME is rendering into, not the one
				// GetFrameBuf names — read here, after its frame drained and before
				// our own list runs (our list sets FBP/FBW/FPF itself). GetFrameBuf
				// is still needed: the HUD must be painted on the visible buffer,
				// and it is the fallback when the reading fails validation.
				{
					u32 gfb = 0; int gbw = 0, gpf = 0;
					if (st_read_game_target(&gfb, &gbw, &gpf)) {
						use_fb = gfb; use_bw = gbw; use_pf = gpf; use_game = 1;
						g_stat[STAT_GTGT]++;
					}
				}
				if (sceDisplayGetFrameBuf(&topaddr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_IMMEDIATE) >= 0
				    && topaddr && bufw > 0) {
					u32 before = g_stat[STAT_APPLY];
					if (!use_game) { use_fb = (u32)topaddr; use_bw = bufw; use_pf = pfmt; }
					// Publish the displayed buffer for the injection path, which runs
					// on the game's thread and cannot afford a syscall to look it up.
					g_disp_fb = (u32)topaddr & 0x1FFFFFFF;
					// Once this game is seen rendering somewhere other than the
					// displayed buffer, hand the correction over to the injection
					// path — it lands deterministically ahead of the composite. Both
					// running would double-apply and wash the image out.
					if (use_game && use_fb != g_disp_fb && !g_inject_unfit) {
						// Publish the offscreen buffer too — the injection needs the
						// address at a moment it cannot read it (see g_offscreen_fb).
						if (use_fb != g_offscreen_fb) {
							char b[96];
							sprintf(b, "[ST] arm offscreen=%08x bw=%d pf=%d disp=%08x",
							        (unsigned)use_fb, use_bw, use_pf, (unsigned)g_disp_fb);
							ST_UART(b);
						}
						g_offscreen_fb = use_fb;
						g_offscreen_bw = use_bw;
						g_offscreen_pf = use_pf;
						st_cov_from_buffer(use_fb, use_bw, use_pf);
						// Sample the proof window on the 0->1 edge only, or it would
						// restart every tick and never expire.
						if (!g_inject_arm) { g_inject_arm = 1; g_arm_seq = g_st_ge_seq; }
					} else if (!g_inject_arm && !g_inject_unfit &&
					           g_arm_sweeps < ST_ARM_SWEEP_MAX) {
						g_arm_sweeps++;
						g_arm_seq = g_st_ge_seq;   // anchor the budget-retry window
						// ── Arming sweep (only while unarmed) ────────────────────
						// The single reading above is taken right after
						// st_wait_ge_drain(), i.e. at GE IDLE — sampling across a
						// frame removes dependence on where the tick landed. Thread
						// context only (no GE reads from an interrupt handler).
						// Self-limiting: runs only until the first successful
						// capture.
						int s;
						for (s = 0; s < 8; s++) {
							u32 sfb; int sbw, spf;
							sceKernelDelayThread(4000);   // span two frames, break the phase lock
							if (st_read_game_target(&sfb, &sbw, &spf)
							    && sfb != g_disp_fb) {
								g_offscreen_fb = sfb;
								g_offscreen_bw = sbw;
								g_offscreen_pf = spf;
								st_cov_from_buffer(sfb, sbw, spf);
								g_inject_arm = 1;
								break;
							}
						}
					}
					// !g_is_pops: under POPS the correction comes from the submit
					// hook above; letting this fire too would put a second,
					// unordered pass on the same buffer.
					if (!g_inject_arm && !g_is_pops)
						st_apply((void *)use_fb, use_bw, use_pf, now, 1);
					if (g_stat[STAT_APPLY] != before) g_stat[STAT_POLL]++;
					// These games never reach the present hook, so the HUD is
					// painted here too (after the pass — never curve the UI).
					if (g_st_hud && !g_menu_open) st_hud_draw(st_pops_hud_fb(topaddr), bufw, pfmt);
				}
			}
			}   // end else (non-POPS present-less path)
		}
#if ST_DETAILED_UART_STATS
		if (DBG_UART()) {
			u32 tnow = sceKernelGetSystemTimeLow();
			if (tnow - g_stat_log_us >= 1000000) {
				char b[320];   // ~220 chars with the rej= breakdown; keep real margin
				int i;
				sprintf(b, "[ST] gam=%d tmp=%d apply=%u ap=%u/%u/%u/%u dw=%u/%u inj=%u(i%u,idle%u) arm=%d par=%d flip=%u rej=%u/%u/%u/%u/%u/%u/%u/%u poll=%u nqui=%u narm=%u nore=%u pgeb=%u skipS=%u skipR=%u early=%u geb=%u dlg=%u busy=%u err=%u(%08x) max=%uus",
				        g_st_gamma, g_st_temp,
				        (unsigned)g_stat[STAT_APPLY],
				        (unsigned)g_stat[STAT_AP_INACT], (unsigned)g_stat[STAT_AP_MENU],
				        (unsigned)g_stat[STAT_AP_ARG], (unsigned)g_stat[STAT_AP_RES],
				        (unsigned)g_stat[STAT_DWAKE], (unsigned)g_stat[STAT_DNULL],
				        (unsigned)g_stat[STAT_INJECT],
				        (unsigned)g_stat[STAT_INJ_INTR], (unsigned)g_stat[STAT_IIDLE],
				        g_inject_arm, g_par_valid, (unsigned)g_stat[STAT_PFLIP],
				        (unsigned)g_stat[STAT_REJ_ARM], (unsigned)g_stat[STAT_REJ_OFF],
				        (unsigned)g_stat[STAT_REJ_BUSY], (unsigned)g_stat[STAT_REJ_INTR],
				        (unsigned)g_stat[STAT_REJ_DISP], (unsigned)g_stat[STAT_REJ_READ],
				        (unsigned)g_stat[STAT_REJ_FLOOR], (unsigned)g_stat[STAT_REJ_PAR],
				        (unsigned)g_stat[STAT_POLL],
				        (unsigned)g_stat[STAT_POLL_NQUI], (unsigned)g_stat[STAT_POLL_NARM],
				        (unsigned)g_stat[STAT_POLL_NOREND], (unsigned)g_stat[STAT_POLL_GEBUSY],
				        (unsigned)g_stat[STAT_SKIP_STATIC], (unsigned)g_stat[STAT_SKIP_REP],
				        (unsigned)g_stat[STAT_SKIP_EARLY], (unsigned)g_stat[STAT_SKIP_GEBUSY],
				        (unsigned)g_stat[STAT_SKIP_DLG],
				        (unsigned)g_stat[STAT_BUSY], (unsigned)g_stat[STAT_ERR],
				        (unsigned)g_last_err, (unsigned)g_stat[STAT_MAX_US]);
				ST_UART(b);
				if (g_is_pops) st_pops_report();
				g_stat_log_us = tnow;
				for (i = 0; i < STAT_N; i++) g_stat[i] = 0;
			}
		}
#endif // ST_DETAILED_UART_STATS
	}
	return 0;
}

// Called from the game-exit hook (overclock.c, which already owns the
// sceKernelExitGame patch; a second patch on the same NIDs would replace its
// entry and bypass its clock revert). Stops the pass and waits, bounded, for
// the in-flight submit and the worker to end. The worker must not outlive its
// game: once the display is gone sceDisplayWaitVblankStart stops blocking, so a
// live worker would spin. The present path runs on the game's own thread and
// blocks in sceGeListSync; if the exit tears the GE down underneath that thread
// it crashes. So drain BOTH g_busy (the in-flight submit) and the worker. The
// quit flag is set first, so nothing new can enter either.
void st_stop(void)
{
	int i, busy_waits = 0;
	g_quit = 1;
	// Disarm the vblank ISR before anything else — it writes the scanout register
	// from interrupt context. The redirect flag makes it no-op right now;
	// st_pops_remove_flip() disables and releases the handler itself.
	g_pops_redirected = 0;
	// Take the firmware patch out BEFORE anything else — it must never outlive
	// this module. Also kills the vblank ISR.
	st_pops_remove_flip();
	// Give the display back to POPS immediately. The worker restores it too, but
	// it may not get another tick before it exits, and leaving the scanout on a
	// shadow nobody refreshes would freeze the picture on the way out.
	st_pops_restore();
	// In-flight submit on a game thread. Bounded; st_submit no longer starts a
	// fresh sync once quit is set.
	for (i = 0; i < 25 && g_busy; i++) { sceKernelDelayThread(2000); busy_waits++; }
	// If the game thread is still inside sceGeListSync, drain the GE ourselves.
	if (g_busy) st_wait_ge_drain();
	// One final bound after draining — the sync should have released g_busy; if
	// not, the game thread is stuck and we bound rather than loop forever (the
	// real ExitGame is coming either way).
	for (i = 0; i < 10 && g_busy; i++) sceKernelDelayThread(2000);

	for (i = 0; i < 50 && g_st_worker_started; i++)
		sceKernelDelayThread(2000);   // up to ~100ms; one vblank wait is ~16.7ms
	// Worker done; ensure the GE is idle before handing back to the caller,
	// which will change the bus clock and then tear the GE down.
	st_wait_ge_drain();
	// Free the shadow block now the worker (its only user) has stopped and the
	// scanout is back on POPS's own buffer (st_pops_restore above).
	if (g_shadow_memid >= 0) {
		sceKernelFreePartitionMemory(g_shadow_memid);
		g_shadow_memid = -1;
		g_shadow_base = 0;
	}
	g_shadow_state = 0;
	{
		char b[80];
		sprintf(b, "[ST] stop busy=%d(w%d) worker=%d", g_busy, busy_waits,
		        g_st_worker_started);
		ST_UART(b);
	}
}

// ── Injection: decide BEFORE the game's list, enqueue AFTER it ───────────────
// The composite can be submitted from an interrupt handler, where we must not
// enqueue — so the composite itself cannot be acted on. GE lists are FIFO, so
// enqueuing ours after the game's SCENE list (thread context) puts the
// correction after the scene completes and before the composite is submitted:
//
//   scene submit (THREAD ctx) -> [game scene list] -> [our correction] -> ...
//                                          ... -> composite submit (INTR ctx)

// st_ge_on_submit runs BEFORE the real enqueue and only decides;
// st_ge_after_submit runs after it and does the work. Splitting it matters:
// FBP is a hardware register updated when a list EXECUTES, so after the real
// enqueue returns the scene may already be running and the reading would name
// the offscreen buffer instead of the displayed one.
//
// `list` is the raw GE display list the game just submitted, forwarded from the
// enqueue hooks — used only by the POPS list dump.
void st_ge_on_submit(const void *list)
{
	u32 fb, now;
	int bw, pf, intr, scene;
	(void)list;   // only the removed GE-list dumper ever read this

	// Every rejection is counted — one counter per guard, indexed by STAT_REJ_*.
	g_inject_pending = 0;
	if (g_quit) return;
	if (g_st_suspended) return;   // stood down for a firmware suspend (power coordination)
	// ── POPS: census only, for the [POPS] report line — never enqueue. The
	// correction runs on the CPU in the worker (st_pops_tick); this hook does
	// not participate. The census itself is diagnostic-only, so skip the
	// sceGeGetCmd read entirely when nothing will read its output.
	if (g_is_pops) {
		if (DBG_UART()) st_pops_observe();
		return;
	}
	// DO NOT call sceGeGetCmd from interrupt context: it brackets the read with
	// sceSysregAwRegABusClockEnable/Disable, so the interrupt-context branch
	// below classifies from the parity latch and reads no GE register at all.
	// The offscreen capture likewise happens in THREAD context, in the worker.
	//
	// st_active() first: this hook fires on every game GE submit, so the
	// fully-off state must cost two loads and a branch.
	if (!st_active()) return;

	// Parity bookkeeping before any guard that can return: a submit skipped for
	// an unrelated reason still consumed its slot in the alternation, so the
	// latch has to advance for it or every skip desyncs the next frame.
	intr  = sceKernelIsIntrContext();
	scene = g_par_scene_next;
	g_par_scene_next = !scene;

	if (!g_inject_arm)                          { g_stat[STAT_REJ_ARM]++;  return; }
	if (g_menu_open || g_st_op_hold)            { g_stat[STAT_REJ_OFF]++;  return; }   // g_st_op_hold: held off across a save/load op
	if (g_busy)                                 { g_stat[STAT_REJ_BUSY]++; return; }
	if (!g_disp_fb || !g_offscreen_fb)          { g_stat[STAT_REJ_DISP]++; return; }

	if (!intr) {
		// Thread context: FBP is readable, so take the truth and re-anchor the
		// latch to it. The SCENE submit is the one where FBP still names the
		// displayed buffer, because the list that last executed was the composite.
		if (!st_read_game_target(&fb, &bw, &pf)) { g_stat[STAT_REJ_READ]++; return; }
		{
			int truth = (fb == g_disp_fb);
			if (g_par_valid && truth != scene) g_stat[STAT_PFLIP]++;
			scene = truth;
			g_par_scene_next = !truth;
			g_par_valid = 1;
		}
	} else if (!g_par_valid) {
		// Never anchored, so there is nothing to extrapolate from — guessing here
		// would inject into a buffer the composite is about to read.
		g_stat[STAT_REJ_INTR]++; return;
	}

	if (!scene) {
		g_stat[STAT_REJ_PAR]++;
		return;
	}
	// No GE-idle check: we are deliberately queueing BEHIND the scene list, so a
	// busy GE is expected.
	//
	// The floor is a backstop against a third submit in one frame. It applies at
	// full strength only to LATCH-GUESSED submits (nothing verified the role); a
	// thread-context submit was FBP-verified as the scene and is not floored as
	// hard.
	now = sceKernelGetSystemTimeLow();
	if ((u32)(now - g_inject_last_us) < (intr ? ST_INJECT_FLOOR_US : ST_VERIFIED_FLOOR_US))
	                                            { g_stat[STAT_REJ_FLOOR]++; return; }
	g_inject_pending = 1;
}

// Called from BOTH hooks AFTER the real enqueue, so FIFO puts our correction
// behind the game's scene list. Enqueue only — never sceGeListSync (which would
// block on the game's thread). Enqueuing from interrupt context is deliberate:
// uofw _sceGeListEnQueue touches the GE hardware registers only in the
// queue-EMPTY branch; with a list already active it appends to a linked list
// under sceKernelCpuSuspendIntr. STAT_IIDLE counts the times the GE nevertheless
// reported idle here.
void st_ge_after_submit(void)
{
	if (!g_inject_pending) return;
	g_inject_pending = 0;
	if (g_st_suspended) return;   // stood down for a firmware suspend (power coordination)
	if (!st_claim()) return;
	if (sceKernelIsIntrContext()) {
		g_stat[STAT_INJ_INTR]++;
		// sceGeDrawSync(1) is a poll, not a wait: safe to call here.
		if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) g_stat[STAT_IIDLE]++;
	}
	// The one path that legitimately uses the enlarged coverage: it writes the
	// OFFSCREEN buffer, which is the source the composite scales down.
	if (st_submit(g_offscreen_fb, g_offscreen_bw, g_offscreen_pf, 0,
	              g_cov_w, g_cov_h)) {
		g_inject_last_us = sceKernelGetSystemTimeLow();
		g_last_fb = g_offscreen_fb; g_last_seq = g_st_ge_seq;   // keep the worker's guard in sync
		g_stat[STAT_INJECT]++; g_inject_total++;
	}
	st_release();
}

// Reserve the user-RAM GE-buffer block early — called from the boot auto-open
// block (menu thread, right after the game boots) whether or not the pass is
// active, so a later mid-game st_banks_ensure still has user-heap room.
void st_prealloc(void)
{
	st_resolve();
	st_banks_ensure();
}

void st_ensure_started(void)
{
	SceUID thid;
	st_resolve();
	st_banks_ensure();   // allocate the user-RAM GE-buffer block once, at pass start
	if (g_evf < 0)
		g_evf = sceKernelCreateEventFlag("pspstates_st_evf", 0, 0, NULL);
	if (g_st_worker_started) return;
	// Anchor the quiet-window clock and arming streak to the worker's start time
	// (not the 0 st_init() left), so the poll fallback does not arm on its very
	// first tick.
	g_last_present_us = sceKernelGetSystemTimeLow();
	g_poll_last_seq = g_st_ge_seq;
	g_poll_arm_ticks = 0;
	g_arm_sweeps = 0;   // a new game gets a fresh budget to find its target
	g_arm_seq = 0; g_inject_unfit = 0;   // ...and a fresh chance to prove the injection
	// POPS corrects at the flip. Installed here, removed in st_stop.
	if (g_is_pops && !g_flip_at) st_pops_install_flip();
	// POPS in-game: the vblank ISR does the double-buffer scanout flip (see
	// st_db_vblank_flip). Installed here, released in st_stop so it never
	// outlives the module.
	if (g_is_pops && !g_db_vbl_on) {
		if (sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, ST_DB_VBL_SUBNO,
		        (void *)st_db_vblank_flip, NULL) >= 0 &&
		    sceKernelEnableSubIntr(PSP_VBLANK_INT, ST_DB_VBL_SUBNO) >= 0) {
			g_db_vbl_on = 1;
			ST_UART("[POPSDB] vblank flip handler installed");
		} else {
			ST_UART("[POPSDB] vblank subintr register FAILED");
		}
	}
	// A new game must take its own FBP anchor before the latch is trusted.
	g_par_scene_next = 0; g_par_valid = 0;
	g_inject_total = 0;
	g_quit = 0;   // a previous stop must not kill the new worker
	g_st_worker_started = 1;
	{
		char b[96];
		sprintf(b, "[ST] worker start gam=%d tmp=%d enq=%08x sync=%08x",
		        g_st_gamma, g_st_temp, (unsigned)g_ge_enqueue, (unsigned)g_ge_sync);
		ST_UART(b);
	}
	// Background thread priority (32), lower than the menu thread (16).
	thid = sceKernelCreateThread("pspstates_st", st_worker, 32, ST_WORKER_STACK_BYTES, 0, NULL);
	if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
	else g_st_worker_started = 0;
}
