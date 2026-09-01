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
#include "corevolt.h"        /* cv_probe/cv_boot_apply/cv_revert — core voltage rides with the clock */

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

// Ramp the clock-domain ratio registers to their targets in fixed steps: the CPU
// domain to its 1:1 ceiling, the BUS domain to whatever gear realises its requested
// absolute MHz (or, for target 0, sync — 1:1). See overclock.h for the gear/MHz
// solver. The ME domain (0xBC200008) is deliberately NOT touched: the plugin never
// used to write it, so the ME keeps the firmware's boot fraction.
//
// The two domains are genuinely independent: the firmware programs them from two
// separate (num, den) pairs via sceClkcSetCpuGear / sceClkcSetBusGear (scePower
// @0x880E282C), which are exactly these two registers. Pinning both to 1:1 was
// inherited from the psp-beyond-444mhz reference, not a hardware requirement.
// [0] = 0 = sync (gear 1:1, bus scales with the PLL). The rest are absolute targets.
const short g_oc_bus_tab[OC_BUS_TARGETS] = { 0, 111, 133, 166, 190, 222, 266 };
int g_oc_bus_sel = 0;             // index into g_oc_bus_tab; 0 = the historic behaviour
u32 g_oc_cpu_num = 0, g_oc_cpu_den = 0;   // gears READ BACK after the ramp
u32 g_oc_bus_num = 0, g_oc_bus_den = 0;
u32 g_oc_bus_want = 0;                    // what we asked for; differs => hardware refused

// One field, one tick, toward its own target — in EITHER direction. The bus ratio
// has to be able to come down, which the old increment-only ramp could not do.
static u32 oc_ramp1(u32 v, u32 target, u32 step)
{
	if (v < target) { v += step; if (v > target) v = target; }
	else if (v > target) { v = (v < target + step) ? target : v - step; }
	return v;
}

