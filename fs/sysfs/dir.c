/*
 * fs/sysfs/dir.c - sysfs core and dir operation implementation
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007 Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#undef DEBUG

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/idr.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/security.h>
#include "sysfs.h"

DEFINE_MUTEX(sysfs_mutex);
DEFINE_SPINLOCK(sysfs_assoc_lock);

static DEFINE_SPINLOCK(sysfs_ino_lock);
static DEFINE_IDA(sysfs_ino_ida);

/**
 *	sysfs_link_sibling - link sysfs_dirent into sibling list
 *	@sd: sysfs_dirent of interest
 *
 *	Link @sd into its sibling list which starts from
 *	sd->s_parent->s_dir.children.
 *
 *	Locking:
 *	mutex_lock(sysfs_mutex)
 */
static void sysfs_link_sibling(struct sysfs_dirent *sd)
{
	struct sysfs_dirent *parent_sd = sd->s_parent;
	struct sysfs_dirent **pos;

	BUG_ON(sd->s_sibling);

	/* Store directory entries in order by ino.  This allows
	 * readdir to properly restart without having to add a
	 * cursor into the s_dir.children list.
	 */
	for (pos = &parent_sd->s_dir.children; *pos; pos = &(*pos)->s_sibling) {
		if (sd->s_ino < (*pos)->s_ino)
			break;
	}
	sd->s_sibling = *pos;
	*pos = sd;
}

/**
 *	sysfs_unlink_sibling - unlink sysfs_dirent from sibling list
 *	@sd: sysfs_dirent of interest
 *
 *	Unlink @sd from its sibling list which starts from
 *	sd->s_parent->s_dir.children.
 *
 *	Locking:
 *	mutex_lock(sysfs_mutex)
 */
static void sysfs_unlink_sibling(struct sysfs_dirent *sd)
{
	struct sysfs_dirent **pos;

	for (pos = &sd->s_parent->s_dir.children; *pos;
	     pos = &(*pos)->s_sibling) {
		if (*pos == sd) {
			*pos = sd->s_sibling;
			sd->s_sibling = NULL;
			break;
		}
	}
}

/**
 *	sysfs_get_active - get an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to get an active reference to
 *
 *	Get an active reference of @sd.  This function is noop if @sd
 *	is NULL.
 *
 *	RETURNS:
 *	Pointer to @sd on success, NULL on failure.
 */
static struct sysfs_dirent *sysfs_get_active(struct sysfs_dirent *sd)
{
	if (unlikely(!sd))
		return NULL;

	while (1) {
		int v, t;

		v = atomic_read(&sd->s_active);
		if (unlikely(v < 0))
			return NULL;

		t = atomic_cmpxchg(&sd->s_active, v, v + 1);
		if (likely(t == v)) {
			rwsem_acquire_read(&sd->dep_map, 0, 1, _RET_IP_);
			return sd;
		}
		if (t < 0)
			return NULL;

		cpu_relax();
	}
}

/**
 *	sysfs_put_active - put an active reference to sysfs_dirent
 *	@sd: sysfs_dirent to put an active reference to
 *
 *	Put an active reference to @sd.  This function is noop if @sd
 *	is NULL.
 */
static void sysfs_put_active(struct sysfs_dirent *sd)
{
	struct completion *cmpl;
	int v;

	if (unlikely(!sd))
		return;

	rwsem_release(&sd->dep_map, 1, _RET_IP_);
	v = atomic_dec_return(&sd->s_active);
	if (likely(v != SD_DEACTIVATED_BIAS))
		return;

	/* atomic_dec_return() is a mb(), we'll always see the updated
	 * sd->s_sibling.
	 */
	cmpl = (void *)sd->s_sibling;
	complete(cmpl);
}

/**
 *	sysfs_get_active_two - get active references to sysfs_dirent and parent
 *	@sd: sysfs_dirent of interest
 *
 *	Get active reference to @sd and its parent.  Parent's active
 *	reference is grabbed first.  This function is noop if @sd is
 *	NULL.
 *
 *	RETURNS:
 *	Pointer to @sd on success, NULL on failure.
 */
struct sysfs_dirent *sysfs_get_active_two(struct sysfs_dirent *sd)
{
	if (sd) {
		if (sd->s_parent && unlikely(!sysfs_get_active(sd->s_parent)))
			return NULL;
		if (unlikely(!sysfs_get_active(sd))) {
			sysfs_put_active(sd->s_parent);
			return NULL;
		}
	}
	return sd;
}

