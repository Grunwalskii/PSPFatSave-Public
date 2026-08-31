// Kernel exception handler: dumps full CPU state over UART on a genuine CPU
// fault (bad memory access, bus error, reserved instruction, etc.) and stops
// there — no automatic reset or power-off, so the dump stays on the wire for
// the user to read.

#include "pspfatsave.h"
#include "crash_handler.h"
#include <pspdebug.h>
#include <pspexception.h>

static const char *g_exc_code_txt[32] = {
	"Interrupt", "TLB modification", "TLB load/inst fetch", "TLB store",
	"Address load/inst fetch", "Address store", "Bus error (instr)",
	"Bus error (data)", "Syscall", "Breakpoint", "Reserved instruction",
	"Coprocessor unusable", "Arithmetic overflow", "Unknown 13", "Unknown 14",
	"Unknown 15", "Unknown 16", "Unknown 17", "Unknown 18", "Unknown 19",
	"Unknown 20", "Unknown 21", "Unknown 22", "Unknown 23", "Unknown 24",
	"Unknown 25", "Unknown 26", "Unknown 27", "Unknown 28", "Unknown 29",
	"Unknown 30", "Unknown 31"
};

static const char g_reg_names[32][4] = {
	"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

static void crash_handler_dump(PspDebugRegBlock *regs)
{
	char b[176];
	int i;
	u32 epc = regs->epc;
	u32 ra = regs->r[31];

	uart_puts("=== [EXC] UNHANDLED CPU EXCEPTION ===");
	sprintf(b, "[EXC] cause=%s (%08x) epc=%08x status=%08x badvaddr=%08x",
		g_exc_code_txt[(regs->cause >> 2) & 31], (unsigned)regs->cause,
		(unsigned)epc, (unsigned)regs->status, (unsigned)regs->badvaddr);
	uart_puts(b);

	{
		SceModule2 *m = (SceModule2 *)sceKernelFindModuleByAddress(epc);
		if (m) sprintf(b, "[EXC] EPC in module %s @ offset %08x", m->modname, (unsigned)(epc - m->text_addr));
		else   sprintf(b, "[EXC] EPC %08x: no owning module found", (unsigned)epc);
		uart_puts(b);
	}
	{
		SceModule2 *m = (SceModule2 *)sceKernelFindModuleByAddress(ra);
		if (m) sprintf(b, "[EXC] RA(r31) in module %s @ offset %08x", m->modname, (unsigned)(ra - m->text_addr));
		else   sprintf(b, "[EXC] RA %08x: no owning module found", (unsigned)ra);
		uart_puts(b);
	}

	for (i = 0; i < 32; i += 4) {
		sprintf(b, "[EXC] %s=%08x %s=%08x %s=%08x %s=%08x",
			g_reg_names[i],     (unsigned)regs->r[i],
			g_reg_names[i + 1], (unsigned)regs->r[i + 1],
			g_reg_names[i + 2], (unsigned)regs->r[i + 2],
			g_reg_names[i + 3], (unsigned)regs->r[i + 3]);
		uart_puts(b);
	}
	uart_puts("=== [EXC] END DUMP ===");
}

void crash_handler_install(void)
{
	int r = pspDebugInstallErrorHandler(crash_handler_dump);
	uart_log_hex("[EXC] handler installed, ret=", (u32)r);
}
