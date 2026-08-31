#include "pspfatsave.h"      // umdid, DBG_UART, uart_puts, g_menu_open, g_st_op_hold
#include "cheats.h"
#include "sysstats.h"        // fps_wait_vblank_real (real sceDisplayWaitVblankStart)
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputils.h>        // sceKernel*cacheInvalidateRange
#include <string.h>
#include <stdio.h>

// ── Lightweight per-game FPS unlock (v1) ─────────────────────────────────────
// Streams the CWCheat text DB in our folder, keeps only the running game's
// FPS-named cheats, and re-applies the selected one every vblank.

#define CHEAT_DB_PATH "ms0:/seplugins/SAVESTATE/cheats.db"

#define FPS_MAX_OPTIONS      12     // FPS cheats offered (excl. "Stock")
#define FPS_MAX_LINES        256    // shared code-line pool (8 bytes each = 2KB)
#define FPS_NAME_LEN         40     // full cheat name (kept for persistence match)
#define FPS_LABEL_LEN        24     // short menu label, e.g. "60 FPS Unstable"
#define FPS_MAX_CHEAT_LINES  160    // cap per single cheat

#define GAME_RAM_LO 0x08800000u
#define GAME_RAM_HI 0x0A000000u     // Phat user RAM ceiling (matches screen_tuning.c)

typedef struct { u32 p1, p2; } cline;

int g_fps_opt_count = 0;
int g_fps_db_found = 0;  // 1 = a cheat DB file was OPENED this boot (cheats_load_for_game);
                         // with g_fps_opt_count==0 it picks the menu's empty-state label
int g_fps_active    = 0;

static char  g_fps_name[FPS_MAX_OPTIONS][FPS_NAME_LEN];    // full DB name (persistence key)
static char  g_fps_label[FPS_MAX_OPTIONS][FPS_LABEL_LEN];  // short menu label
static u16   g_fps_off[FPS_MAX_OPTIONS];
static u16   g_fps_cnt[FPS_MAX_OPTIONS];
static cline g_fps_pool[FPS_MAX_LINES];
static int   g_fps_pool_used;

static int   g_fps_thread_started;
static volatile int g_fps_quit;    // set by cheats_stop() at game exit -> thread ends

// Original-bytes backup per write line of the active cheat, captured from the
// live game just before a cheat is first applied; written back on deactivation
// so returning to "Stock" (or switching options) reverts the cheat's patches.
static u32   g_bk_addr[FPS_MAX_CHEAT_LINES];
static u8    g_bk_sz[FPS_MAX_CHEAT_LINES];
static u32   g_bk_val[FPS_MAX_CHEAT_LINES];
static int   g_bk_cnt;      // backed-up entries
static int   g_bk_opt;      // option the backup belongs to (1..count), 0 = none live

static void cheats_restore_backup(void);      // defined after the interpreter below

// ── parse accumulator (used only during cheats_load_for_game, boot/menu thread) ─
static char  pa_mykey[16];      // our disc id (umdid up to '_'), for _S matching
static int   pa_in_game, pa_done;
static int   pa_matched;        // diag: our _S section was ever entered
static int   pa_seen_fps;       // diag: FPS-named cheats encountered (pre support-filter)
static int   pa_cur_fps, pa_cur_unsup, pa_cur_cnt;
static int   pa_cur_real;        // cheat has >=1 non-zero write (not a dummy placeholder)
static char  pa_cur_name[FPS_NAME_LEN];
static cline pa_cur_lines[FPS_MAX_CHEAT_LINES];

// ── small text helpers ───────────────────────────────────────────────────────
static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

