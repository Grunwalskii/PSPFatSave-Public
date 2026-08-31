#include "pspfatsave.h"
#include <stdarg.h>   // WriteDebugLogRawF (DEBUG only)

// Event handler: routes through SysEventShim (extras.S) then ProcessSignals.
PspSysEventHandler events =
{
	sizeof(PspSysEventHandler),
	"ProcessSignals",
	0x00FFFF00,
	SysEventShim,
}; // 0x2EF0

// extern
extern int state_flag;			// 0x2F50
// Sleep-hybrid flags (defined in fatsave.c).
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

// Drain PL080 DMA channels; returns 0 if fully drained, nonzero if timeout.
static u32 dmac_drain_wait(void)
{
	volatile u32 *en0 = (volatile u32 *)(0xBC900000 + 0x1C);
	volatile u32 *en1 = (volatile u32 *)(0xBCA00000 + 0x1C);
	u32 spin = 0;
	while ((*en0 | *en1) && spin < 200000) spin++;
	return (*en0 | *en1);
}

#if DEBUG_BUILD
// Write one line to debug file only if DBG_MS() is on; UART handled by callers.
static void ms_write_line(const char* msg)
{
	SceUID fd;
	if (!DBG_MS()) return;
	fd = sceIoOpen("ms0:/pspstates_debug.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0) {
		sceIoWrite(fd, msg, strlen(msg));
		sceIoWrite(fd, "\n", 1);
		sceIoClose(fd);
	}
}

// Version banner + failure lines; mirrors to UART if DBG_UART().
void WriteDebugLogRaw(const char* msg)
{
	ms_write_line(msg);
	if (DBG_UART()) uart_puts(msg);
}

// MS log if DBG_MS(); UART if DBG_UART().
void WriteDebugLog(const char* msg)
{
	if (DBG_MS())   ms_write_line(msg);
	if (DBG_UART()) uart_puts(msg);
}

// Build "<prefix> 0xXXXXXXXX" format line.
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

// Format once, route to MS/UART independently.
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
	ms_write_line(buf);                     // MS write gated inside ms_write_line() (DBG_MS())
	if (DBG_UART()) uart_puts(buf);
}

// Formatted variant; compiles out when DEBUG_BUILD is 0.
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
#else
// RELEASE-ONLY vsnprintf stub. Not code we run — a link-order fix: in a debug
// build WriteDebugLogRawF (above) references vsnprintf, so the linker pulls
// libc's vsnprintf.o when it scans -lc (early on the link line). In a release
// build ALL our vsnprintf references compile out, so -lc loads nothing — and
// later, -lpspdebug (appended by build_prx.mak) drags in libpspdebug's
// pspDebugScreenPrintf.o via the never-executed screen fallback inside
// exception.o (see the Makefile's -lpspge note), which wants vsnprintf after
// -lc has already been scanned -> "undefined reference to vsnprintf". The only
// consumer of this stub is that dead SDK path; if it ever DID run it would
// just format an empty string (returns 0 = "nothing written"), never crash.
int vsnprintf(char *s, size_t n, const char *fmt, __builtin_va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, __builtin_va_list ap)
{
	(void)s; (void)n; (void)fmt; (void)ap;
	return 0;
}
#endif

// Find driver entry by name; allows df_init/df_exit in-place without sceIoDelDrv.
// k1 must be elevated by caller.
static PspIoDrvArg *find_io_driver_entry(const char *name)
{
	u32 deldrv, lookup = 0, a;
	deldrv = sctrlHENFindFunction("sceIOFileManager", "IoFileMgrForKernel", 0x76DA16E3);
	if (!deldrv) return NULL;
	// Scan for first JAL opcode to find lookup_device_list.
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

// Call driver's df_exit in-place. NULL-safe.
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

// System event handler: arm RTC alarm on POWER_LOCK; handle RESUME_COMPLETED cleanup.
int ProcessSignals(int ev_id, char *ev_name, void *param, int *result)
{
	// Do not log on every sysevent; only the [M9]/[M10] markers are written here.
	(void)ev_name; (void)param; (void)result;
	// Screen-tuning power coordination — stand the gamma pass down during the
	// suspend-descent phases ((ev_id & 0xFF00) == 0x4000); see screen_tuning.c.
	if((ev_id & 0xFF00) == 0x4000)
		st_pwr_suspend();
	if(ev_id == PSP_SYSEVENT_KERNEL_POWER_LOCK_PHASE1)
	{
		if(g_sleep_arm == 1)
		{
			u64 tick;
			sceRtcGetCurrentTick(&tick);
			// Auto-wake delay. RTC tick unit is 1us, so +100ms = +100000 ticks.
			tick += 100000ULL;   // 100 ms
			sceRtcSetAlarmTick(&tick);
			g_sleep_arm = 2; // armed
		}
	}
	else if(ev_id == g_cap_phase)
	{
		// Capture/apply phase (PHASE0_0); mode 9=SAVE, 10=LOAD. No sceIo here.
		if(g_sleep_mode == 9)
		{
			// Capture kernel to game-RAM staging (cached, not uncached-alias).
#if DEBUG_BUILD
			if (DBG_UART()) uart_puts("[M9] enter (SAVE kernel capture)");
#endif
			g_dma_drain_left = dmac_drain_wait();
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
			// Apply snapshot via ApplyHandoff blob (runs from game RAM); restores
			// SAVE-TIME dispatcher context and jumps; LOAD session abandoned.
#if DEBUG_BUILD
			if (DBG_UART()) uart_puts("[M10] enter (LOAD handoff)");
#endif
			dmac_drain_wait();
			{
				void (*handoff)(const void *, void *, unsigned int, const void *) =
					(void (*)(const void *, void *, unsigned int, const void *))(g_apply_base + KSEG1_ALIAS);
				g_sleep_mode = 0;          // this branch never returns; keep state consistent pre-copy
#if DEBUG_BUILD
				// Last C-side line; blob emits B1/B2/B3 markers. Branch never returns.
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
		// Arm the delayed gamma re-enable (see st_pwr_resume).
		st_pwr_resume();
		// Clear alarm; prevents immediate wake on next native sleep.
		sceRtcSetAlarmTick(NULL);
		g_sleep_arm = 0;
		// Clear mode/handoff.
		g_sleep_mode = 0;
		g_handoff_ctx_addr = 0;
		// Rebuild flash drivers on LOAD (in-place df_exit/df_init).
		{
			int r;
			u32 k1;
			PspIoDrvArg *lf, *ff;
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
			io_driver_exit(lf, "[LFL] lflash");     // in-place df_exit/df_init so the
			io_driver_exit(ff, "[LFL] flashfat");   // driver ENTRY stays (no handle-kill).
			io_driver_init(lf, "[LFL] lflash");
			io_driver_init(ff, "[LFL] flashfat");
			pspSdkSetK1(k1);
			if (is_load) {
				r = sceIoAssign("flash0:", "lflash0:0,0", "flashfat0:", IOASSIGN_RDONLY, NULL, 0);
				uart_log_hex("[FLASH1] assign flash0=", (u32)r);
				r = sceIoAssign("flash1:", "lflash0:0,1", "flashfat1:", IOASSIGN_RDWR, NULL, 0);
				uart_log_hex("[FLASH1] assign flash1=", (u32)r);
			}
		}
		// Re-apply overclock after resume.
		if (g_overclock_id > 0) oc_apply(g_overclock_id);
		g_resumed = 1; // wait_for_resume() polls this to detect the firmware resume
	}

	return 0;
}


