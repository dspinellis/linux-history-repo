/*
 * Copyright 2009 Jerome Glisse.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors:
 *    Jerome Glisse <glisse@freedesktop.org>
 *    Thomas Hellstrom <thomas-at-tungstengraphics-dot-com>
 *    Dave Airlie
 */
#include <linux/list.h>
#include <drm/drmP.h>
#include "radeon_drm.h"
#include "radeon.h"


int radeon_ttm_init(struct radeon_device *rdev);
void radeon_ttm_fini(struct radeon_device *rdev);
static void radeon_bo_clear_surface_reg(struct radeon_bo *bo);

/*
 * To exclude mutual BO access we rely on bo_reserve exclusion, as all
 * function are calling it.
 */

static void radeon_ttm_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct radeon_bo *bo;

	bo = container_of(tbo, struct radeon_bo, tbo);
	mutex_lock(&bo->rdev->gem.mutex);
	list_del_init(&bo->list);
	mutex_unlock(&bo->rdev->gem.mutex);
	radeon_bo_clear_surface_reg(bo);
	kfree(bo);
}

static inline u32 radeon_ttm_flags_from_domain(u32 domain)
{
	u32 flags = 0;

	if (domain & RADEON_GEM_DOMAIN_VRAM) {
		flags |= TTM_PL_FLAG_VRAM | TTM_PL_FLAG_WC | TTM_PL_FLAG_UNCACHED;
	}
	if (domain & RADEON_GEM_DOMAIN_GTT) {
		flags |= TTM_PL_FLAG_TT | TTM_PL_MASK_CACHING;
	}
	if (domain & RADEON_GEM_DOMAIN_CPU) {
		flags |= TTM_PL_FLAG_SYSTEM | TTM_PL_MASK_CACHING;
	}
	if (!flags) {
		flags |= TTM_PL_FLAG_SYSTEM | TTM_PL_MASK_CACHING;
	}
	return flags;
}

void radeon_ttm_placement_from_domain(struct radeon_bo *rbo, u32 domain)
{
	u32 c = 0;

	rbo->placement.fpfn = 0;
	rbo->placement.lpfn = 0;
	rbo->placement.placement = rbo->placements;
	rbo->placement.busy_placement = rbo->placements;
	if (domain & RADEON_GEM_DOMAIN_VRAM)
		rbo->placements[c++] = TTM_PL_FLAG_WC | TTM_PL_FLAG_UNCACHED |
					TTM_PL_FLAG_VRAM;
	if (domain & RADEON_GEM_DOMAIN_GTT)
		rbo->placements[c++] = TTM_PL_MASK_CACHING | TTM_PL_FLAG_TT;
	if (domain & RADEON_GEM_DOMAIN_CPU)
		rbo->placements[c++] = TTM_PL_MASK_CACHING | TTM_PL_FLAG_SYSTEM;
	rbo->placement.num_placement = c;
	rbo->placement.num_busy_placement = c;
}

int radeon_bo_create(struct radeon_device *rdev, struct drm_gem_object *gobj,
			unsigned long size, bool kernel, u32 domain,
			struct radeon_bo **bo_ptr)
{
	struct radeon_bo *bo;
	enum ttm_bo_type type;
	u32 flags;
	int r;

	if (unlikely(rdev->mman.bdev.dev_mapping == NULL)) {
		rdev->mman.bdev.dev_mapping = rdev->ddev->dev_mapping;
	}
	if (kernel) {
		type = ttm_bo_type_kernel;
	} else {
		type = ttm_bo_type_device;
	}
	*bo_ptr = NULL;
	bo = kzalloc(sizeof(struct radeon_bo), GFP_KERNEL);
	if (bo == NULL)
		return -ENOMEM;
	bo->rdev = rdev;
	bo->gobj = gobj;
	bo->surface_reg = -1;
	INIT_LIST_HEAD(&bo->list);

