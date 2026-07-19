	.set noreorder
	.set noat

# PspStates — Home-menu save/load

# SysEventShim — sysevent handler entry point.
# Stores dispatcher context to g_handoff_ctx on every dispatch, then jumps to ProcessSignals.
# ApplyHandoff (apply.S) restores this context on LOAD and jumps to save-time ra.
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
	mfc0	$v1, $12
	sw	$v1, 48($v0)
	j	ProcessSignals
	nop
.end SysEventShim
