#ifndef __PSPFATSAVE_H__
#define __PSPFATSAVE_H__

// header
#include <pspkernel.h>
#include <systemctrl.h>
#include <pspsysevent.h>
#include <pspsysmem_kernel.h>
#include <pspiofilemgr_kernel.h>
#include <psppower.h>
#include <pspctrl.h>
#include <pspthreadman.h>
#include <pspthreadman_kernel.h>
#include <pspintrman_kernel.h>   /* sceKernelIsIntrContext — the present hook's interrupt-context gate */
#include <pspge.h>
#include <pspinit.h>
#include <psprtc.h>
#include <pspdisplay.h>
#include <pspumd.h>
#include <string.h>
#include <stdio.h>

#include "version.h"

// event_id
#define PSP_SYSEVENT_RESUME_COMPLETED			0x400000
#define PSP_SYSEVENT_KERNEL_POWER_LOCK_PHASE1	0x401
// LAST suspend sysevent before power-down (uofw sysmem_sysevent.h):
// suspend order is QUERY->START->PHASE2(16..0)->PHASE1(0..2)->FREEZE->
// PHASE0(15..0), and PHASE0_0 fires LAST = fully quiesced. This is where
// we touch/overwrite kernel RAM so resume continues from it consistently.
#define SCE_SYSTEM_SUSPEND_EVENT_PHASE0_0		0x4000

// Sleep-hybrid flags/buffer (shared fatsave.c <-> utils.c).
// g_sleep_arm: 1 = arm the RTC auto-wake on the next power-lock sysevent.
// g_resumed:   set by RESUME_COMPLETED; poll thread logs it (no sceIo in handler).
// g_sleep_mode: action to run at g_cap_phase — 0 none, 9 = SAVE capture, 10 = LOAD
//   apply (see utils.c::ProcessSignals).
extern int g_sleep_arm;
extern int g_resumed;
extern int g_sleep_mode;
extern volatile int g_cap_phase;   // suspend sysevent at which mode 9/10 run; always PHASE0_0 currently
// g_op_mode (0 = SAVE, 1 = LOAD) is the first field of this contiguous load-carry
// struct. It is patched INTO the kernel snapshot on a load, so after the PHASE0_0
// rollback the live value survives (a plain global would be clobbered back to the
// snapshot's captured value). ProcessSignals (utils.c) reads g_lc.op_mode on
// RESUME_COMPLETED to gate the LOAD-only flash1: reinit — hence the extern here.
struct load_carry { volatile u32 op_mode; volatile int verify_mbps; volatile int read_mbps;
                    volatile u32 start_lo; volatile u32 start_hi; };
extern volatile struct load_carry g_lc;
extern volatile u32 g_dma_drain_left; // mode-9 DMAC drain residual (0=drained); logged post-resume

// ── Save-time dispatcher-context handoff (cross-session load fix) ──
// g_handoff_ctx: filled by SysEventShim (extras.S) at the entry of EVERY sysevent
// dispatch — s0-s7, fp, gp, sp, ra, CP0 Status (word layout in extras.S). Lives in
// plugin .data inside the kernel image, so the SAVE's mode-9 capture embeds the
// save-time dispatcher-return context in every snapshot. The LOAD's ApplyHandoff
// (apply.S) restores it from the applied image and jumps to the save-time ra —
// the load session is abandoned instead of returning through load-session code
// whose addresses no longer match the applied image (the cross-session hang).
// g_handoff_ctx_addr: LOAD side — the SAVE-TIME address of g_handoff_ctx (from the
// save header), passed to ApplyHandoff by mode 10. 0 = no handoff armed (DIAG paths).
extern volatile u32 g_handoff_ctx[16];
extern volatile u32 g_handoff_ctx_addr;

// extras.S — sysevent entry shim; registered as events.handler (see above).
int SysEventShim(int ev_id, char *ev_name, void *param, int *result);

// ── Overclock (shared overclock.c <-> utils.c) ──
// g_overclock_id: GLOBAL persisted step (0 = stock 333MHz) — see overclock.c's oc_*
// block. oc_apply(): writes the raw PLL registers for a step; non-static so
// utils.c's ProcessSignals can re-apply it on RESUME_COMPLETED (ANY firmware
// resume — native sleep or our own save/load — reverts the PLL as a side effect).
extern int g_overclock_id;
void oc_apply(int id);

