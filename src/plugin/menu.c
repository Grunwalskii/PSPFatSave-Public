#include "pspfatsave.h"
#include "gfx.h"
#include "debug.h"
#include "overclock.h"
#include "sysstats.h"
#include "videoskip.h"
#include "menu.h"
#include "fatsave.h"

// Set when the menu is opened by the boot auto-open (vs a NOTE tap): the browser then starts
// on the newest save (for a quick load) regardless of the Default Slot setting. One-shot.
static int g_autoopen_launch = 0;
// Set by the pad hook (GAME thread) to ask the MENU thread to evaluate boot auto-open. The
// hook must NOT touch the Memory Stick (a game-thread sceIo* at boot contends with the game's
// own startup streaming, so a game thread ends up mid-msstor-read when the menu suspends it ->
// the frozen enumerate deadlocks on the FATMS lock, MS LED flashing). The menu thread does the
// MS check on its own (safe) thread.
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

// Load the RUNNING game's Frame Limit from its per-game settings file into
// Load g_frame_limit from game config (must be called from MENU thread, not game thread).
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

	// Boot auto-open (once): on the first controller read (display + game threads are up,
	// umdid known) just SIGNAL the menu thread. Do NOT read the MS here — a game-thread
	// sceIo* at boot contends with the game's startup streaming and reintroduces the
	// frozen-enumerate FATMS deadlock (MS LED flashing). The menu thread does the per-game
	// check + opens on its own (safe) thread, and retries as the game settles.
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
static void detect_note_tap(void)
{
	SceCtrlData kpad;
	// Whole body under k1=0 (not just the peek): the launcher runs kernel calls
	// with kernel-stack pointers from the GAME's syscall context. kpeek's own
	// k1 save/restore nests harmlessly inside this bracket.
	int k1 = pspSdkSetK1(0);
	if (kpeek(&kpad) > 0)
		PspLsLibraryLauncher(&kpad);
	pspSdkSetK1(k1);
}

int sceCtrlPeekBufferPositivePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_peek ? g_real_ctrl_peek(pad_data, count)
	                           : sceCtrlPeekBufferPositive(pad_data, count);
	vskip_inject_buttons(pad_data, count, res, 0);
	detect_note_tap();
	return res;
}

// THIRD button leak fix (buffer-backlog games, e.g. Tomb Raider Legends), grounded
// in PSP_References/uofw-master/src/kd/ctrl/ctrl.c's _sceCtrlReadBuf: mode is a
// 2-bit field (bit1 = Read(2/3) vs Peek(0/1), bit0 = Negative(1/3) vs Positive(0/2)).
// ONLY a "Read" call (mode & READ_BUFFER_POSITIVE, true for BOTH mode 2 and mode 3 —
// Positive and Negative alike) advances a persistent per-caller-mode "unread" ring
// cursor (firstUnReadUpdatedBufIndex) into a 64-slot hardware-sampled ring
// (CTRL_NUM_INTERNAL_CONTROLLER_BUFFERS); a "Peek" call never does — it always
// returns just the latest sample. That ring is filled by a kernel timer/interrupt
// which suspend_escalating() does NOT freeze (only the game's own user threads are
// suspended; kernel threads/dispatcher/interrupts stay on), so real hardware button
// presses made WHILE the menu is open (X to save, O to close) keep landing in the
// game's own user-mode ring the whole time, utterly unrelated to what our
// kernel-mode wait_buttons_up()/kpeek() polls (that touches g_ctrl.kernelModeData,
// a SEPARATE buffer from the game's g_ctrl.userModeData — selected by
// pspK1IsUserMode() at call time). A Read-based game's first several post-resume
// Read calls drain that backlog ONE ENTRY AT A TIME (not all at once), replaying
// the menu's own presses as delayed "live" input — this is what neither
// wait_buttons_up() (drains only the CURRENT peek state) nor g_suppress_latch
// (drains a same-instant OR-accumulator, unrelated buffer) ever covered.
// Fix: blank (force-zero Positive / force-all-1s Negative — see the mode&PEEK_BUFFER_
// NEGATIVE bit-inversion in _sceCtrlReadBuf) only the ring slots that were SAMPLED DURING
// THE MENU, identified by their timestamp. Every controller sample carries the
// sceKernelGetSystemTimeLow() it was captured at (uofw ctrl.c:1649/2136); g_suppress_posbuf_ts
// is that clock captured at resume (arm time). A slot stamped at/before the arm is stale menu
// input -> blank it; the FIRST slot stamped AFTER the arm is genuine post-resume input, so it
// (and everything after) passes through and suppression ENDS immediately.
//
// This replaces a flat "blank the next N slots" count, which also ate REAL presses: a game
// that drains the ring slowly (GTA reads few slots/sec) stretched 80 blanked slots into ~5s
// of dead X after every menu close/save. The timestamp gate blanks exactly the menu backlog
// and not one fresh press. g_suppress_posbuf_calls stays as an UPPER BOUND only (safety, in
// case a game never presents a fresh-stamped slot). Armed via arm_input_suppress().
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
	vskip_inject_buttons(pad_data, count, res, 0);    // AFTER the suppress, or it would wipe the injected press
	detect_note_tap();
	return res;
}

