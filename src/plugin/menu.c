#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"
#include "screen_tuning.h"
#include "cheats.h"

// Set when the menu is opened by the boot auto-open (vs a NOTE tap): the browser then starts
// on the newest save (for a quick load) regardless of the Default Slot setting. One-shot.
static int g_autoopen_launch = 0;
// Set by the pad hook (GAME thread) to ask the MENU thread to evaluate boot auto-open. The
// menu thread does the MS check on its own (safe) thread.
static int g_autoopen_pending = 0;
// run_save_browser() sets this to 1 the moment the freeze succeeds and it actually opens; it
// stays 0 if the freeze aborted (game too busy). Lets the boot auto-open retry until it opens.
static int g_browser_opened = 0;
// Read the running game's Auto-Open-on-Boot flag from its per-game settings file.
// Self-contained (used by the boot hook before the settings helpers are defined).
static int game_autoopen_enabled(void)
{
	char p[96]; SceUID fd; u32 buf[2]; int en = 0;
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/gameset.cfg", umdid[0] ? umdid : "globalstate");
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return 0;
	if (sceIoRead(fd, buf, sizeof(buf)) == (int)sizeof(buf) && buf[0] == GAMESET_MAGIC)
		en = buf[1] ? 1 : 0;
	sceIoClose(fd);
	return en;
}

// Load the RUNNING game's Frame Limit from its per-game settings file into g_frame_limit
// (must be called from MENU thread, not game thread).
void game_frame_limit_load(void)
{
	char p[96]; SceUID fd; u32 buf[4]; int n;
	g_frame_limit = 0;                     // default OFF (no file / pre-Frame-Limit file)
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/gameset.cfg", umdid[0] ? umdid : "globalstate");
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return;
	n = sceIoRead(fd, buf, sizeof(buf));
	if (n >= (int)(4 * sizeof(u32)) && buf[0] == GAMESET_MAGIC) {
		int fl = (int)buf[3];              // sanitize: only OFF or 20..60
		g_frame_limit = (fl >= 20 && fl <= 60) ? fl : 0;
	}
	sceIoClose(fd);
}
// True if the running game already has at least one .bin save in its folder.
// Used by the boot auto-open — a light directory scan, run once from the hook.
static int game_has_save(void)
{
	char dir[80]; SceUID dfd; SceIoDirent ent; int found = 0;
	sprintf(dir, "ms0:/seplugins/SAVESTATE/%s", umdid[0] ? umdid : "globalstate");
	dfd = sceIoDopen(dir);
	if (dfd < 0) return 0;
	memset(&ent, 0, sizeof(ent));
	while (sceIoDread(dfd, &ent) > 0) {
		const char *n = ent.d_name; int len = (int)strlen(n);
		if (len >= 5 && strcmp(n + len - 4, ".bin") == 0) { found = 1; break; }
		memset(&ent, 0, sizeof(ent));
	}
	sceIoDclose(dfd);
	return found;
}

// Called from the syscall-patched controller reads every frame. Opens the save
// browser on a SHORT TAP of NOTE (press+release < NOTE_TAP_US). A long hold is
// the game's mute, so we leave NOTE alone and only decide on release. We don't
// mask NOTE — the game still gets it (mute-on-hold keeps working); a tap is a
// brief blip. The triggering tap is already released when the menu opens, so the
// browser starts with buttons up (no debounce needed).
void PspLsLibraryLauncher(SceCtrlData *pad_data)
{
	static int note_was_down = 0;
	static u64 note_down_at = 0;
	int note_down = (pad_data->Buttons & PSP_CTRL_NOTE) ? 1 : 0;

	if (g_menu_open) { note_was_down = note_down; return; }  // browser up — track edge only

	// Boot auto-open (once): on the first controller read just SIGNAL the menu thread; do
	// not read the MS here. The menu thread does the per-game check + opens on its own
	// (safe) thread, and retries as the game settles.
	if (g_autoload_armed) {
		g_autoload_armed = 0;
		g_autoopen_pending = 1;
		if (g_menu_thid >= 0)
			sceKernelWakeupThread(g_menu_thid);
		note_was_down = note_down;
		return;
	}

	if (note_down && !note_was_down) {
		note_down_at = now_us();                 // NOTE pressed
	} else if (!note_down && note_was_down) {    // NOTE released
		if (now_us() - note_down_at < NOTE_TAP_US) {  // quick tap -> open browser
			g_menu_open = 1;
			if (g_menu_thid >= 0)
				sceKernelWakeupThread(g_menu_thid);
		}
		// else: long hold = the game's mute; do nothing
	}
	note_was_down = note_down;
}

// Syscall-patched controller reads (ARK-4 vshctrl pattern): main.c uses
// sctrlHENPatchSyscall to redirect the real sceController_Service Peek/Read here,
// so the GAME's own per-frame controller syscalls run through us. g_real_ctrl_*
// (defined near the top) are the real functions, set in main.c.

// NOTE is a KERNEL-only button: the game's USER-mode read masks it out, so the
// game's pad never shows it. To detect the NOTE tap we do our OWN kernel-mode
// peek (k1=0 -> unmasked, includes NOTE) into a scratch pad and run detection on
// that. The game's own pad is returned untouched (mute-on-hold still reaches it).
// ── Live gamma HUD input ──
// Runs off the SAME kernel-mode peek the NOTE tap uses, so it sees the real pad
// regardless of what we blank from the game (st_hud_mask). Edge-triggered:
// one step per press, which is plenty for a 10-step range and avoids any
// auto-repeat timing on the game's thread.
// g_hud_prev starts all-1s so the very first sample can never register a press
// (a stale edge here would close the HUD the instant it opened).
static u32 g_hud_prev = 0xFFFFFFFFu;

static int fl_repeat_fire(int hold);   // forward — defined below run_st_test

void st_hud_open(void)
{
	g_hud_prev = 0xFFFFFFFFu;
	g_st_hud = 1;
}

// Hold-to-repeat state for the live HUD (level-driven, like Frame Limit).
static int gh_hold_l, gh_hold_r, gh_hold_u, gh_hold_d;

static void st_hud_input(u32 buttons)
{
	// Level-driven auto-repeat for all four directions.
	// L: colour temp down
	if (buttons & PSP_CTRL_LEFT) {
		gh_hold_l++; gh_hold_r = 0;
		if (fl_repeat_fire(gh_hold_l) && g_st_temp > 0) {
			g_st_temp--; g_st_hud_dirty = 1;
		}
	} else {
		if (gh_hold_l >= 1 && gh_hold_l < 5 && g_st_temp > 0) {
			g_st_temp--; g_st_hud_dirty = 1;
		}
		gh_hold_l = 0;
	}
	// R: colour temp up
	if (buttons & PSP_CTRL_RIGHT) {
		gh_hold_r++; gh_hold_l = 0;
		if (fl_repeat_fire(gh_hold_r) && g_st_temp < 200) {
			g_st_temp++; g_st_hud_dirty = 1;
		}
	} else {
		if (gh_hold_r >= 1 && gh_hold_r < 5 && g_st_temp < 200) {
			g_st_temp++; g_st_hud_dirty = 1;
		}
		gh_hold_r = 0;
	}
	// UP: gamma up (brighter, 1.01..2.00)
	if (buttons & PSP_CTRL_UP) {
		gh_hold_u++; gh_hold_d = 0;
		if (fl_repeat_fire(gh_hold_u) && g_st_gamma < 200) {
			g_st_gamma++; g_st_hud_dirty = 1;
		}
	} else {
		if (gh_hold_u >= 1 && gh_hold_u < 5 && g_st_gamma < 200) {
			g_st_gamma++; g_st_hud_dirty = 1;
		}
		gh_hold_u = 0;
	}
	// DOWN: gamma down (darker, 0.50..0.99)
	if (buttons & PSP_CTRL_DOWN) {
		gh_hold_d++; gh_hold_u = 0;
		if (fl_repeat_fire(gh_hold_d) && g_st_gamma > 50) {
			g_st_gamma--; g_st_hud_dirty = 1;
		}
	} else {
		if (gh_hold_d >= 1 && gh_hold_d < 5 && g_st_gamma > 50) {
			g_st_gamma--; g_st_hud_dirty = 1;
		}
		gh_hold_d = 0;
	}

	// Close on new press of X/O (edge-driven — don't want hold to close).
	{
		u32 pressed = buttons & ~g_hud_prev;
		g_hud_prev = buttons;
		if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)) g_st_hud = 0;
	}
	// If the user just turned gamma/temp back on from neutral, restart the
	// worker — it exits when st_active() goes false (gamma=1.0, temp=100).
	if (st_active()) st_ensure_started();
}

// While the HUD is up the D-Pad and X/O are OURS, not the game's — otherwise
// tuning gamma also steers the car. Blank them from every buffer read the game makes.
#define ST_HUD_MASK (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CROSS | PSP_CTRL_CIRCLE)
static void st_hud_mask(SceCtrlData *pad_data, int count, int res, int negative)
{
	int i, n;
	if (!g_st_hud || !pad_data || res <= 0) return;
	n = (res < count) ? res : count;
	for (i = 0; i < n; i++) {
		if (negative) pad_data[i].Buttons |=  ST_HUD_MASK;   // negative logic: 1 = not pressed
		else          pad_data[i].Buttons &= ~ST_HUD_MASK;
	}
}

static void detect_note_tap(void)
{
	SceCtrlData kpad;
	// Whole body under k1=0 (not just the peek): the launcher runs kernel calls
	// with kernel-stack pointers from the GAME's syscall context. kpeek's own
	// k1 save/restore nests harmlessly inside this bracket.
	int k1 = pspSdkSetK1(0);
	if (kpeek(&kpad) > 0) {
		// !g_menu_open: the browser/settings screens drive themselves off this same
		// kernel pad, so without this the D-Pad would move a menu row AND retune
		// gamma at once.
		if (g_st_hud && !g_menu_open) st_hud_input(kpad.Buttons);
		PspLsLibraryLauncher(&kpad);
	}
	pspSdkSetK1(k1);
}

int sceCtrlPeekBufferPositivePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_peek ? g_real_ctrl_peek(pad_data, count)
	                           : sceCtrlPeekBufferPositive(pad_data, count);
	st_hud_mask(pad_data, count, res, 0);          // BEFORE vskip, or it would wipe the injected press
	vskip_inject_buttons(pad_data, count, res, 0);
	detect_note_tap();
	return res;
}

// Blank controller ring slots sampled during the menu (identified by their timestamp,
// at/before the resume arm) so a Read-based game doesn't replay the menu's presses as
// delayed "live" input. The first slot stamped after the arm passes through and suppression
// ends. g_suppress_posbuf_calls stays as an UPPER BOUND only (safety, in case a game never
// presents a fresh-stamped slot). Armed via arm_input_suppress().
void suppress_posbuf_slots(SceCtrlData *pad_data, int count, int res, u32 clean_value)
{
	int i, n;
	if (g_suppress_posbuf_calls <= 0 || !pad_data || res <= 0) return;
	n = (res < count) ? res : count;
	for (i = 0; i < n; i++) {
		// Signed delta handles the 32-bit microsecond wrap. >0 => sampled after the arm =>
		// real input the user made post-resume: stop blanking here and pass it through.
		if ((s32)(pad_data[i].TimeStamp - g_suppress_posbuf_ts) > 0) {
			g_suppress_posbuf_calls = 0;
			return;
		}
		pad_data[i].Buttons = clean_value;
		g_suppress_posbuf_calls--;
		if (g_suppress_posbuf_calls <= 0) { g_suppress_posbuf_calls = 0; return; }
	}
}

int sceCtrlReadBufferPositivePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_read ? g_real_ctrl_read(pad_data, count)
	                           : sceCtrlReadBufferPositive(pad_data, count);
	suppress_posbuf_slots(pad_data, count, res, 0);   // positive logic: 0 = nothing pressed
	st_hud_mask(pad_data, count, res, 0);
	vskip_inject_buttons(pad_data, count, res, 0);    // AFTER the suppress, or it would wipe the injected press
	detect_note_tap();
	return res;
}

// Peek never carries backlog — pure passthrough, kept only so NOTE-tap detection
// still fires for a game that reads this way.
int sceCtrlPeekBufferNegativePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_peek_neg ? g_real_ctrl_peek_neg(pad_data, count)
	                               : sceCtrlPeekBufferNegative(pad_data, count);
	st_hud_mask(pad_data, count, res, 1);
	vskip_inject_buttons(pad_data, count, res, 1);
	detect_note_tap();
	return res;
}

// Same drain as ReadBufferPositive, with the negative-logic "clean" value (all 1s).
int sceCtrlReadBufferNegativePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_read_neg ? g_real_ctrl_read_neg(pad_data, count)
	                               : sceCtrlReadBufferNegative(pad_data, count);
	suppress_posbuf_slots(pad_data, count, res, 0xFFFFFFFFu);   // negative logic: all-1s = nothing pressed
	st_hud_mask(pad_data, count, res, 1);
	vskip_inject_buttons(pad_data, count, res, 1);              // AFTER the suppress, or it would wipe the injected press
	detect_note_tap();
	return res;
}

// While g_suppress_latch is set (armed by arm_input_suppress()), zero the returned
// latch edges so the game never sees the menu's stale presses. SELF-CLEARING: a
// single drained read empties the latch accumulator, so the first hit while armed
// clears g_suppress_latch back to 0 itself.
int sceCtrlReadLatchPatched(SceCtrlLatch *latch)
{
	int res = g_real_ctrl_readlatch ? g_real_ctrl_readlatch(latch)
	                                : sceCtrlReadLatch(latch);
	if (g_suppress_latch && latch) {
		latch->uiMake = 0; latch->uiBreak = 0; latch->uiPress = 0; latch->uiRelease = 0;
		g_suppress_latch = 0;
		return 0;
	}
	vskip_inject_latch(latch);   // reach latch-reading games the buffer injection can't (see the function)
	return res;
}

int sceCtrlPeekLatchPatched(SceCtrlLatch *latch)
{
	if (g_suppress_latch && latch && g_real_ctrl_readlatch) {
		g_real_ctrl_readlatch(latch);   // drain the user latch (as Read) inside the window
		latch->uiMake = 0; latch->uiBreak = 0; latch->uiPress = 0; latch->uiRelease = 0;
		g_suppress_latch = 0;
		return 0;
	}
	{
		int res = g_real_ctrl_peeklatch ? g_real_ctrl_peeklatch(latch)
		                                : sceCtrlPeekLatch(latch);
		vskip_inject_latch(latch);   // reach latch-reading games the buffer injection can't
		return res;
	}
}

// Read a game folder's title.txt (written at save time) into out; "" if absent.
static void read_folder_title(const char *folder, char *out, int sz)
{
	char p[96]; SceUID fd; int n;
	out[0] = 0;
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/title.txt", folder);
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return;
	n = sceIoRead(fd, out, sz - 1);
	sceIoClose(fd);
	if (n < 0) n = 0;
	out[n] = 0;
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
		out[--n] = 0;
}

// Header text shared by the save browser and settings screens: the game's title
// (from its folder's title.txt) when known, else its GameID. `out` must be large
// enough for "PSPFatSave v<ver>  " + up to 47 chars of title/id.
static void format_game_header(char *out, const char *gid)
{
	char gtitle[48];
	read_folder_title(gid, gtitle, sizeof(gtitle));
	if (gtitle[0]) sprintf(out, "PSPFatSave v%s  %s", VERSION_STRING, gtitle);
	else           sprintf(out, "PSPFatSave v%s  GameID: %s", VERSION_STRING, gid);
}

struct save_row {
	char           name[64];
	u32            size;
	ScePspDateTime mtime;
};
static struct save_row g_rows[MAX_SAVE_ROWS];  // BSS ~2.7KB
static int g_row_count;
// 1 = the list has a leading "New Savegame" row (idx 0); 0 = saves only (auto-open
// load view). When 0, save idx i maps to g_rows[i]; when 1, to g_rows[i-1].
static int g_show_newsave = 1;
char g_browse_dir[64];   // current game's save folder (for thumbnail paths)

// Sortable key (newest-first when compared descending).
static u64 dt_key(const ScePspDateTime *d)
{
	return ((u64)d->year << 40) | ((u64)d->month << 32) | ((u64)d->day << 24)
	     | ((u64)d->hour << 16) | ((u64)d->minute << 8) | (u64)d->second;
}