// Compression chunk size. fastlz processes the game RAM / VRAM / kernel in
// SAVE_CHUNK_SIZE input chunks. 64KB benchmarked faster than 32KB (fewer sceIo
// calls + a better compression ratio) and is the speed sweet spot.
#define SAVE_CHUNK_SIZE   0x10000   // 64KB input chunks
// work_buf holds one record: [8-byte header][fastlz output]. fastlz can EXPAND
// incompressible data, so size for its worst case: input + ~6.25% + the 8-byte
// header + slack (fastlz's contract is "5% larger, min 66 bytes"; input/16 = 6.25%
// stays safely above that). For 64KB that's ~68KB — not the old fantasy 128KB.
// (Derived from SAVE_CHUNK_SIZE so it stays correct if the chunk size changes.)
#define COMPRESS_BUF_SIZE (SAVE_CHUNK_SIZE + (SAVE_CHUNK_SIZE / 16) + 128)

// Save file magic & version
#define SAVESTATE_MAGIC   0x50535053  // "PSPS"
#define SAVESTATE_VERSION (0x00000000 | VERSION_NUMBER)
// header[11] normally holds the VRAM section offset (nonzero == 64KB-aligned format).
// Its top bit additionally flags an UNCOMPRESSED (Fast) save, whose RAM/VRAM/kernel
// sections are raw contiguous blobs (no records) written straight from memory — see
// write_region_direct / read_region_direct. Mask it off to recover the offset.
#define SAVE_FLAG_UNCOMPRESSED 0x80000000u
#define SAVE_VRAMPOS_MASK      0x7FFFFFFFu
// Save header size in u32 words. 10 -> 12: [10] = save-time &g_handoff_ctx (the
// dispatcher-context block ApplyHandoff restores from the applied image), [11] =
// VRAM section offset (see above). 12 -> 13: [12] = save-time &g_overclock_id —
// the kernel rollback on LOAD would otherwise restore the SAVE's own overclock
// setting instead of the CURRENT session's; FreezeLoad patches the CURRENT live
// g_overclock_id into the staged snapshot at this offset (same technique as
// g_op_mode's patch), so "current session wins" over whatever a given save was
// made at. Both FreezeSave and FreezeLoad size their header[] with this so the
// section offsets (sizeof(header)) stay symmetric. Old (12-word) saves fail the
// CRC check under a 13-word header (the verify range shifts) — clean, resumable
// "Load aborted", not a hard failure — re-save under this build to carry it.
#define SAVE_HEADER_WORDS 13

// Game memory regions to save (24MB user RAM — safe for both Phat & Slim)
// KSEG0: cached, directly-mapped (kernel-mode safe). KSEG1: uncached.
#define MAIN_RAM_BASE_CACHED   0x88800000  // KSEG0 — cached, fast, direct-mapped
#define VRAM_BASE_CACHED       0x84000000  // KSEG0 — cached

#define MAIN_RAM_SIZE          0x01800000  // 24MB
#define VRAM_SIZE              0x00200000  // 2MB (Phat); Slim has 4MB but save 2MB for compat

// Kernel partition. The lower 8MB (0x88000000-0x88800000) splits (firmware
// partition table, sysmem.c) into: 4MB REAL kernel (0x88000000-0x88400000:
// other1 3MB + other2 1MB — TCBs, semaphores/event-flags/timers, exception
// vectors, and this plugin incl. g_handoff_ctx, which loads at ~0x88200000)
// and 4MB "vshell"/volatile scratch (0x88400000-0x88800000, the region
// sceKernelVolatileMemLock hands out). The game releases + rebuilds volatile
// across the freeze (cooperative_volmem_release), so its contents are
// don't-care — we capture/restore ONLY the lower 4MB. Capture/apply run at
// PHASE0_0 (fully firmware-quiesced), where this range is readable/writable;
// it is not under a live hard freeze (intr_suspend()), where the low pages fault.
#define KERNEL_RAM_BASE        0x88000000
#define KERNEL_RAM_SIZE        0x00400000  // 4MB: 0x88000000 .. 0x88400000 (real kernel; skips volatile)

