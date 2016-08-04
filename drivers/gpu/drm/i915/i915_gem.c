/*
 * Copyright © 2008-2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <drm/drmP.h>
#include <drm/drm_vma_manager.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "i915_gem_dmabuf.h"
#include "i915_vgpu.h"
#include "i915_trace.h"
#include "intel_drv.h"
#include "intel_mocs.h"
#include <linux/reservation.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/pagemap.h>

static void i915_gem_object_flush_gtt_write_domain(struct drm_i915_gem_object *obj);
static void i915_gem_object_flush_cpu_write_domain(struct drm_i915_gem_object *obj);

static bool cpu_cache_is_coherent(struct drm_device *dev,
				  enum i915_cache_level level)
{
	return HAS_LLC(dev) || level != I915_CACHE_NONE;
}

static bool cpu_write_needs_clflush(struct drm_i915_gem_object *obj)
{
	if (obj->base.write_domain == I915_GEM_DOMAIN_CPU)
		return false;

	if (!cpu_cache_is_coherent(obj->base.dev, obj->cache_level))
		return true;

	return obj->pin_display;
}

static int
insert_mappable_node(struct drm_i915_private *i915,
                     struct drm_mm_node *node, u32 size)
{
	memset(node, 0, sizeof(*node));
	return drm_mm_insert_node_in_range_generic(&i915->ggtt.base.mm, node,
						   size, 0, 0, 0,
						   i915->ggtt.mappable_end,
						   DRM_MM_SEARCH_DEFAULT,
						   DRM_MM_CREATE_DEFAULT);
}

static void
remove_mappable_node(struct drm_mm_node *node)
{
	drm_mm_remove_node(node);
}

/* some bookkeeping */
static void i915_gem_info_add_obj(struct drm_i915_private *dev_priv,
				  size_t size)
{
	spin_lock(&dev_priv->mm.object_stat_lock);
	dev_priv->mm.object_count++;
	dev_priv->mm.object_memory += size;
	spin_unlock(&dev_priv->mm.object_stat_lock);
}

static void i915_gem_info_remove_obj(struct drm_i915_private *dev_priv,
				     size_t size)
{
	spin_lock(&dev_priv->mm.object_stat_lock);
	dev_priv->mm.object_count--;
	dev_priv->mm.object_memory -= size;
	spin_unlock(&dev_priv->mm.object_stat_lock);
}

static int
i915_gem_wait_for_error(struct i915_gpu_error *error)
{
	int ret;

	if (!i915_reset_in_progress(error))
		return 0;

	/*
	 * Only wait 10 seconds for the gpu reset to complete to avoid hanging
	 * userspace. If it takes that long something really bad is going on and
	 * we should simply try to bail out and fail as gracefully as possible.
	 */
	ret = wait_event_interruptible_timeout(error->reset_queue,
					       !i915_reset_in_progress(error),
					       10*HZ);
	if (ret == 0) {
		DRM_ERROR("Timed out waiting for the gpu reset to complete\n");
		return -EIO;
	} else if (ret < 0) {
		return ret;
	} else {
		return 0;
	}
}

int i915_mutex_lock_interruptible(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	int ret;

	ret = i915_gem_wait_for_error(&dev_priv->gpu_error);
	if (ret)
		return ret;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	return 0;
}

int
i915_gem_get_aperture_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i915_ggtt *ggtt = &dev_priv->ggtt;
	struct drm_i915_gem_get_aperture *args = data;
	struct i915_vma *vma;
	size_t pinned;

	pinned = 0;
	mutex_lock(&dev->struct_mutex);
	list_for_each_entry(vma, &ggtt->base.active_list, vm_link)
		if (vma->pin_count)
			pinned += vma->node.size;
	list_for_each_entry(vma, &ggtt->base.inactive_list, vm_link)
		if (vma->pin_count)
			pinned += vma->node.size;
	mutex_unlock(&dev->struct_mutex);

	args->aper_size = ggtt->base.total;
	args->aper_available_size = args->aper_size - pinned;

	return 0;
}

static int
i915_gem_object_get_pages_phys(struct drm_i915_gem_object *obj)
{
	struct address_space *mapping = obj->base.filp->f_mapping;
	char *vaddr = obj->phys_handle->vaddr;
	struct sg_table *st;
	struct scatterlist *sg;
	int i;

	if (WARN_ON(i915_gem_object_needs_bit17_swizzle(obj)))
		return -EINVAL;

	for (i = 0; i < obj->base.size / PAGE_SIZE; i++) {
		struct page *page;
		char *src;

		page = shmem_read_mapping_page(mapping, i);
		if (IS_ERR(page))
			return PTR_ERR(page);

		src = kmap_atomic(page);
		memcpy(vaddr, src, PAGE_SIZE);
		drm_clflush_virt_range(vaddr, PAGE_SIZE);
		kunmap_atomic(src);

		put_page(page);
		vaddr += PAGE_SIZE;
	}

	i915_gem_chipset_flush(to_i915(obj->base.dev));

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (st == NULL)
		return -ENOMEM;

	if (sg_alloc_table(st, 1, GFP_KERNEL)) {
		kfree(st);
		return -ENOMEM;
	}

	sg = st->sgl;
	sg->offset = 0;
	sg->length = obj->base.size;

	sg_dma_address(sg) = obj->phys_handle->busaddr;
	sg_dma_len(sg) = obj->base.size;

	obj->pages = st;
	return 0;
}

static void
i915_gem_object_put_pages_phys(struct drm_i915_gem_object *obj)
{
	int ret;

	BUG_ON(obj->madv == __I915_MADV_PURGED);

	ret = i915_gem_object_set_to_cpu_domain(obj, true);
	if (WARN_ON(ret)) {
		/* In the event of a disaster, abandon all caches and
		 * hope for the best.
		 */
		obj->base.read_domains = obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	}

	if (obj->madv == I915_MADV_DONTNEED)
		obj->dirty = 0;

	if (obj->dirty) {
		struct address_space *mapping = obj->base.filp->f_mapping;
		char *vaddr = obj->phys_handle->vaddr;
		int i;

		for (i = 0; i < obj->base.size / PAGE_SIZE; i++) {
			struct page *page;
			char *dst;

			page = shmem_read_mapping_page(mapping, i);
			if (IS_ERR(page))
				continue;

			dst = kmap_atomic(page);
			drm_clflush_virt_range(vaddr, PAGE_SIZE);
			memcpy(dst, vaddr, PAGE_SIZE);
			kunmap_atomic(dst);

			set_page_dirty(page);
			if (obj->madv == I915_MADV_WILLNEED)
				mark_page_accessed(page);
			put_page(page);
			vaddr += PAGE_SIZE;
		}
		obj->dirty = 0;
	}

	sg_free_table(obj->pages);
	kfree(obj->pages);
}

static void
i915_gem_object_release_phys(struct drm_i915_gem_object *obj)
{
	drm_pci_free(obj->base.dev, obj->phys_handle);
}

static const struct drm_i915_gem_object_ops i915_gem_phys_ops = {
	.get_pages = i915_gem_object_get_pages_phys,
	.put_pages = i915_gem_object_put_pages_phys,
	.release = i915_gem_object_release_phys,
};

int
i915_gem_object_unbind(struct drm_i915_gem_object *obj)
{
	struct i915_vma *vma;
	LIST_HEAD(still_in_list);
	int ret;

	/* The vma will only be freed if it is marked as closed, and if we wait
	 * upon rendering to the vma, we may unbind anything in the list.
	 */
	while ((vma = list_first_entry_or_null(&obj->vma_list,
					       struct i915_vma,
					       obj_link))) {
		list_move_tail(&vma->obj_link, &still_in_list);
		ret = i915_vma_unbind(vma);
		if (ret)
			break;
	}
	list_splice(&still_in_list, &obj->vma_list);

	return ret;
}

int
i915_gem_object_attach_phys(struct drm_i915_gem_object *obj,
			    int align)
{
	drm_dma_handle_t *phys;
	int ret;

	if (obj->phys_handle) {
		if ((unsigned long)obj->phys_handle->vaddr & (align -1))
			return -EBUSY;

		return 0;
	}

	if (obj->madv != I915_MADV_WILLNEED)
		return -EFAULT;

	if (obj->base.filp == NULL)
		return -EINVAL;

	ret = i915_gem_object_unbind(obj);
	if (ret)
		return ret;

	ret = i915_gem_object_put_pages(obj);
	if (ret)
		return ret;

	/* create a new object */
	phys = drm_pci_alloc(obj->base.dev, obj->base.size, align);
	if (!phys)
		return -ENOMEM;

	obj->phys_handle = phys;
	obj->ops = &i915_gem_phys_ops;

	return i915_gem_object_get_pages(obj);
}

static int
i915_gem_phys_pwrite(struct drm_i915_gem_object *obj,
		     struct drm_i915_gem_pwrite *args,
		     struct drm_file *file_priv)
{
	struct drm_device *dev = obj->base.dev;
	void *vaddr = obj->phys_handle->vaddr + args->offset;
	char __user *user_data = u64_to_user_ptr(args->data_ptr);
	int ret = 0;

	/* We manually control the domain here and pretend that it
	 * remains coherent i.e. in the GTT domain, like shmem_pwrite.
	 */
	ret = i915_gem_object_wait_rendering(obj, false);
	if (ret)
		return ret;

	intel_fb_obj_invalidate(obj, ORIGIN_CPU);
	if (__copy_from_user_inatomic_nocache(vaddr, user_data, args->size)) {
		unsigned long unwritten;

		/* The physical object once assigned is fixed for the lifetime
		 * of the obj, so we can safely drop the lock and continue
		 * to access vaddr.
		 */
		mutex_unlock(&dev->struct_mutex);
		unwritten = copy_from_user(vaddr, user_data, args->size);
		mutex_lock(&dev->struct_mutex);
		if (unwritten) {
			ret = -EFAULT;
			goto out;
		}
	}

	drm_clflush_virt_range(vaddr, args->size);
	i915_gem_chipset_flush(to_i915(dev));

out:
	intel_fb_obj_flush(obj, false, ORIGIN_CPU);
	return ret;
}

void *i915_gem_object_alloc(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	return kmem_cache_zalloc(dev_priv->objects, GFP_KERNEL);
}

void i915_gem_object_free(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	kmem_cache_free(dev_priv->objects, obj);
}

static int
i915_gem_create(struct drm_file *file,
		struct drm_device *dev,
		uint64_t size,
		uint32_t *handle_p)
{
	struct drm_i915_gem_object *obj;
	int ret;
	u32 handle;

	size = roundup(size, PAGE_SIZE);
	if (size == 0)
		return -EINVAL;

	/* Allocate the new object */
	obj = i915_gem_object_create(dev, size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, &obj->base, &handle);
	/* drop reference from allocate - handle holds it now */
	i915_gem_object_put_unlocked(obj);
	if (ret)
		return ret;

	*handle_p = handle;
	return 0;
}

int
i915_gem_dumb_create(struct drm_file *file,
		     struct drm_device *dev,
		     struct drm_mode_create_dumb *args)
{
	/* have to work out size/pitch and return them */
	args->pitch = ALIGN(args->width * DIV_ROUND_UP(args->bpp, 8), 64);
	args->size = args->pitch * args->height;
	return i915_gem_create(file, dev,
			       args->size, &args->handle);
}

/**
 * Creates a new mm object and returns a handle to it.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 */
int
i915_gem_create_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_gem_create *args = data;

	return i915_gem_create(file, dev,
			       args->size, &args->handle);
}

static inline int
__copy_to_user_swizzled(char __user *cpu_vaddr,
			const char *gpu_vaddr, int gpu_offset,
			int length)
{
	int ret, cpu_offset = 0;

	while (length > 0) {
		int cacheline_end = ALIGN(gpu_offset + 1, 64);
		int this_length = min(cacheline_end - gpu_offset, length);
		int swizzled_gpu_offset = gpu_offset ^ 64;

		ret = __copy_to_user(cpu_vaddr + cpu_offset,
				     gpu_vaddr + swizzled_gpu_offset,
				     this_length);
		if (ret)
			return ret + length;

		cpu_offset += this_length;
		gpu_offset += this_length;
		length -= this_length;
	}

	return 0;
}

static inline int
__copy_from_user_swizzled(char *gpu_vaddr, int gpu_offset,
			  const char __user *cpu_vaddr,
			  int length)
{
	int ret, cpu_offset = 0;

	while (length > 0) {
		int cacheline_end = ALIGN(gpu_offset + 1, 64);
		int this_length = min(cacheline_end - gpu_offset, length);
		int swizzled_gpu_offset = gpu_offset ^ 64;

		ret = __copy_from_user(gpu_vaddr + swizzled_gpu_offset,
				       cpu_vaddr + cpu_offset,
				       this_length);
		if (ret)
			return ret + length;

		cpu_offset += this_length;
		gpu_offset += this_length;
		length -= this_length;
	}

	return 0;
}

/*
 * Pins the specified object's pages and synchronizes the object with
 * GPU accesses. Sets needs_clflush to non-zero if the caller should
 * flush the object from the CPU cache.
 */
int i915_gem_obj_prepare_shmem_read(struct drm_i915_gem_object *obj,
				    int *needs_clflush)
{
	int ret;

	*needs_clflush = 0;

	if (WARN_ON(!i915_gem_object_has_struct_page(obj)))
		return -EINVAL;

	ret = i915_gem_object_wait_rendering(obj, true);
	if (ret)
		return ret;

	if (!(obj->base.read_domains & I915_GEM_DOMAIN_CPU)) {
		/* If we're not in the cpu read domain, set ourself into the gtt
		 * read domain and manually flush cachelines (if required). This
		 * optimizes for the case when the gpu will dirty the data
		 * anyway again before the next pread happens. */
		*needs_clflush = !cpu_cache_is_coherent(obj->base.dev,
							obj->cache_level);
	}

	ret = i915_gem_object_get_pages(obj);
	if (ret)
		return ret;

	i915_gem_object_pin_pages(obj);

	return ret;
}

/* Per-page copy function for the shmem pread fastpath.
 * Flushes invalid cachelines before reading the target if
 * needs_clflush is set. */
static int
shmem_pread_fast(struct page *page, int shmem_page_offset, int page_length,
		 char __user *user_data,
		 bool page_do_bit17_swizzling, bool needs_clflush)
{
	char *vaddr;
	int ret;

	if (unlikely(page_do_bit17_swizzling))
		return -EINVAL;

	vaddr = kmap_atomic(page);
	if (needs_clflush)
		drm_clflush_virt_range(vaddr + shmem_page_offset,
				       page_length);
	ret = __copy_to_user_inatomic(user_data,
				      vaddr + shmem_page_offset,
				      page_length);
	kunmap_atomic(vaddr);

	return ret ? -EFAULT : 0;
}

static void
shmem_clflush_swizzled_range(char *addr, unsigned long length,
			     bool swizzled)
{
	if (unlikely(swizzled)) {
		unsigned long start = (unsigned long) addr;
		unsigned long end = (unsigned long) addr + length;

		/* For swizzling simply ensure that we always flush both
		 * channels. Lame, but simple and it works. Swizzled
		 * pwrite/pread is far from a hotpath - current userspace
		 * doesn't use it at all. */
		start = round_down(start, 128);
		end = round_up(end, 128);

		drm_clflush_virt_range((void *)start, end - start);
	} else {
		drm_clflush_virt_range(addr, length);
	}

}

/* Only difference to the fast-path function is that this can handle bit17
 * and uses non-atomic copy and kmap functions. */
static int
shmem_pread_slow(struct page *page, int shmem_page_offset, int page_length,
		 char __user *user_data,
		 bool page_do_bit17_swizzling, bool needs_clflush)
{
	char *vaddr;
	int ret;

	vaddr = kmap(page);
	if (needs_clflush)
		shmem_clflush_swizzled_range(vaddr + shmem_page_offset,
					     page_length,
					     page_do_bit17_swizzling);

	if (page_do_bit17_swizzling)
		ret = __copy_to_user_swizzled(user_data,
					      vaddr, shmem_page_offset,
					      page_length);
	else
		ret = __copy_to_user(user_data,
				     vaddr + shmem_page_offset,
				     page_length);
	kunmap(page);

	return ret ? - EFAULT : 0;
}

static inline unsigned long
slow_user_access(struct io_mapping *mapping,
		 uint64_t page_base, int page_offset,
		 char __user *user_data,
		 unsigned long length, bool pwrite)
{
	void __iomem *ioaddr;
	void *vaddr;
	uint64_t unwritten;

	ioaddr = io_mapping_map_wc(mapping, page_base, PAGE_SIZE);
	/* We can use the cpu mem copy function because this is X86. */
	vaddr = (void __force *)ioaddr + page_offset;
	if (pwrite)
		unwritten = __copy_from_user(vaddr, user_data, length);
	else
		unwritten = __copy_to_user(user_data, vaddr, length);

	io_mapping_unmap(ioaddr);
	return unwritten;
}

