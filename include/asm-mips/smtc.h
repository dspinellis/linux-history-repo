#ifndef _ASM_SMTC_MT_H
#define _ASM_SMTC_MT_H

/*
 * Definitions for SMTC multitasking on MIPS MT cores
 */

#include <asm/mips_mt.h>

/*
 * System-wide SMTC status information
 */

extern unsigned int smtc_status;

#define SMTC_TLB_SHARED	0x00000001
#define SMTC_MTC_ACTIVE	0x00000002

/*
 * TLB/ASID Management information
 */

#define MAX_SMTC_TLBS 2
#define MAX_SMTC_ASIDS 256
#if NR_CPUS <= 8
typedef char asiduse;
#else
#if NR_CPUS <= 16
typedef short asiduse;
#else
typedef long asiduse;
#endif
#endif

extern asiduse smtc_live_asid[MAX_SMTC_TLBS][MAX_SMTC_ASIDS];

void smtc_get_new_mmu_context(struct mm_struct *mm, unsigned long cpu);

void smtc_flush_tlb_asid(unsigned long asid);
extern int mipsmt_build_cpu_map(int startslot);
extern void mipsmt_prepare_cpus(void);
extern void smtc_smp_finish(void);
extern void smtc_boot_secondary(int cpu, struct task_struct *t);

/*
 * Sharing the TLB between multiple VPEs means that the
 * "random" index selection function is not allowed to
 * select the current value of the Index register. To
 * avoid additional TLB pressure, the Index registers
 * are "parked" with an non-Valid value.
 */

#define PARKED_INDEX	((unsigned int)0x80000000)

#endif /*  _ASM_SMTC_MT_H */
