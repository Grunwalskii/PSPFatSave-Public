#include "pspfatsave.h"
#include "corevolt.h"

// ── Allegrex/Tachyon core voltage (PSP-1000) ────────────────────────────────
// Derived from PSP_References/dumps/power_dump.bin (a 6.6x scePower kernel-module
// memory dump, load base 0x880DE700 — disassemble with
//   psp-objdump -D -b binary -m mips -EL --adjust-vma=0x880DE700).
// Treat the numbers below as the current best understanding of how the firmware
// drives the rail, not as a datasheet: there is no MB44C001 documentation in
// PSP_References, and uofw's Baryon reversal has exec_syscon_cmd_ctrl_voltage as
// an empty stub, so nothing here says what a code is worth in millivolts.
//
// The chain is Allegrex -> SPI(0xBE580000) -> Baryon/SYSCON -> Pommel MB44C001
// -> core rail. The Allegrex has no register for it; every change is a syscon
// command packet (0x42 CTRL_VOLTAGE, rail selector 1 = Tachyon core).
//
// What scePower does with it (scePowerSetClockFrequency @0x880E1F98):
//   fuse      = *(u32*)0xBC100098                  (== sceSysregGetFuseConfig())
//   code_high = (~fuse) & 0x700                    fuse bits  8..10 -> 0x000..0x700
//   code_low  = 0xB00 - ((fuse & 0x3800) >> 3)     fuse bits 11..13 -> 0x400..0xB00
// It picks a PLL entry from the 12-entry table at 0x880E5670 ({MHz, sel}: 19/8,
// 37/0, 74/9, 95/10, 111/11, 133/12, 148/1, 166/13, 190/2, 222/3, 266/4, 333/5),
// then programs code_high when that entry's sel is 4 or 5 (266 and 333 MHz) and
// code_low otherwise. There are only two sceSysconCtrlTachyonVoltage call sites
// in the whole module (0x880E21F4, 0x880E2404) — two voltage levels, not three;
// the "111/222/333" often quoted are PLL table entries, not voltage steps.
//
// So the code used at the HIGH clock is the numerically SMALLER one:
// smaller code = higher voltage. Cross-check: the IPL programs the identical
// 0x700 - (fuse & 0x700) at boot (ARK-4 libs/iplsdk/emcddr.c, get_fuse_id_based_code).
// Overvolting therefore means stepping the code DOWN toward 0x000, and the
// headroom a chip has is exactly code_high/0x100 steps — it is factory binning,
// so a chip that binned at 7 has code_high == 0x000 and no headroom at all.
// cv_probe() reports which one this unit is.
//
// Two things that otherwise hide any change:
//  - scePower only reprograms the voltage when the PLL *select* actually changes
//    (beq t5,s4 -> skip at 0x880E2178), and it reprograms it from its own stored
//    pair. cv_apply therefore also calls scePowerSetTachyonVoltage so the stored
//    pair agrees with us and a later clock change does not undo it.
//  - oc_apply() writes the PLL register directly and never tells scePower, so as
//    far as the firmware is concerned we are still at 333 MHz: an overclocked PSP
//    runs on the 333 MHz voltage code until something here changes it.

#define CV_FUSE_REG         0xBC100098   // Tachyon fuse config (what sceSysregGetFuseConfig returns)
#define CV_TACHYON_VER_REG  0xBC100040   // tachyon version (the same read iplsdk does)
#define CV_FUSE_HI_MASK     0x0700       // bits 8..10  -> high-clock code
#define CV_FUSE_LO_MASK     0x3800       // bits 11..13 -> low-clock code

int g_corevolt_level = CV_LEVEL_NONE;   // absolute level; NONE = leave at stock
volatile int g_corevolt_reapply = 0;

