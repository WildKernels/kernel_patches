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

#ifndef __EVDI_DRV_H__
#define __EVDI_DRV_H__

#include <linux/module.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/bitops.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/llist.h>
#include <linux/file.h>
#include <linux/types.h>
#include <linux/mempool.h>
#include <linux/uaccess.h>
#include <linux/jump_label.h>

#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_vblank.h>
#elif KERNEL_VERSION(4, 11, 0) <= LINUX_VERSION_CODE
#include <drm/drm_drv.h>
#include <drm/drmP.h>
#include <drm/drm_gem.h>
#else
#include <drm/drmP.h>
#endif

#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE
#include <drm/drm_probe_helper.h>
#endif

#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>

#if KERNEL_VERSION(4, 19, 0) <= LINUX_VERSION_CODE
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#define EVDI_HAVE_KMS_HELPER 1
#else
#undef EVDI_HAVE_KMS_HELPER
#endif

#include <drm/drm_simple_kms_helper.h>

#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE
#include <linux/xarray.h>
#define EVDI_HAVE_XARRAY 1
#else
#include <linux/idr.h>
#undef EVDI_HAVE_XARRAY
#endif

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
#include <drm/drm_managed.h>
#define EVDI_HAVE_DRM_MANAGED 1
#else
#undef EVDI_HAVE_DRM_MANAGED
#endif

#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_DRM_EVENT_RESERVE 1
#else
#undef EVDI_HAVE_DRM_EVENT_RESERVE
#endif

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_ATOMIC_CMPXCHG_RELAXED 1
#endif

#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_XA_ALLOC_CYCLIC 1
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
#ifndef drm_gem_object_put_unlocked
#define drm_gem_object_put_unlocked drm_gem_object_unreference_unlocked
#endif
#ifndef drm_dev_put
#define drm_dev_put drm_dev_unref
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#ifndef evdi_access_ok_read
#define evdi_access_ok_read(uaddr, size)  access_ok(VERIFY_READ,  (uaddr), (size))
#define evdi_access_ok_write(uaddr, size) access_ok(VERIFY_WRITE, (uaddr), (size))
#endif
#else
#ifndef evdi_access_ok_read
#define evdi_access_ok_read(uaddr, size)  access_ok((uaddr), (size))
#define evdi_access_ok_write(uaddr, size) access_ok((uaddr), (size))
#endif
#endif

#include "uapi/evdi_drm.h"

#define DRIVER_NAME "evdi-lindroid"
#define DRIVER_DESC "Lindroid Virtual Display Interface"
#define DRIVER_DATE   "NEVER"
#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

#define EVDI_WAIT_TIMEOUT	msecs_to_jiffies(5000)

#define EVDI_MAX_FDS   32
#define EVDI_MAX_INTS  128
#define EVDI_GRALLOC_POOL_MIN 32
#define EVDI_INFLIGHT_POOL_MIN 64
#define EVDI_GRALLOC_DATA_POOL_MIN 32

#define EVDI_EVENT_PAYLOAD_MAX 32

#define EVDI__CONCAT2(a, b)			a##b
#define EVDI__CONCAT(a, b)			EVDI__CONCAT2(a, b)
#define EVDI_BUILD_BUG_ON(cond)						\
	typedef char EVDI__CONCAT(evdi_build_bug_on_, __LINE__)[(cond) ? -1 : 1] __maybe_unused

#define LINDROID_MAX_CONNECTORS 5

struct evdi_device;

struct evdi_gralloc_buf_user {
	int version;
	int numFds;
	int numInts;
	int data[EVDI_MAX_FDS + EVDI_MAX_INTS];
};

/* Must be +1 poll event types */
#define EVDI_EVENT_TYPE_MAX 6

struct evdi_event_pool {
	struct kmem_cache *cache;
	struct kmem_cache *drm_cache;
	struct kmem_cache *inflight_cache;
	struct kmem_cache *type_cache[EVDI_EVENT_TYPE_MAX];
	mempool_t *inflight_pool;
	mempool_t *gralloc_data_pool;
};

