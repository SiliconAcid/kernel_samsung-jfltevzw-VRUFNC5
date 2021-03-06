/*
 * machine_kexec.c - handle transition of Linux booting another kernel
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kallsyms.h>
#include <linux/memblock.h>
#include <asm/pgtable.h>
#include <linux/of_fdt.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>
#include <asm/mach-types.h>
#include <asm/system.h>
#include <asm/smp_plat.h>
#include <asm/system_misc.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/kallsyms.h>
#include <asm/mmu_writeable.h>
#include <asm/outercache.h>
#include <asm/kexec.h>
#include <asm/fncpy.h>
#include <asm/cputype.h>
#define USE_SERIAL 0

extern void relocate_new_kernel(void);
extern const unsigned int relocate_new_kernel_size;

void (*kexec_gic_raise_softirq)(const struct cpumask *mask, unsigned int irq);
int (*kexec_msm_pm_wait_cpu_shutdown)(unsigned int cpu);

extern unsigned long kexec_start_address;
extern unsigned long kexec_indirection_page;
extern unsigned long kexec_mach_type;
extern unsigned long kexec_boot_atags;
void **my_syscall_table;

#ifdef CONFIG_KEXEC_HARDBOOT
extern unsigned long kexec_hardboot;
extern unsigned long kexec_boot_atags_len;
extern unsigned long kexec_kernel_len;
void (*kexec_hardboot_hook)(void);
#endif


void kexec_cpu_v7_proc_fin(void);

//static atomic_t waiting_for_crash_ipi;
static unsigned long dt_mem;

extern void kexec_call_with_stack(void (*fn)(void *), void *arg, void *sp);
typedef void (*phys_reset_t)(unsigned long);

static void kexec_idmap_add_pmd(pud_t *pud, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pmd_t *pmd = pmd_offset(pud, addr);

	addr = (addr & PMD_MASK) | prot;
	pmd[0] = __pmd(addr);
	addr += SECTION_SIZE;
	pmd[1] = __pmd(addr);
	flush_pmd_entry(pmd);
}

static void kexec_idmap_add_pud(pgd_t *pgd, unsigned long addr, unsigned long end,
	unsigned long prot)
{
	pud_t *pud = pud_offset(pgd, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		kexec_idmap_add_pmd(pud, addr, next, prot);
	} while (pud++, addr = next, addr != end);
}

void kexec_identity_mapping_add(pgd_t *pgd, unsigned long addr, unsigned long end)
{
	unsigned long prot, next;

	prot = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AF;
//	if (cpu_architecture() <= CPU_ARCH_ARMv5TEJ && !cpu_is_xscale())
//		prot |= PMD_BIT4;

	pgd += pgd_index(addr);
	do {
		next = pgd_addr_end(addr, end);
		kexec_idmap_add_pud(pgd, addr, next, prot);
		// HASH: flush
		local_flush_tlb_all();
	} while (pgd++, addr = next, addr != end);
	printk(KERN_EMERG "MKEXEC: end mappings end==0x%08lx\n", end);
}

/*
 * In order to soft-boot, we need to insert a 1:1 mapping in place of
 * the user-mode pages.  This will then ensure that we have predictable
 * results when turning the mmu off
 */

void kexec_identity_map(unsigned long phys_addr)
{
	pgd_t *pgd;
	pmd_t *pmd;

	int prot = PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | PMD_TYPE_SECT;
	unsigned long phys = phys_addr & PMD_MASK;

//	if (cpu_architecture() <= CPU_ARCH_ARMv5TEJ && !cpu_is_xscale())
//		prot |= PMD_BIT4;

	/*
	 * We need to access to user-mode page tables here. For kernel threads
	 * we don't have any user-mode mappings so we use the context that we
	 * "borrowed".
	 */

	pgd = pgd_offset(current->active_mm, phys);
	pmd = pmd_offset(pud_offset(pgd, phys), phys);
	pmd[0] = __pmd(phys | prot);
	pmd[1] = __pmd((phys + (1 << (PGDIR_SHIFT - 1))) | prot);

	flush_pmd_entry(pmd);

	local_flush_tlb_all();
}
/*
 * A temporary stack to use for CPU reset. This is static so that we
 * don't clobber it with the identity mapping. When running with this
 * stack, any references to the current task *will not work* so you
 * should really do as little as possible before jumping to your reset
 * code.
 */
static u64 soft_restart_stack[16];

#define MSM_DEBUG_UART_PHYS	0x16640000

