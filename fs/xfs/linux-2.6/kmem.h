/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef __XFS_SUPPORT_KMEM_H__
#define __XFS_SUPPORT_KMEM_H__

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>

/*
 * Process flags handling
 */

#define PFLAGS_TEST_NOIO()              (current->flags & PF_NOIO)
#define PFLAGS_TEST_FSTRANS()           (current->flags & PF_FSTRANS)

#define PFLAGS_SET_NOIO() do {		\
	current->flags |= PF_NOIO;	\
} while (0)

#define PFLAGS_CLEAR_NOIO() do {	\
	current->flags &= ~PF_NOIO;	\
} while (0)

/* these could be nested, so we save state */
#define PFLAGS_SET_FSTRANS(STATEP) do {	\
	*(STATEP) = current->flags;	\
	current->flags |= PF_FSTRANS;	\
} while (0)

#define PFLAGS_CLEAR_FSTRANS(STATEP) do { \
	*(STATEP) = current->flags;	\
	current->flags &= ~PF_FSTRANS;	\
} while (0)

/* Restore the PF_FSTRANS state to what was saved in STATEP */
#define PFLAGS_RESTORE_FSTRANS(STATEP) do {     		\
	current->flags = ((current->flags & ~PF_FSTRANS) |	\
			  (*(STATEP) & PF_FSTRANS));		\
} while (0)

#define PFLAGS_DUP(OSTATEP, NSTATEP) do { \
	*(NSTATEP) = *(OSTATEP);	\
} while (0)

/*
 * General memory allocation interfaces
 */

#define KM_SLEEP	0x0001u
#define KM_NOSLEEP	0x0002u
#define KM_NOFS		0x0004u
#define KM_MAYFAIL	0x0008u

/*
 * We use a special process flag to avoid recursive callbacks into
 * the filesystem during transactions.  We will also issue our own
 * warnings, so we explicitly skip any generic ones (silly of us).
 */
static inline gfp_t
kmem_flags_convert(unsigned int __nocast flags)
{
	gfp_t	lflags;

	BUG_ON(flags & ~(KM_SLEEP|KM_NOSLEEP|KM_NOFS|KM_MAYFAIL));

	if (flags & KM_NOSLEEP) {
		lflags = GFP_ATOMIC | __GFP_NOWARN;
	} else {
		lflags = GFP_KERNEL | __GFP_NOWARN;
		if (PFLAGS_TEST_FSTRANS() || (flags & KM_NOFS))
			lflags &= ~__GFP_FS;
	}
	return lflags;
}

extern void *kmem_alloc(size_t, unsigned int __nocast);
extern void *kmem_realloc(void *, size_t, size_t, unsigned int __nocast);
extern void *kmem_zalloc(size_t, unsigned int __nocast);
extern void  kmem_free(void *, size_t);

/*
 * Zone interfaces
 */

#define KM_ZONE_HWALIGN	SLAB_HWCACHE_ALIGN
#define KM_ZONE_RECLAIM	SLAB_RECLAIM_ACCOUNT
#define KM_ZONE_SPREAD	0

#define kmem_zone	kmem_cache
#define kmem_zone_t	struct kmem_cache

static inline kmem_zone_t *
kmem_zone_init(int size, char *zone_name)
{
	return kmem_cache_create(zone_name, size, 0, 0, NULL, NULL);
}

static inline kmem_zone_t *
kmem_zone_init_flags(int size, char *zone_name, unsigned long flags,
		     void (*construct)(void *, kmem_zone_t *, unsigned long))
{
	return kmem_cache_create(zone_name, size, 0, flags, construct, NULL);
}

static inline void
kmem_zone_free(kmem_zone_t *zone, void *ptr)
{
	kmem_cache_free(zone, ptr);
}

static inline void
kmem_zone_destroy(kmem_zone_t *zone)
{
	if (zone && kmem_cache_destroy(zone))
		BUG();
}

extern void *kmem_zone_alloc(kmem_zone_t *, unsigned int __nocast);
extern void *kmem_zone_zalloc(kmem_zone_t *, unsigned int __nocast);

/*
 * Low memory cache shrinkers
 */

typedef struct shrinker *kmem_shaker_t;
typedef int (*kmem_shake_func_t)(int, gfp_t);

static inline kmem_shaker_t
kmem_shake_register(kmem_shake_func_t sfunc)
{
	return set_shrinker(DEFAULT_SEEKS, sfunc);
}

static inline void
kmem_shake_deregister(kmem_shaker_t shrinker)
{
	remove_shrinker(shrinker);
}

static inline int
kmem_shake_allow(gfp_t gfp_mask)
{
	return (gfp_mask & __GFP_WAIT);
}

#endif /* __XFS_SUPPORT_KMEM_H__ */