// Resolved once via sctrlHENFindFunction, exactly like sysstats.c's battery NIDs.
// These are 1.50 NIDs; ARK's resolver maps each to its 6.60 equivalent (all five
// are present in ARK-4's nid_660_data.c).
static int (*g_syscon_set_volt)(int)         = NULL;   // sceSysconCtrlTachyonVoltage
static int (*g_power_set_volt)(int, int)     = NULL;   // scePowerSetTachyonVoltage
static int (*g_power_get_volt)(int *, int *) = NULL;   // scePowerGetTachyonVoltage
static int (*g_power_get_cur_volt)(void)     = NULL;   // scePowerGetCurrentTachyonVoltage
static int (*g_sysreg_pll_sel)(void)         = NULL;   // sceSysregPllGetOutSelect
// Pommel register READ, two ways. Unlike the five above, neither NID is in ARK's
// 1.50->6.60 map, so sctrlHENFindFunction passes the raw NID straight through and
// they only resolve if 6.60 kept them. Whether they do is itself a result worth
// logging. Read-only — nothing here ever writes a Pommel register.
static int (*g_syscon_read_pommel)(u8, s16 *) = NULL;  // sceSysconReadPommelReg
static int (*g_syscon_cmd_exec)(void *, u32)  = NULL;  // sceSysconCmdExec (fallback)
static int (*g_id_lookup)(u16, u32, void *, u32) = NULL;  // sceIdStorageLookup (read-only)
static int (*g_syscon_ctrl_volt)(int, int)   = NULL;  // sceSysconCtrlVoltage(selector, value)

// Defined with the rail statics further down; cv_probe needs it before then.
static void cv_rails_boot_init(void);

static int g_cv_probed = 0;
static int g_cv_ok     = 0;    // fuse model agreed with scePower's stored pair
static u32 g_cv_fuse   = 0;
static int g_cv_stock  = 0;    // code_high — our reference "stock" code
static int g_cv_low    = 0;    // code_low
static int g_cv_now    = -1;   // last code we programmed (-1 = we never have)

u32 cv_fuse(void)       { return g_cv_fuse; }
int cv_code_stock(void) { return g_cv_stock; }
int cv_code_low(void)   { return g_cv_low; }
int cv_code_now(void)   { return g_cv_now; }
int cv_ok(void)         { return g_cv_ok; }

// Overvolt steps available on this chip: how many 0x100 notches sit between the
// stock code and 0x000. Zero means this unit binned at the top of the range and
// there is nothing above stock to give it.
int cv_max_up(void)
{
	if (!g_cv_ok) return 0;
	return g_cv_stock / CV_CODE_STEP;      /* levels between stock and the 0x000 ceiling */
}

int cv_stock_level(void) { return g_cv_ok ? CV_CODE_TO_LEVEL(g_cv_stock) : -1; }

int cv_level(void)
{
	if (!g_cv_ok) return -1;
	return (g_corevolt_level == CV_LEVEL_NONE) ? CV_CODE_TO_LEVEL(g_cv_stock) : g_corevolt_level;
}

static void cv_resolve(void)
{
	static int resolved;
	if (resolved) return;
	resolved = 1;
	g_syscon_set_volt    = (int (*)(int))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0x08DA3752);
	g_power_set_volt     = (int (*)(int, int))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0xDD27F119);
	g_power_get_volt     = (int (*)(int *, int *))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0x55D2D789);
	g_power_get_cur_volt = (int (*)(void))sctrlHENFindFunction("scePower_Service", "scePower_driver", 0x57A098B4);
	g_sysreg_pll_sel     = (int (*)(void))sctrlHENFindFunction("sceLowIO_Driver", "sceSysreg_driver", 0xB4560C45);
	g_syscon_read_pommel = (int (*)(u8, s16 *))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0x3DE38336);
	g_syscon_cmd_exec    = (int (*)(void *, u32))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0x5B9ACC97);
	g_id_lookup          = (int (*)(u16, u32, void *, u32))sctrlHENFindFunction("sceIdStorage_Service", "sceIdStorage_driver", 0x6FE062D1);
	g_syscon_ctrl_volt   = (int (*)(int, int))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0x01677F91);
}

// ── IdStorage voltage calibration (read-only) ───────────────────────────────
// There is a SECOND source for these codes besides the fuse register, which the
// fuse-only reading of scePower missed. module_start does, at 0x880DE74C:
//   sceIdStorageLookup(4, 0, buf, 0x200)
//   b = buf[0x20];  a0 = (b & 0x80) ? ((b & 0x7F) << 8) : -1     (seb + bltz at 0x880DE864)
//   b = buf[0x21];  a1 = (b & 0x80) ? ((b & 0x7F) << 8) : -1
//   scePowerSetTachyonVoltage(a0, a1)                             (jal at 0x880DE880)
// so bit 7 is a "present" flag and the low 7 bits are the code, in the same
// <<8 form the syscon takes. Leaf 6 feeds scePowerSetDdrVoltage (jal 0x880DE9B8)
// and the third-rail setter (jal 0x880DE9C8) the same way.
//
#define CV_POMMEL_REGS  0x40
// Preferred path is the named wrapper. The fallback hand-builds the same packet
// for sceSysconCmdExec: syscon cmd 0x49 READ_POMMEL_REG, tx = [cmd, len, reg] and
// the driver appends the checksum itself (sceSysconCmdExecAsync computes
// ~sum(tx[0..len-1]) whenever flags & 0x100 is clear, so we must NOT pre-fill it).
// Field offsets are SceSysconPacket from PSP_References/uofw-master/include/syscon.h:
// next/status/semaId (12 bytes) then tx[16] at +12 and rx[16] at +28; rx data
// starts at rx[3] (PSP_SYSCON_RX_DATA(0)). The real struct is 96 bytes — the
// buffer below is deliberately far larger and zeroed, so even if this firmware's
// layout differs the driver can only ever scribble inside our own stack buffer.
#define CV_POMMEL_REGS  0x40
#define CV_PKT_TX_OFF   12
#define CV_PKT_RX_OFF   28