struct evdi_event {
	enum poll_event_type type;
	int poll_id;
	u32 payload_size;
	u8 payload[EVDI_EVENT_PAYLOAD_MAX];
	struct evdi_event *next;
	struct llist_node llist;
	struct drm_file *owner;
	struct evdi_device *evdi;
	atomic_t freed;
	u8 cache_idx;
	bool from_pool;
};

struct evdi_inflight_req {
	int type;
	struct drm_file *owner;
	struct completion done;
	struct kref refcount;
	atomic_t from_percpu;
	atomic_t freed;
	u8 percpu_slot;
	union {
		struct {
			int id;
			u32 stride;
		} create;
		struct {
			int status;
			struct {
				int version;
				int numFds;
				int numInts;
				struct evdi_gralloc_data *gralloc;
			} gralloc_buf;
		} get_buf;
	} reply;
};

struct evdi_gralloc_data {
	int version;
	int numFds;
	int numInts;
	struct file *data_files[EVDI_MAX_FDS];
	int data_ints[EVDI_MAX_INTS];
};

struct evdi_gem_object {
	struct drm_gem_object base;
	struct page **pages;
	atomic_t pages_pin_count;
	struct mutex pages_lock;
	void *vmapping;
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE
	bool vmap_is_iomem;
#endif
	bool vmap_is_vmram;
	struct sg_table *sg;
};

#define to_evdi_bo(x) container_of(x, struct evdi_gem_object, base)

struct evdi_swap {
	int id;
	int display_id;
};

struct evdi_swap_mailbox {
	atomic64_t	seq;
	atomic64_t	payload; /* (u32)id << 32 | (u32)display_id */
	atomic_t	poll_id;
	struct drm_file	*owner;
};

static __always_inline u64 evdi_swap_pack(int id, int display_id)
{
	return ((u64)(u32)id << 32) | (u64)(u32)display_id;
}

/*
 * If any payload from future UAPI changes grows beyond the current 32 bytes,
 * Just double EVDI_EVENT_PAYLOAD_MAX to 64 bytes.
 */
EVDI_BUILD_BUG_ON(sizeof(struct drm_evdi_gbm_create_buff) > EVDI_EVENT_PAYLOAD_MAX);
EVDI_BUILD_BUG_ON(sizeof(struct drm_evdi_gbm_get_buff) > EVDI_EVENT_PAYLOAD_MAX);
EVDI_BUILD_BUG_ON(sizeof(struct evdi_swap) > EVDI_EVENT_PAYLOAD_MAX);
EVDI_BUILD_BUG_ON(sizeof(int) > EVDI_EVENT_PAYLOAD_MAX);

struct evdi_display {
	bool connected;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_rate;
	uint32_t generation;
};

struct evdi_file_priv {
	struct mutex lock;
#ifdef EVDI_HAVE_XARRAY
	struct xarray bufid_to_handle;
	struct xarray handle_to_bufid;
	u32 next_handle;
#else
	struct idr bufid_to_handle;
	struct idr handle_to_bufid;
	u32 next_handle;
#endif
	u64 last_swap_seq[LINDROID_MAX_CONNECTORS];
	u8 swap_rr;
	unsigned long pending_swaps;
};

struct evdi_device {
	struct drm_device *ddev;
	struct drm_connector *connector[LINDROID_MAX_CONNECTORS];
	struct drm_simple_display_pipe pipe[LINDROID_MAX_CONNECTORS];

	int dev_index;

	struct evdi_display displays[LINDROID_MAX_CONNECTORS];

	struct drm_file *drm_client;

	struct {
		struct rcu_head rcu;
		struct evdi_device *evdi;
	} cleanup_rcu;

	struct {
		spinlock_t lock;
		atomic_t cleanup_in_progress;
		struct evdi_event * volatile head;
		struct evdi_event * volatile tail;
		struct llist_head lockfree_head;
		wait_queue_head_t wait_queue;
		struct evdi_event_pool pool;
		atomic_t wake_pending;
		atomic_t queue_size;
		atomic_t mailbox_wake_pending;
		atomic_t next_poll_id;
		atomic_t stopping;
		atomic64_t events_queued;
		atomic64_t events_dequeued;
	} events;

