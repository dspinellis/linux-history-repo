/*
 *  include/asm-s390/pgalloc.h
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Hartmut Penner (hp@de.ibm.com)
 *               Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 *  Derived from "include/asm-i386/pgalloc.h"
 *    Copyright (C) 1994  Linus Torvalds
 */

#ifndef _S390_PGALLOC_H
#define _S390_PGALLOC_H

#include <linux/threads.h>
#include <linux/gfp.h>
#include <linux/mm.h>

#define check_pgt_cache()	do {} while (0)

unsigned long *crst_table_alloc(struct mm_struct *, int);
void crst_table_free(struct mm_struct *, unsigned long *);

unsigned long *page_table_alloc(struct mm_struct *);
void page_table_free(struct mm_struct *, unsigned long *);
void disable_noexec(struct mm_struct *, struct task_struct *);

static inline void clear_table(unsigned long *s, unsigned long val, size_t n)
{
	*s = val;
	n = (n / 256) - 1;
	asm volatile(
#ifdef CONFIG_64BIT
		"	mvc	8(248,%0),0(%0)\n"
#else
		"	mvc	4(252,%0),0(%0)\n"
#endif
		"0:	mvc	256(256,%0),0(%0)\n"
		"	la	%0,256(%0)\n"
		"	brct	%1,0b\n"
		: "+a" (s), "+d" (n));
}

static inline void crst_table_init(unsigned long *crst, unsigned long entry)
{
	clear_table(crst, entry, sizeof(unsigned long)*2048);
	crst = get_shadow_table(crst);
	if (crst)
		clear_table(crst, entry, sizeof(unsigned long)*2048);
}

#ifndef __s390x__

static inline unsigned long pgd_entry_type(struct mm_struct *mm)
{
	return _SEGMENT_ENTRY_EMPTY;
}

#define pud_alloc_one(mm,address)		({ BUG(); ((pud_t *)2); })
#define pud_free(mm, x)				do { } while (0)

#define pmd_alloc_one(mm,address)		({ BUG(); ((pmd_t *)2); })
#define pmd_free(mm, x)				do { } while (0)

#define pgd_populate(mm, pgd, pud)		BUG()
#define pgd_populate_kernel(mm, pgd, pud)	BUG()

#define pud_populate(mm, pud, pmd)		BUG()
#define pud_populate_kernel(mm, pud, pmd)	BUG()

#else /* __s390x__ */

static inline unsigned long pgd_entry_type(struct mm_struct *mm)
{
	return _REGION3_ENTRY_EMPTY;
}

#define pud_alloc_one(mm,address)		({ BUG(); ((pud_t *)2); })
#define pud_free(mm, x)				do { } while (0)

static inline pmd_t *pmd_alloc_one(struct mm_struct *mm, unsigned long vmaddr)
{
	unsigned long *table = crst_table_alloc(mm, mm->context.noexec);
	if (table)
		crst_table_init(table, _SEGMENT_ENTRY_EMPTY);
	return (pmd_t *) table;
}
#define pmd_free(mm, pmd) crst_table_free(mm, (unsigned long *) pmd)

#define pgd_populate(mm, pgd, pud)		BUG()
#define pgd_populate_kernel(mm, pgd, pud)	BUG()

static inline void pud_populate_kernel(struct mm_struct *mm,
				       pud_t *pud, pmd_t *pmd)
{
	pud_val(*pud) = _REGION3_ENTRY | __pa(pmd);
}

static inline void pud_populate(struct mm_struct *mm, pud_t *pud, pmd_t *pmd)
{
	pud_populate_kernel(mm, pud, pmd);
	if (mm->context.noexec) {
		pud = get_shadow_table(pud);
		pmd = get_shadow_table(pmd);
		pud_populate_kernel(mm, pud, pmd);
	}
}

#endif /* __s390x__ */

static inline pgd_t *pgd_alloc(struct mm_struct *mm)
{
	unsigned long *crst;

	INIT_LIST_HEAD(&mm->context.crst_list);
	INIT_LIST_HEAD(&mm->context.pgtable_list);
	crst = crst_table_alloc(mm, s390_noexec);
	if (crst)
		crst_table_init(crst, pgd_entry_type(mm));
	return (pgd_t *) crst;
}
#define pgd_free(mm, pgd) crst_table_free(mm, (unsigned long *) pgd)

static inline void pmd_populate_kernel(struct mm_struct *mm,
				       pmd_t *pmd, pte_t *pte)
{
	pmd_val(*pmd) = _SEGMENT_ENTRY + __pa(pte);
}

static inline void pmd_populate(struct mm_struct *mm,
				pmd_t *pmd, pgtable_t pte)
{
	pmd_populate_kernel(mm, pmd, pte);
	if (mm->context.noexec) {
		pmd = get_shadow_table(pmd);
		pmd_populate_kernel(mm, pmd, pte + PTRS_PER_PTE);
	}
}

#define pmd_pgtable(pmd) \
	(pgtable_t)(pmd_val(pmd) & -sizeof(pte_t)*PTRS_PER_PTE)

/*
 * page table entry allocation/free routines.
 */
#define pte_alloc_one_kernel(mm, vmaddr) ((pte_t *) page_table_alloc(mm))
#define pte_alloc_one(mm, vmaddr) ((pte_t *) page_table_alloc(mm))

#define pte_free_kernel(mm, pte) page_table_free(mm, (unsigned long *) pte)
#define pte_free(mm, pte) page_table_free(mm, (unsigned long *) pte)

#endif /* _S390_PGALLOC_H */