static int cv_pommel_read(int reg, int *out)
{
	if (g_syscon_read_pommel) {
		s16 v = 0;
		int r = g_syscon_read_pommel((u8)reg, &v);
		if (r < 0) return r;
		*out = (int)((u16)v);
		return 0;
	}
	if (g_syscon_cmd_exec) {
		u8 pkt[256];
		u8 *tx = pkt + CV_PKT_TX_OFF, *rx = pkt + CV_PKT_RX_OFF;
		int r;
		memset(pkt, 0, sizeof(pkt));
		tx[0] = 0x49;          /* PSP_SYSCON_CMD_READ_POMMEL_REG */
		tx[1] = 3;             /* length */
		tx[2] = (u8)reg;
		r = g_syscon_cmd_exec(pkt, 0);
		if (r < 0) return r;
		*out = (int)(rx[3] | (rx[4] << 8));
		return 0;
	}
	return -1;
}

static int g_pom[CV_POMMEL_REGS];
static u8  g_pom_ok[CV_POMMEL_REGS];   // 1 = this register answered on the full sweep
static int g_pom_valid = 0;

// Full 64-register sweep, results kept for cv_pom_value (the DDR/unknown rails
// read their boot codes from here). No logging — the earlier dump/diff variants
// existed only to identify the rails, which is done.
void cv_pommel_dump(int full)
{
	int cur[CV_POMMEL_REGS];
	int i;

	cv_resolve();
	if (!g_syscon_read_pommel && !g_syscon_cmd_exec) return;

	// A read is ~34ms, so a full 64-register sweep costs ~2.2s. After the first
	// sweep only the registers that answered are worth re-reading.
	for (i = 0; i < CV_POMMEL_REGS; i++) {
		int rc;
		cur[i] = -1;
		if (!full && g_pom_valid && !g_pom_ok[i]) { cur[i] = g_pom[i]; continue; }
		rc = cv_pommel_read(i, &cur[i]);
		if (rc == 0) { if (full) g_pom_ok[i] = 1; }
		else         { if (full) g_pom_ok[i] = 0; }
	}

	for (i = 0; i < CV_POMMEL_REGS; i++) g_pom[i] = cur[i];
	g_pom_valid = 1;
}

void cv_probe(void)
{
	int a = -1, b = -1;

	if (g_cv_probed) return;
	g_cv_probed = 1;
	cv_resolve();

	g_cv_fuse  = *((volatile u32 *)CV_FUSE_REG);
	g_cv_stock = (int)((~g_cv_fuse) & CV_FUSE_HI_MASK);
	g_cv_low   = 0xB00 - (int)((g_cv_fuse & CV_FUSE_LO_MASK) >> 3);

	if (g_power_get_volt) g_power_get_volt(&a, &b);

	// The model check: scePower derives its stored pair from the same fuse, so if
	// our two computed codes don't match what it holds, the derivation is wrong on
	// this firmware and nothing here may write to the rail.
	g_cv_ok = (g_power_get_volt && g_syscon_set_volt && a == g_cv_stock && b == g_cv_low);

	cv_pommel_dump(1);   // baseline: every register, so the rails know their boot codes

	cv_rails_boot_init();
}

// Program one code. Both writes matter: the syscon call changes the rail now, the
// scePower call keeps its stored pair in agreement so its own PLL transitions
// don't quietly put the stock code back.
static void cv_write_code(int code)
{
	if (code < CV_CODE_MIN) code = CV_CODE_MIN;
	if (code > CV_CODE_MAX) code = CV_CODE_MAX;
	if (g_syscon_set_volt) g_syscon_set_volt(code);
	if (g_power_set_volt)  g_power_set_volt(code, -1);
	g_cv_now = code;
}