	struct evdi_swap_mailbox swap_mailbox[LINDROID_MAX_CONNECTORS];

	struct mutex config_mutex;

	struct platform_device *pdev;

#ifdef EVDI_HAVE_XARRAY
	struct xarray file_xa;
	struct xarray inflight_xa;
	u32 inflight_next_id;
#else
	struct idr file_idr;
	spinlock_t file_lock;
	struct idr inflight_idr;
	spinlock_t inflight_lock;
#endif
	struct evdi_percpu_inflight __percpu	*percpu_inflight;
};

struct evdi_percpu_inflight {
	struct evdi_inflight_req	req[2];
	atomic_t			in_use[2];
};

struct evdi_inflight_req;
void evdi_inflight_req_get(struct evdi_inflight_req *req);
void evdi_inflight_req_put(struct evdi_inflight_req *req);

extern struct evdi_event_pool global_event_pool;
extern atomic_t evdi_device_count;

static inline void evdi_gem_object_put(struct drm_gem_object *obj)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	drm_gem_object_put(obj);
#else
	drm_gem_object_put_unlocked(obj);
#endif
}

/* evdi_lindroid_drv.c */
int evdi_device_init(struct evdi_device *evdi, struct platform_device *pdev);
void evdi_device_cleanup(struct evdi_device *evdi);

/* evdi_modeset.c */
int evdi_modeset_init(struct drm_device *dev);
void evdi_modeset_cleanup(struct drm_device *dev);

/* evdi_connector.c */
int evdi_connector_init(struct drm_device *dev, struct evdi_device *evdi);
void evdi_connector_cleanup(struct evdi_device *evdi);

