/*
 * Copyright (C) 2006-2007 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */

#include <linux/miscdevice.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/dlm.h>
#include <linux/dlm_device.h>

#include "dlm_internal.h"
#include "lockspace.h"
#include "lock.h"
#include "lvb_table.h"
#include "user.h"

static const char name_prefix[] = "dlm";
static const struct file_operations device_fops;

#ifdef CONFIG_COMPAT

struct dlm_lock_params32 {
	__u8 mode;
	__u8 namelen;
	__u16 unused;
	__u32 flags;
	__u32 lkid;
	__u32 parent;
	__u64 xid;
	__u64 timeout;
	__u32 castparam;
	__u32 castaddr;
	__u32 bastparam;
	__u32 bastaddr;
	__u32 lksb;
	char lvb[DLM_USER_LVB_LEN];
	char name[0];
};

struct dlm_write_request32 {
	__u32 version[3];
	__u8 cmd;
	__u8 is64bit;
	__u8 unused[2];

	union  {
		struct dlm_lock_params32 lock;
		struct dlm_lspace_params lspace;
		struct dlm_purge_params purge;
	} i;
};

struct dlm_lksb32 {
	__u32 sb_status;
	__u32 sb_lkid;
	__u8 sb_flags;
	__u32 sb_lvbptr;
};

struct dlm_lock_result32 {
	__u32 version[3];
	__u32 length;
	__u32 user_astaddr;
	__u32 user_astparam;
	__u32 user_lksb;
	struct dlm_lksb32 lksb;
	__u8 bast_mode;
	__u8 unused[3];
	/* Offsets may be zero if no data is present */
	__u32 lvb_offset;
};

static void compat_input(struct dlm_write_request *kb,
			 struct dlm_write_request32 *kb32,
			 size_t count)
{
	kb->version[0] = kb32->version[0];
	kb->version[1] = kb32->version[1];
	kb->version[2] = kb32->version[2];

	kb->cmd = kb32->cmd;
	kb->is64bit = kb32->is64bit;
	if (kb->cmd == DLM_USER_CREATE_LOCKSPACE ||
	    kb->cmd == DLM_USER_REMOVE_LOCKSPACE) {
		kb->i.lspace.flags = kb32->i.lspace.flags;
		kb->i.lspace.minor = kb32->i.lspace.minor;
		memcpy(kb->i.lspace.name, kb32->i.lspace.name, count -
			offsetof(struct dlm_write_request32, i.lspace.name));
	} else if (kb->cmd == DLM_USER_PURGE) {
		kb->i.purge.nodeid = kb32->i.purge.nodeid;
		kb->i.purge.pid = kb32->i.purge.pid;
	} else {
		kb->i.lock.mode = kb32->i.lock.mode;
		kb->i.lock.namelen = kb32->i.lock.namelen;
		kb->i.lock.flags = kb32->i.lock.flags;
		kb->i.lock.lkid = kb32->i.lock.lkid;
		kb->i.lock.parent = kb32->i.lock.parent;
		kb->i.lock.xid = kb32->i.lock.xid;
		kb->i.lock.timeout = kb32->i.lock.timeout;
		kb->i.lock.castparam = (void *)(long)kb32->i.lock.castparam;
		kb->i.lock.castaddr = (void *)(long)kb32->i.lock.castaddr;
		kb->i.lock.bastparam = (void *)(long)kb32->i.lock.bastparam;
		kb->i.lock.bastaddr = (void *)(long)kb32->i.lock.bastaddr;
		kb->i.lock.lksb = (void *)(long)kb32->i.lock.lksb;
		memcpy(kb->i.lock.lvb, kb32->i.lock.lvb, DLM_USER_LVB_LEN);
		memcpy(kb->i.lock.name, kb32->i.lock.name, count -
			offsetof(struct dlm_write_request32, i.lock.name));
	}
}

static void compat_output(struct dlm_lock_result *res,
			  struct dlm_lock_result32 *res32)
{
	res32->version[0] = res->version[0];
	res32->version[1] = res->version[1];
	res32->version[2] = res->version[2];

