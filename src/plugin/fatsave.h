#ifndef FS_FATSAVE_H
#define FS_FATSAVE_H

#include "pspfatsave.h"

#define MAX_SAVE_ROWS 32   // save-browser row cap (g_rows[]); shared with menu.c
#define SUPPRESS_POSBUF_CALLS 80   // post-resume posbuf suppress cap (ring is 64 slots; safety upper bound)
#define GAMESET_MAGIC 0x47534554u  // per-game settings file magic ("GSET"); shared with menu.c/videoskip.c

// Shared compression / MS-staging work buffer (menu.c decodes thumbnails into it).
extern u8 work_buf[COMPRESS_BUF_SIZE];

// Auto-generated public interface for fatsave.c (review/trim).
void arm_input_suppress(void);
int cooperative_volmem_release(const SceUID *game_tids, int tcount);
void copy_game_thumb(const char *bin_path);
extern volatile u32 g_apply_base;
extern int g_autoload_armed;
extern int    g_game_pcb_count;
extern char g_game_title[64];
extern int g_stage_spot;
extern volatile u32 g_suppress_posbuf_ts;
int kpeek(SceCtrlData *pad);
int me_rpc_probe(void);
int ms_probe_after_freeze(void);
void ms_probe_reap(void);
u64 now_us(void);
void resume_game_threads(void);
int suspend_escalating(int stable_gate, int cap_ms);
u32 wait_button_edge(u32 mask);
void wait_buttons_up(void);
void wait_release(u32 mask);

#endif