#define UARTDM_MR2_OFFSET	0x4
#define UARTDM_CSR_OFFSET	0x8
#define UARTDM_SR_OFFSET	0x8
#define UARTDM_CR_OFFSET	0x10
#define UARTDM_ISR_OFFSET	0x14
#define UARTDM_NCF_TX_OFFSET	0x40
#define UARTDM_TF_OFFSET	0x70

#ifdef USE_SERIAL
#define SERIAL_WRITE(base, c)	while (!(*(volatile uint32_t *)(base + UARTDM_SR_OFFSET) & 0x08)) {}; \
				(*(volatile uint32_t *)(base + UARTDM_CR_OFFSET)) = 0x300; \
				(*(volatile uint32_t *)(base + UARTDM_NCF_TX_OFFSET)) = 0x1; \
				(*(volatile uint32_t *)(base + UARTDM_TF_OFFSET)) = c
#else
#define SERIAL_WRITE(base, c)	;
#endif


static void __soft_restart(void *addr)
{
	phys_reset_t phys_reset = (phys_reset_t)addr;

	/* Take out a flat memory mapping. */
	// HASH: We've already setup our static mapping
	// setup_mm_for_reboot();

	/* Clean and invalidate caches */
	SERIAL_WRITE(MSM_DEBUG_UART_PHYS, 'A');
	flush_cache_all();

	/* Turn off caching */
	SERIAL_WRITE(MSM_DEBUG_UART_PHYS, 'B');
//	cpu_proc_fin();
	kexec_cpu_v7_proc_fin();

	/* Push out any further dirty data, and ensure cache is empty */
	SERIAL_WRITE(MSM_DEBUG_UART_PHYS, 'C');
	flush_cache_all();

	/* Push out the dirty data from external caches */
	SERIAL_WRITE(MSM_DEBUG_UART_PHYS, 'D');
	outer_disable();

	/* Switch to the identity mapping. */
	SERIAL_WRITE(MSM_DEBUG_UART_PHYS, 'E');
	/* Switch to the identity mapping. */
//     phys_reset = (phys_reset_t)(unsigned long)virt_to_phys(cpu_reset);
//    phys_reset((unsigned long)addr);
	phys_reset(0);
	/* Should never get here. */
	BUG();
}

static u64 soft_restart_stack[16];
void soft_restart(unsigned long addr)
{
	u64 *stack = soft_restart_stack + ARRAY_SIZE(soft_restart_stack);

	/* Disable interrupts first */
	local_irq_disable();
	local_fiq_disable();

	/* Disable the L2 if we're the last man standing. */
	if (num_online_cpus() == 1) {
		printk(KERN_EMERG "MKEXEC: outer_flush_all\n");
		outer_flush_all();
		printk(KERN_EMERG "MKEXEC: outer_disable\n");
		outer_disable();
	}

	/* static mappings:
	 * UART
	 * relocate_kernel.S
	 */
	printk(KERN_EMERG "MKEXEC: kexec_identity_mapping_add 0x%08x-0x%08x\n",
		MSM_DEBUG_UART_PHYS, MSM_DEBUG_UART_PHYS + 0x1000);
	kexec_identity_mapping_add(current->active_mm->pgd, MSM_DEBUG_UART_PHYS, MSM_DEBUG_UART_PHYS + 0x1000);
	/* Clean and invalidate L1. */
	printk(KERN_EMERG "MKEXEC: flush_cache_all() \n");
	flush_cache_all();

	/* Flush the TLB. */
	printk(KERN_EMERG "MKEXEC: local_flush_tlb_all() \n");
	local_flush_tlb_all();

	printk(KERN_EMERG "MKEXEC: kexec_call_with_stack (kexec_call_with_stack=0x%8lx, __soft_reset=0x%8lx, addr=0x%8lx, stack=0x%8lx)\n", (unsigned long)kexec_call_with_stack, (unsigned long)__soft_restart, addr, (unsigned long)stack);
	/* Change to the new stack and continue with the reset. */
	printk(KERN_EMERG "MKEXEC: kexec_call_with_stack (va: 0x%08lx, __soft_reset: 0x%08lx, addr: 0x%08lx, stack: 0x%08lx)\n",
		(unsigned long)kexec_call_with_stack, (unsigned long)__soft_restart, addr, (unsigned long)stack);
	kexec_call_with_stack(__soft_restart, (void *)addr, (void *)stack);

	printk(KERN_EMERG "MKEXEC: ARRRRGGGGHH! NOT SUPPOSED TO BE HERE.\n");
	/* Should never get here. */
	BUG();
}

/*
 * Provide a dummy crash_notes definition while crash dump arrives to arm.
 * This prevents breakage of crash_notes attribute in kernel/ksysfs.c.
 */