// Read one hex word ("0x1234" / "1234"), advancing *ps past it. Stops at the
// first non-hex char (so a trailing "//comment" is tolerated). Returns 1 on ok.
static int rd_hex(const char **ps, u32 *out)
{
	const char *s = skip_ws(*ps);
	u32 v = 0; int any = 0, d;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	while ((d = hexv(*s)) >= 0) { v = (v << 4) | (u32)d; s++; any = 1; }
	if (!any) return 0;
	*out = v; *ps = s; return 1;
}
// Case-insensitive substring test for "fps".
static int name_is_fps(const char *s)
{
	for (; s[0] && s[1] && s[2]; s++)
		if ((s[0]|32) == 'f' && (s[1]|32) == 'p' && (s[2]|32) == 's') return 1;
	return 0;
}
// Decorative section header / separator, e.g. "____[>>>FPS Cheats<<<]____" — has
// "FPS" in the name (and often a dummy code line) but is NOT a real option. Reject
// names with ">>>"/"<<<" or a run of 3+ underscores.
static int name_is_separator(const char *s)
{
	int us = 0;
	for (; *s; s++) {
		if (s[0] == '>' && s[1] == '>' && s[2] == '>') return 1;
		if (s[0] == '<' && s[1] == '<' && s[2] == '<') return 1;
		if (*s == '_') { if (++us >= 3) return 1; } else us = 0;
	}
	return 0;
}
static int is_dig(char c) { return c >= '0' && c <= '9'; }
static int is_word(char c) { return is_dig(c) || ((c|32) >= 'a' && (c|32) <= 'z') || c == '-'; }

// Common filler words skipped when picking a qualifier word, so "-Only In-Game-"
// yields "In-Game" not "Only".
static int is_filler(const char *w, int n)
{
	static const char *F[] = { "only", "the", "a", "to", "at", "of", "for", "run" };
	int i, k;
	for (i = 0; i < (int)(sizeof(F) / sizeof(F[0])); i++) {
		int m = (int)strlen(F[i]);
		if (m != n) continue;
		for (k = 0; k < n && (w[k]|32) == F[i][k]; k++) ;
		if (k == n) return 1;
	}
	return 0;
}

// Short, DISTINCT menu label from a verbose cheat name: the first number token
// ("60", "20/30") + " FPS", then a distinguishing qualifier so near-duplicate
// variants stay apart. The qualifier is the first meaningful word of the name's
// "[...]" bracket or " -...-" dash section (filler words skipped), e.g.
//   "...30 FPS -Testing Phase-"          -> "30 FPS Testing"
//   "...30 FPS -Original Value-"         -> "30 FPS Original"
//   "60 FPS [Unstable]" / "[Stable]"     -> "60 FPS Unstable" / "60 FPS Stable"
//   "20/30 FPS [Fixed gamespeed, ...]"   -> "20/30 FPS Fixed"
//   "60 FPS v2"                          -> "60 FPS v2"
// A name with no number falls back to a trimmed copy (leading "↑[#" junk stripped).
static void make_label(const char *name, char *out, int outsz)
{
	const char *s = name, *q = 0, *qend = 0, *p;
	int j = 0, k;

	while (*s && !is_dig(*s)) s++;
	if (!is_dig(*s)) {   // no number -> trimmed name
		while (*name && !((*name|32) >= 'a' && (*name|32) <= 'z') && !is_dig(*name)) name++;
		for (j = 0; name[j] && j < outsz - 1; j++) out[j] = name[j];
		out[j] = 0;
		if (j == 0) { out[0] = 'F'; out[1] = 'P'; out[2] = 'S'; out[3] = 0; }
		return;
	}
	while (*s && j < outsz - 5 && (is_dig(*s) || (*s == '/' && is_dig(s[1])))) out[j++] = *s++;
	{ const char *suf = " FPS"; for (k = 0; suf[k] && j < outsz - 1; k++) out[j++] = suf[k]; }

	// Locate the qualifier section: "[...]" preferred, else a " -...-" dash run.
	for (p = name; *p; p++) if (*p == '[') { q = p + 1; break; }
	if (q) { for (p = q; *p && *p != ']'; p++) ; qend = p; }
	else {
		for (p = name; p[0]; p++) if (p[0] == ' ' && p[1] == '-') { q = p + 2; break; }
		if (q) { for (p = q; *p && *p != '-'; p++) ; qend = p; }
	}

	if (!q) {   // no bracket/dash: keep a trailing "v<digit>" if present
		for (p = name; p[0]; p++)
			if ((p[0]|32) == 'v' && p[1] >= '0' && p[1] <= '9' && (p == name || p[-1] == ' ')) {
				if (j < outsz - 3) { out[j++] = ' '; out[j++] = 'v'; out[j++] = p[1]; }
				break;
			}
		out[j] = 0;
		return;
	}

	// First meaningful word of the qualifier (skip separators + filler words).
	for (p = q; p < qend; ) {
		const char *w; int wlen;
		while (p < qend && !is_word(*p)) p++;
		w = p; while (p < qend && is_word(*p)) p++;
		wlen = (int)(p - w);
		if (wlen == 0) break;
		if (is_filler(w, wlen)) continue;
		if (j < outsz - 2) {
			out[j++] = ' ';
			for (k = 0; k < wlen && j < outsz - 1; k++) out[j++] = w[k];
		}
		break;
	}
	out[j] = 0;
}

