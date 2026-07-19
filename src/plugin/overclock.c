#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// ── Overclock (raw PLL register control, PSP-1000 only) ──────────────────────
// Ported from the "psp-beyond-444mhz" project (m-c/d, MIT license). These PLL/clock-
// domain registers are undocumented in any official SDK or in PSP_References (uofw/
// ARK-4) — this is third-party reverse-engineered register knowledge taken as-is from
// that source, NOT verified against our own reference material (see CLAUDE.md's PSP1000
// hardware-facts rule). Each step's frequency (g_oc_freq_x10, MHz x10) is that source's
// own claim, not independently measured here. NOT special-cased around save/load: per
// the chosen design, whatever step is selected stays active straight through the
// firmware-suspend freeze — that interaction is UNTESTED, since this plugin's suspend/
// resume timing has so far only ever been exercised at stock clock.
//
// FREQUENCY MODEL: MHz = 37 * num / den (37 = PLL base). num is an 8-bit field in the
// 0xbc1000fc multiplier register (see oc_apply: (num << 8) | OC_DEN), so num maxes at
// 0xff = 255. The picker variant of the source used den = 18, which caps at 255/18*37 =
// 524MHz. To reach 555 we use the EXPERIMENTAL variant's den = 17 instead (overclock.h
// MAX_THEORETICAL comment "//555"), where 255/17*37 = 555.0 exactly. den is shared, so
// every step's frequency is re-derived at den=17.
//
// GRANULARITY: the integer landmarks 333/370/407/444/481/518/555 fall on num = 0x99..
// 0xff spaced +17 (each +37MHz). 17 is prime, so no uniform sub-step divides it - to keep
// those landmarks EXACT while roughly quartering the gap (user asked for ~8.7MHz steps),
// each 37MHz interval is split +4,+4,+4,+5. That gives 25 steps ~8.7MHz apart (one num
// unit = 37/17 = 2.18MHz is the hardware floor; this is 4x that). The landmark rows land
// on whole MHz; the three sub-steps between are x.1/x.4/x.7-ish.
//
// 555MHz is OUT OF SPEC and, with no SoC-voltage bump available in any of our sources,
// may simply not be stable on a given unit - the top steps are where to expect it. The
// finer steps are here precisely to let the exact stability ceiling be dialled in.
// (g_overclock_id is a persisted INDEX; adding steps only ever maps an old index to a
// LOWER frequency than before, never higher, so a stale setting can't jump the clock up.)
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

// Duplicate prototypes are harmless if pspintrman.h already declared them (same
// pattern as the me_rpc_probe block above) — kept local so this section doesn't
// depend on include order.
unsigned int sceKernelCpuSuspendIntr(void);
void sceKernelCpuResumeIntr(unsigned int flags);

// Fixed-cycle settle delay, ported verbatim from the source's settle() asm (timed
// empirically there for PLL/clock-domain stability, not derived from a spec).
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

// One-time register unlock (write -1 across 0xbc000000..0xbc00002c) — matches the
// source's unlockMemory(); needed before the PLL/domain registers below take writes.
static void oc_unlock_regs(void)
{
	u32 reg;
	for (reg = 0xbc000000; reg <= 0xbc00002c; reg += 4) OC_HW(reg) = (u32)-1;
	OC_SYNC();
}

// Ramp the clock-domain ratio registers (0xbc200000/0xBC200004) up to 511:511 (their
// 1:1 ceiling) in fixed steps — ported verbatim from the source's adjustDomainRatios().
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