/* evdi_ioctl.c */
int evdi_ioctl_connect(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_poll(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_get_buff_callback(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_destroy_buff_callback(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_create_buff_callback(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_gbm_create_buff(struct drm_device *dev, void *data, struct drm_file *file);
void evdi_inflight_discard_owner(struct evdi_device *evdi, struct drm_file *owner);
int evdi_ioctl_request_update(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_gbm_get_buff(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_ioctl_gbm_del_buff(struct drm_device *dev, void *data, struct drm_file *file);
int evdi_queue_swap_event(struct evdi_device *evdi, int id, int display_id, u32 generation,
			struct drm_file *owner);
int evdi_queue_destroy_event(struct evdi_device *evdi, int id, struct drm_file *owner);
int evdi_ioctl_vsync(struct drm_device *dev, void *data, struct drm_file *file);

/* evdi_event.c */
int evdi_event_init(struct evdi_device *evdi);
void evdi_event_cleanup(struct evdi_device *evdi);
struct evdi_event *evdi_event_alloc(struct evdi_device *evdi,
				   enum poll_event_type type,
				   int poll_id,
				   void *data,
				   size_t data_size,
				   struct drm_file *owner);
void evdi_event_free(struct evdi_event *event);
void evdi_event_queue(struct evdi_device *evdi, struct evdi_event *event);
struct evdi_event *evdi_event_dequeue(struct evdi_device *evdi);
void evdi_event_cleanup_file(struct evdi_device *evdi, struct drm_file *file);
int evdi_event_wait(struct evdi_device *evdi, struct drm_file *file);
struct evdi_inflight_req;
struct evdi_inflight_req *evdi_inflight_req_alloc(struct evdi_device *evdi);
void *evdi_small_payload_alloc(gfp_t gfp);
void evdi_small_payload_free(void *ptr);
void evdi_swap_mailbox_invalidate_display(struct evdi_device *evdi, int display_id);

/* evdi_gem.c */
struct evdi_gem_object *evdi_gem_alloc_object(struct drm_device *dev, size_t size);
int evdi_gem_create(struct drm_file *file, struct drm_device *dev, uint64_t size, uint32_t *handle_p);
int evdi_dumb_create(struct drm_file *file, struct drm_device *dev, struct drm_mode_create_dumb *args);
int evdi_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma);
void evdi_gem_free_object(struct drm_gem_object *gem_obj);
int evdi_gem_cache_init(void);
void evdi_gem_cache_cleanup(void);
uint32_t evdi_gem_object_handle_lookup(struct drm_file *filp, struct drm_gem_object *obj);
struct sg_table *evdi_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *evdi_gem_prime_import(struct drm_device *dev,
					     struct dma_buf *dma_buf);
int evdi_prime_handle_to_fd(struct drm_device *dev,
    struct drm_file *file_priv,
    uint32_t handle,
    uint32_t flags,
    int *prime_fd);
int evdi_prime_fd_to_handle(struct drm_device *dev,
    struct drm_file *file_priv,
    int prime_fd,
    uint32_t *handle);

#if KERNEL_VERSION(4, 17, 0) <= LINUX_VERSION_CODE
vm_fault_t evdi_gem_fault(struct vm_fault *vmf);
#else
int evdi_gem_fault(struct vm_fault *vmf);
#endif

/* evdi_sysfs.c */
int evdi_sysfs_init(void);
void evdi_sysfs_cleanup(void);

/* evdi_fb.c */
struct drm_framebuffer *evdi_fb_user_fb_create(
					struct drm_device *dev,
					struct drm_file *file,
					const struct drm_mode_fb_cmd2 *mode_cmd);
int evdi_fb_cache_init(void);
void evdi_fb_cache_cleanup(void);

#define to_evdi_fb(x) container_of(x, struct evdi_framebuffer, base)

struct evdi_framebuffer {
	struct drm_framebuffer base;
	struct evdi_gem_object *obj;
	bool active;
	int gralloc_buf_id;
	struct drm_file *owner;
	int bound_display_id;
	u32 bound_generation;
};

/* evdi_connector.c */
int evdi_connector_slot(const struct evdi_device *evdi,
			const struct drm_connector *conn);

/* Helpers */
static __always_inline bool evdi_likely_connected(struct evdi_device *evdi, int id)
{
	return likely(id >= 0 && id < LINDROID_MAX_CONNECTORS &&
		      READ_ONCE(evdi->displays[id].connected));
}

static __always_inline bool evdi_likely_not_stopping(struct evdi_device *evdi)
{
	return likely(!atomic_read(&evdi->events.stopping));
}

static __always_inline bool evdi_swap_file_pending(struct drm_file *file)
{
	struct evdi_file_priv *priv;

	priv = file ? file->driver_priv : NULL;
	if (unlikely(!priv))
		return false;
	return READ_ONCE(priv->pending_swaps) != 0;
}

/* Memory barriers */
static __always_inline void evdi_smp_wmb(void)
{
	smp_wmb();
}

static __always_inline void evdi_smp_rmb(void)
{
	smp_rmb();
}

static __always_inline void evdi_smp_mb(void)
{
	smp_mb();
}

/* Macros */
#if KERNEL_VERSION(4, 11, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_DRM_OPEN_CLOSE 1
#else
#define EVDI_HAVE_DRM_OPEN_CLOSE 0
#endif

#if KERNEL_VERSION(4, 19, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_ATOMIC_HELPERS 1
#else
#define EVDI_HAVE_ATOMIC_HELPERS 0
#endif

#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_CONNECTOR_INIT_WITH_DDC 1
#else
#define EVDI_HAVE_CONNECTOR_INIT_WITH_DDC 0
#endif

#if KERNEL_VERSION(4, 16, 0) <= LINUX_VERSION_CODE
#define EVDI_HAVE_KMEM_USERCOPY 1
#else
#define EVDI_HAVE_KMEM_USERCOPY 0
#endif

#define EVDI_MAX_INFLIGHT_REQUESTS 1000

/* Debug and statistics */
#ifdef DEBUG
#define evdi_debug(fmt, ...) \
	pr_debug("[evdi-lindroid] " fmt "\n", ##__VA_ARGS__)
#else
#define evdi_debug(fmt, ...) do { } while (0)
#endif

#define evdi_info(fmt, ...) \
	pr_info("[evdi-lindroid] " fmt "\n", ##__VA_ARGS__)

#define evdi_warn(fmt, ...) \
	pr_warn("[evdi-lindroid] " fmt "\n", ##__VA_ARGS__)

#define evdi_err(fmt, ...) \
	pr_err("[evdi-lindroid] " fmt "\n", ##__VA_ARGS__)

/* Performance counters for monitoring */
DECLARE_STATIC_KEY_FALSE(evdi_perf_key);
extern bool evdi_perf_on;
#define EVDI_PERF_ENABLED() static_branch_unlikely(&evdi_perf_key)
#define EVDI_PERF_INC64(p)	do { if (EVDI_PERF_ENABLED()) atomic64_inc((p)); } while (0)
#define EVDI_PERF_ADD64(p,v)	do { if (EVDI_PERF_ENABLED()) atomic64_add((v),(p)); } while (0)

struct evdi_perf_counters {
	atomic64_t ioctl_calls[16];
	atomic64_t event_queue_ops;
	atomic64_t event_dequeue_ops;
	atomic64_t allocs;
	atomic64_t swap_updates;
	atomic64_t swap_delivered;
	atomic64_t wakeup_count;
	atomic64_t poll_cycles;
	atomic64_t inflight_percpu_hits;
	atomic64_t inflight_percpu_misses;
};

extern struct evdi_perf_counters evdi_perf;

/* Queue wakeup helpers */
static __always_inline bool evdi_events_inc_and_test_first(struct evdi_device *evdi)
{
	int qsz;

	if (unlikely(!evdi))
		return false;

	qsz = atomic_inc_return(&evdi->events.queue_size);
	return likely(qsz == 1);
}

static __always_inline bool evdi_events_dec_and_test_empty(struct evdi_device *evdi)
{
	int qsz;

	if (unlikely(!evdi))
		return false;

	qsz = atomic_dec_return(&evdi->events.queue_size);
	return likely(qsz == 0);
}

static __always_inline void evdi_wakeup_pollers(struct evdi_device *evdi)
{
	if (unlikely(!evdi))
		return;

	if (unlikely(atomic_read(&evdi->events.queue_size) <= 0))
		return;

	if (unlikely(!waitqueue_active(&evdi->events.wait_queue)))
		return;

#ifdef EVDI_HAVE_ATOMIC_CMPXCHG_RELAXED
	if (atomic_cmpxchg_relaxed(&evdi->events.wake_pending, 0, 1) != 0)
		return;
#else
	if (atomic_cmpxchg(&evdi->events.wake_pending, 0, 1) != 0)
		return;
#endif
	evdi_smp_wmb();
	if (likely(waitqueue_active(&evdi->events.wait_queue)))
		wake_up_interruptible(&evdi->events.wait_queue);
	EVDI_PERF_INC64(&evdi_perf.wakeup_count);
}

static __always_inline void evdi_wakeup_mailbox_pollers(struct evdi_device *evdi)
{
	if (unlikely(!evdi))
		return;

	if (unlikely(!waitqueue_active(&evdi->events.wait_queue)))
		return;

#ifdef EVDI_HAVE_ATOMIC_CMPXCHG_RELAXED
	if (atomic_cmpxchg_relaxed(&evdi->events.mailbox_wake_pending, 0, 1) != 0)
		return;
#else
	if (atomic_cmpxchg(&evdi->events.mailbox_wake_pending, 0, 1) != 0)
		return;
#endif

	evdi_smp_wmb();
	if (likely(waitqueue_active(&evdi->events.wait_queue)))
		wake_up_interruptible(&evdi->events.wait_queue);
	EVDI_PERF_INC64(&evdi_perf.wakeup_count);
}

/* External vm_ops */
extern const struct vm_operations_struct evdi_gem_vm_ops;

#endif /* __EVDI_DRV_H__ */