int machine_kexec_prepare(struct kimage *image)
{
	int __init_memblock (*memblock_is_region_memory_new)(phys_addr_t, phys_addr_t) = (void *)kallsyms_lookup_name("memblock_is_region_memory");
	struct kexec_segment *current_segment;
	__be32 header;
	int i, err;

	/* No segment at default ATAGs address. try to locate
	 * a dtb using magic */
	for (i = 0; i < image->nr_segments; i++) {
		current_segment = &image->segment[i];

		err = memblock_is_region_memory_new(current_segment->mem,
				 current_segment->memsz);
		if (!err)
			return - EINVAL;

#ifdef CONFIG_KEXEC_HARDBOOT
		if(current_segment->mem == image->start)
			mem_text_write_kernel_word(&kexec_kernel_len, current_segment->memsz);

#endif

		err = get_user(header, (__be32*)current_segment->buf);
		if (err)
			return err;

		if (be32_to_cpu(header) == OF_DT_HEADER)
			kexec_boot_atags = current_segment->mem;
		{
			mem_text_write_kernel_word(&kexec_boot_atags, current_segment->mem);

#ifdef CONFIG_KEXEC_HARDBOOT
			mem_text_write_kernel_word(&kexec_boot_atags_len, current_segment->memsz);
#endif
		}
	}
	return 0;
}
EXPORT_SYMBOL(machine_kexec_prepare);

void machine_kexec_cleanup(struct kimage *image)
{
}
EXPORT_SYMBOL(machine_kexec_cleanup);

enum ipi_msg_type {
	IPI_CPU_START = 1,
	IPI_TIMER = 2,
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CALL_FUNC_SINGLE,
	IPI_CPU_STOP,
	IPI_CPU_BACKTRACE,
};

static void kexec_smp_kill_cpus(cpumask_t *mask)
{
	unsigned int cpu;
	for_each_cpu(cpu, mask) {
		kexec_msm_pm_wait_cpu_shutdown(cpu);
	}
}

void machine_shutdown(void)
{
	unsigned long timeout;
	struct cpumask mask;

	kexec_gic_raise_softirq = (void *)kallsyms_lookup_name("gic_raise_softirq");
	kexec_msm_pm_wait_cpu_shutdown = (void *)kallsyms_lookup_name("msm_pm_wait_cpu_shutdown");
	if (!kexec_msm_pm_wait_cpu_shutdown) {
		printk(KERN_EMERG "MKEXEC: msm_pm_wait_cpu_shutdown NOT FOUND!\n");
		return;
	}

	if (kexec_gic_raise_softirq) {
		printk(KERN_EMERG "MKEXEC: found gic_raise_softirq: %p\n", kexec_gic_raise_softirq);

		cpumask_copy(&mask, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &mask);
		if (!cpumask_empty(&mask)) {
			printk(KERN_EMERG "MKEXEC: Sending STOP to extra CPUs ...\n");
			kexec_gic_raise_softirq(&mask, IPI_CPU_STOP);
		}

		/* Wait up to five seconds for other CPUs to stop */
		timeout = USEC_PER_SEC;
		printk(KERN_EMERG "MKEXEC: waiting for CPUs ...(%lu)\n", timeout);
		while (num_online_cpus() > 1 && timeout--)
			udelay(1);

		if (num_online_cpus() > 1)
			pr_warning("MKEXEC: SMP: failed to stop secondary CPUs\n");

		kexec_smp_kill_cpus(&mask);
	}
	else {
		pr_warning("MKEXEC: SMP: failed to stop secondary CPUs\n");
	}
}
EXPORT_SYMBOL(machine_shutdown);

void machine_crash_nonpanic_core(void *unused)
{
#if 0
	struct pt_regs regs;

	crash_setup_regs(&regs, NULL);
	printk(KERN_DEBUG "CPU %u will stop doing anything useful since another CPU has crashed\n",
	       smp_processor_id());
	crash_save_cpu(&regs, smp_processor_id());
	flush_cache_all();

	set_cpu_online(smp_processor_id(), false);
	atomic_dec(&waiting_for_crash_ipi);
	while (1)
		cpu_relax();
#endif
}

#if 0
static void machine_kexec_mask_interrupts(void)
{
	unsigned int i;
	struct irq_desc *desc;

	for_each_irq_desc(i, desc) {
		struct irq_chip *chip;

		chip = irq_desc_get_chip(desc);
		if (!chip)
			continue;

		if (chip->irq_eoi && irqd_irq_inprogress(&desc->irq_data))
			chip->irq_eoi(&desc->irq_data);

		if (chip->irq_mask)
			chip->irq_mask(&desc->irq_data);

		if (chip->irq_disable && !irqd_irq_disabled(&desc->irq_data))
			chip->irq_disable(&desc->irq_data);
	}
}
#endif