	res32->user_astaddr = (__u32)(long)res->user_astaddr;
	res32->user_astparam = (__u32)(long)res->user_astparam;
	res32->user_lksb = (__u32)(long)res->user_lksb;
	res32->bast_mode = res->bast_mode;

	res32->lvb_offset = res->lvb_offset;
	res32->length = res->length;

	res32->lksb.sb_status = res->lksb.sb_status;
	res32->lksb.sb_flags = res->lksb.sb_flags;
	res32->lksb.sb_lkid = res->lksb.sb_lkid;
	res32->lksb.sb_lvbptr = (__u32)(long)res->lksb.sb_lvbptr;
}
#endif

/* Figure out if this lock is at the end of its life and no longer
   available for the application to use.  The lkb still exists until
   the final ast is read.  A lock becomes EOL in three situations:
     1. a noqueue request fails with EAGAIN
     2. an unlock completes with EUNLOCK
     3. a cancel of a waiting request completes with ECANCEL/EDEADLK
   An EOL lock needs to be removed from the process's list of locks.
   And we can't allow any new operation on an EOL lock.  This is
   not related to the lifetime of the lkb struct which is managed
   entirely by refcount. */

static int lkb_is_endoflife(struct dlm_lkb *lkb, int sb_status, int type)
{
	switch (sb_status) {
	case -DLM_EUNLOCK:
		return 1;
	case -DLM_ECANCEL:
	case -ETIMEDOUT:
	case -EDEADLK:
		if (lkb->lkb_grmode == DLM_LOCK_IV)
			return 1;
		break;
	case -EAGAIN:
		if (type == AST_COMP && lkb->lkb_grmode == DLM_LOCK_IV)
			return 1;
		break;
	}
	return 0;
}

/* we could possibly check if the cancel of an orphan has resulted in the lkb
   being removed and then remove that lkb from the orphans list and free it */

void dlm_user_add_ast(struct dlm_lkb *lkb, int type)
{
	struct dlm_ls *ls;
	struct dlm_user_args *ua;
	struct dlm_user_proc *proc;
	int eol = 0, ast_type;

	if (lkb->lkb_flags & (DLM_IFL_ORPHAN | DLM_IFL_DEAD))
		return;

	ls = lkb->lkb_resource->res_ls;
	mutex_lock(&ls->ls_clear_proc_locks);

	/* If ORPHAN/DEAD flag is set, it means the process is dead so an ast
	   can't be delivered.  For ORPHAN's, dlm_clear_proc_locks() freed
	   lkb->ua so we can't try to use it.  This second check is necessary
	   for cases where a completion ast is received for an operation that
	   began before clear_proc_locks did its cancel/unlock. */

	if (lkb->lkb_flags & (DLM_IFL_ORPHAN | DLM_IFL_DEAD))
		goto out;

	DLM_ASSERT(lkb->lkb_ua, dlm_print_lkb(lkb););
	ua = lkb->lkb_ua;
	proc = ua->proc;

	if (type == AST_BAST && ua->bastaddr == NULL)
		goto out;

	spin_lock(&proc->asts_spin);

	ast_type = lkb->lkb_ast_type;
	lkb->lkb_ast_type |= type;

	if (!ast_type) {
		kref_get(&lkb->lkb_ref);
		list_add_tail(&lkb->lkb_astqueue, &proc->asts);
		wake_up_interruptible(&proc->wait);
	}
	if (type == AST_COMP && (ast_type & AST_COMP))
		log_debug(ls, "ast overlap %x status %x %x",
			  lkb->lkb_id, ua->lksb.sb_status, lkb->lkb_flags);

	eol = lkb_is_endoflife(lkb, ua->lksb.sb_status, type);
	if (eol) {
		lkb->lkb_ast_type &= ~AST_BAST;
		lkb->lkb_flags |= DLM_IFL_ENDOFLIFE;
	}

	/* We want to copy the lvb to userspace when the completion
	   ast is read if the status is 0, the lock has an lvb and
	   lvb_ops says we should.  We could probably have set_lvb_lock()
	   set update_user_lvb instead and not need old_mode */

	if ((lkb->lkb_ast_type & AST_COMP) &&
	    (lkb->lkb_lksb->sb_status == 0) &&
	    lkb->lkb_lksb->sb_lvbptr &&
	    dlm_lvb_operations[ua->old_mode + 1][lkb->lkb_grmode + 1])
		ua->update_user_lvb = 1;
	else
		ua->update_user_lvb = 0;

	spin_unlock(&proc->asts_spin);

	if (eol) {
		spin_lock(&proc->locks_spin);
		if (!list_empty(&lkb->lkb_ownqueue)) {
			list_del_init(&lkb->lkb_ownqueue);
			dlm_put_lkb(lkb);
		}
		spin_unlock(&proc->locks_spin);
	}
 out:
	mutex_unlock(&ls->ls_clear_proc_locks);
}