// Enumerate *.bin in the game's save folder into g_rows[], newest first.
static void enumerate_saves(const char *dir)
{
	SceUID dfd;
	SceIoDirent ent;
	int i, j;

	g_row_count = 0;
	dfd = sceIoDopen(dir);
	if (dfd < 0) return;

	memset(&ent, 0, sizeof(ent));
	while (sceIoDread(dfd, &ent) > 0 && g_row_count < MAX_SAVE_ROWS) {
		const char *n = ent.d_name;
		int len = (int)strlen(n);
		if (len >= 5 && strcmp(n + len - 4, ".bin") == 0) {  // skip . .. and non-saves
			struct save_row *r = &g_rows[g_row_count++];
			strncpy(r->name, n, sizeof(r->name) - 1);
			r->name[sizeof(r->name) - 1] = 0;
			r->size  = (u32)ent.d_stat.st_size;
			r->mtime = ent.d_stat.st_mtime;
		}
		memset(&ent, 0, sizeof(ent));
	}
	sceIoDclose(dfd);

	// insertion sort by mtime, descending (newest first)
	for (i = 1; i < g_row_count; i++) {
		struct save_row tmp = g_rows[i];
		u64 k = dt_key(&tmp.mtime);
		for (j = i - 1; j >= 0 && dt_key(&g_rows[j].mtime) < k; j--)
			g_rows[j + 1] = g_rows[j];
		g_rows[j + 1] = tmp;
	}
}


// Blit a 120x68 565 thumbnail file (read via work_buf) into the framebuffer at
// (px,py). Black box if missing/unreadable. Shared by the save-slot previews
// (.thb sidecars) and the game-folder previews (Game.thb).
static void draw_thumb_file(int px, int py, const char *path)
{
	int x, y, ok = 0;
	u16 *buf = (u16 *)work_buf;
	SceUID fd;

	fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd >= 0) {
		if (sceIoRead(fd, buf, THUMB_W * THUMB_H * 2) == THUMB_W * THUMB_H * 2) ok = 1;
		sceIoClose(fd);
	}
	if (!ok) { dbg_fill_rect(px, py, THUMB_W, THUMB_H, 0xFF000000); return; }

	if (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
		for (y = 0; y < THUMB_H; y++) {
			volatile u32 *d = (volatile u32 *)dbg_fb + (py + y) * dbg_bufw + px;
			for (x = 0; x < THUMB_W; x++) d[x] = from565(buf[y * THUMB_W + x]);
		}
	} else {
		for (y = 0; y < THUMB_H; y++) {
			volatile u16 *d = (volatile u16 *)dbg_fb + (py + y) * dbg_bufw + px;
			for (x = 0; x < THUMB_W; x++) d[x] = img16_from565(buf[y * THUMB_W + x], dbg_pfmt);
		}
	}
}

// Save-slot preview: <g_browse_dir>/<name with .bin -> .thb>.
static void draw_thumb(int px, int py, const char *name)
{
	char thb[160]; int len;

	// Guard: g_browse_dir + '/' + name + NUL must fit (a user-renamed long filename
	// could otherwise smash the menu thread's kernel stack while the game is frozen).
	if (strlen(g_browse_dir) + strlen(name) + 2 > sizeof(thb)) {
		dbg_fill_rect(px, py, THUMB_W, THUMB_H, 0xFF000000);
		return;
	}
	sprintf(thb, "%s/%s", g_browse_dir, name);
	len = (int)strlen(thb);
	if (len < 4) {
		dbg_fill_rect(px, py, THUMB_W, THUMB_H, 0xFF000000);
		return;
	}
	memcpy(thb + len - 4, ".thb", 5);
	draw_thumb_file(px, py, thb);
}

// Show the full 480x272 screenshot fullscreen (read the .scr sidecar, streamed in
// row batches through work_buf since it's >work_buf). Waits for Right or O to
// close. No-op if there's no .scr (e.g. an old save). The caller redraws after.
static void show_screenshot(const char *name)
{
	char scr[160]; int len, x, y, y0;
	u16 *buf = (u16 *)work_buf;
	int rows_per = (int)(COMPRESS_BUF_SIZE / 2) / 480;   // rows that fit in work_buf
	SceUID fd;

	// Guard: composed path must fit (see draw_thumb) — avoid a kernel-stack smash.
	if (strlen(g_browse_dir) + strlen(name) + 2 > sizeof(scr)) return;
	sprintf(scr, "%s/%s", g_browse_dir, name);
	len = (int)strlen(scr);
	if (len < 4) return;
	memcpy(scr + len - 4, ".scr", 5);
	fd = sceIoOpen(scr, PSP_O_RDONLY, 0);
	if (fd < 0) return;

	for (y0 = 0; y0 < 272; ) {
		int rows = (272 - y0 < rows_per) ? (272 - y0) : rows_per;
		int got = sceIoRead(fd, buf, rows * 480 * 2);
		int gr  = got / (480 * 2);
		for (y = 0; y < gr; y++) {
			if (dbg_pfmt == PSP_DISPLAY_PIXEL_FORMAT_8888) {
				volatile u32 *d = (volatile u32 *)dbg_fb + (y0 + y) * dbg_bufw;
				for (x = 0; x < 480; x++) d[x] = from565(buf[y * 480 + x]);
			} else {
				volatile u16 *d = (volatile u16 *)dbg_fb + (y0 + y) * dbg_bufw;
				for (x = 0; x < 480; x++) d[x] = img16_from565(buf[y * 480 + x], dbg_pfmt);
			}
		}
		if (gr < rows) break;
		y0 += gr;
	}
	sceIoClose(fd);

	// Border so it's clearly a screenshot, not the live game.
	dbg_fill_rect(0,   0,   480, 3, BR_CYAN);   // top
	dbg_fill_rect(0,   269, 480, 3, BR_CYAN);   // bottom
	dbg_fill_rect(0,   0,   3,   272, BR_CYAN); // left
	dbg_fill_rect(477, 0,   3,   272, BR_CYAN); // right

	wait_button_edge(PSP_CTRL_RIGHT | PSP_CTRL_CIRCLE);
}

// Vertical scrollbar on the right edge: track + a thumb sized/positioned by the
// visible window over the total entries.
static void draw_scrollbar(int top, int total)
{
	int tx = 472, tw = 4, ty = 24, th = 236;   // half-width, hugging the right edge
	dbg_fill_rect(tx, ty, tw, th, 0xFF202020);
	if (total > BR_VISIBLE) {
		int hh = th * BR_VISIBLE / total;
		int hy;
		if (hh < 10) hh = 10;
		hy = ty + (th - hh) * top / (total - BR_VISIBLE);
		dbg_fill_rect(tx, hy, tw, hh, BR_CYAN);
	} else {
		dbg_fill_rect(tx, ty, tw, th, BR_CYAN);  // everything visible
	}
}

// Geometry + background for one visible list slot (both browsers): fills the
// row band (leaves the scrollbar) and returns the slot's top char-row;
// *bg_out = the band color (selected/normal) for the text drawn on top of it.
static int draw_row_band(int e, int selected, u32 *bg_out)
{
	int r  = BR_LIST_ROW + e * BR_ROW_H;
	u32 bg = selected ? BR_SEL : BR_CARD;
	dbg_fill_rect(8, r * 8, 456, BR_ROW_H * 8 - 2, bg);
	if (selected) dbg_fill_rect(8, r * 8, 3, BR_ROW_H * 8 - 2, BR_STRIPE);   // accent stripe
	*bg_out = bg;
	return r;
}

// Draw one list entry. e = visible slot (0..BR_VISIBLE-1), idx = row index. When
// g_show_newsave, idx 0 = "New Savegame" and saves start at idx 1; otherwise idx 0.. are
// saves (auto-open load view). selected = highlight.
static void draw_one(int e, int idx, int selected)
{
	u32 bg;
	int r    = draw_row_band(e, selected, &bg);
	int py   = r * 8;
	int tcol = 18;                       // text column, right of the preview (px ~144)
	int trow = r + (BR_ROW_H / 2) - 1;   // vertically centered in the band
	char line[80];

	if (g_show_newsave && idx == 0) {
		dbg_fill_rect(12, py + 6, THUMB_W, THUMB_H, 0xFF000000);  // empty preview
		dbg_text(tcol, trow, BR_WHITE, bg, "New Savegame");
	} else {
		const struct save_row *s = &g_rows[idx - g_show_newsave];
		const ScePspDateTime *d = &s->mtime;
		draw_thumb(12, py + 6, s->name);                          // preview image
		sprintf(line, "Date: %04d-%02d-%02d", d->year, d->month, d->day);
		dbg_text(tcol, r + 2, BR_WHITE, bg, line);
		sprintf(line, "Time: %02d:%02d", d->hour, d->minute);
		dbg_text(tcol, r + 4, BR_WHITE, bg, line);
		sprintf(line, "Size: %u KB", (unsigned)((s->size + 1023) / 1024));
		dbg_text(tcol, r + 6, BR_GREY, bg, line);
	}
}

// Redraw just the list rows + scrollbar for EITHER browser (called on selection/
// scroll change — avoids the full-screen repaint that caused flicker). draw_fn
// paints a populated slot; slots past the end get a plain background band.
static void draw_list_rows(int sel, int top, int total,
                           void (*draw_fn)(int e, int idx, int selected))
{
	int e, idx;
	for (e = 0; e < BR_VISIBLE; e++) {
		idx = top + e;
		if (idx < total) {
			draw_fn(e, idx, idx == sel);
		} else {
			int py = (BR_LIST_ROW + e * BR_ROW_H) * 8;
			dbg_fill_rect(8, py, 456, BR_ROW_H * 8 - 2, BR_BG);
		}
	}
	draw_scrollbar(top, total);
}

static void draw_list(int sel, int top)
{
	draw_list_rows(sel, top, g_row_count + g_show_newsave, draw_one);
}

// Full-screen chrome shared by the browser/game-list/settings screens:
// background fill + centered cyan title on BR_TITLE_ROW + centered grey footer
// on row 33 (60 text columns wide).
static void draw_screen_chrome(const char *title, const char *foot)
{
	int fc = (60 - (int)strlen(foot)) / 2;
	if (fc < 0) fc = 0;
	dbg_fill_rect(0, 0, 480, 272, BR_BG);
	dbg_text(1, BR_TITLE_ROW, BR_CYAN, BR_BG, title);   // azure title, left-aligned
	dbg_fill_rect(8, 20, 464, 2, BR_STRIPE);            // accent underline under the header
	dbg_fill_rect(0, 262, 480, 10, BR_CARD);            // footer bar (behind the hints)
	dbg_fill_rect(0, 262, 480, 1, BR_LINE);             // its top hairline
	dbg_text(fc, 33, BR_GREY, BR_CARD, foot);           // hints on the bar (row 33 = y264)
}

// Full draw: background + title + footer + list. Done once on open; navigation
// only redraws the list (draw_list), so the screen is otherwise static.
static void draw_browser(int sel, int top, const char *gid)
{
	char title[80];
	format_game_header(title, gid);
	draw_screen_chrome(title, "L:Games <:View X:Save []:Load /\\:Delete O:Close R:Settings");
	// Save-slot counter in the top-right corner: used slots / cap (see MAX_SAVE_ROWS).
	{
		char cnt[16];
		sprintf(cnt, "%d/%d", g_row_count, MAX_SAVE_ROWS);
		dbg_text(60 - (int)strlen(cnt), BR_TITLE_ROW, BR_GREY, BR_BG, cnt);
	}
	draw_list(sel, top);
}

// Confirm/info text on the RIGHT strip (px 280..472), clear of the slot stats
// (which end ~px 272). No box — text drawn straight onto the (highlighted)
// selected slot, so its bg matches BR_SEL. Centered in the strip; `by` aligns it
// vertically with the selected slot. c1/c2 = per-line colors (RED main line for
// blocked-action / warning variants).
static void draw_msg_colored(const char *l1, u32 c1, const char *l2, u32 c2, int by)
{
	int cl = 280 / 8, cw = 192 / 8, rr = by / 8;
	dbg_text(cl + (cw - (int)strlen(l1)) / 2, rr + 2, c1, BR_SEL, l1);
	if (l2) dbg_text(cl + (cw - (int)strlen(l2)) / 2, rr + 4, c2, BR_SEL, l2);
}
#define draw_msg(l1, l2, by) draw_msg_colored((l1), BR_WHITE, (l2), BR_CYAN, (by))

// Box top-y (char-row-snapped) that vertically centers the 64px box on the
// visible slot at selection `sel` given scroll `top`.
static int msg_box_y(int sel, int top)
{
	int row = BR_LIST_ROW + (sel - top) * BR_ROW_H;   // selected slot's char row
	int y = row * 8 + (BR_ROW_H * 8 - 64) / 2;        // center the 64px box in the slot
	return (y / 8) * 8;                                // snap to a char row
}

// One-line info notice on the right strip; waits for any button to dismiss.
static void info_msg(const char *l1, const char *l2, int by)
{
	draw_msg(l1, l2, by);
	wait_button_edge(~0u);   // any new press dismisses
}

// Like info_msg but the main line is RED — for a BLOCKED action (e.g. saving into a
// different game's folder). Drawn where the confirm prompt normally appears.
static void info_msg_red(const char *l1, const char *l2, int by)
{
	draw_msg_colored(l1, BR_RED, l2, BR_WHITE, by);
	wait_button_edge(~0u);
}

