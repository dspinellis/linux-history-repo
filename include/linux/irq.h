#ifndef _LINUX_IRQ_H
#define _LINUX_IRQ_H

/*
 * Please do not include this file in generic code.  There is currently
 * no requirement for any architecture to implement anything held
 * within this file.
 *
 * Thanks. --rmk
 */

#include <linux/smp.h>

#ifndef CONFIG_S390

#include <linux/linkage.h>
#include <linux/cache.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/irqreturn.h>
#include <linux/irqnr.h>
#include <linux/errno.h>

#include <asm/irq.h>
#include <asm/ptrace.h>
#include <asm/irq_regs.h>

struct irq_desc;
typedef	void (*irq_flow_handler_t)(unsigned int irq,
					    struct irq_desc *desc);


/*
 * IRQ line status.
 *
 * Bits 0-7 are reserved for the IRQF_* bits in linux/interrupt.h
 *
 * IRQ types
 */
#define IRQ_TYPE_NONE		0x00000000	/* Default, unspecified type */
#define IRQ_TYPE_EDGE_RISING	0x00000001	/* Edge rising type */
#define IRQ_TYPE_EDGE_FALLING	0x00000002	/* Edge falling type */
#define IRQ_TYPE_EDGE_BOTH (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING)
#define IRQ_TYPE_LEVEL_HIGH	0x00000004	/* Level high type */
#define IRQ_TYPE_LEVEL_LOW	0x00000008	/* Level low type */
#define IRQ_TYPE_SENSE_MASK	0x0000000f	/* Mask of the above */
#define IRQ_TYPE_PROBE		0x00000010	/* Probing in progress */

/* Internal flags */
#define IRQ_INPROGRESS		0x00000100	/* IRQ handler active - do not enter! */
#define IRQ_DISABLED		0x00000200	/* IRQ disabled - do not enter! */
#define IRQ_PENDING		0x00000400	/* IRQ pending - replay on enable */
#define IRQ_REPLAY		0x00000800	/* IRQ has been replayed but not acked yet */
#define IRQ_AUTODETECT		0x00001000	/* IRQ is being autodetected */
#define IRQ_WAITING		0x00002000	/* IRQ not yet seen - for autodetection */
#define IRQ_LEVEL		0x00004000	/* IRQ level triggered */
#define IRQ_MASKED		0x00008000	/* IRQ masked - shouldn't be seen again */
#define IRQ_PER_CPU		0x00010000	/* IRQ is per CPU */
#define IRQ_NOPROBE		0x00020000	/* IRQ is not valid for probing */
#define IRQ_NOREQUEST		0x00040000	/* IRQ cannot be requested */
#define IRQ_NOAUTOEN		0x00080000	/* IRQ will not be enabled on request irq */
#define IRQ_WAKEUP		0x00100000	/* IRQ triggers system wakeup */
#define IRQ_MOVE_PENDING	0x00200000	/* need to re-target IRQ destination */
#define IRQ_NO_BALANCING	0x00400000	/* IRQ is excluded from balancing */
#define IRQ_SPURIOUS_DISABLED	0x00800000	/* IRQ was disabled by the spurious trap */
#define IRQ_MOVE_PCNTXT		0x01000000	/* IRQ migration from process context */
#define IRQ_AFFINITY_SET	0x02000000	/* IRQ affinity was set from userspace*/

#ifdef CONFIG_IRQ_PER_CPU
# define CHECK_IRQ_PER_CPU(var) ((var) & IRQ_PER_CPU)
# define IRQ_NO_BALANCING_MASK	(IRQ_PER_CPU | IRQ_NO_BALANCING)
#else
# define CHECK_IRQ_PER_CPU(var) 0
# define IRQ_NO_BALANCING_MASK	IRQ_NO_BALANCING
#endif

struct proc_dir_entry;
struct msi_desc;

