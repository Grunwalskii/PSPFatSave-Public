/* ApplyHandoff — kernel-overwrite routine for LOAD path.
 * Position-independent (byte-copied to game RAM). Leaf routine (no stack).
 */
	.text
/* ApplyHandoff(src, dst, len, ctx) — one-way kernel apply for LOAD.
 * Never returns: copies len bytes, flushes caches, restores save-time context, jumps to save-time ra.
 * UART markers B1/B2/B3 track progress (hang diagnostic); clobber t8/t9/v0/v1 only.
 */

// Emit one byte to UART with bounded TX-full wait. Clobbers t9/v0/v1.
	.macro TXB ch
	li	$t9, 100000
	.LtxW\@:
	lw	$v0, 0x18($t8)
	andi	$v0, $v0, 0x20
	beqz	$v0, .LtxG\@
	addiu	$t9, $t9, -1
	bnez	$t9, .LtxW\@
	nop
	.LtxG\@:
	li	$v1, \ch
	sw	$v1, 0($t8)
	.endm

// Emit "Bn\r\n" checkpoint marker. Clobbers t8/t9/v0/v1.
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
	/* B1 — entry (before copy) */
	MARKB	'1'

	beq	$a2, $zero, 2f
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
	/* B2 — copy done (before cache flush) */
	MARKB	'2'

	/* Flush D-cache */
	mfc0	$t0, $16
	li	$t1, 2048
	ext	$t0, $t0, 6, 3
	sllv	$t1, $t1, $t0
	move	$t0, $zero
3:
	cache	0x14, 0($t0)
	addiu	$t0, $t0, 64
	bne	$t0, $t1, 3b
	cache	0x14, -64($t0)
	sync

	/* Flush I-cache */
	mfc0	$t0, $16
	li	$t1, 4096
	ext	$t0, $t0, 9, 3
	sllv	$t1, $t1, $t0
	mtc0	$zero, $28
	mtc0	$zero, $29
	move	$t0, $zero
4:
	addiu	$t0, $t0, 64
	cache	0x1, -64($t0)
	bne	$t0, $t1, 4b
	cache	0x3, -64($t0)

	/* B3 — caches flushed (about to restore/jump) */
	MARKB	'3'

	/* Restore save-time dispatcher context from staged image */
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
	mtc0	$v1, $12
	nop
	nop
	nop
	move	$v0, $zero
	jr	$ra
	nop
	.end ApplyHandoff
ApplyHandoffEnd:
	.set pop
