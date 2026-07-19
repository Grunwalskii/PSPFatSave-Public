/* apply.S — ApplyHandoff, the kernel-overwrite routine for the LOAD path.
 *
 * POSITION-INDEPENDENT (only relative branches, no $gp, no absolute refs, no
 * calls, no stack) so it can be byte-copied into GAME RAM and executed there.
 * That matters because on LOAD it overwrites the entire lower-8MB kernel
 * partition — including this plugin's own code — so it CANNOT be running from
 * the kernel partition; it runs from game RAM (V2 ran from VRAM, but eDRAM
 * execution is unavailable on the phat). Leaf, no stack, so it never touches the
 * kernel stack it is overwriting.
 */
	.text
/* ApplyHandoff(const void *src, void *dst, unsigned int len, const void *ctx)
 * — the ONE-WAY kernel apply for the LOAD path (byte-copied into game RAM,
 * executed via the KSEG1 alias with CP0 interrupts already masked by the
 * caller). It NEVER RETURNS to the caller:
 *
 *   1. copy len bytes src->dst through the CACHED KSEG0 aliases (a0/a1 are cached
 *      addresses). Copying the FULL kernel window through the dcache leaves every
 *      cached kernel line holding new-image data (stale load-session lines for
 *      kernel addresses are hit-updated, never left behind) — this removes the
 *      cache-alias incoherence of the old uncached apply.
 *   2. D-cache index writeback-invalidate ALL (sysmem's own
 *      sceKernelL1DcacheWritebackInvalidateAll sequence: cache 0x14 twice per
 *      64-byte index) — pushes the applied image (and any other dirty line) to RAM
 *      and leaves the dcache empty: every later read fetches the applied image.
 *   3. I-cache index invalidate ALL (sysmem's sceKernelL1IcacheInvalidateAll
 *      sequence: TagLo/TagHi zeroed, cache 0x1 + 0x3 per index) — no stale
 *      load-session instructions can execute afterwards.
 *   4. restore the SAVE-TIME dispatcher-return context from ctx (= the save-time
 *      address of g_handoff_ctx inside the just-applied image; layout in extras.S),
 *      set v0=0 (the handler return value) and jr to the save-time ra: execution
 *      continues inside the SAVE session's sceKernelSysEventDispatch as if its own
 *      PHASE0_0 handler had just returned. The load session is abandoned — the V2
 *      pattern (HijackPowerModuleForLoadState/load_processing_data), generalized.
 *
 * UART markers B1/B2/B3 — the ONLY visibility into the cross-load hang site (this
 * blob runs inside the fully-quiesced suspend window where sceIo and the MS log are
 * dead). Pure register I/O to the HPREMOTE UART FIFO (0xBE500000), safe here (no
 * firmware call, no stack). B1 = entry (before the copy), B2 = copy done (before
 * caches), B3 = caches flushed (about to jump). The last Bn seen on the wire = where
 * it hung. Emitted UNCONDITIONALLY: the blob is position-independent and cannot read
 * the g_uart_log global, and an always-on write is harmless — if the port is
 * idle/unpowered the bounded TXFULL wait just times out and the byte drops. Each
 * emit clobbers ONLY t8 (FIFO base) / t9 (TXFULL countdown) / v0 (STAT read) —
 * never a0-a3 (copy/handoff args) or t0/t1 (cache loops). v0 is reused as copy
 * scratch below, dead across the markers.
 *
 *   UART_FIFO = 0xBE500000 (t8), UART_STAT = t8+0x18, UART_TXFULL = 0x20.
 *
 * emit_byte macro: send $a0's low byte? — no, avoids clobbering a0. Instead the byte
 * value is baked in via `li $v1, <ch>` at each call. Bounded wait then store.
 *
 * Register budget: a0-a2 consumed by the copy, a3 (ctx) preserved until step 4,
 * t0/t1 cache loops, t8/t9/v0/v1 UART markers + copy scratch. Explicit noreorder —
 * delay slots are manual.
 */