// A line's opcode is one we can execute one-`_L`-at-a-time with no extra-line
// consumption and no pad read. Everything else -> the whole cheat is dropped.
static int type_supported(u32 p1, u32 p2)
{
	int t = (int)(p1 >> 28);
	if (t <= 2)   return 1;
	if (t == 0xE) return 1;
	if (t == 0xD) { int sub = (int)(p2 >> 28); return (sub == 0 || sub == 2); }
	return 0;
}

// ── parser state machine (one call per DB text line) ─────────────────────────
static void commit_cur(void)
{
	// pa_cur_real drops "placeholder" cheats whose only lines are 0x00000000
	// 0x00000000 (this DB ships many "Force FPS..." entries as such dummies).
	// Require at least one real (non-zero) write.
	if (pa_cur_fps && pa_cur_real && !pa_cur_unsup && pa_cur_cnt > 0 &&
	    g_fps_opt_count < FPS_MAX_OPTIONS &&
	    g_fps_pool_used + pa_cur_cnt <= FPS_MAX_LINES) {
		memcpy(&g_fps_pool[g_fps_pool_used], pa_cur_lines, (size_t)pa_cur_cnt * sizeof(cline));
		strncpy(g_fps_name[g_fps_opt_count], pa_cur_name, FPS_NAME_LEN - 1);
		g_fps_name[g_fps_opt_count][FPS_NAME_LEN - 1] = 0;
		make_label(pa_cur_name, g_fps_label[g_fps_opt_count], FPS_LABEL_LEN);
		g_fps_off[g_fps_opt_count] = (u16)g_fps_pool_used;
		g_fps_cnt[g_fps_opt_count] = (u16)pa_cur_cnt;
		g_fps_pool_used += pa_cur_cnt;
		g_fps_opt_count++;
	} else if (pa_cur_fps && pa_cur_real && !pa_cur_unsup && pa_cur_cnt > 0) {
		// A supported, real FPS cheat was dropped (option cap / pool full / too
		// long). Logged so a missing menu entry is explainable, not silent.
#if DEBUG_BUILD
		const char *why = (g_fps_opt_count >= FPS_MAX_OPTIONS) ? "option cap"
		                  : (g_fps_pool_used + pa_cur_cnt > FPS_MAX_LINES) ? "line pool full"
		                  : "cheat too long";
		WriteDebugLogRawF("[FPS] '%.24s' dropped (%s)", pa_cur_name, why);
#endif
	}
	pa_cur_fps = pa_cur_unsup = pa_cur_cnt = pa_cur_real = 0; pa_cur_name[0] = 0;
}

