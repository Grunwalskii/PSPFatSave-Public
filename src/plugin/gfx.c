#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// 8×8 font — subset of ASCII 32-127
const u8 font8x8[96][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
	{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
	{0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
	{0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
	{0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // '$'
	{0x00,0x66,0xAC,0xD8,0x36,0x6A,0xCC,0x00}, // '%'
	{0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
	{0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '''
	{0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
	{0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
	{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
	{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
	{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
	{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
	{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
	{0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
	{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // '0'
	{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
	{0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0x00}, // '2'
	{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // '3'
	{0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00}, // '4'
	{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // '5'
	{0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, // '6'
	{0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00}, // '7'
	{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // '8'
	{0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}, // '9'
	{0x00,0x00,0x18,0x00,0x00,0x18,0x00,0x00}, // ':'
	{0x00,0x00,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
	{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // '<'
	{0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, // '='
	{0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // '>'
	{0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00}, // '?'
	{0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00}, // '@'
	{0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 'A'
	{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // 'B'
	{0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 'C'
	{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // 'D'
	{0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}, // 'E'
	{0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, // 'F'
	{0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, // 'G'
	{0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 'H'
	{0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 'I'
	{0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 'J'
	{0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 'K'
	{0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // 'L'
	{0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // 'M'
	{0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // 'N'
	{0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'O'
	{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 'P'
	{0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // 'Q'
	{0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // 'R'
	{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // 'S'
	{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 'T'
	{0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'U'
	{0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'V'
	{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W'
	{0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 'X'
	{0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 'Y'
	{0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 'Z'
	{0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
	{0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\'
	{0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
	{0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
	{0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
	{0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, // 'a'
	{0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 'b'
	{0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00}, // 'c'
	{0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // 'd'
	{0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // 'e'
	{0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00}, // 'f'
	{0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, // 'g'
	{0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'h'
	{0x18,0x00,0x18,0x18,0x18,0x18,0x0C,0x00}, // 'i'
	{0x0C,0x00,0x0C,0x0C,0x0C,0x0C,0x0C,0x38}, // 'j'
	{0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // 'k'
	{0x18,0x18,0x18,0x18,0x18,0x18,0x0C,0x00}, // 'l'
	{0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, // 'm'
	{0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'n'
	{0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // 'o'
	{0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // 'p'
	{0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, // 'q'
	{0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // 'r'
	{0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, // 's'
	{0x30,0x30,0x7E,0x30,0x30,0x30,0x1C,0x00}, // 't'
	{0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // 'u'
	{0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'v'
	{0x00,0x00,0x63,0x6B,0x7F,0x36,0x22,0x00}, // 'w'
	{0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // 'x'
	{0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // 'y'
	{0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 'z'
	{0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00}, // '{'
	{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
	{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // '}'
	{0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00}, // '~'
};

// Global debug-screen state — shared by all freeze_cp / dbg_ calls
void  *dbg_fb;      // KSEG1 framebuffer base
int    dbg_bufw;     // pixels per row (pitch)
int    dbg_pfmt;     // pixel format
int    dbg_col;      // current column (chars)
int    dbg_row;      // current row (chars)
u32    dbg_fg;       // foreground color (32-bit 0xAABBGGRR)
u32    dbg_bg;       // background color
int    dbg_transparent = 0;  // when set, dbg_putchar draws only glyph (fg) pixels, leaving the existing background untouched

void dbg_init(void)
{
	void *addr;
	int bufw, pfmt;
	sceDisplayGetFrameBuf(&addr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
	if (!addr || bufw <= 0) return;
	dbg_fb   = (void *)(0xA0000000 | (u32)addr);
	dbg_bufw = bufw;
	dbg_pfmt = pfmt;
	dbg_col  = 0;
	dbg_row  = 0;
	dbg_fg   = 0xFFFFFFFF; // white
	dbg_bg   = 0xFF000000; // transparent black
}

// Convert 32-bit AABBGGRR to 16-bit 565
u16 to565(u32 c) {
	return (u16)(((c >> 3) & 0x1F) | ((c >> 5) & 0x7E0) | ((c >> 8) & 0xF800));
}
// Convert 16-bit 565 back to 32-bit AABBGGRR (opaque).
u32 from565(u16 c) {
	u32 r = (u32)(c & 0x1F) << 3;
	u32 g = (u32)((c >> 5) & 0x3F) << 2;
	u32 b = (u32)((c >> 11) & 0x1F) << 3;
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}
// Convert the two OTHER 16-bit PSP display formats to 565. All three are ABGR (R in
// the low bits — see pspsdk scr_printf.c convert_8888_to_5551/565 + gu/doc/commands.txt):
//   565  = B5 G6 R5 : R@0, G@5(6b), B@11(5b)
//   5551 = A1 B5 G5 R5 : R@0, G@5(5b), B@10(5b)   -> expand G 5->6 bits
//   4444 = A4 B4 G4 R4 : R@0, G@4, B@8            -> expand R/B 4->5, G 4->6
// Thumbnails/screenshots are stored 565, so a 5551/4444 source MUST be converted or its
// colors come out wrong (the "other games' thumbnails look off" bug — those games render
// in 5551/4444). Bit-replicate on expansion so full-scale stays full-scale.
u16 c5551_to565(u16 c) {
	u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
	g = (g << 1) | (g >> 4);                       // 5 -> 6 bits
	return (u16)(r | (g << 5) | (b << 11));
}
u16 c4444_to565(u16 c) {
	u32 r = c & 0xF, g = (c >> 4) & 0xF, b = (c >> 8) & 0xF;
	r = (r << 1) | (r >> 3);                       // 4 -> 5 bits
	g = (g << 2) | (g >> 2);                       // 4 -> 6 bits
	b = (b << 1) | (b >> 3);                       // 4 -> 5 bits
	return (u16)(r | (g << 5) | (b << 11));
}
// Pack 32-bit AABBGGRR -> the other two 16-bit formats (exact layouts from pspsdk
// scr_printf.c convert_8888_to_5551 / _4444; our color word is ABGR, R low, matching).
static u16 to5551(u32 c) {
	u32 a = (c >> 24) ? 0x8000u : 0u;
	return (u16)(a | ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
}
static u16 to4444(u32 c) {
	return (u16)((((c >> 28) & 0xF) << 12) | ((c >> 4) & 0xF) | (((c >> 12) & 0xF) << 4) | (((c >> 20) & 0xF) << 8));
}
// Pack an AABBGGRR color into a given 16-bit display format (for our solid-color UI:
// text glyphs, fills). Every 16-bit draw MUST use this, not a bare to565() — writing a
// 565 value into a 5551/4444 framebuffer garbles the color (the "same game screenshot
// wrong" regression: previews are stored true-565 now, but a 5551/4444 game's display
// needs them repacked to ITS format).
u16 pack16_fmt(u32 c, int pfmt) {
	if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_5551) return to5551(c);
	if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_4444) return to4444(c);
	return to565(c);                               // 565 (and any other) -> 565
}
// Convert a stored-565 image pixel (thumbnail/screenshot) to a given 16-bit display
// format. 565 display: pass through raw (no round-trip loss); 5551/4444: repack.
u16 img16_from565(u16 c565, int pfmt) {
	if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_5551) return to5551(from565(c565));
	if (pfmt == PSP_DISPLAY_PIXEL_FORMAT_4444) return to4444(from565(c565));
	return c565;
}

void dbg_putchar(char ch)
{
	int i, j;
	const u8 *glyph;
	int ci = (unsigned char)ch;

	// Hard backstop: never draw outside the captured framebuffer, regardless of what
	// row/col a caller computed — same overflow class v547 fixed for the panel fill
	// (a distant constant change silently pushed a row/col past the buffer edge).
	if (dbg_row < 0 || dbg_col < 0) return;
	if (dbg_row * 8 + 8 > 272) return;
	if (dbg_col * 8 + 8 > dbg_bufw) return;

	if (ci < 32 || ci > 126) ci = 32; // replace non-printables with space
	glyph = font8x8[ci - 32];

	if (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
		volatile u32 *vram = (volatile u32 *)dbg_fb
			+ dbg_col * 8 + dbg_row * 8 * dbg_bufw;
		u32 fg = dbg_fg, bg = dbg_bg;
		for (i = 0; i < 8; i++) {
			volatile u32 *row = vram;
			u8 bits = glyph[i];
			for (j = 0; j < 8; j++) {
				if (bits & (128 >> j)) row[j] = fg;
				else if (!dbg_transparent) row[j] = bg;
			}
			vram += dbg_bufw;
		}
	} else {
		volatile u16 *vram = (volatile u16 *)dbg_fb
			+ dbg_col * 8 + dbg_row * 8 * dbg_bufw;
		u16 fg = pack16_fmt(dbg_fg, dbg_pfmt), bg = pack16_fmt(dbg_bg, dbg_pfmt);
		for (i = 0; i < 8; i++) {
			volatile u16 *row = vram;
			u8 bits = glyph[i];
			for (j = 0; j < 8; j++) {
				if (bits & (128 >> j)) row[j] = fg;
				else if (!dbg_transparent) row[j] = bg;
			}
			vram += dbg_bufw;
		}
	}
}

void dbg_print(const char *str)
{
	char c;
	while ((c = *str++) != '\0') {
		if (c == '\n') {
			dbg_col = 0;
			dbg_row++;
			if (dbg_row >= 34) dbg_row = 0;
			continue;
		}
		dbg_putchar(c);
		dbg_col++;
		if (dbg_col >= dbg_bufw / 8) {   // wrap at the buffer's real pixel stride, not a guessed column count
			dbg_col = 0;
			dbg_row++;
			if (dbg_row >= 34) dbg_row = 0;
		}
	}
}

// Capture both framebuffers (draw+display) before freeze so we can write to both
struct dbg_fb_info {
	void *fb;
	int   bufw;
	int   pfmt;
};
static struct dbg_fb_info dbg_bufs[2];
int dbg_buf_count;

// Display framebuffer captured (raw addr, before any cache-alias) for re-asserting the
// screen after a game blanks it: GTA disables the display in its SUSPENDING handler (see
// cooperative_volmem_release), so our overlay/prompt would otherwise draw to an off-screen
// buffer. The display driver is a kernel thread (not frozen), so re-pointing it mid-freeze
// is safe.
static void *g_disp_addr = NULL;
static int   g_disp_bufw = 0, g_disp_pfmt = 0;

void reassert_display(void)
{
	// NEXTFRAME (vsync), not IMMEDIATE — sceDisplaySetFrameBuf rejects IMMEDIATE here with
	// SCE_DISPLAY_ERROR_ARGUMENT. k1=0 so the framebuffer-address validation passes (as the
	// ctrl peek does). The captured addr/bufw/pfmt are the game's own (valid) values.
	if (g_disp_addr) {
		int k1 = pspSdkSetK1(0);
		sceDisplaySetFrameBuf(g_disp_addr, g_disp_bufw, g_disp_pfmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
		pspSdkSetK1(k1);
	}
}

// Post-freeze: pin the display to the buffer ACTUALLY on screen right now and adopt
// its exact geometry as our single draw target. Used for games that do NOT blank in a
// power callback (g_game_pcb_count == 0). Their pre-freeze capture in
// dbg_capture_both_bufs() can be STALE: the game keeps flipping between that capture
// and the freeze — possibly to a THIRD framebuffer we never captured (triple buffering)
// or with a stride the display controller uses that no longer matches. That showed up
// as the save/load banner landing on an off-screen buffer (no banner at all) or at a
// mismatched stride (the ~50% horizontal shift), intermittently and game-dependent
// (e.g. Pirates). Reading GetFrameBuf(IMMEDIATE) AFTER the freeze — game stopped, front
// buffer stable — gives the true current buffer + width/format; we draw ONLY to it and
// SetFrameBuf it back so the controller's stride matches our draw stride. Blanking games
// (pcb>0) still use reassert_display() with their pre-blank capture (a post-freeze
// GetFrameBuf there would return the game's blanked buffer). Register/​syscall-safe here:
// the display driver is a kernel thread (not frozen). k1=0 so the address validation
// passes, exactly like reassert_display().
void pin_current_display(void)
{
	void *addr = NULL; int bufw = 0, pfmt = 0;
	int k1 = pspSdkSetK1(0);
	if (sceDisplayGetFrameBuf(&addr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_IMMEDIATE) == 0
		&& addr && bufw > 0) {
		dbg_bufs[0].fb   = (void *)(0xA0000000 | (u32)addr);
		dbg_bufs[0].bufw = bufw;
		dbg_bufs[0].pfmt = pfmt;
		dbg_buf_count    = 1;
		g_disp_addr = addr; g_disp_bufw = bufw; g_disp_pfmt = pfmt;  // keep reassert_display() consistent too
		sceDisplaySetFrameBuf(addr, bufw, pfmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
	}
#if DEBUG_BUILD
	if (DBG_UART()) {
		uart_log_hex("[FBPIN] addr=", (u32)addr);
		uart_log_hex("[FBPIN] bufw=", (u32)bufw);
		uart_log_hex("[FBPIN] pfmt=", (u32)pfmt);
	}
#endif
	pspSdkSetK1(k1);
}

void dbg_capture_both_bufs(void)
{
	dbg_buf_count = 0;
	void *addr;
	int bufw, pfmt;
	u32 kseg1_addr[2] = {0, 0};

	// immediate (draw) buffer — index 0
	if (sceDisplayGetFrameBuf(&addr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_IMMEDIATE) == 0
		&& addr && bufw > 0) {
		kseg1_addr[0] = 0xA0000000 | (u32)addr;
		dbg_bufs[0].fb   = (void *)kseg1_addr[0];
		dbg_bufs[0].bufw = bufw;
		dbg_bufs[0].pfmt = pfmt;
		dbg_buf_count = 1;
		g_disp_addr = addr; g_disp_bufw = bufw; g_disp_pfmt = pfmt;  // for reassert_display()
	}
	// next frame (display) buffer — index 1 (if different from index 0)
	if (sceDisplayGetFrameBuf(&addr, &bufw, &pfmt, PSP_DISPLAY_SETBUF_NEXTFRAME) == 0
		&& addr && bufw > 0) {
		kseg1_addr[1] = 0xA0000000 | (u32)addr;
		// Strip cache flags to compare raw VRAM addresses
		if (dbg_buf_count == 0 || (kseg1_addr[1] & 0x3FFFFFFF) != (kseg1_addr[0] & 0x3FFFFFFF)) {
			dbg_bufs[dbg_buf_count].fb   = (void *)kseg1_addr[1];
			dbg_bufs[dbg_buf_count].bufw = bufw;
			dbg_bufs[dbg_buf_count].pfmt = pfmt;
			dbg_buf_count++;
		}
	}
	if (dbg_buf_count == 0) {
		// fallback to single-buffer mode
		dbg_init();
		// Assign fields individually — do NOT type-pun &dbg_fb as a struct (the three
		// globals aren't guaranteed to be laid out like dbg_fb_info). Mirrors :375.
		dbg_bufs[0].fb   = dbg_fb;
		dbg_bufs[0].bufw = dbg_bufw;
		dbg_bufs[0].pfmt = dbg_pfmt;
		dbg_buf_count = 1;
	}
#if DEBUG_BUILD
	// Pre-freeze geometry, for comparison with the post-freeze [FBPIN] line: a mismatch
	// (different bufw/pfmt, or a third front buffer) is the banner-shift / no-banner cause.
	if (DBG_UART()) {
		int b;
		uart_log_hex("[FBCAP] count=", (u32)dbg_buf_count);
		for (b = 0; b < dbg_buf_count; b++) {
			uart_log_hex("[FBCAP] fb=",   (u32)dbg_bufs[b].fb);
			uart_log_hex("[FBCAP] bufw=", (u32)dbg_bufs[b].bufw);
			uart_log_hex("[FBCAP] pfmt=", (u32)dbg_bufs[b].pfmt);
		}
	}
#endif
}

// Write text to all captured buffers at the current global dbg position
void dbg_print_all(const char *str)
{
	int i;
	struct dbg_fb_info saved = { dbg_fb, dbg_bufw, dbg_pfmt };
	for (i = 0; i < dbg_buf_count; i++) {
		dbg_fb   = dbg_bufs[i].fb;
		dbg_bufw = dbg_bufs[i].bufw;
		dbg_pfmt = dbg_bufs[i].pfmt;
		dbg_print(str);
	}
	dbg_fb   = saved.fb;
	dbg_bufw = saved.bufw;
	dbg_pfmt = saved.pfmt;
}
// Fill a band SOLID with an AABBGGRR color across every captured framebuffer.
void dbg_fill_band_color(int row0, int nrows, u32 color)
{
	int b, y, x;
	int y0 = row0 * 8, y1 = (row0 + nrows) * 8 + PANEL_EXTRA_PX;   // +extra px past the row grid (see PANEL_EXTRA_PX)
	// CLAMP to the 272-row display: with PANEL_TOP=15, PANEL_H=18, PANEL_EXTRA_PX=10 the
	// band bottom is (15+18)*8+10 = 274 — 2 rows PAST the framebuffer. Those out-of-bounds
	// writes clobber ~2-4KB of VRAM just past the game's framebuffer (its other buffer /
	// textures / GE data) on every save, faulting the game seconds later when it renders —
	// THE per-attempt-random GTA-after-save freeze. v481 fit (266); v482 grew the panel and
	// started overflowing (274). (Row grid stays intact; only the extra pad is trimmed.)
	if (y1 > 272) y1 = 272;
	for (b = 0; b < dbg_buf_count; b++) {
		int w = dbg_bufs[b].bufw;
		if (dbg_bufs[b].pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
			volatile u32 *fb = (volatile u32 *)dbg_bufs[b].fb;
			for (y = y0; y < y1; y++) for (x = 0; x < 480; x++) fb[y * w + x] = color;
		} else {
			u16 c16 = pack16_fmt(color, dbg_bufs[b].pfmt);   // per-buffer format (565/5551/4444)
			volatile u16 *fb = (volatile u16 *)dbg_bufs[b].fb;
			for (y = y0; y < y1; y++) for (x = 0; x < 480; x++) fb[y * w + x] = c16;
		}
	}
}
// Fill a pixel rectangle across EVERY captured framebuffer (the pre-/post-suspend-safe
// path, same buffer set dbg_print_all / dbg_fill_band_color use). For banner chrome —
// progress bars, prompt pills — that must land on whichever buffer is actually on screen.
void dbg_fill_rect_all(int px, int py, int w, int h, u32 color)
{
	int i;
	struct dbg_fb_info saved = { dbg_fb, dbg_bufw, dbg_pfmt };
	for (i = 0; i < dbg_buf_count; i++) {
		dbg_fb   = dbg_bufs[i].fb;
		dbg_bufw = dbg_bufs[i].bufw;
		dbg_pfmt = dbg_bufs[i].pfmt;
		dbg_fill_rect(px, py, w, h, color);
	}
	dbg_fb   = saved.fb;
	dbg_bufw = saved.bufw;
	dbg_pfmt = saved.pfmt;
}
// Fill a pixel rectangle in the current framebuffer (8888 or 16-bit).
void dbg_fill_rect(int px, int py, int w, int h, u32 color)
{
	int x, y;
	if (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
		for (y = 0; y < h; y++) {
			volatile u32 *row = (volatile u32 *)dbg_fb + (py + y) * dbg_bufw + px;
			for (x = 0; x < w; x++) row[x] = color;
		}
	} else {
		u16 c = pack16_fmt(color, dbg_pfmt);
		for (y = 0; y < h; y++) {
			volatile u16 *row = (volatile u16 *)dbg_fb + (py + y) * dbg_bufw + px;
			for (x = 0; x < w; x++) row[x] = c;
		}
	}
}

void dbg_text(int col, int row, u32 fg, u32 bg, const char *s)
{
	dbg_col = col; dbg_row = row; dbg_fg = fg; dbg_bg = bg;
	dbg_print(s);
}