// Apply an absolute level (0..11, higher = more volts). CV_LEVEL_NONE puts the
// rail back at this chip's fuse-derived stock code. Thread context only —
// sceSysconCmdExec refuses to run in a handler or with interrupts off.
void cv_apply(int level)
{
	cv_resolve();
	if (!g_cv_probed) cv_probe();
	if (!g_cv_ok) { g_corevolt_level = CV_LEVEL_NONE; return; }
	if (level == CV_LEVEL_NONE) { cv_revert(); return; }
	/* The whole encoding is reachable: level 11 is the 0x000 ceiling, level 0 the
	   0xB00 floor. Which of those is above stock is a per-chip fact (fuse binning),
	   reported by cv_max_up, not a limit imposed here. */
	if (level > CV_LEVEL_MAX) level = CV_LEVEL_MAX;
	if (level < 0)            level = 0;
	g_corevolt_level = level;
	cv_write_code(CV_LEVEL_TO_CODE(level));
	// No Pommel diff sweep after the write: it re-reads up to 64 registers over the
	// syscon (~34ms each) just to re-confirm which register is the DAC, and the
	// answer is already established (reg 0x10). The step must feel instant in the
	// menu; the diagnostics probes still do the full sweep on demand.
}

void cv_revert(void)
{
	if (!g_cv_ok) return;
	g_corevolt_level = CV_LEVEL_NONE;
	if (g_cv_now < 0 || g_cv_now == g_cv_stock) return;   // never wrote, or already stock
	cv_write_code(g_cv_stock);
}

// Any firmware resume reprograms the rail from scePower's own state, so the step
// has to go back on afterwards. ProcessSignals only sets the flag; this runs on
// the menu thread, where a syscon transaction is legal.
void cv_poll_reapply(void)
{
	if (!g_corevolt_reapply) return;
	g_corevolt_reapply = 0;
	if (g_cv_ok && g_corevolt_level != CV_LEVEL_NONE) cv_apply(g_corevolt_level);
}

// Compact value for the Overclock settings row (see draw_settings).
void cv_short_str(char *out)
{
	// Max 3 chars: the Overclock row it shares has ~51 columns and already carries
	// the frequency, the bus divider and the STABLE tag.
	if (!g_cv_probed || !g_cv_ok)         { strcpy(out, "V--"); return; }
	if (g_corevolt_level == CV_LEVEL_NONE) { strcpy(out, "V-");  return; }
	sprintf(out, "V%d", g_corevolt_level);
}

int cv_pom_value(int reg)
{
	if (reg < 0 || reg >= CV_POMMEL_REGS || !g_pom_valid || !g_pom_ok[reg]) return -1;
	return g_pom[reg];
}

// ── DDR rail ────────────────────────────────────────────────────────────────
// See corevolt.h. Steps across the rail's whole reachable span from its boot value, one
// syscon write per step, one log line, nothing read back — the battery-current
// bracketing (1s+ per side) and the reg-12 read-back each step measured nothing
// the log could act on and made the row feel dead. The boot base comes from the
// full Pommel sweep cv_probe already did at startup, so stepping costs one
// transaction.
int g_ddr_level = CV_LEVEL_NONE;   // absolute level; NONE = left at the boot code
static int g_ddr_base = -1;     // code read from reg 0x12 at boot
static int g_ddr_now  = -1;     // last code we programmed
// The +/-2 notch caps are GONE (were v963's Unlock toggle, now the default): every
// rail steps across its whole 0x000..0xB00 span. The caps bought nothing - the
// boot-value anchors and the per-step readbacks are the actual safety.

int cv_ddr_base(void) { return g_ddr_base; }
int cv_ddr_base_level(void) { return (g_ddr_base < 0) ? -1 : CV_CODE_TO_LEVEL(g_ddr_base); }
int cv_ddr_code(void) { return (g_ddr_now >= 0) ? g_ddr_now : g_ddr_base; }

void cv_ddr_apply(int level)
{
	int code;

	cv_resolve();
	if (!g_syscon_ctrl_volt) return;
	if (!g_pom_valid) cv_pommel_dump(1);
	if (g_ddr_base < 0) g_ddr_base = cv_pom_value(CV_DDR_REG);
	if (g_ddr_base < 0) return;   // reg unreadable - do not write blind

	/* Absolute level, so the same number is the same voltage on every boot — which
	   an offset from the boot code could never be, since that code varies. */
	if (level > CV_LEVEL_MAX) level = CV_LEVEL_MAX;
	if (level < 0)            level = 0;
	code = CV_LEVEL_TO_CODE(level);

	g_syscon_ctrl_volt(CV_DDR_SEL, code);
	g_ddr_level = level;
	g_ddr_now   = code;
}

