/*
 * Machine specific setup for xen
 *
 * Jeremy Fitzhardinge <jeremy@xensource.com>, XenSource Inc, 2007
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pm.h>

#include <asm/elf.h>
#include <asm/vdso.h>
#include <asm/e820.h>
#include <asm/setup.h>
#include <asm/acpi.h>
#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>

#include <xen/page.h>
#include <xen/interface/callback.h>
#include <xen/interface/physdev.h>
#include <xen/features.h>

#include "xen-ops.h"
#include "vdso.h"

/* These are code, but not functions.  Defined in entry.S */
extern const char xen_hypervisor_callback[];
extern const char xen_failsafe_callback[];


/**
 * machine_specific_memory_setup - Hook for machine specific memory setup.
 **/

char * __init xen_memory_setup(void)
{
	unsigned long max_pfn = xen_start_info->nr_pages;

	max_pfn = min(MAX_DOMAIN_PAGES, max_pfn);

	e820.nr_map = 0;

	e820_add_region(0, PFN_PHYS(max_pfn), E820_RAM);

	/*
	 * Even though this is normal, usable memory under Xen, reserve
	 * ISA memory anyway because too many things think they can poke
	 * about in there.
	 */
	e820_add_region(ISA_START_ADDRESS, ISA_END_ADDRESS - ISA_START_ADDRESS,
			E820_RESERVED);

	/*
	 * Reserve Xen bits:
	 *  - mfn_list
	 *  - xen_start_info
	 * See comment above "struct start_info" in <xen/interface/xen.h>
	 */
	e820_add_region(__pa(xen_start_info->mfn_list),
			xen_start_info->pt_base - xen_start_info->mfn_list,
			E820_RESERVED);

	sanitize_e820_map(e820.map, ARRAY_SIZE(e820.map), &e820.nr_map);

	return "Xen";
}

static void xen_idle(void)
{
	local_irq_disable();

	if (need_resched())
		local_irq_enable();
	else {
		current_thread_info()->status &= ~TS_POLLING;
		smp_mb__after_clear_bit();
		safe_halt();
		current_thread_info()->status |= TS_POLLING;
	}
}

/*
 * Set the bit indicating "nosegneg" library variants should be used.
 */
static void __init fiddle_vdso(void)
{
#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
	extern const char vdso32_default_start;
	u32 *mask = VDSO32_SYMBOL(&vdso32_default_start, NOTE_MASK);
	*mask |= 1 << VDSO_NOTE_NONEGSEG_BIT;
#endif
}

static __cpuinit int register_callback(unsigned type, const void *func)
{
	struct callback_register callback = {
		.type = type,
		.address = XEN_CALLBACK(__KERNEL_CS, func),
		.flags = CALLBACKF_mask_events,
	};

	return HYPERVISOR_callback_op(CALLBACKOP_register, &callback);
}

void __cpuinit xen_enable_sysenter(void)
{
	int cpu = smp_processor_id();
	extern void xen_sysenter_target(void);
	int ret;

#ifdef CONFIG_X86_32
	if (!boot_cpu_has(X86_FEATURE_SEP)) {
		return;
	}
#else
	if (boot_cpu_data.x86_vendor != X86_VENDOR_INTEL &&
	    boot_cpu_data.x86_vendor != X86_VENDOR_CENTAUR) {
		return;
	}
#endif

	ret = register_callback(CALLBACKTYPE_sysenter, xen_sysenter_target);
	if(ret != 0) {
		clear_cpu_cap(&cpu_data(cpu), X86_FEATURE_SEP);
		clear_cpu_cap(&boot_cpu_data, X86_FEATURE_SEP);
	}
}

void __cpuinit xen_enable_syscall(void)
{
#ifdef CONFIG_X86_64
	int cpu = smp_processor_id();
	int ret;
	extern void xen_syscall_target(void);
	extern void xen_syscall32_target(void);

	ret = register_callback(CALLBACKTYPE_syscall, xen_syscall_target);
	if (ret != 0) {
		printk("failed to set syscall: %d\n", ret);
		clear_cpu_cap(&cpu_data(cpu), X86_FEATURE_SYSCALL);
		clear_cpu_cap(&boot_cpu_data, X86_FEATURE_SYSCALL);
	} else {
		ret = register_callback(CALLBACKTYPE_syscall32,
					xen_syscall32_target);
		if (ret != 0)
			printk("failed to set 32-bit syscall: %d\n", ret);
	}
#endif /* CONFIG_X86_64 */
}

void __init xen_arch_setup(void)
{
	struct physdev_set_iopl set_iopl;
	int rc;

	HYPERVISOR_vm_assist(VMASST_CMD_enable, VMASST_TYPE_4gb_segments);
	HYPERVISOR_vm_assist(VMASST_CMD_enable, VMASST_TYPE_writable_pagetables);

	if (!xen_feature(XENFEAT_auto_translated_physmap))
		HYPERVISOR_vm_assist(VMASST_CMD_enable, VMASST_TYPE_pae_extended_cr3);

	if (register_callback(CALLBACKTYPE_event, xen_hypervisor_callback) ||
	    register_callback(CALLBACKTYPE_failsafe, xen_failsafe_callback))
		BUG();

	xen_enable_sysenter();
	xen_enable_syscall();

	set_iopl.iopl = 1;
	rc = HYPERVISOR_physdev_op(PHYSDEVOP_set_iopl, &set_iopl);
	if (rc != 0)
		printk(KERN_INFO "physdev_op failed %d\n", rc);

#ifdef CONFIG_ACPI
	if (!(xen_start_info->flags & SIF_INITDOMAIN)) {
		printk(KERN_INFO "ACPI in unprivileged domain disabled\n");
		disable_acpi();
	}
#endif

	memcpy(boot_command_line, xen_start_info->cmd_line,
	       MAX_GUEST_CMDLINE > COMMAND_LINE_SIZE ?
	       COMMAND_LINE_SIZE : MAX_GUEST_CMDLINE);

	pm_idle = xen_idle;

	paravirt_disable_iospace();

	fiddle_vdso();
}
