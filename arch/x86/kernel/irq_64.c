/*
 *	Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 *
 * This file contains the lowest level x86_64-specific interrupt
 * entry and irq statistics code. All the remaining irq logic is
 * done by the generic kernel/irq/ code and in the
 * x86_64-specific irq controller code. (e.g. i8259.c and
 * io_apic.c.)
 */

#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <asm/io_apic.h>
#include <asm/idle.h>
#include <asm/smp.h>

atomic_t irq_err_count;

/*
 * 'what should we do if we get a hw irq event on an illegal vector'.
 * each architecture has to answer this themselves.
 */
void ack_bad_irq(unsigned int irq)
{
	printk(KERN_WARNING "unexpected IRQ trap at vector %02x\n", irq);
	/*
	 * Currently unexpected vectors happen only on SMP and APIC.
	 * We _must_ ack these because every local APIC has only N
	 * irq slots per priority level, and a 'hanging, unacked' IRQ
	 * holds up an irq slot - in excessive cases (when multiple
	 * unexpected vectors occur) that might lock up the APIC
	 * completely.
	 * But don't ack when the APIC is disabled. -AK
	 */
	if (!disable_apic)
		ack_APIC_irq();
}

#ifdef CONFIG_DEBUG_STACKOVERFLOW
/*
 * Probabilistic stack overflow check:
 *
 * Only check the stack in process context, because everything else
 * runs on the big interrupt stacks. Checking reliably is too expensive,
 * so we just check from interrupts.
 */
static inline void stack_overflow_check(struct pt_regs *regs)
{
	u64 curbase = (u64)task_stack_page(current);
	static unsigned long warned = -60*HZ;

	if (regs->sp >= curbase && regs->sp <= curbase + THREAD_SIZE &&
	    regs->sp <  curbase + sizeof(struct thread_info) + 128 &&
	    time_after(jiffies, warned + 60*HZ)) {
		printk("do_IRQ: %s near stack overflow (cur:%Lx,sp:%lx)\n",
		       current->comm, curbase, regs->sp);
		show_stack(NULL,NULL);
		warned = jiffies;
	}
}
#endif

/*
 * Generic, controller-independent functions:
 */