static int
i915_gem_gtt_pread(struct drm_device *dev,
		   struct drm_i915_gem_object *obj, uint64_t size,
		   uint64_t data_offset, uint64_t data_ptr)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i915_ggtt *ggtt = &dev_priv->ggtt;
	struct drm_mm_node node;
	char __user *user_data;
	uint64_t remain;
	uint64_t offset;
	int ret;

	ret = i915_gem_obj_ggtt_pin(obj, 0, PIN_MAPPABLE);
	if (ret) {
		ret = insert_mappable_node(dev_priv, &node, PAGE_SIZE);
		if (ret)
			goto out;

		ret = i915_gem_object_get_pages(obj);
		if (ret) {
			remove_mappable_node(&node);
			goto out;
		}

		i915_gem_object_pin_pages(obj);
	} else {
		node.start = i915_gem_obj_ggtt_offset(obj);
		node.allocated = false;
		ret = i915_gem_object_put_fence(obj);
		if (ret)
			goto out_unpin;
	}

	ret = i915_gem_object_set_to_gtt_domain(obj, false);
	if (ret)
		goto out_unpin;

	user_data = u64_to_user_ptr(data_ptr);
	remain = size;
	offset = data_offset;

	mutex_unlock(&dev->struct_mutex);
	if (likely(!i915.prefault_disable)) {
		ret = fault_in_multipages_writeable(user_data, remain);
		if (ret) {
			mutex_lock(&dev->struct_mutex);
			goto out_unpin;
		}
	}

	while (remain > 0) {
		/* Operation in this page
		 *
		 * page_base = page offset within aperture
		 * page_offset = offset within page
		 * page_length = bytes to copy for this page
		 */
		u32 page_base = node.start;
		unsigned page_offset = offset_in_page(offset);
		unsigned page_length = PAGE_SIZE - page_offset;
		page_length = remain < page_length ? remain : page_length;
		if (node.allocated) {
			wmb();
			ggtt->base.insert_page(&ggtt->base,
					       i915_gem_object_get_dma_address(obj, offset >> PAGE_SHIFT),
					       node.start,
					       I915_CACHE_NONE, 0);
			wmb();
		} else {
			page_base += offset & PAGE_MASK;
		}
		/* This is a slow read/write as it tries to read from
		 * and write to user memory which may result into page
		 * faults, and so we cannot perform this under struct_mutex.
		 */
		if (slow_user_access(ggtt->mappable, page_base,
				     page_offset, user_data,
				     page_length, false)) {
			ret = -EFAULT;
			break;
		}

		remain -= page_length;
		user_data += page_length;
		offset += page_length;
	}

	mutex_lock(&dev->struct_mutex);
	if (ret == 0 && (obj->base.read_domains & I915_GEM_DOMAIN_GTT) == 0) {
		/* The user has modified the object whilst we tried
		 * reading from it, and we now have no idea what domain
		 * the pages should be in. As we have just been touching
		 * them directly, flush everything back to the GTT
		 * domain.
		 */
		ret = i915_gem_object_set_to_gtt_domain(obj, false);
	}

out_unpin:
	if (node.allocated) {
		wmb();
		ggtt->base.clear_range(&ggtt->base,
				       node.start, node.size,
				       true);
		i915_gem_object_unpin_pages(obj);
		remove_mappable_node(&node);
	} else {
		i915_gem_object_ggtt_unpin(obj);
	}
out:
	return ret;
}

static int
i915_gem_shmem_pread(struct drm_device *dev,
		     struct drm_i915_gem_object *obj,
		     struct drm_i915_gem_pread *args,
		     struct drm_file *file)
{
	char __user *user_data;
	ssize_t remain;
	loff_t offset;
	int shmem_page_offset, page_length, ret = 0;
	int obj_do_bit17_swizzling, page_do_bit17_swizzling;
	int prefaulted = 0;
	int needs_clflush = 0;
	struct sg_page_iter sg_iter;

	if (!i915_gem_object_has_struct_page(obj))
		return -ENODEV;

	user_data = u64_to_user_ptr(args->data_ptr);
	remain = args->size;

	obj_do_bit17_swizzling = i915_gem_object_needs_bit17_swizzle(obj);

	ret = i915_gem_obj_prepare_shmem_read(obj, &needs_clflush);
	if (ret)
		return ret;

	offset = args->offset;

	for_each_sg_page(obj->pages->sgl, &sg_iter, obj->pages->nents,
			 offset >> PAGE_SHIFT) {
		struct page *page = sg_page_iter_page(&sg_iter);

		if (remain <= 0)
			break;

		/* Operation in this page
		 *
		 * shmem_page_offset = offset within page in shmem file
		 * page_length = bytes to copy for this page
		 */
		shmem_page_offset = offset_in_page(offset);
		page_length = remain;
		if ((shmem_page_offset + page_length) > PAGE_SIZE)
			page_length = PAGE_SIZE - shmem_page_offset;

		page_do_bit17_swizzling = obj_do_bit17_swizzling &&
			(page_to_phys(page) & (1 << 17)) != 0;

		ret = shmem_pread_fast(page, shmem_page_offset, page_length,
				       user_data, page_do_bit17_swizzling,
				       needs_clflush);
		if (ret == 0)
			goto next_page;

		mutex_unlock(&dev->struct_mutex);

		if (likely(!i915.prefault_disable) && !prefaulted) {
			ret = fault_in_multipages_writeable(user_data, remain);
			/* Userspace is tricking us, but we've already clobbered
			 * its pages with the prefault and promised to write the
			 * data up to the first fault. Hence ignore any errors
			 * and just continue. */
			(void)ret;
			prefaulted = 1;
		}

		ret = shmem_pread_slow(page, shmem_page_offset, page_length,
				       user_data, page_do_bit17_swizzling,
				       needs_clflush);

		mutex_lock(&dev->struct_mutex);

		if (ret)
			goto out;

next_page:
		remain -= page_length;
		user_data += page_length;
		offset += page_length;
	}

out:
	i915_gem_object_unpin_pages(obj);

	return ret;
}

/**
 * Reads data from the object referenced by handle.
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 *
 * On error, the contents of *data are undefined.
 */
int
i915_gem_pread_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file)
{
	struct drm_i915_gem_pread *args = data;
	struct drm_i915_gem_object *obj;
	int ret = 0;

	if (args->size == 0)
		return 0;

	if (!access_ok(VERIFY_WRITE,
		       u64_to_user_ptr(args->data_ptr),
		       args->size))
		return -EFAULT;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	/* Bounds check source.  */
	if (args->offset > obj->base.size ||
	    args->size > obj->base.size - args->offset) {
		ret = -EINVAL;
		goto out;
	}

	trace_i915_gem_object_pread(obj, args->offset, args->size);

	ret = i915_gem_shmem_pread(dev, obj, args, file);

	/* pread for non shmem backed objects */
	if (ret == -EFAULT || ret == -ENODEV) {
		intel_runtime_pm_get(to_i915(dev));
		ret = i915_gem_gtt_pread(dev, obj, args->size,
					args->offset, args->data_ptr);
		intel_runtime_pm_put(to_i915(dev));
	}

out:
	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

/* This is the fast write path which cannot handle
 * page faults in the source data
 */

static inline int
fast_user_write(struct io_mapping *mapping,
		loff_t page_base, int page_offset,
		char __user *user_data,
		int length)
{
	void __iomem *vaddr_atomic;
	void *vaddr;
	unsigned long unwritten;

	vaddr_atomic = io_mapping_map_atomic_wc(mapping, page_base);
	/* We can use the cpu mem copy function because this is X86. */
	vaddr = (void __force*)vaddr_atomic + page_offset;
	unwritten = __copy_from_user_inatomic_nocache(vaddr,
						      user_data, length);
	io_mapping_unmap_atomic(vaddr_atomic);
	return unwritten;
}

/**
 * This is the fast pwrite path, where we copy the data directly from the
 * user into the GTT, uncached.
 * @i915: i915 device private data
 * @obj: i915 gem object
 * @args: pwrite arguments structure
 * @file: drm file pointer
 */
static int
i915_gem_gtt_pwrite_fast(struct drm_i915_private *i915,
			 struct drm_i915_gem_object *obj,
			 struct drm_i915_gem_pwrite *args,
			 struct drm_file *file)
{
	struct i915_ggtt *ggtt = &i915->ggtt;
	struct drm_device *dev = obj->base.dev;
	struct drm_mm_node node;
	uint64_t remain, offset;
	char __user *user_data;
	int ret;
	bool hit_slow_path = false;

	if (obj->tiling_mode != I915_TILING_NONE)
		return -EFAULT;

	ret = i915_gem_obj_ggtt_pin(obj, 0, PIN_MAPPABLE | PIN_NONBLOCK);
	if (ret) {
		ret = insert_mappable_node(i915, &node, PAGE_SIZE);
		if (ret)
			goto out;

		ret = i915_gem_object_get_pages(obj);
		if (ret) {
			remove_mappable_node(&node);
			goto out;
		}

		i915_gem_object_pin_pages(obj);
	} else {
		node.start = i915_gem_obj_ggtt_offset(obj);
		node.allocated = false;
		ret = i915_gem_object_put_fence(obj);
		if (ret)
			goto out_unpin;
	}

	ret = i915_gem_object_set_to_gtt_domain(obj, true);
	if (ret)
		goto out_unpin;

	intel_fb_obj_invalidate(obj, ORIGIN_GTT);
	obj->dirty = true;

	user_data = u64_to_user_ptr(args->data_ptr);
	offset = args->offset;
	remain = args->size;
	while (remain) {
		/* Operation in this page
		 *
		 * page_base = page offset within aperture
		 * page_offset = offset within page
		 * page_length = bytes to copy for this page
		 */
		u32 page_base = node.start;
		unsigned page_offset = offset_in_page(offset);
		unsigned page_length = PAGE_SIZE - page_offset;
		page_length = remain < page_length ? remain : page_length;
		if (node.allocated) {
			wmb(); /* flush the write before we modify the GGTT */
			ggtt->base.insert_page(&ggtt->base,
					       i915_gem_object_get_dma_address(obj, offset >> PAGE_SHIFT),
					       node.start, I915_CACHE_NONE, 0);
			wmb(); /* flush modifications to the GGTT (insert_page) */
		} else {
			page_base += offset & PAGE_MASK;
		}
		/* If we get a fault while copying data, then (presumably) our
		 * source page isn't available.  Return the error and we'll
		 * retry in the slow path.
		 * If the object is non-shmem backed, we retry again with the
		 * path that handles page fault.
		 */
		if (fast_user_write(ggtt->mappable, page_base,
				    page_offset, user_data, page_length)) {
			hit_slow_path = true;
			mutex_unlock(&dev->struct_mutex);
			if (slow_user_access(ggtt->mappable,
					     page_base,
					     page_offset, user_data,
					     page_length, true)) {
				ret = -EFAULT;
				mutex_lock(&dev->struct_mutex);
				goto out_flush;
			}

			mutex_lock(&dev->struct_mutex);
		}

		remain -= page_length;
		user_data += page_length;
		offset += page_length;
	}

out_flush:
	if (hit_slow_path) {
		if (ret == 0 &&
		    (obj->base.read_domains & I915_GEM_DOMAIN_GTT) == 0) {
			/* The user has modified the object whilst we tried
			 * reading from it, and we now have no idea what domain
			 * the pages should be in. As we have just been touching
			 * them directly, flush everything back to the GTT
			 * domain.
			 */
			ret = i915_gem_object_set_to_gtt_domain(obj, false);
		}
	}

	intel_fb_obj_flush(obj, false, ORIGIN_GTT);
out_unpin:
	if (node.allocated) {
		wmb();
		ggtt->base.clear_range(&ggtt->base,
				       node.start, node.size,
				       true);
		i915_gem_object_unpin_pages(obj);
		remove_mappable_node(&node);
	} else {
		i915_gem_object_ggtt_unpin(obj);
	}
out:
	return ret;
}

/* Per-page copy function for the shmem pwrite fastpath.
 * Flushes invalid cachelines before writing to the target if
 * needs_clflush_before is set and flushes out any written cachelines after
 * writing if needs_clflush is set. */
static int
shmem_pwrite_fast(struct page *page, int shmem_page_offset, int page_length,
		  char __user *user_data,
		  bool page_do_bit17_swizzling,
		  bool needs_clflush_before,
		  bool needs_clflush_after)
{
	char *vaddr;
	int ret;

	if (unlikely(page_do_bit17_swizzling))
		return -EINVAL;

	vaddr = kmap_atomic(page);
	if (needs_clflush_before)
		drm_clflush_virt_range(vaddr + shmem_page_offset,
				       page_length);
	ret = __copy_from_user_inatomic(vaddr + shmem_page_offset,
					user_data, page_length);
	if (needs_clflush_after)
		drm_clflush_virt_range(vaddr + shmem_page_offset,
				       page_length);
	kunmap_atomic(vaddr);

	return ret ? -EFAULT : 0;
}

/* Only difference to the fast-path function is that this can handle bit17
 * and uses non-atomic copy and kmap functions. */
static int
shmem_pwrite_slow(struct page *page, int shmem_page_offset, int page_length,
		  char __user *user_data,
		  bool page_do_bit17_swizzling,
		  bool needs_clflush_before,
		  bool needs_clflush_after)
{
	char *vaddr;
	int ret;

	vaddr = kmap(page);
	if (unlikely(needs_clflush_before || page_do_bit17_swizzling))
		shmem_clflush_swizzled_range(vaddr + shmem_page_offset,
					     page_length,
					     page_do_bit17_swizzling);
	if (page_do_bit17_swizzling)
		ret = __copy_from_user_swizzled(vaddr, shmem_page_offset,
						user_data,
						page_length);
	else
		ret = __copy_from_user(vaddr + shmem_page_offset,
				       user_data,
				       page_length);
	if (needs_clflush_after)
		shmem_clflush_swizzled_range(vaddr + shmem_page_offset,
					     page_length,
					     page_do_bit17_swizzling);
	kunmap(page);

	return ret ? -EFAULT : 0;
}

static int
i915_gem_shmem_pwrite(struct drm_device *dev,
		      struct drm_i915_gem_object *obj,
		      struct drm_i915_gem_pwrite *args,
		      struct drm_file *file)
{
	ssize_t remain;
	loff_t offset;
	char __user *user_data;
	int shmem_page_offset, page_length, ret = 0;
	int obj_do_bit17_swizzling, page_do_bit17_swizzling;
	int hit_slowpath = 0;
	int needs_clflush_after = 0;
	int needs_clflush_before = 0;
	struct sg_page_iter sg_iter;

	user_data = u64_to_user_ptr(args->data_ptr);
	remain = args->size;

	obj_do_bit17_swizzling = i915_gem_object_needs_bit17_swizzle(obj);

	ret = i915_gem_object_wait_rendering(obj, false);
	if (ret)
		return ret;

	if (obj->base.write_domain != I915_GEM_DOMAIN_CPU) {
		/* If we're not in the cpu write domain, set ourself into the gtt
		 * write domain and manually flush cachelines (if required). This
		 * optimizes for the case when the gpu will use the data
		 * right away and we therefore have to clflush anyway. */
		needs_clflush_after = cpu_write_needs_clflush(obj);
	}
	/* Same trick applies to invalidate partially written cachelines read
	 * before writing. */
	if ((obj->base.read_domains & I915_GEM_DOMAIN_CPU) == 0)
		needs_clflush_before =
			!cpu_cache_is_coherent(dev, obj->cache_level);

	ret = i915_gem_object_get_pages(obj);
	if (ret)
		return ret;

	intel_fb_obj_invalidate(obj, ORIGIN_CPU);

	i915_gem_object_pin_pages(obj);

	offset = args->offset;
	obj->dirty = 1;

	for_each_sg_page(obj->pages->sgl, &sg_iter, obj->pages->nents,
			 offset >> PAGE_SHIFT) {
		struct page *page = sg_page_iter_page(&sg_iter);
		int partial_cacheline_write;

		if (remain <= 0)
			break;

		/* Operation in this page
		 *
		 * shmem_page_offset = offset within page in shmem file
		 * page_length = bytes to copy for this page
		 */
		shmem_page_offset = offset_in_page(offset);

		page_length = remain;
		if ((shmem_page_offset + page_length) > PAGE_SIZE)
			page_length = PAGE_SIZE - shmem_page_offset;

		/* If we don't overwrite a cacheline completely we need to be
		 * careful to have up-to-date data by first clflushing. Don't
		 * overcomplicate things and flush the entire patch. */
		partial_cacheline_write = needs_clflush_before &&
			((shmem_page_offset | page_length)
				& (boot_cpu_data.x86_clflush_size - 1));

		page_do_bit17_swizzling = obj_do_bit17_swizzling &&
			(page_to_phys(page) & (1 << 17)) != 0;

		ret = shmem_pwrite_fast(page, shmem_page_offset, page_length,
					user_data, page_do_bit17_swizzling,
					partial_cacheline_write,
					needs_clflush_after);
		if (ret == 0)
			goto next_page;

		hit_slowpath = 1;
		mutex_unlock(&dev->struct_mutex);
		ret = shmem_pwrite_slow(page, shmem_page_offset, page_length,
					user_data, page_do_bit17_swizzling,
					partial_cacheline_write,
					needs_clflush_after);

		mutex_lock(&dev->struct_mutex);

		if (ret)
			goto out;

next_page:
		remain -= page_length;
		user_data += page_length;
		offset += page_length;
	}

out:
	i915_gem_object_unpin_pages(obj);

	if (hit_slowpath) {
		/*
		 * Fixup: Flush cpu caches in case we didn't flush the dirty
		 * cachelines in-line while writing and the object moved
		 * out of the cpu write domain while we've dropped the lock.
		 */
		if (!needs_clflush_after &&
		    obj->base.write_domain != I915_GEM_DOMAIN_CPU) {
			if (i915_gem_clflush_object(obj, obj->pin_display))
				needs_clflush_after = true;
		}
	}

	if (needs_clflush_after)
		i915_gem_chipset_flush(to_i915(dev));
	else
		obj->cache_dirty = true;

	intel_fb_obj_flush(obj, false, ORIGIN_CPU);
	return ret;
}

/**
 * Writes data to the object referenced by handle.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 *
 * On error, the contents of the buffer that were to be modified are undefined.
 */
int
i915_gem_pwrite_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_pwrite *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	if (args->size == 0)
		return 0;

	if (!access_ok(VERIFY_READ,
		       u64_to_user_ptr(args->data_ptr),
		       args->size))
		return -EFAULT;

	if (likely(!i915.prefault_disable)) {
		ret = fault_in_multipages_readable(u64_to_user_ptr(args->data_ptr),
						   args->size);
		if (ret)
			return -EFAULT;
	}

	intel_runtime_pm_get(dev_priv);

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		goto put_rpm;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	/* Bounds check destination. */
	if (args->offset > obj->base.size ||
	    args->size > obj->base.size - args->offset) {
		ret = -EINVAL;
		goto out;
	}

	trace_i915_gem_object_pwrite(obj, args->offset, args->size);

	ret = -EFAULT;
	/* We can only do the GTT pwrite on untiled buffers, as otherwise
	 * it would end up going through the fenced access, and we'll get
	 * different detiling behavior between reading and writing.
	 * pread/pwrite currently are reading and writing from the CPU
	 * perspective, requiring manual detiling by the client.
	 */
	if (!i915_gem_object_has_struct_page(obj) ||
	    cpu_write_needs_clflush(obj)) {
		ret = i915_gem_gtt_pwrite_fast(dev_priv, obj, args, file);
		/* Note that the gtt paths might fail with non-page-backed user
		 * pointers (e.g. gtt mappings when moving data between
		 * textures). Fallback to the shmem path in that case. */
	}

	if (ret == -EFAULT || ret == -ENOSPC) {
		if (obj->phys_handle)
			ret = i915_gem_phys_pwrite(obj, args, file);
		else if (i915_gem_object_has_struct_page(obj))
			ret = i915_gem_shmem_pwrite(dev, obj, args, file);
		else
			ret = -ENODEV;
	}

out:
	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
put_rpm:
	intel_runtime_pm_put(dev_priv);

	return ret;
}

