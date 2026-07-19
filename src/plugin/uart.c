// PSP->PC UART debug channel (TX-only) over the PSP-1000 remote connector
// (HPREMOTE UART, port 4, base 0xBE500000). This is the ONLY log that survives
// the firmware suspend window: emitting a byte is pure memory-mapped register
// I/O — no firmware call, no interrupt, no scheduler — so it works when sceIo,
// the MS debug log, and the pspsh/USB link are all dead.
//
// Self-contained per HOWTO_UART.md. Additive logging only: it must NOT change
// save/load behavior. See HOWTO_UART.md §7a — uart_init() makes firmware calls
// and sleeps, so it is called ONLY where the firmware is fully alive
// (module_start, and post-resume AFTER wait_for_resume). Inside the suspend
// descent, use ONLY the register-only uart_puts/uart_log_hex.
//
// All of this compiles OUT when DEBUG_BUILD is 0 (see pspstates.h — the calls
// become no-op macros there, so this file's body is empty in a release build).

#include "pspstates.h"

#if DEBUG_BUILD

#include <pspsdk.h>   // pspSdkSetK1

// sceHprmEnd links via -lpsphprm_driver (NID 0x588845DA on 6.61). Not declared
// in a public SDK header, so declare it here.
int sceHprmEnd(void);

// ── HPREMOTE UART registers, base 0xBE500000 (port 4). All raw — no firmware
//    call is needed to TX, so they stay usable inside the suspend window. ──
#define UART_FIFO    0xBE500000   // write a byte here -> TX
#define UART_STAT    0xBE500018   // bit 0x20 (UART_TXFULL) = TX FIFO full
#define UART_DIV1    0xBE500024   // baud high bits = (CLK/baud) >> 6
#define UART_DIV2    0xBE500028   // baud low  bits = (CLK/baud) & 0x3F
#define UART_CTRL    0xBE50002C   // 0x60 = enable
#define UART_CTRL2   0xBE500030   // firmware writes |= 0x301  (framing/control)
#define UART_INTR    0xBE500040   // interrupt/status source (read)
#define UART_INTRCLR 0xBE500044   // firmware writes = 0x7FF (CLEAR latched intr/status)
#define UART_TXFULL  0x20

// sysreg clock / IO enable (port 4). Raw -> re-assertable mid-suspend.
#define SYSREG_BUSCLK 0xBC100050  // |= 0x4000            : UART/APB bus clock
#define SYSREG_CLK2   0xBC100058  // |= (0x40   << port)  : per-UART clock (port 4 => 0x400)
#define SYSREG_IOEN   0xBC100078  // |= (0x10000<< port)  : per-UART IO    (port 4 => 0x100000)
#define UART_PORT     4

#define UART_CLK   96000000
#define UART_BAUD  115200         // div = 96e6/115200 = 833 -> DIV1=13, DIV2=1
#define REG32(a)   (*(volatile unsigned int *)(a))

// dead-port streak guard: if the FIFO stays full for this many consecutive
// bytes, presume the port dead and stop until the next uart_init un-wedges it.
#define UART_DEAD_STREAK 64

static int g_uart_ready = 0;
static int g_uart_dead_streak = 0;

// Resolved-at-runtime firmware helpers (NIDs from HOWTO_UART.md §4).
static void (*g_hprm_reset)(void) = 0;   // sceHprmReset — release UART4 (paired with End)
static int  (*g_hrpower)(int)     = 0;   // sceSysconCtrlHRPower — (re)power the remote rail