/**
 * struct irq_chip - hardware interrupt chip descriptor
 *
 * @name:		name for /proc/interrupts
 * @startup:		start up the interrupt (defaults to ->enable if NULL)
 * @shutdown:		shut down the interrupt (defaults to ->disable if NULL)
 * @enable:		enable the interrupt (defaults to chip->unmask if NULL)
 * @disable:		disable the interrupt (defaults to chip->mask if NULL)
 * @ack:		start of a new interrupt
 * @mask:		mask an interrupt source
 * @mask_ack:		ack and mask an interrupt source
 * @unmask:		unmask an interrupt source
 * @eoi:		end of interrupt - chip level
 * @end:		end of interrupt - flow level
 * @set_affinity:	set the CPU affinity on SMP machines
 * @retrigger:		resend an IRQ to the CPU
 * @set_type:		set the flow type (IRQ_TYPE_LEVEL/etc.) of an IRQ
 * @set_wake:		enable/disable power-management wake-on of an IRQ
 *
 * @release:		release function solely used by UML
 * @typename:		obsoleted by name, kept as migration helper
 */
struct irq_chip {
	const char	*name;
	unsigned int	(*startup)(unsigned int irq);
	void		(*shutdown)(unsigned int irq);
	void		(*enable)(unsigned int irq);
	void		(*disable)(unsigned int irq);

	void		(*ack)(unsigned int irq);
	void		(*mask)(unsigned int irq);
	void		(*mask_ack)(unsigned int irq);
	void		(*unmask)(unsigned int irq);
	void		(*eoi)(unsigned int irq);

	void		(*end)(unsigned int irq);
	void		(*set_affinity)(unsigned int irq, cpumask_t dest);
	int		(*retrigger)(unsigned int irq);
	int		(*set_type)(unsigned int irq, unsigned int flow_type);
	int		(*set_wake)(unsigned int irq, unsigned int on);

	/* Currently used only by UML, might disappear one day.*/
#ifdef CONFIG_IRQ_RELEASE_METHOD
	void		(*release)(unsigned int irq, void *dev_id);
#endif
	/*
	 * For compatibility, ->typename is copied into ->name.
	 * Will disappear.
	 */
	const char	*typename;
};

struct timer_rand_state;
struct irq_2_iommu;
/**
 * struct irq_desc - interrupt descriptor
 * @irq:		interrupt number for this descriptor
 * @timer_rand_state:	pointer to timer rand state struct
 * @kstat_irqs:		irq stats per cpu
 * @irq_2_iommu:	iommu with this irq
 * @handle_irq:		highlevel irq-events handler [if NULL, __do_IRQ()]
 * @chip:		low level interrupt hardware access
 * @msi_desc:		MSI descriptor
 * @handler_data:	per-IRQ data for the irq_chip methods
 * @chip_data:		platform-specific per-chip private data for the chip
 *			methods, to allow shared chip implementations
 * @action:		the irq action chain
 * @status:		status information
 * @depth:		disable-depth, for nested irq_disable() calls
 * @wake_depth:		enable depth, for multiple set_irq_wake() callers
 * @irq_count:		stats field to detect stalled irqs
 * @last_unhandled:	aging timer for unhandled count
 * @irqs_unhandled:	stats field for spurious unhandled interrupts
 * @lock:		locking for SMP
 * @affinity:		IRQ affinity on SMP
 * @cpu:		cpu index useful for balancing
 * @pending_mask:	pending rebalanced interrupts
 * @dir:		/proc/irq/ procfs entry
 * @name:		flow handler name for /proc/interrupts output
 */
struct irq_desc {
	unsigned int		irq;
#ifdef CONFIG_SPARSE_IRQ
	struct timer_rand_state *timer_rand_state;
	unsigned int            *kstat_irqs;
# ifdef CONFIG_INTR_REMAP
	struct irq_2_iommu      *irq_2_iommu;
# endif
#endif
	irq_flow_handler_t	handle_irq;
	struct irq_chip		*chip;
	struct msi_desc		*msi_desc;
	void			*handler_data;
	void			*chip_data;
	struct irqaction	*action;	/* IRQ action list */
	unsigned int		status;		/* IRQ status */