/* One-byte UART emit with a bounded TXFULL wait. t8 = FIFO base (preloaded).
 * Clobbers t9,v0,v1. Uses \@ (unique per expansion) to name its two local labels. */
	.macro TXB ch
	li	$t9, 100000
	.LtxW\@:
	lw	$v0, 0x18($t8)		/* UART_STAT */
	andi	$v0, $v0, 0x20		/* TXFULL bit */
	beqz	$v0, .LtxG\@		/* clear -> go send */
	addiu	$t9, $t9, -1
	bnez	$t9, .LtxW\@		/* else keep waiting (bounded) */
	nop
	.LtxG\@:
	li	$v1, \ch
	sw	$v1, 0($t8)		/* write byte -> TX FIFO */
	.endm

/* Emit "Bn\r\n" (n baked in). Clobbers t8,t9,v0,v1. */
	.macro MARKB n
	li	$t8, 0xBE500000
	TXB	'B'
	TXB	\n
	TXB	0x0D
	TXB	0x0A
	.endm

	.set push
	.set noreorder
	.global ApplyHandoff
	.global ApplyHandoffEnd
	.ent ApplyHandoff
ApplyHandoff:
	/* B1 — entry, before the 8MB kernel copy. */
	MARKB	'1'

	beq	$a2, $zero, 2f		/* len == 0 -> skip copy */
	nop
1:
	lw	$v0, 0($a0)
	sw	$v0, 0($a1)
	addiu	$a0, $a0, 4
	addiu	$a1, $a1, 4
	addiu	$a2, $a2, -4
	bne	$a2, $zero, 1b
	nop
2:
	/* B2 — copy done, before the cache maintenance. */
	MARKB	'2'

	/* D-cache writeback+invalidate all (uofw sysmem start.S:511-526) */
	mfc0	$t0, $16		/* Config */
	li	$t1, 2048
	ext	$t0, $t0, 6, 3		/* D-cache size field */
	sllv	$t1, $t1, $t0
	move	$t0, $zero
3:
	cache	0x14, 0($t0)		/* index writeback-invalidate D, both ways */
	addiu	$t0, $t0, 64
	bne	$t0, $t1, 3b
	cache	0x14, -64($t0)
	sync

	/* I-cache invalidate all (uofw sysmem start.S:1152-1170; IE already masked) */
	mfc0	$t0, $16		/* Config */
	li	$t1, 4096
	ext	$t0, $t0, 9, 3		/* I-cache size field */
	sllv	$t1, $t1, $t0
	mtc0	$zero, $28		/* TagLo */
	mtc0	$zero, $29		/* TagHi */
	move	$t0, $zero
4:
	addiu	$t0, $t0, 64
	cache	0x1, -64($t0)		/* index invalidate I */
	bne	$t0, $t1, 4b
	cache	0x3, -64($t0)

	/* B3 — caches flushed, about to restore context and jump. */
	MARKB	'3'

	/* Restore the save-time dispatcher-return context (layout: extras.S). All
	 * loads fetch the APPLIED image from RAM (dcache is empty after step 2). */
	lw	$s0, 0($a3)
	lw	$s1, 4($a3)
	lw	$s2, 8($a3)
	lw	$s3, 12($a3)
	lw	$s4, 16($a3)
	lw	$s5, 20($a3)
	lw	$s6, 24($a3)
	lw	$s7, 28($a3)
	lw	$fp, 32($a3)
	lw	$gp, 36($a3)
	lw	$sp, 40($a3)
	lw	$ra, 44($a3)
	lw	$v1, 48($a3)
	mtc0	$v1, $12		/* save-time CP0 Status (re-enables interrupts) */
	nop
	nop
	nop
	move	$v0, $zero		/* handler return value the dispatcher expects */
	jr	$ra			/* -> save-time sceKernelSysEventDispatch, post-handler */
	nop
	.end ApplyHandoff
ApplyHandoffEnd:
	.set pop