	flags = radeon_ttm_flags_from_domain(domain);
	/* Kernel allocation are uninterruptible */
	r = ttm_buffer_object_init(&rdev->mman.bdev, &bo->tbo, size, type,
					flags, 0, 0, !kernel, NULL, size,
					&radeon_ttm_bo_destroy);
	if (unlikely(r != 0)) {
		if (r != -ERESTARTSYS)
			dev_err(rdev->dev,
				"object_init failed for (%ld, 0x%08X)\n",
				size, flags);
		return r;
	}
	*bo_ptr = bo;
	if (gobj) {
		mutex_lock(&bo->rdev->gem.mutex);
		list_add_tail(&bo->list, &rdev->gem.objects);
		mutex_unlock(&bo->rdev->gem.mutex);
	}
	return 0;
}

int radeon_bo_kmap(struct radeon_bo *bo, void **ptr)
{
	bool is_iomem;
	int r;

	if (bo->kptr) {
		if (ptr) {
			*ptr = bo->kptr;
		}
		return 0;
	}
	r = ttm_bo_kmap(&bo->tbo, 0, bo->tbo.num_pages, &bo->kmap);
	if (r) {
		return r;
	}
	bo->kptr = ttm_kmap_obj_virtual(&bo->kmap, &is_iomem);
	if (ptr) {
		*ptr = bo->kptr;
	}
	radeon_bo_check_tiling(bo, 0, 0);
	return 0;
}

void radeon_bo_kunmap(struct radeon_bo *bo)
{
	if (bo->kptr == NULL)
		return;
	bo->kptr = NULL;
	radeon_bo_check_tiling(bo, 0, 0);
	ttm_bo_kunmap(&bo->kmap);
}

void radeon_bo_unref(struct radeon_bo **bo)
{
	struct ttm_buffer_object *tbo;

	if ((*bo) == NULL)
		return;
	tbo = &((*bo)->tbo);
	ttm_bo_unref(&tbo);
	if (tbo == NULL)
		*bo = NULL;
}

int radeon_bo_pin(struct radeon_bo *bo, u32 domain, u64 *gpu_addr)
{
	int r, i;

	radeon_ttm_placement_from_domain(bo, domain);
	if (bo->pin_count) {
		bo->pin_count++;
		if (gpu_addr)
			*gpu_addr = radeon_bo_gpu_offset(bo);
		return 0;
	}
	radeon_ttm_placement_from_domain(bo, domain);
	for (i = 0; i < bo->placement.num_placement; i++)
		bo->placements[i] |= TTM_PL_FLAG_NO_EVICT;
	r = ttm_buffer_object_validate(&bo->tbo, &bo->placement, false, false);
	if (likely(r == 0)) {
		bo->pin_count = 1;
		if (gpu_addr != NULL)
			*gpu_addr = radeon_bo_gpu_offset(bo);
	}
	if (unlikely(r != 0))
		dev_err(bo->rdev->dev, "%p pin failed\n", bo);
	return r;
}

int radeon_bo_unpin(struct radeon_bo *bo)
{
	int r, i;

	if (!bo->pin_count) {
		dev_warn(bo->rdev->dev, "%p unpin not necessary\n", bo);
		return 0;
	}
	bo->pin_count--;
	if (bo->pin_count)
		return 0;
	for (i = 0; i < bo->placement.num_placement; i++)
		bo->placements[i] &= ~TTM_PL_FLAG_NO_EVICT;
	r = ttm_buffer_object_validate(&bo->tbo, &bo->placement, false, false);
	if (unlikely(r != 0))
		dev_err(bo->rdev->dev, "%p validate failed for unpin\n", bo);
	return r;
}

int radeon_bo_evict_vram(struct radeon_device *rdev)
{
	if (rdev->flags & RADEON_IS_IGP) {
		/* Useless to evict on IGP chips */
		return 0;
	}
	return ttm_bo_evict_mm(&rdev->mman.bdev, TTM_PL_VRAM);
}

