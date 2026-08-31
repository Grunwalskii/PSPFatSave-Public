#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"
#include "screen_tuning.h"   /* st_stop — this file owns the only game-exit hook */
#include "cheats.h"          /* cheats_stop — same game-exit teardown */

// ── Overclock (raw PLL register control, PSP-1000 only) ──────────────────────
// Frequency model: MHz = 37 * num / den (37 = PLL base). num is the 8-bit field in the
// 0xbc1000fc multiplier register (see oc_apply: (num << 8) | OC_DEN), range 0..0xff.
// OC_DEN = 17 (den) is shared by all steps. The 25 steps are num 0x99..0xff.
const u32 g_oc_multipliers[OC_STEPS] = {
	0x99, 0x9d, 0xa1, 0xa5, 0xaa, 0xae, 0xb2, 0xb6, 0xbb, 0xbf, 0xc3, 0xc7, 0xcc,
	0xd0, 0xd4, 0xd8, 0xdd, 0xe1, 0xe5, 0xe9, 0xee, 0xf2, 0xf6, 0xfa, 0xff,
};
const int g_oc_freq_x10[OC_STEPS] = {   // MHz x10 per step = 37 * num / OC_DEN
	3330, 3417, 3504, 3591, 3700, 3787, 3874, 3961, 4070, 4157, 4244, 4331, 4440,
	4527, 4614, 4701, 4810, 4897, 4984, 5071, 5180, 5267, 5354, 5441, 5550,
};
#define OC_DEN 0x11   // PLL denominator (17), shared by all steps — see the model above

#define OC_HW(addr)  (*((volatile u32*)(addr)))
#define OC_SYNC()    asm volatile("sync\n")
#define OC_NOP7()    asm volatile("nop;nop;nop;nop;nop;nop;nop\n")

// Fixed-cycle settle delay for PLL/clock-domain stability.
static void oc_settle(void)
{
	asm volatile(
		".set push              \n"
		".set noreorder         \n"
		".set nomacro           \n"
		".set volatile          \n"
		".set noat              \n"
		"sync                   \n"
		"lui  $t0, 0x02         \n"
		"ori  $t0, $t0, 0xffff  \n"
		"1:                     \n"
		"  nop                  \n"
		"  nop                  \n"
		"  nop                  \n"
		"  nop                  \n"
		"  nop                  \n"
		"  nop                  \n"
		"  nop                  \n"
		"  addiu $t0, $t0, -1   \n"
		"  bnez  $t0, 1b        \n"
		"  nop                  \n"
		".set pop               \n"
		: : : "t0", "memory"
	);
}

static void oc_pll_ready(void)
{
	do { OC_NOP7(); } while (OC_HW(0xbc100068) & 0x80);
	OC_SYNC();
}

// One-time register unlock (write -1 across 0xbc000000..0xbc00002c); needed before
// the PLL/domain registers below take writes.
static void oc_unlock_regs(void)
{
	u32 reg;
	for (reg = 0xbc000000; reg <= 0xbc00002c; reg += 4) OC_HW(reg) = (u32)-1;
	OC_SYNC();
}

// Ramp the clock-domain ratio registers (0xbc200000/0xBC200004) up to 511:511 (their
// 1:1 ceiling) in fixed steps.
static void oc_adjust_domain_ratios(void)
{
	unsigned int intr; int ds;
	u32 cpu, bus, cpuDen, cpuNum, busDen, busNum;
	const int step = 18;

	ds = sceKernelSuspendDispatchThread();
	intr = sceKernelCpuSuspendIntr();

	cpu = OC_HW(0xbc200000); bus = OC_HW(0xBC200004);
	OC_SYNC();
	cpuDen = cpu & 0x1ff; cpuNum = (cpu >> 16) & 0x1ff;
	busDen = bus & 0x1ff; busNum = (bus >> 16) & 0x1ff;

	OC_HW(0xbc200000) = (cpuNum << 16) | cpuDen;
	OC_HW(0xBC200004) = (busNum << 16) | busDen;
	oc_settle();

	while ((cpuNum & cpuDen & busNum & busDen) != 0x1ff) {
		u32 nCpuNum = cpuNum + step, nCpuDen = cpuDen + step;
		u32 nBusNum = busNum + step, nBusDen = busDen + step;
		cpuNum = (nCpuNum > 0x1ff) ? 0x1ff : nCpuNum;
		cpuDen = (nCpuDen > 0x1ff) ? 0x1ff : nCpuDen;
		busNum = (nBusNum > 0x1ff) ? 0x1ff : nBusNum;
		busDen = (nBusDen > 0x1ff) ? 0x1ff : nBusDen;
		OC_HW(0xbc200000) = (cpuNum << 16) | cpuDen;
		OC_HW(0xBC200004) = (busNum << 16) | busDen;
		oc_settle();
	}

	sceKernelCpuResumeIntr(intr);
	sceKernelResumeDispatchThread(ds);
}