void cv_ddr_revert(void)
{
	if (g_ddr_base < 0 || g_ddr_now < 0 || g_ddr_now == g_ddr_base) { g_ddr_level = CV_LEVEL_NONE; return; }
	cv_ddr_apply(CV_CODE_TO_LEVEL(g_ddr_base));
	g_ddr_level = CV_LEVEL_NONE;   /* back at the boot code = untouched again */
}

// ── Unknown rails (selectors 2, 4, 5, 7) ────────────────────────────────────
// See corevolt.h. The selector sweep wrote each rail's own register value back
// without error, so the write path is known-good; what each rail FEEDS is not.
// Same one-write discipline as the DDR rail, and the boot value is per
// rail — read from that rail's own register during the full Pommel sweep.
typedef struct { int sel, reg, base, now, level; } CvRail;
static CvRail g_rails[] = {
	{ 2, 0x11, -1, -1, CV_LEVEL_NONE },
	{ 4, 0x13, -1, -1, CV_LEVEL_NONE },
	{ 5, 0x14, -1, -1, CV_LEVEL_NONE },
	{ 7, 0x16, -1, -1, CV_LEVEL_NONE },
};
#define CV_RAIL_N  ((int)(sizeof(g_rails) / sizeof(g_rails[0])))

int cv_rail_count(void) { return CV_RAIL_N; }
int cv_rail_sel(int i)  { return (i >= 0 && i < CV_RAIL_N) ? g_rails[i].sel : -1; }
int cv_rail_reg(int i)  { return (i >= 0 && i < CV_RAIL_N) ? g_rails[i].reg : -1; }
int cv_rail_base(int i) { return (i >= 0 && i < CV_RAIL_N) ? g_rails[i].base : -1; }
int cv_rail_level(int i) { return (i >= 0 && i < CV_RAIL_N) ? g_rails[i].level : CV_LEVEL_NONE; }
int cv_rail_base_level(int i)
{
	int b = cv_rail_base(i);
	return (b < 0) ? -1 : CV_CODE_TO_LEVEL(b);
}
int cv_rail_code(int i)
{
	if (i < 0 || i >= CV_RAIL_N) return -1;
	return (g_rails[i].now >= 0) ? g_rails[i].now : g_rails[i].base;
}

void cv_rail_apply(int i, int level)
{
	int code;

	cv_resolve();
	if (i < 0 || i >= CV_RAIL_N) return;
	if (!g_syscon_ctrl_volt) return;
	if (!g_pom_valid) cv_pommel_dump(1);
	if (g_rails[i].base < 0) g_rails[i].base = cv_pom_value(g_rails[i].reg);
	if (g_rails[i].base < 0) return;   // unreadable - do not write blind

	/* Absolute level, as cv_ddr_apply — see the note there. */
	if (level > CV_LEVEL_MAX) level = CV_LEVEL_MAX;
	if (level < 0)            level = 0;
	code = CV_LEVEL_TO_CODE(level);

	g_syscon_ctrl_volt(g_rails[i].sel, code);
	g_rails[i].level = level;
	g_rails[i].now   = code;
}

void cv_rails_revert(void)
{
	int i;
	for (i = 0; i < CV_RAIL_N; i++) {
		if (g_rails[i].level == CV_LEVEL_NONE || g_rails[i].base < 0) continue;
		cv_rail_apply(i, CV_CODE_TO_LEVEL(g_rails[i].base));
		g_rails[i].level = CV_LEVEL_NONE;
	}
}

// ── Rail defaults across power cycles ───────────────────────────────────────
// See corevolt.h for why this exists: the Pommel holds its registers through a
// reboot and a hard reset, and nothing in the firmware reprograms the DDR or the
// unknown rails, so a crashed session leaves them wherever they were.
//
// Packing for settings.cfg, one word: bit 31 = valid, then five 4-bit LEVELS in
// nibbles 0..4 — DDR first, then rails in table order (selectors 2, 4, 5, 7). A
// level is 0..11 so it fits a nibble exactly.
#define CV_DEF_VALID  0x80000000u

