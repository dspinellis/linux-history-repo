/* Internal definitions for FS-Cache
 *
 * Copyright (C) 2004-2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/*
 * Lock order, in the order in which multiple locks should be obtained:
 * - fscache_addremove_sem
 * - cookie->lock
 * - cookie->parent->lock
 * - cache->object_list_lock
 * - object->lock
 * - object->parent->lock
 * - fscache_thread_lock
 *
 */

#include <linux/fscache-cache.h>
#include <linux/sched.h>

#define FSCACHE_MIN_THREADS	4
#define FSCACHE_MAX_THREADS	32

/*
 * fsc-histogram.c
 */
#ifdef CONFIG_FSCACHE_HISTOGRAM
extern atomic_t fscache_obj_instantiate_histogram[HZ];
extern atomic_t fscache_objs_histogram[HZ];
extern atomic_t fscache_ops_histogram[HZ];
extern atomic_t fscache_retrieval_delay_histogram[HZ];
extern atomic_t fscache_retrieval_histogram[HZ];

static inline void fscache_hist(atomic_t histogram[], unsigned long start_jif)
{
	unsigned long jif = jiffies - start_jif;
	if (jif >= HZ)
		jif = HZ - 1;
	atomic_inc(&histogram[jif]);
}

extern const struct file_operations fscache_histogram_fops;

#else
#define fscache_hist(hist, start_jif) do {} while (0)
#endif

/*
 * fsc-main.c
 */
extern unsigned fscache_defer_lookup;
extern unsigned fscache_defer_create;
extern unsigned fscache_debug;
extern struct kobject *fscache_root;

/*
 * fsc-proc.c
 */
#ifdef CONFIG_PROC_FS
extern int __init fscache_proc_init(void);
extern void fscache_proc_cleanup(void);
#else
#define fscache_proc_init()	(0)
#define fscache_proc_cleanup()	do {} while (0)
#endif

/*
 * fsc-stats.c
 */
#ifdef CONFIG_FSCACHE_STATS
extern atomic_t fscache_n_ops_processed[FSCACHE_MAX_THREADS];
extern atomic_t fscache_n_objs_processed[FSCACHE_MAX_THREADS];

extern atomic_t fscache_n_op_pend;
extern atomic_t fscache_n_op_run;
extern atomic_t fscache_n_op_enqueue;
extern atomic_t fscache_n_op_deferred_release;
extern atomic_t fscache_n_op_release;
extern atomic_t fscache_n_op_gc;

extern atomic_t fscache_n_attr_changed;
extern atomic_t fscache_n_attr_changed_ok;
extern atomic_t fscache_n_attr_changed_nobufs;
extern atomic_t fscache_n_attr_changed_nomem;
extern atomic_t fscache_n_attr_changed_calls;

extern atomic_t fscache_n_allocs;
extern atomic_t fscache_n_allocs_ok;
extern atomic_t fscache_n_allocs_wait;
extern atomic_t fscache_n_allocs_nobufs;
extern atomic_t fscache_n_alloc_ops;
extern atomic_t fscache_n_alloc_op_waits;

extern atomic_t fscache_n_retrievals;
extern atomic_t fscache_n_retrievals_ok;
extern atomic_t fscache_n_retrievals_wait;
extern atomic_t fscache_n_retrievals_nodata;
extern atomic_t fscache_n_retrievals_nobufs;
extern atomic_t fscache_n_retrievals_intr;
extern atomic_t fscache_n_retrievals_nomem;
extern atomic_t fscache_n_retrieval_ops;
extern atomic_t fscache_n_retrieval_op_waits;

extern atomic_t fscache_n_stores;
extern atomic_t fscache_n_stores_ok;
extern atomic_t fscache_n_stores_again;
extern atomic_t fscache_n_stores_nobufs;
extern atomic_t fscache_n_stores_oom;
extern atomic_t fscache_n_store_ops;
extern atomic_t fscache_n_store_calls;

extern atomic_t fscache_n_marks;
extern atomic_t fscache_n_uncaches;

extern atomic_t fscache_n_acquires;
extern atomic_t fscache_n_acquires_null;
extern atomic_t fscache_n_acquires_no_cache;
extern atomic_t fscache_n_acquires_ok;
extern atomic_t fscache_n_acquires_nobufs;
extern atomic_t fscache_n_acquires_oom;

extern atomic_t fscache_n_updates;
extern atomic_t fscache_n_updates_null;
extern atomic_t fscache_n_updates_run;

extern atomic_t fscache_n_relinquishes;
extern atomic_t fscache_n_relinquishes_null;
extern atomic_t fscache_n_relinquishes_waitcrt;

extern atomic_t fscache_n_cookie_index;
extern atomic_t fscache_n_cookie_data;
extern atomic_t fscache_n_cookie_special;

extern atomic_t fscache_n_object_alloc;
extern atomic_t fscache_n_object_no_alloc;
extern atomic_t fscache_n_object_lookups;
extern atomic_t fscache_n_object_lookups_negative;
extern atomic_t fscache_n_object_lookups_positive;
extern atomic_t fscache_n_object_created;
extern atomic_t fscache_n_object_avail;
extern atomic_t fscache_n_object_dead;

extern atomic_t fscache_n_checkaux_none;
extern atomic_t fscache_n_checkaux_okay;
extern atomic_t fscache_n_checkaux_update;
extern atomic_t fscache_n_checkaux_obsolete;