int show_interrupts(struct seq_file *p, void *v)
{
	int i, j;
	struct irqaction * action;
	unsigned long flags;
	unsigned int entries;
	struct irq_desc *desc = NULL;
	int tail = 0;

#ifdef CONFIG_HAVE_SPARSE_IRQ
	desc = (struct irq_desc *)v;
	entries = -1U;
	i = desc->irq;
	if (!desc->next)
		tail = 1;
#else
	entries = nr_irqs - 1;
	i = *(loff_t *) v;
	if (i == nr_irqs)
		tail = 1;
	else
		desc = irq_to_desc(i);
#endif

	if (i == 0) {
		seq_printf(p, "           ");
		for_each_online_cpu(j)
			seq_printf(p, "CPU%-8d",j);
		seq_putc(p, '\n');
	}

	if (i <= entries) {
		unsigned any_count = 0;

		spin_lock_irqsave(&desc->lock, flags);
#ifndef CONFIG_SMP
		any_count = kstat_irqs(i);
#else
		for_each_online_cpu(j)
			any_count |= kstat_irqs_cpu(i, j);
#endif
		action = desc->action;
		if (!action && !any_count)
			goto skip;
		seq_printf(p, "%3d: ", i);
#ifndef CONFIG_SMP
		seq_printf(p, "%10u ", kstat_irqs(i));
#else
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", kstat_irqs_cpu(i, j));
#endif
		seq_printf(p, " %8s", desc->chip->name);
		seq_printf(p, "-%-8s", desc->name);

		if (action) {
			seq_printf(p, "  %s", action->name);
			while ((action = action->next) != NULL)
				seq_printf(p, ", %s", action->name);
		}
		seq_putc(p, '\n');
skip:
		spin_unlock_irqrestore(&desc->lock, flags);
	}

	if (tail) {
		seq_printf(p, "NMI: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->__nmi_count);
		seq_printf(p, "  Non-maskable interrupts\n");
		seq_printf(p, "LOC: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->apic_timer_irqs);
		seq_printf(p, "  Local timer interrupts\n");
#ifdef CONFIG_SMP
		seq_printf(p, "RES: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->irq_resched_count);
		seq_printf(p, "  Rescheduling interrupts\n");
		seq_printf(p, "CAL: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->irq_call_count);
		seq_printf(p, "  Function call interrupts\n");
		seq_printf(p, "TLB: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->irq_tlb_count);
		seq_printf(p, "  TLB shootdowns\n");
#endif
#ifdef CONFIG_X86_MCE
		seq_printf(p, "TRM: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->irq_thermal_count);
		seq_printf(p, "  Thermal event interrupts\n");
		seq_printf(p, "THR: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->irq_threshold_count);
		seq_printf(p, "  Threshold APIC interrupts\n");
#endif
		seq_printf(p, "SPU: ");
		for_each_online_cpu(j)
			seq_printf(p, "%10u ", cpu_pda(j)->irq_spurious_count);
		seq_printf(p, "  Spurious interrupts\n");
		seq_printf(p, "ERR: %10u\n", atomic_read(&irq_err_count));
	}

	return 0;
}

/*
 * /proc/stat helpers
 */
u64 arch_irq_stat_cpu(unsigned int cpu)
{
	u64 sum = cpu_pda(cpu)->__nmi_count;

	sum += cpu_pda(cpu)->apic_timer_irqs;
#ifdef CONFIG_SMP
	sum += cpu_pda(cpu)->irq_resched_count;
	sum += cpu_pda(cpu)->irq_call_count;
	sum += cpu_pda(cpu)->irq_tlb_count;
#endif
#ifdef CONFIG_X86_MCE
	sum += cpu_pda(cpu)->irq_thermal_count;
	sum += cpu_pda(cpu)->irq_threshold_count;
#endif
	sum += cpu_pda(cpu)->irq_spurious_count;
	return sum;
}

u64 arch_irq_stat(void)
{
	return atomic_read(&irq_err_count);
}

/*
 * do_IRQ handles all normal device IRQ's (the special
 * SMP cross-CPU interrupts have their own specific
 * handlers).
 */
asmlinkage unsigned int do_IRQ(struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	struct irq_desc *desc;

	/* high bit used in ret_from_ code  */
	unsigned vector = ~regs->orig_ax;
	unsigned irq;

	exit_idle();
	irq_enter();
	irq = __get_cpu_var(vector_irq)[vector];

#ifdef CONFIG_DEBUG_STACKOVERFLOW
	stack_overflow_check(regs);
#endif

	desc = irq_to_desc(irq);
	if (likely(desc))
		generic_handle_irq_desc(irq, desc);
	else {
		if (!disable_apic)
			ack_APIC_irq();

		if (printk_ratelimit())
			printk(KERN_EMERG "%s: %d.%d No irq handler for vector\n",
				__func__, smp_processor_id(), vector);
	}

	irq_exit();

	set_irq_regs(old_regs);
	return 1;
}

#ifdef CONFIG_HOTPLUG_CPU
void fixup_irqs(cpumask_t map)
{
	unsigned int irq;
	static int warned;
	struct irq_desc *desc;

	for_each_irq_desc(irq, desc) {
		cpumask_t mask;
		int break_affinity = 0;
		int set_affinity = 1;

		if (irq == 2)
			continue;

		/* interrupt's are disabled at this point */
		spin_lock(&desc->lock);

		if (!irq_has_action(irq) ||
		    cpus_equal(desc->affinity, map)) {
			spin_unlock(&desc->lock);
			continue;
		}

		cpus_and(mask, desc->affinity, map);
		if (cpus_empty(mask)) {
			break_affinity = 1;
			mask = map;
		}

		if (desc->chip->mask)
			desc->chip->mask(irq);

		if (desc->chip->set_affinity)
			desc->chip->set_affinity(irq, mask);
		else if (!(warned++))
			set_affinity = 0;

		if (desc->chip->unmask)
			desc->chip->unmask(irq);

		spin_unlock(&desc->lock);

		if (break_affinity && set_affinity)
			printk("Broke affinity for irq %i\n", irq);
		else if (!set_affinity)
			printk("Cannot set affinity for irq %i\n", irq);
	}

	/* That doesn't seem sufficient.  Give it 1ms. */
	local_irq_enable();
	mdelay(1);
	local_irq_disable();
}
#endif

extern void call_softirq(void);

asmlinkage void do_softirq(void)
{
 	__u32 pending;
 	unsigned long flags;

 	if (in_interrupt())
 		return;

 	local_irq_save(flags);
 	pending = local_softirq_pending();
 	/* Switch to interrupt stack */
 	if (pending) {
		call_softirq();
		WARN_ON_ONCE(softirq_count());
	}
 	local_irq_restore(flags);
}