/**
 * Ensures that all rendering to the object has completed and the object is
 * safe to unbind from the GTT or access from the CPU.
 * @obj: i915 gem object
 * @readonly: waiting for read access or write
 */
int
i915_gem_object_wait_rendering(struct drm_i915_gem_object *obj,
			       bool readonly)
{
	struct reservation_object *resv;
	struct i915_gem_active *active;
	unsigned long active_mask;
	int idx, ret;

	lockdep_assert_held(&obj->base.dev->struct_mutex);

	if (!readonly) {
		active = obj->last_read;
		active_mask = obj->active;
	} else {
		active_mask = 1;
		active = &obj->last_write;
	}

	for_each_active(active_mask, idx) {
		ret = i915_gem_active_wait(&active[idx],
					   &obj->base.dev->struct_mutex);
		if (ret)
			return ret;
	}

	resv = i915_gem_object_get_dmabuf_resv(obj);
	if (resv) {
		long err;

		err = reservation_object_wait_timeout_rcu(resv, !readonly, true,
							  MAX_SCHEDULE_TIMEOUT);
		if (err < 0)
			return err;
	}

	return 0;
}

/* A nonblocking variant of the above wait. This is a highly dangerous routine
 * as the object state may change during this call.
 */
static __must_check int
i915_gem_object_wait_rendering__nonblocking(struct drm_i915_gem_object *obj,
					    struct intel_rps_client *rps,
					    bool readonly)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_request *requests[I915_NUM_ENGINES];
	struct i915_gem_active *active;
	unsigned long active_mask;
	int ret, i, n = 0;

	BUG_ON(!mutex_is_locked(&dev->struct_mutex));
	BUG_ON(!dev_priv->mm.interruptible);

	active_mask = obj->active;
	if (!active_mask)
		return 0;

	if (!readonly) {
		active = obj->last_read;
	} else {
		active_mask = 1;
		active = &obj->last_write;
	}

	for_each_active(active_mask, i) {
		struct drm_i915_gem_request *req;

		req = i915_gem_active_get(&active[i],
					  &obj->base.dev->struct_mutex);
		if (req)
			requests[n++] = req;
	}

	mutex_unlock(&dev->struct_mutex);
	ret = 0;
	for (i = 0; ret == 0 && i < n; i++)
		ret = i915_wait_request(requests[i], true, NULL, rps);
	mutex_lock(&dev->struct_mutex);

	for (i = 0; i < n; i++)
		i915_gem_request_put(requests[i]);

	return ret;
}

static struct intel_rps_client *to_rps_client(struct drm_file *file)
{
	struct drm_i915_file_private *fpriv = file->driver_priv;
	return &fpriv->rps;
}

static enum fb_op_origin
write_origin(struct drm_i915_gem_object *obj, unsigned domain)
{
	return domain == I915_GEM_DOMAIN_GTT && !obj->has_wc_mmap ?
	       ORIGIN_GTT : ORIGIN_CPU;
}

/**
 * Called when user space prepares to use an object with the CPU, either
 * through the mmap ioctl's mapping or a GTT mapping.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 */
int
i915_gem_set_domain_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	struct drm_i915_gem_set_domain *args = data;
	struct drm_i915_gem_object *obj;
	uint32_t read_domains = args->read_domains;
	uint32_t write_domain = args->write_domain;
	int ret;

	/* Only handle setting domains to types used by the CPU. */
	if (write_domain & I915_GEM_GPU_DOMAINS)
		return -EINVAL;

	if (read_domains & I915_GEM_GPU_DOMAINS)
		return -EINVAL;

	/* Having something in the write domain implies it's in the read
	 * domain, and only that read domain.  Enforce that in the request.
	 */
	if (write_domain != 0 && read_domains != write_domain)
		return -EINVAL;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	/* Try to flush the object off the GPU without holding the lock.
	 * We will repeat the flush holding the lock in the normal manner
	 * to catch cases where we are gazumped.
	 */
	ret = i915_gem_object_wait_rendering__nonblocking(obj,
							  to_rps_client(file),
							  !write_domain);
	if (ret)
		goto unref;

	if (read_domains & I915_GEM_DOMAIN_GTT)
		ret = i915_gem_object_set_to_gtt_domain(obj, write_domain != 0);
	else
		ret = i915_gem_object_set_to_cpu_domain(obj, write_domain != 0);

	if (write_domain != 0)
		intel_fb_obj_invalidate(obj, write_origin(obj, write_domain));

unref:
	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

/**
 * Called when user space has done writes to this buffer
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 */
int
i915_gem_sw_finish_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct drm_i915_gem_sw_finish *args = data;
	struct drm_i915_gem_object *obj;
	int ret = 0;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	/* Pinned buffers may be scanout, so flush the cache */
	if (obj->pin_display)
		i915_gem_object_flush_cpu_write_domain(obj);

	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

/**
 * i915_gem_mmap_ioctl - Maps the contents of an object, returning the address
 *			 it is mapped to.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 *
 * While the mapping holds a reference on the contents of the object, it doesn't
 * imply a ref on the object itself.
 *
 * IMPORTANT:
 *
 * DRM driver writers who look a this function as an example for how to do GEM
 * mmap support, please don't implement mmap support like here. The modern way
 * to implement DRM mmap support is with an mmap offset ioctl (like
 * i915_gem_mmap_gtt) and then using the mmap syscall on the DRM fd directly.
 * That way debug tooling like valgrind will understand what's going on, hiding
 * the mmap call in a driver private ioctl will break that. The i915 driver only
 * does cpu mmaps this way because we didn't know better.
 */
int
i915_gem_mmap_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file)
{
	struct drm_i915_gem_mmap *args = data;
	struct drm_i915_gem_object *obj;
	unsigned long addr;
#ifndef __linux__
	vm_object_t vmobj;
	struct proc *p;
	vm_map_t map;
	vm_size_t size;
	int error, rv;
#endif

#ifdef __notyet__
/*
 * This is another example of the antique xf86-intel ddx passing in
 * bad values. Accomodate users still using 2.2 for now.
 */
	if (args->flags & ~(I915_MMAP_WC)) {
		DRM_DEBUG("Attempting to mmap with flag other than WC set\n");
		return -EINVAL;
	}
	if (args->flags & I915_MMAP_WC && !cpu_has_pat) {
		DRM_DEBUG("Attempting to mmap WC without pat\n");
		return -ENODEV;
	}
#endif

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	/* prime objects have no backing filp to GEM mmap
	 * pages from.
	 */
	if (!obj->base.filp) {
		i915_gem_object_put_unlocked(obj);
		return -EINVAL;
	}

#ifdef __linux__	
	addr = vm_mmap(obj->base.filp, 0, args->size,
		       PROT_READ | PROT_WRITE, MAP_SHARED,
		       args->offset);
	if (args->flags & I915_MMAP_WC) {
		struct mm_struct *mm = current->mm;
		struct vm_area_struct *vma;

		if (down_write_killable(&mm->mmap_sem)) {
			i915_gem_object_put_unlocked(obj);
			return -EINTR;
		}
		vma = find_vma(mm, addr);
		if (vma)
			vma->vm_page_prot =
				pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
		else
			addr = -ENOMEM;
		up_write(&mm->mmap_sem);

		/* This may race, but that's ok, it only gets set */
		WRITE_ONCE(obj->has_wc_mmap, true);
	}
#else
	error = 0;
	addr = 0;
	if (args->size == 0)
		goto out;
	p = curproc;
	map = &p->p_vmspace->vm_map;
	size = round_page(args->size);
	PROC_LOCK(p);
	if (map->size + size > lim_cur_proc(p, RLIMIT_VMEM)) {
		PROC_UNLOCK(p);
		error = -ENOMEM;
		goto out;
	}
	PROC_UNLOCK(p);

	vmobj = file_inode(obj->base.filp)->i_mapping;
	vm_object_reference(vmobj);
	rv = vm_map_find(map, vmobj, args->offset, &addr, args->size, 0,
	    VMFS_OPTIMAL_SPACE, VM_PROT_READ | VM_PROT_WRITE,
	    VM_PROT_READ | VM_PROT_WRITE, MAP_INHERIT_SHARE);

	/* currently disabled as it causes artifacts on FreeBSD */
	if ((rv == KERN_SUCCESS) && (args->flags & I915_MMAP_WC)) {
		VM_OBJECT_WLOCK(vmobj);
		if (vm_object_set_memattr(vmobj, VM_MEMATTR_WRITE_COMBINING) != KERN_SUCCESS) {
			for (vm_page_t page = vm_page_find_least(vmobj, 0); page != NULL; page = vm_page_next(page)) {
				pmap_page_set_memattr(page, VM_MEMATTR_WRITE_COMBINING);
			}
		}
		VM_OBJECT_WUNLOCK(vmobj);
	}

	if (rv != KERN_SUCCESS) {
		vm_object_deallocate(vmobj);
		error = -vm_mmap_to_errno(rv);
	} else {
		args->addr_ptr = (uint64_t)addr;
	}


out:
#endif
	i915_gem_object_put_unlocked(obj);
#ifdef __linux__
	if (IS_ERR((void *)addr))
		return addr;
#else
	if (error)
		return error;
#endif
	args->addr_ptr = (uint64_t) addr;

	return 0;
}

/**
 * i915_gem_fault - fault a page into the GTT
 * @vma: VMA in question
 * @vmf: fault info
 *
 * The fault handler is set up by drm_gem_mmap() when a object is GTT mapped
 * from userspace.  The fault handler takes care of binding the object to
 * the GTT (if needed), allocating and programming a fence register (again,
 * only if needed based on whether the old reg is still valid or the object
 * is tiled) and inserting a new PTE into the faulting process.
 *
 * Note that the faulting process may involve evicting existing objects
 * from the GTT and/or fence registers to make room.  So performance may
 * suffer if the GTT working set is large or there are few fence registers
 * left.
 */
int i915_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_i915_gem_object *obj = to_intel_bo(vma->vm_private_data);
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i915_ggtt *ggtt = &dev_priv->ggtt;
	struct i915_ggtt_view view = i915_ggtt_view_normal;
	pgoff_t page_offset;
	unsigned long pfn;
	int ret = 0;
	bool write = !!(vmf->flags & FAULT_FLAG_WRITE);

	intel_runtime_pm_get(dev_priv);

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = ((unsigned long)vmf->virtual_address - vma->vm_start) >>
		PAGE_SHIFT;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		goto out;

	trace_i915_gem_object_fault(obj, page_offset, true, write);

	/* Try to flush the object off the GPU first without holding the lock.
	 * Upon reacquiring the lock, we will perform our sanity checks and then
	 * repeat the flush holding the lock in the normal manner to catch cases
	 * where we are gazumped.
	 */
	ret = i915_gem_object_wait_rendering__nonblocking(obj, NULL, !write);
	if (ret)
		goto unlock;

	/* Access to snoopable pages through the GTT is incoherent. */
	if (obj->cache_level != I915_CACHE_NONE && !HAS_LLC(dev)) {
		ret = -EFAULT;
		goto unlock;
	}

	/* Use a partial view if the object is bigger than the aperture. */
	if (obj->base.size >= ggtt->mappable_end &&
	    obj->tiling_mode == I915_TILING_NONE) {
		static const unsigned int chunk_size = 256; // 1 MiB

		memset(&view, 0, sizeof(view));
		view.type = I915_GGTT_VIEW_PARTIAL;
		view.params.partial.offset = rounddown(page_offset, chunk_size);
		view.params.partial.size =
			min_t(unsigned int,
			      chunk_size,
			      (vma->vm_end - vma->vm_start)/PAGE_SIZE -
			      view.params.partial.offset);
	}

	/* Now pin it into the GTT if needed */
	ret = i915_gem_object_ggtt_pin(obj, &view, 0, 0, PIN_MAPPABLE);
	if (ret)
		goto unlock;

	ret = i915_gem_object_set_to_gtt_domain(obj, write);
	if (ret)
		goto unpin;

	ret = i915_gem_object_get_fence(obj);
	if (ret)
		goto unpin;

	/* Finally, remap it using the new GTT offset */
	pfn = ggtt->mappable_base +
		i915_gem_obj_ggtt_offset_view(obj, &view);
	pfn >>= PAGE_SHIFT;

	if (unlikely(view.type == I915_GGTT_VIEW_PARTIAL)) {
		/* Overriding existing pages in partial view does not cause
		 * us any trouble as TLBs are still valid because the fault
		 * is due to userspace losing part of the mapping or never
		 * having accessed it before (at this partials' range).
		 */
		unsigned long base = vma->vm_start +
				     (view.params.partial.offset << PAGE_SHIFT);
		unsigned int i;

		for (i = 0; i < view.params.partial.size; i++) {
			ret = vm_insert_pfn(vma, base + i * PAGE_SIZE, pfn + i);
			if (ret)
				break;
		}

		obj->fault_mappable = true;
	} else {
		if (!obj->fault_mappable) {
			unsigned long size = min_t(unsigned long,
						   vma->vm_end - vma->vm_start,
						   obj->base.size);
			int i;

			for (i = 0; i < size >> PAGE_SHIFT; i++) {
				ret = vm_insert_pfn(vma,
						    (unsigned long)vma->vm_start + i * PAGE_SIZE,
						    pfn + i);
				if (ret)
					break;
			}

			obj->fault_mappable = true;
		} else
			ret = vm_insert_pfn(vma,
					    (unsigned long)vmf->virtual_address,
					    pfn + page_offset);
	}
unpin:
	i915_gem_object_ggtt_unpin_view(obj, &view);
unlock:
	mutex_unlock(&dev->struct_mutex);
out:
	switch (ret) {
	case -EIO:
		/*
		 * We eat errors when the gpu is terminally wedged to avoid
		 * userspace unduly crashing (gl has no provisions for mmaps to
		 * fail). But any other -EIO isn't ours (e.g. swap in failure)
		 * and so needs to be reported.
		 */
		if (!i915_terminally_wedged(&dev_priv->gpu_error)) {
			ret = VM_FAULT_SIGBUS;
			break;
		}
	case -EAGAIN:
		/*
		 * EAGAIN means the gpu is hung and we'll wait for the error
		 * handler to reset everything when re-faulting in
		 * i915_mutex_lock_interruptible.
		 */
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		/*
		 * EBUSY is ok: this just means that another thread
		 * already did the job.
		 */
		ret = VM_FAULT_NOPAGE;
		break;
	case -ENOMEM:
		ret = VM_FAULT_OOM;
		break;
	case -ENOSPC:
	case -EFAULT:
		ret = VM_FAULT_SIGBUS;
		break;
	default:
		WARN_ONCE(ret, "unhandled error in i915_gem_fault: %i\n", ret);
		ret = VM_FAULT_SIGBUS;
		break;
	}

	intel_runtime_pm_put(dev_priv);
	return ret;
}

/**
 * i915_gem_release_mmap - remove physical page mappings
 * @obj: obj in question
 *
 * Preserve the reservation of the mmapping with the DRM core code, but
 * relinquish ownership of the pages back to the system.
 *
 * It is vital that we remove the page mapping if we have mapped a tiled
 * object through the GTT and then lose the fence register due to
 * resource pressure. Similarly if the object has been moved out of the
 * aperture, than pages mapped into userspace must be revoked. Removing the
 * mapping will then trigger a page fault on the next user access, allowing
 * fixup by i915_gem_fault().
 */
void
i915_gem_release_mmap(struct drm_i915_gem_object *obj)
{
	/* Serialisation between user GTT access and our code depends upon
	 * revoking the CPU's PTE whilst the mutex is held. The next user
	 * pagefault then has to wait until we release the mutex.
	 */
	lockdep_assert_held(&obj->base.dev->struct_mutex);

	if (!obj->fault_mappable)
		return;

	drm_vma_node_unmap(&obj->base.vma_node,
			   obj->base.dev->anon_inode->i_mapping);

	/* Ensure that the CPU's PTE are revoked and there are not outstanding
	 * memory transactions from userspace before we return. The TLB
	 * flushing implied above by changing the PTE above *should* be
	 * sufficient, an extra barrier here just provides us with a bit
	 * of paranoid documentation about our requirement to serialise
	 * memory writes before touching registers / GSM.
	 */
	wmb();

	obj->fault_mappable = false;
}

