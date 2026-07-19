// PspStates for PSP (32MB & 64MB) by Dark-Alex
// Kernel-freeze save/load — bare-metal MS I/O, atomic RAM access

// header
#include "pspstates.h"

// info
PSP_MODULE_INFO("pspstates_v" VERSION_STRING, PSP_MODULE_KERNEL | PSP_MODULE_SINGLE_LOAD | PSP_MODULE_SINGLE_START, 1, 1);

// global — MUST be explicitly initialized (kernel PRX BSS is not zeroed!)
char umdid[24] = {0};			// disc id + "_" + 8-hex ISO-master hash (fan-game disambiguation)
STMOD_HANDLER previous = NULL;	// 0x2F90

// extern
extern int is_running;				// 0x2FA8
extern int state_flag;				// 0x2F50
extern PspSysEventHandler events;
extern char g_game_title[64];		// game display name (filled here from the PVD volume label)

// Read the running game's display name from PARAM.SFO (TITLE key) into g_game_title.
// The real title lives in PARAM.SFO, NOT the ISO volume label. disc0: is only mounted
// a little after our first module hook, so this is retried from OnModuleStart until it
// succeeds — at boot, never at menu time (a menu-time disc read is unsafe). SFO is a
// small fixed-layout table: header @8/12/16, 16-byte index entries from offset 20.
static void try_read_sfo_title(void)
{
	static char sbuf[3072] __attribute__((aligned(4)));
	SceUID fd; int n; u32 keytab, datatab, nent, idx;
	fd = sceIoOpen("disc0:/PSP_GAME/PARAM.SFO", PSP_O_RDONLY, 0);
	if(fd < 0) return;
	n = sceIoRead(fd, sbuf, sizeof(sbuf));
	sceIoClose(fd);
	if(n < 20 || *(u32 *)sbuf != 0x46535000) return;   // "\0PSF" magic
	keytab  = *(u32 *)(sbuf + 8);
	datatab = *(u32 *)(sbuf + 12);
	nent    = *(u32 *)(sbuf + 16);
	if(nent > 128) nent = 128;
	for(idx = 0; idx < nent; idx++)
	{
		unsigned char *e = (unsigned char *)sbuf + 20 + idx * 16;
		u16 koff = *(u16 *)(e + 0);
		u32 dlen = *(u32 *)(e + 4);
		u32 doff = *(u32 *)(e + 12);
		if((u32)(20 + idx * 16 + 16) > (u32)n) break;
		// keytab/datatab/koff/doff are untrusted (straight from the disc SFO).
		// Check each addend against n separately so the sum can't wrap past
		// 0xFFFFFFFF and slip a wild offset past the bound. (n <= sizeof(sbuf).)
		if(keytab >= (u32)n || koff >= (u32)n - keytab) continue;
		if(strcmp(sbuf + keytab + koff, "TITLE") == 0)
		{
			u32 j, m, avail;
			if(datatab >= (u32)n || doff >= (u32)n - datatab) return;
			avail = (u32)n - datatab - doff;   // > 0: datatab + doff < n proven above
			m = (dlen < 63) ? dlen : 63;
			if(m > avail) m = avail;
			for(j = 0; j < m; j++) {
				char c = sbuf[datatab + doff + j];
				if(!c) break;
				g_game_title[j] = c;
			}
			g_game_title[j] = '\0';
			return;
		}
	}
}