// ── Externally-visible CPU frequency: direct write to the getter's backing var ──
// The CPU/bus frequency getters return a private RAM variable inside scePower_driver,
// not a hardware register. We find that variable by decoding the getter's compiled
// instructions for the MIPS "return a stored global" idiom (lui rt,HI; lw rt,LO(rt);
// jr $ra) and write the frequency there. Addresses stay 0 if the pattern isn't found,
// so the write is skipped rather than guessed.
static u32 g_hud_cpu_freq_addr = 0;
static u32 g_hud_bus_freq_addr = 0;

// Scan up to 6 words from `fn` for "lui rt,HI" followed (not necessarily
// immediately) by "lw rt,LO(rt)" on the SAME register, and return HI<<16 + LO.
// Returns 0 if the pattern isn't found in range.
static u32 oc_decode_getter_addr(u32 fn)
{
	u32 i, hi = 0; int hi_reg = -1;
	for (i = 0; i < 6; i++) {
		u32 insn = *((volatile u32 *)(fn + i * 4));
		u32 op = insn >> 26;
		if (op == 0x0F) {                        // LUI rt, imm
			hi_reg = (int)((insn >> 16) & 0x1F);
			hi = (insn & 0xFFFF) << 16;
		} else if (op == 0x23 && hi_reg >= 0) {   // LW rt, imm(base)
			int base = (int)((insn >> 21) & 0x1F);
			if (base == hi_reg) {
				s16 lo = (s16)(insn & 0xFFFF);
				return hi + (u32)(s32)lo;
			}
		}
	}
	return 0;
}

// Resolve one getter's backing address (read-only — no writes happen here) and
// log what it found when UART logging is on, so a failed decode is visible
// instead of silently doing nothing. Shared by oc_probe_hud_getters below.
static u32 oc_resolve_hud_addr(u32 nid, const char *label)
{
	u32 fn = sctrlHENFindFunction("scePower_Service", "scePower_driver", nid);
	u32 addr = fn ? oc_decode_getter_addr(fn) : 0;
	if (DBG_UART()) {
		if (addr) { char b[48]; sprintf(b, "[OC] HUD %s var @", label); uart_log_hex(b, addr); }
		else      { char b[48]; sprintf(b, "[OC] HUD %s var: not resolved", label); uart_puts(b); }
	}
	return addr;
}

static void oc_probe_hud_getters(void)
{
	g_hud_cpu_freq_addr = oc_resolve_hud_addr(0xFDB5BFE9, "cpu-freq");
	g_hud_bus_freq_addr = oc_resolve_hud_addr(0xBD681969, "bus-freq");
}