// Peek never carries backlog (see the block comment above) — pure passthrough,
// kept only so NOTE-tap detection still fires for a game that reads this way.
int sceCtrlPeekBufferNegativePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_peek_neg ? g_real_ctrl_peek_neg(pad_data, count)
	                               : sceCtrlPeekBufferNegative(pad_data, count);
	vskip_inject_buttons(pad_data, count, res, 1);
	detect_note_tap();
	return res;
}

// Same backlog-cursor issue as ReadBufferPositive (see the block comment above —
// the mode bit that gates it doesn't distinguish Positive from Negative), so it
// needs the identical drain, just with the negative-logic "clean" value (all 1s).
int sceCtrlReadBufferNegativePatched(SceCtrlData *pad_data, int count)
{
	int res = g_real_ctrl_read_neg ? g_real_ctrl_read_neg(pad_data, count)
	                               : sceCtrlReadBufferNegative(pad_data, count);
	suppress_posbuf_slots(pad_data, count, res, 0xFFFFFFFFu);   // negative logic: all-1s = nothing pressed
	vskip_inject_buttons(pad_data, count, res, 1);              // AFTER the suppress, or it would wipe the injected press
	detect_note_tap();
	return res;
}

// SECOND button leak fix (latch-reading games, e.g. Ratchet & Clank). The game
// calls these via syscall, so the real sceCtrlReadLatch below runs in the GAME's
// USER-mode k1 and drains g_ctrl.userModeData — the latch the game actually reads
// (our kernel-mode drain in wait_buttons_up hit the KERNEL latch; uofw ctrl.c
// selects user/kernel by pspK1IsUserMode). While g_suppress_latch is set (armed by
// arm_input_suppress() at every menu->game resume point), zero the returned edges
// so the game never sees the menu's stale "X make". Read drains normally (that's
// what Read does); Peek is made to drain too INSIDE the window so a peek-only game
// doesn't keep seeing the stale edge after the window clears. SELF-CLEARING: unlike
// the buffer backlog above (which drains gradually, one ring slot per game call),
// the latch accumulator is a same-instant OR of make/break/press/release bits with
// no depth — ONE drained read (ours or the game's) empties it completely, so the
// very first hit while armed clears g_suppress_latch back to 0 itself. This lets
// every arm_input_suppress() call site skip a matching "clear" step (harmless if a
// game never calls these at all — the flag just sits at 1, inert, forever).
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
// Forward decl: save_settings() is defined further below (global settings
// persistence), after this function — needed here so declining a non-stock
// step can write the reset back to settings.cfg immediately.
static void save_settings(void);

// Boot-time frozen prompt sequence, run on the menu thread at first wake. Freezes the game
// ONCE (same FAST dispatch-off gate as run_save_browser, no MS-lock probe) and, while frozen,
// shows any of: the overclock-confirm prompt, then the CAPTURE arm gate — then resumes ONCE.
// A single continuous freeze is deliberate: the game never advances a frame between the two
// prompts, so the intro stays exactly where it was until the user arms the skip.
//   do_oc  : a non-stock overclock step is persisted -> ask X/O before applying it (so a step
//            that hung last session can be declined without editing settings.cfg by hand).
//            Declining resets g_overclock_id to 0 and persists it (won't ask again next boot).
//   do_arm : Video Skip = CAPTURE -> show "Hold RIGHT until Intro skipped" and wait for the
//            user to hold D-pad Right for 1s. The game then resumes with Right ALREADY held,
//            so the capture (started right after) fires from the intro's first frame under
//            the user's control. This replaces the old forced-5s window, which on a psmf-patch
//            game (Pirates) skipped the intro before the banner was even readable.
//
// MUST freeze first: the game keeps rendering its own frames, which instantly overwrites
// anything drawn straight to the framebuffer. pin_current_display() reasserts the live buffer
// to the display controller so the draw doesn't land off-screen (the FreezeSave/FreezeLoad
// Pirates/Tomb Raider "invisible banner" bug); dbg_init() then grabs that consistent buffer.
// Defined further below (with the save browser) — same MS-FAT-lock hazard applies here.
int  ms_probe_after_freeze(void);
void ms_probe_reap(void);

