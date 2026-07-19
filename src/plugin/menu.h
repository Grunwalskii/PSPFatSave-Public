#ifndef FS_MENU_H
#define FS_MENU_H

#include "pspfatsave.h"

// Save-browser layout constants (BR_BG/BR_SEL/... colors live in pspfatsave.h).
#define BR_TITLE_ROW   1
#define BR_LIST_ROW    3     // first list char-row
#define BR_ROW_H       10    // char-rows per entry (80px)
#define BR_VISIBLE     3     // entries on screen at once

// Auto-generated public interface for menu.c (review/trim).
void boot_frozen_prompts(int do_oc, int do_arm);
int confirm(const char *msg, int by);
void draw_settings(int sel, const char *gid);
void game_frame_limit_load(void);
void load_game_settings(const char *gid);   // -> g_autoload + g_compress + g_frame_limit + g_video_skip(+ms) for gid
void load_settings(void);
void ram_usage_kb(u32 *static_kb, u32 *dynamic_kb, u32 *free_kb);
void run_save_browser(void);
void save_game_settings(const char *gid);
void suppress_posbuf_slots(SceCtrlData *pad_data, int count, int res, u32 clean_value);

#endif