/**
 *	sysfs_put_active_two - put active references to sysfs_dirent and parent
 *	@sd: sysfs_dirent of interest
 *
 *	Put active references to @sd and its parent.  This function is
 *	noop if @sd is NULL.
 */
void sysfs_put_active_two(struct sysfs_dirent *sd)
{
	if (sd) {
		sysfs_put_active(sd);
		sysfs_put_active(sd->s_parent);
	}
}

/**
 *	sysfs_deactivate - deactivate sysfs_dirent
 *	@sd: sysfs_dirent to deactivate
 *
 *	Deny new active references and drain existing ones.
 */
static void sysfs_deactivate(struct sysfs_dirent *sd)
{
	DECLARE_COMPLETION_ONSTACK(wait);
	int v;

	BUG_ON(sd->s_sibling || !(sd->s_flags & SYSFS_FLAG_REMOVED));
	sd->s_sibling = (void *)&wait;

	rwsem_acquire(&sd->dep_map, 0, 0, _RET_IP_);
	/* atomic_add_return() is a mb(), put_active() will always see
	 * the updated sd->s_sibling.
	 */
	v = atomic_add_return(SD_DEACTIVATED_BIAS, &sd->s_active);

	if (v != SD_DEACTIVATED_BIAS) {
		lock_contended(&sd->dep_map, _RET_IP_);
		wait_for_completion(&wait);
	}

	sd->s_sibling = NULL;

	lock_acquired(&sd->dep_map, _RET_IP_);
	rwsem_release(&sd->dep_map, 1, _RET_IP_);
}

static int sysfs_alloc_ino(ino_t *pino)
{
	int ino, rc;

 retry:
	spin_lock(&sysfs_ino_lock);
	rc = ida_get_new_above(&sysfs_ino_ida, 2, &ino);
	spin_unlock(&sysfs_ino_lock);

	if (rc == -EAGAIN) {
		if (ida_pre_get(&sysfs_ino_ida, GFP_KERNEL))
			goto retry;
		rc = -ENOMEM;
	}

	*pino = ino;
	return rc;
}

static void sysfs_free_ino(ino_t ino)
{
	spin_lock(&sysfs_ino_lock);
	ida_remove(&sysfs_ino_ida, ino);
	spin_unlock(&sysfs_ino_lock);
}

void release_sysfs_dirent(struct sysfs_dirent * sd)
{
	struct sysfs_dirent *parent_sd;

 repeat:
	/* Moving/renaming is always done while holding reference.
	 * sd->s_parent won't change beneath us.
	 */
	parent_sd = sd->s_parent;

	if (sysfs_type(sd) == SYSFS_KOBJ_LINK)
		sysfs_put(sd->s_symlink.target_sd);
	if (sysfs_type(sd) & SYSFS_COPY_NAME)
		kfree(sd->s_name);
	if (sd->s_iattr && sd->s_iattr->ia_secdata)
		security_release_secctx(sd->s_iattr->ia_secdata,
					sd->s_iattr->ia_secdata_len);
	kfree(sd->s_iattr);
	sysfs_free_ino(sd->s_ino);
	kmem_cache_free(sysfs_dir_cachep, sd);

	sd = parent_sd;
	if (sd && atomic_dec_and_test(&sd->s_count))
		goto repeat;
}

static int sysfs_dentry_delete(struct dentry *dentry)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	return !!(sd->s_flags & SYSFS_FLAG_REMOVED);
}

static int sysfs_dentry_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct sysfs_dirent *sd = dentry->d_fsdata;
	int is_dir;

	mutex_lock(&sysfs_mutex);

	/* The sysfs dirent has been deleted */
	if (sd->s_flags & SYSFS_FLAG_REMOVED)
		goto out_bad;

	/* The sysfs dirent has been moved? */
	if (dentry->d_parent->d_fsdata != sd->s_parent)
		goto out_bad;

	/* The sysfs dirent has been renamed */
	if (strcmp(dentry->d_name.name, sd->s_name) != 0)
		goto out_bad;

	mutex_unlock(&sysfs_mutex);
out_valid:
	return 1;