// Reference measurement, in g_def_level order (DDR, then selectors 2, 4, 5, 7).
// Taken from this console's Pommel dumps BEFORE the plugin could write these rails
// — v950/v951/v953 all read "10: 0400 0600 0200 0200 0200 ---- 0300" across
// several boots, so that agreement is the power-on state rather than a leftover.
// Codes 0x200/0x600/0x200/0x200/0x300 -> levels 9/5/9/9/8.
//
// This is ONE unit (TA-081). It is offered as something to adopt deliberately, not
// applied on its own, because another board could differ and writing a wrong
// "default" to an unidentified rail at every boot would be worse than the problem
// it solves.
static const int CV_DEF_BUILTIN[1 + CV_RAIL_N] = { 9, 5, 9, 9, 8 };

static int g_def_valid = 0;
static int g_def_level[1 + CV_RAIL_N];   // [0] = DDR, [1..] = g_rails order

int cv_rail_defaults_valid(void) { return g_def_valid; }

u32 cv_rail_defaults_get(void)
{
	u32 v = 0;
	int i;
	if (!g_def_valid) return 0;
	for (i = 0; i < 1 + CV_RAIL_N; i++)
		v |= ((u32)(g_def_level[i] & 0xF)) << (i * 4);
	return v | CV_DEF_VALID;
}

void cv_rail_defaults_load(u32 v)
{
	int i;
	if (!(v & CV_DEF_VALID)) { g_def_valid = 0; return; }
	for (i = 0; i < 1 + CV_RAIL_N; i++) {
		int lv = (int)((v >> (i * 4)) & 0xF);
		// A nibble can hold 0..15 but only 0..CV_LEVEL_MAX is a real level; anything
		// else means a foreign generation, so reject the whole set rather than
		// restore rails to a value that was never a level.
		if (lv > CV_LEVEL_MAX) { g_def_valid = 0; return; }
		g_def_level[i] = lv;
	}
	g_def_valid = 1;
}

void cv_rail_defaults_builtin(void)
{
	int i;
	for (i = 0; i < 1 + CV_RAIL_N; i++) g_def_level[i] = CV_DEF_BUILTIN[i];
	g_def_valid = 1;
	cv_rail_defaults_restore();
}

// Put every rail back on the baseline, and the Tachyon rail back on its
// FUSE-DERIVED stock code (not a stored level). Called at startup (see
// cv_probe) so a session that crashed with a rail parked somewhere is
// recovered without needing the battery pulled, and from the menu on demand.
void cv_rail_defaults_restore(void)
{
	int i;

	if (!g_def_valid) return;
	cv_resolve();
	if (!g_pom_valid) cv_pommel_dump(1);

	// Tachyon rail: fuse stock is the only honest "default" for it — the IPL
	// reprograms it from the fuse every boot anyway, so any stored level here
	// is a deviation, not a baseline. Clearing g_corevolt_level (rather than
	// pointing it at stock) keeps the row displaying the stock code it now
	// genuinely runs at.
	if (g_cv_ok && g_cv_now >= 0 && g_cv_now != g_cv_stock) {
		cv_write_code(g_cv_stock);
		g_corevolt_level = CV_LEVEL_NONE;
	}

	if (cv_ddr_code() >= 0 && CV_CODE_TO_LEVEL(cv_ddr_code()) != g_def_level[0]) {
		cv_ddr_apply(g_def_level[0]);
		g_ddr_base  = CV_LEVEL_TO_CODE(g_def_level[0]);   /* the baseline IS "boot" now */
		g_ddr_level = CV_LEVEL_NONE;
	}
	for (i = 0; i < CV_RAIL_N; i++) {
		int c = cv_rail_code(i);
		if (c < 0 || CV_CODE_TO_LEVEL(c) == g_def_level[1 + i]) continue;
		cv_rail_apply(i, g_def_level[1 + i]);
		g_rails[i].base  = CV_LEVEL_TO_CODE(g_def_level[1 + i]);
		g_rails[i].level = CV_LEVEL_NONE;
	}
}

// Capture every rail's boot code up front (rather than lazily on the first write,
// which would leave nothing to compare against) and then undo whatever a previous
// session left behind — the Pommel keeps its registers across reboots and hard
// resets, so that is not hypothetical.
static void cv_rails_boot_init(void)
{
	int i;
	g_ddr_base = cv_pom_value(CV_DDR_REG);
	for (i = 0; i < CV_RAIL_N; i++) g_rails[i].base = cv_pom_value(g_rails[i].reg);
	if (g_def_valid) cv_rail_defaults_restore();
}
