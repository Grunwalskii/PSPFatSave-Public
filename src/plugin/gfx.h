#ifndef FS_GFX_H
#define FS_GFX_H

#include "pspfatsave.h"

// Extra pixels added past the row grid at the progress-band bottom (used by
// dbg_fill_band_color; the prog panel in fatsave.c sizes to match).
#define PANEL_EXTRA_PX 10

// Auto-generated public interface for gfx.c (review/trim).
u16 c4444_to565(u16 c);
u16 c5551_to565(u16 c);
extern u32    dbg_bg;       // background color
extern int    dbg_bufw;     // pixels per row (pitch)
extern int    dbg_col;      // current column (chars)
extern int    dbg_row;      // current row (chars)
extern u32    dbg_fg;       // foreground color (0xAABBGGRR)
extern int    dbg_transparent;
extern int dbg_buf_count;
void dbg_capture_both_bufs(void);
extern void  *dbg_fb;      // KSEG1 framebuffer base;
void dbg_fill_band_color(int row0, int nrows, u32 color);
void dbg_fill_rect(int px, int py, int w, int h, u32 color);
void dbg_fill_rect_all(int px, int py, int w, int h, u32 color);
void dbg_init(void);
extern int    dbg_pfmt;     // pixel format;
void dbg_print(const char *str);
void dbg_print_all(const char *str);
void dbg_putchar(char ch);
void dbg_text(int col, int row, u32 fg, u32 bg, const char *s);
extern const u8 font8x8[96][8];
u32 from565(u16 c);
u16 img16_from565(u16 c565, int pfmt);
u16 pack16_fmt(u32 c, int pfmt);
void pin_current_display(void);
void reassert_display(void);
u16 to565(u32 c);

#endif