static void proc_line(char *s)
{
	char t;
	if (pa_done || s[0] != '_') return;
	t = s[1];
	if (t == 'S') {                         // _S <gameid> — section boundary
		commit_cur();
		if (pa_in_game) { pa_done = 1; return; }   // reached the game after ours
		{
			const char *p = skip_ws(s + 2);
			char db[16]; int j = 0;
			for (; *p && *p != ' ' && *p != '\t' && j < 15; p++)
				if (*p != '-') db[j++] = *p;        // strip the dash: ULUS-10249 -> ULUS10249
			db[j] = 0;
			if (pa_mykey[0] && !strcmp(db, pa_mykey)) { pa_in_game = 1; pa_matched = 1; }
		}
		return;
	}
	if (!pa_in_game) return;
	if (t == 'C') {                         // _C0/_C1 <name> — new cheat block
		const char *p = s + 2;
		commit_cur();
		if (*p) p++;                        // skip the 0/1
		p = skip_ws(p);
		strncpy(pa_cur_name, p, FPS_NAME_LEN - 1); pa_cur_name[FPS_NAME_LEN - 1] = 0;
		pa_cur_fps   = name_is_fps(pa_cur_name) && !name_is_separator(pa_cur_name);
		if (pa_cur_fps) pa_seen_fps++;
		pa_cur_unsup = 0; pa_cur_cnt = 0; pa_cur_real = 0;
		return;
	}
	if (t == 'L') {                         // _L P1 P2 [//comment] — code line
		const char *p = s + 2; u32 a, b;
		if (!pa_cur_fps) return;            // only parse lines of FPS cheats
		if (!rd_hex(&p, &a) || !rd_hex(&p, &b)) return;
		if (pa_cur_cnt >= FPS_MAX_CHEAT_LINES) { pa_cur_unsup = 1; return; }
		pa_cur_lines[pa_cur_cnt].p1 = a; pa_cur_lines[pa_cur_cnt].p2 = b;
		if (!type_supported(a, b)) pa_cur_unsup = 1;
		if (a || b) pa_cur_real = 1;        // a real write (not a 0x0/0x0 placeholder)
		pa_cur_cnt++;
	}
}

// ── per-game persistence: SAVESTATE/<gid>/fps.cfg (selected cheat NAME) ───────
#define FPS_CFG_MAGIC 0x46505343u   // "FPSC"
static void cheats_cfg_path(char *p)
{
	const char *gid = umdid[0] ? umdid : "globalstate";
	sprintf(p, "ms0:/seplugins/SAVESTATE/%s/fps.cfg", gid);
}
static void cheats_save_selection(void)
{
	char p[96], d[80]; SceUID fd; u32 magic = FPS_CFG_MAGIC;
	const char *gid = umdid[0] ? umdid : "globalstate";
	const char *nm = (g_fps_active > 0 && g_fps_active <= g_fps_opt_count)
	                 ? g_fps_name[g_fps_active - 1] : "";
	sceIoMkdir("ms0:/seplugins/SAVESTATE", 0777);
	sprintf(d, "ms0:/seplugins/SAVESTATE/%s", gid); sceIoMkdir(d, 0777);
	sprintf(p, "%s/fps.cfg", d);
	fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0) { sceIoWrite(fd, &magic, 4); sceIoWrite(fd, nm, (int)strlen(nm) + 1); sceIoClose(fd); }
}
static void cheats_load_selection(void)
{
	char p[96]; SceUID fd; u32 magic = 0; char nm[FPS_NAME_LEN];
	g_fps_active = 0;
	cheats_cfg_path(p);
	fd = sceIoOpen(p, PSP_O_RDONLY, 0);
	if (fd < 0) return;
	if (sceIoRead(fd, &magic, 4) == 4 && magic == FPS_CFG_MAGIC) {
		int n = sceIoRead(fd, nm, sizeof(nm) - 1);
		if (n < 0) n = 0; nm[n] = 0;
		if (nm[0]) {
			int i;
			for (i = 0; i < g_fps_opt_count; i++)
				if (!strcmp(nm, g_fps_name[i])) { g_fps_active = i + 1; break; }
		}
	}
	sceIoClose(fd);
}

