/*
 * Copyright (C) 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Licensed under the GPL
 */

#ifndef __START_H__
#define __START_H__

#include "uml-config.h"
#include "kern_constants.h"

/*
 * Stolen from linux/const.h, which can't be directly included since
 * this is used in userspace code, which has no access to the kernel
 * headers.  Changed to be suitable for adding casts to the start,
 * rather than "UL" to the end.
 */

/* Some constant macros are used in both assembler and
 * C code.  Therefore we cannot annotate them always with
 * 'UL' and other type specifiers unilaterally.  We
 * use the following macros to deal with this.
 */

#ifdef __ASSEMBLY__
#define _AC(X, Y)	(Y)
#else
#define __AC(X, Y)	(X (Y))
#define _AC(X, Y)	__AC(X, Y)
#endif

/*
 * The "- 1"'s are to avoid gcc complaining about integer overflows
 * and unrepresentable decimal constants.  With 3-level page tables,
 * TASK_SIZE is 0x80000000, which gets turned into its signed decimal
 * equivalent in asm-offsets.s.  gcc then complains about that being
 * unsigned only in C90.  To avoid that, UM_TASK_SIZE is defined as
 * TASK_SIZE - 1.  To compensate, we need to add the 1 back here.
 * However, adding it back to UM_TASK_SIZE produces more gcc
 * complaints.  So, I adjust the thing being subtracted from
 * UM_TASK_SIZE instead.  Bah.
 */
#define STUB_CODE _AC((unsigned long), \
		      UM_TASK_SIZE - (2 * UM_KERN_PAGE_SIZE - 1))
#define STUB_DATA _AC((unsigned long), UM_TASK_SIZE - (UM_KERN_PAGE_SIZE - 1))
#define STUB_START _AC(, STUB_CODE)

#ifndef __ASSEMBLY__

#include "sysdep/ptrace.h"

struct cpu_task {
	int pid;
	void *task;
};

extern struct cpu_task cpu_tasks[];

extern unsigned long low_physmem;
extern unsigned long high_physmem;
extern unsigned long uml_physmem;
extern unsigned long uml_reserved;
extern unsigned long end_vm;
extern unsigned long start_vm;
extern unsigned long long highmem;

extern unsigned long _stext, _etext, _sdata, _edata, __bss_start, _end;
extern unsigned long _unprotected_end;
extern unsigned long brk_start;

extern int linux_main(int argc, char **argv);

extern void (*sig_info[])(int, struct uml_pt_regs *);

#endif

#endif
