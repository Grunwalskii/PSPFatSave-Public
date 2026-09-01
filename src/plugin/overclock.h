#ifndef FS_OVERCLOCK_H
#define FS_OVERCLOCK_H

#include "pspfatsave.h"

#define OC_STEPS 25   // number of overclock steps (index range for g_oc_multipliers/g_oc_freq_x10)

// Bus clock TARGET (LIVE ONLY, deliberately not persisted — this is a diagnostic).
// The gear register is a fraction, not a fixed divisor, so an arbitrary frequency is
// reachable. At stock (PLL 333, gear 1:1) the bus is 166MHz, so the bus domain's base
// is PLL/2 and the gear scales that:
//     bus_MHz = (PLL / 2) * (num / den)   ->   num = 511 * 2 * bus_MHz / PLL   (den 511)
// Index 0 = "sync": leave the gear at 1:1 so the bus scales with the PLL, which is what
// the plugin has always done — and which at PLL 470 means a 235MHz bus. The earlier
// "B/2" divider was NOT "hold the bus at stock": it halved that again, to 117MHz. A
// real frequency target is the only way to run the CPU high with the bus at 166.
#define OC_BUS_TARGETS 7
extern const short g_oc_bus_tab[OC_BUS_TARGETS];   // MHz; [0] = 0 = sync (gear 1:1)
extern int g_oc_bus_sel;                           // index into g_oc_bus_tab
extern u32 g_oc_cpu_num, g_oc_cpu_den, g_oc_bus_num, g_oc_bus_den;   // gears READ BACK
extern u32 g_oc_bus_want;   // bus numerator we asked for; != g_oc_bus_num means it was refused

extern const int g_oc_freq_x10[OC_STEPS];
extern const u32 g_oc_multipliers[OC_STEPS];
void oc_init(void);

#endif
