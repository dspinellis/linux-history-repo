/* Things the lguest guest needs to know.  Note: like all lguest interfaces,
 * this is subject to wild and random change between versions. */
#ifndef _LINUX_LGUEST_H
#define _LINUX_LGUEST_H

#ifndef __ASSEMBLY__
#include <asm/irq.h>
#include <asm/lguest_hcall.h>

#define LG_CLOCK_MIN_DELTA	100UL
#define LG_CLOCK_MAX_DELTA	ULONG_MAX

/*G:032 The second method of communicating with the Host is to via "struct
 * lguest_data".  The Guest's very first hypercall is to tell the Host where
 * this is, and then the Guest and Host both publish information in it. :*/
struct lguest_data
{
	/* 512 == enabled (same as eflags in normal hardware).  The Guest
	 * changes interrupts so often that a hypercall is too slow. */
	unsigned int irq_enabled;
	/* Fine-grained interrupt disabling by the Guest */
	DECLARE_BITMAP(blocked_interrupts, LGUEST_IRQS);

	/* The Host writes the virtual address of the last page fault here,
	 * which saves the Guest a hypercall.  CR2 is the native register where
	 * this address would normally be found. */
	unsigned long cr2;

	/* Wallclock time set by the Host. */
	struct timespec time;

	/* Async hypercall ring.  Instead of directly making hypercalls, we can
	 * place them in here for processing the next time the Host wants.
	 * This batching can be quite efficient. */

	/* 0xFF == done (set by Host), 0 == pending (set by Guest). */
	u8 hcall_status[LHCALL_RING_SIZE];
	/* The actual registers for the hypercalls. */
	struct hcall_ring hcalls[LHCALL_RING_SIZE];

/* Fields initialized by the Host at boot: */
	/* Memory not to try to access */
	unsigned long reserve_mem;
	/* ID of this Guest (used by network driver to set ethernet address) */
	u16 guestid;
	/* KHz for the TSC clock. */
	u32 tsc_khz;

/* Fields initialized by the Guest at boot: */
	/* Instruction range to suppress interrupts even if enabled */
	unsigned long noirq_start, noirq_end;
};
extern struct lguest_data lguest_data;
#endif /* __ASSEMBLY__ */
#endif	/* _LINUX_LGUEST_H */