static int device_user_lock(struct dlm_user_proc *proc,
			    struct dlm_lock_params *params)
{
	struct dlm_ls *ls;
	struct dlm_user_args *ua;
	int error = -ENOMEM;

	ls = dlm_find_lockspace_local(proc->lockspace);
	if (!ls)
		return -ENOENT;

	if (!params->castaddr || !params->lksb) {
		error = -EINVAL;
		goto out;
	}

	ua = kzalloc(sizeof(struct dlm_user_args), GFP_KERNEL);
	if (!ua)
		goto out;
	ua->proc = proc;
	ua->user_lksb = params->lksb;
	ua->castparam = params->castparam;
	ua->castaddr = params->castaddr;
	ua->bastparam = params->bastparam;
	ua->bastaddr = params->bastaddr;
	ua->xid = params->xid;

	if (params->flags & DLM_LKF_CONVERT)
		error = dlm_user_convert(ls, ua,
				         params->mode, params->flags,
				         params->lkid, params->lvb,
					 (unsigned long) params->timeout);
	else {
		error = dlm_user_request(ls, ua,
					 params->mode, params->flags,
					 params->name, params->namelen,
					 (unsigned long) params->timeout);
		if (!error)
			error = ua->lksb.sb_lkid;
	}
 out:
	dlm_put_lockspace(ls);
	return error;
}

static int device_user_unlock(struct dlm_user_proc *proc,
			      struct dlm_lock_params *params)
{
	struct dlm_ls *ls;
	struct dlm_user_args *ua;
	int error = -ENOMEM;

	ls = dlm_find_lockspace_local(proc->lockspace);
	if (!ls)
		return -ENOENT;

	ua = kzalloc(sizeof(struct dlm_user_args), GFP_KERNEL);
	if (!ua)
		goto out;
	ua->proc = proc;
	ua->user_lksb = params->lksb;
	ua->castparam = params->castparam;
	ua->castaddr = params->castaddr;

	if (params->flags & DLM_LKF_CANCEL)
		error = dlm_user_cancel(ls, ua, params->flags, params->lkid);
	else
		error = dlm_user_unlock(ls, ua, params->flags, params->lkid,
					params->lvb);
 out:
	dlm_put_lockspace(ls);
	return error;
}

static int device_user_deadlock(struct dlm_user_proc *proc,
				struct dlm_lock_params *params)
{
	struct dlm_ls *ls;
	int error;

	ls = dlm_find_lockspace_local(proc->lockspace);
	if (!ls)
		return -ENOENT;

	error = dlm_user_deadlock(ls, params->flags, params->lkid);

	dlm_put_lockspace(ls);
	return error;
}

static int create_misc_device(struct dlm_ls *ls, char *name)
{
	int error, len;

	error = -ENOMEM;
	len = strlen(name) + strlen(name_prefix) + 2;
	ls->ls_device.name = kzalloc(len, GFP_KERNEL);
	if (!ls->ls_device.name)
		goto fail;

	snprintf((char *)ls->ls_device.name, len, "%s_%s", name_prefix,
		 name);
	ls->ls_device.fops = &device_fops;
	ls->ls_device.minor = MISC_DYNAMIC_MINOR;

	error = misc_register(&ls->ls_device);
	if (error) {
		kfree(ls->ls_device.name);
	}
fail:
	return error;
}