void boot_frozen_prompts(int do_oc, int do_arm)
{
	int attempt, frozen = 0;

	if (!do_oc && !do_arm) return;

	// Same freeze + MS-lock probe the save browser uses. At BOOT the game is streaming its
	// ISO from the Memory Stick, so a bare freeze can catch a game thread INSIDE the FAT
	// driver holding its lock: the MS LED then blinks the whole time we're frozen, and any
	// MS I/O we do (the O/decline path's save_settings) blocks on that lock forever. The
	// probe (async open — the IO thread blocks on the lock, not us) confirms the lock is
	// FREE before we proceed; if a frozen thread holds it, resume so it finishes and retry.
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
// [4]=UART logging, [5]=FPS overlay mode (0/1/2/3s), [6]=FPS 1% Low toggle,
// [7]=Battery overlay mode (0=Off 1=Percent 2=Percent+Time 3=ALL),
// [8]=CPU & GPU Usage toggle (0=Off 1=On), [9]=Frametime Chart toggle (0/1).
// Per-game settings (Auto-Open, Intro Video Skip, Frame Limit, Save Compression)
// stored in gameset.cfg. Older settings.cfg files load correctly (size-tolerant).
#define SETTINGS_PATH  "ms0:/seplugins/SAVESTATE/settings.cfg"
#define SETTINGS_MAGIC 0x53455441u   // "SETA"

void load_settings(void)
{
	SceUID fd; u32 buf[11]; int n;
	fd = sceIoOpen(SETTINGS_PATH, PSP_O_RDONLY, 0);
	if (fd < 0) return;                          // no file -> keep defaults
	n = sceIoRead(fd, buf, sizeof(buf));
	if (n >= (int)(5 * sizeof(u32)) && buf[0] == SETTINGS_MAGIC) {
		g_show_debug   = (int)buf[1];            // debug routing 0..3 (see g_show_debug)
		if (g_show_debug < 0 || g_show_debug > 3) g_show_debug = 0;
		g_default_slot = buf[2] ? 1 : 0;
		g_stage_spot   = 1;                      // always Mid: the snapshot stages in the safe middle 8MB
		// buf[3] was the unused "Apply blob spot" setting (removed; g_apply_base is now fixed
		// at 0x89800000) — reused for the overclock step so the 5-word format stays stable.
		g_overclock_id = (int)buf[3];
		if (g_overclock_id < 0 || g_overclock_id >= OC_STEPS) g_overclock_id = 0;
		g_uart_log     = buf[4] ? 1 : 0;
		// buf[5]/buf[6]/buf[7]/buf[8]/buf[9] are NEWER — an older file won't have
		// some or all of them; default off rather than rejecting the whole read.
		g_show_fps_overlay = (n >= (int)(6 * sizeof(u32))) ? (int)buf[5] : 0;
		if (g_show_fps_overlay < 0 || g_show_fps_overlay > 3) g_show_fps_overlay = 0;
		g_fps_show_lows = (n >= (int)(7 * sizeof(u32))) ? (buf[6] ? 1 : 0) : 0;
		g_show_battery  = (n >= (int)(8 * sizeof(u32))) ? (int)buf[7] : 0;
		if (g_show_battery < 0 || g_show_battery > 3) g_show_battery = 0;
		g_show_cpu_usage = (n >= (int)(9 * sizeof(u32))) ? (buf[8] ? 1 : 0) : 0;
		g_show_ft_chart  = (n >= (int)(10 * sizeof(u32))) ? (buf[9] ? 1 : 0) : 0;
		g_overclock_stable = (n >= (int)(11 * sizeof(u32))) ? (buf[10] ? 1 : 0) : 0;
	}
	sceIoClose(fd);
}

static void save_settings(void)
{
	SceUID fd; u32 buf[11];
	buf[0] = SETTINGS_MAGIC; buf[1] = (u32)g_show_debug;
	buf[2] = (u32)g_default_slot; buf[3] = (u32)g_overclock_id;
	buf[4] = (u32)g_uart_log; buf[5] = (u32)g_show_fps_overlay;
	buf[6] = (u32)g_fps_show_lows; buf[7] = (u32)g_show_battery;
	buf[8] = (u32)g_show_cpu_usage; buf[9] = (u32)g_show_ft_chart;
	buf[10] = (u32)g_overclock_stable;
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

// Free KERNEL partition (mpid 1) RAM, in KB — the partition this kernel PRX
// actually lives in, so it reflects the plugin's own footprint pressure.
// sceKernelTotalFreeMemSize() is the USER partition (== sceKernelPartitionTotalFreeMemSize(2),
// uofw partition.c) = the game's spare RAM, which is game-dependent and does NOT
// change with the plugin's own size — the wrong number for this. Resolve the
// kernel export at runtime (SysMemForKernel 0x0115B0F8, same lookup me_rpc_probe
// uses); fall back to the user-partition call if it can't be resolved. Shared by
// the Settings-footer readout (ram_usage_kb) and the low-RAM save/load guard
// (run_save_browser) so both agree on the exact same number.
//
// LOW_RAM_SAVE_LOAD_KB: below this much free kernel RAM, Save/Load are refused
// outright instead of attempted - observed (user report) that free RAM around
// ~35KB caused a crash during save/load (the freeze + compress/decompress work
// needs real headroom). 50KB is a safety margin above that observed failure
// point, not a value derived from a measured hard minimum - revisit if a crash
// is ever seen above it, or if legitimate saves ever get refused below it.
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
	               + (g_battery_poll_started ? BATTERY_POLL_STACK_BYTES : 0) + 1023) / 1024;
	*free_kb = free_kernel_ram_kb();
}

