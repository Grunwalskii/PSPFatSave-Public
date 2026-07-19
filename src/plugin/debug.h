#ifndef FS_DEBUG_H
#define FS_DEBUG_H

#include "pspfatsave.h"

// Debug checkpoint / trace / diag API. The real functions live in debug.c and are
// compiled only in a DEBUG_BUILD; in a release build every entry point collapses to
// a no-op macro so the call sites, their sprintf work, and the string literals all
// drop out of the binary. g_menu_quiet / ms_test_row are plain ints, always present
// (the core reads g_menu_quiet even in release), so they stay outside the gate.
extern int g_menu_quiet;   // menu-open flag: suppresses per-checkpoint MS writes
extern int ms_test_row;    // next on-screen checkpoint row

#if DEBUG_BUILD
void gatelog_line(const char *line);
void gatelog_flush(void);
void gatelog_reset(void);
void trace_start(void);
void trace_flush(const char *tag);
void ms_test_cp(u32 cp, u32 retval, const char *label);
void diag_profile_ex(const char *phase, int with_vcrc);
void diag_threads(const char *phase, const SceUID *tids, int tcount);
#define diag_profile(phase)        diag_profile_ex((phase), 1)   // pre-freeze: full (with vcrc)
#define diag_profile_frozen(phase) diag_profile_ex((phase), 0)   // frozen: MMIO only (no vcrc)
#else
#define gatelog_line(line)                ((void)0)
#define gatelog_flush()                   ((void)0)
#define gatelog_reset()                   ((void)0)
#define trace_start()                     ((void)0)
#define trace_flush(tag)                  ((void)0)
#define ms_test_cp(cp, retval, label)     ((void)0)
#define diag_profile(phase)               ((void)0)
#define diag_profile_frozen(phase)        ((void)0)
#define diag_threads(phase, tids, tcount) ((void)0)
#endif

#endif