	unsigned int		depth;		/* nested irq disables */
	unsigned int		wake_depth;	/* nested wake enables */
	unsigned int		irq_count;	/* For detecting broken IRQs */
	unsigned long		last_unhandled;	/* Aging timer for unhandled count */
	unsigned int		irqs_unhandled;
	spinlock_t		lock;
#ifdef CONFIG_SMP
	cpumask_t		affinity;
	unsigned int		cpu;
#endif
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_t		pending_mask;
#endif
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry	*dir;
#endif
	const char		*name;
} ____cacheline_internodealigned_in_smp;

extern void early_irq_init(void);
extern void arch_early_irq_init(void);
extern void arch_init_chip_data(struct irq_desc *desc, int cpu);
extern void arch_init_copy_chip_data(struct irq_desc *old_desc,
					struct irq_desc *desc, int cpu);
extern void arch_free_chip_data(struct irq_desc *old_desc, struct irq_desc *desc);

#ifndef CONFIG_SPARSE_IRQ
extern struct irq_desc irq_desc[NR_IRQS];

static inline struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;
}
static inline struct irq_desc *irq_to_desc_alloc_cpu(unsigned int irq, int cpu)
{
	return irq_to_desc(irq);
}

#else

extern struct irq_desc *irq_to_desc(unsigned int irq);
extern struct irq_desc *irq_to_desc_alloc_cpu(unsigned int irq, int cpu);
extern struct irq_desc *move_irq_desc(struct irq_desc *old_desc, int cpu);

# define for_each_irq_desc(irq, desc)		\
	for (irq = 0, desc = irq_to_desc(irq); irq < nr_irqs; irq++, desc = irq_to_desc(irq))
# define for_each_irq_desc_reverse(irq, desc)                          \
	for (irq = nr_irqs - 1, desc = irq_to_desc(irq); irq >= 0; irq--, desc = irq_to_desc(irq))

#define kstat_irqs_this_cpu(DESC) \
	((DESC)->kstat_irqs[smp_processor_id()])
#define kstat_incr_irqs_this_cpu(irqno, DESC) \
	((DESC)->kstat_irqs[smp_processor_id()]++)

#endif

static inline struct irq_desc *
irq_remap_to_desc(unsigned int irq, struct irq_desc *desc)
{
#ifdef CONFIG_NUMA_MIGRATE_IRQ_DESC
	return irq_to_desc(irq);
#else
	return desc;
#endif
}

/*
 * Migration helpers for obsolete names, they will go away:
 */
#define hw_interrupt_type	irq_chip
typedef struct irq_chip		hw_irq_controller;
#define no_irq_type		no_irq_chip
typedef struct irq_desc		irq_desc_t;

/*
 * Pick up the arch-dependent methods:
 */
#include <asm/hw_irq.h>

extern int setup_irq(unsigned int irq, struct irqaction *new);

#ifdef CONFIG_GENERIC_HARDIRQS

#ifdef CONFIG_SMP

#ifdef CONFIG_GENERIC_PENDING_IRQ

void move_native_irq(int irq);
void move_masked_irq(int irq);

#else /* CONFIG_GENERIC_PENDING_IRQ */

static inline void move_irq(int irq)
{
}

static inline void move_native_irq(int irq)
{
}

static inline void move_masked_irq(int irq)
{
}

#endif /* CONFIG_GENERIC_PENDING_IRQ */

#else /* CONFIG_SMP */

#define move_native_irq(x)
#define move_masked_irq(x)

#endif /* CONFIG_SMP */

extern int no_irq_affinity;

static inline int irq_balancing_disabled(unsigned int irq)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	return desc->status & IRQ_NO_BALANCING_MASK;
}

/* Handle irq action chains: */
extern int handle_IRQ_event(unsigned int irq, struct irqaction *action);

/*
 * Built-in IRQ handlers for various IRQ types,
 * callable via desc->chip->handle_irq()
 */
extern void handle_level_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_fasteoi_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_edge_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_simple_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_percpu_irq(unsigned int irq, struct irq_desc *desc);
extern void handle_bad_irq(unsigned int irq, struct irq_desc *desc);

/*
 * Monolithic do_IRQ implementation.
 */
#ifndef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ
extern unsigned int __do_IRQ(unsigned int irq);
#endif