// sub_0000076C
int OnModuleStart(SceModule2 *module)
{
	int i, n;
	SceUID fd, memid;
	char *sector_buffer = NULL, *filename, *filename_tail;

	if(strcmp(module->modname, "sceKernelLibrary") == 0)
	{
		if(sceKernelBootFrom() == PSP_BOOT_DISC)
		{
			memid = 0;

			if(sceIoDevctl("umd0:", 0x01E28035, NULL, 0, &sector_buffer, 4) < 0)
			{
				memid = sceKernelAllocPartitionMemory(8, "PVD", 0x800, PSP_SMEM_High, NULL);

				if(memid > 0)
				{
					char *blk = sceKernelGetBlockHeadAddr(memid);

					fd = sceIoOpen("umd0:", PSP_O_RDONLY, 0);

					if(fd > 0)
					{
						sceIoLseek(fd, 0x10, PSP_SEEK_SET);
						// Only adopt the buffer if the read actually succeeded — otherwise
						// blk points at uninitialized heap and would yield a garbage umdid.
						if(sceIoRead(fd, blk, 1) == 1)
							sector_buffer = blk;
						sceIoClose(fd);
					}
				}
			}

			if(sector_buffer != NULL)
			{
				for(i = 0, n = 0; i < 10; i++)
				{
					if(sector_buffer[0x373 + i] != '-')
						umdid[n++] = sector_buffer[0x373 + i];
				}

				umdid[9] = '\0';

				// Fan games re-use an official disc ID but ship a different ISO master.
				// The sector here is the ISO9660 Primary Volume Descriptor (the disc ID
				// at 0x373 is its 512-byte "Application Used" field). Hash the per-master
				// PVD fields — volume identifier (@40, 32B), volume space size (@80, 8B),
				// and the volume creation+modification timestamps (@813, 17B+17B) — into
				// an 8-hex suffix so each master maps to its own save folder. Stable for
				// a given ISO every launch, distinct between re-masters. (All offsets are
				// below 0x373, so they're within the same already-valid sector.)
				{
					static const char hexd[] = "0123456789ABCDEF";
					unsigned int h = 2166136261u;   // FNV-1a 32-bit
					int b, k, L;
					for(b = 40;  b < 40 + 32;  b++) h = (h ^ (unsigned char)sector_buffer[b]) * 16777619u;
					for(b = 80;  b < 80 + 8;   b++) h = (h ^ (unsigned char)sector_buffer[b]) * 16777619u;
					for(b = 813; b < 813 + 34; b++) h = (h ^ (unsigned char)sector_buffer[b]) * 16777619u;
					L = (int)strlen(umdid);
					if(L > 9) L = 9;
					umdid[L] = '_';
					for(k = 0; k < 8; k++)
						umdid[L + 1 + k] = hexd[(h >> ((7 - k) * 4)) & 0xF];
					umdid[L + 1 + 8] = '\0';
				}
			}

			if(memid > 0)
				sceKernelFreePartitionMemory(memid);
		}
		else if(sceKernelBootFrom() == PSP_BOOT_MS)
		{
			filename = sceKernelInitFileName();

			if(filename != NULL)
			{
				filename_tail = strrchr(filename, '/');
				if(filename_tail != NULL)
				{
					char *parent;
					filename_tail[0] = '\0';               // truncate at the last '/'
					parent = strrchr(filename, '/');       // the '/' before the leaf dir
					if(parent != NULL)                     // NULL for a single-'/' path (ms0:/EBOOT.PBP)
					{
						strncpy(umdid, parent + 1, 9);
						umdid[9] = '\0';
					}
					filename_tail[0] = '/';                // restore the path
				}
			}
		}
	}

	// Controller interception is done once via sctrlHENPatchSyscall in
	// module_start (catches the game's syscalls regardless of module), so no
	// per-module hook is needed here.

	// Grab the game's display title once disc0: is mounted (a bit after our first
	// hook). Retried on each module start until it succeeds; boot-time only.
	if(g_game_title[0] == '\0')
		try_read_sfo_title();

	return previous ? previous(module) : 0;
}