// Batched-I/O buffer = the FULL 4MB vshell/volatile scratch (0x88400000-
// 0x88800000). Accumulates many compressed records and flushes in a few large
// writes (killing the per-small-write cluster read-modify-write cost). Two
// gates, both understood now:
//  - DDR MPU nibbles (0xBC000008 = ME half parts 16-23, 0xBC00000C = GE/AW
//    half parts 24-31): power.prx zeroes them (all-denied) the moment the
//    game releases the volatile lock; any access under 0x0 hangs. Every use
//    goes through iobuf_open_verified() (set 0xF = the lock-holder value on
//    BOTH regs, readback-verify, settle, retry; per-chunk work_buf fallback).
//  - What looked like a second, address-based gate on the ME half
//    (0x88400000-0x88600000) — bulk fills there froze v397/v398 even under a
//    verified 0xF — turned out to be the SAME root cause as the freezes on
//    the GE/AW half (v399): an unaligned u32 header store in the packed
//    record writer (MIPS faults on unaligned word access). Fixed at the
//    record level (memcpy, not direct pointer stores), so BOTH halves are
//    fully usable — v401's probe proved the ME half fills/verifies clean.
// Contents are don't-care across the freeze (game releases + rebuilds
// volatile; firmware clobbers it at suspend/resume) — NOT usable for the
// kernel section, which straddles the suspend. Sits directly below game RAM,
// so no overlap with the RAM/VRAM being saved.
#define IOBUF_BASE             0x88400000
#define IOBUF_SIZE             0x00400000  // 4MB: both halves
// The two MPU regs the 4MB spans (see above) — iobuf_open_verified/
// iobuf_still_open must check BOTH, since a partial revoke (one half zeroed,
// the other not) is just as unsafe as a full one.
#define IOBUF_MPU_REG_LO       0xBC000008   // ME half, 0x88400000-0x885FFFFF
#define IOBUF_MPU_REG_HI       0xBC00000C   // GE/AW half, 0x88600000-0x887FFFFF

// Game-RAM scratch used during the firmware suspend (the phat has no spare bank,
// so the staging region IS game RAM — freed by writing it to MS first). The 24MB
// user partition splits into three 8MB slots (Low/Mid/High); the kernel snapshot
// stages in whichever the Settings "Stage spot" selects (g_stage_base, default Mid
// 0x89000000 — the slot that avoids both GTA's low-RAM DMA and the firmware's
// top-RAM suspend bookkeeping). KSEG1 (|0x20000000) = uncached alias for executing
// copied code without icache flushes (VRAM/eDRAM exec fails; main-RAM works).
// GAME_STAGE_KERNEL is the legacy default base (Low); the runtime path uses g_stage_base.
// The LOAD apply routine + its scratch stack live at the START of the upper 8MB (High
// slot), immediately above the Mid snapshot — clear of the Mid/Low staging slots and
// the kernel window it overwrites, AND clear of the firmware's top-of-RAM suspend
// bookkeeping (the old top-64KB spot 0x89FF0000 sat right in that region and could be
// clobbered mid-suspend). (High-slot staging still overlaps this 64KB, so High is
// save-only; Mid/Low are load-safe.) A LOAD's game-RAM restore later clobbers it, so
// reusing game data here is fine.
#define GAME_STAGE_APPLY       0x89800000  // apply routine (LOAD only): start of upper 8MB
#define KSEG1_ALIAS            0x20000000  // add to a KSEG0 (0x8x) addr -> KSEG1 (0xAx) uncached

// ── Debug build switch ──
// 1 = development build: debug output available, routed by the "Debug Messages"
//     setting (g_show_debug below).
// 0 = public/stable release: ALL debug output (MS log + on-screen checkpoints,
//     including failure lines and the boot version banner) is compiled OUT —
//     the log functions/checkpoints become no-op macros, so the calls, their
//     sprintf work AND the string literals drop out of the binary entirely.
//     NB: with 0 the deploy loop's version check (greps the MS log for
//     "pspstates_v<N>") reports a warning — expected for release builds.
#ifndef DEBUG_BUILD
#define DEBUG_BUILD 1
#endif

