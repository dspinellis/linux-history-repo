#include <linux/config.h>
#include <linux/stddef.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <asm/page.h>

#define DEFINE(sym, val) \
        asm volatile("\n->" #sym " %0 " #val : : "i" (val))

#define STR(x) #x
#define DEFINE_STR(sym, val) asm volatile("\n->" #sym " " STR(val) " " #val: : )

#define BLANK() asm volatile("\n->" : : )

#define OFFSET(sym, str, mem) \
	DEFINE(sym, offsetof(struct str, mem));

void foo(void)
{
	OFFSET(TASK_DEBUGREGS, task_struct, thread.arch.debugregs);
#ifdef CONFIG_MODE_TT
	OFFSET(TASK_EXTERN_PID, task_struct, thread.mode.tt.extern_pid);
#endif
#include <common-offsets.h>
}