void
i915_gem_release_all_mmaps(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *obj;

	list_for_each_entry(obj, &dev_priv->mm.bound_list, global_list)
		i915_gem_release_mmap(obj);
}

/**
 * i915_gem_get_ggtt_size - return required global GTT size for an object
 * @dev: drm device
 * @size: object size
 * @tiling_mode: tiling mode
 *
 * Return the required global GTT size for an object, taking into account
 * potential fence register mapping.
 */
u64 i915_gem_get_ggtt_size(struct drm_device *dev, u64 size, int tiling_mode)
{
	u64 ggtt_size;

	GEM_BUG_ON(size == 0);

	if (INTEL_GEN(dev) >= 4 ||
	    tiling_mode == I915_TILING_NONE)
		return size;

	/* Previous chips need a power-of-two fence region when tiling */
	if (IS_GEN3(dev))
		ggtt_size = 1024*1024;
	else
		ggtt_size = 512*1024;

	while (ggtt_size < size)
		ggtt_size <<= 1;

	return ggtt_size;
}

/**
 * i915_gem_get_ggtt_alignment - return required global GTT alignment
 * @dev: drm device
 * @size: object size
 * @tiling_mode: tiling mode
 * @fenced: is fenced alignment required or not
 *
 * Return the required global GTT alignment for an object, taking into account
 * potential fence register mapping.
 */
u64 i915_gem_get_ggtt_alignment(struct drm_device *dev, u64 size,
				int tiling_mode, bool fenced)
{
	GEM_BUG_ON(size == 0);

	/*
	 * Minimum alignment is 4k (GTT page size), but might be greater
	 * if a fence register is needed for the object.
	 */
	if (INTEL_GEN(dev) >= 4 || (!fenced && IS_G33(dev)) ||
	    tiling_mode == I915_TILING_NONE)
		return 4096;

	/*
	 * Previous chips need to be aligned to the size of the smallest
	 * fence register that can contain the object.
	 */
	return i915_gem_get_ggtt_size(dev, size, tiling_mode);
}

static int i915_gem_object_create_mmap_offset(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	int ret;

	dev_priv->mm.shrinker_no_lock_stealing = true;

	ret = drm_gem_create_mmap_offset(&obj->base);
	if (ret != -ENOSPC)
		goto out;

	/* Badly fragmented mmap space? The only way we can recover
	 * space is by destroying unwanted objects. We can't randomly release
	 * mmap_offsets as userspace expects them to be persistent for the
	 * lifetime of the objects. The closest we can is to release the
	 * offsets on purgeable objects by truncating it and marking it purged,
	 * which prevents userspace from ever using that object again.
	 */
	i915_gem_shrink(dev_priv,
			obj->base.size >> PAGE_SHIFT,
			I915_SHRINK_BOUND |
			I915_SHRINK_UNBOUND |
			I915_SHRINK_PURGEABLE);
	ret = drm_gem_create_mmap_offset(&obj->base);
	if (ret != -ENOSPC)
		goto out;

	i915_gem_shrink_all(dev_priv);
	ret = drm_gem_create_mmap_offset(&obj->base);
out:
	dev_priv->mm.shrinker_no_lock_stealing = false;

	return ret;
}

static void i915_gem_object_free_mmap_offset(struct drm_i915_gem_object *obj)
{
	drm_gem_free_mmap_offset(&obj->base);
}

int
i915_gem_mmap_gtt(struct drm_file *file,
		  struct drm_device *dev,
		  uint32_t handle,
		  uint64_t *offset)
{
	struct drm_i915_gem_object *obj;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file, handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	if (obj->madv != I915_MADV_WILLNEED) {
		DRM_DEBUG("Attempting to mmap a purgeable buffer\n");
		ret = -EFAULT;
		goto out;
	}

	ret = i915_gem_object_create_mmap_offset(obj);
	if (ret)
		goto out;

	*offset = drm_vma_node_offset_addr(&obj->base.vma_node);

out:
	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

/**
 * i915_gem_mmap_gtt_ioctl - prepare an object for GTT mmap'ing
 * @dev: DRM device
 * @data: GTT mapping ioctl data
 * @file: GEM object info
 *
 * Simply returns the fake offset to userspace so it can mmap it.
 * The mmap call will end up in drm_gem_mmap(), which will set things
 * up so we can get faults in the handler above.
 *
 * The fault handler will take care of binding the object into the GTT
 * (since it may have been evicted to make room for something), allocating
 * a fence register, and mapping the appropriate aperture address into
 * userspace.
 */
int
i915_gem_mmap_gtt_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct drm_i915_gem_mmap_gtt *args = data;

	return i915_gem_mmap_gtt(file, dev, args->handle, &args->offset);
}

/* Immediately discard the backing storage */
static void
i915_gem_object_truncate(struct drm_i915_gem_object *obj)
{
	i915_gem_object_free_mmap_offset(obj);

	if (obj->base.filp == NULL)
		return;

	/* Our goal here is to return as much of the memory as
	 * is possible back to the system as we are called from OOM.
	 * To do this we must instruct the shmfs to drop all of its
	 * backing pages, *now*.
	 */
	shmem_truncate_range(file_inode(obj->base.filp), 0, (loff_t)-1);
	obj->madv = __I915_MADV_PURGED;
}

/* Try to discard unwanted pages */
static void
i915_gem_object_invalidate(struct drm_i915_gem_object *obj)
{
	struct address_space *mapping;

	switch (obj->madv) {
	case I915_MADV_DONTNEED:
		i915_gem_object_truncate(obj);
	case __I915_MADV_PURGED:
		return;
	}

	if (obj->base.filp == NULL)
		return;

	mapping = obj->base.filp->f_mapping,
	invalidate_mapping_pages(mapping, 0, (loff_t)-1);
}

static void
i915_gem_object_put_pages_gtt(struct drm_i915_gem_object *obj)
{
	struct sgt_iter sgt_iter;
	struct page *page;
	int ret;

	BUG_ON(obj->madv == __I915_MADV_PURGED);

	ret = i915_gem_object_set_to_cpu_domain(obj, true);
	if (WARN_ON(ret)) {
		/* In the event of a disaster, abandon all caches and
		 * hope for the best.
		 */
		i915_gem_clflush_object(obj, true);
		obj->base.read_domains = obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	}

	i915_gem_gtt_finish_object(obj);

	if (i915_gem_object_needs_bit17_swizzle(obj))
		i915_gem_object_save_bit_17_swizzle(obj);

	if (obj->madv == I915_MADV_DONTNEED)
		obj->dirty = 0;

	for_each_sgt_page(page, sgt_iter, obj->pages) {
		if (obj->dirty)
			set_page_dirty(page);

		if (obj->madv == I915_MADV_WILLNEED)
			mark_page_accessed(page);

		put_page(page);
	}
	obj->dirty = 0;

	sg_free_table(obj->pages);
	kfree(obj->pages);
}

int
i915_gem_object_put_pages(struct drm_i915_gem_object *obj)
{
	const struct drm_i915_gem_object_ops *ops = obj->ops;

	if (obj->pages == NULL)
		return 0;

	if (obj->pages_pin_count)
		return -EBUSY;

	GEM_BUG_ON(obj->bind_count);

	/* ->put_pages might need to allocate memory for the bit17 swizzle
	 * array, hence protect them from being reaped by removing them from gtt
	 * lists early. */
	list_del(&obj->global_list);

	if (obj->mapping) {
		if (is_vmalloc_addr(obj->mapping))
			vunmap(obj->mapping);
		else
			kunmap(kmap_to_page(obj->mapping));
		obj->mapping = NULL;
	}

	ops->put_pages(obj);
	obj->pages = NULL;

	i915_gem_object_invalidate(obj);

	return 0;
}

static int
i915_gem_object_get_pages_gtt(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	int page_count, i;
	struct address_space *mapping;
	struct sg_table *st;
	struct scatterlist *sg;
	struct sgt_iter sgt_iter;
	struct page *page;
	unsigned long last_pfn = 0;	/* suppress gcc warning */
	int ret;
	gfp_t gfp;

	/* Assert that the object is not currently in any GPU domain. As it
	 * wasn't in the GTT, there shouldn't be any way it could have been in
	 * a GPU cache
	 */
	BUG_ON(obj->base.read_domains & I915_GEM_GPU_DOMAINS);
	BUG_ON(obj->base.write_domain & I915_GEM_GPU_DOMAINS);

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (st == NULL)
		return -ENOMEM;

	page_count = obj->base.size / PAGE_SIZE;
	if (sg_alloc_table(st, page_count, GFP_KERNEL)) {
		kfree(st);
		return -ENOMEM;
	}

	/* Get the list of pages out of our struct file.  They'll be pinned
	 * at this point until we release them.
	 *
	 * Fail silently without starting the shrinker
	 */
	mapping = obj->base.filp->f_mapping;
	gfp = mapping_gfp_constraint(mapping, ~(__GFP_IO | __GFP_RECLAIM));
	gfp |= __GFP_NORETRY | __GFP_NOWARN;
	sg = st->sgl;
	st->nents = 0;
	for (i = 0; i < page_count; i++) {
		page = shmem_read_mapping_page_gfp(mapping, i, gfp);
		if (IS_ERR(page)) {
			i915_gem_shrink(dev_priv,
					page_count,
					I915_SHRINK_BOUND |
					I915_SHRINK_UNBOUND |
					I915_SHRINK_PURGEABLE);
			page = shmem_read_mapping_page_gfp(mapping, i, gfp);
		}
		if (IS_ERR(page)) {
			/* We've tried hard to allocate the memory by reaping
			 * our own buffer, now let the real VM do its job and
			 * go down in flames if truly OOM.
			 */
			i915_gem_shrink_all(dev_priv);
			page = shmem_read_mapping_page(mapping, i);
			if (IS_ERR(page)) {
				ret = PTR_ERR(page);
				goto err_pages;
			}
		}
#ifdef CONFIG_SWIOTLB
		if (swiotlb_nr_tbl()) {
			st->nents++;
			sg_set_page(sg, page, PAGE_SIZE, 0);
			sg = sg_next(sg);
			continue;
		}
#endif
		if (!i || page_to_pfn(page) != last_pfn + 1) {
			if (i)
				sg = sg_next(sg);
			st->nents++;
			sg_set_page(sg, page, PAGE_SIZE, 0);
		} else {
			sg->length += PAGE_SIZE;
		}
		last_pfn = page_to_pfn(page);

		/* Check that the i965g/gm workaround works. */
		WARN_ON((gfp & __GFP_DMA32) && (last_pfn >= 0x00100000UL));
	}
#ifdef CONFIG_SWIOTLB
	if (!swiotlb_nr_tbl())
#endif
		sg_mark_end(sg);
	obj->pages = st;

	ret = i915_gem_gtt_prepare_object(obj);
	if (ret)
		goto err_pages;

	if (i915_gem_object_needs_bit17_swizzle(obj))
		i915_gem_object_do_bit_17_swizzle(obj);

	if (obj->tiling_mode != I915_TILING_NONE &&
	    dev_priv->quirks & QUIRK_PIN_SWIZZLED_PAGES)
		i915_gem_object_pin_pages(obj);

	return 0;

err_pages:
	sg_mark_end(sg);
	for_each_sgt_page(page, sgt_iter, st)
		put_page(page);
	sg_free_table(st);
	kfree(st);

	/* shmemfs first checks if there is enough memory to allocate the page
	 * and reports ENOSPC should there be insufficient, along with the usual
	 * ENOMEM for a genuine allocation failure.
	 *
	 * We use ENOSPC in our driver to mean that we have run out of aperture
	 * space and so want to translate the error from shmemfs back to our
	 * usual understanding of ENOMEM.
	 */
	if (ret == -ENOSPC)
		ret = -ENOMEM;

	return ret;
}

/* Ensure that the associated pages are gathered from the backing storage
 * and pinned into our object. i915_gem_object_get_pages() may be called
 * multiple times before they are released by a single call to
 * i915_gem_object_put_pages() - once the pages are no longer referenced
 * either as a result of memory pressure (reaping pages under the shrinker)
 * or as the object is itself released.
 */
int
i915_gem_object_get_pages(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	const struct drm_i915_gem_object_ops *ops = obj->ops;
	int ret;

	if (obj->pages)
		return 0;

	if (obj->madv != I915_MADV_WILLNEED) {
		DRM_DEBUG("Attempting to obtain a purgeable object\n");
		return -EFAULT;
	}

	BUG_ON(obj->pages_pin_count);

	ret = ops->get_pages(obj);
	if (ret)
		return ret;

	list_add_tail(&obj->global_list, &dev_priv->mm.unbound_list);

	obj->get_page.sg = obj->pages->sgl;
	obj->get_page.last = 0;

	return 0;
}

/* The 'mapping' part of i915_gem_object_pin_map() below */
static void *i915_gem_object_map(const struct drm_i915_gem_object *obj)
{
	unsigned long n_pages = obj->base.size >> PAGE_SHIFT;
	struct sg_table *sgt = obj->pages;
	struct sgt_iter sgt_iter;
	struct page *page;
	struct page *stack_pages[32];
	struct page **pages = stack_pages;
	unsigned long i = 0;
	void *addr;

	/* A single page can always be kmapped */
	if (n_pages == 1)
		return kmap(sg_page(sgt->sgl));

	if (n_pages > ARRAY_SIZE(stack_pages)) {
		/* Too big for stack -- allocate temporary array instead */
		pages = drm_malloc_gfp(n_pages, sizeof(*pages), GFP_TEMPORARY);
		if (!pages)
			return NULL;
	}

	for_each_sgt_page(page, sgt_iter, sgt)
		pages[i++] = page;

	/* Check that we have the expected number of pages */
	GEM_BUG_ON(i != n_pages);

	addr = vmap(pages, n_pages, 0, PAGE_KERNEL);

	if (pages != stack_pages)
		drm_free_large(pages);

	return addr;
}

/* get, pin, and map the pages of the object into kernel space */
void *i915_gem_object_pin_map(struct drm_i915_gem_object *obj)
{
	int ret;

	lockdep_assert_held(&obj->base.dev->struct_mutex);

	ret = i915_gem_object_get_pages(obj);
	if (ret)
		return ERR_PTR(ret);

	i915_gem_object_pin_pages(obj);

	if (!obj->mapping) {
		obj->mapping = i915_gem_object_map(obj);
		if (!obj->mapping) {
			i915_gem_object_unpin_pages(obj);
			return ERR_PTR(-ENOMEM);
		}
	}

	return obj->mapping;
}

static void
i915_gem_object_retire__write(struct i915_gem_active *active,
			      struct drm_i915_gem_request *request)
{
	struct drm_i915_gem_object *obj =
		container_of(active, struct drm_i915_gem_object, last_write);

	intel_fb_obj_flush(obj, true, ORIGIN_CS);
}

static void
i915_gem_object_retire__read(struct i915_gem_active *active,
			     struct drm_i915_gem_request *request)
{
	int idx = request->engine->id;
	struct drm_i915_gem_object *obj =
		container_of(active, struct drm_i915_gem_object, last_read[idx]);

	GEM_BUG_ON((obj->active & (1 << idx)) == 0);

	obj->active &= ~(1 << idx);
	if (obj->active)
		return;

	/* Bump our place on the bound list to keep it roughly in LRU order
	 * so that we don't steal from recently used but inactive objects
	 * (unless we are forced to ofc!)
	 */
	if (obj->bind_count)
		list_move_tail(&obj->global_list,
			       &request->i915->mm.bound_list);

	i915_gem_object_put(obj);
}

static bool i915_context_is_banned(const struct i915_gem_context *ctx)
{
	unsigned long elapsed;

	if (ctx->hang_stats.banned)
		return true;

	elapsed = get_seconds() - ctx->hang_stats.guilty_ts;
	if (ctx->hang_stats.ban_period_seconds &&
	    elapsed <= ctx->hang_stats.ban_period_seconds) {
		DRM_DEBUG("context hanging too fast, banning!\n");
		return true;
	}

	return false;
}

static void i915_set_reset_status(struct i915_gem_context *ctx,
				  const bool guilty)
{
	struct i915_ctx_hang_stats *hs = &ctx->hang_stats;

	if (guilty) {
		hs->banned = i915_context_is_banned(ctx);
		hs->batch_active++;
		hs->guilty_ts = get_seconds();
	} else {
		hs->batch_pending++;
	}
}

struct drm_i915_gem_request *
i915_gem_find_active_request(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_request *request;

	/* We are called by the error capture and reset at a random
	 * point in time. In particular, note that neither is crucially
	 * ordered with an interrupt. After a hang, the GPU is dead and we
	 * assume that no more writes can happen (we waited long enough for
	 * all writes that were in transaction to be flushed) - adding an
	 * extra delay for a recent interrupt is pointless. Hence, we do
	 * not need an engine->irq_seqno_barrier() before the seqno reads.
	 */
	list_for_each_entry(request, &engine->request_list, link) {
		if (i915_gem_request_completed(request))
			continue;

		return request;
	}

	return NULL;
}

static void i915_gem_reset_engine_status(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_request *request;
	bool ring_hung;

	request = i915_gem_find_active_request(engine);
	if (request == NULL)
		return;

	ring_hung = engine->hangcheck.score >= HANGCHECK_SCORE_RING_HUNG;

	i915_set_reset_status(request->ctx, ring_hung);
	list_for_each_entry_continue(request, &engine->request_list, link)
		i915_set_reset_status(request->ctx, false);
}