void machine_crash_shutdown(struct pt_regs *regs)
{
#if 0
	unsigned long msecs;

	local_irq_disable();

	atomic_set(&waiting_for_crash_ipi, num_online_cpus() - 1);
	smp_call_function(machine_crash_nonpanic_core, NULL, false);
	msecs = 1000; /* Wait at most a second for the other cpus to stop */
	while ((atomic_read(&waiting_for_crash_ipi) > 0) && msecs) {
		mdelay(1);
		msecs--;
	}
	if (atomic_read(&waiting_for_crash_ipi) > 0)
		printk(KERN_WARNING "Non-crashing CPUs did not react to IPI\n");

	crash_save_cpu(regs, smp_processor_id());
	machine_kexec_mask_interrupts();

	printk(KERN_INFO "Loading crashdump kernel...\n");
#endif
}

/*
 * Function pointer to optional machine-specific reinitialization
 */
void (*kexec_reinit)(void);
/*
 * Instead of patching the kernel .text (which might be Read-only by
 * CONFIG_DEBUG_RODATA), patch the already-copied template.
 */
static void patch_boot_parameters(char *copied_template, struct kimage *image) {

       unsigned long page_list = image->head & PAGE_MASK;
       uintptr_t base = (uintptr_t)relocate_new_kernel & ~(uintptr_t)1;
       uintptr_t start_addr_offset = (uintptr_t)&kexec_start_address - base;
       uintptr_t indir_page_offset = (uintptr_t)&kexec_indirection_page - base;
       uintptr_t mach_type_offset = (uintptr_t)&kexec_mach_type - base;
       uintptr_t boot_atags_offset = (uintptr_t)&kexec_boot_atags - base;

#define patch_value(offset,res) \
       *(unsigned long *)(copied_template + (offset)) = (res)

       patch_value(start_addr_offset, image->start);
       patch_value(indir_page_offset, page_list);
       patch_value(mach_type_offset, machine_arch_type);

       if (!dt_mem)
               patch_value(boot_atags_offset, image->start -
                                KEXEC_ARM_ZIMAGE_OFFSET +
                                KEXEC_ARM_ATAGS_OFFSET);
       else
               patch_value(boot_atags_offset, dt_mem);
}