// ── Externally-visible CPU frequency: direct write, no hook (kernel plugin) ──
// v556's syscall-hook attempt (patching scePower_driver_GetCpuClockFrequencyInt/
// GetBusClockFrequencyInt via sctrlHENPatchSyscall, same technique main.c uses
// for sceCtrl) was tried and CONFIRMED to still not change what a separately-
// running on-screen frequency overlay displays — removed; reason unclear
// (possibly that overlay caches a resolved pointer at its own load time rather
// than re-dispatching through the syscall table every poll). What it reads
// isn't a hardware register at a documented address — it's whatever that
// getter returns, almost certainly a private RAM variable inside the closed-
// source scePower_driver module (no address for it exists anywhere in
// PSP_References). But we're a kernel plugin with full memory access, so we
// can find it directly: read the getter's own compiled instructions and
// decode the standard MIPS "return a stored global" idiom (lui $r,HI;
// lw $r,LO($r); jr $ra) to compute the absolute address — the same live-
// instruction-decoding technique utils.c's find_io_driver_entry already uses
// (scanning for a JAL to resolve an undocumented function address).
// g_hud_cpu_freq_addr/g_hud_bus_freq_addr stay 0 if the pattern isn't found in
// range (e.g. gp-relative addressing instead) — a write is skipped, not
// guessed, rather than risk poking a wrong address in kernel RAM.
//
// A v581 experiment (later reverted) replaced this with two hardcoded address
// constants (0x8812C964/0x8812C968 — the same values this decode had produced
// on that boot) to skip re-deriving them every boot, reasoning the kernel
// module load layout should be deterministic for this fixed Ark-4 6.61/PSP-1000
// target. That reasoning is apparently NOT reliable enough in practice — the
// overlay's frequency display stopped updating after that change — so this
// stays dynamic: re-decode every boot, cheap and self-correcting if the load
// layout ever does shift for some reason (different games/plugin sets/etc.),
// rather than trusting a snapshot from one observed boot.
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
// only when the user has confirmed it (see boot_frozen_prompts), live from the Settings
// menu, or from utils.c's ProcessSignals on RESUME_COMPLETED (hence non-static — see
// its prototype in pspfatsave.h) — ported from the source's setOverclock().
void oc_apply(int id)
{
	int ds; unsigned int intr; u32 mul; int mhz10;

	if (id < 0 || id >= OC_STEPS) id = 0;

	oc_adjust_domain_ratios();

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

	if (DBG_UART()) {
		char buf[64];
		sprintf(buf, "[OC] applied step %d -> %d.%dMHz", id, mhz10 / 10, mhz10 % 10);
		WriteDebugLog(buf);
	}
}

// ── Revert to stock on game exit ──────────────────────────────────────────
// Confirmed by the user: the overclock otherwise STICKS past game exit (into
// XMB or whatever comes next) — module_stop() never ran anything to undo it,
// and XMB's own clock handling apparently doesn't reset the raw PLL registers
// either. The source project treats this as a real concern too — its
// "experimental" variant hooks these same two exit calls specifically to
// revert before letting the game actually exit (the overclock-picker variant
// we ported has that call commented out, presumably part of why it's the
// simpler/safer one — but the safety point still applies here).
// sceKernelExitGame / sceKernelExitGameWithStatus (module "sceLoadExec",
// library "LoadExecForUser", NIDs 0x05572A5F / 0x2AC9954B — standard PSPSDK
// NIDs, confirmed in PSP_References/pspsdk-master/src/user/LoadExecForUser.S
// and hooked by ARK-4 itself at several of these exact call sites for its own
// compat patches). Same sctrlHENFindFunction + sctrlHENPatchSyscall pattern as
// everywhere else in this file. Only actually touches the registers if we
// ever applied a non-stock step (g_overclock_id > 0) — a normal exit with the
// setting at stock does nothing extra.
static void (*g_real_exit_game)(void) = NULL;
static int (*g_real_exit_game_status)(int) = NULL;

static void oc_exit_game_patched(void)
{
	if (g_overclock_id > 0) oc_apply(0);
	if (g_real_exit_game) g_real_exit_game();
}

static int oc_exit_game_status_patched(int status)
{
	if (g_overclock_id > 0) oc_apply(0);
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
// Boot-time entry: baseline to stock 333MHz via the firmware call (matches the
// source's initOverclock), unlock the PLL registers, resolve the externally-
// visible frequency addresses, and install the exit-time revert hook. Does NOT apply a
// persisted non-stock step — see boot_frozen_prompts, called from menu_thread on
// first wake (once the game's display is actually up).
void oc_init(void)
{
	sceKernelIcacheInvalidateAll();
	oc_unlock_regs();
	scePowerSetClockFrequency(333, 333, 166);
	oc_probe_hud_getters();
	oc_install_exit_hook();
}