out_bad:
	/* Remove the dentry from the dcache hashes.
	 * If this is a deleted dentry we use d_drop instead of d_delete
	 * so sysfs doesn't need to cope with negative dentries.
	 *
	 * If this is a dentry that has simply been renamed we
	 * use d_drop to remove it from the dcache lookup on its
	 * old parent.  If this dentry persists later when a lookup
	 * is performed at its new name the dentry will be readded
	 * to the dcache hashes.
	 */
	is_dir = (sysfs_type(sd) == SYSFS_DIR);
	mutex_unlock(&sysfs_mutex);
	if (is_dir) {
		/* If we have submounts we must allow the vfs caches
		 * to lie about the state of the filesystem to prevent
		 * leaks and other nasty things.
		 */
		if (have_submounts(dentry))
			goto out_valid;
		shrink_dcache_parent(dentry);
	}
	d_drop(dentry);
	return 0;
}

static void sysfs_dentry_iput(struct dentry *dentry, struct inode *inode)
{
	struct sysfs_dirent * sd = dentry->d_fsdata;

	sysfs_put(sd);
	iput(inode);
}

static const struct dentry_operations sysfs_dentry_ops = {
	.d_revalidate	= sysfs_dentry_revalidate,
	.d_delete	= sysfs_dentry_delete,
	.d_iput		= sysfs_dentry_iput,
};

struct sysfs_dirent *sysfs_new_dirent(const char *name, umode_t mode, int type)
{
	char *dup_name = NULL;
	struct sysfs_dirent *sd;

	if (type & SYSFS_COPY_NAME) {
		name = dup_name = kstrdup(name, GFP_KERNEL);
		if (!name)
			return NULL;
	}

	sd = kmem_cache_zalloc(sysfs_dir_cachep, GFP_KERNEL);
	if (!sd)
		goto err_out1;

	if (sysfs_alloc_ino(&sd->s_ino))
		goto err_out2;

	atomic_set(&sd->s_count, 1);
	atomic_set(&sd->s_active, 0);
	sysfs_dirent_init_lockdep(sd);

	sd->s_name = name;
	sd->s_mode = mode;
	sd->s_flags = type;

	return sd;

 err_out2:
	kmem_cache_free(sysfs_dir_cachep, sd);
 err_out1:
	kfree(dup_name);
	return NULL;
}

/**
 *	sysfs_addrm_start - prepare for sysfs_dirent add/remove
 *	@acxt: pointer to sysfs_addrm_cxt to be used
 *	@parent_sd: parent sysfs_dirent
 *
 *	This function is called when the caller is about to add or
 *	remove sysfs_dirent under @parent_sd.  This function acquires
 *	sysfs_mutex.  @acxt is used to keep and pass context to
 *	other addrm functions.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).  sysfs_mutex is locked on
 *	return.
 */
void sysfs_addrm_start(struct sysfs_addrm_cxt *acxt,
		       struct sysfs_dirent *parent_sd)
{
	memset(acxt, 0, sizeof(*acxt));
	acxt->parent_sd = parent_sd;

	mutex_lock(&sysfs_mutex);
}

/**
 *	__sysfs_add_one - add sysfs_dirent to parent without warning
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be added
 *
 *	Get @acxt->parent_sd and set sd->s_parent to it and increment
 *	nlink of parent inode if @sd is a directory and link into the
 *	children list of the parent.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 *
 *	RETURNS:
 *	0 on success, -EEXIST if entry with the given name already
 *	exists.
 */
int __sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *ps_iattr;

	if (sysfs_find_dirent(acxt->parent_sd, sd->s_name))
		return -EEXIST;

	sd->s_parent = sysfs_get(acxt->parent_sd);

	sysfs_link_sibling(sd);

	/* Update timestamps on the parent */
	ps_iattr = acxt->parent_sd->s_iattr;
	if (ps_iattr) {
		struct iattr *ps_iattrs = &ps_iattr->ia_iattr;
		ps_iattrs->ia_ctime = ps_iattrs->ia_mtime = CURRENT_TIME;
	}

	return 0;
}

/**
 *	sysfs_pathname - return full path to sysfs dirent
 *	@sd: sysfs_dirent whose path we want
 *	@path: caller allocated buffer
 *
 *	Gives the name "/" to the sysfs_root entry; any path returned
 *	is relative to wherever sysfs is mounted.
 *
 *	XXX: does no error checking on @path size
 */
