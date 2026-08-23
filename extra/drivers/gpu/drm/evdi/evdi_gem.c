// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/shmem_fs.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <drm/drm_cache.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/mm.h>
#if KERNEL_VERSION(5, 18, 0) <= LINUX_VERSION_CODE
#include <linux/iosys-map.h>
#endif

#include <drm/drm_gem.h>
#include <drm/drm_prime.h>

#if defined(MODULE_IMPORT_NS) && defined(DMA_BUF)
MODULE_IMPORT_NS(DMA_BUF);
#endif

#include "evdi_drv.h"

static struct kmem_cache *evdi_gem_cache;

static int evdi_pin_pages(struct evdi_gem_object *obj);
static void evdi_unpin_pages(struct evdi_gem_object *obj);

int evdi_gem_cache_init(void)
{
	evdi_gem_cache = kmem_cache_create("evdi_gem_object",
					   sizeof(struct evdi_gem_object),
					   0, SLAB_HWCACHE_ALIGN, NULL);
	if (!evdi_gem_cache)
		return -ENOMEM;

	return 0;
}

void evdi_gem_cache_cleanup(void)
{
	if (evdi_gem_cache) {
		kmem_cache_destroy(evdi_gem_cache);
		evdi_gem_cache = NULL;
	}
}

static struct evdi_gem_object *evdi_gem_cache_alloc(gfp_t gfp)
{
	if (unlikely(!evdi_gem_cache))
		return NULL;
	return kmem_cache_zalloc(evdi_gem_cache, gfp);
}

static void evdi_gem_vm_open(struct vm_area_struct *vma)
{
	struct evdi_gem_object *obj = to_evdi_bo(vma->vm_private_data);
	drm_gem_vm_open(vma);
	evdi_pin_pages(obj);
}

static void evdi_gem_vm_close(struct vm_area_struct *vma)
{
	struct evdi_gem_object *obj = to_evdi_bo(vma->vm_private_data);
	evdi_unpin_pages(obj);
	drm_gem_vm_close(vma);
}

const struct vm_operations_struct evdi_gem_vm_ops = {
	.fault = evdi_gem_fault,
	.open = evdi_gem_vm_open,
	.close = evdi_gem_vm_close,
};

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
static int evdi_prime_pin(struct drm_gem_object *obj)
{
	struct evdi_gem_object *bo = to_evdi_bo(obj);
	return evdi_pin_pages(bo);
}

static void evdi_prime_unpin(struct drm_gem_object *obj)
{
	struct evdi_gem_object *bo = to_evdi_bo(obj);
	evdi_unpin_pages(bo);
}

static const struct drm_gem_object_funcs gem_obj_funcs = {
	.free = evdi_gem_free_object,
	.pin = evdi_prime_pin,
	.unpin = evdi_prime_unpin,
	.vm_ops = &evdi_gem_vm_ops,
	.export = drm_gem_prime_export,
	.get_sg_table = evdi_prime_get_sg_table,
};
#endif

static bool evdi_drm_gem_object_use_import_attach(struct drm_gem_object *obj)
{
	if (!obj || !obj->import_attach || !obj->import_attach->dmabuf)
		return false;

	return true;
}

uint32_t evdi_gem_object_handle_lookup(struct drm_file *filp, struct drm_gem_object *obj)
{
	uint32_t it_handle = 0;
	struct drm_gem_object *it_obj = NULL;

	spin_lock(&filp->table_lock);
	idr_for_each_entry(&filp->object_idr, it_obj, it_handle) {
		if (it_obj == obj)
			break;
	}
	spin_unlock(&filp->table_lock);

	if (!it_obj)
		it_handle = 0;

	return it_handle;
}

struct evdi_gem_object *evdi_gem_alloc_object(struct drm_device *dev, size_t size)
{
	struct evdi_gem_object *obj;

	if (unlikely(!size))
		return NULL;