static int device_user_purge(struct dlm_user_proc *proc,
			     struct dlm_purge_params *params)
{
	struct dlm_ls *ls;
	int error;

	ls = dlm_find_lockspace_local(proc->lockspace);
	if (!ls)
		return -ENOENT;

	error = dlm_user_purge(ls, proc, params->nodeid, params->pid);

	dlm_put_lockspace(ls);
	return error;
}

static int device_create_lockspace(struct dlm_lspace_params *params)
{
	dlm_lockspace_t *lockspace;
	struct dlm_ls *ls;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	error = dlm_new_lockspace(params->name, strlen(params->name),
				  &lockspace, params->flags, DLM_USER_LVB_LEN);
	if (error)
		return error;

	ls = dlm_find_lockspace_local(lockspace);
	if (!ls)
		return -ENOENT;

	error = create_misc_device(ls, params->name);
	dlm_put_lockspace(ls);

	if (error)
		dlm_release_lockspace(lockspace, 0);
	else
		error = ls->ls_device.minor;

	return error;
}

static int device_remove_lockspace(struct dlm_lspace_params *params)
{
	dlm_lockspace_t *lockspace;
	struct dlm_ls *ls;
	int error, force = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ls = dlm_find_lockspace_device(params->minor);
	if (!ls)
		return -ENOENT;

	/* Deregister the misc device first, so we don't have
	 * a device that's not attached to a lockspace. If
	 * dlm_release_lockspace fails then we can recreate it
	 */
	error = misc_deregister(&ls->ls_device);
	if (error) {
		dlm_put_lockspace(ls);
		goto out;
	}
	kfree(ls->ls_device.name);

	if (params->flags & DLM_USER_LSFLG_FORCEFREE)
		force = 2;

	lockspace = ls->ls_local_handle;

	/* dlm_release_lockspace waits for references to go to zero,
	   so all processes will need to close their device for the ls
	   before the release will procede */

	dlm_put_lockspace(ls);
	error = dlm_release_lockspace(lockspace, force);
	if (error)
		create_misc_device(ls, ls->ls_name);
 out:
	return error;
}

/* Check the user's version matches ours */
static int check_version(struct dlm_write_request *req)
{
	if (req->version[0] != DLM_DEVICE_VERSION_MAJOR ||
	    (req->version[0] == DLM_DEVICE_VERSION_MAJOR &&
	     req->version[1] > DLM_DEVICE_VERSION_MINOR)) {

		printk(KERN_DEBUG "dlm: process %s (%d) version mismatch "
		       "user (%d.%d.%d) kernel (%d.%d.%d)\n",
		       current->comm,
		       task_pid_nr(current),
		       req->version[0],
		       req->version[1],
		       req->version[2],
		       DLM_DEVICE_VERSION_MAJOR,
		       DLM_DEVICE_VERSION_MINOR,
		       DLM_DEVICE_VERSION_PATCH);
		return -EINVAL;
	}
	return 0;
}

/*
 * device_write
 *
 *   device_user_lock
 *     dlm_user_request -> request_lock
 *     dlm_user_convert -> convert_lock
 *
 *   device_user_unlock
 *     dlm_user_unlock -> unlock_lock
 *     dlm_user_cancel -> cancel_lock
 *
 *   device_create_lockspace
 *     dlm_new_lockspace
 *
 *   device_remove_lockspace
 *     dlm_release_lockspace
 */

/* a write to a lockspace device is a lock or unlock request, a write
   to the control device is to create/remove a lockspace */

static ssize_t device_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct dlm_user_proc *proc = file->private_data;
	struct dlm_write_request *kbuf;
	sigset_t tmpsig, allsigs;
	int error;

#ifdef CONFIG_COMPAT
	if (count < sizeof(struct dlm_write_request32))
