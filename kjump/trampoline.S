#include <asm/asm.h>
#include <asm/regdef.h>
#include <asm/mipsregs.h>
#include <asm/addrspace.h>

	.text
	.set noreorder
	.set noat
	.globl kjump_trampoline
	.ent kjump_trampoline
kjump_trampoline:
	/* 0. Blackbox Milestone 4: Trampoline Enter */
	lui	k1, 0xABF0
	lw	k0, 0(k1)
	lui	t0, 0x5043
	ori	t0, t0, 0x4842		/* 0x50434842 ("PCHB") */
	bne	k0, t0, 1f
	nop

	li	k0, 4			/* MS_TRAMPOLINE_ENTER */
	sw	k0, 0x08(k1)		/* bb->current_milestone */
	lui	t0, 0xA001
	lw	k0, 0x0048(t0)		/* SYS_xtal_in_cnt */
	sw	k0, 0x2C(k1)		/* bb->t_trampoline_enter */
	lw	k0, 0xFD00(t0)		/* SYS_watchdog_counter */
	sw	k0, 0x10(k1)		/* bb->wdt_ticks_remaining */
	sw	sp, 0x50(k1)		/* bb->reg_sp */
	sw	ra, 0x54(k1)		/* bb->reg_ra */
	sync
1:
	li	s0, 0xA4000000		/* destination base in KSEG1 (physical 0x04000000) */

	/* 1. Copy pages from table 0 */
	beqz	a2, copy_table1
	nop

table0_loop:
	lw	t0, 0(a0)		/* t0 = source page address in KSEG1 DRAM */
	addiu	a0, a0, 4		/* advance table 0 pointer */
	li	t1, 1024		/* 1024 words = 4096 bytes */

word0_loop:
	lw	t2, 0(t0)
	sw	t2, 0(s0)
	addiu	t0, t0, 4
	addiu	s0, s0, 4
	addiu	t1, t1, -1
	bnez	t1, word0_loop
	nop

	addiu	a2, a2, -1
	bnez	a2, table0_loop
	nop

copy_table1:
	/* 2. Copy pages from table 1 */
	beqz	a3, copy_done
	nop

table1_loop:
	lw	t0, 0(a1)		/* t0 = source page address in KSEG1 DRAM */
	addiu	a1, a1, 4		/* advance table 1 pointer */
	li	t1, 1024		/* 1024 words = 4096 bytes */

word1_loop:
	lw	t2, 0(t0)
	sw	t2, 0(s0)
	addiu	t0, t0, 4
	addiu	s0, s0, 4
	addiu	t1, t1, -1
	bnez	t1, word1_loop
	nop

	addiu	a3, a3, -1
	bnez	a3, table1_loop
	nop

copy_done:
	sync
	ehb

	/* Blackbox Milestone 6: Copy Done */
	lui	k1, 0xABF0
	lw	k0, 0(k1)
	lui	t0, 0x5043
	ori	t0, t0, 0x4842
	bne	k0, t0, 2f
	nop
	li	k0, 6			/* MS_COPY_DONE */
	sw	k0, 0x08(k1)
	lui	t0, 0xA001
	lw	k0, 0x0048(t0)
	sw	k0, 0x34(k1)		/* bb->t_copy_done */
	lw	k0, 0xFD00(t0)
	sw	k0, 0x10(k1)
	sync
2:

	/* Invalidate entire L1 Instruction Cache */
	mtc0	zero, $28		/* CP0 TagLo = 0 */
	mtc0	zero, $29		/* CP0 TagHi = 0 */
	ehb

	li	t0, 0x80000000		/* KSEG0 start */
	li	t1, 1024		/* 1024 lines of 32 bytes = 32KB */
flush_icache:
	cache	0x08, 0(t0)		/* Index_Store_Tag_I */
	addiu	t0, t0, 32
	addiu	t1, t1, -1
	bnez	t1, flush_icache
	nop

	sync
	ehb

	/* Blackbox Milestone 5: Cache Flushed */
	lui	k1, 0xABF0
	lw	k0, 0(k1)
	lui	t0, 0x5043
	ori	t0, t0, 0x4842
	bne	k0, t0, 3f
	nop
	li	k0, 5			/* MS_CACHE_FLUSHED */
	sw	k0, 0x08(k1)
	lui	t0, 0xA001
	lw	k0, 0x0048(t0)
	sw	k0, 0x30(k1)		/* bb->t_cache_flushed */
	lw	k0, 0xFD00(t0)
	sw	k0, 0x10(k1)
	sync
3:

	/* ARM HARDWARE WATCHDOG (30 SECONDS) DIRECTLY IN ASSEMBLY VIA 32-BIT SW */
	li	t0, 0xA001FD04		/* WD_CONFIG register */
	li	t1, 0x00000001		/* XTAL_IN=1, DISABLE=0 */
	sw	t1, 0(t0)
	li	t0, 0xA001FD00		/* WD_COUNTER register */
	li	t1, 0x60EE1000		/* 30s * 27MHz = 810,000,000 ticks */
	sw	t1, 0(t0)
	sync
	ehb

	/* Blackbox Milestone 7: Watchdog armed, jumping to target */
	lui	k1, 0xABF0
	lw	k0, 0(k1)
	lui	t0, 0x5043
	ori	t0, t0, 0x4842
	bne	k0, t0, 4f
	nop
	li	k0, 7			/* MS_JUMP_TAKEN */
	sw	k0, 0x08(k1)
	lui	t0, 0xA001
	lw	k0, 0x0048(t0)
	sw	k0, 0x38(k1)		/* bb->t_jump_taken */
	lw	k0, 0xFD00(t0)
	sw	k0, 0x10(k1)
	li	t9, 0xA4000400
	sw	t9, 0x58(k1)		/* bb->reg_t9 */
	sync
4:

	/* Clear firmware argument registers */
	move	a0, zero
	move	a1, zero
	move	a2, zero
	move	a3, zero

	/* JUMP DIRECTLY INTO NEW LINUX 4.9 KERNEL ENTRY POINT IN KSEG1 */
	li	t9, 0xA4000400
	jr	t9
	nop

	.end kjump_trampoline

	.globl kjump_trampoline_end
kjump_trampoline_end:
	nop