void machine_kexec(struct kimage *image)
{

	unsigned long page_list = image->head & PAGE_MASK;
	unsigned long reboot_code_buffer_phys;
	unsigned long reboot_entry = (unsigned long)relocate_new_kernel;
	unsigned long reboot_entry_phys;
	unsigned long cpu_reset_phys;
	void *reboot_code_buffer;
      unsigned long v2p_offset;
      void *entry_point; 
      
	page_list = image->head & PAGE_MASK;

	
      if (num_online_cpus() > 1) {
		pr_err("kexec: error: multiple CPUs still online\n");
		return;
	}
	
	/* Disable preemption */
	preempt_disable();
	reboot_code_buffer = page_address(image->control_code_page);
      v2p_offset = (page_to_pfn(image->control_code_page) << PAGE_SHIFT)
                - (unsigned long)reboot_code_buffer; 

	printk(KERN_EMERG "MKEXEC: va: %08x\n", (unsigned int)reboot_code_buffer);


#ifdef CONFIG_KEXEC_HARDBOOT
	/*kexec_hardboot = image->hardboot;*/
	mem_text_write_kernel_word(&kexec_hardboot, image->hardboot);
#endif

      /* Identity map the code which turns off the mmu (cpu_reset) and
	   the code which will be executed immediately afterwards
	   (relocate_new_kernel).
	   Store the old entries so they can be restored. */
	/* cpu_reset cannot be used directly when MULTI_CPU is true, see
	   cpu-multi32.h, instead processor.reset will have to be used */

	/* Identity map the code which turns off the mmu (cpu_reset) and
	   the code which will be executed immediately afterwards
	   (relocate_new_kernel).
	   Store the old entries so they can be restored. */
	/* cpu_reset cannot be used directly when MULTI_CPU is true, see
	   cpu-multi32.h, instead processor.reset will have to be used */
	   
	   
#ifdef MULTI_CPU
	cpu_reset_phys = virt_to_phys(processor.reset);
#else
	cpu_reset_phys = virt_to_phys(cpu_reset);
#endif


/*	kexec_identity_mapping_add(current->active_mm->pgd, reboot_code_buffer_phys,
			     ALIGN(reboot_code_buffer_phys, PGDIR_SIZE)
			     + PGDIR_SIZE,);
*/


		
	/* Prepare parameters for reboot_code_buffer*/
      patch_boot_parameters(reboot_code_buffer, image);
	kexec_start_address = image->start;
	kexec_indirection_page = page_list;
	kexec_mach_type = machine_arch_type;
	if (!kexec_boot_atags)
	kexec_boot_atags = image->start - KEXEC_ARM_ZIMAGE_OFFSET + KEXEC_ARM_ATAGS_OFFSET;
	printk(KERN_EMERG "MKEXEC: kexec_start_address: %08lx\n", kexec_start_address);
	printk(KERN_EMERG "MKEXEC: kexec_indirection_page: %08lx\n", kexec_indirection_page);
	printk(KERN_EMERG "MKEXEC: kexec_mach_type: %08lx\n", kexec_mach_type);
	printk(KERN_EMERG "MKEXEC: kexec_boot_atags: %08lx\n", kexec_boot_atags);

	/* copy our kernel relocation code to the control code page */
	kexec_identity_mapping_add(current->active_mm->pgd, cpu_reset_phys,
			     ALIGN(cpu_reset_phys, PGDIR_SIZE)+PGDIR_SIZE);
	printk(KERN_EMERG "MKEXEC: copy relocate code: addr=0x%08lx, len==%d\n", (unsigned long)reboot_entry, relocate_new_kernel_size);
	reboot_entry = fncpy (reboot_code_buffer,
			 reboot_entry,
			 relocate_new_kernel_size);
      entry_point = fncpy(reboot_code_buffer,
                   &relocate_new_kernel, relocate_new_kernel_size); 
	reboot_entry_phys = (unsigned long)reboot_entry +
		(reboot_code_buffer_phys - (unsigned long)reboot_code_buffer);

       /* Prepare parameters for reboot_code_buffer*/
        patch_boot_parameters(reboot_code_buffer, image);
kexec_start_address = image->start;
	kexec_indirection_page = page_list;
	kexec_mach_type = machine_arch_type;
	if (!kexec_boot_atags)
		kexec_boot_atags = image->start - KEXEC_ARM_ZIMAGE_OFFSET + KEXEC_ARM_ATAGS_OFFSET;
	
	/* copy our kernel relocation code to the control code page */
	entry_point = fncpy(reboot_code_buffer,
			    &relocate_new_kernel, relocate_new_kernel_size);

	printk(KERN_EMERG "MKEXEC: entry_point\n");

	printk(KERN_EMERG "MKEXEC: kexec_reinit\n");
	if (kexec_reinit)
		kexec_reinit();
	//local_irq_disable();
	//local_fiq_disable();

	//outer_flush_all();
	//outer_disable();
	printk(KERN_EMERG "MKEXEC: soft_restart\n");
	soft_restart((unsigned long)entry_point + v2p_offset); 
}
EXPORT_SYMBOL(machine_kexec);

static int __init arm_kexec_init(void)
{
	void (*set_cpu_online_ptr)(unsigned int cpu, bool online) = (void *)kallsyms_lookup_name("set_cpu_online");
	void (*set_cpu_present_ptr)(unsigned int cpu, bool present) = (void *)kallsyms_lookup_name("set_cpu_present");
	void (*set_cpu_possible_ptr)(unsigned int cpu, bool possible) = (void *)kallsyms_lookup_name("set_cpu_possible");
	int (*disable_nonboot_cpus)(void) = (void *)kallsyms_lookup_name("disable_nonboot_cpus");
	int nbcval = 0;

	nbcval = disable_nonboot_cpus();
	if (nbcval < 0)
		printk(KERN_INFO "MKEXEC: !!!WARNING!!! disable_nonboot_cpus have FAILED!\n \
				  Continuing to boot anyway: something can go wrong!\n");

	set_cpu_online_ptr(1, false);
	set_cpu_present_ptr(1, false);
	set_cpu_possible_ptr(1, false);

	set_cpu_online_ptr(2, false);
	set_cpu_present_ptr(2, false);
	set_cpu_possible_ptr(2, false);

	set_cpu_online_ptr(2, false);
	set_cpu_present_ptr(2, false);
	set_cpu_possible_ptr(2, false);

	return 0;
}

static void __exit arm_kexec_exit(void)
{
}

module_init(arm_kexec_init);
module_exit(arm_kexec_exit);

MODULE_LICENSE("GPL");