void radeon_bo_force_delete(struct radeon_device *rdev)
{
	struct radeon_bo *bo, *n;
	struct drm_gem_object *gobj;

	if (list_empty(&rdev->gem.objects)) {
		return;
	}
	dev_err(rdev->dev, "Userspace still has active objects !\n");
	list_for_each_entry_safe(bo, n, &rdev->gem.objects, list) {
		mutex_lock(&rdev->ddev->struct_mutex);
		gobj = bo->gobj;
		dev_err(rdev->dev, "%p %p %lu %lu force free\n",
			gobj, bo, (unsigned long)gobj->size,
			*((unsigned long *)&gobj->refcount));
		mutex_lock(&bo->rdev->gem.mutex);
		list_del_init(&bo->list);
		mutex_unlock(&bo->rdev->gem.mutex);
		radeon_bo_unref(&bo);
		gobj->driver_private = NULL;
		drm_gem_object_unreference(gobj);
		mutex_unlock(&rdev->ddev->struct_mutex);
	}
}

int radeon_bo_init(struct radeon_device *rdev)
{
	/* Add an MTRR for the VRAM */
	rdev->mc.vram_mtrr = mtrr_add(rdev->mc.aper_base, rdev->mc.aper_size,
			MTRR_TYPE_WRCOMB, 1);
	DRM_INFO("Detected VRAM RAM=%lluM, BAR=%lluM\n",
		rdev->mc.mc_vram_size >> 20,
		(unsigned long long)rdev->mc.aper_size >> 20);
	DRM_INFO("RAM width %dbits %cDR\n",
			rdev->mc.vram_width, rdev->mc.vram_is_ddr ? 'D' : 'S');
	return radeon_ttm_init(rdev);
}

void radeon_bo_fini(struct radeon_device *rdev)
{
	radeon_ttm_fini(rdev);
}

void radeon_bo_list_add_object(struct radeon_bo_list *lobj,
				struct list_head *head)
{
	if (lobj->wdomain) {
		list_add(&lobj->list, head);
	} else {
		list_add_tail(&lobj->list, head);
	}
}

int radeon_bo_list_reserve(struct list_head *head)
{
	struct radeon_bo_list *lobj;
	int r;

	list_for_each_entry(lobj, head, list){
		r = radeon_bo_reserve(lobj->bo, false);
		if (unlikely(r != 0))
			return r;
	}
	return 0;
}

void radeon_bo_list_unreserve(struct list_head *head)
{
	struct radeon_bo_list *lobj;

	list_for_each_entry(lobj, head, list) {
		/* only unreserve object we successfully reserved */
		if (radeon_bo_is_reserved(lobj->bo))
			radeon_bo_unreserve(lobj->bo);
	}
}

int radeon_bo_list_validate(struct list_head *head, void *fence)
{
	struct radeon_bo_list *lobj;
	struct radeon_bo *bo;
	struct radeon_fence *old_fence = NULL;
	int r;

	r = radeon_bo_list_reserve(head);
	if (unlikely(r != 0)) {
		return r;
	}
	list_for_each_entry(lobj, head, list) {
		bo = lobj->bo;
		if (!bo->pin_count) {
			if (lobj->wdomain) {
				radeon_ttm_placement_from_domain(bo,
								lobj->wdomain);
			} else {
				radeon_ttm_placement_from_domain(bo,
								lobj->rdomain);
			}
			r = ttm_buffer_object_validate(&bo->tbo,
						&bo->placement,
						true, false);
			if (unlikely(r))
				return r;
		}
		lobj->gpu_offset = radeon_bo_gpu_offset(bo);
		lobj->tiling_flags = bo->tiling_flags;
		if (fence) {
			old_fence = (struct radeon_fence *)bo->tbo.sync_obj;
			bo->tbo.sync_obj = radeon_fence_ref(fence);
			bo->tbo.sync_obj_arg = NULL;
		}
		if (old_fence) {
			radeon_fence_unref(&old_fence);
		}
	}
	return 0;
}

void radeon_bo_list_unvalidate(struct list_head *head, void *fence)
{
	struct radeon_bo_list *lobj;
	struct radeon_fence *old_fence;

	if (fence)
		list_for_each_entry(lobj, head, list) {
			old_fence = to_radeon_fence(lobj->bo->tbo.sync_obj);
			if (old_fence == fence) {
				lobj->bo->tbo.sync_obj = NULL;
				radeon_fence_unref(&old_fence);
			}
		}
	radeon_bo_list_unreserve(head);
}

int radeon_bo_fbdev_mmap(struct radeon_bo *bo,
			     struct vm_area_struct *vma)
{
	return ttm_fbdev_mmap(vma, &bo->tbo);
}