// Draw the settings screen. sel = highlighted row (GLOBAL section first, then the
// per-game section): 1 Default Slot, 2 Show FPS, 3 FPS 1% Lows, 4 Frametime Chart,
// 5 CPU Usage, 6 Battery Status, 7 Overclock (all global) | 8 Debug Messages (debug
// builds only; merged with UART Log) | 9 Intro Video Skip, 10 Auto-Open, 11 Frame Limit
// (all per-game, gameset.cfg). Debug Messages sits alone above the "Game specific
// Settings" header.
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
// into a fixed-width field (%-20s) so every row's "<" starts at the same
// column regardless of label length — previously each row hand-counted its
// own padding spaces, and they'd drifted out of sync with each other (values
// visibly staggered horizontally row to row) whenever a label changed length.
// 20 chars comfortably fits the longest label here ("Auto-Open on Boot:").
static void settings_line(char *line, const char *label, const char *value)
{
	sprintf(line, "%-20s< %s >", label, value);
}

void draw_settings(int sel, const char *gid)
{
	// val must hold the LONGEST value string any row renders — currently Intro Video
	// Skip's "ON - Time capture next boot" (27 bytes with the NUL). It was 24 and that
	// sprintf ran off the end into line[]/header[], garbling the whole screen (v667).
	char line[64], val[40], header[80];
	const char *t  = "Settings";
	const char *gt = "Game specific Settings";
	// Rows 2 apart vertically; per-game section header keeps a 3-row gap on
	// either side for visual separation from the global settings above it.
	// Shifted down by 1 row to give the Settings banner more space at the top.
	int r1 = 6, r2 = 8, r3 = 10, r4 = 12, r5 = 14, r6 = 16, r7 = 18, r8 = 20, gr = 23, r9 = 25, r10 = 27, r11 = 29;

	format_game_header(header, gid);
	draw_screen_chrome(header, "Up/Dn sel  L/R change  Tri/X stable/time  O Return");
	dbg_text((60 - (int)strlen(t)) / 2, 3, BR_CYAN, BR_BG, t);

	// Row 1: Default Slot (global).
	settings_line(line, "Default Slot:", g_default_slot ? "Last" : "New");
	draw_settings_row(r1, sel == 1, line);

	// Row 2: Show FPS (GLOBAL, both builds) — live counter drawn during actual
	// gameplay (menu closed) via the sceDisplaySetFrameBuf/vblank hooks and
	// fps_poll_thread. Off / On 1s / On 0.5s / On 0.2s — the number is how often
	// fps_tick() recomputes the displayed value (fps_window_us()). See
	// install_fps_overlay_hook / fps_display_set_frame_buf_patched.
	{
		static const char *fps_name[4] = { "OFF", "On 1s", "On 0.5s", "On 0.2s" };
		settings_line(line, "Show FPS:", fps_name[(g_show_fps_overlay >= 0 && g_show_fps_overlay <= 3) ? g_show_fps_overlay : 0]);
	}
	draw_settings_row(r2, sel == 2, line);

	// Row 3: FPS 1% Lows (GLOBAL, both builds) — also draws the average FPS of the
	// slowest 1% of recent frames (a stutter/worst-case metric) alongside the main
	// FPS number. Only has any visible effect while Show FPS is also On. See
	// fps_calc_1pct_low / g_fps_show_lows.
	settings_line(line, "FPS 1% Lows:", g_fps_show_lows ? "On" : "OFF");
	draw_settings_row(r3, sel == 3, line);

	// Row 4: Frametime Chart (GLOBAL, both builds) — full-width scrolling
	// frametime histogram, one column per frame; see ft_chart_tick/ft_chart_draw.
	// Independent of Show FPS — ticks and draws even if that's off.
	settings_line(line, "Frametime Chart:", g_show_ft_chart ? "On" : "OFF");
	draw_settings_row(r4, sel == 4, line);

	// Row 5: CPU & GPU Usage (GLOBAL, both builds) — CPU via idle-clocks,
	// GPU via GE busy-duty-cycle sampling. One toggle drives both.
	settings_line(line, "CPU & GPU Usage:", g_show_cpu_usage ? "On" : "OFF");
	draw_settings_row(r5, sel == 5, line);

	// Row 6: Battery Status (GLOBAL, both builds) — live overlay drawn by the
	// same poll thread as the FPS counter. Off / Percent / Percent+Time / ALL
	// (the extra telemetry tier — see the block comment above battery_refresh).
	{
		static const char *batt_name[4] = { "OFF", "Percent", "Percent+Time", "ALL" };
		settings_line(line, "Battery Status:", batt_name[(g_show_battery >= 0 && g_show_battery <= 3) ? g_show_battery : 0]);
	}
	draw_settings_row(r6, sel == 6, line);

	// Row 7: Overclock (GLOBAL, both builds — raw PLL registers, PSP-1000 only; see
	// the oc_* block above load_settings). 0 = stock 333MHz.
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

	// Row 8: Debug Messages (GLOBAL, debug builds only) — merged with UART Log.
	// Display modes: 0=OFF, 1=Log MS, 2=Log Screen, 3=Screen and MS, 4=UART.
	// Internal: 0-3 map to g_show_debug, 4 = (g_uart_log=1, g_show_debug=0).
	// (On release builds, this is compiled out.)
#if DEBUG_BUILD
	{
		static const char *dbg_modes[5] = { "OFF", "Log MS", "Log Screen", "Screen and MS", "UART" };
		int mode = (g_uart_log && g_show_debug == 0) ? 4 : (g_show_debug >= 0 && g_show_debug <= 3) ? g_show_debug : 0;
		settings_line(line, "Debug Messages:", dbg_modes[mode]);
	}
	draw_settings_row(r8, sel == 8, line);
#endif

	dbg_text((60 - (int)strlen(gt)) / 2, gr, BR_CYAN, BR_BG, gt);   // per-game section header

	// Row 9: Intro Video Skip (PER-GAME, gameset.cfg) — see the video_skip_* block.
	// Three states: OFF / learn the window on the next boot / fire the learned window.
	if (g_video_skip == VSKIP_CAPTURE)    sprintf(val, "ON - Time capture next boot");
	else if (g_video_skip == VSKIP_TIMED) sprintf(val, "ON - %d.%ds", g_video_skip_ms / 1000,
	                                              (g_video_skip_ms % 1000) / 100);
	else                                  sprintf(val, "OFF");
	settings_line(line, "Intro Video Skip:", val);
	draw_settings_row(r9, sel == 9, line);

	// Row 10: Auto-Open on Boot (PER-GAME, gameset.cfg).
	settings_line(line, "Auto-Open on Boot:", g_autoload ? "Yes" : "OFF");
	draw_settings_row(r10, sel == 10, line);

	// Row 11: Frame Limit (PER-GAME, gameset.cfg) — OFF or a 25..60 FPS cap,
	// enforced by frame_limit_wait in the present hook. See g_frame_limit.
	if (g_frame_limit > 0) sprintf(val, "%d FPS", g_frame_limit);
	else                   sprintf(val, "OFF");
	settings_line(line, "Frame Limit:", val);
	draw_settings_row(r11, sel == 11, line);

	// RAM usage readout (three figures, bottom of the screen above the help line).
	{
		u32 s_kb, d_kb, f_kb; char rl[64];
		ram_usage_kb(&s_kb, &d_kb, &f_kb);
		// Compact to fit 60 cols even with a multi-MB Free value.
		sprintf(rl, "RAM:  Plugin static %uK  dynamic %uK  Free %uK",
		        (unsigned)s_kb, (unsigned)d_kb, (unsigned)f_kb);
		dbg_text((60 - (int)strlen(rl)) / 2, 31, BR_GREY, BR_BG, rl);   // row 31: one-row gap above the footer (was 32, touching it)
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
	if (hold <= 1) return 1;          // initial press
	if (hold < 8)  return 0;          // ~320ms delay before auto-repeat kicks in
	{
		int period = (hold >= 20) ? 5 : 6;
		return ((hold - 8) % period) == 0;
	}
}

static void run_settings_menu(const char *gid)
{
	SceCtrlData pad;
	int fl_hold = 0, fl_hold_dir = 0;   // Frame-Limit hold state (auto-repeat)
	// Rows: 1 Default Slot, 2 Show FPS, 3 FPS 1% Lows, 4 Frametime Chart, 5 CPU Usage,
	// 6 Battery Status, 7 Overclock | 8 Debug Messages (debug builds only) | 9 Video Skip,
	// 10 Auto-Open, 11 Frame Limit. Row 8 is debug-only; a release starts at 1 = Default Slot
	// (draw_settings doesn't draw row 8 there). Rows 9-11 are the per-game section (gameset.cfg).
	const int sel_min = DEBUG_BUILD ? 1 : 1;
	int sel = sel_min, prev, changed = 0, gchanged = 0;
	load_game_settings(gid);   // per-game settings for gid -> g_autoload, g_frame_limit, g_video_skip
	draw_settings(sel, gid);
	kpeek(&pad); prev = pad.Buttons;
	for (;;) {
		int pressed, osel = sel;
		sceKernelDelayThread(40000);
		kpeek(&pad);
		pressed = pad.Buttons & ~prev;
		prev = pad.Buttons;

		// Contiguous rows sel_min..11. Up/Down clamp to that range.
		if (pressed & PSP_CTRL_UP)   { if (sel > sel_min) sel--; }
		if (pressed & PSP_CTRL_DOWN) { if (sel < 11)      sel++; }

		// Frame Limit (PER-GAME, row 11): auto-repeat on HOLD (per user request), and the
		// value LOOPS (OFF -> 20 -> ... -> 60 -> OFF). Level-driven, not edge-driven, so it
		// sits before the edge-based chain below. Left/Right sweep OFF / 20 / 21 / ... / 60.
		// Applies live — frame_limit_ge reads g_frame_limit every GE submit. 60, 30 and 20
		// are the whole-vblank divisors of the 59.94Hz panel; the in-between steps judder a
		// little but were tried by hand and are for real use (see frame_limit_target_us).
		if (sel == 11) {
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
				fl_hold = 0; fl_hold_dir = 0;
			}
		}

		// Triangle / X are the "up / down" pair for two rows (Left/Right stays "change value"
		// elsewhere; X no longer toggles plain settings — that role was removed per request):
		//   Overclock (row 7, non-stock only): Triangle = mark STABLE (skip the boot confirm,
		//     shown RED), X = clear it back to the normal white "ask every boot".
		//   Intro Video Skip (row 9, only once a timer is captured = TIMED): Triangle = +0.1s,
		//     X = -0.1s on the learned window (clamped to 0.1s .. VSKIP_LEARN_MAX_MS).
		if (sel == 7 && g_overclock_id > 0 && (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS))) {
			if (pressed & PSP_CTRL_TRIANGLE) g_overclock_stable = 1;
			if (pressed & PSP_CTRL_CROSS)    g_overclock_stable = 0;
			changed = 1;
			draw_settings(sel, gid);
		}
		if (sel == 9 && g_video_skip == VSKIP_TIMED && (pressed & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS))) {
			if (pressed & PSP_CTRL_TRIANGLE) g_video_skip_ms += 100;
			if (pressed & PSP_CTRL_CROSS)    g_video_skip_ms -= 100;
			if (g_video_skip_ms < 100)                 g_video_skip_ms = 100;
			if (g_video_skip_ms > VSKIP_LEARN_MAX_MS)  g_video_skip_ms = VSKIP_LEARN_MAX_MS;
			if (DBG_UART()) { char b[48]; sprintf(b, "[VSKIP] window set to %d.%03ds", g_video_skip_ms / 1000, g_video_skip_ms % 1000); uart_puts(b); }
			gchanged = 1;
			draw_settings(sel, gid);
		}

		if (sel == 2 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Show FPS: Left/Right cycle Off / On 1s / On 0.5s / On 0.2s (0..3), wrapping.
			if (pressed & PSP_CTRL_LEFT)  g_show_fps_overlay = (g_show_fps_overlay > 0) ? g_show_fps_overlay - 1 : 3;
			if (pressed & PSP_CTRL_RIGHT) g_show_fps_overlay = (g_show_fps_overlay < 3) ? g_show_fps_overlay + 1 : 0;
			if (g_show_fps_overlay) fps_poll_ensure_started();
			changed = 1;
			draw_settings(sel, gid);
		} else if (sel == 6 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Battery Status: Left/Right cycle Off / Percent / Percent+Time / ALL (0..3), wrapping.
			if (pressed & PSP_CTRL_LEFT)  g_show_battery = (g_show_battery > 0) ? g_show_battery - 1 : 3;
			if (pressed & PSP_CTRL_RIGHT) g_show_battery = (g_show_battery < 3) ? g_show_battery + 1 : 0;
			if (g_show_battery) {   // lazy: first time it's turned on this session
				fps_poll_ensure_started();
				battery_poll_ensure_started();
			}
			changed = 1;
			draw_settings(sel, gid);
		} else if (sel == 7 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// Overclock: Left/Right step the multiplier table (0 = stock 333MHz). Applied
			// immediately, live — see oc_apply. Deliberately does NOT wrap (unlike the other
			// settings): wrapping would let LEFT at stock jump straight to the top step and
			// apply max clock live in one press, which is real hardware stress on a Phat.
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
		} else if (sel != 2 && sel != 6 && sel != 7 && sel != 8 && sel != 11 && (pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
			// X REMOVED from this chain (per request): plain rows now change only on Left/Right,
			// leaving X free as the "down/disable/decrease" key on the Overclock + Video Skip rows.
			if      (sel == 1) { g_default_slot     = !g_default_slot;     changed  = 1; }   // global
			else if (sel == 3) { g_fps_show_lows    = !g_fps_show_lows;    changed  = 1; }   // global
			else if (sel == 4) {                                                             // global
				g_show_ft_chart = !g_show_ft_chart;
				if (g_show_ft_chart) fps_poll_ensure_started();
				changed = 1;
			}
			else if (sel == 5) {                                                             // global
				g_show_cpu_usage = !g_show_cpu_usage;
				if (g_show_cpu_usage) fps_poll_ensure_started();
				changed = 1;
			}
			else if (sel == 9)  {
				// Intro Video Skip: Cycles OFF -> capture -> (learned, only once one exists) -> OFF.
				// Takes effect on the game's NEXT boot (the watcher is started once from menu_thread
				// at startup, so toggling here cannot affect this session).
				if      (g_video_skip == VSKIP_OFF)     g_video_skip = VSKIP_CAPTURE;
				else if (g_video_skip == VSKIP_CAPTURE) g_video_skip = (g_video_skip_ms > 0) ? VSKIP_TIMED : VSKIP_OFF;
				else                                    g_video_skip = VSKIP_OFF;
				gchanged = 1;
			}
			else if (sel == 10) { g_autoload         = !g_autoload;         gchanged = 1; }   // per-game
			draw_settings(sel, gid);
		} else if (sel != osel) {
			draw_settings(sel, gid);
		}
		// O or L-trigger -> back to the save folder (L continues the L|save|R strip).
		if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_LTRIGGER)) break;
	}
	if (changed)  save_settings();            // global settings.cfg
	if (gchanged) save_game_settings(gid);    // per-game gameset.cfg (for gid)
	// This screen loaded gid's per-game values, and gid may be a DIFFERENT game's
	// folder than the one running (the browser can switch games) — so re-sync the
	// live frame cap to the RUNNING game, or we'd pace it with another game's limit.
	game_frame_limit_load();
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
// Running from MS, the game's "UMD" streaming executes the whole inferno ->
// fatms -> msstor chain on the GAME'S OWN thread (PSP io drivers run in the
// caller's context; ARK-4's inferno only creates a one-shot mount thread), so a
// freeze can catch a game thread INSIDE the FAT driver holding its lock — the
// browser's first MS call would then block on that lock forever (the historic
// 1-in-10 frozen menu, MS LED blinking mid-transaction). Instead of predicting
// that from thread wait-states (the old seconds-long stable-wait gate), test the
// hazard directly AFTER freezing: open a file that always exists via
// sceIoOpenAsync — the ASYNC IO THREAD blocks on the FAT lock, not us — and
// poll it briefly. Completed = lock free (frozen threads can't take it anymore),
// safe to browse. Still pending = a frozen thread holds it -> caller unfreezes

