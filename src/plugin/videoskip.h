#ifndef FS_VIDEOSKIP_H
#define FS_VIDEOSKIP_H

#include "pspfatsave.h"

// Per-game Intro Video Skip mode (gameset.cfg word 4) — shared with menu.c settings.
#define VSKIP_OFF     0
#define VSKIP_CAPTURE 1
#define VSKIP_TIMED   2
#define VSKIP_LEARN_MAX_MS (120 * 1000)   // sanity clamp on a learned window (menu.c settings)
#define VSKIP_HOLD_BTN     PSP_CTRL_RIGHT // capture-mode hold button (menu.c boot prompt)

// Auto-generated public interface for videoskip.c (review/trim).
extern volatile int g_vskip_banner;
extern volatile int g_vskip_active;   // watcher lifetime (CAPTURE/TIMED) — read by the frame limiter
void game_video_skip_load(void);
int video_skip_thread(SceSize args, void *argp);
void vskip_banner_draw(void *topaddr, int bufferwidth, int pixelformat);
void vskip_inject_buttons(SceCtrlData *pad_data, int count, int res, int negative);
void vskip_inject_latch(SceCtrlLatch *latch);

#endif