// Debug-output routing (persisted in settings.cfg; set via the Settings menu):
//   0 = OFF (default)   1 = Log MS   2 = Log Screen   3 = Screen and MS
// bit0 = write debug lines to the MS log, bit1 = draw checkpoints on screen.
// The MS log file (ms0:/pspstates_debug.txt) is NEVER created or appended to
// while DBG_MS() is off, no exceptions — ms_write_line() (utils.c) is the sole
// choke point every WriteDebugLog* variant goes through, including the *Raw
// ones (version banner, FAILURE/ABORT lines), so nothing touches the MS file
// unless Debug (MS) is explicitly on. UART (DBG_UART()) stays independent of
// all of this and still mirrors *Raw lines regardless of DBG_MS().
extern int g_show_debug;
// UART logging is INDEPENDENT of g_show_debug: 0 = off, 1 = on. When on, every
// routed debug line + checkpoint is mirrored to the PSP->PC UART regardless of
// the MS/Screen setting (so UART can carry the full trace with the MS log OFF,
// and vice-versa). Persisted in settings.cfg. See DBG_UART() below and uart.c.
extern int g_uart_log;
#if DEBUG_BUILD
#define DBG_MS()   (g_show_debug & 1)
#define DBG_SCR()  (g_show_debug & 2)
#define DBG_UART() (g_uart_log)
#else
#define DBG_MS()   0
#define DBG_SCR()  0
#define DBG_UART() 0
#endif

// Software threshold (in KB) at which the compressed save/load loops flush the
// accumulated IOBUF to the Memory Stick, and the chunk size the Fast-mode direct
// path uses per sceIoWrite/Read. Fixed at 1MB (no longer a menu setting). The
// DDR-MPU unlock always covers the full 4MB (IOBUF_BASE/IOBUF_SIZE).
extern int g_iobuf_flush_kb;
// Save compression mode: 1 = Compact (FastLZ level 1), 0 = Fast (store raw). See
// g_compress / encode_chunk in fatsave.c. Persisted; toggled from the Settings menu.
extern int g_compress;
// PER-GAME Intro Video Skip (gameset.cfg word 4). A LEARNED time window, not a detector:
//   VSKIP_OFF(0)     - off (default)
//   VSKIP_CAPTURE(1) - next boot shows a banner and learns the intro length from the user
//   VSKIP_TIMED(2)   - fire the skip for g_video_skip_ms from the boot anchor
// See the video_skip_* block in videoskip.c for why a learned window beat every automatic
// signal we tried.
extern int g_video_skip;
// The learned intro length in ms (gameset.cfg word 5), valid when g_video_skip==VSKIP_TIMED.
extern int g_video_skip_ms;

// Overlay / frame-limit / browser settings. Defined in fatsave.c; read by the
// sysstats.c overlays (FPS/battery/CPU/frametime, frame limiter) and written from
// the menu.c settings UI. Kept here with the other persisted settings globals.
extern int g_default_slot;       // browser initial selection (0=New, 1=Last)
extern int g_stage_spot;         // kernel-snapshot staging slot (0=Low 1=Mid 2=High)
extern int g_autoload;           // per-game auto-open-on-boot
extern int g_frame_limit;        // per-game frame cap (0=off, else target FPS)
extern int g_overclock_stable;   // overclock marked stable (skip boot confirm)
extern int g_show_fps_overlay;   // FPS overlay window (0=off,1/2/3)
extern int g_fps_show_lows;      // draw 1% low alongside FPS
extern int g_show_battery;       // battery overlay (0=off,1/2/3)
extern int g_show_cpu_usage;     // CPU load overlay
extern int g_show_ft_chart;      // scrolling frametime histogram

// Disc id + "_" + 8-hex ISO-master hash (provided by systemctrl). Shared by the
// core, menu browser, and video-skip per-game settings.
extern char umdid[24];

// FastLZ
int fastlz_compress_level(int level, const void* input, int length, void* output);
int fastlz_decompress(const void* input, int length, void* output, int maxout);

// Hard freeze — clear the CP0 Status IE bit to stop ALL interrupts +
// the dispatcher (everything halts except us). Used to read the kernel
// partition atomically/safely when a soft-freeze read hangs. NO sceIo
// works while hard-frozen — only plain memory ops. Always pair
// suspend/resume and keep the window short.
static inline u32 intr_suspend(void) {
	u32 sr;
	asm volatile("mfc0 %0, $12" : "=r"(sr));
	asm volatile("mtc0 %0, $12" :: "r"(sr & ~1));
	asm volatile("nop; nop; nop; nop");
	return sr;
}
static inline void intr_resume(u32 sr) {
	asm volatile("mtc0 %0, $12" :: "r"(sr));
	asm volatile("nop; nop; nop; nop");
}