#else
	if (count < sizeof(struct dlm_write_request))
#endif
		return -EINVAL;

	kbuf = kzalloc(count + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, buf, count)) {
		error = -EFAULT;
		goto out_free;
	}

	if (check_version(kbuf)) {
		error = -EBADE;
		goto out_free;
	}

#ifdef CONFIG_COMPAT
	if (!kbuf->is64bit) {
		struct dlm_write_request32 *k32buf;
		k32buf = (struct dlm_write_request32 *)kbuf;
		kbuf = kmalloc(count + 1 + (sizeof(struct dlm_write_request) -
			       sizeof(struct dlm_write_request32)), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		if (proc)
			set_bit(DLM_PROC_FLAGS_COMPAT, &proc->flags);
		compat_input(kbuf, k32buf, count + 1);
		kfree(k32buf);
	}
#endif

	/* do we really need this? can a write happen after a close? */
	if ((kbuf->cmd == DLM_USER_LOCK || kbuf->cmd == DLM_USER_UNLOCK) &&
	    test_bit(DLM_PROC_FLAGS_CLOSING, &proc->flags))
		return -EINVAL;

	sigfillset(&allsigs);
	sigprocmask(SIG_BLOCK, &allsigs, &tmpsig);

	error = -EINVAL;

	switch (kbuf->cmd)
	{
	case DLM_USER_LOCK:
		if (!proc) {
			log_print("no locking on control device");
			goto out_sig;
		}
		error = device_user_lock(proc, &kbuf->i.lock);
		break;

	case DLM_USER_UNLOCK:
		if (!proc) {
			log_print("no locking on control device");
			goto out_sig;
		}
		error = device_user_unlock(proc, &kbuf->i.lock);
		break;

	case DLM_USER_DEADLOCK:
		if (!proc) {
			log_print("no locking on control device");
			goto out_sig;
		}
		error = device_user_deadlock(proc, &kbuf->i.lock);
		break;

	case DLM_USER_CREATE_LOCKSPACE:
		if (proc) {
			log_print("create/remove only on control device");
			goto out_sig;
		}
		error = device_create_lockspace(&kbuf->i.lspace);
		break;

	case DLM_USER_REMOVE_LOCKSPACE:
		if (proc) {
			log_print("create/remove only on control device");
			goto out_sig;
		}
		error = device_remove_lockspace(&kbuf->i.lspace);
		break;

	case DLM_USER_PURGE:
		if (!proc) {
			log_print("no locking on control device");
			goto out_sig;
		}
		error = device_user_purge(proc, &kbuf->i.purge);
		break;

	default:
		log_print("Unknown command passed to DLM device : %d\n",
			  kbuf->cmd);
	}

 out_sig:
	sigprocmask(SIG_SETMASK, &tmpsig, NULL);
	recalc_sigpending();
 out_free:
	kfree(kbuf);
	return error;
}

/* Every process that opens the lockspace device has its own "proc" structure
   hanging off the open file that's used to keep track of locks owned by the
   process and asts that need to be delivered to the process. */

static int device_open(struct inode *inode, struct file *file)
{
	struct dlm_user_proc *proc;
	struct dlm_ls *ls;

	ls = dlm_find_lockspace_device(iminor(inode));
	if (!ls)
		return -ENOENT;

	proc = kzalloc(sizeof(struct dlm_user_proc), GFP_KERNEL);
	if (!proc) {
		dlm_put_lockspace(ls);
		return -ENOMEM;
	}

	proc->lockspace = ls->ls_local_handle;
	INIT_LIST_HEAD(&proc->asts);
	INIT_LIST_HEAD(&proc->locks);
	INIT_LIST_HEAD(&proc->unlocking);
	spin_lock_init(&proc->asts_spin);
	spin_lock_init(&proc->locks_spin);
	init_waitqueue_head(&proc->wait);
	file->private_data = proc;

	return 0;
}

