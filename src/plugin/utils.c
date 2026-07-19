// PspStates for PSP (32MB)
// Home-menu streaming — simplified utils

// header
#include "pspstates.h"
#include <stdarg.h>   // WriteDebugLogRawF (DEBUG only)

// event_handler — entered via SysEventShim (extras.S), which records the
// dispatcher-return context into g_handoff_ctx on every dispatch (so the SAVE's
// mode-9 capture embeds it in the snapshot) and then tail-jumps to ProcessSignals.
PspSysEventHandler events =
{
	sizeof(PspSysEventHandler),
	"ProcessSignals",
	0x00FFFF00,
	SysEventShim,
}; // 0x2EF0

// extern
extern int state_flag;			// 0x2F50
// Sleep-hybrid flags (defined in lslibrary.c).
extern int g_sleep_arm;
extern int g_resumed;
extern int g_sleep_mode;
extern volatile u32 g_kcap_size;   // kernel capture/apply window size (KERNEL_RAM_SIZE)
extern volatile u32 g_kcap_off;    // kernel capture/apply window byte offset (0)
extern volatile u32 g_stage_base;     // runtime base of the kernel-snapshot staging slot
extern volatile u32 g_apply_base;   // LOAD apply-blob base (user setting); mode 10 runs from here
extern volatile u32 g_dma_drain_left;

// sub_000001F8
void ClearCaches(void)
{
	sceKernelDcacheWritebackAll();
	sceKernelIcacheClearAll();
}

// Custom Debug Loggers — see pspstates.h: WriteDebugLog/Hex are routed by the
// Debug setting's MS bit; the *Raw variants always write (version banner +
// failure lines). All compiled out entirely when DEBUG_BUILD is 0.
#if DEBUG_BUILD
// The MS sink: write one line to the debug file. UART is handled separately in
// the callers so it can fire INDEPENDENTLY of the MS setting (DBG_UART()).
static void ms_write_line(const char* msg)
{
	SceUID fd = sceIoOpen("ms0:/pspstates_debug.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0) {
		sceIoWrite(fd, msg, strlen(msg));
		sceIoWrite(fd, "\n", 1);
		sceIoClose(fd);
	}
}

// Raw: ALWAYS write to the MS log (version banner + failure lines). Additionally
// mirror to UART whenever UART logging is on — the two sinks are independent.
void WriteDebugLogRaw(const char* msg)
{
	ms_write_line(msg);
	if (DBG_UART()) uart_puts(msg);
}

// Routed: MS log only when DBG_MS(); UART only when DBG_UART(). Each sink decides
// on its own so UART can carry the trace with the MS log OFF (and vice-versa).
void WriteDebugLog(const char* msg)
{
	if (DBG_MS())   ms_write_line(msg);
	if (DBG_UART()) uart_puts(msg);
}

// Shared "<prefix> 0xXXXXXXXX" line builder for the two hex loggers.
static void build_hex_line(char *buf, const char* prefix, u32 val)
{
	int len = 0, p;
	const char* hex = "0123456789ABCDEF";
	while(prefix[len] && len < 64) { buf[len] = prefix[len]; len++; }
	buf[len++] = ' '; buf[len++] = '0'; buf[len++] = 'x';
	for(p = 7; p >= 0; p--) {
		buf[len++] = hex[(val >> (p * 4)) & 0xF];
	}
	buf[len] = '\0';
}

// Quick Hex-to-String loggers (routed / raw). Format once if EITHER sink is
// active, then route MS and UART independently.
void WriteDebugLogHex(const char* prefix, u32 val)
{
	char buf[128];
	if (!DBG_MS() && !DBG_UART()) return;   // skip the formatting too when both off
	build_hex_line(buf, prefix, val);
	if (DBG_MS())   ms_write_line(buf);
	if (DBG_UART()) uart_puts(buf);
}

void WriteDebugLogHexRaw(const char* prefix, u32 val)
{
	char buf[128];
	build_hex_line(buf, prefix, val);
	ms_write_line(buf);                     // raw: always to MS
	if (DBG_UART()) uart_puts(buf);
}

// Formatted always-write: same sinks as WriteDebugLogRaw, but formats the line here so
// callers don't need a separate sprintf (which would survive into a release binary —
// see pspstates.h). The whole call, format string included, compiles out when
// DEBUG_BUILD is 0.
void WriteDebugLogRawF(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ms_write_line(buf);
	if (DBG_UART()) uart_puts(buf);
}
#endif

