	.set noreorder
	.set noat

# PspStates — Home-menu save/load

# ── Sysevent entry shim ──
# Registered as events.handler instead of ProcessSignals. On EVERY sysevent dispatch it
# stores the dispatcher-return context — the callee-saved registers, sp, ra and CP0
# Status exactly as they stand at the `cur->handler(...)` call — into g_handoff_ctx
# (plugin .data, inside the kernel image), then tail-jumps to ProcessSignals. Because
# the SAVE's mode-9 kernel capture happens INSIDE the very dispatch that entered here,
# every snapshot automatically embeds the save-time dispatcher context. The LOAD's
# ApplyHandoff (apply.S) restores that context from the applied image and jumps to the
# save-time ra: the machine returns into the SAVE session's sysevent dispatcher as if
# ITS handler had just returned — no load-session code or data is touched again.
#
# g_handoff_ctx word layout (also relied on by ApplyHandoff); words w0..w15:
#   w0-w7 = s0-s7, w8 = fp, w9 = gp, w10 = sp, w11 = ra, w12 = CP0 Status, w13-w15 pad
# (NB: cpp/gas parse comment lines that begin with "#" followed by a number or the
# word "line" as directives — phrase comments here so they never start that way.)
# Only v0/v1 are used as scratch (caller-saved); a0-a3 pass through untouched.
.global SysEventShim
.ent SysEventShim
SysEventShim:
	lui	$v0, %hi(g_handoff_ctx)
	addiu	$v0, $v0, %lo(g_handoff_ctx)
	sw	$s0, 0($v0)
	sw	$s1, 4($v0)
	sw	$s2, 8($v0)
	sw	$s3, 12($v0)
	sw	$s4, 16($v0)
	sw	$s5, 20($v0)
	sw	$s6, 24($v0)
	sw	$s7, 28($v0)
	sw	$fp, 32($v0)
	sw	$gp, 36($v0)
	sw	$sp, 40($v0)
	sw	$ra, 44($v0)
	mfc0	$v1, $12		# CP0 Status (interrupts are ON here — dispatcher resumed them)
	sw	$v1, 48($v0)
	j	ProcessSignals		# tail-call: args a0-a3 untouched, returns straight to the dispatcher
	nop
.end SysEventShim