static int device_close(struct inode *inode, struct file *file)
{
	struct dlm_user_proc *proc = file->private_data;
	struct dlm_ls *ls;
	sigset_t tmpsig, allsigs;

	ls = dlm_find_lockspace_local(proc->lockspace);
	if (!ls)
		return -ENOENT;

	sigfillset(&allsigs);
	sigprocmask(SIG_BLOCK, &allsigs, &tmpsig);

	set_bit(DLM_PROC_FLAGS_CLOSING, &proc->flags);

	dlm_clear_proc_locks(ls, proc);

	/* at this point no more lkb's should exist for this lockspace,
	   so there's no chance of dlm_user_add_ast() being called and
	   looking for lkb->ua->proc */

	kfree(proc);
	file->private_data = NULL;

	dlm_put_lockspace(ls);
	dlm_put_lockspace(ls);  /* for the find in device_open() */

	/* FIXME: AUTOFREE: if this ls is no longer used do
	   device_remove_lockspace() */

	sigprocmask(SIG_SETMASK, &tmpsig, NULL);
	recalc_sigpending();

	return 0;
}

static int copy_result_to_user(struct dlm_user_args *ua, int compat, int type,
			       int bmode, char __user *buf, size_t count)
{
#ifdef CONFIG_COMPAT
	struct dlm_lock_result32 result32;
#endif
	struct dlm_lock_result result;
	void *resultptr;
	int error=0;
	int len;
	int struct_len;

	memset(&result, 0, sizeof(struct dlm_lock_result));
	result.version[0] = DLM_DEVICE_VERSION_MAJOR;
	result.version[1] = DLM_DEVICE_VERSION_MINOR;
	result.version[2] = DLM_DEVICE_VERSION_PATCH;
	memcpy(&result.lksb, &ua->lksb, sizeof(struct dlm_lksb));
	result.user_lksb = ua->user_lksb;

	/* FIXME: dlm1 provides for the user's bastparam/addr to not be updated
	   in a conversion unless the conversion is successful.  See code
	   in dlm_user_convert() for updating ua from ua_tmp.  OpenVMS, though,
	   notes that a new blocking AST address and parameter are set even if
	   the conversion fails, so maybe we should just do that. */

	if (type == AST_BAST) {
		result.user_astaddr = ua->bastaddr;
		result.user_astparam = ua->bastparam;
		result.bast_mode = bmode;
	} else {
		result.user_astaddr = ua->castaddr;
		result.user_astparam = ua->castparam;
	}

#ifdef CONFIG_COMPAT
	if (compat)
		len = sizeof(struct dlm_lock_result32);
	else
#endif
		len = sizeof(struct dlm_lock_result);
	struct_len = len;

	/* copy lvb to userspace if there is one, it's been updated, and
	   the user buffer has space for it */

	if (ua->update_user_lvb && ua->lksb.sb_lvbptr &&
	    count >= len + DLM_USER_LVB_LEN) {
		if (copy_to_user(buf+len, ua->lksb.sb_lvbptr,
				 DLM_USER_LVB_LEN)) {
			error = -EFAULT;
			goto out;
		}

		result.lvb_offset = len;
		len += DLM_USER_LVB_LEN;
	}

	result.length = len;
	resultptr = &result;
#ifdef CONFIG_COMPAT
	if (compat) {
		compat_output(&result, &result32);
		resultptr = &result32;
	}
#endif

	if (copy_to_user(buf, resultptr, struct_len))
		error = -EFAULT;
	else
		error = len;
 out:
	return error;
}

static int copy_version_to_user(char __user *buf, size_t count)
{
	struct dlm_device_version ver;

	memset(&ver, 0, sizeof(struct dlm_device_version));
	ver.version[0] = DLM_DEVICE_VERSION_MAJOR;
	ver.version[1] = DLM_DEVICE_VERSION_MINOR;
	ver.version[2] = DLM_DEVICE_VERSION_PATCH;

	if (copy_to_user(buf, &ver, sizeof(struct dlm_device_version)))
		return -EFAULT;
	return sizeof(struct dlm_device_version);
}

/* a read returns a single ast described in a struct dlm_lock_result */