// The save browser: soft-freeze, enumerate, draw, navigate, and act (Square=save,
// X=load, Triangle=delete; confirm for existing slots, none for a new save). A
// save or load CLOSES the browser (on load, the system becomes the saved state and
// resumes there — the menu does not reappear). L-trigger -> game/folder list.
void run_save_browser(void)
{
	char dir[80], path[128], cur_id[20];
	int load_only;
	int total, sel, top, prev;
	SceCtrlData pad;

	g_browser_opened = 0;                  // becomes 1 only if the freeze below succeeds and we open
	g_menu_quiet = 1;                      // keep the paced CP checkpoints quiet during the browse
	// Pause-only freeze + MS probe. The menu needs exactly two things: the game
	// paused, and the MS usable from this thread. It does NOT need the save/load
	// paths' quiescent freeze (no firmware suspend happens here), so use the FAST
	// gate (first observed WAITING, or ~0.5s best-effort, then a dispatch-off
	// suspend — safe for READY threads) instead of the old stable-wait gate that
	// randomly cost 3-5s waiting for GTA's threadmain to show a steady wait.
	// The one real hazard — freezing a game thread while it's inside the FAT
	// driver holding its lock (see ms_probe_after_freeze above) — is now checked
	// DIRECTLY after freezing; if the lock is held, unfreeze so the holder
	// finishes (~ms) and retry. Deterministic, typically opens on the first try.
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
			run_settings_menu(cur_id);
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
				// Observed (user report): free kernel RAM dropping to ~35KB caused a
				// crash during save/load - the freeze + compression/decompression work
				// needs real headroom. 50KB is a safety margin above that observed
				// failure point, not a value derived from a known hard requirement.
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
				// Close the menu FIRST: resume so the game redraws a clean frame,
				// then save (FreezeSave re-freezes). Clears g_menu_open before the
				// snapshot so a loaded save isn't stuck "open".
				// Wait for the trigger/confirm X to be RELEASED before resuming — the
				// game runs ~0.8s (the 100ms redraw + FreezeSave's freeze latency) with
				// a still-held X as real game input, and that reaction gets captured
				// INTO the savestate (the "button leak through save": every load replays
				// it). Game still frozen here, so the wait is safe (as the O-close path).
				wait_buttons_up();
				// Suppress+drain BOTH input paths across the ~100ms window the game runs
				// before FreezeSave re-freezes it — latch-reading games (Ratchet & Clank)
				// and buffer-backlog games (Tomb Raider Legends) would otherwise see the
				// menu's stale press and act on it. Force BOTH back to 0 BEFORE FreezeSave
				// so the mode-9 snapshot never captures a nonzero value: a captured
				// g_suppress_latch=1 would restore into a loaded session and mute its latch
				// forever; a captured g_suppress_posbuf_calls>0 would spend its remaining
				// count suppressing unrelated future input right after that load.
				arm_input_suppress();
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
				// disabled — a cross-game load would resume another game's kernel/session
				// over the running one. Same red notice as the disabled Save above; any key
				// dismisses. (Deleting/browsing other games' folders is still allowed.)
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
				// A load resumes the SAVE's own kernel/plugin build. If that build
				// differs from the running one, the resumed plugin (hook, menu
				// thread, layout) can misbehave — warn and require confirmation.
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
	load_settings();   // restore Show-Debug / Default-Slot / Overclock (defaults if no file)
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
	}

	return 0;
}