/*
 * Architectures call this to let the generic IRQ layer
 * handle an interrupt. If the descriptor is attached to an
 * irqchip-style controller then we call the ->handle_irq() handler,
 * and it calls __do_IRQ() if it's attached to an irqtype-style controller.
 */
static inline void generic_handle_irq_desc(unsigned int irq, struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ
	desc->handle_irq(irq, desc);
#else
	if (likely(desc->handle_irq))
		desc->handle_irq(irq, desc);
	else
		__do_IRQ(irq);
#endif
}

static inline void generic_handle_irq(unsigned int irq)
{
	generic_handle_irq_desc(irq, irq_to_desc(irq));
}

/* Handling of unhandled and spurious interrupts: */
extern void note_interrupt(unsigned int irq, struct irq_desc *desc,
			   int action_ret);

/* Resending of interrupts :*/
void check_irq_resend(struct irq_desc *desc, unsigned int irq);

/* Enable/disable irq debugging output: */
extern int noirqdebug_setup(char *str);

/* Checks whether the interrupt can be requested by request_irq(): */
extern int can_request_irq(unsigned int irq, unsigned long irqflags);

/* Dummy irq-chip implementations: */
extern struct irq_chip no_irq_chip;
extern struct irq_chip dummy_irq_chip;

extern void
set_irq_chip_and_handler(unsigned int irq, struct irq_chip *chip,
			 irq_flow_handler_t handle);
extern void
set_irq_chip_and_handler_name(unsigned int irq, struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name);

extern void
__set_irq_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		  const char *name);

/* caller has locked the irq_desc and both params are valid */
static inline void __set_irq_handler_unlocked(int irq,
					      irq_flow_handler_t handler)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	desc->handle_irq = handler;
}

/*
 * Set a highlevel flow handler for a given IRQ:
 */
static inline void
set_irq_handler(unsigned int irq, irq_flow_handler_t handle)
{
	__set_irq_handler(irq, handle, 0, NULL);
}

/*
 * Set a highlevel chained flow handler for a given IRQ.
 * (a chained handler is automatically enabled and set to
 *  IRQ_NOREQUEST and IRQ_NOPROBE)
 */
static inline void
set_irq_chained_handler(unsigned int irq,
			irq_flow_handler_t handle)
{
	__set_irq_handler(irq, handle, 1, NULL);
}

extern void set_irq_noprobe(unsigned int irq);
extern void set_irq_probe(unsigned int irq);

/* Handle dynamic irq creation and destruction */
extern unsigned int create_irq_nr(unsigned int irq_want);
extern int create_irq(void);
extern void destroy_irq(unsigned int irq);

/* Test to see if a driver has successfully requested an irq */
static inline int irq_has_action(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	return desc->action != NULL;
}

/* Dynamic irq helper functions */
extern void dynamic_irq_init(unsigned int irq);
extern void dynamic_irq_cleanup(unsigned int irq);

/* Set/get chip/data for an IRQ: */
extern int set_irq_chip(unsigned int irq, struct irq_chip *chip);
extern int set_irq_data(unsigned int irq, void *data);
extern int set_irq_chip_data(unsigned int irq, void *data);
extern int set_irq_type(unsigned int irq, unsigned int type);
extern int set_irq_msi(unsigned int irq, struct msi_desc *entry);

#define get_irq_chip(irq)	(irq_to_desc(irq)->chip)
#define get_irq_chip_data(irq)	(irq_to_desc(irq)->chip_data)
#define get_irq_data(irq)	(irq_to_desc(irq)->handler_data)
#define get_irq_msi(irq)	(irq_to_desc(irq)->msi_desc)

#define get_irq_desc_chip(desc)		((desc)->chip)
#define get_irq_desc_chip_data(desc)	((desc)->chip_data)
#define get_irq_desc_data(desc)		((desc)->handler_data)
#define get_irq_desc_msi(desc)		((desc)->msi_desc)

#endif /* CONFIG_GENERIC_HARDIRQS */

#endif /* !CONFIG_S390 */

#endif /* _LINUX_IRQ_H */