// ── boot load: stream the DB, build the option list, restore the selection ────
void cheats_load_for_game(void)
{
	static char rdbuf[2048];
	// Candidate DB locations (first that opens wins). The seplugins root is the
	// preferred home; the SAVESTATE folder (the original location) stays as a
	// fallback. Robust to how the user named the copy: cheats.db (canonical) /
	// cheat.db / CHEAT5.DB.
	static const char *cand[] = {
		"ms0:/seplugins/cheats.db",
		"ms0:/seplugins/cheat.db",
		"ms0:/seplugins/CHEAT5.DB",
		"ms0:/seplugins/SAVESTATE/cheats.db",
		"ms0:/seplugins/SAVESTATE/cheat.db",
		"ms0:/seplugins/SAVESTATE/CHEAT5.DB",
	};
	char linebuf[288];
	SceUID fd = -1; int i, llen, ci; const char *dbpath = cand[0];

	g_fps_opt_count = 0; g_fps_pool_used = 0; g_fps_active = 0; g_fps_quit = 0; g_fps_db_found = 0;
	pa_in_game = pa_done = pa_matched = pa_seen_fps = 0;
	pa_cur_fps = pa_cur_unsup = pa_cur_cnt = 0; pa_cur_name[0] = 0;

	// our game key = umdid up to the '_' (disc id, no dash), e.g. "ULUS10249".
	for (i = 0; umdid[i] && umdid[i] != '_' && i < 15; i++) pa_mykey[i] = umdid[i];
	pa_mykey[i] = 0;

	for (ci = 0; ci < (int)(sizeof(cand) / sizeof(cand[0])); ci++) {
		fd = sceIoOpen(cand[ci], PSP_O_RDONLY, 0);
		if (fd >= 0) { dbpath = cand[ci]; g_fps_db_found = 1; break; }
	}
	if (fd < 0) {
		if (DBG_UART()) uart_puts("[FPS] no DB found (seplugins/ or seplugins/SAVESTATE/) - Stock only");
		return;
	}
	if (DBG_UART()) { char b[96]; sprintf(b, "[FPS] opened %s, game key=%.9s", dbpath, pa_mykey); uart_puts(b); }
	llen = 0;
	for (;;) {
		int n = sceIoRead(fd, rdbuf, sizeof(rdbuf)), k;
		if (n <= 0) break;
		for (k = 0; k < n && !pa_done; k++) {
			char c = rdbuf[k];
			if (c == '\n' || c == '\r') {
				if (llen > 0) { linebuf[llen] = 0; proc_line(linebuf); llen = 0; }
			} else if (llen < (int)sizeof(linebuf) - 1) {
				linebuf[llen++] = c;
			}
			// over-long line: extra chars dropped, still terminates on newline
		}
		if (pa_done) break;
	}
	if (!pa_done && llen > 0) { linebuf[llen] = 0; proc_line(linebuf); }
	commit_cur();   // flush a trailing cheat (our section was the file's last)
	sceIoClose(fd);

	cheats_load_selection();
	if (DBG_UART()) {
		char b[96];
		sprintf(b, "[FPS] matched=%d fps_named=%d kept=%d active=%d",
		        pa_matched, pa_seen_fps, g_fps_opt_count, g_fps_active);
		uart_puts(b);
	}
	// The apply thread lazily backs up each address the instant before it first
	// changes it (see backup_addr), so no up-front snapshot here.
	cheats_ensure_started();
}