// Look up a registered IO driver's LIVE device entry (&list->arg) by name, so the
// caller can call the driver's own df_init/df_exit IN PLACE — rebuilding the driver
// without ever removing it from iofilemgr's list (no sceIoDelDrv -> no do_deldrv, which
// redirects every open iob to a dead stub + frees the node = the historic Dissidia
// freeze on the game's open flash0: font handle). Lookup replicates ARK's
// sctrlHENFindDriver (core/systemctrl/src/sctrl_hen.c): the FIRST JAL inside sceIoDelDrv
// (NID 0x76DA16E3) targets iofilemgr's internal lookup_device_list(name); the returned
// node is { next; PspIoDrvArg entry }, so entry = node+1 and entry->drv (== d_dp)->funcs
// holds IoInit/IoExit. Returns the entry, or NULL if any rung fails. k1 must already be
// elevated by the caller (pspSdkSetK1(0)).
static PspIoDrvArg *find_io_driver_entry(const char *name)
{
	u32 deldrv, lookup = 0, a;
	deldrv = sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", 0x76DA16E3);
	if (!deldrv) return NULL;
	// findFirstJAL, bounded: scan forward for opcode 3 (JAL), decode the 26-bit index
	// into the current 256MB segment. lookup_device_list is the first call in
	// sceIoDelDrv; 0x100 bytes is well past it.
	for (a = deldrv; a < deldrv + 0x100; a += 4) {
		u32 insn = *(volatile u32 *)a;
		if ((insn >> 26) == 3) { lookup = ((insn & 0x03FFFFFF) << 2) | (a & 0xF0000000); break; }
	}
	if (!lookup) return NULL;
	{
		u32 *(*lookup_device_list)(const char *) = (u32 *(*)(const char *))lookup;
		u32 *node = lookup_device_list(name);
		if (!node) return NULL;
		return (PspIoDrvArg *)(node + 1);   // skip ->next; ->drv overlays d_dp
	}
}

// In-place df_exit then df_init on a driver entry (the AddDrv/DelDrv work, minus the
// do_deldrv handle-kill and node free). Split so the caller can exit BOTH drivers
// before init-ing BOTH, exactly mirroring the pre-v512 full-init order. NULL-safe.
static void io_driver_exit(PspIoDrvArg *e, const char *tag)
{
	if (e && e->drv && e->drv->funcs && e->drv->funcs->IoExit) {
		int r = e->drv->funcs->IoExit(e);
		uart_puts(tag); uart_log_hex(" df_exit=", (u32)r);
	}
}
static void io_driver_init(PspIoDrvArg *e, const char *tag)
{
	if (e && e->drv && e->drv->funcs && e->drv->funcs->IoInit) {
		int r = e->drv->funcs->IoInit(e);
		uart_puts(tag); uart_log_hex(" df_init=", (u32)r);
	}
}