// Write the PLL multiplier for step `id` (0..OC_STEPS-1, 0 = stock 333MHz). Called
// when the user has confirmed it, live from the Settings menu, or from utils.c's
// ProcessSignals on RESUME_COMPLETED (hence non-static — see pspfatsave.h).
void oc_apply(int id)
{
	int ds; unsigned int intr; u32 mul; int mhz10;

	if (id < 0 || id >= OC_STEPS) id = 0;

	// UART checkpoints (unconditional, register-only) around each step.
	uart_puts("[OC1] enter");
	oc_adjust_domain_ratios();
	uart_puts("[OC2] ratios done");

	ds = sceKernelSuspendDispatchThread();
	intr = sceKernelCpuSuspendIntr();
	uart_puts("[OC3] dispatch+intr off");

	OC_HW(0xbc100068) = 0x85;
	OC_SYNC();
	uart_puts("[OC4] ctrl=0x85 written");
	oc_pll_ready();
	uart_puts("[OC5] pll ready");
	oc_settle();

	mul = g_oc_multipliers[id];
	OC_HW(0xbc1000fc) = (OC_HW(0xbc1000fc) & 0xffff0000) | (mul << 8) | OC_DEN;
	OC_SYNC();
	uart_puts("[OC6] multiplier written");
	oc_settle();

	sceKernelCpuResumeIntr(intr);
	sceKernelResumeDispatchThread(ds);
	uart_puts("[OC7] dispatch+intr on");

	sceKernelDelayThread(100);

	mhz10 = g_oc_freq_x10[id];
	if (g_hud_cpu_freq_addr) *((volatile u32 *)g_hud_cpu_freq_addr) = (u32)((mhz10 + 5) / 10);
	if (g_hud_bus_freq_addr) *((volatile u32 *)g_hud_bus_freq_addr) = (u32)(((mhz10 + 5) / 10) / 2);

	if (DBG_UART()) {
		char buf[64];
		sprintf(buf, "[OC] applied step %d -> %d.%dMHz", id, mhz10 / 10, mhz10 % 10);
		WriteDebugLog(buf);
	}
}

// ── Revert to stock on game exit ──────────────────────────────────────────
// The overclock otherwise sticks past game exit, so hook sceKernelExitGame /
// sceKernelExitGameWithStatus (sceLoadExec LoadExecForUser, NIDs 0x05572A5F /
// 0x2AC9954B) and revert to stock (oc_apply(0)) before the game exits. Only
// touches the registers if a non-stock step was applied (g_overclock_id > 0).
static void (*g_real_exit_game)(void) = NULL;
static int (*g_real_exit_game_status)(int) = NULL;

// These two are the plugin's ONLY game-exit hook, so anything that must be torn
// down before the game goes away hangs off them. st_stop() is called here
// (unconditionally) rather than installing its own patch, since a second
// sctrlHENPatchSyscall on the same NIDs would replace this entry.
static void oc_exit_game_patched(void)
{
	cheats_stop();   // stop the FPS apply thread before game RAM is torn down
	st_stop();
	uart_puts("[OC0] st_stop done");   // confirms we reached the revert step
	if (g_overclock_id > 0) oc_apply(0);
	uart_puts("[OC] exit -> real");
	// Drain the GE to idle before the real exit call.
	{ int i; for (i = 0; i < 40; i++) {
		if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) break;
		sceKernelDelayThread(200);
	}}
	if (g_real_exit_game) g_real_exit_game();
}

static int oc_exit_game_status_patched(int status)
{
	cheats_stop();   // stop the FPS apply thread before game RAM is torn down
	st_stop();
	uart_puts("[OC0] st_stop done");   // confirms we reached the revert step
	if (g_overclock_id > 0) oc_apply(0);
	uart_puts("[OC] exitstatus -> real");   // see oc_exit_game_patched
	{ int i; for (i = 0; i < 40; i++) {
		if (sceGeDrawSync(1) == PSP_GE_LIST_DONE) break;
		sceKernelDelayThread(200);
	}}
	return g_real_exit_game_status ? g_real_exit_game_status(status) : 0;
}

static void oc_install_exit_hook(void)
{
	static int installed = 0;
	if (installed) return;
	installed = 1;

	g_real_exit_game = (void (*)(void))sctrlHENFindFunction("sceLoadExec", "LoadExecForUser", 0x05572A5F);
	if (g_real_exit_game) sctrlHENPatchSyscall((void *)g_real_exit_game, oc_exit_game_patched);

	g_real_exit_game_status = (int (*)(int))sctrlHENFindFunction("sceLoadExec", "LoadExecForUser", 0x2AC9954B);
	if (g_real_exit_game_status) sctrlHENPatchSyscall((void *)g_real_exit_game_status, oc_exit_game_status_patched);
}
// Boot-time entry: baseline to stock 333MHz via the firmware call, unlock the PLL
// registers, resolve the externally-visible frequency addresses, and install the
// exit-time revert hook. Does NOT apply a persisted non-stock step — see
// boot_frozen_prompts, called from menu_thread on first wake.
void oc_init(void)
{
	sceKernelIcacheInvalidateAll();
	oc_unlock_regs();
	scePowerSetClockFrequency(333, 333, 166);
	oc_probe_hud_getters();
	oc_install_exit_hook();
}