// ── interpreter ──────────────────────────────────────────────────────────────
static u32 rd_mem(u32 addr, int sz)
{
	if (sz == 1) return *(volatile u8  *)addr;
	if (sz == 2) return *(volatile u16 *)addr;
	return *(volatile u32 *)addr;
}
static int wr_mem(u32 addr, u32 val, int sz)   // returns 1 if the value changed
{
	if (sz == 1) { volatile u8  *p = (volatile u8  *)addr; if (*p == (u8)val)  return 0; *p = (u8)val;  return 1; }
	if (sz == 2) { volatile u16 *p = (volatile u16 *)addr; if (*p == (u16)val) return 0; *p = (u16)val; return 1; }
	{ volatile u32 *p = (volatile u32 *)addr; if (*p == val) return 0; *p = val; return 1; }
}
static int cond_true(int op, u32 a, u32 b)
{
	switch (op & 3) { case 0: return a == b; case 1: return a != b; case 2: return a < b; default: return a > b; }
}

// Lazy original-bytes backup: record addr's value the FIRST time the cheat is about
// to CHANGE it (orig = the value read just before the write). Capturing at
// first-change, not up front, guarantees the game's real pre-patch code is present.
static void backup_addr(u32 addr, int sz, u32 orig)
{
	int i;
	for (i = 0; i < g_bk_cnt; i++) if (g_bk_addr[i] == addr) return;   // already recorded
	if (g_bk_cnt >= FPS_MAX_CHEAT_LINES) return;
	g_bk_addr[g_bk_cnt] = addr; g_bk_sz[g_bk_cnt] = (u8)sz; g_bk_val[g_bk_cnt] = orig;
	g_bk_cnt++;
	g_bk_opt = g_fps_active;
}

static void cheats_apply_active(void)
{
	cline *L; int n, i;
	int a = g_fps_active;
	if (a <= 0 || a > g_fps_opt_count) return;
	L = &g_fps_pool[g_fps_off[a - 1]];
	n = g_fps_cnt[a - 1];

	for (i = 0; i < n; ) {
		u32 p1 = L[i].p1, p2 = L[i].p2; int t = (int)(p1 >> 28); i++;
		if (t <= 2) {
			u32 addr = ((p1 & 0x0FFFFFFF) + GAME_RAM_LO) & 0x3FFFFFFF;
			int sz = (t == 1) ? 2 : (t == 2) ? 4
			       : (p2 & 0xFFFF0000) ? 4 : (p2 & 0xFF00) ? 2 : 1;   // type-0 auto-size
			u32 want = (sz == 1) ? (p2 & 0xFF) : (sz == 2) ? (p2 & 0xFFFF) : p2;
			if (addr >= GAME_RAM_LO && addr + (u32)sz <= GAME_RAM_HI) {
				u32 cur = rd_mem(addr, sz);
				if (cur != want) {          // about to change it -> back up the ORIGINAL first
					backup_addr(addr, sz, cur);
					wr_mem(addr, p2, sz);
					// Flush only THIS write's line.
					sceKernelDcacheWritebackInvalidateRange((void *)addr, (u32)sz);
					sceKernelIcacheInvalidateRange((void *)addr, (u32)sz);
				}
			}
		} else if (t == 0xE) {
			int is8   = ((p1 >> 24) == 0xE1);
			u32 val   = is8 ? (p1 & 0xFF) : (p1 & 0xFFFF);
			u32 addr  = ((p2 & 0x0FFFFFFF) + GAME_RAM_LO) & 0x3FFFFFFF;
			int op    = (int)(p2 >> 28);
			int skip  = (int)((p1 >> 16) & (is8 ? 0xFF : 0xFFF));
			if (addr >= GAME_RAM_LO && addr < GAME_RAM_HI) {
				if (!cond_true(op, rd_mem(addr, is8 ? 1 : 2), val)) i += skip;
			} else i += skip;   // unreadable guard address -> treat as false, skip block
		} else if (t == 0xD) {
			u32 addr = ((p1 & 0x0FFFFFFF) + GAME_RAM_LO) & 0x3FFFFFFF;
			int is8  = ((p2 >> 28) == 0x2);
			u32 val  = is8 ? (p2 & 0xFF) : (p2 & 0xFFFF);
			int op   = (int)((p2 >> 20) & 0xF);
			if (addr >= GAME_RAM_LO && addr < GAME_RAM_HI) {
				if (!cond_true(op, rd_mem(addr, is8 ? 1 : 2), val)) i += 1;
			} else i += 1;
		}
		/* other types were excluded at load (type_supported) */
	}
}