static char *sysfs_pathname(struct sysfs_dirent *sd, char *path)
{
	if (sd->s_parent) {
		sysfs_pathname(sd->s_parent, path);
		strcat(path, "/");
	}
	strcat(path, sd->s_name);
	return path;
}

/**
 *	sysfs_add_one - add sysfs_dirent to parent
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be added
 *
 *	Get @acxt->parent_sd and set sd->s_parent to it and increment
 *	nlink of parent inode if @sd is a directory and link into the
 *	children list of the parent.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 *
 *	RETURNS:
 *	0 on success, -EEXIST if entry with the given name already
 *	exists.
 */
int sysfs_add_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	int ret;

	ret = __sysfs_add_one(acxt, sd);
	if (ret == -EEXIST) {
		char *path = kzalloc(PATH_MAX, GFP_KERNEL);
		WARN(1, KERN_WARNING
		     "sysfs: cannot create duplicate filename '%s'\n",
		     (path == NULL) ? sd->s_name :
		     strcat(strcat(sysfs_pathname(acxt->parent_sd, path), "/"),
		            sd->s_name));
		kfree(path);
	}

	return ret;
}

/**
 *	sysfs_remove_one - remove sysfs_dirent from parent
 *	@acxt: addrm context to use
 *	@sd: sysfs_dirent to be removed
 *
 *	Mark @sd removed and drop nlink of parent inode if @sd is a
 *	directory.  @sd is unlinked from the children list.
 *
 *	This function should be called between calls to
 *	sysfs_addrm_start() and sysfs_addrm_finish() and should be
 *	passed the same @acxt as passed to sysfs_addrm_start().
 *
 *	LOCKING:
 *	Determined by sysfs_addrm_start().
 */
void sysfs_remove_one(struct sysfs_addrm_cxt *acxt, struct sysfs_dirent *sd)
{
	struct sysfs_inode_attrs *ps_iattr;

	BUG_ON(sd->s_flags & SYSFS_FLAG_REMOVED);

	sysfs_unlink_sibling(sd);

	/* Update timestamps on the parent */
	ps_iattr = acxt->parent_sd->s_iattr;
	if (ps_iattr) {
		struct iattr *ps_iattrs = &ps_iattr->ia_iattr;
		ps_iattrs->ia_ctime = ps_iattrs->ia_mtime = CURRENT_TIME;
	}

	sd->s_flags |= SYSFS_FLAG_REMOVED;
	sd->s_sibling = acxt->removed;
	acxt->removed = sd;
}

/**
 *	sysfs_addrm_finish - finish up sysfs_dirent add/remove
 *	@acxt: addrm context to finish up
 *
 *	Finish up sysfs_dirent add/remove.  Resources acquired by
 *	sysfs_addrm_start() are released and removed sysfs_dirents are
 *	cleaned up.
 *
 *	LOCKING:
 *	sysfs_mutex is released.
 */
void sysfs_addrm_finish(struct sysfs_addrm_cxt *acxt)
{
	/* release resources acquired by sysfs_addrm_start() */
	mutex_unlock(&sysfs_mutex);

	/* kill removed sysfs_dirents */
	while (acxt->removed) {
		struct sysfs_dirent *sd = acxt->removed;

		acxt->removed = sd->s_sibling;
		sd->s_sibling = NULL;

		sysfs_deactivate(sd);
		unmap_bin_file(sd);
		sysfs_put(sd);
	}
}

/**
 *	sysfs_find_dirent - find sysfs_dirent with the given name
 *	@parent_sd: sysfs_dirent to search under
 *	@name: name to look for
 *
 *	Look for sysfs_dirent with name @name under @parent_sd.
 *
 *	LOCKING:
 *	mutex_lock(sysfs_mutex)
 *
 *	RETURNS:
 *	Pointer to sysfs_dirent if found, NULL if not.
 */
struct sysfs_dirent *sysfs_find_dirent(struct sysfs_dirent *parent_sd,
				       const unsigned char *name)
{
	struct sysfs_dirent *sd;

	for (sd = parent_sd->s_dir.children; sd; sd = sd->s_sibling)
		if (!strcmp(sd->s_name, name))
			return sd;
	return NULL;
}

/**
 *	sysfs_get_dirent - find and get sysfs_dirent with the given name
 *	@parent_sd: sysfs_dirent to search under
 *	@name: name to look for
 *
 *	Look for sysfs_dirent with name @name under @parent_sd and get
 *	it if found.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).  Grabs sysfs_mutex.
 *
 *	RETURNS:
 *	Pointer to sysfs_dirent if found, NULL if not.
 */