static void i915_gem_reset_engine_cleanup(struct intel_engine_cs *engine)
{
	struct intel_ring *ring;

	/* Mark all pending requests as complete so that any concurrent
	 * (lockless) lookup doesn't try and wait upon the request as we
	 * reset it.
	 */
	intel_engine_init_seqno(engine, engine->last_submitted_seqno);

	/*
	 * Clear the execlists queue up before freeing the requests, as those
	 * are the ones that keep the context and ringbuffer backing objects
	 * pinned in place.
	 */

	if (i915.enable_execlists) {
		/* Ensure irq handler finishes or is cancelled. */
		tasklet_kill(&engine->irq_tasklet);

		intel_execlists_cancel_requests(engine);
	}

	/*
	 * We must free the requests after all the corresponding objects have
	 * been moved off active lists. Which is the same order as the normal
	 * retire_requests function does. This is important if object hold
	 * implicit references on things like e.g. ppgtt address spaces through
	 * the request.
	 */
	if (!list_empty(&engine->request_list)) {
		struct drm_i915_gem_request *request;

		request = list_last_entry(&engine->request_list,
					  struct drm_i915_gem_request,
					  link);

		i915_gem_request_retire_upto(request);
	}

	/* Having flushed all requests from all queues, we know that all
	 * ringbuffers must now be empty. However, since we do not reclaim
	 * all space when retiring the request (to prevent HEADs colliding
	 * with rapid ringbuffer wraparound) the amount of available space
	 * upon reset is less than when we start. Do one more pass over
	 * all the ringbuffers to reset last_retired_head.
	 */
	list_for_each_entry(ring, &engine->buffers, link) {
		ring->last_retired_head = ring->tail;
		intel_ring_update_space(ring);
	}

	engine->i915->gt.active_engines &= ~intel_engine_flag(engine);
}

void i915_gem_reset(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_engine_cs *engine;

	/*
	 * Before we free the objects from the requests, we need to inspect
	 * them for finding the guilty party. As the requests only borrow
	 * their reference to the objects, the inspection must be done first.
	 */
	for_each_engine(engine, dev_priv)
		i915_gem_reset_engine_status(engine);

	for_each_engine(engine, dev_priv)
		i915_gem_reset_engine_cleanup(engine);
	mod_delayed_work(dev_priv->wq, &dev_priv->gt.idle_work, 0);

	i915_gem_context_reset(dev);

	i915_gem_restore_fences(dev);
}

static void
i915_gem_retire_work_handler(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv), gt.retire_work.work);
	struct drm_device *dev = &dev_priv->drm;

	/* Come back later if the device is busy... */
	if (mutex_trylock(&dev->struct_mutex)) {
		i915_gem_retire_requests(dev_priv);
		mutex_unlock(&dev->struct_mutex);
	}

	/* Keep the retire handler running until we are finally idle.
	 * We do not need to do this test under locking as in the worst-case
	 * we queue the retire worker once too often.
	 */
	if (READ_ONCE(dev_priv->gt.awake)) {
		i915_queue_hangcheck(dev_priv);
		queue_delayed_work(dev_priv->wq,
				   &dev_priv->gt.retire_work,
				   round_jiffies_up_relative(HZ));
	}
}

static void
i915_gem_idle_work_handler(struct work_struct *work)
{
	struct drm_i915_private *dev_priv =
		container_of(work, typeof(*dev_priv), gt.idle_work.work);
	struct drm_device *dev = &dev_priv->drm;
	struct intel_engine_cs *engine;
	unsigned int stuck_engines;
	bool rearm_hangcheck;

	if (!READ_ONCE(dev_priv->gt.awake))
		return;

	if (READ_ONCE(dev_priv->gt.active_engines))
		return;

	rearm_hangcheck =
		cancel_delayed_work_sync(&dev_priv->gpu_error.hangcheck_work);

	if (!mutex_trylock(&dev->struct_mutex)) {
		/* Currently busy, come back later */
		mod_delayed_work(dev_priv->wq,
				 &dev_priv->gt.idle_work,
				 msecs_to_jiffies(50));
		goto out_rearm;
	}

	if (dev_priv->gt.active_engines)
		goto out_unlock;

	for_each_engine(engine, dev_priv)
		i915_gem_batch_pool_fini(&engine->batch_pool);

	GEM_BUG_ON(!dev_priv->gt.awake);
	dev_priv->gt.awake = false;
	rearm_hangcheck = false;

	/* As we have disabled hangcheck, we need to unstick any waiters still
	 * hanging around. However, as we may be racing against the interrupt
	 * handler or the waiters themselves, we skip enabling the fake-irq.
	 */
	stuck_engines = intel_kick_waiters(dev_priv);
	if (unlikely(stuck_engines))
		DRM_DEBUG_DRIVER("kicked stuck waiters (%x)...missed irq?\n",
				 stuck_engines);

	if (INTEL_GEN(dev_priv) >= 6)
		gen6_rps_idle(dev_priv);
	intel_runtime_pm_put(dev_priv);
out_unlock:
	mutex_unlock(&dev->struct_mutex);

out_rearm:
	if (rearm_hangcheck) {
		GEM_BUG_ON(!dev_priv->gt.awake);
		i915_queue_hangcheck(dev_priv);
	}
}

void i915_gem_close_object(struct drm_gem_object *gem, struct drm_file *file)
{
	struct drm_i915_gem_object *obj = to_intel_bo(gem);
	struct drm_i915_file_private *fpriv = file->driver_priv;
	struct i915_vma *vma, *vn;

	mutex_lock(&obj->base.dev->struct_mutex);
	list_for_each_entry_safe(vma, vn, &obj->vma_list, obj_link)
		if (vma->vm->file == fpriv)
			i915_vma_close(vma);
	mutex_unlock(&obj->base.dev->struct_mutex);
}

/**
 * i915_gem_wait_ioctl - implements DRM_IOCTL_I915_GEM_WAIT
 * @dev: drm device pointer
 * @data: ioctl data blob
 * @file: drm file pointer
 *
 * Returns 0 if successful, else an error is returned with the remaining time in
 * the timeout parameter.
 *  -ETIME: object is still busy after timeout
 *  -ERESTARTSYS: signal interrupted the wait
 *  -ENONENT: object doesn't exist
 * Also possible, but rare:
 *  -EAGAIN: GPU wedged
 *  -ENOMEM: damn
 *  -ENODEV: Internal IRQ fail
 *  -E?: The add request failed
 *
 * The wait ioctl with a timeout of 0 reimplements the busy ioctl. With any
 * non-zero timeout parameter the wait ioctl will wait for the given number of
 * nanoseconds on an object becoming unbusy. Since the wait itself does so
 * without holding struct_mutex the object may become re-busied before this
 * function completes. A similar but shorter * race condition exists in the busy
 * ioctl
 */
int
i915_gem_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_i915_gem_wait *args = data;
	struct drm_i915_gem_object *obj;
	struct drm_i915_gem_request *requests[I915_NUM_ENGINES];
	int i, n = 0;
	int ret;

	if (args->flags != 0)
		return -EINVAL;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file, args->bo_handle);
	if (!obj) {
		mutex_unlock(&dev->struct_mutex);
		return -ENOENT;
	}

	if (!obj->active)
		goto out;

	for (i = 0; i < I915_NUM_ENGINES; i++) {
		struct drm_i915_gem_request *req;

		req = i915_gem_active_get(&obj->last_read[i],
					  &obj->base.dev->struct_mutex);
		if (req)
			requests[n++] = req;
	}

out:
	i915_gem_object_put(obj);
	mutex_unlock(&dev->struct_mutex);

	for (i = 0; i < n; i++) {
		if (ret == 0)
			ret = i915_wait_request(requests[i], true,
						args->timeout_ns > 0 ? &args->timeout_ns : NULL,
						to_rps_client(file));
		i915_gem_request_put(requests[i]);
	}
	return ret;
}

static int
__i915_gem_object_sync(struct drm_i915_gem_request *to,
		       struct drm_i915_gem_request *from)
{
	int ret;

	if (to->engine == from->engine)
		return 0;

	if (!i915.semaphores) {
		ret = i915_wait_request(from,
					from->i915->mm.interruptible,
					NULL,
					NO_WAITBOOST);
		if (ret)
			return ret;
	} else {
		int idx = intel_engine_sync_index(from->engine, to->engine);
		if (from->fence.seqno <= from->engine->semaphore.sync_seqno[idx])
			return 0;

		trace_i915_gem_ring_sync_to(to, from);
		ret = to->engine->semaphore.sync_to(to, from);
		if (ret)
			return ret;

		from->engine->semaphore.sync_seqno[idx] = from->fence.seqno;
	}

	return 0;
}

/**
 * i915_gem_object_sync - sync an object to a ring.
 *
 * @obj: object which may be in use on another ring.
 * @to: request we are wishing to use
 *
 * This code is meant to abstract object synchronization with the GPU.
 * Conceptually we serialise writes between engines inside the GPU.
 * We only allow one engine to write into a buffer at any time, but
 * multiple readers. To ensure each has a coherent view of memory, we must:
 *
 * - If there is an outstanding write request to the object, the new
 *   request must wait for it to complete (either CPU or in hw, requests
 *   on the same ring will be naturally ordered).
 *
 * - If we are a write request (pending_write_domain is set), the new
 *   request must wait for outstanding read requests to complete.
 *
 * Returns 0 if successful, else propagates up the lower layer error.
 */
int
i915_gem_object_sync(struct drm_i915_gem_object *obj,
		     struct drm_i915_gem_request *to)
{
	struct i915_gem_active *active;
	unsigned long active_mask;
	int idx;

	lockdep_assert_held(&obj->base.dev->struct_mutex);

	active_mask = obj->active;
	if (!active_mask)
		return 0;

	if (obj->base.pending_write_domain) {
		active = obj->last_read;
	} else {
		active_mask = 1;
		active = &obj->last_write;
	}

	for_each_active(active_mask, idx) {
		struct drm_i915_gem_request *request;
		int ret;

		request = i915_gem_active_peek(&active[idx],
					       &obj->base.dev->struct_mutex);
		if (!request)
			continue;

		ret = __i915_gem_object_sync(to, request);
		if (ret)
			return ret;
	}

	return 0;
}

static void i915_gem_object_finish_gtt(struct drm_i915_gem_object *obj)
{
	u32 old_write_domain, old_read_domains;

	/* Force a pagefault for domain tracking on next user access */
	i915_gem_release_mmap(obj);

	if ((obj->base.read_domains & I915_GEM_DOMAIN_GTT) == 0)
		return;

	old_read_domains = obj->base.read_domains;
	old_write_domain = obj->base.write_domain;

	obj->base.read_domains &= ~I915_GEM_DOMAIN_GTT;
	obj->base.write_domain &= ~I915_GEM_DOMAIN_GTT;

	trace_i915_gem_object_change_domain(obj,
					    old_read_domains,
					    old_write_domain);
}

static void __i915_vma_iounmap(struct i915_vma *vma)
{
	GEM_BUG_ON(vma->pin_count);

	if (vma->iomap == NULL)
		return;

	io_mapping_unmap(vma->iomap);
	vma->iomap = NULL;
}

int i915_vma_unbind(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;
	unsigned long active;
	int ret;

	/* First wait upon any activity as retiring the request may
	 * have side-effects such as unpinning or even unbinding this vma.
	 */
	active = i915_vma_get_active(vma);
	if (active) {
		int idx;

		/* When a closed VMA is retired, it is unbound - eek.
		 * In order to prevent it from being recursively closed,
		 * take a pin on the vma so that the second unbind is
		 * aborted.
		 */
		vma->pin_count++;

		for_each_active(active, idx) {
			ret = i915_gem_active_retire(&vma->last_read[idx],
						   &vma->vm->dev->struct_mutex);
			if (ret)
				break;
		}

		vma->pin_count--;
		if (ret)
			return ret;

		GEM_BUG_ON(i915_vma_is_active(vma));
	}

	if (vma->pin_count)
		return -EBUSY;

	if (!drm_mm_node_allocated(&vma->node))
		goto destroy;

	GEM_BUG_ON(obj->bind_count == 0);
	GEM_BUG_ON(!obj->pages);

	if (vma->is_ggtt && vma->ggtt_view.type == I915_GGTT_VIEW_NORMAL) {
		i915_gem_object_finish_gtt(obj);

		/* release the fence reg _after_ flushing */
		ret = i915_gem_object_put_fence(obj);
		if (ret)
			return ret;

		__i915_vma_iounmap(vma);
	}

	if (likely(!vma->vm->closed)) {
		trace_i915_vma_unbind(vma);
		vma->vm->unbind_vma(vma);
	}
	vma->bound = 0;

	drm_mm_remove_node(&vma->node);
	list_move_tail(&vma->vm_link, &vma->vm->unbound_list);

	if (vma->is_ggtt) {
		if (vma->ggtt_view.type == I915_GGTT_VIEW_NORMAL) {
			obj->map_and_fenceable = false;
		} else if (vma->ggtt_view.pages) {
			sg_free_table(vma->ggtt_view.pages);
			kfree(vma->ggtt_view.pages);
		}
		vma->ggtt_view.pages = NULL;
	}

	/* Since the unbound list is global, only move to that list if
	 * no more VMAs exist. */
	if (--obj->bind_count == 0)
		list_move_tail(&obj->global_list,
			       &to_i915(obj->base.dev)->mm.unbound_list);

	/* And finally now the object is completely decoupled from this vma,
	 * we can drop its hold on the backing storage and allow it to be
	 * reaped by the shrinker.
	 */
	i915_gem_object_unpin_pages(obj);

destroy:
	if (unlikely(vma->closed))
		i915_vma_destroy(vma);

	return 0;
}

int i915_gem_wait_for_idle(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	int ret;

	lockdep_assert_held(&dev_priv->drm.struct_mutex);

	for_each_engine(engine, dev_priv) {
		if (engine->last_context == NULL)
			continue;

		ret = intel_engine_idle(engine);
		if (ret)
			return ret;
	}

	return 0;
}

static bool i915_gem_valid_gtt_space(struct i915_vma *vma,
				     unsigned long cache_level)
{
	struct drm_mm_node *gtt_space = &vma->node;
	struct drm_mm_node *other;

	/*
	 * On some machines we have to be careful when putting differing types
	 * of snoopable memory together to avoid the prefetcher crossing memory
	 * domains and dying. During vm initialisation, we decide whether or not
	 * these constraints apply and set the drm_mm.color_adjust
	 * appropriately.
	 */
	if (vma->vm->mm.color_adjust == NULL)
		return true;

	if (!drm_mm_node_allocated(gtt_space))
		return true;

	if (list_empty(&gtt_space->node_list))
		return true;

	other = list_entry(gtt_space->node_list.prev, struct drm_mm_node, node_list);
	if (other->allocated && !other->hole_follows && other->color != cache_level)
		return false;

	other = list_entry(gtt_space->node_list.next, struct drm_mm_node, node_list);
	if (other->allocated && !gtt_space->hole_follows && other->color != cache_level)
		return false;

	return true;
}

/**
 * Finds free space in the GTT aperture and binds the object or a view of it
 * there.
 * @obj: object to bind
 * @vm: address space to bind into
 * @ggtt_view: global gtt view if applicable
 * @size: requested size in bytes (can be larger than the VMA)
 * @alignment: requested alignment
 * @flags: mask of PIN_* flags to use
 */
static struct i915_vma *
i915_gem_object_insert_into_vm(struct drm_i915_gem_object *obj,
			       struct i915_address_space *vm,
			       const struct i915_ggtt_view *ggtt_view,
			       u64 size,
			       u64 alignment,
			       u64 flags)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	u64 start, end;
	u32 search_flag, alloc_flag;
	struct i915_vma *vma;
	int ret;

#ifdef __future__
/*
 * It appears that the antique xf86-intel driver in ports
 * does not set this correctly, so we need to bypass this
 * sanit check
 */
	if (obj->madv != I915_MADV_WILLNEED) {
		DRM_ERROR("Attempting to bind a purgeable object\n");
		return PTR_ERR(-EINVAL);
	}