// Write the backed-up original bytes back, undoing the live cheat's patches.
static void cheats_restore_backup(void)
{
	int i;
	if (g_bk_opt == 0) return;
	for (i = 0; i < g_bk_cnt; i++) {
		if (wr_mem(g_bk_addr[i], g_bk_val[i], g_bk_sz[i])) {
			sceKernelDcacheWritebackInvalidateRange((void *)g_bk_addr[i], g_bk_sz[i]);
			sceKernelIcacheInvalidateRange((void *)g_bk_addr[i], g_bk_sz[i]);
		}
	}
	g_bk_opt = 0; g_bk_cnt = 0;
}

// ── per-vblank apply thread ──────────────────────────────────────────────────
static int cheats_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	for (;;) {
		if (g_fps_quit || g_fps_active <= 0) break; // game exit / back to Stock -> retire the thread
		// Gated (menu open OR a save/load op in flight): SLEEP, do not fall through
		// to fps_wait_vblank_real.
		if (g_menu_open || g_st_op_hold) { sceKernelDelayThread(100000); continue; }
		fps_wait_vblank_real();
		if (g_fps_quit || g_fps_active <= 0) break;
		if (g_menu_open || g_st_op_hold) continue;  // re-check after the wait -> never touch game RAM mid-op
		cheats_apply_active();
	}
	g_fps_thread_started = 0;
	return 0;
}

void cheats_ensure_started(void)
{
	SceUID t;
	if (g_fps_thread_started || g_fps_active <= 0) return;
	g_fps_quit = 0;
	g_fps_thread_started = 1;
	t = sceKernelCreateThread("pspstates_fps", cheats_thread, 32, 0x1000, 0, NULL);
	if (t >= 0) sceKernelStartThread(t, 0, NULL);
	else        g_fps_thread_started = 0;
}

// Called from the game-exit hook (overclock.c) BEFORE the game's RAM/GE are torn
// down: stop the apply thread and wait (bounded) for it to actually exit.
void cheats_stop(void)
{
	int i;
	g_fps_quit = 1;
	for (i = 0; i < 60 && g_fps_thread_started; i++)
		sceKernelDelayThread(2000);   // up to ~120ms (a gated tick sleeps 100ms)
}

// ── menu-facing ──────────────────────────────────────────────────────────────
const char *cheats_fps_name(int idx)
{
	// Empty-state labels for the menu row: no DB file at all vs. a DB that has
	// no FPS cheats for this game. With options present, idx 0 = "Stock".
	if (g_fps_opt_count <= 0)
		return g_fps_db_found ? "No FPS unlock in Cheat.db" : "No Cheat.db found";
	if (idx <= 0 || idx > g_fps_opt_count) return "Stock";
	return g_fps_label[idx - 1];   // short label for the menu row
}

void cheats_fps_cycle(int dir)
{
	int m = g_fps_opt_count, nw;            // options 1..m, plus Stock at 0
	if (m <= 0) { g_fps_active = 0; return; }
	nw = (dir > 0) ? ((g_fps_active < m) ? g_fps_active + 1 : 0)
	               : ((g_fps_active > 0) ? g_fps_active - 1 : m);
	if (nw == g_fps_active) return;
	// Called from the settings menu (apply thread gated off). Undo the currently-applied
	// cheat first; the new option's backup is built lazily by the apply thread.
	cheats_restore_backup();
	g_fps_active = nw;
	cheats_save_selection();
	cheats_ensure_started();
}