struct sysfs_dirent *sysfs_get_dirent(struct sysfs_dirent *parent_sd,
				      const unsigned char *name)
{
	struct sysfs_dirent *sd;

	mutex_lock(&sysfs_mutex);
	sd = sysfs_find_dirent(parent_sd, name);
	sysfs_get(sd);
	mutex_unlock(&sysfs_mutex);

	return sd;
}
EXPORT_SYMBOL_GPL(sysfs_get_dirent);

static int create_dir(struct kobject *kobj, struct sysfs_dirent *parent_sd,
		      const char *name, struct sysfs_dirent **p_sd)
{
	umode_t mode = S_IFDIR| S_IRWXU | S_IRUGO | S_IXUGO;
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent *sd;
	int rc;

	/* allocate */
	sd = sysfs_new_dirent(name, mode, SYSFS_DIR);
	if (!sd)
		return -ENOMEM;
	sd->s_dir.kobj = kobj;

	/* link in */
	sysfs_addrm_start(&acxt, parent_sd);
	rc = sysfs_add_one(&acxt, sd);
	sysfs_addrm_finish(&acxt);

	if (rc == 0)
		*p_sd = sd;
	else
		sysfs_put(sd);

	return rc;
}

int sysfs_create_subdir(struct kobject *kobj, const char *name,
			struct sysfs_dirent **p_sd)
{
	return create_dir(kobj, kobj->sd, name, p_sd);
}

/**
 *	sysfs_create_dir - create a directory for an object.
 *	@kobj:		object we're creating directory for. 
 */
int sysfs_create_dir(struct kobject * kobj)
{
	struct sysfs_dirent *parent_sd, *sd;
	int error = 0;

	BUG_ON(!kobj);

	if (kobj->parent)
		parent_sd = kobj->parent->sd;
	else
		parent_sd = &sysfs_root;

	error = create_dir(kobj, parent_sd, kobject_name(kobj), &sd);
	if (!error)
		kobj->sd = sd;
	return error;
}

static struct dentry * sysfs_lookup(struct inode *dir, struct dentry *dentry,
				struct nameidata *nd)
{
	struct dentry *ret = NULL;
	struct sysfs_dirent *parent_sd = dentry->d_parent->d_fsdata;
	struct sysfs_dirent *sd;
	struct inode *inode;

	mutex_lock(&sysfs_mutex);

	sd = sysfs_find_dirent(parent_sd, dentry->d_name.name);

	/* no such entry */
	if (!sd) {
		ret = ERR_PTR(-ENOENT);
		goto out_unlock;
	}

	/* attach dentry and inode */
	inode = sysfs_get_inode(sd);
	if (!inode) {
		ret = ERR_PTR(-ENOMEM);
		goto out_unlock;
	}

	/* instantiate and hash dentry */
	ret = d_find_alias(inode);
	if (!ret) {
		dentry->d_op = &sysfs_dentry_ops;
		dentry->d_fsdata = sysfs_get(sd);
		d_add(dentry, inode);
	} else {
		d_move(ret, dentry);
		iput(inode);
	}

 out_unlock:
	mutex_unlock(&sysfs_mutex);
	return ret;
}

const struct inode_operations sysfs_dir_inode_operations = {
	.lookup		= sysfs_lookup,
	.permission	= sysfs_permission,
	.setattr	= sysfs_setattr,
	.getattr	= sysfs_getattr,
	.setxattr	= sysfs_setxattr,
};

static void remove_dir(struct sysfs_dirent *sd)
{
	struct sysfs_addrm_cxt acxt;

	sysfs_addrm_start(&acxt, sd->s_parent);
	sysfs_remove_one(&acxt, sd);
	sysfs_addrm_finish(&acxt);
}

void sysfs_remove_subdir(struct sysfs_dirent *sd)
{
	remove_dir(sd);
}


static void __sysfs_remove_dir(struct sysfs_dirent *dir_sd)
{
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent **pos;

	if (!dir_sd)
		return;

	pr_debug("sysfs %s: removing dir\n", dir_sd->s_name);
	sysfs_addrm_start(&acxt, dir_sd);
	pos = &dir_sd->s_dir.children;
	while (*pos) {
		struct sysfs_dirent *sd = *pos;

		if (sysfs_type(sd) != SYSFS_DIR)
			sysfs_remove_one(&acxt, sd);
		else
			pos = &(*pos)->s_sibling;
	}
	sysfs_addrm_finish(&acxt);

	remove_dir(dir_sd);
}

