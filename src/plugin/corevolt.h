#ifndef FS_COREVOLT_H
#define FS_COREVOLT_H

#include "pspfatsave.h"

// ── Allegrex/Tachyon core voltage (PSP-1000) ────────────────────────────────
// See the block comment at the top of corevolt.c for the derivation. Short
// version: the core rail is driven by the Pommel (MB44C001) DC-DC, which only
// SYSCON can talk to. The firmware sends it a 16-bit "code" via syscon command
// 0x42, and (as derived from scePower) SMALLER CODE = HIGHER VOLTAGE.
//
// Step limits are NOT fixed constants: they are derived per rail from that rail's
// own stock/boot code, because the reachable span differs per chip (the Tachyon
// stock code is fuse-binned) and per boot (the DDR boot code has been observed at
// 0x300, 0x200 and 0x000 on this unit). cv_apply / cv_ddr_apply / cv_rail_apply
// each clamp the STEP to what their span actually reaches.
#define CV_CODE_STEP  0x100    // one notch; the firmware only ever uses multiples of this
#define CV_CODE_MIN   0x000    // lowest code the firmware's own range reaches (= most volts)
#define CV_CODE_MAX   0xB00    // highest code the firmware's own range reaches (= least volts)

// ── LEVEL: the absolute scale the UI and the persisted settings use ─────────
// The raw code runs backwards (0x000 = most volts) and everything used to be
// expressed as a signed offset from a baseline — which was unusable for the DDR
// and unknown rails, because their baseline is whatever the Pommel happened to
// boot at and that VARIES between boots. "+1" was a different voltage on
// different days. A level is absolute: 0..11, and HIGHER MEANS MORE VOLTAGE.
//     level 11 = code 0x000 = most volts
//     level  0 = code 0xB00 = least volts
// CV_LEVEL_NONE means "untouched, leave the rail where it booted" — needed as a
// separate value because 0 is a legitimate level, not an absence of one.
#define CV_LEVEL_MAX   11
#define CV_LEVEL_NONE  (-1)
#define CV_CODE_TO_LEVEL(c)  (CV_LEVEL_MAX - ((c) / CV_CODE_STEP))
#define CV_LEVEL_TO_CODE(l)  ((CV_LEVEL_MAX - (l)) * CV_CODE_STEP)

// g_corevolt_level / g_corevolt_reapply live in pspfatsave.h — utils.c needs them
// and only includes that header.
//
// A persisted step is NEVER applied on its own at boot. It rides the overclock's
// boot decision (boot_frozen_prompts / the STABLE branch in menu_thread): the
// confirm prompt names it, declining clears it, and with the overclock OFF it is
// reset to stock. Extra voltage only ever exists to hold up an overclock, and a
// silent permanent overvolt across reboots is exactly what that gate is for.

void cv_probe(void);          // one-shot probe (cached); call from a thread
// Full sweep of Pommel registers 0x00..0x3F over the syscon (cmd 0x49), results
// kept for cv_pom_value. Never writes. Thread context only (64 syscon
// transactions), and a no-op if neither entry point resolved.
void cv_pommel_dump(int full);
// The one-shot diagnostic probes (Pommel dump/diff logging, IdStorage read-out)
// are gone with their logging: the sub-step probe answered its question (0x100 IS
// the hardware step, the low byte is discarded) and the selector sweep answered
// its own (the 0x1X bank is indexed by selector — reg = 0x0F + sel; proven for
// selector 1 and, by watching reg 0x12 follow real writes, for selector 3 as
// well). Those findings live in the comments here.
int cv_pom_value(int reg);   // last known value of a Pommel register, -1 if unreadable

// ── Rail defaults across power cycles ───────────────────────────────────────
// MEASURED, and it matters: the Pommel keeps its register contents through a warm
// reboot AND a syscon hard reset. Only pulling the battery and AC clears it. The
// Tachyon rail hides this because the IPL and scePower both reprogram it from the
// fuse on every boot — but nothing reprograms the DDR or the unknown rails, so
// whatever this plugin last wrote is still there on the next boot.
//
// That also explains the "DDR boot value varies between boots (0x300/0x200/0x000)"
// puzzle: it was not cold-vs-warm boot at all, it was this plugin's own leftovers
// being read back as if they were the boot value.
//
// So the game-exit revert is not enough — a crash, a hang or a power-switch off
// leaves a rail parked wherever it was. These persist a baseline in settings.cfg
// and put every rail back to it at plugin start, which is the only thing that
// makes a crashed session recoverable without a battery pull.
//
// The baseline is only as good as the state it was captured from. Capture it from
// the Overclock page after a REAL power-down (battery and AC out), which is the
// only way to see the true power-on values.
u32  cv_rail_defaults_get(void);      // packed for settings.cfg (bit31 = valid)
void cv_rail_defaults_load(u32 v);    // adopt a stored baseline at startup
int  cv_rail_defaults_valid(void);
// Adopt the compiled-in reference. Needed because clearing the Pommel for
// real means pulling the battery, which is impossible on a console with a soldered
// cell — but the values are recoverable anyway: the boot dumps from before this
// plugin could write these rails agreed across several boots, and that reading IS
// the power-on state. See CV_DEF_BUILTIN in corevolt.c for the measurement and the
// caveat about it being one unit's.
void cv_rail_defaults_builtin(void);
void cv_rail_defaults_restore(void);  // write the baseline to every rail that differs