	size = round_up(size, PAGE_SIZE);

	obj = evdi_gem_cache_alloc(GFP_KERNEL);
	if (obj == NULL)
		return NULL;

	if (drm_gem_object_init(dev, &obj->base, size) != 0) {
		kmem_cache_free(evdi_gem_cache, obj);
		return NULL;
	}

	atomic_set(&obj->pages_pin_count, 0);

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
	obj->base.funcs = &gem_obj_funcs;
#endif

	mutex_init(&obj->pages_lock);

	return obj;
}

int evdi_gem_create(struct drm_file *file, struct drm_device *dev,
		    uint64_t size, uint32_t *handle_p)
{
	struct evdi_gem_object *obj;
	int ret;
	u32 handle;

	size = round_up(size, PAGE_SIZE);

	obj = evdi_gem_alloc_object(dev, size);
	if (obj == NULL)
		return -ENOMEM;

	ret = drm_gem_handle_create(file, &obj->base, &handle);
	if (ret) {
		drm_gem_object_release(&obj->base);
		kmem_cache_free(evdi_gem_cache, obj);
		return ret;
	}

#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
	drm_gem_object_put(&obj->base);
#else
	drm_gem_object_put_unlocked(&obj->base);
#endif

	*handle_p = handle;
	return 0;
}

static int evdi_align_pitch(int width, int cpp)
{
	int aligned = width;
	int pitch_mask = 0;

	switch (cpp) {
	case 1:
		pitch_mask = 255;
		break;
	case 2:
		pitch_mask = 127;
		break;
	case 3:
	case 4:
		pitch_mask = 63;
		break;
	}

	aligned += pitch_mask;
	aligned &= ~pitch_mask;
	return aligned * cpp;
}

int evdi_dumb_create(struct drm_file *file, struct drm_device *dev,
		     struct drm_mode_create_dumb *args)
{
	args->pitch = evdi_align_pitch(args->width, DIV_ROUND_UP(args->bpp, 8));
	args->size = args->pitch * args->height;
	return evdi_gem_create(file, dev, args->size, &args->handle);
}

int evdi_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

#if KERNEL_VERSION(6, 1, 0) <= LINUX_VERSION_CODE
	vm_flags_mod(vma, VM_MIXEDMAP | VM_DONTDUMP | VM_DONTEXPAND | VM_DONTCOPY,
		     VM_PFNMAP);
#else
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP | VM_DONTDUMP | VM_DONTEXPAND | VM_DONTCOPY;
#endif

#if KERNEL_VERSION(5, 11, 0) > LINUX_VERSION_CODE
	vma->vm_ops = &evdi_gem_vm_ops;
#endif
#if defined(CONFIG_X86) || defined(CONFIG_ARM64)
	{
		struct drm_gem_object *gobj = vma->vm_private_data;
		if (gobj && !evdi_drm_gem_object_use_import_attach(gobj)) {
			vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		}
	}
#endif
	return ret;
}

#if KERNEL_VERSION(4, 17, 0) <= LINUX_VERSION_CODE
vm_fault_t evdi_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#else
int evdi_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#endif
	struct evdi_gem_object *obj = to_evdi_bo(vma->vm_private_data);
	struct page *page;
	pgoff_t page_offset;
	loff_t num_pages;
	int ret = 0;

	page_offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	num_pages = obj->base.size >> PAGE_SHIFT;

	if (!obj->pages || page_offset >= num_pages)
		return VM_FAULT_SIGBUS;

	page = obj->pages[page_offset];

#if KERNEL_VERSION(4, 17, 0) <= LINUX_VERSION_CODE
	ret = vmf_insert_page(vma, vmf->address, page);
#else
	ret = vm_insert_page(vma, vmf->address, page);
#endif

	switch (ret) {
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
	case -EBUSY:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
}