#endif
	if (i915_is_ggtt(vm)) {
		u32 fence_size, fence_alignment, unfenced_alignment;
		u64 view_size;

		if (WARN_ON(!ggtt_view))
			return ERR_PTR(-EINVAL);

		view_size = i915_ggtt_view_size(obj, ggtt_view);

		fence_size = i915_gem_get_ggtt_size(dev,
						    view_size,
						    obj->tiling_mode);
		fence_alignment = i915_gem_get_ggtt_alignment(dev,
							      view_size,
							      obj->tiling_mode,
							      true);
		unfenced_alignment = i915_gem_get_ggtt_alignment(dev,
								 view_size,
								 obj->tiling_mode,
								 false);
		size = max(size, view_size);
		if (flags & PIN_MAPPABLE)
			size = max_t(u64, size, fence_size);

		if (alignment == 0)
			alignment = flags & PIN_MAPPABLE ? fence_alignment :
				unfenced_alignment;
		if (flags & PIN_MAPPABLE && alignment & (fence_alignment - 1)) {
			DRM_DEBUG("Invalid object (view type=%u) alignment requested %llx\n",
				  ggtt_view ? ggtt_view->type : 0,
				  alignment);
			return ERR_PTR(-EINVAL);
		}
	} else {
		size = max_t(u64, size, obj->base.size);
		alignment = 4096;
	}

	start = flags & PIN_OFFSET_BIAS ? flags & PIN_OFFSET_MASK : 0;
	end = vm->total;
	if (flags & PIN_MAPPABLE)
		end = min_t(u64, end, dev_priv->ggtt.mappable_end);
	if (flags & PIN_ZONE_4G)
		end = min_t(u64, end, (1ULL << 32) - PAGE_SIZE);

	/* If binding the object/GGTT view requires more space than the entire
	 * aperture has, reject it early before evicting everything in a vain
	 * attempt to find space.
	 */
	if (size > end) {
		DRM_DEBUG("Attempting to bind an object (view type=%u) larger than the aperture: request=%llu [object=%zd] > %s aperture=%llu\n",
			  ggtt_view ? ggtt_view->type : 0,
			  size, obj->base.size,
			  flags & PIN_MAPPABLE ? "mappable" : "total",
			  end);
		return ERR_PTR(-E2BIG);
	}

	ret = i915_gem_object_get_pages(obj);
	if (ret)
		return ERR_PTR(ret);

	i915_gem_object_pin_pages(obj);

	vma = ggtt_view ? i915_gem_obj_lookup_or_create_ggtt_vma(obj, ggtt_view) :
			  i915_gem_obj_lookup_or_create_vma(obj, vm);

	if (IS_ERR(vma))
		goto err_unpin;

	if (flags & PIN_OFFSET_FIXED) {
		uint64_t offset = flags & PIN_OFFSET_MASK;

		if (offset & (alignment - 1) || offset + size > end) {
			ret = -EINVAL;
			goto err_vma;
		}
		vma->node.start = offset;
		vma->node.size = size;
		vma->node.color = obj->cache_level;
		ret = drm_mm_reserve_node(&vm->mm, &vma->node);
		if (ret) {
			ret = i915_gem_evict_for_vma(vma);
			if (ret == 0)
				ret = drm_mm_reserve_node(&vm->mm, &vma->node);
		}
		if (ret)
			goto err_vma;
	} else {
		if (flags & PIN_HIGH) {
			search_flag = DRM_MM_SEARCH_BELOW;
			alloc_flag = DRM_MM_CREATE_TOP;
		} else {
			search_flag = DRM_MM_SEARCH_DEFAULT;
			alloc_flag = DRM_MM_CREATE_DEFAULT;
		}

		/* We only allocate in PAGE_SIZE/GTT_PAGE_SIZE (4096) chunks,
		 * so we know that we always have a minimum alignment of 4096.
		 * The drm_mm range manager is optimised to return results
		 * with zero alignment, so where possible use the optimal
		 * path.
		 */
		if (alignment <= 4096)
			alignment = 0;

search_free:
		ret = drm_mm_insert_node_in_range_generic(&vm->mm, &vma->node,
							  size, alignment,
							  obj->cache_level,
							  start, end,
							  search_flag,
							  alloc_flag);
		if (ret) {
			ret = i915_gem_evict_something(vm, size, alignment,
						       obj->cache_level,
						       start, end,
						       flags);
			if (ret == 0)
				goto search_free;

			goto err_vma;
		}
	}
	GEM_BUG_ON(!i915_gem_valid_gtt_space(vma, obj->cache_level));

	list_move_tail(&obj->global_list, &dev_priv->mm.bound_list);
	list_move_tail(&vma->vm_link, &vm->inactive_list);
	obj->bind_count++;

	return vma;

err_vma:
	vma = ERR_PTR(ret);
err_unpin:
	i915_gem_object_unpin_pages(obj);
	return vma;
}

bool
i915_gem_clflush_object(struct drm_i915_gem_object *obj,
			bool force)
{
	/* If we don't have a page list set up, then we're not pinned
	 * to GPU, and we can ignore the cache flush because it'll happen
	 * again at bind time.
	 */
	if (obj->pages == NULL)
		return false;

	/*
	 * Stolen memory is always coherent with the GPU as it is explicitly
	 * marked as wc by the system, or the system is cache-coherent.
	 */
	if (obj->stolen || obj->phys_handle)
		return false;

	/* If the GPU is snooping the contents of the CPU cache,
	 * we do not need to manually clear the CPU cache lines.  However,
	 * the caches are only snooped when the render cache is
	 * flushed/invalidated.  As we always have to emit invalidations
	 * and flushes when moving into and out of the RENDER domain, correct
	 * snooping behaviour occurs naturally as the result of our domain
	 * tracking.
	 */
	if (!force && cpu_cache_is_coherent(obj->base.dev, obj->cache_level)) {
		obj->cache_dirty = true;
		return false;
	}

	trace_i915_gem_object_clflush(obj);
	drm_clflush_sg(obj->pages);
	obj->cache_dirty = false;

	return true;
}

/** Flushes the GTT write domain for the object if it's dirty. */
static void
i915_gem_object_flush_gtt_write_domain(struct drm_i915_gem_object *obj)
{
	uint32_t old_write_domain;

	if (obj->base.write_domain != I915_GEM_DOMAIN_GTT)
		return;

	/* No actual flushing is required for the GTT write domain.  Writes
	 * to it immediately go to main memory as far as we know, so there's
	 * no chipset flush.  It also doesn't land in render cache.
	 *
	 * However, we do have to enforce the order so that all writes through
	 * the GTT land before any writes to the device, such as updates to
	 * the GATT itself.
	 */
	wmb();

	old_write_domain = obj->base.write_domain;
	obj->base.write_domain = 0;

	intel_fb_obj_flush(obj, false, ORIGIN_GTT);

	trace_i915_gem_object_change_domain(obj,
					    obj->base.read_domains,
					    old_write_domain);
}

/** Flushes the CPU write domain for the object if it's dirty. */
static void
i915_gem_object_flush_cpu_write_domain(struct drm_i915_gem_object *obj)
{
	uint32_t old_write_domain;

	if (obj->base.write_domain != I915_GEM_DOMAIN_CPU)
		return;

	if (i915_gem_clflush_object(obj, obj->pin_display))
		i915_gem_chipset_flush(to_i915(obj->base.dev));

	old_write_domain = obj->base.write_domain;
	obj->base.write_domain = 0;

	intel_fb_obj_flush(obj, false, ORIGIN_CPU);

	trace_i915_gem_object_change_domain(obj,
					    obj->base.read_domains,
					    old_write_domain);
}

/**
 * Moves a single object to the GTT read, and possibly write domain.
 * @obj: object to act on
 * @write: ask for write access or read only
 *
 * This function returns when the move is complete, including waiting on
 * flushes to occur.
 */
int
i915_gem_object_set_to_gtt_domain(struct drm_i915_gem_object *obj, bool write)
{
	uint32_t old_write_domain, old_read_domains;
	struct i915_vma *vma;
	int ret;

	ret = i915_gem_object_wait_rendering(obj, !write);
	if (ret)
		return ret;

	if (obj->base.write_domain == I915_GEM_DOMAIN_GTT)
		return 0;

	/* Flush and acquire obj->pages so that we are coherent through
	 * direct access in memory with previous cached writes through
	 * shmemfs and that our cache domain tracking remains valid.
	 * For example, if the obj->filp was moved to swap without us
	 * being notified and releasing the pages, we would mistakenly
	 * continue to assume that the obj remained out of the CPU cached
	 * domain.
	 */
	ret = i915_gem_object_get_pages(obj);
	if (ret)
		return ret;

	i915_gem_object_flush_cpu_write_domain(obj);

	/* Serialise direct access to this object with the barriers for
	 * coherent writes from the GPU, by effectively invalidating the
	 * GTT domain upon first access.
	 */
	if ((obj->base.read_domains & I915_GEM_DOMAIN_GTT) == 0)
		mb();

	old_write_domain = obj->base.write_domain;
	old_read_domains = obj->base.read_domains;

	/* It should now be out of any other write domains, and we can update
	 * the domain values for our changes.
	 */
	BUG_ON((obj->base.write_domain & ~I915_GEM_DOMAIN_GTT) != 0);
	obj->base.read_domains |= I915_GEM_DOMAIN_GTT;
	if (write) {
		obj->base.read_domains = I915_GEM_DOMAIN_GTT;
		obj->base.write_domain = I915_GEM_DOMAIN_GTT;
		obj->dirty = 1;
	}

	trace_i915_gem_object_change_domain(obj,
					    old_read_domains,
					    old_write_domain);

	/* And bump the LRU for this access */
	vma = i915_gem_obj_to_ggtt(obj);
	if (vma &&
	    drm_mm_node_allocated(&vma->node) &&
	    !i915_vma_is_active(vma))
		list_move_tail(&vma->vm_link, &vma->vm->inactive_list);

	return 0;
}

/**
 * Changes the cache-level of an object across all VMA.
 * @obj: object to act on
 * @cache_level: new cache level to set for the object
 *
 * After this function returns, the object will be in the new cache-level
 * across all GTT and the contents of the backing storage will be coherent,
 * with respect to the new cache-level. In order to keep the backing storage
 * coherent for all users, we only allow a single cache level to be set
 * globally on the object and prevent it from being changed whilst the
 * hardware is reading from the object. That is if the object is currently
 * on the scanout it will be set to uncached (or equivalent display
 * cache coherency) and all non-MOCS GPU access will also be uncached so
 * that all direct access to the scanout remains coherent.
 */
int i915_gem_object_set_cache_level(struct drm_i915_gem_object *obj,
				    enum i915_cache_level cache_level)
{
	struct i915_vma *vma;
	int ret = 0;

	if (obj->cache_level == cache_level)
		goto out;

	/* Inspect the list of currently bound VMA and unbind any that would
	 * be invalid given the new cache-level. This is principally to
	 * catch the issue of the CS prefetch crossing page boundaries and
	 * reading an invalid PTE on older architectures.
	 */
restart:
	list_for_each_entry(vma, &obj->vma_list, obj_link) {
		if (!drm_mm_node_allocated(&vma->node))
			continue;

		if (vma->pin_count) {
			DRM_DEBUG("can not change the cache level of pinned objects\n");
			return -EBUSY;
		}

		if (i915_gem_valid_gtt_space(vma, cache_level))
			continue;

		ret = i915_vma_unbind(vma);
		if (ret)
			return ret;

		/* As unbinding may affect other elements in the
		 * obj->vma_list (due to side-effects from retiring
		 * an active vma), play safe and restart the iterator.
		 */
		goto restart;
	}

	/* We can reuse the existing drm_mm nodes but need to change the
	 * cache-level on the PTE. We could simply unbind them all and
	 * rebind with the correct cache-level on next use. However since
	 * we already have a valid slot, dma mapping, pages etc, we may as
	 * rewrite the PTE in the belief that doing so tramples upon less
	 * state and so involves less work.
	 */
	if (obj->bind_count) {
		/* Before we change the PTE, the GPU must not be accessing it.
		 * If we wait upon the object, we know that all the bound
		 * VMA are no longer active.
		 */
		ret = i915_gem_object_wait_rendering(obj, false);
		if (ret)
			return ret;

		if (!HAS_LLC(obj->base.dev) && cache_level != I915_CACHE_NONE) {
			/* Access to snoopable pages through the GTT is
			 * incoherent and on some machines causes a hard
			 * lockup. Relinquish the CPU mmaping to force
			 * userspace to refault in the pages and we can
			 * then double check if the GTT mapping is still
			 * valid for that pointer access.
			 */
			i915_gem_release_mmap(obj);

			/* As we no longer need a fence for GTT access,
			 * we can relinquish it now (and so prevent having
			 * to steal a fence from someone else on the next
			 * fence request). Note GPU activity would have
			 * dropped the fence as all snoopable access is
			 * supposed to be linear.
			 */
			ret = i915_gem_object_put_fence(obj);
			if (ret)
				return ret;
		} else {
			/* We either have incoherent backing store and
			 * so no GTT access or the architecture is fully
			 * coherent. In such cases, existing GTT mmaps
			 * ignore the cache bit in the PTE and we can
			 * rewrite it without confusing the GPU or having
			 * to force userspace to fault back in its mmaps.
			 */
		}

		list_for_each_entry(vma, &obj->vma_list, obj_link) {
			if (!drm_mm_node_allocated(&vma->node))
				continue;

			ret = i915_vma_bind(vma, cache_level, PIN_UPDATE);
			if (ret)
				return ret;
		}
	}

	list_for_each_entry(vma, &obj->vma_list, obj_link)
		vma->node.color = cache_level;
	obj->cache_level = cache_level;

out:
	/* Flush the dirty CPU caches to the backing storage so that the
	 * object is now coherent at its new cache level (with respect
	 * to the access domain).
	 */
	if (obj->cache_dirty && cpu_write_needs_clflush(obj)) {
		if (i915_gem_clflush_object(obj, true))
			i915_gem_chipset_flush(to_i915(obj->base.dev));
	}

	return 0;
}

int i915_gem_get_caching_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_i915_gem_caching *args = data;
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	switch (obj->cache_level) {
	case I915_CACHE_LLC:
	case I915_CACHE_L3_LLC:
		args->caching = I915_CACHING_CACHED;
		break;

	case I915_CACHE_WT:
		args->caching = I915_CACHING_DISPLAY;
		break;

	default:
		args->caching = I915_CACHING_NONE;
		break;
	}

	i915_gem_object_put_unlocked(obj);
	return 0;
}

int i915_gem_set_caching_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_caching *args = data;
	struct drm_i915_gem_object *obj;
	enum i915_cache_level level;
	int ret;

	switch (args->caching) {
	case I915_CACHING_NONE:
		level = I915_CACHE_NONE;
		break;
	case I915_CACHING_CACHED:
		/*
		 * Due to a HW issue on BXT A stepping, GPU stores via a
		 * snooped mapping may leave stale data in a corresponding CPU
		 * cacheline, whereas normally such cachelines would get
		 * invalidated.
		 */
		if (!HAS_LLC(dev) && !HAS_SNOOP(dev))
			return -ENODEV;

		level = I915_CACHE_LLC;
		break;
	case I915_CACHING_DISPLAY:
		level = HAS_WT(dev) ? I915_CACHE_WT : I915_CACHE_NONE;
		break;
	default:
		return -EINVAL;
	}

	intel_runtime_pm_get(dev_priv);

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		goto rpm_put;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	ret = i915_gem_object_set_cache_level(obj, level);

	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
rpm_put:
	intel_runtime_pm_put(dev_priv);

	return ret;
}

/*
 * Prepare buffer for display plane (scanout, cursors, etc).
 * Can be called from an uninterruptible phase (modesetting) and allows
 * any flushes to be pipelined (for pageflips).
 */
int
i915_gem_object_pin_to_display_plane(struct drm_i915_gem_object *obj,
				     u32 alignment,
				     const struct i915_ggtt_view *view)
{
	u32 old_read_domains, old_write_domain;
	int ret;

	/* Mark the pin_display early so that we account for the
	 * display coherency whilst setting up the cache domains.
	 */
	obj->pin_display++;

	/* The display engine is not coherent with the LLC cache on gen6.  As
	 * a result, we make sure that the pinning that is about to occur is
	 * done with uncached PTEs. This is lowest common denominator for all
	 * chipsets.
	 *
	 * However for gen6+, we could do better by using the GFDT bit instead
	 * of uncaching, which would allow us to flush all the LLC-cached data
	 * with that bit in the PTE to main memory with just one PIPE_CONTROL.
	 */
	ret = i915_gem_object_set_cache_level(obj,
					      HAS_WT(obj->base.dev) ? I915_CACHE_WT : I915_CACHE_NONE);
	if (ret)
		goto err_unpin_display;

	/* As the user may map the buffer once pinned in the display plane
	 * (e.g. libkms for the bootup splash), we have to ensure that we
	 * always use map_and_fenceable for all scanout buffers.
	 */
	ret = i915_gem_object_ggtt_pin(obj, view, 0, alignment,
				       view->type == I915_GGTT_VIEW_NORMAL ?
				       PIN_MAPPABLE : 0);
	if (ret)
		goto err_unpin_display;

	i915_gem_object_flush_cpu_write_domain(obj);

	old_write_domain = obj->base.write_domain;
	old_read_domains = obj->base.read_domains;

	/* It should now be out of any other write domains, and we can update
	 * the domain values for our changes.
	 */
	obj->base.write_domain = 0;
	obj->base.read_domains |= I915_GEM_DOMAIN_GTT;

	trace_i915_gem_object_change_domain(obj,
					    old_read_domains,
					    old_write_domain);

	return 0;

err_unpin_display:
	obj->pin_display--;
	return ret;
}

void
i915_gem_object_unpin_from_display_plane(struct drm_i915_gem_object *obj,
					 const struct i915_ggtt_view *view)
{
	if (WARN_ON(obj->pin_display == 0))
		return;

	i915_gem_object_ggtt_unpin_view(obj, view);

	obj->pin_display--;
}

/**
 * Moves a single object to the CPU read, and possibly write domain.
 * @obj: object to act on
 * @write: requesting write or read-only access
 *
 * This function returns when the move is complete, including waiting on
 * flushes to occur.
 */
int
i915_gem_object_set_to_cpu_domain(struct drm_i915_gem_object *obj, bool write)
{
	uint32_t old_write_domain, old_read_domains;
	int ret;

	ret = i915_gem_object_wait_rendering(obj, !write);
	if (ret)
		return ret;

	if (obj->base.write_domain == I915_GEM_DOMAIN_CPU)
		return 0;

	i915_gem_object_flush_gtt_write_domain(obj);

	old_write_domain = obj->base.write_domain;
	old_read_domains = obj->base.read_domains;

	/* Flush the CPU cache if it's still invalid. */
	if ((obj->base.read_domains & I915_GEM_DOMAIN_CPU) == 0) {
		i915_gem_clflush_object(obj, false);

		obj->base.read_domains |= I915_GEM_DOMAIN_CPU;
	}

	/* It should now be out of any other write domains, and we can update
	 * the domain values for our changes.
	 */
	BUG_ON((obj->base.write_domain & ~I915_GEM_DOMAIN_CPU) != 0);

	/* If we're writing through the CPU, then the GPU read domains will
	 * need to be invalidated at next use.
	 */
	if (write) {
		obj->base.read_domains = I915_GEM_DOMAIN_CPU;
		obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	}

	trace_i915_gem_object_change_domain(obj,
					    old_read_domains,
					    old_write_domain);

	return 0;
}