static int radeon_bo_get_surface_reg(struct radeon_bo *bo)
{
	struct radeon_device *rdev = bo->rdev;
	struct radeon_surface_reg *reg;
	struct radeon_bo *old_object;
	int steal;
	int i;

	BUG_ON(!atomic_read(&bo->tbo.reserved));

	if (!bo->tiling_flags)
		return 0;

	if (bo->surface_reg >= 0) {
		reg = &rdev->surface_regs[bo->surface_reg];
		i = bo->surface_reg;
		goto out;
	}

	steal = -1;
	for (i = 0; i < RADEON_GEM_MAX_SURFACES; i++) {

		reg = &rdev->surface_regs[i];
		if (!reg->bo)
			break;

		old_object = reg->bo;
		if (old_object->pin_count == 0)
			steal = i;
	}

	/* if we are all out */
	if (i == RADEON_GEM_MAX_SURFACES) {
		if (steal == -1)
			return -ENOMEM;
		/* find someone with a surface reg and nuke their BO */
		reg = &rdev->surface_regs[steal];
		old_object = reg->bo;
		/* blow away the mapping */
		DRM_DEBUG("stealing surface reg %d from %p\n", steal, old_object);
		ttm_bo_unmap_virtual(&old_object->tbo);
		old_object->surface_reg = -1;
		i = steal;
	}

	bo->surface_reg = i;
	reg->bo = bo;

out:
	radeon_set_surface_reg(rdev, i, bo->tiling_flags, bo->pitch,
			       bo->tbo.mem.mm_node->start << PAGE_SHIFT,
			       bo->tbo.num_pages << PAGE_SHIFT);
	return 0;
}

static void radeon_bo_clear_surface_reg(struct radeon_bo *bo)
{
	struct radeon_device *rdev = bo->rdev;
	struct radeon_surface_reg *reg;

	if (bo->surface_reg == -1)
		return;

	reg = &rdev->surface_regs[bo->surface_reg];
	radeon_clear_surface_reg(rdev, bo->surface_reg);

	reg->bo = NULL;
	bo->surface_reg = -1;
}

int radeon_bo_set_tiling_flags(struct radeon_bo *bo,
				uint32_t tiling_flags, uint32_t pitch)
{
	int r;

	r = radeon_bo_reserve(bo, false);
	if (unlikely(r != 0))
		return r;
	bo->tiling_flags = tiling_flags;
	bo->pitch = pitch;
	radeon_bo_unreserve(bo);
	return 0;
}

void radeon_bo_get_tiling_flags(struct radeon_bo *bo,
				uint32_t *tiling_flags,
				uint32_t *pitch)
{
	BUG_ON(!atomic_read(&bo->tbo.reserved));
	if (tiling_flags)
		*tiling_flags = bo->tiling_flags;
	if (pitch)
		*pitch = bo->pitch;
}

int radeon_bo_check_tiling(struct radeon_bo *bo, bool has_moved,
				bool force_drop)
{
	BUG_ON(!atomic_read(&bo->tbo.reserved));

	if (!(bo->tiling_flags & RADEON_TILING_SURFACE))
		return 0;

	if (force_drop) {
		radeon_bo_clear_surface_reg(bo);
		return 0;
	}

	if (bo->tbo.mem.mem_type != TTM_PL_VRAM) {
		if (!has_moved)
			return 0;

		if (bo->surface_reg >= 0)
			radeon_bo_clear_surface_reg(bo);
		return 0;
	}

	if ((bo->surface_reg >= 0) && !has_moved)
		return 0;

	return radeon_bo_get_surface_reg(bo);
}

void radeon_bo_move_notify(struct ttm_buffer_object *bo,
				struct ttm_mem_reg *mem)
{
	struct radeon_bo *rbo = container_of(bo, struct radeon_bo, tbo);
	radeon_bo_check_tiling(rbo, 0, 1);
}

void radeon_bo_fault_reserve_notify(struct ttm_buffer_object *bo)
{
	struct radeon_bo *rbo = container_of(bo, struct radeon_bo, tbo);
	radeon_bo_check_tiling(rbo, 0, 0);
}
