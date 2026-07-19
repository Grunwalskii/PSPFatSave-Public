#ifndef FS_OVERCLOCK_H
#define FS_OVERCLOCK_H

#include "pspfatsave.h"

#define OC_STEPS 25   // number of overclock steps (index range for g_oc_multipliers/g_oc_freq_x10)

// Auto-generated public interface for overclock.c (review/trim).
extern const int g_oc_freq_x10[OC_STEPS];
extern const u32 g_oc_multipliers[OC_STEPS];
void oc_init(void);

#endif
