#ifndef _ASM_X86_FUTEX_H
#define _ASM_X86_FUTEX_H

#ifdef __KERNEL__

#include <linux/futex.h>

#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#define __futex_atomic_op1(insn, ret, oldval, uaddr, oparg)	\
  __asm__ __volatile(						\
"1:	" insn "\n"						\
"2:	.section .fixup,\"ax\"\n				\
3:	mov	%3, %1\n					\
	jmp	2b\n						\
	.previous\n"						\
	_ASM_EXTABLE(1b,3b)					\
	: "=r" (oldval), "=r" (ret), "+m" (*uaddr)		\
	: "i" (-EFAULT), "0" (oparg), "1" (0))

#define __futex_atomic_op2(insn, ret, oldval, uaddr, oparg)	\
  __asm__ __volatile(						\
"1:	movl	%2, %0\n					\
	movl	%0, %3\n"					\
	insn "\n"						\
"2:	lock; cmpxchgl %3, %2\n					\
	jnz	1b\n						\
3:	.section .fixup,\"ax\"\n				\
4:	mov	%5, %1\n					\
	jmp	3b\n						\
	.previous\n"						\
	_ASM_EXTABLE(1b,4b)					\
	_ASM_EXTABLE(2b,4b)					\
	: "=&a" (oldval), "=&r" (ret), "+m" (*uaddr),		\
	  "=&r" (tem)						\
	: "r" (oparg), "i" (-EFAULT), "1" (0))

static inline int
futex_atomic_op_inuser(int encoded_op, int __user *uaddr)
{
	int op = (encoded_op >> 28) & 7;
	int cmp = (encoded_op >> 24) & 15;
	int oparg = (encoded_op << 8) >> 20;
	int cmparg = (encoded_op << 20) >> 20;
	int oldval = 0, ret, tem;

	if (encoded_op & (FUTEX_OP_OPARG_SHIFT << 28))
		oparg = 1 << oparg;

	if (!access_ok(VERIFY_WRITE, uaddr, sizeof(int)))
		return -EFAULT;

#if defined(CONFIG_X86_32) && !defined(CONFIG_X86_BSWAP)
	/* Real i386 machines can only support FUTEX_OP_SET */
	if (op != FUTEX_OP_SET && boot_cpu_data.x86 == 3)
		return -ENOSYS;
#endif

	pagefault_disable();

	switch (op) {
	case FUTEX_OP_SET:
		__futex_atomic_op1("xchgl %0, %2", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_ADD:
		__futex_atomic_op1("lock; xaddl %0, %2", ret, oldval,
				   uaddr, oparg);
		break;
	case FUTEX_OP_OR:
		__futex_atomic_op2("orl %4, %3", ret, oldval, uaddr, oparg);
		break;
	case FUTEX_OP_ANDN:
		__futex_atomic_op2("andl %4, %3", ret, oldval, uaddr, ~oparg);
		break;
	case FUTEX_OP_XOR:
		__futex_atomic_op2("xorl %4, %3", ret, oldval, uaddr, oparg);
		break;
	default:
		ret = -ENOSYS;
	}

	pagefault_enable();

	if (!ret) {
		switch (cmp) {
		case FUTEX_OP_CMP_EQ: ret = (oldval == cmparg); break;
		case FUTEX_OP_CMP_NE: ret = (oldval != cmparg); break;
		case FUTEX_OP_CMP_LT: ret = (oldval < cmparg); break;
		case FUTEX_OP_CMP_GE: ret = (oldval >= cmparg); break;
		case FUTEX_OP_CMP_LE: ret = (oldval <= cmparg); break;
		case FUTEX_OP_CMP_GT: ret = (oldval > cmparg); break;
		default: ret = -ENOSYS;
		}
	}
	return ret;
}

static inline int
futex_atomic_cmpxchg_inatomic(int __user *uaddr, int oldval, int newval)
{
	if (!access_ok(VERIFY_WRITE, uaddr, sizeof(int)))
		return -EFAULT;

	__asm__ __volatile__(
		"1:	lock; cmpxchgl %3, %1			\n"
		"2:	.section .fixup, \"ax\"			\n"
		"3:	mov     %2, %0				\n"
		"	jmp     2b				\n"
		"	.previous				\n"
		_ASM_EXTABLE(1b,3b)
		: "=a" (oldval), "+m" (*uaddr)
		: "i" (-EFAULT), "r" (newval), "0" (oldval)
		: "memory"
	);

	return oldval;
}

#endif
#endif