/**
 *	sysfs_remove_dir - remove an object's directory.
 *	@kobj:	object.
 *
 *	The only thing special about this is that we remove any files in
 *	the directory before we remove the directory, and we've inlined
 *	what used to be sysfs_rmdir() below, instead of calling separately.
 */

void sysfs_remove_dir(struct kobject * kobj)
{
	struct sysfs_dirent *sd = kobj->sd;

	spin_lock(&sysfs_assoc_lock);
	kobj->sd = NULL;
	spin_unlock(&sysfs_assoc_lock);

	__sysfs_remove_dir(sd);
}

int sysfs_rename(struct sysfs_dirent *sd,
	struct sysfs_dirent *new_parent_sd, const char *new_name)
{
	const char *dup_name = NULL;
	int error;

	mutex_lock(&sysfs_mutex);

	error = 0;
	if ((sd->s_parent == new_parent_sd) &&
	    (strcmp(sd->s_name, new_name) == 0))
		goto out;	/* nothing to rename */

	error = -EEXIST;
	if (sysfs_find_dirent(new_parent_sd, new_name))
		goto out;

	/* rename sysfs_dirent */
	if (strcmp(sd->s_name, new_name) != 0) {
		error = -ENOMEM;
		new_name = dup_name = kstrdup(new_name, GFP_KERNEL);
		if (!new_name)
			goto out;

		dup_name = sd->s_name;
		sd->s_name = new_name;
	}

	/* Remove from old parent's list and insert into new parent's list. */
	if (sd->s_parent != new_parent_sd) {
		sysfs_unlink_sibling(sd);
		sysfs_get(new_parent_sd);
		sysfs_put(sd->s_parent);
		sd->s_parent = new_parent_sd;
		sysfs_link_sibling(sd);
	}

	error = 0;
 out:
	mutex_unlock(&sysfs_mutex);
	kfree(dup_name);
	return error;
}

int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
	return sysfs_rename(kobj->sd, kobj->sd->s_parent, new_name);
}

int sysfs_move_dir(struct kobject *kobj, struct kobject *new_parent_kobj)
{
	struct sysfs_dirent *sd = kobj->sd;
	struct sysfs_dirent *new_parent_sd;

	BUG_ON(!sd->s_parent);
	new_parent_sd = new_parent_kobj && new_parent_kobj->sd ?
		new_parent_kobj->sd : &sysfs_root;

	return sysfs_rename(sd, new_parent_sd, sd->s_name);
}

/* Relationship between s_mode and the DT_xxx types */
static inline unsigned char dt_type(struct sysfs_dirent *sd)
{
	return (sd->s_mode >> 12) & 15;
}

static int sysfs_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry;
	struct sysfs_dirent * parent_sd = dentry->d_fsdata;
	struct sysfs_dirent *pos;
	ino_t ino;

	if (filp->f_pos == 0) {
		ino = parent_sd->s_ino;
		if (filldir(dirent, ".", 1, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;
	}
	if (filp->f_pos == 1) {
		if (parent_sd->s_parent)
			ino = parent_sd->s_parent->s_ino;
		else
			ino = parent_sd->s_ino;
		if (filldir(dirent, "..", 2, filp->f_pos, ino, DT_DIR) == 0)
			filp->f_pos++;
	}
	if ((filp->f_pos > 1) && (filp->f_pos < INT_MAX)) {
		mutex_lock(&sysfs_mutex);

		/* Skip the dentries we have already reported */
		pos = parent_sd->s_dir.children;
		while (pos && (filp->f_pos > pos->s_ino))
			pos = pos->s_sibling;

		for ( ; pos; pos = pos->s_sibling) {
			const char * name;
			int len;

			name = pos->s_name;
			len = strlen(name);
			filp->f_pos = ino = pos->s_ino;

			if (filldir(dirent, name, len, filp->f_pos, ino,
					 dt_type(pos)) < 0)
				break;
		}
		if (!pos)
			filp->f_pos = INT_MAX;
		mutex_unlock(&sysfs_mutex);
	}
	return 0;
}


const struct file_operations sysfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= sysfs_readdir,
	.llseek		= generic_file_llseek,
};