static inline void fscache_stat(atomic_t *stat)
{
	atomic_inc(stat);
}

extern const struct file_operations fscache_stats_fops;
#else

#define fscache_stat(stat) do {} while (0)
#endif

/*****************************************************************************/
/*
 * debug tracing
 */
#define dbgprintk(FMT, ...) \
	printk(KERN_DEBUG "[%-6.6s] "FMT"\n", current->comm, ##__VA_ARGS__)

/* make sure we maintain the format strings, even when debugging is disabled */
static inline __attribute__((format(printf, 1, 2)))
void _dbprintk(const char *fmt, ...)
{
}

#define kenter(FMT, ...) dbgprintk("==> %s("FMT")", __func__, ##__VA_ARGS__)
#define kleave(FMT, ...) dbgprintk("<== %s()"FMT"", __func__, ##__VA_ARGS__)
#define kdebug(FMT, ...) dbgprintk(FMT, ##__VA_ARGS__)

#define kjournal(FMT, ...) _dbprintk(FMT, ##__VA_ARGS__)

#ifdef __KDEBUG
#define _enter(FMT, ...) kenter(FMT, ##__VA_ARGS__)
#define _leave(FMT, ...) kleave(FMT, ##__VA_ARGS__)
#define _debug(FMT, ...) kdebug(FMT, ##__VA_ARGS__)

#elif defined(CONFIG_FSCACHE_DEBUG)
#define _enter(FMT, ...)			\
do {						\
	if (__do_kdebug(ENTER))			\
		kenter(FMT, ##__VA_ARGS__);	\
} while (0)

#define _leave(FMT, ...)			\
do {						\
	if (__do_kdebug(LEAVE))			\
		kleave(FMT, ##__VA_ARGS__);	\
} while (0)

#define _debug(FMT, ...)			\
do {						\
	if (__do_kdebug(DEBUG))			\
		kdebug(FMT, ##__VA_ARGS__);	\
} while (0)

#else
#define _enter(FMT, ...) _dbprintk("==> %s("FMT")", __func__, ##__VA_ARGS__)
#define _leave(FMT, ...) _dbprintk("<== %s()"FMT"", __func__, ##__VA_ARGS__)
#define _debug(FMT, ...) _dbprintk(FMT, ##__VA_ARGS__)
#endif

/*
 * determine whether a particular optional debugging point should be logged
 * - we need to go through three steps to persuade cpp to correctly join the
 *   shorthand in FSCACHE_DEBUG_LEVEL with its prefix
 */
#define ____do_kdebug(LEVEL, POINT) \
	unlikely((fscache_debug & \
		  (FSCACHE_POINT_##POINT << (FSCACHE_DEBUG_ ## LEVEL * 3))))
#define ___do_kdebug(LEVEL, POINT) \
	____do_kdebug(LEVEL, POINT)
#define __do_kdebug(POINT) \
	___do_kdebug(FSCACHE_DEBUG_LEVEL, POINT)

#define FSCACHE_DEBUG_CACHE	0
#define FSCACHE_DEBUG_COOKIE	1
#define FSCACHE_DEBUG_PAGE	2
#define FSCACHE_DEBUG_OPERATION	3

#define FSCACHE_POINT_ENTER	1
#define FSCACHE_POINT_LEAVE	2
#define FSCACHE_POINT_DEBUG	4

#ifndef FSCACHE_DEBUG_LEVEL
#define FSCACHE_DEBUG_LEVEL CACHE
#endif

/*
 * assertions
 */
#if 1 /* defined(__KDEBUGALL) */

#define ASSERT(X)							\
do {									\
	if (unlikely(!(X))) {						\
		printk(KERN_ERR "\n");					\
		printk(KERN_ERR "FS-Cache: Assertion failed\n");	\
		BUG();							\
	}								\
} while (0)

#define ASSERTCMP(X, OP, Y)						\
do {									\
	if (unlikely(!((X) OP (Y)))) {					\
		printk(KERN_ERR "\n");					\
		printk(KERN_ERR "FS-Cache: Assertion failed\n");	\
		printk(KERN_ERR "%lx " #OP " %lx is false\n",		\
		       (unsigned long)(X), (unsigned long)(Y));		\
		BUG();							\
	}								\
} while (0)

#define ASSERTIF(C, X)							\
do {									\
	if (unlikely((C) && !(X))) {					\
		printk(KERN_ERR "\n");					\
		printk(KERN_ERR "FS-Cache: Assertion failed\n");	\
		BUG();							\
	}								\
} while (0)

#define ASSERTIFCMP(C, X, OP, Y)					\
do {									\
	if (unlikely((C) && !((X) OP (Y)))) {				\
		printk(KERN_ERR "\n");					\
		printk(KERN_ERR "FS-Cache: Assertion failed\n");	\
		printk(KERN_ERR "%lx " #OP " %lx is false\n",		\
		       (unsigned long)(X), (unsigned long)(Y));		\
		BUG();							\
	}								\
} while (0)

#else

#define ASSERT(X)			do {} while (0)
#define ASSERTCMP(X, OP, Y)		do {} while (0)
#define ASSERTIF(C, X)			do {} while (0)
#define ASSERTIFCMP(C, X, OP, Y)	do {} while (0)

#endif /* assert or not */