// module_start
int module_start(SceSize args, void *argp)
{
	SceUID thid;

	// ── Runtime BSS zero-init (kernel PRX .bss is NOT auto-zeroed!) ──
	state_flag = 0;
	is_running = 0;
	g_game_title[0] = '\0';

	// Version banner is RAW: it must appear regardless of the Debug setting —
	// the deploy loop greps the MS log for "pspstates_v<N>" to verify the
	// running build after every deploy (default setting is OFF).
	WriteDebugLogRaw("=== pspstates_v" VERSION_STRING " module_start() ===");
	WriteDebugLog("Save browser — hook-only input + on-demand menu thread");

	if(sceKernelInitKeyConfig() != PSP_INIT_KEYCONFIG_GAME) {
		WriteDebugLog("KeyConfig is not GAME. Exiting.");
		return 1;
	}

	// Bring up the PSP->PC UART debug channel and print the version boot banner
	// (the deploy/version check — UART carries it even with the MS Debug setting
	// off). In-game only: the remote-connector port is dead in the XMB, and we
	// only reach here for a GAME keyconfig. See HOWTO_UART.md.
	uart_init();
	uart_puts("=== pspstates_v" VERSION_STRING " boot ===");

	// Intercept the GAME's controller reads via the SYSCALL table (ARK-4 vshctrl
	// pattern) — robust and firmware-independent, unlike hooking sceImpose's
	// imports (sceImpose only reads on HOME) or a fixed text offset. Find the real
	// sceController_Service Peek/Read, save them so the wrappers can call through,
	// then redirect the syscalls to our wrappers. Every user-mode sceCtrl*Positive
	// call (i.e. the game, every frame) now passes through us.
	g_real_ctrl_peek = (int(*)(SceCtrlData*,int))sctrlHENFindFunction("sceController_Service", "sceCtrl", 0x3A622550);
	g_real_ctrl_read = (int(*)(SceCtrlData*,int))sctrlHENFindFunction("sceController_Service", "sceCtrl", 0x1F803938);
	if (g_real_ctrl_peek) sctrlHENPatchSyscall((void *)g_real_ctrl_peek, sceCtrlPeekBufferPositivePatched);
	if (g_real_ctrl_read) sctrlHENPatchSyscall((void *)g_real_ctrl_read, sceCtrlReadBufferPositivePatched);
	// Also hook the LATCH reads (second button leak, latch-reading games e.g.
	// Ratchet & Clank): sceCtrlReadLatch 0x0B588501 / sceCtrlPeekLatch 0xB1D0E5CD.
	g_real_ctrl_readlatch = (int(*)(SceCtrlLatch*))sctrlHENFindFunction("sceController_Service", "sceCtrl", 0x0B588501);
	g_real_ctrl_peeklatch = (int(*)(SceCtrlLatch*))sctrlHENFindFunction("sceController_Service", "sceCtrl", 0xB1D0E5CD);
	if (g_real_ctrl_readlatch) sctrlHENPatchSyscall((void *)g_real_ctrl_readlatch, sceCtrlReadLatchPatched);
	if (g_real_ctrl_peeklatch) sctrlHENPatchSyscall((void *)g_real_ctrl_peeklatch, sceCtrlPeekLatchPatched);
	ClearCaches();
	WriteDebugLogHex("ctrl Peek real=", (u32)g_real_ctrl_peek);
	WriteDebugLogHex("ctrl Read real=", (u32)g_real_ctrl_read);
	WriteDebugLogHex("ctrl ReadLatch real=", (u32)g_real_ctrl_readlatch);
	WriteDebugLogHex("ctrl PeekLatch real=", (u32)g_real_ctrl_peeklatch);

	// Resolve sceImposeSetHomePopup so the browser can disable HOME while open
	// (pressing HOME + exiting mid-freeze hangs the PSP). NID differs by firmware
	// — try the 1.50 NID then the 6.60 one; use whichever resolves.
	g_set_home_popup = (int(*)(int))sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x5595A71A);
	if (!g_set_home_popup)
		g_set_home_popup = (int(*)(int))sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0xC08C41EF);
	WriteDebugLogHex("sceImposeSetHomePopup=", (u32)g_set_home_popup);

	// ── Menu thread: sleeps until the open combo wakes it, then owns the
	// soft-freeze + save browser (and the heavy save/load). 20KB stack: fastlz's
	// 8KB compress hash table is now a STATIC (.bss) table, not a stack local (see
	// g_fastlz_htab in fastlz_compress.c), so FreezeSave's frame (ge_ctx 2KB, thread
	// arrays, suspend-probe payload) ~ 16KB real need; 20KB keeps margin. (Was 40KB
	// when htab was on the stack; moving it static freed ~20KB of kernel RAM.)
	thid = sceKernelCreateThread("pspstates_menu", menu_thread, 16, 0x5000, 0, NULL);
	if(thid >= 0) {
		g_menu_thid = thid;
		sceKernelStartThread(thid, 0, NULL);
		WriteDebugLog("Menu thread started.");
	} else {
		WriteDebugLogHexRaw("FAILED to create menu thread:", (u32)thid);
	}

	sceKernelRegisterSysEventHandler(&events);

	previous = sctrlHENSetStartModuleHandler(OnModuleStart);

	WriteDebugLog("module_start() successfully finished! Save browser ready.");
	return 0;
}

// module_stop
int module_stop(SceSize args, void *argp)
{
	is_running = 1;
	return 0;
}