// ── uart_init(): the exact working sequence from HOWTO_UART.md §5. ──
// NOT register-only — makes firmware calls and sleeps 100ms. Call ONLY where the
// firmware is alive (module_start, post-resume after wait_for_resume). NEVER
// inside the suspend descent (see HOWTO §7a).
void uart_init(void)
{
	unsigned int k1 = pspSdkSetK1(0);
	unsigned int div = UART_CLK / UART_BAUD;               // 833

	// HPRM teardown (release UART4). Reset first, then End.
	if (!g_hprm_reset)
		g_hprm_reset = (void(*)(void))sctrlHENFindFunction("sceHprm_Driver", "sceHprm_driver", 0x4D1E622C);
	if (g_hprm_reset) g_hprm_reset();
	sceHprmEnd();

	// Restore the remote-connector rail (suspend power-down cuts it).
	if (!g_hrpower)
		g_hrpower = (int(*)(int))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0xBB7260C8);
	if (g_hrpower) g_hrpower(1);

	// Enable clock + IO, then a sync barrier (firmware sub_49B8 does this).
	REG32(SYSREG_BUSCLK) |= 0x4000;
	REG32(SYSREG_CLK2)   |= (0x40u    << UART_PORT);
	REG32(SYSREG_IOEN)   |= (0x10000u << UART_PORT);
	REG32(SYSREG_IOEN)   |= (0x10000u << 1);               // psp-uart-library also enables port 1's IO
	asm volatile("sync");
	sceKernelDelayThread(100000);                          // ~100ms rail settle

	// Program baud + enable, then the two firmware writes (the +0x44 clear = the fix).
	REG32(UART_DIV1)    = div >> 6;                        // 13
	REG32(UART_DIV2)    = div & 0x3F;                       // 1
	REG32(UART_CTRL)    = 0x60;                             // enable
	REG32(UART_CTRL2)  |= 0x301;                            // control/framing
	REG32(UART_INTRCLR) = 0x7FF;                            // clear latched status -> un-wedge TXFULL
	asm volatile("sync");

	g_uart_ready = 1;
	g_uart_dead_streak = 0;
	pspSdkSetK1(k1);
}

// Re-assert clock/IO (the suspend descent can gate the clock). Register-only.
static void uart_reassert_clocks(void)
{
	REG32(SYSREG_BUSCLK) |= 0x4000;
	REG32(SYSREG_CLK2)   |= (0x40u    << UART_PORT);
	REG32(SYSREG_IOEN)   |= (0x10000u << UART_PORT);
}

// Bounded TX-full wait; DROP the byte on timeout (an unbounded spin in a suspend
// path would itself become a hang). A dead-port streak marks the port not-ready
// until the next uart_init. Pure register I/O — safe inside the suspend window.
static void uart_putc_bounded(int ch)
{
	int i;
	for (i = 0; i < 100000 && (REG32(UART_STAT) & UART_TXFULL); i++)
		;
	if (REG32(UART_STAT) & UART_TXFULL) {
		if (++g_uart_dead_streak >= UART_DEAD_STREAK)
			g_uart_ready = 0;
		return;
	}
	g_uart_dead_streak = 0;
	REG32(UART_FIFO) = (unsigned int)ch;
}

void uart_puts(const char *s)
{
	if (!g_uart_ready || !s) return;
	uart_reassert_clocks();   // in case we log inside the suspend window
	while (*s) uart_putc_bounded(*s++);
	uart_putc_bounded('\r');
	uart_putc_bounded('\n');
}

// Self-contained "<prefix> 0xXXXXXXXX" hex line — no sprintf (keeps it safe in
// the dead window, where the C library may be unusable). Register-only.
void uart_log_hex(const char *prefix, unsigned int val)
{
	static const char hex[] = "0123456789ABCDEF";
	int p;
	if (!g_uart_ready) return;
	uart_reassert_clocks();
	if (prefix) while (*prefix) uart_putc_bounded(*prefix++);
	uart_putc_bounded(' ');
	uart_putc_bounded('0');
	uart_putc_bounded('x');
	for (p = 7; p >= 0; p--)
		uart_putc_bounded(hex[(val >> (p * 4)) & 0xF]);
	uart_putc_bounded('\r');
	uart_putc_bounded('\n');
}

#endif // DEBUG_BUILD