static ssize_t device_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct dlm_user_proc *proc = file->private_data;
	struct dlm_lkb *lkb;
	DECLARE_WAITQUEUE(wait, current);
	int error, type=0, bmode=0, removed = 0;

	if (count == sizeof(struct dlm_device_version)) {
		error = copy_version_to_user(buf, count);
		return error;
	}

	if (!proc) {
		log_print("non-version read from control device %zu", count);
		return -EINVAL;
	}

#ifdef CONFIG_COMPAT
	if (count < sizeof(struct dlm_lock_result32))
#else
	if (count < sizeof(struct dlm_lock_result))
#endif
		return -EINVAL;

	/* do we really need this? can a read happen after a close? */
	if (test_bit(DLM_PROC_FLAGS_CLOSING, &proc->flags))
		return -EINVAL;

	spin_lock(&proc->asts_spin);
	if (list_empty(&proc->asts)) {
		if (file->f_flags & O_NONBLOCK) {
			spin_unlock(&proc->asts_spin);
			return -EAGAIN;
		}

		add_wait_queue(&proc->wait, &wait);

	repeat:
		set_current_state(TASK_INTERRUPTIBLE);
		if (list_empty(&proc->asts) && !signal_pending(current)) {
			spin_unlock(&proc->asts_spin);
			schedule();
			spin_lock(&proc->asts_spin);
			goto repeat;
		}
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&proc->wait, &wait);

		if (signal_pending(current)) {
			spin_unlock(&proc->asts_spin);
			return -ERESTARTSYS;
		}
	}

	/* there may be both completion and blocking asts to return for
	   the lkb, don't remove lkb from asts list unless no asts remain */

	lkb = list_entry(proc->asts.next, struct dlm_lkb, lkb_astqueue);

	if (lkb->lkb_ast_type & AST_COMP) {
		lkb->lkb_ast_type &= ~AST_COMP;
		type = AST_COMP;
	} else if (lkb->lkb_ast_type & AST_BAST) {
		lkb->lkb_ast_type &= ~AST_BAST;
		type = AST_BAST;
		bmode = lkb->lkb_bastmode;
	}

	if (!lkb->lkb_ast_type) {
		list_del(&lkb->lkb_astqueue);
		removed = 1;
	}
	spin_unlock(&proc->asts_spin);

	error = copy_result_to_user(lkb->lkb_ua,
			 	test_bit(DLM_PROC_FLAGS_COMPAT, &proc->flags),
				type, bmode, buf, count);

	/* removes reference for the proc->asts lists added by
	   dlm_user_add_ast() and may result in the lkb being freed */
	if (removed)
		dlm_put_lkb(lkb);

	return error;
}

static unsigned int device_poll(struct file *file, poll_table *wait)
{
	struct dlm_user_proc *proc = file->private_data;

	poll_wait(file, &proc->wait, wait);

	spin_lock(&proc->asts_spin);
	if (!list_empty(&proc->asts)) {
		spin_unlock(&proc->asts_spin);
		return POLLIN | POLLRDNORM;
	}
	spin_unlock(&proc->asts_spin);
	return 0;
}

static int ctl_device_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int ctl_device_close(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations device_fops = {
	.open    = device_open,
	.release = device_close,
	.read    = device_read,
	.write   = device_write,
	.poll    = device_poll,
	.owner   = THIS_MODULE,
};

static const struct file_operations ctl_device_fops = {
	.open    = ctl_device_open,
	.release = ctl_device_close,
	.read    = device_read,
	.write   = device_write,
	.owner   = THIS_MODULE,
};

static struct miscdevice ctl_device = {
	.name  = "dlm-control",
	.fops  = &ctl_device_fops,
	.minor = MISC_DYNAMIC_MINOR,
};

int dlm_user_init(void)
{
	int error;

	error = misc_register(&ctl_device);
	if (error)
		log_print("misc_register failed for control device");

	return error;
}

void dlm_user_exit(void)
{
	misc_deregister(&ctl_device);
}

