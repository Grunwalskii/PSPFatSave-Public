#ifndef FS_SYSSTATS_H
#define FS_SYSSTATS_H

#include "pspfatsave.h"

// Poll-thread stack sizes (menu.c ram_usage_kb accounts for them).
#define BATTERY_POLL_STACK_BYTES 3072
#define FPS_POLL_STACK_BYTES     2048

// Auto-generated public interface for sysstats.c (review/trim).
int battery_draw(int y);
void battery_poll_ensure_started(void);
int battery_poll_thread(SceSize args, void *argp);
void battery_refresh(void);
void cpu_usage_tick(u32 now);
void fps_calc_1pct_low(u32 now);
int fps_display_set_frame_buf_patched(void *topaddr, int bufferwidth, int pixelformat, int sync);
void fps_draw(void *topaddr, int bufferwidth, int pixelformat);
void fps_poll_ensure_started(void);
int fps_poll_thread(SceSize args, void *argp);
void fps_tick(u32 now);
u32 fps_window_us(void);
void frame_limit_ge(void);
u32 frame_limit_target_us(int fps);
void ft_chart_draw(void);
void ft_chart_tick(u32 delta_us);
extern int g_battery_poll_started;
extern int g_fps_poll_started;
void gpu_usage_sample(void);
void install_fps_overlay_hook(void);

#endif
