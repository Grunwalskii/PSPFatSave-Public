#ifndef FS_CRASH_HANDLER_H
#define FS_CRASH_HANDLER_H

// Kernel exception handler: dumps EPC/Cause/Status/BadVAddr + all 32 GPRs +
// faulting module/offset over UART on a genuine CPU fault (bad memory access,
// bus error, reserved instruction, etc.). Call once from module_start, after
// uart_init() so the handler can log immediately if it ever fires.
void crash_handler_install(void);

#endif