// ── DDR rail (syscon selector 3, Pommel reg 0x12) ───────────────────────────
// The rail identity is established: scePower's own apply path calls
// sceSysconCtrlVoltage(3, ...) fed from scePowerSetDdrVoltage. On a PSP-1000 it
// never actually runs — the pair stays -1 and IdStorage leaf 6 is empty — so the
// rail sits at whatever the Pommel booted with.
//
// That boot value is NOT a fixed factory constant: on this unit the boot dump has
// read 0x300 and 0x200 on earlier runs and 0x000 on the two latest boots
// ([POM] 10: 0400 0600 0000 ... — cold vs warm boot is the current suspicion,
// unproven). So "stock" below means "whatever the rail booted at this boot",
// nothing more.
//
// What is NOT established, and matters: this rail's range and DIRECTION. The
// Tachyon rail runs 0x000..0xB00 with smaller meaning more volts; nothing says
// that generalises. So a step is just "N notches from the boot value", and the
// full reachable span is offered. Be aware of what the varying boot value does to
// that: at a boot code of 0x000 the "+" direction has NO range at all and "-" is
// an 11-notch one-way trip, so the same step number is a different absolute
// voltage on different boots. Never persisted — a stored bad DDR voltage would
// re-apply on every boot.
//
// No read-back anywhere on this path: every Pommel read is a ~34ms syscon
// transaction and the 8-sample battery-current averaging made each step take
// seconds for data that never answered the question. One log line per step.
#define CV_DDR_SEL       3
#define CV_DDR_REG       0x12

extern int g_ddr_level;             // CV_LEVEL_NONE = untouched, else 0..11
int  cv_ddr_base(void);            // the code found at boot (-1 = not captured)
int  cv_ddr_base_level(void);      // that code as a level (-1 if not captured)
int  cv_ddr_code(void);            // the code now running (boot code if untouched)
void cv_ddr_apply(int level);      // clamp to 0..11, write, log
void cv_ddr_revert(void);          // straight back to the boot code

// ── Unknown rails (syscon selectors 2, 4, 5, 7) ─────────────────────────────
// The 0x1X Pommel register bank holds more rails than the two identified ones.
// The no-op selector sweep proved the WRITE path works for selectors 2/4/5/7
// (each accepted its predicted register's own value back without error) and
// found their registers — 0x11, 0x13, 0x14, 0x16 — but nothing identifies what
// each rail feeds. Candidates on this board: LCD, backlight, audio codec,
// remote/headphone rail, SDRAM Termination. Touching one blind changes an
// unknown supply by an unknown amount.
//
// Same discipline as the DDR rail before its identity was proven: live-only,
// stepped from the boot value of ITS OWN register across that register's whole
// reachable span, one syscon write per step, one log line, never persisted. If
// anything misbehaves, the game exit path restores every rail from its boot value.
// i indexes the table in corevolt.c; order is fixed (2, 4, 5, 7).
int  cv_rail_count(void);
int  cv_rail_sel(int i);        // syscon selector of rail i
int  cv_rail_reg(int i);        // its Pommel register
int  cv_rail_base(int i);       // boot value of that register (-1 unknown)
int  cv_rail_base_level(int i); // that boot value as a level (-1 unknown)
int  cv_rail_level(int i);      // CV_LEVEL_NONE = untouched, else 0..11
int  cv_rail_code(int i);       // the code now running (base if untouched)
void cv_rail_apply(int i, int level);  // clamp to 0..11, write, read back, log
void cv_rails_revert(void);            // all unknown rails back to boot values
void cv_apply(int level);     // clamp to 0..11, apply, adopt as g_corevolt_level
void cv_revert(void);         // back to the stock code (game exit / teardown)
void cv_poll_reapply(void);   // menu-thread side of g_corevolt_reapply
int  cv_stock_level(void);    // this chip's fuse-derived stock code as a level
int  cv_level(void);          // level running now (stock level if untouched)
int  cv_max_up(void);         // levels available ABOVE stock on THIS chip (0 = none)
int  cv_ok(void);             // 1 = the fuse model matched scePower; 0 = don't touch
void cv_short_str(char *out); // "V:stock" / "V+2" / "V-1" / "V:n/a" for the menu row

// Cached read-out, for the Overclock help page (all 0 until cv_probe has run).
u32  cv_fuse(void);           // raw 0xBC100098
int  cv_code_stock(void);     // (~fuse) & 0x700   — used by scePower at >= 266MHz
int  cv_code_low(void);       // 0xB00 - ((fuse & 0x3800) >> 3) — used at <= 222MHz
int  cv_code_now(void);       // the code this module last programmed (-1 = none yet)

#endif