static int evdi_gem_get_pages(struct evdi_gem_object *obj, gfp_t gfpmask)
{
	struct page **pages;

	if (obj->pages)
		return 0;

	pages = drm_gem_get_pages(&obj->base);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	obj->pages = pages;

#ifdef CONFIG_X86
	drm_clflush_pages(obj->pages, DIV_ROUND_UP(obj->base.size, PAGE_SIZE));
#endif

	return 0;
}

static void evdi_gem_put_pages(struct evdi_gem_object *obj)
{
	if (evdi_drm_gem_object_use_import_attach(&obj->base)) {
		obj->pages = NULL;
		return;
	}

	drm_gem_put_pages(&obj->base, obj->pages, false, true);
	obj->pages = NULL;
}

static int evdi_pin_pages(struct evdi_gem_object *obj)
{
	int ret = 0;

	if (unlikely(!obj))
		return -EINVAL;

	/* Fast path if pinned */
	if (likely(atomic_inc_not_zero(&obj->pages_pin_count))) {
		evdi_smp_rmb();
		return 0;
	}

	/* Slow path */
	mutex_lock(&obj->pages_lock);
	if (atomic_read(&obj->pages_pin_count) == 0) {
		ret = evdi_gem_get_pages(obj, GFP_KERNEL);
		if (!ret) {
			evdi_smp_wmb();
			atomic_set(&obj->pages_pin_count, 1);
		}
	} else {
		atomic_inc(&obj->pages_pin_count);
	}
	mutex_unlock(&obj->pages_lock);

	return ret;
}

static void evdi_unpin_pages(struct evdi_gem_object *obj)
{
	int new_cnt;

	if (unlikely(!obj))
		return;

	new_cnt = atomic_dec_return(&obj->pages_pin_count);
	if (unlikely(new_cnt == 0)) {
		mutex_lock(&obj->pages_lock);
		if (atomic_read(&obj->pages_pin_count) == 0)
			evdi_gem_put_pages(obj);
		mutex_unlock(&obj->pages_lock);
	}
}

struct drm_gem_object *evdi_gem_prime_import(struct drm_device *dev,
					     struct dma_buf *dma_buf)
{
	struct dma_buf_attachment *attach;
	struct evdi_gem_object *obj;

	attach = dma_buf_attach(dma_buf, dev->dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);

	obj = evdi_gem_alloc_object(dev, dma_buf->size);
	if (!obj) {
		dma_buf_detach(dma_buf, attach);
		dma_buf_put(dma_buf);
		return ERR_PTR(-ENOMEM);
	}

	obj->base.import_attach = attach;
	return &obj->base;
}