// sub_00000A54 — sysevent handler.
// On the power-lock phase (fires DURING the firmware suspend sequence) we
// arm a +1s RTC alarm so the unit auto-wakes — same trick as Dark_Alex's
// v2. We do ONLY RTC calls here (no sceIo — the MS path isn't usable mid-
// suspend). On RESUME_COMPLETED we just set a flag; the poll thread logs
// it (so we never call sceIo from inside the sysevent handler).
int ProcessSignals(int ev_id, char *ev_name, void *param, int *result)
{
	// NOTE: do NOT log on every sysevent here. This handler fires for the
	// high-frequency suspend-handshake events (0x401 PHASE1 / 0x402), and even a
	// register-only uart_log_hex adds enough latency (its bounded TXFULL wait can
	// spin) to wedge the firmware's suspend handshake into a retry loop that never
	// descends to PHASE0_0 — observed as a 139x 0x401/0x402 loop then a CP27
	// save-suspend TIMEOUT (regressed v467's per-event [SIG] block; removed). Only
	// the one-shot [M9]/[M10] markers at the capture/handoff point are safe.
	(void)ev_name; (void)param; (void)result;
	if(ev_id == PSP_SYSEVENT_KERNEL_POWER_LOCK_PHASE1)
	{
		if(g_sleep_arm == 1)
		{
			u64 tick;
			sceRtcGetCurrentTick(&tick);
			// This is the only alarm arm in the plugin (SAVE and LOAD both use it);
			// it reliably wakes same-session ops. A cross-session load (power-off /
			// warm relaunch) needs a manual power press instead — see the mode-10
			// NOTE below on why there is no post-apply re-arm.
			// Auto-wake delay. The alarm must fire AFTER the syscon power-down, else
			// it goes past-due mid-descent and the wake relies on undefined past-due
			// behavior (historically a lost wake -> sleep until a manual press). +2s is
			// the SWEET SPOT: the ~2s firmware descent (incl. the 4MB PHASE0_0 capture)
			// is the real floor on the suspend's wall-clock, so the alarm fires right as
			// power-down completes — no wasted sleep, reliable wake. Measured: +3s=5.6s,
			// +2s=4.3s, +1s=4.4s (no gain below the descent, only added risk).
			sceRtcTickAddSeconds(&tick, &tick, 2);
			sceRtcSetAlarmTick(&tick);
			g_sleep_arm = 2; // armed
		}
	}
	else if(ev_id == g_cap_phase)
	{
		// Capture/apply phase (g_cap_phase, always PHASE0_0 — see lslibrary.c): the
		// last, fully-quiesced suspend event, and the point where mode 9/10 below
		// touch/overwrite kernel RAM (resume continues straight from RAM). Only
		// plain memory ops here (no sceIo). g_sleep_mode is only ever 0 (none),
		// 9 (SAVE capture), or 10 (LOAD apply).
		if(g_sleep_mode == 9)
		{
			// M4 SAVE: capture the kernel window into GAME RAM staging at the
			// quiesced point (game RAM is free — already written to MS). This
			// is read-only w.r.t. the kernel, so the caller (poll thread)
			// survives and continues after resume to drain it to MS + restore
			// game RAM. CACHED copy + dcache writeback to push it to RAM, like
			// V2's capture blob (c87abf6) — NOT an uncached-alias memcpy:
			// megabytes of uncached writes are very slow and stress the memory
			// controller mid-suspend (the game-dependent backlight-on freeze).
			// Post-resume the dcache is empty (power cycle), so the tail's
			// cached/DMA read fetches the snapshot from RAM regardless.
			// Drain DMAC before reading kernel memory. An in-flight PL080 transfer
			// to the same DRAM bank as a CPU read causes an infinite hardware stall
			// (no exception, watchdog reset). Spin until both controllers idle.
#if DEBUG_BUILD
			if (DBG_UART()) uart_puts("[M9] enter (SAVE kernel capture)");
#endif
			{
				volatile u32 *en0 = (volatile u32 *)(0xBC900000 + 0x1C);
				volatile u32 *en1 = (volatile u32 *)(0xBCA00000 + 0x1C);
				u32 spin = 0;
				while ((*en0 | *en1) && spin < 200000) spin++;
				// Residual channel-enable bits when the spin loop above stopped. 0 = drained;
				// nonzero means the spin timed out with DMA still live. The poll thread logs
				// this post-resume (see lslibrary.c).
				g_dma_drain_left = (*en0 | *en1);
			}
#if DEBUG_BUILD
			if (DBG_UART()) uart_log_hex("[M9] drain_left=", g_dma_drain_left);
#endif
			memcpy((void *)(g_stage_base + g_kcap_off),
			       (const void *)(KERNEL_RAM_BASE + g_kcap_off), g_kcap_size);
			sceKernelDcacheWritebackAll(); // flush the snapshot to RAM (survives power-down)
#if DEBUG_BUILD
			if (DBG_UART()) uart_puts("[M9] memcpy+wb done");
#endif
		}
		else if(g_sleep_mode == 10)
		{
			// M5 LOAD: overwrite the FULL kernel window with the save-time snapshot
			// staged in game RAM, by running ApplyHandoff FROM game RAM (it overwrites
			// this plugin's own code, so it can't run from the kernel). ApplyHandoff is
			// a ONE-WAY apply: it copies through the CACHED aliases (removing KSEG0/KSEG1
			// alias incoherence), writeback-invalidates D-cache + invalidates I-cache,
			// restores the SAVE-TIME dispatcher context (the g_handoff_ctx block
			// SysEventShim embedded in the snapshot) and jumps to its ra. Execution
			// continues inside the SAVE session's sysevent dispatch as if ITS handler
			// had just returned; this load session is abandoned. Returning here through
			// load-session code was the cross-session hang. The firmware suspend then
			// descends to power-down as the save-time system, whose poll thread
			// (captured mid-FreezeSave) finishes the save's tail after resume and
			// restores game RAM = the save point. (V2's load blob does the same; apply.S.)
			//
			// Drain the DMAC BEFORE touching the kernel (mirror the mode-9 drain):
			// writing the kernel bank while a PL080 transfer is in flight to the same
			// DRAM bank is an infinite hardware stall (no exception, watchdog reset).
#if DEBUG_BUILD
			if (DBG_UART()) uart_puts("[M10] enter (LOAD handoff)");
#endif
			{
				volatile u32 *en0 = (volatile u32 *)(0xBC900000 + 0x1C);
				volatile u32 *en1 = (volatile u32 *)(0xBCA00000 + 0x1C);
				u32 spin = 0;
				while ((*en0 | *en1) && spin < 200000) spin++;
			}
			{
				void (*handoff)(const void *, void *, unsigned int, const void *) =
					(void (*)(const void *, void *, unsigned int, const void *))(g_apply_base + KSEG1_ALIAS);
				g_sleep_mode = 0;          // this branch never returns; keep state consistent pre-copy
#if DEBUG_BUILD
				// Last C-side line before the PIC blob takes over; after this only the
				// blob's own B1/B2/B3 UART markers are emitted (this branch never returns).
				if (DBG_UART()) uart_log_hex("[M10] -> ApplyHandoff base=", g_apply_base + KSEG1_ALIAS);
#endif
				intr_suspend();            // mask; ApplyHandoff restores the SAVE-TIME Status at the jump
				handoff((const void *)(g_stage_base + g_kcap_off),      // cached KSEG0 src (staging)
				        (void *)(KERNEL_RAM_BASE + g_kcap_off),         // cached KSEG0 dst (kernel)
				        g_kcap_size,
				        (const void *)g_handoff_ctx_addr);              // save-time g_handoff_ctx
				/* not reached */
			}
		}
		g_sleep_mode = 0;
	}
	else if(ev_id == PSP_SYSEVENT_RESUME_COMPLETED)
	{
		state_flag = 0;
		// Clear the auto-wake alarm we armed for OUR suspend and disarm. Otherwise the
		// past-due alarm tick stays registered, so the next NATIVE (power-switch) sleep
		// sees an already-due alarm and wakes the console immediately. (Mirrors v2's
		// ClearAlarmTick after each op.) Harmless on a native resume (no alarm set).
		sceRtcSetAlarmTick(NULL);
		g_sleep_arm = 0;
		// Clear the PHASE0_0 action too. The SAVE snapshot is captured while the
		// mode-9 branch is still executing (before the handler's trailing
		// g_sleep_mode = 0), so every snapshot embeds g_sleep_mode == 9 — after a
		// LOAD the restored live system would otherwise keep mode 9 armed, and the
		// next NATIVE power-switch sleep would run the kernel capture at PHASE0_0
		// and clobber 8MB of live game RAM at g_stage_base. Same for the restored
		// handoff address: stale-nonzero would make a later DIAG mode-10 take the
		// handoff branch with a bogus context.
		g_sleep_mode = 0;
		g_handoff_ctx_addr = 0;
		// Flash repair on EVERY resume — BOTH save and load — in the RESUME_COMPLETED
		// callback, the same place PSPStates v2 and our pre-v512 builds ran it. Both
		// paths need it:
		//   - LOAD rolls the 8MB kernel back, reverting the flash drivers to the save
		//     session while the physical NAND moved on; writing flash1: on quit (the
		//     registry flush) through the stale state resets/corrupts the settings.
		//   - SAVE does not roll the kernel back, yet Pirates' game FREEZES (no sound,
		//     then hang — the kernel stays fine: Home-menu exit to XMB still works and
		//     settings survive) after a save unless the flash drivers are rebuilt here.
		//     This regressed when v512 removed the repair from the save path; the old
		//     notes record GTA/Pirates *tolerating* the full init, so it was load-bearing.
		// This is v520's key change: rebuild BOTH drivers, matching the pre-v512 full init
		// that Pirates tolerated. That full init did sceIoDelDrv/AddDrv on "lflash" AND
		// "flashfat"; v517-v519 rebuilt only lflash (in place) and merely re-mounted
		// flashfat's partitions — leaving flashfat's FS-driver state half-rebuilt, which
		// is what Pirates streams audio through -> it hangs on it. So now we run the
		// driver's OWN df_exit+df_init on BOTH lflash and flashfat, in the full-init order
		// (exit both, then init both), but WITHOUT sceIoDelDrv — its internal do_deldrv
		// redirects every open iob to a dead stub + frees the node, dangling the game's
		// open flash0: handle = the Dissidia freeze. In-place df_exit/df_init keeps the
		// device entries in iofilemgr's list, so open handles are not walked/killed.
		// Bracketed by Unassign/Assign of both partitions (df_umount top-down before the
		// driver teardown; df_mount bottom-up after). flash0: RDONLY, flash1: RDWR. No
		// op_mode gate (save and load alike). sceIo is alive here; on a save this precedes
		// the tail's MS snapshot write (msstor, not flash) so no interference.
		// HYPOTHESIS: rebuilding flashfat in place (its df_exit while flash0: is unmounted)
		// gives Pirates the driver rebuild it needs WITHOUT the do_deldrv that froze
		// Dissidia. Risk: flashfat's df_exit may still disturb the game's open flash0:
		// per-file state — if Dissidia refreezes, that is the cause and flashfat needs a
		// gentler refresh (or the game's flash0: FD closed first).
		{
			int r;
			u32 k1;
			PspIoDrvArg *lf, *ff;
			// Flash repair on resume. The unassign/re-assign of flash0:/flash1: (the flashfat
			// RE-MOUNT) leaks ~22-30KB of kernel RAM per cycle: each flashfat mount allocates a
			// FAT cache that df_umount does NOT free (measured: the whole leak is the assign;
			// unassign + df_exit/df_init rebuild == 0). A SAVE never rolls the kernel back, so
			// flash0:/flash1: are already correctly mounted — it needs only the DRIVER rebuild
			// (df_exit/df_init in place, which Pirates depends on), NOT the re-mount. So gate the
			// leaky unassign/re-assign to LOAD (g_lc.op_mode == 1), where the kernel rollback
			// reverts the flash drivers and a fresh mount is genuinely required. The in-place
			// df_exit/df_init keeps the driver ENTRY, so running it on save with the mounts still
			// active leaves the entry the mounts reference intact.
			// TEST STATUS: this gate's Dissidia/Pirates SAVE safety is UNCONFIRMED — the panel-
			// fill overflow that crashed every save masked it. Verify now that the overflow is
			// fixed; if Dissidia freezes after save, the unmount bracket was load-bearing ->
			// revert to the full v520 repair (unassign/assign unconditionally).
			int is_load = (g_lc.op_mode == 1);
			uart_log_hex("[FLASH1] repair (is_load)=", (u32)is_load);
			if (is_load) {
				r = sceIoUnassign("flash0:");
				uart_log_hex("[FLASH1] unassign flash0=", (u32)r);
				r = sceIoUnassign("flash1:");
				uart_log_hex("[FLASH1] unassign flash1=", (u32)r);
			}
			k1 = pspSdkSetK1(0);
			lf = find_io_driver_entry("lflash");
			ff = find_io_driver_entry("flashfat");
			uart_log_hex("[LFL] lflash entry=", (u32)lf);
			uart_log_hex("[LFL] flashfat entry=", (u32)ff);
			io_driver_exit(lf, "[LFL] lflash");     // driver rebuild (both ops): in-place df_exit
			io_driver_exit(ff, "[LFL] flashfat");   //   then df_init, so the driver ENTRY stays
			io_driver_init(lf, "[LFL] lflash");     //   (no do_deldrv handle-kill).
			io_driver_init(ff, "[LFL] flashfat");
			pspSdkSetK1(k1);
			if (is_load) {
				r = sceIoAssign("flash0:", "lflash0:0,0", "flashfat0:", IOASSIGN_RDONLY, NULL, 0);
				uart_log_hex("[FLASH1] assign flash0=", (u32)r);
				r = sceIoAssign("flash1:", "lflash0:0,1", "flashfat1:", IOASSIGN_RDWR, NULL, 0);
				uart_log_hex("[FLASH1] assign flash1=", (u32)r);
			}
		}
		g_resumed = 1; // wait_for_resume() polls this to detect the firmware resume
	}

	return 0;
}