/* Throttle our rendering by waiting until the ring has completed our requests
 * emitted over 20 msec ago.
 *
 * Note that if we were to use the current jiffies each time around the loop,
 * we wouldn't escape the function with any frames outstanding if the time to
 * render a frame was over 20ms.
 *
 * This should get us reasonable parallelism between CPU and GPU but also
 * relatively low latency when blocking on a particular request to finish.
 */
static int
i915_gem_ring_throttle(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_file_private *file_priv = file->driver_priv;
	unsigned long recent_enough = jiffies - DRM_I915_THROTTLE_JIFFIES;
	struct drm_i915_gem_request *request, *target = NULL;
	int ret;

	ret = i915_gem_wait_for_error(&dev_priv->gpu_error);
	if (ret)
		return ret;

	/* ABI: return -EIO if already wedged */
	if (i915_terminally_wedged(&dev_priv->gpu_error))
		return -EIO;

	spin_lock(&file_priv->mm.lock);
	list_for_each_entry(request, &file_priv->mm.request_list, client_list) {
		if (time_after_eq(request->emitted_jiffies, recent_enough))
			break;

		/*
		 * Note that the request might not have been submitted yet.
		 * In which case emitted_jiffies will be zero.
		 */
		if (!request->emitted_jiffies)
			continue;

		target = request;
	}
	if (target)
		i915_gem_request_get(target);
	spin_unlock(&file_priv->mm.lock);

	if (target == NULL)
		return 0;

	ret = i915_wait_request(target, true, NULL, NULL);
	i915_gem_request_put(target);

	return ret;
}

static bool
i915_vma_misplaced(struct i915_vma *vma, u64 size, u64 alignment, u64 flags)
{
	struct drm_i915_gem_object *obj = vma->obj;

	if (vma->node.size < size)
		return true;

	if (alignment && vma->node.start & (alignment - 1))
		return true;

	if (flags & PIN_MAPPABLE && !obj->map_and_fenceable)
		return true;

	if (flags & PIN_OFFSET_BIAS &&
	    vma->node.start < (flags & PIN_OFFSET_MASK))
		return true;

	if (flags & PIN_OFFSET_FIXED &&
	    vma->node.start != (flags & PIN_OFFSET_MASK))
		return true;

	return false;
}

void __i915_vma_set_map_and_fenceable(struct i915_vma *vma)
{
	struct drm_i915_gem_object *obj = vma->obj;
	bool mappable, fenceable;
	u32 fence_size, fence_alignment;

	fence_size = i915_gem_get_ggtt_size(obj->base.dev,
					    obj->base.size,
					    obj->tiling_mode);
	fence_alignment = i915_gem_get_ggtt_alignment(obj->base.dev,
						      obj->base.size,
						      obj->tiling_mode,
						      true);

	fenceable = (vma->node.size == fence_size &&
		     (vma->node.start & (fence_alignment - 1)) == 0);

	mappable = (vma->node.start + fence_size <=
		    to_i915(obj->base.dev)->ggtt.mappable_end);

	obj->map_and_fenceable = mappable && fenceable;
}

static int
i915_gem_object_do_pin(struct drm_i915_gem_object *obj,
		       struct i915_address_space *vm,
		       const struct i915_ggtt_view *ggtt_view,
		       u64 size,
		       u64 alignment,
		       u64 flags)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	struct i915_vma *vma;
	unsigned bound;
	int ret;

	if (WARN_ON(vm == &dev_priv->mm.aliasing_ppgtt->base))
		return -ENODEV;

	if (WARN_ON(flags & (PIN_GLOBAL | PIN_MAPPABLE) && !i915_is_ggtt(vm)))
		return -EINVAL;

	if (WARN_ON((flags & (PIN_MAPPABLE | PIN_GLOBAL)) == PIN_MAPPABLE))
		return -EINVAL;

	if (WARN_ON(i915_is_ggtt(vm) != !!ggtt_view))
		return -EINVAL;

	vma = ggtt_view ? i915_gem_obj_to_ggtt_view(obj, ggtt_view) :
			  i915_gem_obj_to_vma(obj, vm);

	if (vma) {
		if (WARN_ON(vma->pin_count == DRM_I915_GEM_OBJECT_MAX_PIN_COUNT))
			return -EBUSY;

		if (i915_vma_misplaced(vma, size, alignment, flags)) {
			WARN(vma->pin_count,
			     "bo is already pinned in %s with incorrect alignment:"
			     " offset=%08x %08x, req.alignment=%llx, req.map_and_fenceable=%d,"
			     " obj->map_and_fenceable=%d\n",
			     ggtt_view ? "ggtt" : "ppgtt",
			     upper_32_bits(vma->node.start),
			     lower_32_bits(vma->node.start),
			     alignment,
			     !!(flags & PIN_MAPPABLE),
			     obj->map_and_fenceable);
			ret = i915_vma_unbind(vma);
			if (ret)
				return ret;

			vma = NULL;
		}
	}

	if (vma == NULL || !drm_mm_node_allocated(&vma->node)) {
		vma = i915_gem_object_insert_into_vm(obj, vm, ggtt_view,
						     size, alignment, flags);
		if (IS_ERR(vma))
			return PTR_ERR(vma);
	}

	bound = vma->bound;
	ret = i915_vma_bind(vma, obj->cache_level, flags);
	if (ret)
		return ret;

	if (ggtt_view && ggtt_view->type == I915_GGTT_VIEW_NORMAL &&
	    (bound ^ vma->bound) & GLOBAL_BIND) {
		__i915_vma_set_map_and_fenceable(vma);
		WARN_ON(flags & PIN_MAPPABLE && !obj->map_and_fenceable);
	}

	GEM_BUG_ON(i915_vma_misplaced(vma, size, alignment, flags));

	vma->pin_count++;
	return 0;
}

int
i915_gem_object_pin(struct drm_i915_gem_object *obj,
		    struct i915_address_space *vm,
		    u64 size,
		    u64 alignment,
		    u64 flags)
{
	return i915_gem_object_do_pin(obj, vm,
				      i915_is_ggtt(vm) ? &i915_ggtt_view_normal : NULL,
				      size, alignment, flags);
}

int
i915_gem_object_ggtt_pin(struct drm_i915_gem_object *obj,
			 const struct i915_ggtt_view *view,
			 u64 size,
			 u64 alignment,
			 u64 flags)
{
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i915_ggtt *ggtt = &dev_priv->ggtt;

	BUG_ON(!view);

	return i915_gem_object_do_pin(obj, &ggtt->base, view,
				      size, alignment, flags | PIN_GLOBAL);
}

void
i915_gem_object_ggtt_unpin_view(struct drm_i915_gem_object *obj,
				const struct i915_ggtt_view *view)
{
	struct i915_vma *vma = i915_gem_obj_to_ggtt_view(obj, view);

	WARN_ON(vma->pin_count == 0);
	WARN_ON(!i915_gem_obj_ggtt_bound_view(obj, view));

	--vma->pin_count;
}

int
i915_gem_busy_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file)
{
	struct drm_i915_gem_busy *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	/* Count all active objects as busy, even if they are currently not used
	 * by the gpu. Users of this interface expect objects to eventually
	 * become non-busy without any further actions.
	 */
	args->busy = 0;
	if (obj->active) {
		struct drm_i915_gem_request *req;
		int i;

		for (i = 0; i < I915_NUM_ENGINES; i++) {
			req = i915_gem_active_peek(&obj->last_read[i],
						   &obj->base.dev->struct_mutex);
			if (req)
				args->busy |= 1 << (16 + req->engine->exec_id);
		}
		req = i915_gem_active_peek(&obj->last_write,
					   &obj->base.dev->struct_mutex);
		if (req)
			args->busy |= req->engine->exec_id;
	}

	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

int
i915_gem_throttle_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	return i915_gem_ring_throttle(dev, file_priv);
}

int
i915_gem_madvise_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_madvise *args = data;
	struct drm_i915_gem_object *obj;
	int ret;

	switch (args->madv) {
	case I915_MADV_DONTNEED:
	case I915_MADV_WILLNEED:
	    break;
	default:
	    return -EINVAL;
	}

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	obj = i915_gem_object_lookup(file_priv, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	if (i915_gem_obj_is_pinned(obj)) {
		ret = -EINVAL;
		goto out;
	}

	if (obj->pages &&
	    obj->tiling_mode != I915_TILING_NONE &&
	    dev_priv->quirks & QUIRK_PIN_SWIZZLED_PAGES) {
		if (obj->madv == I915_MADV_WILLNEED)
			i915_gem_object_unpin_pages(obj);
		if (args->madv == I915_MADV_WILLNEED)
			i915_gem_object_pin_pages(obj);
	}

	if (obj->madv != __I915_MADV_PURGED)
		obj->madv = args->madv;

	/* if the object is no longer attached, discard its backing storage */
	if (obj->madv == I915_MADV_DONTNEED && obj->pages == NULL)
		i915_gem_object_truncate(obj);

	args->retained = obj->madv != __I915_MADV_PURGED;

out:
	i915_gem_object_put(obj);
unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

void i915_gem_object_init(struct drm_i915_gem_object *obj,
			  const struct drm_i915_gem_object_ops *ops)
{
	int i;

	INIT_LIST_HEAD(&obj->global_list);
	for (i = 0; i < I915_NUM_ENGINES; i++)
		init_request_active(&obj->last_read[i],
				    i915_gem_object_retire__read);
	init_request_active(&obj->last_write,
			    i915_gem_object_retire__write);
	init_request_active(&obj->last_fence, NULL);
	INIT_LIST_HEAD(&obj->obj_exec_link);
	INIT_LIST_HEAD(&obj->vma_list);
	INIT_LIST_HEAD(&obj->batch_pool_link);

	obj->ops = ops;

	obj->fence_reg = I915_FENCE_REG_NONE;
	obj->madv = I915_MADV_WILLNEED;

	i915_gem_info_add_obj(to_i915(obj->base.dev), obj->base.size);
}

static const struct drm_i915_gem_object_ops i915_gem_object_ops = {
	.flags = I915_GEM_OBJECT_HAS_STRUCT_PAGE,
	.get_pages = i915_gem_object_get_pages_gtt,
	.put_pages = i915_gem_object_put_pages_gtt,
};

struct drm_i915_gem_object *i915_gem_object_create(struct drm_device *dev,
						  size_t size)
{
	struct drm_i915_gem_object *obj;
	struct address_space *mapping;
	gfp_t mask;
	int ret;

	obj = i915_gem_object_alloc(dev);
	if (obj == NULL)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_object_init(dev, &obj->base, size);
	if (ret)
		goto fail;

	mask = GFP_HIGHUSER | __GFP_RECLAIMABLE;
	if (IS_CRESTLINE(dev) || IS_BROADWATER(dev)) {
		/* 965gm cannot relocate objects above 4GiB. */
		mask &= ~__GFP_HIGHMEM;
		mask |= __GFP_DMA32;
	}

	mapping = obj->base.filp->f_mapping;
	mapping_set_gfp_mask(mapping, mask);

	i915_gem_object_init(obj, &i915_gem_object_ops);

	obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	obj->base.read_domains = I915_GEM_DOMAIN_CPU;

	if (HAS_LLC(dev)) {
		/* On some devices, we can have the GPU use the LLC (the CPU
		 * cache) for about a 10% performance improvement
		 * compared to uncached.  Graphics requests other than
		 * display scanout are coherent with the CPU in
		 * accessing this cache.  This means in this mode we
		 * don't need to clflush on the CPU side, and on the
		 * GPU side we only need to flush internal caches to
		 * get data visible to the CPU.
		 *
		 * However, we maintain the display planes as UC, and so
		 * need to rebind when first used as such.
		 */
		obj->cache_level = I915_CACHE_LLC;
	} else
		obj->cache_level = I915_CACHE_NONE;

	trace_i915_gem_object_create(obj);

	return obj;

fail:
	i915_gem_object_free(obj);

	return ERR_PTR(ret);
}

static bool discard_backing_storage(struct drm_i915_gem_object *obj)
{
	/* If we are the last user of the backing storage (be it shmemfs
	 * pages or stolen etc), we know that the pages are going to be
	 * immediately released. In this case, we can then skip copying
	 * back the contents from the GPU.
	 */

	if (obj->madv != I915_MADV_WILLNEED)
		return false;

	if (obj->base.filp == NULL)
		return true;

	/* At first glance, this looks racy, but then again so would be
	 * userspace racing mmap against close. However, the first external
	 * reference to the filp can only be obtained through the
	 * i915_gem_mmap_ioctl() which safeguards us against the user
	 * acquiring such a reference whilst we are in the middle of
	 * freeing the object.
	 */
	return atomic_long_read(&obj->base.filp->f_count) == 1;
}

void i915_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct drm_i915_gem_object *obj = to_intel_bo(gem_obj);
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct i915_vma *vma, *next;

	intel_runtime_pm_get(dev_priv);

	trace_i915_gem_object_destroy(obj);

	/* All file-owned VMA should have been released by this point through
	 * i915_gem_close_object(), or earlier by i915_gem_context_close().
	 * However, the object may also be bound into the global GTT (e.g.
	 * older GPUs without per-process support, or for direct access through
	 * the GTT either for the user or for scanout). Those VMA still need to
	 * unbound now.
	 */
	list_for_each_entry_safe(vma, next, &obj->vma_list, obj_link) {
		GEM_BUG_ON(!vma->is_ggtt);
		GEM_BUG_ON(i915_vma_is_active(vma));
		vma->pin_count = 0;
		i915_vma_close(vma);
	}
	GEM_BUG_ON(obj->bind_count);

	/* Stolen objects don't hold a ref, but do hold pin count. Fix that up
	 * before progressing. */
	if (obj->stolen)
		i915_gem_object_unpin_pages(obj);

	WARN_ON(obj->frontbuffer_bits);

	if (obj->pages && obj->madv == I915_MADV_WILLNEED &&
	    dev_priv->quirks & QUIRK_PIN_SWIZZLED_PAGES &&
	    obj->tiling_mode != I915_TILING_NONE)
		i915_gem_object_unpin_pages(obj);

	if (WARN_ON(obj->pages_pin_count))
		obj->pages_pin_count = 0;
	if (discard_backing_storage(obj))
		obj->madv = I915_MADV_DONTNEED;
	i915_gem_object_put_pages(obj);

	BUG_ON(obj->pages);

	if (obj->base.import_attach)
		drm_prime_gem_destroy(&obj->base, NULL);

	if (obj->ops->release)
		obj->ops->release(obj);

	drm_gem_object_release(&obj->base);
	i915_gem_info_remove_obj(dev_priv, obj->base.size);

	kfree(obj->bit_17);
	i915_gem_object_free(obj);

	intel_runtime_pm_put(dev_priv);
}

struct i915_vma *i915_gem_obj_to_vma(struct drm_i915_gem_object *obj,
				     struct i915_address_space *vm)
{
	struct i915_vma *vma;
	list_for_each_entry(vma, &obj->vma_list, obj_link) {
		if (vma->ggtt_view.type == I915_GGTT_VIEW_NORMAL &&
		    vma->vm == vm)
			return vma;
	}
	return NULL;
}

struct i915_vma *i915_gem_obj_to_ggtt_view(struct drm_i915_gem_object *obj,
					   const struct i915_ggtt_view *view)
{
	struct i915_vma *vma;

	GEM_BUG_ON(!view);

	list_for_each_entry(vma, &obj->vma_list, obj_link)
		if (vma->is_ggtt && i915_ggtt_view_equal(&vma->ggtt_view, view))
			return vma;
	return NULL;
}

static void
i915_gem_stop_engines(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_engine_cs *engine;

	for_each_engine(engine, dev_priv)
		dev_priv->gt.stop_engine(engine);
}

int
i915_gem_suspend(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	int ret = 0;

	intel_suspend_gt_powersave(dev_priv);

	mutex_lock(&dev->struct_mutex);

	/* We have to flush all the executing contexts to main memory so
	 * that they can saved in the hibernation image. To ensure the last
	 * context image is coherent, we have to switch away from it. That
	 * leaves the dev_priv->kernel_context still active when
	 * we actually suspend, and its image in memory may not match the GPU
	 * state. Fortunately, the kernel_context is disposable and we do
	 * not rely on its state.
	 */
	ret = i915_gem_switch_to_kernel_context(dev_priv);
	if (ret)
		goto err;

	ret = i915_gem_wait_for_idle(dev_priv);
	if (ret)
		goto err;

	i915_gem_retire_requests(dev_priv);

	/* Note that rather than stopping the engines, all we have to do
	 * is assert that every RING_HEAD == RING_TAIL (all execution complete)
	 * and similar for all logical context images (to ensure they are
	 * all ready for hibernation).
	 */
	i915_gem_stop_engines(dev);
	i915_gem_context_lost(dev_priv);
	mutex_unlock(&dev->struct_mutex);

	cancel_delayed_work_sync(&dev_priv->gpu_error.hangcheck_work);
	cancel_delayed_work_sync(&dev_priv->gt.retire_work);
	flush_delayed_work(&dev_priv->gt.idle_work);

	/* Assert that we sucessfully flushed all the work and
	 * reset the GPU back to its idle, low power state.
	 */
	WARN_ON(dev_priv->gt.awake);

	return 0;

err:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}