// Kernel-freeze save/load. Each suspends the game's user threads on entry and
// resumes them on exit. (The old manage_freeze=0 caller-owned-freeze and
// restore_kernel=0 baseline-load variants were removed — see git history.)
// FreezeLoad applies the save-time kernel via the staged firmware suspend; on
// success the handoff jumps into the save-time session and never returns here.
int  FreezeSave(const char *path);
int  FreezeLoad(const char *path);

// stub.S(sceRtc_driver)
int sceRtcSetAlarmTick(u64 *tick);

// main.c
int OnModuleStart(SceModule2 *module);
int module_start(SceSize args, void *argp);
int module_stop(SceSize args, void *argp);

// videoskip.c / menu.c
// Zero the video-skip state (kernel PRX .bss is not zeroed — see video_skip_init).
void video_skip_init(void);
// Called from OnModuleStart for EVERY module the game loads (game thread — no MS
// I/O in here). Logs the module name over UART and latches the address of a psmf
// player's scePsmfPlayerGetCurrentStatus for the video-skip watcher to patch.
void video_skip_probe(SceModule2 *module);
void PspLsLibraryLauncher(SceCtrlData *pad_data);
int sceCtrlPeekBufferPositivePatched(SceCtrlData *pad_data, int count);
int sceCtrlReadBufferPositivePatched(SceCtrlData *pad_data, int count);
int sceCtrlPeekBufferNegativePatched(SceCtrlData *pad_data, int count);
int sceCtrlReadBufferNegativePatched(SceCtrlData *pad_data, int count);   // third button-leak fix (buffer-backlog games)
int sceCtrlReadLatchPatched(SceCtrlLatch *latch);   // second button-leak fix (latch games)
int sceCtrlPeekLatchPatched(SceCtrlLatch *latch);
int menu_thread(SceSize args, void *argp);   // woken by the open combo; owns the save browser

// ── Menu UI colors (used by menu.c / gfx.c) ── AABBGGRR ── "slate + azure" theme.
// (dbg_fill_rect/dbg_text live in gfx.c; wait_buttons_up in fatsave.c; work_buf in fatsave.c.)
// One cohesive dark-slate ground with a single azure primary; each save/load/fail/intro
// operation carries its own accent so the banner colour signals the mode at a glance.
#define BR_BG     0xFF20140F   // ground: deep slate navy (#0F1420)
#define BR_CARD   0xFF312018   // surface/card: one step up from the ground (#182031)
#define BR_SEL    0xFF5E3723   // selected-row band: deep azure (#23375E)
#define BR_LINE   0xFF46342A   // hairline/track divider (#2A3446)
#define BR_WHITE  0xFFFFFFFF   // primary text
#define BR_GREY   0xFFAD9688   // secondary/muted text (#8896AD)
#define BR_CYAN   0xFFFF9A4C   // primary accent (azure #4C9AFF): titles, scrollbar, Save
#define BR_STRIPE 0xFFFF9A4C   // selection accent stripe (azure), alias of the primary
#define BR_AZURE  0xFFFF9A4C   // Save accent
#define BR_TEAL   0xFFC4D42F   // Load accent (#2FD4C4)
#define BR_AMBER  0xFF3DB4FF   // Intro-skip accent (#FFB43D)
#define BR_GREEN  0xFF00FF00   // success / done: pure bright green (#00FF00) — matches the original
#define BR_RED    0xFF3B3BFF   // failure / blocked: bright red (#FF3B3B)
#define BR_SHADOW 0xFF000000   // flat drop-shadow under the card (offset bottom-right)

// In-game HUD overlay text color (FPS/1% Low/Battery, drawn over the live
// game frame — see fps_draw/battery_draw).
#define OVERLAY_FG 0xFFFFFFFF

// Overlay colors: battery arrows + CPU/GPU bottleneck highlights (0xAABBGGRR).
#define OVERLAY_GREEN  0xFF00FF00   // battery charging
#define OVERLAY_ORANGE 0xFF00A5FF   // battery discharging; GPU-bound (text + chart bar)
#define OVERLAY_BLUE   0xFFF6B564   // CPU-bound (text + chart bar)

// fatsave.c — opens+verifies the volatile IOBUF region's DDR-MPU unlock
// (0xF on both halves); returns 1 if it held, 0 if it didn't (caller must not
// touch IOBUF in that case).
int iobuf_open_verified(void);

