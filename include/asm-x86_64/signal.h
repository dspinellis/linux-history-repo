#ifndef _ASMx8664_SIGNAL_H
#define _ASMx8664_SIGNAL_H

#ifndef __ASSEMBLY__
#include <linux/types.h>
#include <linux/linkage.h>
#include <linux/time.h>

/* Avoid too many header ordering problems.  */
struct siginfo;

#ifdef __KERNEL__
/* Most things should be clean enough to redefine this at will, if care
   is taken to make libc match.  */

#define _NSIG		64
#define _NSIG_BPW	64
#define _NSIG_WORDS	(_NSIG / _NSIG_BPW)

typedef unsigned long old_sigset_t;		/* at least 32 bits */

typedef struct {
	unsigned long sig[_NSIG_WORDS];
} sigset_t;


struct pt_regs; 
asmlinkage int do_signal(struct pt_regs *regs, sigset_t *oldset);


#else
/* Here we must cater to libcs that poke about in kernel headers.  */

#define NSIG		32
typedef unsigned long sigset_t;

#endif /* __KERNEL__ */
#endif

#define SIGHUP		 1
#define SIGINT		 2
#define SIGQUIT		 3
#define SIGILL		 4
#define SIGTRAP		 5
#define SIGABRT		 6
#define SIGIOT		 6
#define SIGBUS		 7
#define SIGFPE		 8
#define SIGKILL		 9
#define SIGUSR1		10
#define SIGSEGV		11
#define SIGUSR2		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGSTKFLT	16
#define SIGCHLD		17
#define SIGCONT		18
#define SIGSTOP		19
#define SIGTSTP		20
#define SIGTTIN		21
#define SIGTTOU		22
#define SIGURG		23
#define SIGXCPU		24
#define SIGXFSZ		25
#define SIGVTALRM	26
#define SIGPROF		27
#define SIGWINCH	28
#define SIGIO		29
#define SIGPOLL		SIGIO
/*
#define SIGLOST		29
*/
#define SIGPWR		30
#define SIGSYS		31
#define	SIGUNUSED	31

/* These should not be considered constants from userland.  */
#define SIGRTMIN	32
#define SIGRTMAX	_NSIG

/*
 * SA_FLAGS values:
 *
 * SA_ONSTACK indicates that a registered stack_t will be used.
 * SA_INTERRUPT is a no-op, but left due to historical reasons. Use the
 * SA_RESTART flag to get restarting signals (which were the default long ago)
 * SA_NOCLDSTOP flag to turn off SIGCHLD when children stop.
 * SA_RESETHAND clears the handler when the signal is delivered.
 * SA_NOCLDWAIT flag on SIGCHLD to inhibit zombies.
 * SA_NODEFER prevents the current signal from being masked in the handler.
 *
 * SA_ONESHOT and SA_NOMASK are the historical Linux names for the Single
 * Unix names RESETHAND and NODEFER respectively.
 */
#define SA_NOCLDSTOP	0x00000001
#define SA_NOCLDWAIT	0x00000002
#define SA_SIGINFO	0x00000004
#define SA_ONSTACK	0x08000000
#define SA_RESTART	0x10000000
#define SA_NODEFER	0x40000000
#define SA_RESETHAND	0x80000000

#define SA_NOMASK	SA_NODEFER
#define SA_ONESHOT	SA_RESETHAND
#define SA_INTERRUPT	0x20000000 /* dummy -- ignored */

#define SA_RESTORER	0x04000000

/*
 * sigaltstack controls
 */
#define SS_ONSTACK	1
#define SS_DISABLE	2

#define MINSIGSTKSZ	2048
#define SIGSTKSZ	8192

#define SIG_BLOCK          0	/* for blocking signals */
#define SIG_UNBLOCK        1	/* for unblocking signals */
#define SIG_SETMASK        2	/* for setting the signal mask */

#ifndef __ASSEMBLY__
/* Type of a signal handler.  */
typedef void __signalfn_t(int);
typedef __signalfn_t __user *__sighandler_t;

typedef void __restorefn_t(void);
typedef __restorefn_t __user *__sigrestore_t;

#define SIG_DFL	((__sighandler_t)0)	/* default signal handling */
#define SIG_IGN	((__sighandler_t)1)	/* ignore signal */
#define SIG_ERR	((__sighandler_t)-1)	/* error return from signal */

struct sigaction {
	__sighandler_t sa_handler;
	unsigned long sa_flags;
	__sigrestore_t sa_restorer;
	sigset_t sa_mask;		/* mask last for extensibility */
};

struct k_sigaction {
	struct sigaction sa;
};

typedef struct sigaltstack {
	void __user *ss_sp;
	int ss_flags;
	size_t ss_size;
} stack_t;

#ifdef __KERNEL__
#include <asm/sigcontext.h>

#undef __HAVE_ARCH_SIG_BITOPS
#if 0

extern __inline__ void sigaddset(sigset_t *set, int _sig)
{
	__asm__("btsq %1,%0" : "=m"(*set) : "Ir"(_sig - 1) : "cc");
}

extern __inline__ void sigdelset(sigset_t *set, int _sig)
{
	__asm__("btrq %1,%0" : "=m"(*set) : "Ir"(_sig - 1) : "cc");
}

extern __inline__ int __const_sigismember(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	return 1 & (set->sig[sig / _NSIG_BPW] >> (sig & ~(_NSIG_BPW-1)));
}

extern __inline__ int __gen_sigismember(sigset_t *set, int _sig)
{
	int ret;
	__asm__("btq %2,%1\n\tsbbq %0,%0"
		: "=r"(ret) : "m"(*set), "Ir"(_sig-1) : "cc");
	return ret;
}

#define sigismember(set,sig)			\
	(__builtin_constant_p(sig) ?		\
	 __const_sigismember((set),(sig)) :	\
	 __gen_sigismember((set),(sig)))

extern __inline__ int sigfindinword(unsigned long word)
{
	__asm__("bsfq %1,%0" : "=r"(word) : "rm"(word) : "cc");
	return word;
}
#endif
#endif

#define ptrace_signal_deliver(regs, cookie) do { } while (0)

#endif /* __KERNEL__ */

#endif
