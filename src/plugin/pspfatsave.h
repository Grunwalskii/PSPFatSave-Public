#ifndef __PSPSTATES_V2_H__
#define __PSPSTATES_V2_H__

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

// Sleep-hybrid flags/buffer (shared lslibrary.c <-> utils.c).
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
// reserved. Both FreezeSave and FreezeLoad size their header[] with this so the
// section offsets (sizeof(header)) stay symmetric. Old 10-word saves fail the
// load's ctx-address validation (and the version prompt already warns) — re-save.
#define SAVE_HEADER_WORDS 12

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
//   4 = Time Only (MS) — see below
// bit0 = write debug lines to the MS log, bit1 = draw checkpoints on screen.
// In a debug build, FAILURE/ABORT lines and the boot version banner bypass the
// setting (WriteDebugLog*Raw) — rare, and post-mortems/deploy checks need them.
//
// Value 4 ("Time Only"): deliberately chosen as 0b100 so DBG_MS()/DBG_SCR()
// (bit0/bit1) are BOTH already false for it — every ROUTED checkpoint
// (ms_test_cp, WriteDebugLog/Hex) is automatically silent, while genuine
// FAILURE lines still force through (label_has_fail bypasses the DBG_MS()
// check). The handful of ad-hoc WriteDebugLogHexRaw/WriteDebugLogRaw calls
// added during the volatile-RAM investigation ([VOLMEM], [DDR], [IOBUF]
// informational lines) are NOT routed — they always fire regardless of this
// setting by design — so those specific call sites additionally check
// !DBG_TIME_ONLY() to stay silent in this mode. The result: in Time Only
// mode, the ONLY thing that reaches the MS log during a save/load is the
// once-at-the-end RAM-buffered trace dump (see trace_start/trace_record/
// trace_flush in lslibrary.c) plus real failures — nothing interferes with
// the timing being measured.
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
#define DBG_TIME_ONLY() (g_show_debug == 4)
#else
#define DBG_MS()   0
#define DBG_SCR()  0
#define DBG_UART() 0
#define DBG_TIME_ONLY() 0
#endif

// Software threshold (in KB) at which the compressed save/load loops flush the
// accumulated IOBUF to the Memory Stick, and the chunk size the Fast-mode direct
// path uses per sceIoWrite/Read. Fixed at 1MB (no longer a menu setting). The
// DDR-MPU unlock always covers the full 4MB (IOBUF_BASE/IOBUF_SIZE).
extern int g_iobuf_flush_kb;
// Save compression mode: 1 = Compact (FastLZ level 1), 0 = Fast (store raw). See
// g_compress / encode_chunk in lslibrary.c. Persisted; toggled from the Settings menu.
extern int g_compress;

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

// Kernel-freeze save/load. manage_freeze=1 (the only value the live callers
// pass): the function suspends user threads on entry and resumes them on exit.
// (manage_freeze=0 — caller owns the suspend/resume bracket — is no longer used;
// the same-session self-test that relied on it was removed. See git history.)
int  FreezeSave(const char *path, int manage_freeze);
// restore_kernel=1 additionally restores the kernel partition (Stage 3
// experiment) — overwrites live kernel RAM except our module + our stack.
int  FreezeLoad(const char *path, int manage_freeze, int restore_kernel);

// stub.S(sceRtc_driver)
int sceRtcGetAlarmTick(u64 *tick);
int sceRtcSetAlarmTick(u64 *tick);

// main.c
int OnModuleStart(SceModule2 *module);
int module_start(SceSize args, void *argp);
int module_stop(SceSize args, void *argp);

// lslibrary.c
void PspLsLibraryLauncher(SceCtrlData *pad_data);
int sceCtrlPeekBufferPositivePatched(SceCtrlData *pad_data, int count);
int sceCtrlReadBufferPositivePatched(SceCtrlData *pad_data, int count);
int sceCtrlReadLatchPatched(SceCtrlLatch *latch);   // second button-leak fix (latch games)
int sceCtrlPeekLatchPatched(SceCtrlLatch *latch);
int menu_thread(SceSize args, void *argp);   // woken by the open combo; owns the save browser

// ── Menu UI colors (used by lslibrary.c) ── AABBGGRR.
// (dbg_fill_rect/dbg_text/wait_buttons_up/work_buf are static in lslibrary.c.)
#define BR_BG     0xFF3A1E0E   // menu background (dark blue)
#define BR_SEL    0xFFB05010   // selected-row highlight
#define BR_WHITE  0xFFFFFFFF
#define BR_GREY   0xFFB0B0B0
#define BR_CYAN   0xFFFFFF00

// lslibrary.c — opens+verifies the volatile IOBUF region's DDR-MPU unlock
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
extern int (*g_real_ctrl_readlatch)(SceCtrlLatch *);
extern int (*g_real_ctrl_peeklatch)(SceCtrlLatch *);
extern volatile int g_suppress_latch;
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
// the Debug setting has the MS bit (DBG_MS). The *Raw variants ALWAYS write in
// a debug build — reserved for the boot version banner and failure/ABORT lines.
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

