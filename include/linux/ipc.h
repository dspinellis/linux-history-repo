#ifndef _LINUX_IPC_H
#define _LINUX_IPC_H

#include <linux/types.h>
#include <linux/kref.h>

#define IPC_PRIVATE ((__kernel_key_t) 0)  

/* Obsolete, used only for backwards compatibility and libc5 compiles */
struct ipc_perm
{
	__kernel_key_t	key;
	__kernel_uid_t	uid;
	__kernel_gid_t	gid;
	__kernel_uid_t	cuid;
	__kernel_gid_t	cgid;
	__kernel_mode_t	mode; 
	unsigned short	seq;
};

/* Include the definition of ipc64_perm */
#include <asm/ipcbuf.h>

/* resource get request flags */
#define IPC_CREAT  00001000   /* create if key is nonexistent */
#define IPC_EXCL   00002000   /* fail if key exists */
#define IPC_NOWAIT 00004000   /* return error on wait */

/* these fields are used by the DIPC package so the kernel as standard
   should avoid using them if possible */
   
#define IPC_DIPC 00010000  /* make it distributed */
#define IPC_OWN  00020000  /* this machine is the DIPC owner */

/* 
 * Control commands used with semctl, msgctl and shmctl 
 * see also specific commands in sem.h, msg.h and shm.h
 */
#define IPC_RMID 0     /* remove resource */
#define IPC_SET  1     /* set ipc_perm options */
#define IPC_STAT 2     /* get ipc_perm options */
#define IPC_INFO 3     /* see ipcs */

/*
 * Version flags for semctl, msgctl, and shmctl commands
 * These are passed as bitflags or-ed with the actual command
 */
#define IPC_OLD 0	/* Old version (no 32-bit UID support on many
			   architectures) */
#define IPC_64  0x0100  /* New version (support 32-bit UIDs, bigger
			   message sizes, etc. */

#ifdef __KERNEL__

#define IPCMNI 32768  /* <= MAX_INT limit for ipc arrays (including sysctl changes) */

/* used by in-kernel data structures */
struct kern_ipc_perm
{
	spinlock_t	lock;
	int		deleted;
	key_t		key;
	uid_t		uid;
	gid_t		gid;
	uid_t		cuid;
	gid_t		cgid;
	mode_t		mode; 
	unsigned long	seq;
	void		*security;
};

struct ipc_ids;
struct ipc_namespace {
	struct kref	kref;
	struct ipc_ids	*ids[3];

	int		sem_ctls[4];
	int		used_sems;

	int		msg_ctlmax;
	int		msg_ctlmnb;
	int		msg_ctlmni;

	size_t		shm_ctlmax;
	size_t		shm_ctlall;
	int		shm_ctlmni;
	int		shm_tot;
};

extern struct ipc_namespace init_ipc_ns;
extern void free_ipc_ns(struct kref *kref);
extern int copy_ipcs(unsigned long flags, struct task_struct *tsk);
extern int unshare_ipcs(unsigned long flags, struct ipc_namespace **ns);

static inline struct ipc_namespace *get_ipc_ns(struct ipc_namespace *ns)
{
	if (ns)
		kref_get(&ns->kref);
	return ns;
}

static inline void put_ipc_ns(struct ipc_namespace *ns)
{
	kref_put(&ns->kref, free_ipc_ns);
}

#endif /* __KERNEL__ */

#endif /* _LINUX_IPC_H */