void i915_gem_resume(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	mutex_lock(&dev->struct_mutex);
	i915_gem_restore_gtt_mappings(dev);

	/* As we didn't flush the kernel context before suspend, we cannot
	 * guarantee that the context image is complete. So let's just reset
	 * it and start again.
	 */
	if (i915.enable_execlists)
		intel_lr_context_reset(dev_priv, dev_priv->kernel_context);

	mutex_unlock(&dev->struct_mutex);
}

void i915_gem_init_swizzling(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	if (INTEL_INFO(dev)->gen < 5 ||
	    dev_priv->mm.bit_6_swizzle_x == I915_BIT_6_SWIZZLE_NONE)
		return;

	I915_WRITE(DISP_ARB_CTL, I915_READ(DISP_ARB_CTL) |
				 DISP_TILE_SURFACE_SWIZZLING);

	if (IS_GEN5(dev))
		return;

	I915_WRITE(TILECTL, I915_READ(TILECTL) | TILECTL_SWZCTL);
	if (IS_GEN6(dev))
		I915_WRITE(ARB_MODE, _MASKED_BIT_ENABLE(ARB_MODE_SWIZZLE_SNB));
	else if (IS_GEN7(dev))
		I915_WRITE(ARB_MODE, _MASKED_BIT_ENABLE(ARB_MODE_SWIZZLE_IVB));
	else if (IS_GEN8(dev))
		I915_WRITE(GAMTARBMODE, _MASKED_BIT_ENABLE(ARB_MODE_SWIZZLE_BDW));
	else
		BUG();
}

static void init_unused_ring(struct drm_device *dev, u32 base)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	I915_WRITE(RING_CTL(base), 0);
	I915_WRITE(RING_HEAD(base), 0);
	I915_WRITE(RING_TAIL(base), 0);
	I915_WRITE(RING_START(base), 0);
}

static void init_unused_rings(struct drm_device *dev)
{
	if (IS_I830(dev)) {
		init_unused_ring(dev, PRB1_BASE);
		init_unused_ring(dev, SRB0_BASE);
		init_unused_ring(dev, SRB1_BASE);
		init_unused_ring(dev, SRB2_BASE);
		init_unused_ring(dev, SRB3_BASE);
	} else if (IS_GEN2(dev)) {
		init_unused_ring(dev, SRB0_BASE);
		init_unused_ring(dev, SRB1_BASE);
	} else if (IS_GEN3(dev)) {
		init_unused_ring(dev, PRB1_BASE);
		init_unused_ring(dev, PRB2_BASE);
	}
}

int
i915_gem_init_hw(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_engine_cs *engine;
	int ret;

	/* Double layer security blanket, see i915_gem_init() */
	intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

	if (HAS_EDRAM(dev) && INTEL_GEN(dev_priv) < 9)
		I915_WRITE(HSW_IDICR, I915_READ(HSW_IDICR) | IDIHASHMSK(0xf));

	if (IS_HASWELL(dev))
		I915_WRITE(MI_PREDICATE_RESULT_2, IS_HSW_GT3(dev) ?
			   LOWER_SLICE_ENABLED : LOWER_SLICE_DISABLED);

	if (HAS_PCH_NOP(dev)) {
		if (IS_IVYBRIDGE(dev)) {
			u32 temp = I915_READ(GEN7_MSG_CTL);
			temp &= ~(WAIT_FOR_PCH_FLR_ACK | WAIT_FOR_PCH_RESET_ACK);
			I915_WRITE(GEN7_MSG_CTL, temp);
		} else if (INTEL_INFO(dev)->gen >= 7) {
			u32 temp = I915_READ(HSW_NDE_RSTWRN_OPT);
			temp &= ~RESET_PCH_HANDSHAKE_ENABLE;
			I915_WRITE(HSW_NDE_RSTWRN_OPT, temp);
		}
	}

	i915_gem_init_swizzling(dev);

	/*
	 * At least 830 can leave some of the unused rings
	 * "active" (ie. head != tail) after resume which
	 * will prevent c3 entry. Makes sure all unused rings
	 * are totally idle.
	 */
	init_unused_rings(dev);

	BUG_ON(!dev_priv->kernel_context);

	ret = i915_ppgtt_init_hw(dev);
	if (ret) {
		DRM_ERROR("PPGTT enable HW failed %d\n", ret);
		goto out;
	}

	/* Need to do basic initialisation of all rings first: */
	for_each_engine(engine, dev_priv) {
		ret = engine->init_hw(engine);
		if (ret)
			goto out;
	}

	intel_mocs_init_l3cc_table(dev);

	/* We can't enable contexts until all firmware is loaded */
	ret = intel_guc_setup(dev);
	if (ret)
		goto out;

out:
	intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	return ret;
}

bool intel_sanitize_semaphores(struct drm_i915_private *dev_priv, int value)
{
	if (INTEL_INFO(dev_priv)->gen < 6)
		return false;

	/* TODO: make semaphores and Execlists play nicely together */
	if (i915.enable_execlists)
		return false;

	if (value >= 0)
		return value;

#ifdef CONFIG_INTEL_IOMMU
	/* Enable semaphores on SNB when IO remapping is off */
	if (INTEL_INFO(dev_priv)->gen == 6 && intel_iommu_gfx_mapped)
		return false;
#endif

	return true;
}

int i915_gem_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	int ret;

	mutex_lock(&dev->struct_mutex);

	if (!i915.enable_execlists) {
		dev_priv->gt.cleanup_engine = intel_engine_cleanup;
		dev_priv->gt.stop_engine = intel_engine_stop;
	} else {
		dev_priv->gt.cleanup_engine = intel_logical_ring_cleanup;
		dev_priv->gt.stop_engine = intel_logical_ring_stop;
	}

	/* This is just a security blanket to placate dragons.
	 * On some systems, we very sporadically observe that the first TLBs
	 * used by the CS may be stale, despite us poking the TLB reset. If
	 * we hold the forcewake during initialisation these problems
	 * just magically go away.
	 */
	intel_uncore_forcewake_get(dev_priv, FORCEWAKE_ALL);

	i915_gem_init_userptr(dev_priv);

	ret = i915_gem_init_ggtt(dev_priv);
	if (ret)
		goto out_unlock;

	ret = i915_gem_context_init(dev);
	if (ret)
		goto out_unlock;

	ret = intel_engines_init(dev);
	if (ret)
		goto out_unlock;

	ret = i915_gem_init_hw(dev);
	if (ret == -EIO) {
		/* Allow engine initialisation to fail by marking the GPU as
		 * wedged. But we only want to do this where the GPU is angry,
		 * for all other failure, such as an allocation failure, bail.
		 */
		DRM_ERROR("Failed to initialize GPU, declaring it wedged\n");
		atomic_or(I915_WEDGED, &dev_priv->gpu_error.reset_counter);
		ret = 0;
	}

out_unlock:
	intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

void
i915_gem_cleanup_engines(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_engine_cs *engine;

	for_each_engine(engine, dev_priv)
		dev_priv->gt.cleanup_engine(engine);
}

static void
init_engine_lists(struct intel_engine_cs *engine)
{
	INIT_LIST_HEAD(&engine->request_list);
}

void
i915_gem_load_init_fences(struct drm_i915_private *dev_priv)
{
	struct drm_device *dev = &dev_priv->drm;

	if (INTEL_INFO(dev_priv)->gen >= 7 && !IS_VALLEYVIEW(dev_priv) &&
	    !IS_CHERRYVIEW(dev_priv))
		dev_priv->num_fence_regs = 32;
	else if (INTEL_INFO(dev_priv)->gen >= 4 || IS_I945G(dev_priv) ||
		 IS_I945GM(dev_priv) || IS_G33(dev_priv))
		dev_priv->num_fence_regs = 16;
	else
		dev_priv->num_fence_regs = 8;

	if (intel_vgpu_active(dev_priv))
		dev_priv->num_fence_regs =
				I915_READ(vgtif_reg(avail_rs.fence_num));

	/* Initialize fence registers to zero */
	i915_gem_restore_fences(dev);

	i915_gem_detect_bit_6_swizzle(dev);
}

void
i915_gem_load_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	int i;

	dev_priv->objects =
		kmem_cache_create("i915_gem_object",
				  sizeof(struct drm_i915_gem_object), 0,
				  SLAB_HWCACHE_ALIGN,
				  NULL);
	dev_priv->vmas =
		kmem_cache_create("i915_gem_vma",
				  sizeof(struct i915_vma), 0,
				  SLAB_HWCACHE_ALIGN,
				  NULL);
	dev_priv->requests =
		kmem_cache_create("i915_gem_request",
				  sizeof(struct drm_i915_gem_request), 0,
				  SLAB_HWCACHE_ALIGN,
				  NULL);

	INIT_LIST_HEAD(&dev_priv->context_list);
	INIT_LIST_HEAD(&dev_priv->mm.unbound_list);
	INIT_LIST_HEAD(&dev_priv->mm.bound_list);
	INIT_LIST_HEAD(&dev_priv->mm.fence_list);
	for (i = 0; i < I915_NUM_ENGINES; i++)
		init_engine_lists(&dev_priv->engine[i]);
	for (i = 0; i < I915_MAX_NUM_FENCES; i++)
		INIT_LIST_HEAD(&dev_priv->fence_regs[i].lru_list);
	INIT_DELAYED_WORK(&dev_priv->gt.retire_work,
			  i915_gem_retire_work_handler);
	INIT_DELAYED_WORK(&dev_priv->gt.idle_work,
			  i915_gem_idle_work_handler);
	init_waitqueue_head(&dev_priv->gpu_error.wait_queue);
	init_waitqueue_head(&dev_priv->gpu_error.reset_queue);

	dev_priv->relative_constants_mode = I915_EXEC_CONSTANTS_REL_GENERAL;

	INIT_LIST_HEAD(&dev_priv->mm.fence_list);

	init_waitqueue_head(&dev_priv->pending_flip_queue);

	dev_priv->mm.interruptible = true;

	mutex_init(&dev_priv->fb_tracking.lock);
}

void i915_gem_load_cleanup(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = to_i915(dev);

	kmem_cache_destroy(dev_priv->requests);
	kmem_cache_destroy(dev_priv->vmas);
	kmem_cache_destroy(dev_priv->objects);
}

int i915_gem_freeze_late(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *obj;

	/* Called just before we write the hibernation image.
	 *
	 * We need to update the domain tracking to reflect that the CPU
	 * will be accessing all the pages to create and restore from the
	 * hibernation, and so upon restoration those pages will be in the
	 * CPU domain.
	 *
	 * To make sure the hibernation image contains the latest state,
	 * we update that state just before writing out the image.
	 */

	list_for_each_entry(obj, &dev_priv->mm.unbound_list, global_list) {
		obj->base.read_domains = I915_GEM_DOMAIN_CPU;
		obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	}

	list_for_each_entry(obj, &dev_priv->mm.bound_list, global_list) {
		obj->base.read_domains = I915_GEM_DOMAIN_CPU;
		obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	}

	return 0;
}

void i915_gem_release(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_gem_request *request;

	/* Clean up our request list when the client is going away, so that
	 * later retire_requests won't dereference our soon-to-be-gone
	 * file_priv.
	 */
	spin_lock(&file_priv->mm.lock);
	list_for_each_entry(request, &file_priv->mm.request_list, client_list)
		request->file_priv = NULL;
	spin_unlock(&file_priv->mm.lock);

	if (!list_empty(&file_priv->rps.link)) {
		spin_lock(&to_i915(dev)->rps.client_lock);
		list_del(&file_priv->rps.link);
		spin_unlock(&to_i915(dev)->rps.client_lock);
	}
}

int i915_gem_open(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv;
	int ret;

	DRM_DEBUG_DRIVER("\n");

	file_priv = kzalloc(sizeof(*file_priv), GFP_KERNEL);
	if (!file_priv)
		return -ENOMEM;

	file->driver_priv = file_priv;
	file_priv->dev_priv = to_i915(dev);
	file_priv->file = file;
	INIT_LIST_HEAD(&file_priv->rps.link);

	spin_lock_init(&file_priv->mm.lock);
	INIT_LIST_HEAD(&file_priv->mm.request_list);

	file_priv->bsd_engine = -1;

	ret = i915_gem_context_open(dev, file);
	if (ret)
		kfree(file_priv);

	return ret;
}

/**
 * i915_gem_track_fb - update frontbuffer tracking
 * @old: current GEM buffer for the frontbuffer slots
 * @new: new GEM buffer for the frontbuffer slots
 * @frontbuffer_bits: bitmask of frontbuffer slots
 *
 * This updates the frontbuffer tracking bits @frontbuffer_bits by clearing them
 * from @old and setting them in @new. Both @old and @new can be NULL.
 */
void i915_gem_track_fb(struct drm_i915_gem_object *old,
		       struct drm_i915_gem_object *new,
		       unsigned frontbuffer_bits)
{
	if (old) {
		WARN_ON(!mutex_is_locked(&old->base.dev->struct_mutex));
		WARN_ON(!(old->frontbuffer_bits & frontbuffer_bits));
		old->frontbuffer_bits &= ~frontbuffer_bits;
	}

	if (new) {
		WARN_ON(!mutex_is_locked(&new->base.dev->struct_mutex));
		WARN_ON(new->frontbuffer_bits & frontbuffer_bits);
		new->frontbuffer_bits |= frontbuffer_bits;
	}
}

/* All the new VM stuff */
u64 i915_gem_obj_offset(struct drm_i915_gem_object *o,
			struct i915_address_space *vm)
{
	struct drm_i915_private *dev_priv = to_i915(o->base.dev);
	struct i915_vma *vma;

	WARN_ON(vm == &dev_priv->mm.aliasing_ppgtt->base);

	list_for_each_entry(vma, &o->vma_list, obj_link) {
		if (vma->is_ggtt &&
		    vma->ggtt_view.type != I915_GGTT_VIEW_NORMAL)
			continue;
		if (vma->vm == vm)
			return vma->node.start;
	}

	WARN(1, "%s vma for this object not found.\n",
	     i915_is_ggtt(vm) ? "global" : "ppgtt");
	return -1;
}

u64 i915_gem_obj_ggtt_offset_view(struct drm_i915_gem_object *o,
				  const struct i915_ggtt_view *view)
{
	struct i915_vma *vma;

	list_for_each_entry(vma, &o->vma_list, obj_link)
		if (vma->is_ggtt && i915_ggtt_view_equal(&vma->ggtt_view, view))
			return vma->node.start;

	WARN(1, "global vma for this object not found. (view=%u)\n", view->type);
	return -1;
}

bool i915_gem_obj_bound(struct drm_i915_gem_object *o,
			struct i915_address_space *vm)
{
	struct i915_vma *vma;

	list_for_each_entry(vma, &o->vma_list, obj_link) {
		if (vma->is_ggtt &&
		    vma->ggtt_view.type != I915_GGTT_VIEW_NORMAL)
			continue;
		if (vma->vm == vm && drm_mm_node_allocated(&vma->node))
			return true;
	}

	return false;
}

bool i915_gem_obj_ggtt_bound_view(struct drm_i915_gem_object *o,
				  const struct i915_ggtt_view *view)
{
	struct i915_vma *vma;

	list_for_each_entry(vma, &o->vma_list, obj_link)
		if (vma->is_ggtt &&
		    i915_ggtt_view_equal(&vma->ggtt_view, view) &&
		    drm_mm_node_allocated(&vma->node))
			return true;

	return false;
}

unsigned long i915_gem_obj_ggtt_size(struct drm_i915_gem_object *o)
{
	struct i915_vma *vma;

	GEM_BUG_ON(list_empty(&o->vma_list));

	list_for_each_entry(vma, &o->vma_list, obj_link) {
		if (vma->is_ggtt &&
		    vma->ggtt_view.type == I915_GGTT_VIEW_NORMAL)
			return vma->node.size;
	}

	return 0;
}

bool i915_gem_obj_is_pinned(struct drm_i915_gem_object *obj)
{
	struct i915_vma *vma;
	list_for_each_entry(vma, &obj->vma_list, obj_link)
		if (vma->pin_count > 0)
			return true;

	return false;
}

/* Like i915_gem_object_get_page(), but mark the returned page dirty */
struct page *
i915_gem_object_get_dirty_page(struct drm_i915_gem_object *obj, int n)
{
	struct page *page;

	/* Only default objects have per-page dirty tracking */
	if (WARN_ON(!i915_gem_object_has_struct_page(obj)))
		return NULL;

	page = i915_gem_object_get_page(obj, n);
	set_page_dirty(page);
	return page;
}

/* Allocate a new GEM object and fill it with the supplied data */
struct drm_i915_gem_object *
i915_gem_object_create_from_data(struct drm_device *dev,
			         const void *data, size_t size)
{
	struct drm_i915_gem_object *obj;
	struct sg_table *sg;
	size_t bytes;
	int ret;

	obj = i915_gem_object_create(dev, round_up(size, PAGE_SIZE));
	if (IS_ERR(obj))
		return obj;

	ret = i915_gem_object_set_to_cpu_domain(obj, true);
	if (ret)
		goto fail;

	ret = i915_gem_object_get_pages(obj);
	if (ret)
		goto fail;

	i915_gem_object_pin_pages(obj);
	sg = obj->pages;
	bytes = sg_copy_from_buffer(sg->sgl, sg->nents, (void *)data, size);
	obj->dirty = 1;		/* Backing store is now out of date */
	i915_gem_object_unpin_pages(obj);

	if (WARN_ON(bytes != size)) {
		DRM_ERROR("Incomplete copy, wrote %zu of %zu", bytes, size);
		ret = -EFAULT;
		goto fail;
	}

	return obj;

fail:
	i915_gem_object_put(obj);
	return ERR_PTR(ret);
}