// busMhz = 0 leaves the bus gear at 1:1; otherwise it is solved against the PLL
// frequency this step is about to run at (freqX10 = MHz * 10).
static void oc_adjust_domain_ratios(int busMhz, int freqX10)
{
	unsigned int intr; int ds;
	u32 cpu, bus, cpuDen, cpuNum, busDen, busNum;
	const u32 tCpuNum = 0x1ff, tCpuDen = 0x1ff, tBusDen = 0x1ff;
	u32 tBusNum;
	const u32 step = 18;
	int guard;

	if (busMhz <= 0 || freqX10 <= 0) {
		tBusNum = 0x1ff;                       // sync: bus rides the PLL, as before
	} else {
		// num = 511 * 2 * busMhz / pllMhz, with pllMhz = freqX10/10 folded in and
		// half-ulp rounding: (10220 * busMhz + freqX10/2) / freqX10.
		int n = (10220 * busMhz + freqX10 / 2) / freqX10;
		if (n < 1)     n = 1;
		if (n > 0x1ff) n = 0x1ff;              // asking for more than sync just gives sync
		tBusNum = (u32)n;
	}

	ds = sceKernelSuspendDispatchThread();
	intr = sceKernelCpuSuspendIntr();

	cpu = OC_HW(0xbc200000); bus = OC_HW(0xBC200004);
	OC_SYNC();
	cpuDen = cpu & 0x1ff; cpuNum = (cpu >> 16) & 0x1ff;
	busDen = bus & 0x1ff; busNum = (bus >> 16) & 0x1ff;

	OC_HW(0xbc200000) = (cpuNum << 16) | cpuDen;
	OC_HW(0xBC200004) = (busNum << 16) | busDen;
	oc_settle();

	// Walk each field toward ITS OWN target. The old condition was
	// "while ((cpuNum & cpuDen & busNum & busDen) != 0x1ff)", which never
	// terminates once a target sits below 0x1ff — and it spins with interrupts
	// off, so `guard` caps it regardless (max distance 0x1ff / step 18 = 29).
	for (guard = 0; guard < 64; guard++) {
		if (cpuNum == tCpuNum && cpuDen == tCpuDen &&
		    busNum == tBusNum && busDen == tBusDen) break;
		cpuNum = oc_ramp1(cpuNum, tCpuNum, step);
		cpuDen = oc_ramp1(cpuDen, tCpuDen, step);
		busNum = oc_ramp1(busNum, tBusNum, step);
		busDen = oc_ramp1(busDen, tBusDen, step);
		OC_HW(0xbc200000) = (cpuNum << 16) | cpuDen;
		OC_HW(0xBC200004) = (busNum << 16) | busDen;
		oc_settle();
	}

	// Read the registers BACK rather than recording what we meant to write. The
	// reference implementation only ever ramps both domains to 1:1, so an arbitrary
	// bus ratio is untested territory — the hardware may clamp it, ignore it, or
	// require num == den. Logging the intent would have hidden that.
	cpu = OC_HW(0xbc200000); bus = OC_HW(0xBC200004);
	OC_SYNC();
	g_oc_cpu_num = (cpu >> 16) & 0x1ff; g_oc_cpu_den = cpu & 0x1ff;
	g_oc_bus_num = (bus >> 16) & 0x1ff; g_oc_bus_den = bus & 0x1ff;
	g_oc_bus_want = tBusNum;

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

// Resolve one getter's backing address (read-only — no writes happen here).
// Shared by oc_probe_hud_getters below.
static u32 oc_resolve_hud_addr(u32 nid)
{
	u32 fn = sctrlHENFindFunction("scePower_Service", "scePower_driver", nid);
	return fn ? oc_decode_getter_addr(fn) : 0;
}

// DO NOT add sceClkcGetCpuGear / sceClkcGetBusGear here as int (*)(void). v975 did
// exactly that and crashed the console at boot: a "gear" is a (num, den) PAIR, so
// those functions return nothing and write through OUTPUT POINTERS. Called with no
// arguments they stored to whatever junk was in a1 —
//   cause=Address store  badvaddr=000000b2  a1=000000b2
//   EPC in sceLowIO_Driver +0x2b10, RA in this module
// with a0 holding 0x01ff01ff, the gear register it had just read and was about to
// write out. Nothing here needs them anyway: oc_adjust_domain_ratios reads
// 0xBC200000/4/8 directly, which is the hardware rather than a driver's copy.
//
// The two *Frequency getters above are safe because a frequency is a single int, so
// their (void) signature is consistent with the name. That difference is the whole
// lesson: infer the signature from what the function has to return, and if it could
// be an out-param, do not call it blind.

static void oc_probe_hud_getters(void)
{
	g_hud_cpu_freq_addr = oc_resolve_hud_addr(0xFDB5BFE9);
	g_hud_bus_freq_addr = oc_resolve_hud_addr(0xBD681969);
}

// Write the PLL multiplier for step `id` (0..OC_STEPS-1, 0 = stock 333MHz). Called
// when the user has confirmed it, live from the Settings menu, or from utils.c's
// ProcessSignals on RESUME_COMPLETED (hence non-static — see pspfatsave.h).
void oc_apply(int id)
{
	int ds; unsigned int intr; u32 mul; int mhz10;

	if (id < 0 || id >= OC_STEPS) id = 0;

	oc_adjust_domain_ratios(g_oc_bus_tab[(g_oc_bus_sel >= 0 && g_oc_bus_sel < OC_BUS_TARGETS) ? g_oc_bus_sel : 0],
	                        g_oc_freq_x10[id]);

	ds = sceKernelSuspendDispatchThread();
	intr = sceKernelCpuSuspendIntr();

	OC_HW(0xbc100068) = 0x85;
	OC_SYNC();
	oc_pll_ready();
	oc_settle();

	mul = g_oc_multipliers[id];
	OC_HW(0xbc1000fc) = (OC_HW(0xbc1000fc) & 0xffff0000) | (mul << 8) | OC_DEN;
	OC_SYNC();
	oc_settle();

	sceKernelCpuResumeIntr(intr);
	sceKernelResumeDispatchThread(ds);

	sceKernelDelayThread(100);

	mhz10 = g_oc_freq_x10[id];
	if (g_hud_cpu_freq_addr) *((volatile u32 *)g_hud_cpu_freq_addr) = (u32)((mhz10 + 5) / 10);
	if (g_hud_bus_freq_addr) *((volatile u32 *)g_hud_bus_freq_addr) = (u32)(((mhz10 + 5) / 10) / 2);
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
	if (g_overclock_id > 0) oc_apply(0);
	// NO syscon calls here (cv_revert / cv_ddr_revert / cv_rails_revert were here
	// once). Calling sceSysconCtrlVoltage from the patched-ExitGame syscall context
	// crashed inside sceSYSCON_Driver (Address store, badvaddr=deadbf27, the driver
	// walking a poisoned 0xdeadbeef packet pointer, repeating in the exception
	// handler) - this is not the thread context those transactions require. Nothing
	// is lost by skipping them: the Tachyon rail is reprogrammed from the fuse by
	// the IPL/scePower on every boot, and the DDR/unknown rails are re-baselined by
	// Rail Defaults at the next plugin start (cv_rails_boot_init) - the Pommel keeps
	// its registers across exit. Hypothesis-level on the exact mechanism; the crash
	// signature is from one observed exit, not a reproduced series.
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
	if (g_overclock_id > 0) oc_apply(0);
	// No syscon calls here either - see the comment in oc_exit_game_patched.
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
	// NOTE: with ARK-4's "overclock" setting enabled this call does NOTHING.
	// ARK applies 333/166 itself and then overwrites scePowerSetClockFrequency (and
	// five other clock setters) with "jr $ra; li $v0,0" — see ARK-4
	// core/systemctrl/src/cpuclock.c SetSpeed(). Harmless, since ARK has already put
	// the hardware exactly where this call would: it is kept for the case where that
	// setting is off. The same stubbing is why scePower can never reprogram the core
	// voltage behind us, so cv_apply's syscon write is the only thing that matters.
	scePowerSetClockFrequency(333, 333, 166);
	// AFTER the baseline call: that is what programs the stock (333MHz) core-voltage
	// code, so the probe reads a settled state. Probe ONLY — a persisted voltage step
	// is not applied here. It rides the overclock's boot decision instead (see
	// boot_frozen_prompts / the STABLE branch in menu_thread), so an overvolt can
	// never come back silently across a reboot.
	cv_probe();
	oc_probe_hud_getters();
	oc_install_exit_hook();
}
