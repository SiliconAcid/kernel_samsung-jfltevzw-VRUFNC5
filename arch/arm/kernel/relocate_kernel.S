/*
 * relocate_kernel.S - put the kernel image in place to boot
 */

#include <asm/kexec.h>

#ifdef CONFIG_KEXEC_HARDBOOT
#include <asm/memory.h>
#ifdef CONFIG_ARCH_MSM8960
#include <mach/msm_iomap.h>
#endif
#endif 
/*
 * collection of needed assem code for kexec
 */

/*
 *  included from:
 *  linux/arch/arm/lib/call_with_stack.S
 *  linux/arch/arm/mm/proc-v7.S
 *  linux/arch/arm/mm/cache-v7.S
 *
 *  Copyright (C) 2001 Deep Blue Solutions Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This is the "shell" of the ARMv7 processor support.
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/hwcap.h>
#include <asm/pgtable-hwdef.h>
#include <asm/pgtable.h>
#ifdef TIMA_EMUL_ENABLED
#include <asm/entry-macro-tima.S>
#endif
#include <asm/thread_info.h>
#include <asm/unwind.h>
#include <asm/kexec.h>
#include "../mm/proc-macros.S"

#if 0
/* SERIAL DEBUG */
	movw	r4, 0x0000
	movt	r4, 0x1664
	movw	r3, 0x0300
	movt	r3, 0x0000
	str	r3, [r4, #16]
	movw	r3, 0x0001
	movt	r3, 0x0000
	str	r3, [r4, #64]
	movw	r3, 0x0021			@ "!"
	movt	r3, 0x0000
	str	r3, [r4, #112]
/* END SERIAL DEBUG */
#endif

/*
 * void call_with_stack(void (*fn)(void *), void *arg, void *sp)
 *
 * Change the stack to that pointed at by sp, then invoke fn(arg) with
 * the new stack.
 */
ENTRY(kexec_call_with_stack)
	str	sp, [r2, #-4]!
	str	lr, [r2, #-4]!

	mov	sp, r2
	mov	r2, r0
	mov	r0, r1

	adr	lr, BSYM(1f)
	mov	pc, r2

	.align

1:	ldr	lr, [sp]
	ldr	sp, [sp, #4]
	mov	pc, lr
ENDPROC(kexec_call_with_stack)


/* linux/arch/arm/mm/proc-v7.S */
ENTRY(kexec_cpu_v7_proc_fin)
	mrc	p15, 0, r0, c1, c0, 0           @ ctrl register
	bic	r0, r0, #0x1000                 @ ...i............
	bic	r0, r0, #0x0006                 @ .............ca.
	mcr	p15, 0, r0, c1, c0, 0           @ disable caches
	mov	pc, lr
ENDPROC(kexec_cpu_v7_proc_fin)



/*
 *	cpu_v7_reset(loc)
 *
 *	Perform a soft reset of the system.  Put the CPU into the
 *	same state as it would be if it had been reset, and branch
 *	to what would be the reset vector.
 *
 *	- loc   - location to jump to for soft reset
 *
 *	This code must be executed using a flat identity mapping with
 *      caches disabled.
 */
	.align	5
	.pushsection	.idmap.text, "ax"
ENTRY(cpu_v7_reset)
	sub	pc, pc, #PAGE_OFFSET+4		@ go to physical addresses
	mrc	p15, 0, r1, c1, c0, 0		@ ctrl register
	bic	r1, r1, #0x1			@ ...............m
 THUMB(	bic	r1, r1, #1 << 30 )		@ SCTLR.TE (Thumb exceptions)
	mcr	p15, 0, r1, c1, c0, 0		@ disable MMU
	mcr     p15, 0, ip, c8, c7, 0           @ invalidate I & D,flush TLB
	mcr     p15, 0, ip, c7, c5, 6           @ flush BTC
	dsb
	isb
	mov	pc, r0
ENDPROC(cpu_v7_reset)
	.popsection

/* Exectution of this template in place is a bug: make it non-executable: */
       .section .rodata, "a"

	.align	3	/* not needed for this code, but keeps fncpy() happy */
	.arm
ENTRY(relocate_new_kernel)
	/* We get here when the MMU is in a transitional state.
	   Wait for the virtual address mapping to wear off before
	   overwriting the identity mappings (set up for the sake
	   of MMU disabling) with the previous mappings. */
	ldr	r0, =100
0:
	subs	r0, r0, #1
	beq	0b

	adr	r0, kexec_mmu_ents
	.rept 4
	ldr	r1, [r0], #4
	ldr	r2, [r0], #4
	str	r2, [r1], #4
	ldr	r2, [r0], #4
	str	r2, [r1], #4
	.endr

	/* continue with relocate_new_kernel */

	ldr	r0,kexec_indirection_page
	ldr	r1,kexec_start_address

	/*
	 * If there is no indirection page (we are doing crashdumps)
	 * skip any relocation.
	 */
	cmp	r0, #0
	beq	2f

0:	/* top, read another word for the indirection page */
	ldr	r3, [r0],#4

	/* Is it a destination page. Put destination address to r4 */
	tst	r3,#1,0
	beq	1f
	bic	r4,r3,#1
	b	0b
1:
	/* Is it an indirection page */
	tst	r3,#2,0
	beq	1f
	bic	r0,r3,#2
	b	0b
1:

	/* are we done ? */
	tst	r3,#4,0
	beq	1f
	b	2f

1:
	/* is it source ? */
	tst	r3,#8,0
	beq	0b
	bic r3,r3,#8
	mov r6,#1024
9:
	ldr r5,[r3],#4
	str r5,[r4],#4
	subs r6,r6,#1
	bne 9b
	b 0b

2:
#ifdef CONFIG_KEXEC_HARDBOOT
	ldr	r0, kexec_hardboot
	teq	r0, #0
	bne	hardboot
#endif

	/* Jump to relocated kernel */
	mov lr,r1
	mov r0,#0
	ldr r1,kexec_mach_type
	ldr r2,kexec_boot_atags
 ARM(	ret lr	)
 THUMB(	bx lr		)
ENDPROC(relocate_new_kernel) 
	.align

#ifdef CONFIG_KEXEC_HARDBOOT
hardboot: 
          /* Stash boot arguments in hardboot page:
           *  0: KEXEC_HB_PAGE_MAGIC
           *  4: kexec_start_address
           *  8: kexec_mach_type 
           * 12: kexec_boot_atags */
          ldr       r0, =KEXEC_HB_PAGE_ADDR
          str       r1, [r0, #4] 
          ldr       r1, kexec_mach_type
          str       r1, [r0, #8] 
          ldr       r1, kexec_boot_atags
          str       r1, [r0, #12] 
          ldr       r1, =KEXEC_HB_PAGE_MAGIC
          str       r1, [r0]
   
#if defined(CONFIG_ARCH_MSM8960) || defined(CONFIG_ARCH_APQ8064) || defined(CONFIG_ARCH_MSM8974) 
#if defined(CONFIG_ARCH_APQ8064) 
#define MSM_TLMM_PHYS APQ8064_TLMM_PHYS
#elif defined(CONFIG_ARCH_MSM8960)
#define MSM_TLMM_PHYS MSM8960_TLMM_PHYS
#endif
          /* Hard reset via PMIC, decompressor jumps to kernel. */
          ldr       r0, =MSM_TLMM_PHYS
          mov       r1, #0  
          str       r1, [r0, #0x820]  @ PSHOLD_CTL_SU  
   loop:       b       loop 
   #else 
   #error "No reboot method defined for hardboot." 
   #endif 
    
          .ltorg 
#endif

	.align

	.globl kexec_start_address
kexec_start_address:
	.long	0x0

	.globl kexec_indirection_page
kexec_indirection_page:
	.long	0x0
	
	.globl kexec_mmu_ents
kexec_mmu_ents:
	.space 4*12, 0

	.globl kexec_mach_type
kexec_mach_type:
	.long	0x0

	/* phy addr of the atags for the new kernel */
	.globl kexec_boot_atags
kexec_boot_atags:
	.long	0x0

#ifdef CONFIG_KEXEC_HARDBOOT
	.globl kexec_boot_atags_len
kexec_boot_atags_len:
	.long	0x0

	.globl kexec_kernel_len
kexec_kernel_len:
	.long	0x0

	.globl kexec_hardboot
kexec_hardboot:
	.long	0x0
#endif

relocate_new_kernel_end:

	.globl relocate_new_kernel_size
relocate_new_kernel_size:
	.long relocate_new_kernel_end - relocate_new_kernel


