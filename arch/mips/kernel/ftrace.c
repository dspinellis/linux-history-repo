/*
 * Code for replacing ftrace calls with jumps.
 *
 * Copyright (C) 2007-2008 Steven Rostedt <srostedt@redhat.com>
 * Copyright (C) 2009 DSLab, Lanzhou University, China
 * Author: Wu Zhangjin <wuzj@lemote.com>
 *
 * Thanks goes to Steven Rostedt for writing the original x86 version.
 */

#include <linux/uaccess.h>
#include <linux/init.h>
#include <linux/ftrace.h>

#include <asm/cacheflush.h>
#include <asm/asm.h>
#include <asm/asm-offsets.h>

#ifdef CONFIG_DYNAMIC_FTRACE

#define JAL 0x0c000000		/* jump & link: ip --> ra, jump to target */
#define ADDR_MASK 0x03ffffff	/*  op_code|addr : 31...26|25 ....0 */
#define jump_insn_encode(op_code, addr) \
	((unsigned int)((op_code) | (((addr) >> 2) & ADDR_MASK)))

static unsigned int ftrace_nop = 0x00000000;

static int ftrace_modify_code(unsigned long ip, unsigned int new_code)
{
	*(unsigned int *)ip = new_code;

	flush_icache_range(ip, ip + 8);

	return 0;
}

static int lui_v1;
static int jal_mcount;

int ftrace_make_nop(struct module *mod,
		    struct dyn_ftrace *rec, unsigned long addr)
{
	unsigned int new;
	unsigned long ip = rec->ip;

	/* We have compiled module with -mlong-calls, but compiled the kernel
	 * without it, we need to cope with them respectively. */
	if (ip & 0x40000000) {
		/* record it for ftrace_make_call */
		if (lui_v1 == 0)
			lui_v1 = *(unsigned int *)ip;

		/* lui v1, hi_16bit_of_mcount        --> b 1f (0x10000004)
		 * addiu v1, v1, low_16bit_of_mcount
		 * move at, ra
		 * jalr v1
		 * nop
		 * 				     1f: (ip + 12)
		 */
		new = 0x10000004;
	} else {
		/* record/calculate it for ftrace_make_call */
		if (jal_mcount == 0) {
			/* We can record it directly like this:
			 *     jal_mcount = *(unsigned int *)ip;
			 * Herein, jump over the first two nop instructions */
			jal_mcount = jump_insn_encode(JAL, (MCOUNT_ADDR + 8));
		}

		/* move at, ra
		 * jalr v1		--> nop
		 */
		new = ftrace_nop;
	}
	return ftrace_modify_code(ip, new);
}

static int modified;	/* initialized as 0 by default */

int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	unsigned int new;
	unsigned long ip = rec->ip;

	/* We just need to remove the "b ftrace_stub" at the fist time! */
	if (modified == 0) {
		modified = 1;
		ftrace_modify_code(addr, ftrace_nop);
	}
	/* ip, module: 0xc0000000, kernel: 0x80000000 */
	new = (ip & 0x40000000) ? lui_v1 : jal_mcount;

	return ftrace_modify_code(ip, new);
}

#define FTRACE_CALL_IP ((unsigned long)(&ftrace_call))

int ftrace_update_ftrace_func(ftrace_func_t func)
{
	unsigned int new;

	new = jump_insn_encode(JAL, (unsigned long)func);

	return ftrace_modify_code(FTRACE_CALL_IP, new);
}

int __init ftrace_dyn_arch_init(void *data)
{
	/* The return code is retured via data */
	*(unsigned long *)data = 0;

	return 0;
}
#endif				/* CONFIG_DYNAMIC_FTRACE */

#ifdef CONFIG_FUNCTION_GRAPH_TRACER

#define S_RA_SP	(0xafbf << 16)	/* s{d,w} ra, offset(sp) */
#define S_R_SP	(0xafb0 << 16)  /* s{d,w} R, offset(sp) */
#define OFFSET_MASK	0xffff	/* stack offset range: 0 ~ PT_SIZE */

unsigned long ftrace_get_parent_addr(unsigned long self_addr,
				     unsigned long parent,
				     unsigned long parent_addr,
				     unsigned long fp)
{
	unsigned long sp, ip, ra;
	unsigned int code;

	/* in module or kernel? */
	if (self_addr & 0x40000000) {
		/* module: move to the instruction "lui v1, HI_16BIT_OF_MCOUNT" */
		ip = self_addr - 20;
	} else {
		/* kernel: move to the instruction "move ra, at" */
		ip = self_addr - 12;
	}

	/* search the text until finding the non-store instruction or "s{d,w}
	 * ra, offset(sp)" instruction */
	do {
		ip -= 4;

		/* get the code at "ip" */
		code = *(unsigned int *)ip;

		/* If we hit the non-store instruction before finding where the
		 * ra is stored, then this is a leaf function and it does not
		 * store the ra on the stack. */
		if ((code & S_R_SP) != S_R_SP)
			return parent_addr;

	} while (((code & S_RA_SP) != S_RA_SP));

	sp = fp + (code & OFFSET_MASK);
	ra = *(unsigned long *)sp;

	if (ra == parent)
		return sp;

	return 0;
}

/*
 * Hook the return address and push it in the stack of return addrs
 * in current thread info.
 */
void prepare_ftrace_return(unsigned long *parent, unsigned long self_addr,
			   unsigned long fp)
{
	unsigned long old;
	struct ftrace_graph_ent trace;
	unsigned long return_hooker = (unsigned long)
	    &return_to_handler;

	if (unlikely(atomic_read(&current->tracing_graph_pause)))
		return;

	/* "parent" is the stack address saved the return address of the caller
	 * of _mcount, for a leaf function not save the return address in the
	 * stack address, so, we "emulate" one in _mcount's stack space, and
	 * hijack it directly, but for a non-leaf function, it will save the
	 * return address to the its stack space, so, we can not hijack the
	 * "parent" directly, but need to find the real stack address,
	 * ftrace_get_parent_addr() does it!
	 */

	old = *parent;

	parent = (unsigned long *)ftrace_get_parent_addr(self_addr, old,
							 (unsigned long)parent,
							 fp);

	/* If fails when getting the stack address of the non-leaf function's
	 * ra, stop function graph tracer and return */
	if (parent == 0) {
		ftrace_graph_stop();
		WARN_ON(1);
		return;
	}

	*parent = return_hooker;

	if (ftrace_push_return_trace(old, self_addr, &trace.depth, fp) ==
	    -EBUSY) {
		*parent = old;
		return;
	}

	trace.func = self_addr;

	/* Only trace if the calling function expects to */
	if (!ftrace_graph_entry(&trace)) {
		current->curr_ret_stack--;
		*parent = old;
	}
}
#endif				/* CONFIG_FUNCTION_GRAPH_TRACER */