// Save-preview thumbnail: native 480x272 downsampled by 4 (nearest-neighbor) to
// 120x68 (both divide evenly), stored 16-bit 565 in a "<name>.thb" sidecar.
#define THUMB_W  120
#define THUMB_H  68
#define THUMB_DS 4

// A SHORT TAP of the NOTE button opens the save browser. Holding NOTE is the
// game's audio-mute, so we only trigger on a quick press+release (< NOTE_TAP_US);
// a long hold is left alone for the game. The menu thread does the rest.
#define NOTE_TAP_US 400000   // <400ms press = a tap (open menu); longer = mute (ignore)
extern volatile int g_menu_open;    // 1 while the browser is up (blocks re-trigger)
extern SceUID g_menu_thid;          // the menu thread, woken on the open combo
// Real sceController_Service Peek/Read, captured before sctrlHENPatchSyscall
// redirects them to the *Patched wrappers (main.c). The wrappers call through these.
extern int (*g_real_ctrl_peek)(SceCtrlData *, int);
extern int (*g_real_ctrl_read)(SceCtrlData *, int);
extern int (*g_real_ctrl_peek_neg)(SceCtrlData *, int);
extern int (*g_real_ctrl_read_neg)(SceCtrlData *, int);
extern int (*g_real_ctrl_readlatch)(SceCtrlLatch *);
extern int (*g_real_ctrl_peeklatch)(SceCtrlLatch *);
extern volatile int g_suppress_latch;
extern volatile int g_suppress_posbuf_calls;
extern int (*g_set_home_popup)(int);   // sceImposeSetHomePopup (resolved at runtime)

// apply.S — PIC kernel-overwrite routine, byte-copied into game RAM and run
// from there (it overwrites the lower-8MB kernel, so it can't run from it).
// One-way apply for the load path: cached copy + full I/D cache flush + jump to
// the save-time dispatcher context (never returns). See apply.S header comment.
// ApplyHandoffEnd marks the end so we know how many bytes to copy.
void ApplyHandoff(const void *src, void *dst, unsigned int len, const void *ctx);
extern unsigned char ApplyHandoffEnd[];

// utils.c
void ClearCaches(void);
int ProcessSignals(int ev_id, char *ev_name, void *param, int *result);
// Debug loggers (utils.c). WriteDebugLog/Hex are ROUTED: they only write when
// the Debug setting has the MS bit (DBG_MS). The *Raw variants (reserved for
// the boot version banner and failure/ABORT lines) go through the SAME MS gate
// (ms_write_line() checks DBG_MS() itself) — "Raw" only means they always
// mirror to UART regardless of DBG_MS(), not that they bypass the MS file.
// A release build (DEBUG_BUILD 0) turns all four into no-ops at the call site.
#if DEBUG_BUILD
void WriteDebugLog(const char* msg);
void WriteDebugLogHex(const char* prefix, u32 val);
void WriteDebugLogRaw(const char* msg);
void WriteDebugLogHexRaw(const char* prefix, u32 val);
// Formatted always-write variant (builds the line itself). Use this instead of a
// separate sprintf()+WriteDebugLogRaw() pair so that in a RELEASE build the format
// string AND the formatting call drop out entirely (the pair leaves the literal +
// a dead sprintf in the binary — only the log SINK is macro-gated, not the sprintf).
void WriteDebugLogRawF(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
#define WriteDebugLog(msg)           ((void)0)
#define WriteDebugLogHex(p, v)       ((void)0)
#define WriteDebugLogRaw(msg)        ((void)0)
#define WriteDebugLogHexRaw(p, v)    ((void)0)
#define WriteDebugLogRawF(...)       ((void)0)
#endif

// uart.c — PSP->PC UART debug channel (TX-only) over the remote connector. The
// ONLY log that survives the firmware suspend window (register-only byte I/O).
// See HOWTO_UART.md. uart_init() makes firmware calls + sleeps: call ONLY where
// the firmware is alive (module_start, post-resume after wait_for_resume) — NOT
// inside the suspend descent, where only uart_puts/uart_log_hex are safe.
// Release build (DEBUG_BUILD 0): all three become no-ops at the call site.
#if DEBUG_BUILD
void uart_init(void);
void uart_puts(const char *s);
void uart_log_hex(const char *prefix, unsigned int val);
#else
#define uart_init()          ((void)0)
#define uart_puts(s)         ((void)0)
#define uart_log_hex(p, v)   ((void)0)
#endif

#endif

