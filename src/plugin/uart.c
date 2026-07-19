// PSP->PC UART debug channel (TX-only, HPREMOTE 0xBE500000) — the only log through firmware suspend.
// Register-only operation: works when sceIo/MS/USB are dead. Compiles out (no-ops) when DEBUG_BUILD=0.

#include "pspfatsave.h"

#if DEBUG_BUILD

#include <pspsdk.h>   // pspSdkSetK1

int sceHprmEnd(void);  // sceHprmEnd (NID 0x588845DA on 6.61) — not in public headers

// HPREMOTE UART registers (port 4, base 0xBE500000) — register-only I/O
#define UART_FIFO    0xBE500000
#define UART_STAT    0xBE500018
#define UART_DIV1    0xBE500024
#define UART_DIV2    0xBE500028
#define UART_CTRL    0xBE50002C
#define UART_CTRL2   0xBE500030
#define UART_INTR    0xBE500040
#define UART_INTRCLR 0xBE500044
#define UART_TXFULL  0x20
// sysreg clock/IO enable (port 4) — re-assertable mid-suspend
#define SYSREG_BUSCLK 0xBC100050
#define SYSREG_CLK2   0xBC100058
#define SYSREG_IOEN   0xBC100078
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

// Initialize UART: firmware calls + 100ms settle. Call only when firmware is alive (module_start, post-resume).
void uart_init(void)
{
	unsigned int k1 = pspSdkSetK1(0);
	unsigned int div = UART_CLK / UART_BAUD;               // 833

	// Release UART4 hardware
	if (!g_hprm_reset)
		g_hprm_reset = (void(*)(void))sctrlHENFindFunction("sceHprm_Driver", "sceHprm_driver", 0x4D1E622C);
	if (g_hprm_reset) g_hprm_reset();
	sceHprmEnd();

	// Power the remote-connector rail
	if (!g_hrpower)
		g_hrpower = (int(*)(int))sctrlHENFindFunction("sceSYSCON_Driver", "sceSyscon_driver", 0xBB7260C8);
	if (g_hrpower) g_hrpower(1);

	// Enable clocks and IO
	REG32(SYSREG_BUSCLK) |= 0x4000;
	REG32(SYSREG_CLK2)   |= (0x40u    << UART_PORT);
	REG32(SYSREG_IOEN)   |= (0x10000u << UART_PORT);
	REG32(SYSREG_IOEN)   |= (0x10000u << 1);
	asm volatile("sync");
	sceKernelDelayThread(100000);

	// Program baud and enable UART
	REG32(UART_DIV1)    = div >> 6;
	REG32(UART_DIV2)    = div & 0x3F;
	REG32(UART_CTRL)    = 0x60;
	REG32(UART_CTRL2)  |= 0x301;
	REG32(UART_INTRCLR) = 0x7FF;
	asm volatile("sync");

	g_uart_ready = 1;
	g_uart_dead_streak = 0;
	pspSdkSetK1(k1);
}

// Re-assert clocks/IO (may be gated during suspend descent).
static void uart_reassert_clocks(void)
{
	REG32(SYSREG_BUSCLK) |= 0x4000;
	REG32(SYSREG_CLK2)   |= (0x40u    << UART_PORT);
	REG32(SYSREG_IOEN)   |= (0x10000u << UART_PORT);
}

// Bounded TX-full wait; drop on timeout (unbounded wait would hang). Dead-streak gate marks port-not-ready.
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

// Emit "<prefix> 0xXXXXXXXX" without sprintf (safe in suspend where stdlib may be dead).
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