// Yes/No prompt. X = yes, O = no. Waits for the answer button to RELEASE before
// returning, so it doesn't leak a stale edge into the browser loop.
int confirm(const char *msg, int by)
{
	u32 hit;
	draw_msg(msg, "X = Yes       O = No", by);
	hit = wait_button_edge(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
	wait_release(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
	return (hit & PSP_CTRL_CROSS) ? 1 : 0;
}

// Version-mismatch load prompt: a RED warning with the save vs plugin versions,
// confirmed by TRIANGLE (deliberately NOT X, so it can't be reflexively accepted
// like a normal load). Returns 1 on Triangle, 0 on O.
static int confirm_version_load(u32 save_ver, u32 plug_ver, int by)
{
	char l1[32];
	u32 hit;
	sprintf(l1, "Save:%u Plugin:%u",
	        (unsigned)(save_ver & 0xFFFF), (unsigned)(plug_ver & 0xFFFF));
	draw_msg_colored(l1, BR_RED, "/\\ = Load   O = No", BR_WHITE, by);   // RED main line
	hit = wait_button_edge(PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE);
	wait_release(PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE);
	return (hit & PSP_CTRL_TRIANGLE) ? 1 : 0;
}
// save_settings() is defined further below (global settings persistence), after
// this function — declared in menu.h (non-static: the gamma worker calls it too),
// so declining a non-stock step can write the reset back to settings.cfg here.

// Boot-time frozen prompt sequence, run on the menu thread at first wake. Freezes the game
// ONCE (same FAST dispatch-off gate as run_save_browser, no MS-lock probe) and, while frozen,
// shows any of: the overclock-confirm prompt, then the CAPTURE arm gate — then resumes ONCE.
// A single continuous freeze keeps the game from advancing a frame between the two prompts.
//   do_oc  : a non-stock overclock step is persisted -> ask X/O before applying it.
//            Declining resets g_overclock_id to 0 and persists it.
//   do_arm : Video Skip = CAPTURE -> show "Hold RIGHT until Intro skipped" and wait for the
//            user to hold D-pad Right for 1s. The game then resumes with Right ALREADY held,
//            so the capture fires from the intro's first frame under the user's control.
//
// MUST freeze first: the game keeps rendering its own frames, which instantly overwrites
// anything drawn straight to the framebuffer. pin_current_display() reasserts the live buffer
// to the display controller so the draw doesn't land off-screen; dbg_init() then grabs that
// consistent buffer.
int  ms_probe_after_freeze(void);
void ms_probe_reap(void);

void boot_frozen_prompts(int do_oc, int do_arm)
{
	int attempt, frozen = 0;

	if (!do_oc && !do_arm) return;

	// Same freeze + MS-lock probe the save browser uses: the probe confirms the MS lock is
	// free before we proceed; if a frozen thread holds it, resume so it finishes and retry.
	for (attempt = 0; attempt < 8 && !frozen; attempt++) {
		if (attempt) sceKernelDelayThread(20000);
		if (suspend_escalating(0, 500) != 0) { resume_game_threads(); continue; }  // a thread wouldn't freeze
		if (ms_probe_after_freeze()) { frozen = 1; break; }                        // MS lock free -> safe
		resume_game_threads();                                                     // frozen lock-holder -> let it finish
		ms_probe_reap();                                                           // collect the parked probe fd
	}
	if (!frozen) {
		WriteDebugLog("[BOOT] prompt-freeze FAILED (freeze/MS-busy) - skipping OC confirm / capture arm");
		return;
	}

	pin_current_display();
	dbg_init();             // grab the CURRENT framebuffer (now consistent with the pin above)

	if (do_oc) {
		int mhz10 = g_oc_freq_x10[g_overclock_id];
		char l1[48];
		const char *l2 = "X = Yes       O = No (stays at stock)";
		u32 hit;
		sprintf(l1, "Apply Overclock %d.%dMHz?", mhz10 / 10, mhz10 % 10);
		dbg_fill_rect(0, 0, 480, 272, BR_BG);
		dbg_text((60 - (int)strlen(l1)) / 2, 15, BR_WHITE, BR_BG, l1);
		dbg_text((60 - (int)strlen(l2)) / 2, 17, BR_GREY,  BR_BG, l2);
		// Drain any X/O that is ALREADY down as the prompt appears (the user may still be
		// mashing X from the boot). wait_button_edge needs a RISING edge, so a button held
		// from before entry would never edge and the prompt would look like it "ignores"
		// input until released — clear it first so the next real press is a fresh edge.
		wait_release(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
		if (DBG_UART()) uart_puts("[OC] confirm shown - X=apply O=stock");
		hit = wait_button_edge(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
		if (DBG_UART()) { char b[40]; sprintf(b, "[OC] confirm hit=%08X", (unsigned)hit); uart_puts(b); }
		if (hit & PSP_CTRL_CROSS) {
			oc_apply(g_overclock_id);
		} else {
			g_overclock_id = 0;
			save_settings();
		}
		// Clear the answer press before moving on (the arm gate reads a DIFFERENT button,
		// D-pad Right, but draining keeps the resume clean).
		wait_release(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
	}

	if (do_arm) {
		const char *l1 = "Hold RIGHT until Intro skipped";
		const char *l2 = "Hold D-pad RIGHT for 1s to start the skip";
		u64 hold_start = 0, gate_t0 = now_us();
		dbg_fill_rect(0, 0, 480, 272, BR_BG);
		dbg_text((60 - (int)strlen(l1)) / 2, 15, BR_WHITE, BR_BG, l1);
		dbg_text((60 - (int)strlen(l2)) / 2, 17, BR_GREY,  BR_BG, l2);
		// Wait for a CONTINUOUS 1s hold of the user's REAL hold button (VSKIP_HOLD_BTN =
		// D-pad Right; kpeek = unmasked hardware read, same as the capture watcher — never
		// the game's pad, which our injection writes into). A release resets the timer. We do
		// NOT wait for release afterward: the user keeps holding Right into the intro so the
		// capture fires from frame one. A 30s cap is a safety escape: if the user never holds
		// it (walked away / changed their mind) we must not leave the game frozen forever —
		// resume UNARMED and let the watcher's own 2s grace cancel the capture.
		for (;;) {
			SceCtrlData kpad;
			int x_held, k1;
			k1 = pspSdkSetK1(0);
			x_held = (kpeek(&kpad) > 0 && (kpad.Buttons & VSKIP_HOLD_BTN)) ? 1 : 0;
			pspSdkSetK1(k1);
			if (x_held) {
				if (hold_start == 0) hold_start = now_us();
				else if (now_us() - hold_start >= 1000000) break;   // held 1s -> armed
			} else {
				hold_start = 0;
			}
			if (now_us() - gate_t0 >= 30000000ULL) {   // 30s: no hold -> give up, don't hang
				if (DBG_UART()) uart_puts("[VSKIP] arm gate timed out (no 1s RIGHT hold) - resuming unarmed");
				break;
			}
			sceKernelDelayThread(16000);
		}
		if (DBG_UART()) uart_puts("[VSKIP] capture arm gate done - resuming into intro");
	}

	// OC-only: suppress the confirm press so it doesn't leak a stale edge into the game.
	// When arming, the X is DELIBERATELY carried into the intro, so no suppress there.
	if (do_oc && !do_arm) arm_input_suppress();
	resume_game_threads();
}

// ── Global settings persistence (SAVESTATE/settings.cfg: magic + ints) ──
// [0]=magic, [1]=debug routing, [2]=default slot, [3]=overclock step,
// [4]=UART logging, [5]=Show-FPS mode (0=Off 1=FPS 2=+1% 3=+Frametime),
// [6]=FPS update rate (1..10 = 0.1..1.0s), [7]=Battery overlay mode (0=Off
// 1=Percent 2=Percent+Time 3=ALL), [8]=CPU & GPU Usage toggle (0=Off 1=On),
// [9]=overlay location (0=Up Left 1=Up Right 2=Down Left 3=Down Right; older
// files wrote 0 here, so no generation bump was needed), [10]=Overclock stable
// flag, [11]=real battery capacity mAh (0 = stock/BMS), [12]=free slot,
// [13]=Stop-Charging threshold % (0=OFF, else 80..95).
//
// The magic IS the settings generation; a file whose magic/size doesn't match the current
// build is rejected wholesale (defaults load, and the file is rewritten with the current
// magic + defaults) so the reset happens exactly once.
// Per-game settings (Auto-Open, Intro Video Skip, Frame Limit, Save Compression)
// stored in gameset.cfg; gamma in screen.cfg.
#define SETTINGS_PATH  "ms0:/seplugins/SAVESTATE/settings.cfg"
#define SETTINGS_MAGIC 0x53455443u   // "SETC" — settings generation; bump the letter on any layout change

void load_settings(void)
{
	SceUID fd; u32 buf[14]; int n;
	fd = sceIoOpen(SETTINGS_PATH, PSP_O_RDONLY, 0);
	if (fd < 0) return;                          // no file -> keep defaults, nothing to adopt
	n = sceIoRead(fd, buf, sizeof(buf));
	sceIoClose(fd);                              // close BEFORE any rewrite (load may re-save)
	// Strict: right magic AND full 14 words, else the file is another
	// generation (see the generation scheme above) or corrupt — defaults load.
	if (n == (int)sizeof(buf) && buf[0] == SETTINGS_MAGIC) {
		g_show_debug   = (int)buf[1];            // debug routing 0..3 (see g_show_debug)
		if (g_show_debug < 0 || g_show_debug > 3) g_show_debug = 0;
		g_default_slot = buf[2] ? 1 : 0;
		g_stage_spot   = 1;                      // always Mid: the snapshot stages in the safe middle 8MB
		g_overclock_id = (int)buf[3];
		if (g_overclock_id < 0 || g_overclock_id >= OC_STEPS) g_overclock_id = 0;
		g_uart_log     = buf[4] ? 1 : 0;
		g_show_fps_overlay = (int)buf[5];        // mode 0..3
		if (g_show_fps_overlay < 0 || g_show_fps_overlay > 3) g_show_fps_overlay = 0;
		g_fps_rate = (int)buf[6];                // 1..10 = 0.1..1.0s
		if (g_fps_rate < 1 || g_fps_rate > 10) g_fps_rate = 10;
		g_show_battery  = (int)buf[7];
		if (g_show_battery < 0 || g_show_battery > 3) g_show_battery = 0;
		g_show_cpu_usage = buf[8] ? 1 : 0;
		g_overlay_pos = (int)buf[9];            // overlay anchor corner 0..3
		if (g_overlay_pos < 0 || g_overlay_pos > 3) g_overlay_pos = 0;
		// [12] free slot this generation; 1%/Frametime derive from the mode.
		g_fps_show_lows = (g_show_fps_overlay >= 2) ? 1 : 0;
		g_show_ft_chart = (g_show_fps_overlay >= 3) ? 1 : 0;
		g_overclock_stable = buf[10] ? 1 : 0;
		g_batt_real_mah = (int)buf[11];
		if (g_batt_real_mah < 0 || g_batt_real_mah > 20000) g_batt_real_mah = 0;
		g_welcome_shown = buf[12] ? 1 : 0;   // one-time Welcome screen seen flag
		g_batt_stop_charge = (int)buf[13];   // Stop-Charging threshold % (0=OFF, 100=ON/never, else 70..95)
		if (g_batt_stop_charge != 0 && g_batt_stop_charge != 100 &&
		    (g_batt_stop_charge < 70 || g_batt_stop_charge > 95))
			g_batt_stop_charge = 0;
	} else if (n > 0) {
		// Old generation or corrupt: NO migration — keep defaults and rewrite the
		// file with this build's magic so the reset is visible once, not every boot.
		// (menu-thread context, game just booted: sceIo alive, MS safe to write.)
		WriteDebugLog("[SET] settings.cfg wrong generation - defaults loaded, file rewritten");
		save_settings();
	}
}

// Persists the global settings.cfg (menu thread, Memory-Stick safe). Called from
// the settings menu on close. Gamma and colour temperature are PER-GAME and saved
// separately through st_save_game_gamma() (screen.cfg) — they no longer ride here.
void save_settings(void)
{
	SceUID fd; u32 buf[14];
	buf[0] = SETTINGS_MAGIC; buf[1] = (u32)g_show_debug;
	buf[2] = (u32)g_default_slot; buf[3] = (u32)g_overclock_id;
	buf[4] = (u32)g_uart_log; buf[5] = (u32)g_show_fps_overlay;
	buf[6] = (u32)g_fps_rate; buf[7] = (u32)g_show_battery;
	buf[8] = (u32)g_show_cpu_usage; buf[9] = (u32)g_overlay_pos;   // overlay anchor corner (see word map)
	buf[10] = (u32)g_overclock_stable;
	buf[11] = (u32)g_batt_real_mah;   // real battery capacity mAh (0 = stock)
	buf[12] = (u32)g_welcome_shown;   // one-time Welcome screen seen flag (0 -> show once)
	buf[13] = (u32)g_batt_stop_charge;   // Stop-Charging threshold % (0=OFF, else 80..95)
	sceIoMkdir("ms0:/seplugins/SAVESTATE", 0777);
	fd = sceIoOpen(SETTINGS_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, buf, sizeof(buf)); sceIoClose(fd); }
}

// ── Per-game settings (SAVESTATE/<gameid>/gameset.cfg: magic + 3 ints) ──
// Keyed by whichever game's folder is currently selected in the browser (gid),
// not necessarily the running game. Holds Auto-Open-on-Boot (g_autoload), the
// Save Compression mode (g_compress) and the Frame Limit (g_frame_limit) — all
// per-game. Read LENIENTLY (n >= 3 words, like load_settings does globally) so an
// older 3-word file written before the Frame Limit existed still loads its
// earlier fields instead of being rejected wholesale by a strict size check.
void load_game_settings(const char *gid)   // -> g_autoload + g_compress + g_frame_limit + g_video_skip(+ms) for gid
{
	char p[96]; SceUID fd; u32 buf[6]; int n;
	g_autoload    = 0;                           // default off per game
	g_compress    = 1;                           // default Compact per game
	g_frame_limit = 0;                           // default OFF per game
	g_video_skip  = VSKIP_OFF;                   // default OFF per game
	g_video_skip_ms = 0;
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/gameset.cfg", gid);
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return;
	n = sceIoRead(fd, buf, sizeof(buf));
	if (n >= (int)(3 * sizeof(u32)) && buf[0] == GAMESET_MAGIC) {
		g_autoload = buf[1] ? 1 : 0;
		g_compress = buf[2] ? 1 : 0;
		if (n >= (int)(4 * sizeof(u32))) {       // pre-Frame-Limit file: leave the default
			int fl = (int)buf[3];                // sanitize: only OFF or 20..60
			g_frame_limit = (fl >= 20 && fl <= 60) ? fl : 0;
		}
		if (n >= (int)(5 * sizeof(u32))) {       // pre-Video-Skip file: leave the default
			int m = (int)buf[4];
			g_video_skip = (m == VSKIP_CAPTURE || m == VSKIP_TIMED) ? m : VSKIP_OFF;
			if (n >= (int)(6 * sizeof(u32))) g_video_skip_ms = (int)buf[5];
			if (g_video_skip_ms < 0 || g_video_skip_ms > VSKIP_LEARN_MAX_MS) g_video_skip_ms = 0;
			if (g_video_skip == VSKIP_TIMED && g_video_skip_ms <= 0) g_video_skip = VSKIP_CAPTURE;
		}
	}
	sceIoClose(fd);
}

void save_game_settings(const char *gid)
{
	char p[96], d[80]; SceUID fd; u32 buf[6];
	buf[0] = GAMESET_MAGIC; buf[1] = (u32)g_autoload; buf[2] = (u32)g_compress; buf[3] = (u32)g_frame_limit;
	buf[4] = (u32)g_video_skip;
	buf[5] = (u32)g_video_skip_ms;
	sceIoMkdir("ms0:/seplugins/SAVESTATE", 0777);
	sprintf(d, "ms0:/seplugins/SAVESTATE/%s", gid);
	sceIoMkdir(d, 0777);
	sprintf(p, "%s/gameset.cfg", d);
	fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, buf, sizeof(buf)); sceIoClose(fd); }
}

// Free KERNEL partition (mpid 1) RAM, in KB — the partition this kernel PRX lives in.
// Resolve the kernel export at runtime (SysMemForKernel 0x0115B0F8); fall back to the
// user-partition call if it can't be resolved. Shared by the Settings-footer readout
// (ram_usage_kb) and the low-RAM save/load guard (run_save_browser) so both agree on
// the same number.
//
// LOW_RAM_SAVE_LOAD_KB: below this much free kernel RAM, Save/Load are refused.
#define LOW_RAM_SAVE_LOAD_KB 50
static u32 free_kernel_ram_kb(void)
{
	static u32 (*p_kfree)(int) = NULL;
	if (!p_kfree)
		p_kfree = (u32 (*)(int))sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", 0x0115B0F8);
	return p_kfree ? (p_kfree(1) / 1024) : (u32)(sceKernelTotalFreeMemSize() / 1024);
}

// RAM-usage figures for the Settings footer. Static = our loaded module image
// (code + data + bss, incl. every static buffer like work_buf/g_fastlz_htab) via
// sceKernelQueryModuleInfo on our own modid. Dynamic = runtime allocations OUTSIDE
// the image = the menu thread's stack, PLUS fps_poll_thread's and/or
// battery_poll_thread's stack whenever each is actually running
// (g_fps_poll_started/g_battery_poll_started — both created lazily and
// self-exit when their setting is off, so this reads live rather than
// assuming either is always present). The save IOBUF is BORROWED game/volatile
// RAM, not ours, so it's excluded. Free = free_kernel_ram_kb() above. All in
// KB. Returns 0 on success; leaves fields at 0 on any query failure.
#define MENU_STACK_BYTES 0x5000   // keep in sync with sceKernelCreateThread in main.c
void ram_usage_kb(u32 *static_kb, u32 *dynamic_kb, u32 *free_kb)
{
	*static_kb = *dynamic_kb = *free_kb = 0;
	{
		SceKernelModuleInfo mi;
		SceUID mid = sceKernelGetModuleId();
		memset(&mi, 0, sizeof(mi)); mi.size = sizeof(mi);
		if (mid >= 0 && sceKernelQueryModuleInfo(mid, &mi) >= 0)
			*static_kb = (mi.text_size + mi.data_size + mi.bss_size + 1023) / 1024;
	}
	*dynamic_kb = (MENU_STACK_BYTES + (g_fps_poll_started ? FPS_POLL_STACK_BYTES : 0)
	               + (g_battery_poll_started ? BATTERY_POLL_STACK_BYTES : 0)
	               + (g_st_worker_started ? ST_WORKER_STACK_BYTES : 0) + 1023) / 1024;
	*free_kb = free_kernel_ram_kb();
}

// Draw the settings screen. sel = highlighted row (GLOBAL section first, then the
// per-game section): 1 Default Slot, 2 Show FPS (L/R mode, Tri/X update rate),
// 3 Overlay Location, 4 CPU & GPU Usage, 5 Battery Status, 6 Stop Charging,
// 7 Overclock (all global) | 8 Debug Messages (debug builds only; merged with
// UART Log) | 9 Screen Tuning, 10 FPS unlocks, 11 Frame Limit, 12 Intro Video
// Skip, 13 Auto-Open (all per-game). Debug Messages sits alone above the
// "Game specific Settings" header.
// One settings row: highlight band + white label/value text. Keeps the row
// geometry (band at row*8-4, text at column 6) in exactly one place, so a row
// can't drift out of sync with its own highlight.
static void draw_settings_row_col(int row, int selected, const char *line, u32 fg)
{
	u32 bg = selected ? BR_SEL : BR_BG;
	dbg_fill_rect(40, row * 8 - 4, 424, 16, bg);   // x40..464: covers the longest 51-char value line (ends x456)
	if (selected) dbg_fill_rect(40, row * 8 - 4, 3, 16, BR_STRIPE);   // accent stripe on the selected row
	dbg_text(6, row, fg, bg, line);
}
static void draw_settings_row(int row, int selected, const char *line)
{
	draw_settings_row_col(row, selected, line, BR_WHITE);   // default: white text
}

// Builds one "Label:              < value >" row. The label is left-justified
// into a fixed-width field (%-20s) so every row's "<" starts at the same column
// regardless of label length. 20 chars fits the longest label here ("Auto-Open on Boot:").
static void settings_line(char *line, const char *label, const char *value)
{
	sprintf(line, "%-20s< %s >", label, value);
}

void draw_settings(int sel, const char *gid)
{
	// val must hold the LONGEST value string any row renders — currently Intro Video
	// Skip's "ON - Time capture next boot" (27 bytes with the NUL).
	char line[64], val[40], header[80];
	const char *gt = "Game specific Settings";
	// Global rows sel 1..8, then the per-game section sel 9..13. Rows 2 apart.
	int r1 = 4, r2 = 6, r3 = 8, r4 = 10, r5 = 12, r6 = 14, r7 = 16, r8 = 18, gr = 20, r9 = 22, r10 = 24, r11 = 26, r12 = 28, r13 = 30;

	format_game_header(header, gid);
	draw_screen_chrome(header, "Up/Dn sel  L/R change  R:Help  Tri/X adjust  O Return");

	// sel 1: Default Slot (global).
	settings_line(line, "Default Slot:", g_default_slot ? "Last" : "New");
	draw_settings_row(r1, sel == 1, line);

	// sel 2: Show FPS (GLOBAL) — combines the old FPS / 1% Lows / Frametime toggles.
	// L/R cycles the mode; Tri/X set the update rate (fps_window_us). The 1% and
	// Frametime flags are DERIVED from the mode (see the sel==2 handler / load_settings).
	{
		static const char *fps_mode[4] = { "OFF", "FPS", "FPS + 1%", "FPS + 1% + Frametime" };
		const char *mn = fps_mode[(g_show_fps_overlay >= 0 && g_show_fps_overlay <= 3) ? g_show_fps_overlay : 0];
		if (g_show_fps_overlay > 0)
			sprintf(line, "%-20s< %s >  %d.%ds", "Show FPS:", mn, g_fps_rate / 10, g_fps_rate % 10);
		else
			sprintf(line, "%-20s< %s >", "Show FPS:", mn);
	}
	draw_settings_row(r2, sel == 2, line);

	// sel 3: Overlay Location (GLOBAL, settings.cfg [9]) — corner anchor for the
	// FPS/battery/CPU overlay. Down modes keep the SAME top-to-bottom order
	// (FPS first ... battery last), just anchored on the bottom edge.
	{
		static const char *ov_pos[4] = { "Up Left", "Up Right", "Down Left", "Down Right" };
		settings_line(line, "Overlay Location:", ov_pos[(g_overlay_pos >= 0 && g_overlay_pos <= 3) ? g_overlay_pos : 0]);
	}
	draw_settings_row(r3, sel == 3, line);

	// sel 4: CPU & GPU Usage (GLOBAL) — CPU via idle-clocks, GPU via GE busy-duty-cycle.
	settings_line(line, "CPU & GPU Usage:", g_show_cpu_usage ? "On" : "OFF");
	draw_settings_row(r4, sel == 4, line);

	// sel 5: Battery Status (GLOBAL) — L/R cycles the mode; Tri/X set the real battery
	// capacity (the mAh readout + time are scaled to it — see g_batt_real_mah).
	{
		static const char *batt_name[4] = { "OFF", "Percent", "Percent+Time", "ALL" };
		const char *bn = batt_name[(g_show_battery >= 0 && g_show_battery <= 3) ? g_show_battery : 0];
		char cap[20];
		if (g_batt_real_mah > 0) sprintf(cap, "(Batt %dmAh)", g_batt_real_mah);
		else                     strcpy(cap, "(Stock mAh)");
		sprintf(line, "%-20s< %s >  %s", "Battery Status:", bn, cap);
	}
	draw_settings_row(r5, sel == 5, line);

	// sel 6: Stop Charging (GLOBAL) — L/R cycles OFF / 95 / 90 / 85 / 80 / 75 / 70 / ON.
	// While set, charging is forbidden at/above the threshold (see
	// battery_charge_gate in sysstats.c) so a cell parked on AC holds that %.
	// ON (internal 100) forbids charging unconditionally.
	{
		static const char *sc_name[8] = { "OFF", "95%", "90%", "85%", "80%", "75%", "70%", "ON" };
		int sc = 0;   // 0=OFF, 1..6 = 95..70, 7=ON (internal 100)
		if      (g_batt_stop_charge >= 100) sc = 7;
		else if (g_batt_stop_charge >= 95) sc = 1;
		else if (g_batt_stop_charge >= 90) sc = 2;
		else if (g_batt_stop_charge >= 85) sc = 3;
		else if (g_batt_stop_charge >= 80) sc = 4;
		else if (g_batt_stop_charge >= 75) sc = 5;
		else if (g_batt_stop_charge >= 70) sc = 6;
		settings_line(line, "Stop Charging:", sc_name[sc]);
	}
	draw_settings_row(r6, sel == 6, line);

	// sel 7: Overclock (GLOBAL — raw PLL registers, PSP-1000 only). 0 = stock 333MHz.
	{
		int idx = (g_overclock_id >= 0 && g_overclock_id < OC_STEPS) ? g_overclock_id : 0;
		int mhz10 = g_oc_freq_x10[idx];
		if (idx == 0)
			sprintf(val, "OFF");
		else if (mhz10 % 10)
			sprintf(val, "%d.%d MHz", mhz10 / 10, mhz10 % 10);
		else
			sprintf(val, "%d MHz", mhz10 / 10);
	}
	// Stable (Triangle-marked, non-stock only): RED with a "(Set as Stable)" tag, and the boot
	// confirm is skipped. Otherwise the normal white row (message shown on game start).
	if (g_overclock_stable && g_overclock_id > 0) {
		sprintf(line, "%-20s< %s >  (Set as Stable)", "Overclock:", val);
		draw_settings_row_col(r7, sel == 7, line, BR_RED);
	} else {
		settings_line(line, "Overclock:", val);
		draw_settings_row(r7, sel == 7, line);
	}

	// sel 8: Debug Messages (GLOBAL, debug builds only) — merged with UART Log.
	// 0=OFF 1=Log MS 2=Log Screen 3=Screen and MS 4=UART.
#if DEBUG_BUILD
	{
		static const char *dbg_modes[5] = { "OFF", "Log MS", "Log Screen", "Screen and MS", "UART" };
		int mode = (g_uart_log && g_show_debug == 0) ? 4 : (g_show_debug >= 0 && g_show_debug <= 3) ? g_show_debug : 0;
		settings_line(line, "Debug Messages:", dbg_modes[mode]);
	}
	draw_settings_row(r8, sel == 8, line);
#endif

	dbg_text((60 - (int)strlen(gt)) / 2, gr, BR_CYAN, BR_BG, gt);   // per-game section header

	// Per-game section sel 9..13: Screen Tuning, FPS unlocks, Frame Limit, Intro
	// Video Skip, Auto-Open (the input handlers in run_settings_menu use these numbers).

	// sel 9: Screen Tuning (PER-GAME, screen.cfg) — Triangle = test pattern, X = live HUD.
	sprintf(val, "Gamma %d.%02d  Temp %d", g_st_gamma / 100, g_st_gamma % 100, g_st_temp);
	settings_line(line, "Screen Tuning:", val);
	draw_settings_row(r9, sel == 9, line);

	// sel 10: FPS unlocks (PER-GAME, fps.cfg) — L/R cycles Stock / <DB cheat names>.
	// With no DB / no FPS cheats the row shows the empty-state labels instead.
	{
		const char *nm = cheats_fps_name(g_fps_active);
		char nb[40];
		if ((int)strlen(nm) > 34) { memcpy(nb, nm, 34); nb[34] = 0; nm = nb; }
		settings_line(line, "FPS unlocks:", nm);
	}
	draw_settings_row(r10, sel == 10, line);

	// sel 11: Frame Limit (PER-GAME, gameset.cfg) — OFF or a 25..60 FPS cap.
	if (g_frame_limit > 0) sprintf(val, "%d FPS", g_frame_limit);
	else                   sprintf(val, "OFF");
	settings_line(line, "Frame Limit:", val);
	draw_settings_row(r11, sel == 11, line);

	// sel 12: Intro Video Skip (PER-GAME, gameset.cfg) — OFF / learn / fire.
	if (g_video_skip == VSKIP_CAPTURE)    sprintf(val, "ON - Time capture next boot");
	else if (g_video_skip == VSKIP_TIMED) sprintf(val, "ON - %d.%ds", g_video_skip_ms / 1000,
	                                              (g_video_skip_ms % 1000) / 100);
	else                                  sprintf(val, "OFF");
	settings_line(line, "Intro Video Skip:", val);
	draw_settings_row(r12, sel == 12, line);

	// sel 13: Auto-Open on Boot (PER-GAME, gameset.cfg).
	settings_line(line, "Auto-Open on Boot:", g_autoload ? "Yes" : "OFF");
	draw_settings_row(r13, sel == 13, line);

	// RAM usage readout (three figures, bottom of the screen above the help line).
	{
		u32 s_kb, d_kb, f_kb; char rl[64];
		ram_usage_kb(&s_kb, &d_kb, &f_kb);
		// Compact to fit 60 cols even with a multi-MB Free value.
		sprintf(rl, "RAM:  Plugin static %uK  dynamic %uK  Free %uK",
		        (unsigned)s_kb, (unsigned)d_kb, (unsigned)f_kb);
		dbg_text((60 - (int)strlen(rl)) / 2, 32, BR_GREY, BR_BG, rl);   // just above the hint bar (row 33)
	}
}

// Settings sub-menu (entered from the browser via R-trigger). Saves on close.
// gid = the game folder currently selected in the browser (may differ from the
// running game); the per-game section (Auto-Open) reads/writes THAT game's settings.
// Frame-Limit key auto-repeat timing. hold = consecutive 40ms menu ticks LEFT/RIGHT has
// been held. Returns 1 on the ticks a step should fire: once on the initial press, then
// after a short delay it repeats, accelerating as held. At 40ms/tick: period 5=200ms,
// period 4=160ms. Capped at period 4 (160ms max) to control speed.
static int fl_repeat_fire(int hold)
{
	if (hold < 5)  return 0;          // activation pause: frames 1-4 (~200ms at 40ms/tick)
	if (hold == 5) return 1;          // initial press on frame 5
	if (hold < 9)  return 0;          // delay before auto-repeat (~160ms)
	{
		int period = (hold >= 24) ? 2 : 3;
		return ((hold - 9) % period) == 0;
	}
}

// ── Gamma test pattern (Triangle on the IPS Gamma Fix row) ──────────────────
// Full-screen calibration chart, generated procedurally — nothing is stored (a
// 480x272 16-bit image would be ~255KB, larger than the whole plugin, and this
// is a kernel module). Six 40px bands, top to bottom:
//   0. true gradient 0..255      — continuous ramp: smoothness + banding
//   1. 16-step wedge 0..255      — the same range quantized, for step-to-step reading
//   2. shadow ramp | highlight ramp — the two ends the curve must NOT move
//   3. primaries + secondaries   — hue shift (the pass is per-channel)
//   4. mid-saturation patches    — where the lift is strongest
//   5. pure black | pure white   — clamp reference, both must stay pinned
// The bottom 32px are left clear for the value banner + hint, which run_st_test
// draws AFTER the GE pass so the UI itself stays uncorrected.
// Drawn through dbg_fill_rect rather than raw u32 stores: it carries the REAL
// stride (dbg_bufw, not a hardcoded 512) and packs each color to the game's own
// pixel format — a 5551/565 game would otherwise render this garbled at half
// width. Stays inside 480x272 (dbg_fill_rect does NOT clamp; see the
// out-of-bounds note in gfx.c).
#define GT_RGB(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))   // AABBGGRR, R low
#define GT_BAND 40                    // band height; 6 bands = 240, leaving 32px for the overlay

static void st_test_pattern(void)
{
	static const u8  shadow[8] = {   0,   4,   8,  12,  16,  20,  24,  28 };
	static const u8  highl [8] = { 227, 231, 235, 239, 243, 247, 251, 255 };
	static const u32 prim  [6] = { GT_RGB(255,0,0),   GT_RGB(0,255,0),     GT_RGB(0,0,255),
	                               GT_RGB(0,255,255), GT_RGB(255,0,255),   GT_RGB(255,255,0) };
	static const u32 mid   [6] = { GT_RGB(255,128,0), GT_RGB(224,172,138), GT_RGB(0,128,128),
	                               GT_RGB(128,0,128), GT_RGB(128,128,0),   GT_RGB(128,128,128) };
	int i, v;

	for (i = 0; i < 480; i++) {                    // 0. true gradient, one column per pixel
		v = i * 255 / 479;                         // both ends exact: 0 at x=0, 255 at x=479
		dbg_fill_rect(i, 0, 1, GT_BAND, GT_RGB(v, v, v));
	}
	for (i = 0; i < 16; i++) {                     // 1. wedge, 16 x 30px
		v = i * 17;
		dbg_fill_rect(i * 30, GT_BAND, 30, GT_BAND, GT_RGB(v, v, v));
	}
	for (i = 0; i < 8; i++) {                      // 2. shadows left, highlights right
		v = shadow[i]; dbg_fill_rect(      i * 30, GT_BAND * 2, 30, GT_BAND, GT_RGB(v, v, v));
		v = highl[i];  dbg_fill_rect(240 + i * 30, GT_BAND * 2, 30, GT_BAND, GT_RGB(v, v, v));
	}
	for (i = 0; i < 6; i++) {                      // 3. + 4. color rows, 6 x 80px
		dbg_fill_rect(i * 80, GT_BAND * 3, 80, GT_BAND, prim[i]);
		dbg_fill_rect(i * 80, GT_BAND * 4, 80, GT_BAND, mid[i]);
	}
	dbg_fill_rect(  0, GT_BAND * 5, 240, GT_BAND, GT_RGB(  0,   0,   0));   // 5. clamp reference
	dbg_fill_rect(240, GT_BAND * 5, 240, GT_BAND, GT_RGB(255, 255, 255));
	// Overlay strip: cleared to the menu background so the banner/hint drawn on
	// top of it after the pass sit on a known, uncorrected ground.
	dbg_fill_rect(0, GT_BAND * 6, 480, 272 - GT_BAND * 6, BR_BG);
}

// The test screen. The pass is IN-PLACE, so every change redraws the pattern
// from scratch before re-applying — re-running it over an already-corrected
// screen would compound the curve instead of replacing it. Returns 1 if the
// value changed (caller persists it).
static int run_st_test(void)
{
	SceCtrlData pad;
	int prev, changed = 0, redraw = 1;
	// Auto-repeat state for gamma and temp (like Frame Limit in settings menu).
	int g_hold = 0, g_hold_dir = 0, t_hold = 0, t_hold_dir = 0;

	kpeek(&pad); prev = pad.Buttons;
	for (;;) {
		int pressed;
		if (redraw) {
			char line[64], val[16];
			int applied;
			redraw = 0;
			st_test_pattern();                                      // fresh, uncorrected
			applied = st_apply_test(dbg_fb, dbg_bufw, dbg_pfmt);    // curve it in place
			{
				const char *f = applied ? "L/R:temp  U/D:gamma  Tri/O:exit"
				                        : "GE busy - UNCORRECTED  Tri/O:exit";
				dbg_text((60 - (int)strlen(f)) / 2, 29, BR_GREY, BR_BG, f);
			}
			// Colour temp row (y240..256)
			if      (g_st_temp < 100) sprintf(val, "%d (warm)", g_st_temp);
			else if (g_st_temp > 100) sprintf(val, "%d (cool)", g_st_temp);
			else                       sprintf(val, "neutral");
			settings_line(line, "Color Temp:", val);
			draw_settings_row(30, 1, line);
			// Gamma row (y256..272) — displayed as standard gamma (0.50..2.00).
			sprintf(val, "%d.%02d", g_st_gamma / 100, g_st_gamma % 100);
			settings_line(line, "Gamma:", val);
			draw_settings_row(32, 1, line);
		}
		sceKernelDelayThread(40000);
		kpeek(&pad);
		pressed = pad.Buttons & ~prev;
		prev = pad.Buttons;

		// Colour temp: L/R with auto-repeat
		{
			int dir = (pad.Buttons & PSP_CTRL_RIGHT) ? +1 :
			          (pad.Buttons & PSP_CTRL_LEFT)  ? -1 : 0;
			if (dir != 0) {
				if (dir != t_hold_dir) { t_hold = 0; t_hold_dir = dir; }
				t_hold++;
				if (fl_repeat_fire(t_hold)) {
					if (dir > 0 && g_st_temp < 200) { g_st_temp++; changed = 1; redraw = 1; }
					if (dir < 0 && g_st_temp > 0)   { g_st_temp--; changed = 1; redraw = 1; }
					// Temperature alone drives the pass now (st_active), so this row
					// has to be able to start the worker just like the gamma row.
					if (st_active()) st_ensure_started();
				}
			} else {
				if (t_hold >= 1 && t_hold < 5) {
					if (t_hold_dir > 0 && g_st_temp < 200) { g_st_temp++; changed = 1; redraw = 1; }
					if (t_hold_dir < 0 && g_st_temp > 0)   { g_st_temp--; changed = 1; redraw = 1; }
					if (st_active()) st_ensure_started();
				}
				t_hold = 0; t_hold_dir = 0;
			}
		}
		// Gamma: UP/DOWN with auto-repeat (UP = brighter, DOWN = darker).
		{
			int dir = (pad.Buttons & PSP_CTRL_DOWN) ? +1 :
			          (pad.Buttons & PSP_CTRL_UP)   ? -1 : 0;
			if (dir != 0) {
				if (dir != g_hold_dir) { g_hold = 0; g_hold_dir = dir; }
				g_hold++;
				if (fl_repeat_fire(g_hold)) {
					if (dir > 0 && g_st_gamma > 50)  { g_st_gamma--; changed = 1; redraw = 1; }
					if (dir < 0 && g_st_gamma < 200) { g_st_gamma++; changed = 1; redraw = 1; }
					if (st_active()) st_ensure_started();
				}
			} else {
				if (g_hold >= 1 && g_hold < 5) {
					if (g_hold_dir > 0 && g_st_gamma > 50)  { g_st_gamma--; changed = 1; redraw = 1; }
					if (g_hold_dir < 0 && g_st_gamma < 200) { g_st_gamma++; changed = 1; redraw = 1; }
					if (st_active()) st_ensure_started();
				}
				g_hold = 0; g_hold_dir = 0;
			}
		}
		if (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE)) break;
	}
	st_guard_reset();
	return changed;
}

// Live gamma HUD banner, drawn over the RUNNING game (X on the gamma settings
// row). Same row styling as the settings screen so the value reads identically,
// parked at the bottom. Called from the present hook and — for games that never
// present through it — the gamma worker; both call it AFTER the GE pass, so the
// banner itself stays uncorrected. Adopts the caller's buffer geometry exactly
// like fps_draw does.
void st_hud_draw(void *topaddr, int bufw, int pfmt)
{
	char line[64], val[16];
	if (!topaddr || bufw <= 0) return;
	dbg_fb   = (void *)(0xA0000000 | (u32)topaddr);
	dbg_bufw = bufw;
	dbg_pfmt = pfmt;
	// Hint (y208..216).
	dbg_text(4, 26, BR_WHITE, BR_BG, "L/R:temp  U/D:gamma  X/O:done");
	// Colour temp (y224..240).
	if      (g_st_temp < 100) sprintf(val, "%d (warm)", g_st_temp);
	else if (g_st_temp > 100) sprintf(val, "%d (cool)", g_st_temp);
	else                       sprintf(val, "neutral");
	settings_line(line, "Color Temp:", val);
	draw_settings_row(28, 1, line);
	// Gamma (y240..256) — standard gamma value (0.50..2.00).
	sprintf(val, "%d.%02d", g_st_gamma / 100, g_st_gamma % 100);
	settings_line(line, "Gamma:", val);
	draw_settings_row(30, 1, line);
}

// ── Per-setting explanation pages (R in the Settings menu) ─────────────────
// R on a highlighted settings row opens a full-screen page describing what that
// setting does, how it works, and when to use (or avoid) it. O or R closes and
// returns to the settings list. `sel` is the same row number the input handlers
// use (see draw_settings / run_settings_menu). Drawn with the same chrome as
// every other menu page. Only a debug-build row can ever be 7 (Release hides it
// but still lets it be selected, so the page stays reachable for completeness).
// A single help-page line: text + colour + left column. Colour is used to turn
// each page from one flat white block into a scannable layout: BR_CYAN section
// headers, BR_WHITE body/options, BR_RED cautions. col 3 = body, col 5 = items.
struct exline { const char *t; u32 fg; int col; };
#define EX(t, fg, col) { (t), (fg), (col) }
#define EXH(t)  EX((t), BR_CYAN, 3)    // section header
#define EXB(t)  EX((t), BR_WHITE, 3)   // body text
#define EXI(t)  EX((t), BR_WHITE, 5)   // indented option/item
#define EXC(t)  EX((t), BR_RED, 3)     // caution / important note
#define EXS(t)  EX("", 0, 0)           // spacer (blank line)

static void explain_page(const char *title, const struct exline *ln, int n)
{
	int i;
	draw_screen_chrome(title, "O/R: Back");
	// Body starts two rows below the header chrome (row 5), giving the page
	// breathing room under the title + accent line.
	for (i = 0; i < n; i++)
		if (ln[i].t[0]) dbg_text(ln[i].col, 5 + i, ln[i].fg, BR_BG, ln[i].t);
}

static void settings_explain(int sel)
{
	switch (sel) {
	case 1: {
		static const struct exline b[] = {
			EXB("Which entry is highlighted when the"),
			EXB("save/load menu first opens."),
			EXS(),
			EXH("MODES"),
			EXI("Last - newest save (quickest to load)"),
			EXI("New  - the 'New Savegame' row, ready"),
			EXI("       to make a save."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
		};
		explain_page("Default Slot", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 2: {
		static const struct exline b[] = {
			EXB("Shows a live FPS counter (and optional"),
			EXB("extras) on screen during gameplay."),
			EXS(),
			EXH("MODES  (L/R cycles)"),
			EXI("OFF          - no overlay"),
			EXI("FPS          - frames per second"),
			EXI("FPS + 1%     - adds the 1% Low"),
			EXI("               (worst-stutter figure)"),
			EXI("FPS+1%+Frame - adds a scrolling"),
			EXI("               frametime chart"),
			EXS(),
			EXH("CONTROLS"),
			EXI("Tri/X set the update rate (0.1-1.0s)."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
			EXS(),
			EXH("TIP"),
			EXI("Handy to check a game's performance;"),
			EXI("costs a little CPU. Turn OFF if unused."),
		};
		explain_page("Show FPS", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 3: {
		static const struct exline b[] = {
			EXB("Where the FPS / battery / CPU overlay"),
			EXB("box is anchored on screen."),
			EXS(),
			EXH("OPTIONS  (L/R cycles)"),
			EXI("Up Left / Up Right"),
			EXI("Down Left / Down Right"),
			EXS(),
			EXB("The box keeps the same top-to-bottom"),
			EXB("order (FPS first ... battery last),"),
			EXB("just pinned to the chosen corner."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
			EXS(),
			EXH("TIP"),
			EXI("Pick a corner that does not cover game"),
			EXI("HUD info you want to read."),
		};
		explain_page("Overlay Location", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 4: {
		static const struct exline b[] = {
			EXB("Shows the PSP's CPU and GPU load as"),
			EXB("a percentage, overlaid during play."),
			EXS(),
			EXH("HOW IT'S MEASURED"),
			EXI("CPU - from idle-thread clock time."),
			EXI("GPU - from the Graphics Engine's"),
			EXI("      busy duty-cycle."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
			EXS(),
			EXH("TIP"),
			EXI("See which part is the bottleneck"),
			EXI("(CPU vs GPU). Adds a small per-frame"),
			EXI("cost; turn OFF when unused."),
		};
		explain_page("CPU & GPU Usage", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 5: {
		static const struct exline b[] = {
			EXB("Shows a live battery readout during"),
			EXB("gameplay."),
			EXS(),
			EXH("MODES  (L/R cycles)"),
			EXI("OFF           - no battery overlay"),
			EXI("Percent       - charge %"),
			EXI("Percent+Time  - adds time estimate"),
			EXI("ALL           - every reading"),
			EXS(),
			EXH("CONTROLS"),
			EXI("Tri/X set the REAL battery capacity"),
			EXI("(mAh). The PSP's own meter caps near"),
			EXI("~1800mAh even with a bigger cell, so"),
			EXI("set your cell's real size for an"),
			EXI("accurate % and time."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
		};
		explain_page("Battery Status", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 6: {
		static const struct exline b[] = {
			EXB("Stops the battery from charging once"),
			EXB("it reaches the set level, so a cell"),
			EXB("parked on AC does not sit at 100%."),
			EXS(),
			EXH("VALUES  (L/R cycles)"),
			EXI("OFF - charge normally (default)"),
			EXI("95 / 90 / 85 / 80 / 75 / 70 % -"),
			EXI("  charging is blocked at/above"),
			EXI("  the threshold"),
			EXI("ON - never charge (runs from AC)"),
			EXS(),
			EXH("HOW IT WORKS"),
			EXI("The battery poll thread tells the"),
			EXI("power controller to forbid charging"),
			EXI("once the level is reached, and to"),
			EXI("permit it again below it. The PSP"),
			EXI("then runs from AC while the battery"),
			EXI("holds its charge."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
			EXS(),
			EXH("TIP"),
			EXI("Lithium cells age fastest sitting"),
			EXI("full at high temperature. 80-90% is"),
			EXI("a good parking level for long"),
			EXI("plug-in sessions."),
			EXS(),
			EXC("NOTE: works only while the plugin's"),
			EXC("battery thread is running (it is"),
			EXC("started automatically when this is"),
			EXC("set). A PSP reboot clears the forbid"),
			EXC("state."),
		};
		explain_page("Stop Charging", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 7: {
		static const struct exline b[] = {
			EXB("Raises the CPU clock above stock by"),
			EXB("writing the raw PLL registers."),
			EXS(),
			EXH("VALUES  (L/R cycles)"),
			EXI("OFF      - stock 333 MHz (safe)"),
			EXI("<higher> - faster, but hotter and"),
			EXI("           less stable; a too-high"),
			EXI("           step can hang or crash."),
			EXS(),
			EXC("NOTE: Ark-4's own 'Game overclock'"),
			EXC("setting must be set to 'Overclocked'"),
			EXC("for this to take effect."),
			EXS(),
			EXB("Triangle marks a step 'Stable' (skips"),
			EXB("the every-boot confirm); X clears it."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
			EXS(),
			EXC("Use only a step proven stable on your"),
			EXC("unit. When in doubt, leave OFF."),
		};
		explain_page("Overclock", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 8: {
		static const struct exline b[] = {
			EXB("Controls the plugin's debug output."),
			EXB("Only present in debug builds (a"),
			EXB("release build is silent)."),
			EXS(),
			EXH("MODES  (L/R cycles)"),
			EXI("OFF           - no logging"),
			EXI("Log MS        - to a Memory Stick file"),
			EXI("Log Screen    - checkpoints on screen"),
			EXI("Screen and MS - both"),
			EXI("UART          - to the PC serial port"),
			EXS(),
			EXB("Used for development / diagnosing"),
			EXB("save+load issues."),
			EXS(),
			EXH("SCOPE"),
			EXB("Applies to every game."),
			EXS(),
			EXC("You normally do NOT need this on."),
		};
		explain_page("Debug Messages", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 9: {
		static const struct exline b[] = {
			EXB("Adjusts the picture a game shows,"),
			EXB("saved per game."),
			EXS(),
			EXH("WHAT YOU TUNE"),
			EXI("Gamma - brightness curve (0.50..2.00)"),
			EXI("Temp  - colour temperature (warm..cool)"),
			EXS(),
			EXH("HOW IT'S APPLIED"),
			EXB("The correction is blended over each"),
			EXB("displayed frame by the graphics engine."),
			EXS(),
			EXH("BUTTONS"),
			EXI("Triangle - full-screen test pattern"),
			EXI("X        - live HUD over the real game"),
			EXS(),
			EXH("SCOPE"),
			EXB("Per-game: each game keeps its own value."),
			EXS(),
			EXH("WHY"),
			EXI("Fix a too-dark screen or a colour cast"),
			EXI("without the game's own (often absent)"),
			EXI("picture options."),
		};
		explain_page("Screen Tuning", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 10: {
		static const struct exline b[] = {
			EXB("Applies FPS-unlock codes from a cheat"),
			EXB("DB so a game can run above its cap."),
			EXS(),
			EXH("MODES  (L/R cycles)"),
			EXI("Stock - no unlock (normal limit)."),
			EXI("With cheat.db present, the list shows"),
			EXI("the unlock names found for THIS game."),
			EXS(),
			EXH("SCOPE"),
			EXB("Per-game: stored per game folder."),
			EXS(),
			EXH("CHEAT.DB"),
			EXI("Put cheat.db in ms0:/seplugins/ or"),
			EXI("ms0:/seplugins/SAVESTATE/ to provide"),
			EXI("the codes."),
			EXS(),
			EXC("Unlocking can break some games (physics"),
			EXC("runs faster) - test before relying on it."),
		};
		explain_page("FPS unlocks", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 11: {
		static const struct exline b[] = {
			EXB("Caps the game's frame rate to a fixed"),
			EXB("target (OFF or 20..60 FPS) by pacing"),
			EXB("inside the present hook."),
			EXS(),
			EXB("A stable cap often reads smoother than a"),
			EXB("fluctuating higher number (e.g. a game"),
			EXB("that swings 30-60)."),
			EXS(),
			EXH("SCOPE"),
			EXB("Per-game: stored per game folder."),
			EXS(),
			EXH("NOTE"),
			EXI("The panel is 60Hz, so only 60/30/20 are"),
			EXI("judder-free; 25/35/40/... will judder"),
			EXI("somewhat."),
			EXS(),
			EXH("WHY"),
			EXI("Even out a swinging title, or save"),
			EXI("battery / reduce heat."),
		};
		explain_page("Frame Limit", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	case 12: {
		static const struct exline b[] = {
			EXB("Skips a game's intro / maker videos"),
			EXB("automatically on the NEXT boot."),
			EXS(),
			EXH("HOW IT SKIPS"),
			EXI("- instantly, via a small script patch"),
			EXI("  on the video player API"),
			EXI("- by pulsing the game's OWN skip"),
			EXI("  button (X / START) for the rest"),
			EXS(),
			EXH("MODES  (L/R cycles)"),
			EXI("OFF     - never skip"),
			EXI("capture - learn the intro's length on"),
			EXI("          the next boot"),
			EXI("<time>  - skip once the window is"),
			EXI("          learned"),
			EXS(),
			EXH("CAPTURE"),
			EXI("On the next boot hold D-Pad Right until"),
			EXI("the intro has played; it records how"),
			EXI("long to skip."),
			EXS(),
			EXH("SCOPE"),
			EXB("Per-game: stored per game folder."),
			EXS(),
			EXH("WHY"),
			EXI("Great for games with long, unskippable"),
			EXI("intros."),
		};
		explain_page("Intro Video Skip", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	default: {   // sel 13 (and any unexpected value): Auto-Open on Boot
		static const struct exline b[] = {
			EXB("When ON, the save/load menu opens by"),
			EXB("itself shortly after this game boots"),
			EXB("(if the game has at least one save) -"),
			EXB("so you can load before the title/intro"),
			EXB("plays."),
			EXS(),
			EXH("SCOPE"),
			EXB("Per-game: stored per game folder."),
			EXS(),
			EXH("TIP"),
			EXI("Turn it on for games you load often;"),
			EXI("leave OFF if you would rather open the"),
			EXI("menu manually."),
			EXS(),
			EXC("CAUTION: loading while not in gameplay"),
			EXC("is risky - be aware of the increased"),
			EXC("instability, which varies per game."),
		};
		explain_page("Auto-Open on Boot", b, (int)(sizeof(b)/sizeof(b[0])));
	} break;
	}
	// Let go of the R that opened the page, then wait for O or R to close.
	wait_buttons_up();
	wait_button_edge(PSP_CTRL_RTRIGGER | PSP_CTRL_CIRCLE);
	wait_release(PSP_CTRL_RTRIGGER | PSP_CTRL_CIRCLE);
}

// Returns 1 if the caller should close the WHOLE menu (X on the gamma row hands
// the value to the live HUD, which only makes sense with the game running).
static int run_settings_menu(const char *gid)
{
	SceCtrlData pad;
	int fl_hold = 0, fl_hold_dir = 0;   // Frame-Limit hold state (auto-repeat)
	// Rows: 1 Default Slot, 2 Show FPS (L/R mode, Tri/X update rate), 3 Overlay
	// Location, 4 CPU & GPU Usage, 5 Battery Status, 6 Stop Charging, 7 Overclock
	// | 8 Debug Messages (debug builds only) | 9 Screen Tuning, 10 FPS unlocks,
	// 11 Frame Limit, 12 Intro Video Skip, 13 Auto-Open. Row 8 is debug-only; a
	// release keeps it selectable but undrawn (same as before). Rows 9-13 are the
	// per-game section.
	const int sel_min = DEBUG_BUILD ? 1 : 1;
	int sel = sel_min, prev, changed = 0, gchanged = 0, close_all = 0;
	u32 nav_db_us = 0;   // debounce: ignore L/R for 50ms after UP/DOWN nav
	load_game_settings(gid);   // per-game settings for gid -> g_autoload, g_frame_limit, g_video_skip
	draw_settings(sel, gid);
	kpeek(&pad); prev = pad.Buttons;
	for (;;) {
		int pressed, osel = sel;
		sceKernelDelayThread(40000);
		kpeek(&pad);
		pressed = pad.Buttons & ~prev;
		prev = pad.Buttons;

		// Contiguous rows sel_min..13. Up/Down WRAP around the ends (Up on the first
		// row jumps to the last per-game setting, and vice versa).
		if (pressed & PSP_CTRL_UP)   { sel = (sel > sel_min) ? sel - 1 : 13; }
		if (pressed & PSP_CTRL_DOWN) { sel = (sel < 13)      ? sel + 1 : sel_min; }
		// Debounce: after navigation, ignore L/R for 50ms so the user's
		// thumb rolling off the D-pad doesn't change values on the new row.
		if (sel != osel) {
			nav_db_us = sceKernelGetSystemTimeLow() + 50000;
			// Clear L/R from this iteration's edges AND held state so the
			// new row isn't immediately adjusted by a stale direction.
			pressed &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
			pad.Buttons &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
		}
		if (nav_db_us && (int)(sceKernelGetSystemTimeLow() - nav_db_us) >= 0) {
			nav_db_us = 0;   // window expired
		}
		if (nav_db_us) {
			pressed &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
			pad.Buttons &= ~(PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
		}

		// R: explanation page for the currently highlighted setting. Opens a
		// full-screen "what it does / how / why" page; O or R returns here.
		if (pressed & PSP_CTRL_RTRIGGER) {
			settings_explain(sel);
			draw_settings(sel, gid);
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		// Frame Limit (PER-GAME, row 11): auto-repeat on HOLD, and the value LOOPS
		// (OFF -> 20 -> ... -> 60 -> OFF). Level-driven, not edge-driven, so it sits
		// before the edge-based chain below. Left/Right sweep OFF / 20 / 21 / ... / 60.
		// Applies live — frame_limit_ge reads g_frame_limit every GE submit.
		if (sel == 11) {   // Frame Limit
			int dir = (pad.Buttons & PSP_CTRL_RIGHT) ? +1 :
			          (pad.Buttons & PSP_CTRL_LEFT)  ? -1 : 0;
			if (dir != 0) {
				int step, k;
				if (dir != fl_hold_dir) { fl_hold = 0; fl_hold_dir = dir; }
				fl_hold++;
				if (fl_repeat_fire(fl_hold)) {
					step = 1;   // always 1-step increments
					for (k = 0; k < step; k++) {
						if (dir > 0) {   // increase, wrap 60 -> OFF
							if      (g_frame_limit == 0)  g_frame_limit = 20;
							else if (g_frame_limit >= 60) g_frame_limit = 0;
							else                          g_frame_limit++;
						} else {         // decrease, wrap OFF -> 60
							if      (g_frame_limit == 0)  g_frame_limit = 60;
							else if (g_frame_limit <= 20) g_frame_limit = 0;
							else                          g_frame_limit--;
						}
					}
					if (DBG_UART()) { char b[64]; sprintf(b, "[FLIMIT] set %d fps (0=off)", g_frame_limit); uart_puts(b); }
					gchanged = 1;
					draw_settings(sel, gid);
				}
			} else {
				if (fl_hold >= 1 && fl_hold < 5) {
					if (fl_hold_dir > 0) {   // increase, wrap 60 -> OFF
						if      (g_frame_limit == 0)  g_frame_limit = 20;
						else if (g_frame_limit >= 60) g_frame_limit = 0;
						else                          g_frame_limit++;
					} else {         // decrease, wrap OFF -> 60
						if      (g_frame_limit == 0)  g_frame_limit = 60;
						else if (g_frame_limit <= 20) g_frame_limit = 0;
						else                          g_frame_limit--;
					}
					if (DBG_UART()) { char b[64]; sprintf(b, "[FLIMIT] set %d fps (0=off)", g_frame_limit); uart_puts(b); }
					gchanged = 1;
					draw_settings(sel, gid);
				}
				fl_hold = 0; fl_hold_dir = 0;
			}
		}

		// FPS unlocks (PER-GAME, sel 10): Left/Right cycle Stock / <DB FPS cheats>, wrap.
		// cheats_fps_cycle persists the choice and (re)starts the per-vblank apply
		// thread, so no `changed` flag is needed here.
		if (sel == 10 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			cheats_fps_cycle((pressed & PSP_CTRL_RIGHT) ? +1 : -1);
			draw_settings(sel, gid);
		}

		// Triangle / X are the "up / down" pair on rows that need a second axis
		// (Left/Right stays "change value" elsewhere):
		//   Show FPS (row 2, while a mode is on): Triangle = +0.1s, X = -0.1s on the
		//     update rate (g_fps_rate 1..10 = 0.1..1.0s -> fps_window_us).
		//   Overclock (row 6, non-stock only): Triangle = mark STABLE (skip the boot confirm,
		//     shown RED), X = clear it back to the normal white "ask every boot".
		//   Intro Video Skip (row 11, only once a timer is captured = TIMED): Triangle = +0.1s,
		//     X = -0.1s on the learned window (clamped to 0.1s .. VSKIP_LEARN_MAX_MS).
		if (sel == 2 && g_show_fps_overlay > 0 && (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS))) {
			if ((pressed & PSP_CTRL_TRIANGLE) && g_fps_rate < 10) g_fps_rate++;
			if ((pressed & PSP_CTRL_CROSS)    && g_fps_rate > 1)  g_fps_rate--;
			changed = 1;
			draw_settings(sel, gid);
		}
		if (sel == 7 && g_overclock_id > 0 && (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS))) {
			if (pressed & PSP_CTRL_TRIANGLE) g_overclock_stable = 1;
			if (pressed & PSP_CTRL_CROSS)    g_overclock_stable = 0;
			changed = 1;
			draw_settings(sel, gid);
		}
		// Battery Status (row 5): Triangle/X set the REAL battery capacity in mAh.
		// 100mAh steps, 1000..8000; X below 1000 returns to Stock (0 = use the BMS value).
		if (sel == 5 && (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS))) {
			if (pressed & PSP_CTRL_TRIANGLE) {
				if (g_batt_real_mah == 0)          g_batt_real_mah = 1000;
				else if (g_batt_real_mah < 8000)   g_batt_real_mah += 100;
			}
			if (pressed & PSP_CTRL_CROSS) {
				if (g_batt_real_mah <= 1000)       g_batt_real_mah = 0;
				else                               g_batt_real_mah -= 100;
			}
			changed = 1;
			draw_settings(sel, gid);
		}
		// Screen Tuning (sel 9): Triangle opens the full-screen test pattern with
		// live gamma + colour temp adjustment.  X opens the in-game HUD.
		if (sel == 9 && (pressed & PSP_CTRL_TRIANGLE)) {
			// Exit-path checkpoints: last [STX] line before silence = the step.
			if (DBG_UART()) uart_puts("[STX] test: enter");
			if (run_st_test()) { changed = 1; st_save_game_gamma(); }   // gamma is per-game
			if (DBG_UART()) uart_puts("[STX] test: returned");
			draw_settings(sel, gid);
			kpeek(&pad); prev = pad.Buttons;
			if (DBG_UART()) uart_puts("[STX] test: redrawn, back in settings");
		}
		// X: tune against the REAL game instead of a chart — close the menu
		// entirely and leave a HUD on screen that owns the D-Pad until X/O.
		if (sel == 9 && (pressed & PSP_CTRL_CROSS)) {
			if (DBG_UART()) uart_puts("[STX] hud: opening");
			st_ensure_started();   // worker draws the HUD for present-less games + persists edits
			st_hud_open();
			if (DBG_UART()) uart_puts("[STX] hud: worker up, hud armed");
			changed = 1;              // saved below; the HUD's own edits are saved by the worker
			close_all = 1;
			break;
		}
		if (sel == 12 && g_video_skip == VSKIP_TIMED && (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS))) {
			if (pressed & PSP_CTRL_TRIANGLE) g_video_skip_ms += 100;
			if (pressed & PSP_CTRL_CROSS)    g_video_skip_ms -= 100;
			if (g_video_skip_ms < 100)                 g_video_skip_ms = 100;
			if (g_video_skip_ms > VSKIP_LEARN_MAX_MS)  g_video_skip_ms = VSKIP_LEARN_MAX_MS;
			if (DBG_UART()) { char b[48]; sprintf(b, "[VSKIP] window set to %d.%03ds", g_video_skip_ms / 1000, g_video_skip_ms % 1000); uart_puts(b); }
			gchanged = 1;
			draw_settings(sel, gid);
		}

		if (sel == 2 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Show FPS: Left/Right cycle OFF / FPS / FPS + 1% / FPS + 1% + Frametime
			// (0..3), wrapping. The 1% and chart flags are DERIVED from the mode —
			// the same mapping load_settings applies (keep both in sync).
			if (pressed & PSP_CTRL_LEFT)  g_show_fps_overlay = (g_show_fps_overlay > 0) ? g_show_fps_overlay - 1 : 3;
			if (pressed & PSP_CTRL_RIGHT) g_show_fps_overlay = (g_show_fps_overlay < 3) ? g_show_fps_overlay + 1 : 0;
			g_fps_show_lows = (g_show_fps_overlay >= 2) ? 1 : 0;
			g_show_ft_chart = (g_show_fps_overlay >= 3) ? 1 : 0;
			if (g_show_fps_overlay) fps_poll_ensure_started();
			changed = 1;
			draw_settings(sel, gid);
		} else if (sel == 3 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Overlay Location: Left/Right cycle Up Left / Up Right / Down Left / Down
			// Right (0..3), wrapping. Applied live by the next overlay draw (fps_draw).
			if (pressed & PSP_CTRL_LEFT)  g_overlay_pos = (g_overlay_pos > 0) ? g_overlay_pos - 1 : 3;
			if (pressed & PSP_CTRL_RIGHT) g_overlay_pos = (g_overlay_pos < 3) ? g_overlay_pos + 1 : 0;
			changed = 1;
			draw_settings(sel, gid);
		} else if (sel == 5 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Battery Status: Left/Right cycle Off / Percent / Percent+Time / ALL (0..3), wrapping.
			if (pressed & PSP_CTRL_LEFT)  g_show_battery = (g_show_battery > 0) ? g_show_battery - 1 : 3;
			if (pressed & PSP_CTRL_RIGHT) g_show_battery = (g_show_battery < 3) ? g_show_battery + 1 : 0;
			if (g_show_battery) {   // lazy: first time it's turned on this session
				fps_poll_ensure_started();
				battery_poll_ensure_started();
			}
			changed = 1;
			draw_settings(sel, gid);
		} else if (sel == 6 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Stop Charging (GLOBAL): Left/Right cycle OFF / 95 / 90 / 85 / 80 / 75 /
			// 70 / ON, wrapping. While set, battery_charge_gate (sysstats.c) forbids
			// charging at/above the threshold via scePowerBatteryForbidCharging and
			// re-permits below it; ON (internal 100) forbids unconditionally. The poll
			// thread picks the new value up on its next cycle, so no live apply is
			// needed here.
			static const int sc_steps[8] = { 0, 95, 90, 85, 80, 75, 70, 100 };
			int sc = 0;   // 0=OFF, 1..6 = 95..70, 7=ON (internal 100)
			if      (g_batt_stop_charge >= 100) sc = 7;
			else if (g_batt_stop_charge >= 95) sc = 1;
			else if (g_batt_stop_charge >= 90) sc = 2;
			else if (g_batt_stop_charge >= 85) sc = 3;
			else if (g_batt_stop_charge >= 80) sc = 4;
			else if (g_batt_stop_charge >= 75) sc = 5;
			else if (g_batt_stop_charge >= 70) sc = 6;
			if (pressed & PSP_CTRL_LEFT)  sc = (sc > 0) ? sc - 1 : 7;
			if (pressed & PSP_CTRL_RIGHT) sc = (sc < 7) ? sc + 1 : 0;
			g_batt_stop_charge = sc_steps[sc];
			if (g_batt_stop_charge > 0) battery_poll_ensure_started();   // gate needs the poll thread
			changed = 1;   // global settings.cfg
			draw_settings(sel, gid);
		} else if (sel == 7 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Overclock: Left/Right step the multiplier table (0 = stock 333MHz). Applied
			// immediately, live — see oc_apply. Deliberately does NOT wrap (unlike the other
			// settings): wrapping would let LEFT at stock jump straight to the top step.
			if ((pressed & PSP_CTRL_LEFT)  && g_overclock_id > 0)             g_overclock_id--;
			if ((pressed & PSP_CTRL_RIGHT) && g_overclock_id < OC_STEPS - 1)  g_overclock_id++;
			g_overclock_stable = 0;   // a newly picked step isn't vouched for -> confirm again next boot
			changed = 1;
			oc_apply(g_overclock_id);
			draw_settings(sel, gid);
		} else if (sel == 8 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Debug Messages (merged with UART Log): Left/Right cycle OFF / Log MS / Log Screen /
			// Screen and MS / UART (0..4), wrapping. Internally: 0-3 = g_show_debug modes,
			// 4 = UART (g_uart_log=1, g_show_debug=0).
			int mode = (g_uart_log && g_show_debug == 0) ? 4 : g_show_debug;
			if (pressed & PSP_CTRL_LEFT)  mode = (mode > 0) ? mode - 1 : 4;
			if (pressed & PSP_CTRL_RIGHT) mode = (mode < 4) ? mode + 1 : 0;
			// Map mode back to g_show_debug/g_uart_log.
			if (mode == 4) { g_uart_log = 1; g_show_debug = 0; }
			else { g_uart_log = 0; g_show_debug = mode; }
			changed = 1;
			draw_settings(sel, gid);
		} else if (sel != 2 && sel != 3 && sel != 5 && sel != 6 && sel != 7 && sel != 8 && sel != 9 && sel != 10 && sel != 11 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Excludes rows with their own L/R handling: 2 Show FPS, 3 Overlay Location,
			// 5 Battery, 6 Stop Charging, 7 Overclock, 8 Debug, 9 Screen Tuning (Tri/X only),
			// 10 FPS unlocks, 11 Frame Limit (hold-repeat). Plain rows change only on
			// Left/Right; X is the "down/disable/decrease" key on the Overclock + Video
			// Skip rows.
			if      (sel == 1) { g_default_slot     = !g_default_slot;     changed  = 1; }   // global
			else if (sel == 4) {                                                             // global
				g_show_cpu_usage = !g_show_cpu_usage;
				if (g_show_cpu_usage) fps_poll_ensure_started();
				changed = 1;
			}
			else if (sel == 12)  {
				// Intro Video Skip: Cycles OFF -> capture -> (learned, only once one exists) -> OFF.
				// Takes effect on the game's NEXT boot (the watcher is started once from menu_thread
				// at startup, so toggling here cannot affect this session).
				if      (g_video_skip == VSKIP_OFF)     g_video_skip = VSKIP_CAPTURE;
				else if (g_video_skip == VSKIP_CAPTURE) g_video_skip = (g_video_skip_ms > 0) ? VSKIP_TIMED : VSKIP_OFF;
				else                                    g_video_skip = VSKIP_OFF;
				gchanged = 1;
			}
			else if (sel == 13) { g_autoload         = !g_autoload;         gchanged = 1; }   // per-game
			draw_settings(sel, gid);
		} else if (sel != osel) {
			draw_settings(sel, gid);
		}
		// O or L-trigger -> back to the save folder (L continues the L|save|R strip).
		if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_LTRIGGER)) break;
	}
	if (changed)  save_settings();            // global settings.cfg
	if (gchanged) save_game_settings(gid);    // per-game gameset.cfg (for gid)
	if (close_all) { game_frame_limit_load(); return 1; }   // straight out to the game (HUD)
	// This screen loaded gid's per-game values, and gid may be a DIFFERENT game's
	// folder than the one running (the browser can switch games) — so re-sync the
	// live frame cap to the RUNNING game, or we'd pace it with another game's limit.
	game_frame_limit_load();
	return 0;
}

// ── Game/folder browser ──────────────────────────────────────
// Lists the per-game save folders under SAVESTATE/ so the user can switch which game's
// saves to browse (L-trigger from the save browser). Same look as the save browser:
// a row band, a left preview box (black placeholder for a future game image) + game id.
#define MAX_GAME_ROWS 64
#define FIO_DIR_ATTR  0x10        // FIO_SO_IFDIR: directory bit in dirent st_attr
static char g_game_ids[MAX_GAME_ROWS][20];
char g_game_titles[MAX_GAME_ROWS][40];   // PARAM.SFO TITLE per folder (title.txt), "" if none - truncated to 39 chars
static int  g_game_count;

static void enumerate_game_folders(void)
{
	SceUID dfd; SceIoDirent ent;
	g_game_count = 0;
	dfd = sceIoDopen("ms0:/seplugins/SAVESTATE");
	if (dfd < 0) return;
	memset(&ent, 0, sizeof(ent));
	while (sceIoDread(dfd, &ent) > 0 && g_game_count < MAX_GAME_ROWS) {
		if ((ent.d_stat.st_attr & FIO_DIR_ATTR) && ent.d_name[0] != '.') {  // subdirs only
			strncpy(g_game_ids[g_game_count], ent.d_name, 19);
			g_game_ids[g_game_count][19] = '\0';
			read_folder_title(ent.d_name, g_game_titles[g_game_count], sizeof(g_game_titles[0]));
			g_game_count++;
		}
		memset(&ent, 0, sizeof(ent));
	}
	sceIoDclose(dfd);
}

// Blit a game's <gameid>/Game.thb (120x68 565) at (px,py); black box if missing.
// Game.thb is the newest save's thumbnail, copied there on each save (copy_game_thumb).
static void draw_game_thumb(int px, int py, const char *gameid)
{
	char p[96];
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/Game.thb", gameid);
	draw_thumb_file(px, py, p);
}

static void draw_game_one(int e, int idx, int selected)
{
	u32 bg;
	int r    = draw_row_band(e, selected, &bg);
	int py   = r * 8;
	int tcol = 18;
	int trow = r + (BR_ROW_H / 2) - 1;
	if (idx < g_game_count) {
		draw_game_thumb(12, py + 6, g_game_ids[idx]);            // Game.thb preview (black if none)
		// Title (if known) in front/above, ID below in grey; ID-only otherwise.
		if (g_game_titles[idx][0]) {
			dbg_text(tcol, trow - 1, BR_WHITE, bg, g_game_titles[idx]);
			dbg_text(tcol, trow + 1, BR_GREY,  bg, g_game_ids[idx]);
		} else {
			dbg_text(tcol, trow, BR_WHITE, bg, g_game_ids[idx]);
		}
	}
}

static void draw_game_list(int sel, int top)
{
	draw_list_rows(sel, top, g_game_count, draw_game_one);
}

// Recursively delete a folder and all its contents (files + subdirs).
// Returns 0 on success, <0 on error (partial delete possible).
static int delete_game_folder(const char *path)
{
	SceUID dfd; SceIoDirent ent; int rv = 0;
	char subpath[256];
	dfd = sceIoDopen(path);
	if (dfd < 0) return dfd;  // folder open failed
	memset(&ent, 0, sizeof(ent));
	while (sceIoDread(dfd, &ent) > 0) {
		if (ent.d_name[0] == '.') {
			memset(&ent, 0, sizeof(ent));
			continue;  // skip . and ..
		}
		if (strlen(path) + strlen(ent.d_name) + 2 > sizeof(subpath)) {
			memset(&ent, 0, sizeof(ent));
			continue;  // path too long, skip
		}
		sprintf(subpath, "%s/%s", path, ent.d_name);
		if (ent.d_stat.st_attr & FIO_DIR_ATTR) {
			rv = delete_game_folder(subpath);  // recursively delete subdirs
		} else {
			rv = sceIoRemove(subpath);  // delete file
		}
		memset(&ent, 0, sizeof(ent));
		if (rv < 0) break;
	}
	sceIoDclose(dfd);
	if (rv >= 0) rv = sceIoRmdir(path);  // delete the now-empty folder
	return rv;
}

static void draw_game_browser(int sel, int top)
{
	draw_screen_chrome("Select Game", "Up/Dn   X: Open   /\\:Delete   R/O: Return");
	if (g_game_count == 0)
		dbg_text((60 - 23) / 2, BR_LIST_ROW + 2, BR_GREY, BR_BG, "(no save folders found)");
	draw_game_list(sel, top);
}

// Show the folder list; write the chosen game id to out (size sz) and return 1 on X,
// 0 on O (cancel). Caller has already done dbg_init().
static int run_game_browser(char *out, int sz)
{
	SceCtrlData pad;
	int sel = 0, top = 0, prev;
	enumerate_game_folders();
	draw_game_browser(sel, top);
	kpeek(&pad); prev = pad.Buttons;
	for (;;) {
		int pressed, osel = sel, otop = top;
		sceKernelDelayThread(40000);
		kpeek(&pad);
		pressed = pad.Buttons & ~prev;
		prev = pad.Buttons;
		if (pressed & PSP_CTRL_UP)   { if (sel > 0) sel--; }
		if (pressed & PSP_CTRL_DOWN) { if (sel < g_game_count - 1) sel++; }
		// O or R-trigger -> back to the save folder (R continues the L|save|R strip).
		if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_RTRIGGER)) return 0;
		if ((pressed & PSP_CTRL_CROSS) && g_game_count > 0) {
			strncpy(out, g_game_ids[sel], sz - 1); out[sz - 1] = '\0';
			return 1;
		}
		// ── DELETE (Triangle) ── existing game folder only; confirm; stay in browser.
		if ((pressed & PSP_CTRL_TRIANGLE) && g_game_count > 0) {
			char gamefolder[128];
			sprintf(gamefolder, "ms0:/seplugins/SAVESTATE/%s", g_game_ids[sel]);
			if (confirm("Delete this game?", msg_box_y(sel, top))) {
				delete_game_folder(gamefolder);
				enumerate_game_folders();
				if (sel >= g_game_count && g_game_count > 0) sel = g_game_count - 1;
				top = 0;
			}
			draw_game_browser(sel, top);
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}
		if (sel < top) top = sel;
		if (sel >= top + BR_VISIBLE) top = sel - BR_VISIBLE + 1;
		if (sel != osel || top != otop) draw_game_list(sel, top);
	}
}
// Read a save file's plugin-version stamp (header[1]). Returns 0 if unreadable or
// not a valid savestate. Used to warn before loading a save made by a different
// plugin build (a load resumes THAT build's kernel/plugin — risky if it differs).
static u32 read_save_version(const char *path)
{
	SceUID fd; u32 hdr[SAVE_HEADER_WORDS]; u32 v = 0;   // old shorter saves still read fine (only magic+version used)
	fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd < 0) return 0;
	if (sceIoRead(fd, hdr, sizeof(hdr)) == (int)sizeof(hdr) && hdr[0] == SAVESTATE_MAGIC)
		v = hdr[1];
	sceIoClose(fd);
	return v;
}

// ── Memory-Stick availability probe (menu open) ──
// After freezing, test the MS hazard directly: open a file that always exists via
// sceIoOpenAsync (the async IO thread blocks on the lock, not us) and poll it briefly.
// Completed = lock free, safe to browse. Still pending = a frozen thread holds it ->
// caller unfreezes.

// The save browser: soft-freeze, enumerate, draw, navigate, and act (Square=save,
// X=load, Triangle=delete; confirm for existing slots, none for a new save). A
// save or load CLOSES the browser (on load, the system becomes the saved state and
// resumes there — the menu does not reappear). L-trigger -> game/folder list.
// POPS (PS1) doesn't repaint outside its game area, so blank the display buffer
// black before the game resumes to avoid menu residue in the side bars.
static void pops_clear_on_menu_close(void)
{
	if (!g_is_pops) return;
	dbg_fill_rect(0, 0, 480, 272, 0xFF000000);
}

// ── One-time Welcome screen ────────────────────────────────────────────────
// Shown the first time the menu opens (g_welcome_shown starts 0 and persists in
// settings.cfg word [12] once the user presses X). Full-screen, same chrome as
// the other menu pages. Tells the user it's beta, that not every game is verified
// (esp. save/load), the recommended safe workflow, and where cheat.db goes.
static void run_welcome_screen(void)
{
	static const struct exline b[] = {
		EXB("Welcome to PSPFatSave v" VERSION_STRING "!"),
		EXS(),
		EXB("This is BETA software. Every feature here"),
		EXB("was very hard to build on a closed system"),
		EXB("like the PSP, so please expect rough edges."),
		EXS(),
		EXC("NOT every game has been tested -"),
		EXC("especially saving and loading."),
		EXC("Always be careful:"),
		EXS(),
		EXH("SAFE WORKFLOW"),
		EXI("First test saving while in GAMEPLAY -"),
		EXI("not in the menu or while loading."),
		EXS(),
		EXI("a) Also save with the game's OWN save"),
		EXI("   feature, in between PSPFatSave saves."),
		EXS(),
		EXI("b) Test a NEW game in this order:"),
		EXI("   1. Save"),
		EXI("   2. Quit the game"),
		EXI("   3. Load the game"),
		EXI("   4. Load the save"),
		EXS(),
		EXH("CHEATS"),
		EXI("If you want the FPS unlock feature,"),
		EXI("place cheat.db into ms0:/seplugins/."),
		EXS(),
		EXB("Press X to continue."),
	};
	int i, n = (int)(sizeof(b)/sizeof(b[0]));
	draw_screen_chrome("Welcome", "X: Continue");
	for (i = 0; i < n; i++)
		dbg_text(b[i].col, 3 + i, b[i].fg, BR_BG, b[i].t);
	wait_button_edge(PSP_CTRL_CROSS);
	wait_release(PSP_CTRL_CROSS);
	g_welcome_shown = 1;                     // never show again
	save_settings();                         // persist the flag (MS safe: probe passed)
	if (DBG_UART()) uart_puts("[WELC] welcome shown, persisted");
}

void run_save_browser(void)
{
	char dir[80], path[128], cur_id[20];
	int load_only;
	int total, sel, top, prev;
	SceCtrlData pad;

	g_browser_opened = 0;                  // becomes 1 only if the freeze below succeeds and we open
	g_menu_quiet = 1;                      // keep the paced CP checkpoints quiet during the browse
	// Pause-only freeze + MS probe. The menu needs exactly two things: the game
	// paused, and the MS usable from this thread. Use the FAST gate (first observed
	// WAITING, or ~0.5s best-effort, then a dispatch-off suspend — safe for READY
	// threads). The MS-lock hazard is checked directly after freezing
	// (ms_probe_after_freeze); if the lock is held, unfreeze so the holder finishes
	// and retry.
	{
		int attempt, opened = 0;
		gatelog_reset();                                 // buffered [PRE]/[SUS] from this open only
		for (attempt = 0; attempt < 8 && !opened; attempt++) {
			int fails;
			if (attempt) sceKernelDelayThread(20000);    // give the lock holder / busy thread a window
			fails = suspend_escalating(0, 500);       // menu: FAST gate (dispatch-off freeze; NO MS I/O until the probe)
			if (fails != 0) {                            // a thread wouldn't freeze -> undo, retry
				resume_game_threads();
				continue;
			}
			if (ms_probe_after_freeze()) { opened = 1; break; }   // MS lock free -> browse
			resume_game_threads();                       // frozen lock-holder -> let it finish
			ms_probe_reap();                             // collect the parked probe fd
		}
		if (!opened) {                                   // never froze cleanly with a free MS lock
			g_menu_quiet = 0;
			WriteDebugLogHexRaw("[MENU] open FAILED (freeze/MS-busy), attempts=", (u32)attempt);
			gatelog_flush();                             // game resumed -> MS safe again
			return;                                      // g_browser_opened stays 0 -> caller can retry
		}
		// Probe passed: the MS is usable from here on. Flush the buffered gate
		// diagnostics + one summary line when it did NOT open on the first try.
		gatelog_flush();
		if (attempt > 0) WriteDebugLogHex("[MENU] open ok after retries=", (u32)attempt);
	}
	g_browser_opened = 1;                  // freeze succeeded -> the browser is opening for real
	g_menu_quiet = 0;
	sceIoMkdir("ms0:/seplugins/SAVESTATE", 0777);
	dbg_init();                            // grab the currently-displayed framebuffer

	// One-time beta welcome: show on the very first menu open (game frozen here,
	// MS usable). Persists via settings.cfg after X, so it never appears again.
	if (!g_welcome_shown) run_welcome_screen();

	// Start at the running game's folder (or "globalstate" if none).
	strncpy(cur_id, umdid[0] ? umdid : "globalstate", 19); cur_id[19] = '\0';
	sprintf(dir, "ms0:/seplugins/SAVESTATE/%s", cur_id);
	strcpy(g_browse_dir, dir);             // draw_thumb() builds .thb paths from this
	sceIoMkdir(dir, 0777);
	enumerate_saves(dir);
	// SAVE is only for the running game's own folder; browsing another game (via L-trigger)
	// is load_only (saving the live game into another game's folder is meaningless).
	load_only = (umdid[0] && strcmp(cur_id, umdid) != 0);
	load_game_settings(cur_id);            // per-game Auto-Open + Save Compression for this folder
	// Stamp the running game's title into its folder so the game-folder list can show it.
	if (!load_only && g_game_title[0]) {
		char tp[96]; SceUID tf;
		sprintf(tp, "%s/title.txt", dir);
		tf = sceIoOpen(tp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if (tf >= 0) { sceIoWrite(tf, g_game_title, (int)strlen(g_game_title)); sceIoClose(tf); }
	}
	// Auto-open is a load-oriented launch: hide the "New Savegame" row (saves only) and
	// start on the newest save. A NOTE-tap open shows New Savegame and honors Default Slot.
	g_show_newsave = g_autoopen_launch ? 0 : 1;
	total = g_row_count + g_show_newsave;
	// Newest save sits at index g_show_newsave (0 when hidden, 1 after the New row). Select
	// it when Default Slot = Last or auto-opened; otherwise the New Savegame row (idx 0).
	sel   = ((g_default_slot || g_autoopen_launch) && g_row_count > 0) ? g_show_newsave : 0;
	g_autoopen_launch = 0;                 // one-shot
	top   = 0;
	draw_browser(sel, top, cur_id);        // full draw once
	kpeek(&pad); prev = pad.Buttons;

	for (;;) {
		int pressed, osel = sel, otop = top;

		sceKernelDelayThread(40000);       // ~25Hz input poll
		kpeek(&pad);
		pressed = pad.Buttons & ~prev;     // rising edges
		prev = pad.Buttons;

		if (pressed & PSP_CTRL_UP)     { if (sel > 0)         sel--; }
		if (pressed & PSP_CTRL_DOWN)   { if (sel < total - 1) sel++; }

		if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_NOTE)) {   // close (O or NOTE)
			wait_buttons_up();             // don't leak the close press into the game
			arm_input_suppress();          // ...nor any backlog sampled while the menu was up
			pops_clear_on_menu_close();    // POPS won't repaint the side bars
			resume_game_threads();
			return;
		}

		// ── GAME LIST (L-trigger) ── switch to another game's save folder.
		if (pressed & PSP_CTRL_LTRIGGER) {
			char pick[20];
			if (run_game_browser(pick, sizeof(pick))) {
				strncpy(cur_id, pick, 19); cur_id[19] = '\0';
				sprintf(dir, "ms0:/seplugins/SAVESTATE/%s", cur_id);
				strcpy(g_browse_dir, dir);
				sceIoMkdir(dir, 0777);
				enumerate_saves(dir);
				load_only = (umdid[0] && strcmp(cur_id, umdid) != 0);
				load_game_settings(cur_id);    // reload per-game Auto-Open + Compression for the picked folder
				total = g_row_count + g_show_newsave;
				sel = (g_default_slot && g_row_count > 0) ? g_show_newsave : 0;
				top = 0;
			}
			draw_browser(sel, top, cur_id);
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		// ── VIEW (Left) ── fullscreen preview of an existing save; Right/O closes.
		if ((pressed & PSP_CTRL_LEFT) && sel >= g_show_newsave && g_row_count > 0) {
			show_screenshot(g_rows[sel - g_show_newsave].name);
			draw_browser(sel, top, cur_id);          // restore the browser
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		// ── SETTINGS (R-trigger) ── plugin settings sub-menu; L/O return to saves.
		if (pressed & PSP_CTRL_RTRIGGER) {
			if (run_settings_menu(cur_id)) {         // gamma HUD: close everything, game keeps running
				// Exit-path UART checkpoints (see the [STX] note in run_settings_menu).
				if (DBG_UART()) uart_puts("[STX] exit: settings closed (hud)");
				wait_buttons_up();                   // don't leak the close press into the game
				if (DBG_UART()) uart_puts("[STX] exit: buttons up");
				arm_input_suppress();                // ...nor any backlog sampled while the menu was up
				pops_clear_on_menu_close();          // POPS won't repaint the side bars
				resume_game_threads();
				if (DBG_UART()) uart_puts("[STX] exit: game threads resumed");
				return;
			}
			draw_browser(sel, top, cur_id);          // restore the browser
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		// ── SAVE (Cross/X) ── new slot = no confirm; existing = confirm overwrite.
		// Only for the running game's own folder (load_only browses can't save).
		if (pressed & PSP_CTRL_CROSS) {
			if (load_only) {
				// Browsing a DIFFERENT game's folder than the one running: saving here is
				// disabled (it would write this game's state into another game's folder).
				// Red notice where the confirm prompt normally appears; any key dismisses.
				info_msg_red("Saving disabled", "Different game", msg_box_y(sel, top));
				draw_browser(sel, top, cur_id);
				kpeek(&pad); prev = pad.Buttons;
				continue;
			}
			if (free_kernel_ram_kb() < LOW_RAM_SAVE_LOAD_KB) {
				// Free kernel RAM too low to attempt a save.
				info_msg_red("Save disabled", "Free RAM too low", msg_box_y(sel, top));
				draw_browser(sel, top, cur_id);
				kpeek(&pad); prev = pad.Buttons;
				continue;
			}
			int go = 1;
			if (g_show_newsave && sel == 0) {        // New Savegame -> unique name
				if (g_row_count >= MAX_SAVE_ROWS) {  // cap: the list shows at most 32
					info_msg("Max 32 saves!", "Delete one first", msg_box_y(sel, top));
					go = 0;
				} else {
					u64 tick = 0; sceRtcGetCurrentTick(&tick);
					sprintf(path, "%s/%08X%08X.bin", dir, (u32)(tick >> 32), (u32)tick);
				}
			} else if (g_row_count > 0 && sel >= g_show_newsave) {   // existing -> confirm
				go = confirm("Overwrite this save?", msg_box_y(sel, top));
				if (go) sprintf(path, "%s/%s", dir, g_rows[sel - g_show_newsave].name);
			} else {
				// No valid slot: auto-open view (no New-Savegame row) with all saves
				// deleted. g_rows[] is stale here — do NOT build a path from it.
				go = 0;
			}
			if (go) {
				// Close the menu FIRST: resume so the game redraws a clean frame, then
				// save (FreezeSave re-freezes). Clears g_menu_open before the snapshot.
				// Wait for the trigger/confirm X to be RELEASED before resuming so the
				// still-held button isn't captured into the savestate.
				wait_buttons_up();
				// Suppress+drain BOTH input paths across the window the game runs before
				// FreezeSave re-freezes it, and force both back to 0 so the snapshot never
				// captures a nonzero value.
				arm_input_suppress();
				// Hold the gamma correction OFF for the whole save (g_st_op_hold). It is
				// cleared when the game threads resume after the save or on an abort, and
				// captured =1 so a later LOAD stays held until its resumed tail clears it.
				g_st_op_hold = 1;
				g_menu_open = 0;
				resume_game_threads();
				sceKernelDelayThread(100000);        // ~100ms: let the game redraw (both paths drain here)
				g_suppress_latch = 0;
				g_suppress_posbuf_calls = 0;
				FreezeSave(path);                    // own freeze + save + resume
				return;
			}
			draw_browser(sel, top, cur_id);          // confirm cancelled -> redraw
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		// ── LOAD (Square) ── existing only; always confirm.
		if (pressed & PSP_CTRL_SQUARE) {
			if (load_only) {
				// Browsing a DIFFERENT game's folder than the one running: loading here is
				// disabled. Same red notice as the disabled Save above; any key dismisses.
				info_msg_red("Loading disabled", "Different game", msg_box_y(sel, top));
				draw_browser(sel, top, cur_id);
				kpeek(&pad); prev = pad.Buttons;
				continue;
			}
			if (free_kernel_ram_kb() < LOW_RAM_SAVE_LOAD_KB) {
				// See the SAVE guard above for why 50KB.
				info_msg_red("Load disabled", "Free RAM too low", msg_box_y(sel, top));
				draw_browser(sel, top, cur_id);
				kpeek(&pad); prev = pad.Buttons;
				continue;
			}
			if (sel >= g_show_newsave && g_row_count > 0) {
				int go;
				sprintf(path, "%s/%s", dir, g_rows[sel - g_show_newsave].name);
				// A load resumes the SAVE's own kernel/plugin build; if it differs from the
				// running one, warn and require confirmation.
				{
					u32 sv = read_save_version(path);
					if (sv != SAVESTATE_VERSION)
						go = confirm_version_load(sv, SAVESTATE_VERSION, msg_box_y(sel, top));
					else
						go = confirm("Load this save?", msg_box_y(sel, top));
				}
				if (go) {
					// Same release-wait as the SAVE trigger: the game runs briefly
					// until FreezeLoad's freeze; a held confirm-X is game input there.
					// (Cosmetic for LOAD — the loaded state replaces the session — but
					// don't let the game twitch on the way out.)
					wait_buttons_up();
					arm_input_suppress();
					g_st_op_hold = 1;                // hold gamma off through the load (see SAVE trigger)
					g_menu_open = 0;
					resume_game_threads();           // close the menu first
					FreezeLoad(path);                // own freeze; reconstructs save-time state
					return;
				}
			}
			draw_browser(sel, top, cur_id);
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		// ── DELETE (Triangle) ── existing only; confirm; stay in the browser.
		if (pressed & PSP_CTRL_TRIANGLE) {
			if (sel >= g_show_newsave && g_row_count > 0 && confirm("Delete this save?", msg_box_y(sel, top))) {
				int L;
				sprintf(path, "%s/%s", dir, g_rows[sel - g_show_newsave].name);
				sceIoRemove(path);                       // .bin
				L = (int)strlen(path);
				memcpy(path + L - 4, ".thb", 5); sceIoRemove(path);   // thumbnail
				memcpy(path + L - 4, ".scr", 5); sceIoRemove(path);   // full screenshot
				enumerate_saves(dir);
				total = g_row_count + g_show_newsave;
				if (sel >= total) sel = total - 1;
				if (sel < 0) sel = 0;
				top = 0;
			}
			draw_browser(sel, top, cur_id);
			kpeek(&pad); prev = pad.Buttons;
			continue;
		}

		if (sel < top) top = sel;
		if (sel >= top + BR_VISIBLE) top = sel - BR_VISIBLE + 1;
		if (sel != osel || top != otop)
			draw_list(sel, top);           // redraw list only on change (no flicker)
	}
}

// ────────────────────────────────────────────────────────────
// Menu thread — sleeps until a short NOTE tap wakes it (sceKernelWakeupThread in
// PspLsLibraryLauncher). No polling, no per-frame draw; 48KB stack (vs the old
// 128KB) — must clear fastlz's 32KB stack-resident hash table + FreezeSave's ~4KB
// frame.
// ────────────────────────────────────────────────────────────
int menu_thread(SceSize args, void *argp)
{
	int oc_confirm_needed;
	(void)args; (void)argp;
	dbg_init();
	load_settings();   // restore settings (defaults if no file or another generation's magic)
	oc_init();          // safe stock baseline now; a persisted non-stock step (if any) is
	                    // confirmed + applied on first wake below, once the game's display
	                    // is actually up (see boot_frozen_prompts — nothing is on screen yet here)
	install_fps_overlay_hook();   // safe to install unconditionally — the hook itself
	                               // checks g_show_fps_overlay/g_menu_open before drawing
	oc_confirm_needed = (g_overclock_id > 0 && g_overclock_id < OC_STEPS);
	g_autoload_armed = 1;   // check the per-game auto-open flag on the first controller read
	WriteDebugLog("[MENU] thread started (sleeping until NOTE tap).");

	while (1) {
		sceKernelSleepThread();          // woken by the controller hook (NOTE tap OR boot auto-open)

		// Boot auto-open: the hook only SIGNALLED us (no game-thread MS). Do the per-game MS
		// check HERE (menu thread = safe). If enabled AND a save exists, open on the newest save.
		// The game may still be streaming hard at startup so the freeze can abort (g_browser_opened
		// stays 0) -> retry a few times as it settles. Consume g_autoopen_launch no matter what so
		// a later MANUAL open is never mistaken for the boot auto-open (the leak that showed the
		// Loading-only view on the first manual open).
		if (g_autoopen_pending) {
			g_autoopen_pending = 0;
			// Running game's per-game frame cap — read once at boot, HERE on the menu
			// thread (safe for sceIo), so the limiter applies from the game's first
			// frame rather than only after the settings screen is opened.
			game_frame_limit_load();
			if (DBG_UART()) { char b[64]; sprintf(b, "[FLIMIT] boot: %d fps (0=off)", g_frame_limit); uart_puts(b); }
			// Per-game gamma too: read here (menu thread = MS safe, umdid known),
			// so the pass starts with THIS game's value from the first frame — the
			// earlier menu_thread read ran before umdid existed and always defaulted.
			st_load_game_gamma();
			if (DBG_UART()) { char b[48]; sprintf(b, "[ST] gam boot: %d.%02d", g_st_gamma / 100, g_st_gamma % 100); uart_puts(b); }
			// Per-game FPS unlock: stream the DB for this game's FPS cheats + the saved
			// selection (MS-safe here), and start the apply thread if one is active.
			cheats_load_for_game();
			st_prealloc();   // reserve the GE buffers now (Colin McRae: mid-game enable found the heap full)
			if (st_active()) st_ensure_started();   // a non-neutral loaded gamma starts the pass
			// Same deal for the per-game Intro Video Skip: read here (menu thread = MS
			// safe), then hand the 30s window to its own thread so it can poll for the
			// game to load its psmf module without holding up the auto-open below.
			game_video_skip_load();
			if (DBG_UART()) {
				char b[64];
				if (g_video_skip == VSKIP_CAPTURE)    sprintf(b, "[VSKIP] boot: capture");
				else if (g_video_skip == VSKIP_TIMED) sprintf(b, "[VSKIP] boot: timed %dms", g_video_skip_ms);
				else                                  sprintf(b, "[VSKIP] boot: OFF");
				uart_puts(b);
			}
			// One frozen sequence for BOTH the overclock-confirm prompt and the CAPTURE
			// arm gate (Hold RIGHT for 1s) — see boot_frozen_prompts. Video Skip is loaded
			// just above, so CAPTURE is known here; the game stays frozen from the OC prompt
			// straight through the arm, then resumes ONCE with Right held for the capture.
			{
				int oc_pending = oc_confirm_needed && g_overclock_id > 0 && g_overclock_id < OC_STEPS;
				// STABLE step: user vouched for it -> skip the confirm prompt and apply directly
				// (oc_apply is just PLL register writes, no freeze needed). Non-stable pending ->
				// prompt inside the frozen sequence as before.
				int do_oc  = oc_pending && !g_overclock_stable;
				int do_arm = (g_video_skip == VSKIP_CAPTURE);
				oc_confirm_needed = 0;   // one-shot regardless
				if (oc_pending && g_overclock_stable) {
					oc_apply(g_overclock_id);
					if (DBG_UART()) { char b[56]; sprintf(b, "[OC] stable: applied step %d, no confirm", g_overclock_id); uart_puts(b); }
				}
				if (do_oc || do_arm) {
					g_menu_open = 1;
					if (g_set_home_popup) g_set_home_popup(0);
					boot_frozen_prompts(do_oc, do_arm);
					if (g_set_home_popup) g_set_home_popup(1);
					g_menu_open = 0;
				}
			}
			if (g_video_skip != VSKIP_OFF) {
				SceUID vthid = sceKernelCreateThread("pspstates_vskip", video_skip_thread,
				                                     0x20, 0x1000, 0, NULL);
				if (vthid >= 0) sceKernelStartThread(vthid, 0, NULL);
				else if (DBG_UART()) uart_puts("[VSKIP] FAILED to create the watcher thread");
			}
			if (game_autoopen_enabled() && game_has_save()) {
				int tries;
				WriteDebugLog("[MENU] boot auto-open (enabled + save present)");
				g_autoopen_launch = 1;
				for (tries = 0; tries < 40; tries++) {   // up to ~6s while the game settles
					g_menu_open = 1;
					if (g_set_home_popup) g_set_home_popup(0);
					run_save_browser();
					if (g_set_home_popup) g_set_home_popup(1);
					g_menu_open = 0;
					if (g_browser_opened) break;          // it opened (and closed) -> done
					sceKernelDelayThread(150000);          // freeze aborted (game busy) -> wait, retry
				}
				g_autoopen_launch = 0;                    // one-shot: consume even if it never opened
			} else {
				WriteDebugLog("[MENU] boot auto-open skipped (disabled or no save)");
			}
			continue;                                     // back to sleep for the next NOTE tap
		}

		g_menu_open = 1;                 // NOTE-tap open (the hook also set this; harmless)
		if (g_set_home_popup) g_set_home_popup(0);   // block HOME while the browser is open
		run_save_browser();
		if (g_set_home_popup) g_set_home_popup(1);   // re-enable HOME on close
		g_menu_open = 0;                 // NOTE already released; just re-arm
		// Final [STX] step. The worker's stats line reads all-zero while
		// g_menu_open is 1 (the poll block is skipped and the game is frozen, so
		// no GE submits reach the hook), which is how a stall before this point
		// shows up in the log even without a crash.
		if (DBG_UART()) uart_puts("[STX] exit: menu closed, g_menu_open=0");
	}

	return 0;
}