static int evdi_export_id_as_fd(int id, uint32_t flags, int *out_fd)
{
	struct file *f;
	int fd_flags = 0;
	int newfd;
	loff_t pos = 0;
	ssize_t wr;

	if (!out_fd)
		return -EINVAL;
	if (id <= 0)
		return -EINVAL;

	if (flags & DRM_CLOEXEC)
		fd_flags |= O_CLOEXEC;

	f = shmem_file_setup("evdi-bufid", sizeof(id), 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	newfd = get_unused_fd_flags(fd_flags);
	if (newfd < 0) {
		fput(f);
		return newfd;
	}

	wr = kernel_write(f, (const char *)&id, sizeof(id), &pos);
	if (wr != sizeof(id)) {
		put_unused_fd(newfd);
		fput(f);
		return (wr < 0) ? (int)wr : -EIO;
	}

	fd_install(newfd, f);
	*out_fd = newfd;
	return 0;
}

static int evdi_read_id_from_fd(int fd_u32, int *out_id)
{
	loff_t pos = 0;
	ssize_t rd;
	int id;
	struct file *memfd_file;
	int ret = 0;

	if (!out_id)
		return -EINVAL;
	if (fd_u32 <= 0 || fd_u32 > INT_MAX)
		return -EBADF;

	memfd_file = fget(fd_u32);
	if (!memfd_file)
		return -EBADF;

	rd = kernel_read(memfd_file, &id, sizeof(id), &pos);
	if (rd != sizeof(id))
		ret = -EINVAL;
	else
		*out_id = id;

	if (!ret)
		evdi_debug("Got handle id: %d from fd: %d\n", id, fd_u32);

	fput(memfd_file);
	return ret;
}

int evdi_prime_handle_to_fd(struct drm_device *dev,
			    struct drm_file *file_priv,
			    uint32_t handle,
			    uint32_t flags,
			    int *prime_fd)
{
	return evdi_export_id_as_fd((int)handle, flags, prime_fd);
}

int evdi_prime_fd_to_handle(struct drm_device *dev,
			    struct drm_file *file_priv,
			    int prime_fd,
			    uint32_t *handle)
{
	int id, ret;

	if (!handle)
		return -EINVAL;

	ret = evdi_read_id_from_fd(prime_fd, &id);
	if (ret)
		return ret;

	*handle = (uint32_t)id;
	return 0;
}

void evdi_gem_vunmap(struct evdi_gem_object *obj)
{
	if (evdi_drm_gem_object_use_import_attach(&obj->base)) {
#if KERNEL_VERSION(5, 18, 0) <= LINUX_VERSION_CODE
		{
			struct iosys_map map;
#ifdef IOSYS_MAP_IS_IOMEM
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
			if (obj->vmap_is_iomem)
				iosys_map_set_vaddr_iomem(&map, (void __iomem *)obj->vmapping);
			else
#endif
				iosys_map_set_vaddr(&map, obj->vmapping);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
			if (obj->vmap_is_iomem)
				iosys_map_set_vaddr_iomem(&map, (void __iomem *)obj->vmapping);
			else
#endif
				iosys_map_set_vaddr(&map, obj->vmapping);
#endif
			dma_buf_vunmap(obj->base.import_attach->dmabuf, &map);
		}
#else
		dma_buf_vunmap(obj->base.import_attach->dmabuf, obj->vmapping);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		obj->vmap_is_iomem = false;
#endif
		obj->vmapping = NULL;
		return;
	}

	if (obj->vmapping) {
		if (obj->vmap_is_vmram)
			vm_unmap_ram(obj->vmapping, DIV_ROUND_UP(obj->base.size, PAGE_SIZE));
		else
			vunmap(obj->vmapping);
		obj->vmapping = NULL;
		obj->vmap_is_vmram = false;
		evdi_unpin_pages(obj);
	}
}

void evdi_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct evdi_gem_object *obj = to_evdi_bo(gem_obj);

	if (obj->vmapping)
		evdi_gem_vunmap(obj);

	if (gem_obj->import_attach) {
		dma_buf_detach(gem_obj->import_attach->dmabuf,
			       gem_obj->import_attach);
		dma_buf_put(gem_obj->import_attach->dmabuf);
	}

	if (obj->pages)
		evdi_gem_put_pages(obj);

	if (gem_obj->dev->vma_offset_manager)
		drm_gem_free_mmap_offset(gem_obj);

	mutex_destroy(&obj->pages_lock);

	drm_gem_object_release(&obj->base);
	kmem_cache_free(evdi_gem_cache, obj);
}

struct sg_table *evdi_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct evdi_gem_object *bo = to_evdi_bo(obj);

	if (unlikely(!obj))
		return ERR_PTR(-EINVAL);

	if (unlikely(evdi_drm_gem_object_use_import_attach(obj)))
		return ERR_PTR(-EINVAL);

	if (unlikely(!bo->pages))
		return ERR_PTR(-EINVAL);

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
	return drm_prime_pages_to_sg(obj->dev, bo->pages,
				   bo->base.size >> PAGE_SHIFT);
#else
	return drm_prime_pages_to_sg(bo->pages, bo->base.size >> PAGE_SHIFT);
#endif
}
