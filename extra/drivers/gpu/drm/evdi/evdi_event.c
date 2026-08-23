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

#include "evdi_drv.h"
#include <linux/sched.h>
#include <linux/prefetch.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>

struct evdi_event_pool global_event_pool = {0};
static void evdi_inflight_req_release(struct kref *kref);
void evdi_event_free_immediate(struct evdi_event *event);

DEFINE_STATIC_KEY_FALSE(evdi_perf_key);
bool evdi_perf_on;
struct evdi_perf_counters evdi_perf;

static DEFINE_PER_CPU(int, evdi_inflight_last_slot);

static void *evdi_inflight_req_pool_alloc(gfp_t gfp_mask, void *pool_data)
{
	return kvzalloc(sizeof(struct evdi_inflight_req), gfp_mask);
}

static void evdi_inflight_req_pool_free(void *element, void *pool_data)
{
	kvfree(element);
}

static void *evdi_gralloc_data_alloc(gfp_t gfp_mask, void *pool_data)
{
	struct evdi_gralloc_data *gralloc;
	
	gralloc = kvzalloc(sizeof(struct evdi_gralloc_data), gfp_mask);

	return gralloc;
}

static void evdi_gralloc_data_free(void *element, void *pool_data)
{
	kvfree(element);
}

static void evdi_event_destroy_caches(void)
{
	int i;

	if (global_event_pool.cache) {
		kmem_cache_destroy(global_event_pool.cache);
		global_event_pool.cache = NULL;
	}

	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++) {
		if (global_event_pool.type_cache[i]) {
			kmem_cache_destroy(global_event_pool.type_cache[i]);
			global_event_pool.type_cache[i] = NULL;
		}
	}
}

int evdi_event_system_init(void)
{
	char name[32];
	int i;

#if EVDI_HAVE_KMEM_USERCOPY
	global_event_pool.cache = kmem_cache_create_usercopy("evdi_events",
						   sizeof(struct evdi_event),
						   0, SLAB_HWCACHE_ALIGN,
						   offsetof(struct evdi_event, payload),
						   sizeof(((struct evdi_event *)0)->payload),
						   NULL);
#else
	global_event_pool.cache = kmem_cache_create("evdi_events",
						   sizeof(struct evdi_event),
						   0, SLAB_HWCACHE_ALIGN,
						   NULL);
#endif
	if (!global_event_pool.cache)
		return -ENOMEM;

	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++) {
		snprintf(name, sizeof(name), "evdi_events_%d", i);
		global_event_pool.type_cache[i] =
#if EVDI_HAVE_KMEM_USERCOPY
			kmem_cache_create_usercopy(name,
						   sizeof(struct evdi_event),
						   0, SLAB_HWCACHE_ALIGN,
						   offsetof(struct evdi_event, payload),
						   sizeof(((struct evdi_event *)0)->payload),
						   NULL);
#else
			kmem_cache_create(name, sizeof(struct evdi_event),
					  0, SLAB_HWCACHE_ALIGN, NULL);
#endif
		if (!global_event_pool.type_cache[i]) {
			evdi_event_destroy_caches();
			return -ENOMEM;
		}
	}

	memset(&evdi_perf, 0, sizeof(evdi_perf));
	evdi_perf_on = false;
	evdi_smp_wmb();

	global_event_pool.inflight_pool = mempool_create(
		EVDI_INFLIGHT_POOL_MIN,
		evdi_inflight_req_pool_alloc,
		evdi_inflight_req_pool_free,
		NULL);
	if (!global_event_pool.inflight_pool)
		goto err;

	global_event_pool.gralloc_data_pool = mempool_create(
		EVDI_GRALLOC_DATA_POOL_MIN,
		evdi_gralloc_data_alloc,
		evdi_gralloc_data_free,
		NULL);
	if (!global_event_pool.gralloc_data_pool)
		goto err;

	evdi_info("Event system initialized");

	/* Pre-warm caches */
	{
		const int prealloc = 64;
		int i;
		void *tmp;
		for (i = 0; i < prealloc; i++) {
			tmp = kmem_cache_alloc(global_event_pool.cache, GFP_NOWAIT);
			if (!tmp)
				break;
			kmem_cache_free(global_event_pool.cache, tmp);
		}
	}

	return 0;

err:
	if (global_event_pool.gralloc_data_pool) {
		mempool_destroy(global_event_pool.gralloc_data_pool);
		global_event_pool.gralloc_data_pool = NULL;
	}
	if (global_event_pool.inflight_pool) {
		mempool_destroy(global_event_pool.inflight_pool);
		global_event_pool.inflight_pool = NULL;
	}
	evdi_event_destroy_caches();
	return -ENOMEM;
}

void evdi_event_system_cleanup(void)
{
	if (global_event_pool.gralloc_data_pool)
		mempool_destroy(global_event_pool.gralloc_data_pool);

	if (global_event_pool.inflight_pool)
		mempool_destroy(global_event_pool.inflight_pool);

	evdi_event_destroy_caches();

	evdi_debug("Event system cleaned up");
}

int evdi_event_init(struct evdi_device *evdi)
{
	int i;

	if (unlikely(!evdi))
		return -EINVAL;

	evdi->percpu_inflight = alloc_percpu(struct evdi_percpu_inflight);
	if (!evdi->percpu_inflight) {
		evdi_err("Failed to allocate per-CPU inflight buffers");
		return -ENOMEM;
	}

	spin_lock_init(&evdi->events.lock);
	init_waitqueue_head(&evdi->events.wait_queue);
	atomic_set(&evdi->events.cleanup_in_progress, 0);

	evdi->events.head = NULL;
	evdi->events.tail = NULL;
	atomic_set(&evdi->events.queue_size, 0);
	atomic_set(&evdi->events.mailbox_wake_pending, 0);
	atomic_set(&evdi->events.next_poll_id, 1);
	atomic_set(&evdi->events.stopping, 0);

	init_llist_head(&evdi->events.lockfree_head);

	atomic64_set(&evdi->events.events_queued, 0);
	atomic64_set(&evdi->events.events_dequeued, 0);
	atomic_set(&evdi->events.wake_pending, 0);

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		atomic64_set(&evdi->swap_mailbox[i].seq, 0);
		atomic64_set(&evdi->swap_mailbox[i].payload, 0);
		atomic_set(&evdi->swap_mailbox[i].poll_id, 0);
		WRITE_ONCE(evdi->swap_mailbox[i].owner, NULL);
	}

	evdi_smp_wmb();

	evdi_debug("Event system initialized for device %d", evdi->dev_index);
	return 0;
}

void evdi_swap_mailbox_invalidate_display(struct evdi_device *evdi, int display_id)
{
	struct drm_file *old_owner;
	struct evdi_file_priv *priv;
	struct evdi_swap_mailbox *mb;

	if (!evdi)
		return;
	if (display_id < 0 || display_id >= LINDROID_MAX_CONNECTORS)
		return;

	mb = &evdi->swap_mailbox[display_id];

	old_owner = READ_ONCE(mb->owner);

	/* Mark as unstable, clear owner/payload, then mark stable again */
	atomic64_inc(&mb->seq);
	WRITE_ONCE(mb->owner, NULL);
	atomic_set(&mb->poll_id, 0);
	atomic64_set(&mb->payload, 0);
	evdi_smp_wmb();
	atomic64_inc(&mb->seq);

	priv = old_owner ? old_owner->driver_priv : NULL;
	if (priv)
		clear_bit(display_id, &priv->pending_swaps);
}

void evdi_event_cleanup(struct evdi_device *evdi)
{
	struct evdi_event *event, *next;
	int d;

	if (unlikely(!evdi))
		return;

	atomic_set(&evdi->events.cleanup_in_progress, 1);
	atomic_set(&evdi->events.stopping, 1);

	evdi_smp_wmb();

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		evdi_swap_mailbox_invalidate_display(evdi, d);
	}

	if (evdi->percpu_inflight) {
		free_percpu(evdi->percpu_inflight);
		evdi->percpu_inflight = NULL;
	}

	wake_up_all(&evdi->events.wait_queue);

	spin_lock(&evdi->events.lock);
	event = READ_ONCE(evdi->events.head);
	WRITE_ONCE(evdi->events.head, NULL);
	WRITE_ONCE(evdi->events.tail, NULL);
	atomic_set(&evdi->events.queue_size, 0);
	atomic_set(&evdi->events.mailbox_wake_pending, 0);
	atomic_set(&evdi->events.wake_pending, 0);
	spin_unlock(&evdi->events.lock);

	while (event) {
		next = READ_ONCE(event->next);
		evdi_event_free(event);
		event = next;
	}

	atomic_set(&evdi->events.cleanup_in_progress, 0);

	evdi_debug("Event system cleaned up for device %d", evdi->dev_index);
}

struct evdi_event *evdi_event_alloc(struct evdi_device *evdi,
				   enum poll_event_type type,
				   int poll_id,
				   void *data,
				   size_t data_size,
				   struct drm_file *owner)
{
	struct evdi_event *event;
	struct kmem_cache *cache = NULL;
	u8 idx = 0xff;

	if (type >= 0 && type < EVDI_EVENT_TYPE_MAX)
		cache = global_event_pool.type_cache[type];

	if (!cache)
		cache = global_event_pool.cache;

	event = kmem_cache_alloc(cache, GFP_ATOMIC);
	if (unlikely(!event))
		return NULL;

	if (cache != global_event_pool.cache && type >= 0 &&
	    type < EVDI_EVENT_TYPE_MAX)
		idx = (u8)type;

	EVDI_PERF_INC64(&evdi_perf.allocs);
	event->from_pool = true;
	event->cache_idx = idx;

	event->type = type;
	event->poll_id = poll_id;
	event->payload_size = 0;
	if (data && data_size > 0) {
		size_t copy_size = (data_size > EVDI_EVENT_PAYLOAD_MAX) ?
			EVDI_EVENT_PAYLOAD_MAX : data_size;
		if (unlikely(copy_size > EVDI_EVENT_PAYLOAD_MAX)) {
			evdi_warn("Event payload truncated %zu->%d bytes",
				data_size, EVDI_EVENT_PAYLOAD_MAX);
		}
		memcpy(event->payload, data, copy_size);
		event->payload_size = copy_size;
	}
	event->next = NULL;
	event->owner = owner;
	event->evdi = evdi;
	atomic_set(&event->freed, 0);

	return event;
}

void evdi_inflight_req_get(struct evdi_inflight_req *req)
{
	if (unlikely(!req))
		return;

	kref_get(&req->refcount);
}

void evdi_inflight_req_put(struct evdi_inflight_req *req)
{
	if (unlikely(!req))
		return;

	kref_put(&req->refcount, evdi_inflight_req_release);
}

static void evdi_inflight_req_release(struct kref *kref)
{
	struct evdi_inflight_req *req =
		container_of(kref, struct evdi_inflight_req, refcount);
	struct evdi_percpu_inflight *percpu_req;
	struct evdi_gralloc_data *gralloc;
	int i, nfd, slot;

	if (atomic_xchg(&req->freed, 1))
		return;

	gralloc = req->reply.get_buf.gralloc_buf.gralloc;
	if (gralloc) {
		nfd = gralloc->numFds;
		if (nfd < 0)
			nfd = 0;
		else if (nfd > EVDI_MAX_FDS)
			nfd = EVDI_MAX_FDS;

		for (i = 0; i < nfd; i++) {
			if (gralloc->data_files[i]) {
				fput(gralloc->data_files[i]);
				gralloc->data_files[i] = NULL;
			}
		}
		mempool_free(gralloc, global_event_pool.gralloc_data_pool);
		req->reply.get_buf.gralloc_buf.gralloc = NULL;
	}
	if (atomic_read(&req->from_percpu)) {
		slot = (int)req->percpu_slot;
		if (slot >= 0 && slot < 2) {
			percpu_req = container_of(req, struct evdi_percpu_inflight, req[0]);
			atomic_set(&percpu_req->in_use[slot], 0);
			evdi_smp_wmb();
		}
	} else {
		mempool_free(req, global_event_pool.inflight_pool);
	}
}

static __always_inline void evdi_inflight_req_reinit_fast(struct evdi_inflight_req *req,
							  int slot)
{
	memset(&req->reply, 0, sizeof(req->reply));
	req->type = 0;
	req->owner = NULL;
	kref_init(&req->refcount);
	init_completion(&req->done);
	atomic_set(&req->from_percpu, 1);
	atomic_set(&req->freed, 0);
	req->percpu_slot = slot;
}

struct evdi_inflight_req *evdi_inflight_req_alloc(struct evdi_device *evdi)
{
	struct evdi_inflight_req *req = NULL;
	bool from_percpu = false;
	int sel_slot = -1;
	struct evdi_percpu_inflight *pc;
	int start, i;

	if (likely(evdi && evdi->percpu_inflight)) {
		pc = get_cpu_ptr(evdi->percpu_inflight);
		start = this_cpu_read(evdi_inflight_last_slot) & 1;

		prefetchw(&pc->req[0]);
		prefetchw(&pc->req[1]);
		for (i = 0; i < 2; i++) {
			int s = (start + i) & 1;
			if (atomic_cmpxchg(&pc->in_use[s], 0, 1) == 0) {
				this_cpu_write(evdi_inflight_last_slot, s);
				req = &pc->req[s];
				from_percpu = true;
				sel_slot = s;
				break;
			}
		}
		put_cpu_ptr(evdi->percpu_inflight);
	}

	if (unlikely(!req)) {
		req = mempool_alloc(global_event_pool.inflight_pool, GFP_ATOMIC);
		if (unlikely(!req))
			return NULL;
	}

	if (likely(from_percpu)) {
		evdi_inflight_req_reinit_fast(req, sel_slot);
		return req;
	}

	memset(req, 0, sizeof(*req));
	kref_init(&req->refcount);
	init_completion(&req->done);
	atomic_set(&req->from_percpu, 0);
	req->percpu_slot = -1;
	atomic_set(&req->freed, 0);

	return req;
}

void evdi_event_free_immediate(struct evdi_event *event)
{
	struct kmem_cache *cache = NULL;

	if (!event)
		return;

	if (likely(event->from_pool)) {
		if (event->cache_idx < EVDI_EVENT_TYPE_MAX)
			cache = global_event_pool.type_cache[event->cache_idx];

		if (!cache)
			cache = global_event_pool.cache;

		kmem_cache_free(cache, event);
	} else {
		kfree(event);
	}
}

void evdi_event_free(struct evdi_event *event)
{
	if (!event)
		return;

	if (atomic_xchg(&event->freed, 1))
		return;

	evdi_event_free_immediate(event);
}

static inline bool evdi_event_queue_lockfree(struct evdi_device *evdi, struct evdi_event *event)
{
	bool first;

	if (unlikely(atomic_read_acquire(&evdi->events.cleanup_in_progress)))
		return false;

	if (unlikely(atomic_read_acquire(&evdi->events.stopping)))
		return false;

	prefetchw(&event->llist);
	llist_add(&event->llist, &evdi->events.lockfree_head);

	first = evdi_events_inc_and_test_first(evdi);
	atomic64_inc(&evdi->events.events_queued);
	EVDI_PERF_INC64(&evdi_perf.event_queue_ops);
	
	evdi_smp_wmb();

	if (likely(first))
		evdi_wakeup_pollers(evdi);

	return true;
}

void evdi_event_queue(struct evdi_device *evdi, struct evdi_event *event)
{
	struct evdi_event *tail;
	bool first;

	if (unlikely(!evdi || !event))
		return;

	if (likely(evdi_event_queue_lockfree(evdi, event)))
		return;

	spin_lock(&evdi->events.lock);

	if (unlikely(atomic_read(&evdi->events.stopping))) {
		spin_unlock(&evdi->events.lock);
		evdi_event_free(event);
		return;
	}
	WRITE_ONCE(event->next, NULL);
	evdi_smp_wmb();
	tail = READ_ONCE(evdi->events.tail);
	if (tail)
		WRITE_ONCE(tail->next, event);
	else
		WRITE_ONCE(evdi->events.head, event);

	WRITE_ONCE(evdi->events.tail, event);
	spin_unlock(&evdi->events.lock);

	first = evdi_events_inc_and_test_first(evdi);
	atomic64_inc(&evdi->events.events_queued);
	EVDI_PERF_INC64(&evdi_perf.event_queue_ops);
	if (likely(first))
		evdi_wakeup_pollers(evdi);
}

static inline struct evdi_event *evdi_event_pop_head_locked(struct evdi_device *evdi)
{
	struct evdi_event *e = evdi->events.head;
	if (e) {
		WRITE_ONCE(evdi->events.head, e->next);
		if (!evdi->events.head)
			WRITE_ONCE(evdi->events.tail, NULL);
	}
	return e;
}

static inline void evdi_event_append_list_locked(struct evdi_device *evdi,
						 struct evdi_event *first,
						 struct evdi_event *last)
{
	if (!first)
		return;

	if (!READ_ONCE(evdi->events.head)) {
		WRITE_ONCE(evdi->events.head, first);
		WRITE_ONCE(evdi->events.tail, last);
	} else {
		WRITE_ONCE(evdi->events.tail->next, first);
		WRITE_ONCE(evdi->events.tail, last);
	}
}

static inline struct evdi_event *evdi_event_take_from_lockfree(struct evdi_device *evdi)
{
	struct llist_node *lst, *node;
	struct evdi_event *e;
	struct evdi_event *event = NULL;
	struct evdi_event *first = NULL, *last = NULL;

	lst = llist_del_all(&evdi->events.lockfree_head);
	if (!lst)
		return NULL;

	// Single event fast path
	if (unlikely(!lst->next)) {
		e = llist_entry(lst, struct evdi_event, llist);
		e->next = NULL;
		return e;
	}

	lst = llist_reverse_order(lst);
	for (node = lst; node; node = node->next) {
		struct evdi_event *e = llist_entry(node, struct evdi_event, llist);
		e->next = NULL;
		if (!event) {
			event = e;
			continue;
		}

		if (!first) {
			first = e;
			last = e;
		} else {
			last->next = e;
			last = e;
		}
	}

	if (!event)
		return NULL;

	if (first) {
		spin_lock(&evdi->events.lock);
		evdi_event_append_list_locked(evdi, first, last);
		spin_unlock(&evdi->events.lock);
	}
	return event;
}

struct evdi_event *evdi_event_dequeue(struct evdi_device *evdi)
{
	struct evdi_event *event = NULL;
	bool empty;

 	if (unlikely(!evdi))
 		return NULL;

	if (unlikely(atomic_read_acquire(&evdi->events.cleanup_in_progress))) {
		spin_lock(&evdi->events.lock);
		event = evdi_event_pop_head_locked(evdi);
		spin_unlock(&evdi->events.lock);
		if (!event)
			return NULL;
		goto found_one;
	}

	if (READ_ONCE(evdi->events.head)) {
		spin_lock(&evdi->events.lock);
		event = evdi_event_pop_head_locked(evdi);
		spin_unlock(&evdi->events.lock);
		if (event)
			goto found_one;
	}

	event = evdi_event_take_from_lockfree(evdi);
	if (event)
		goto found_one;
	spin_lock(&evdi->events.lock);
	event = evdi_event_pop_head_locked(evdi);
	spin_unlock(&evdi->events.lock);
	if (!event)
		return NULL;

found_one:
	prefetch(&event->payload[0]);
	empty = evdi_events_dec_and_test_empty(evdi);
	atomic64_inc(&evdi->events.events_dequeued);
	EVDI_PERF_INC64(&evdi_perf.event_dequeue_ops);
	if (likely(empty))
		atomic_set(&evdi->events.wake_pending, 0);
	evdi_smp_wmb();
	return event;
}


void evdi_event_cleanup_file(struct evdi_device *evdi, struct drm_file *file)
{
	struct evdi_event *event, *next;
	struct evdi_event *new_head = NULL, *new_tail = NULL;
	struct evdi_event **restore_events = NULL;
	struct llist_node *llnode, *next_node = NULL;
	struct evdi_file_priv *priv;
	int lf_removed = 0;
	int sp_removed = 0;
	int restore_count = 0;
	int restore_capacity = 0;
	int i, d, queue_estimate;

	if (unlikely(!evdi || !file))
		return;

	priv = file->driver_priv;
	if (priv) {
		WRITE_ONCE(priv->pending_swaps, 0);
		memset(priv->last_swap_seq, 0, sizeof(priv->last_swap_seq));
		priv->swap_rr = 0;
	}

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		if (READ_ONCE(evdi->swap_mailbox[d].owner) != file)
			continue;
		evdi_swap_mailbox_invalidate_display(evdi, d);
	}

	if (atomic_read(&evdi->events.queue_size) == 0 &&
	    llist_empty(&evdi->events.lockfree_head))
		return;

	atomic_set(&evdi->events.cleanup_in_progress, 1);

	queue_estimate = atomic_read(&evdi->events.queue_size);

	if (queue_estimate > 0) {
		restore_capacity = queue_estimate + 64;
		restore_events = kmalloc_array(restore_capacity,
					sizeof(struct evdi_event *), GFP_KERNEL);
	}

	llnode = llist_del_all(&evdi->events.lockfree_head);

	while (llnode) {
		event = llist_entry(llnode, struct evdi_event, llist);
		next_node = llnode->next;

		if (event->owner == file) {
			lf_removed++;
			atomic_dec(&evdi->events.queue_size);
			evdi_event_free(event);
		} else if (restore_events && restore_count < restore_capacity) {
			restore_events[restore_count++] = event;
		}
		llnode = next_node;
	}
	if (restore_events) {
		for (i = 0; i < restore_count; i++) {
			llist_add(&restore_events[i]->llist, &evdi->events.lockfree_head);
		}
		kfree(restore_events);
	}
	evdi_smp_wmb();

	spin_lock(&evdi->events.lock);

	event = READ_ONCE(evdi->events.head);
	while (event) {
		next = READ_ONCE(event->next);
		if (event->owner == file) {
			sp_removed++;
			evdi_event_free(event);
		} else {
			WRITE_ONCE(event->next, NULL);
			if (!new_head) {
				new_head = event;
				new_tail = event;
			} else {
				WRITE_ONCE(new_tail->next, event);
				new_tail = event;
			}
		}
		event = next;
	}
	
	WRITE_ONCE(evdi->events.head, new_head);
	WRITE_ONCE(evdi->events.tail, new_tail);
	if (sp_removed)
		atomic_sub(sp_removed, &evdi->events.queue_size);

	spin_unlock(&evdi->events.lock);

	atomic_set(&evdi->events.cleanup_in_progress, 0);
	evdi_smp_wmb();
	if (atomic_read(&evdi->events.queue_size) == 0)
		atomic_set(&evdi->events.wake_pending, 0);

	wake_up_interruptible(&evdi->events.wait_queue);
	
	if (lf_removed || sp_removed)
		evdi_debug("Cleaned up %d events for closed file (lf:%d sp:%d)",
			   lf_removed + sp_removed, lf_removed, sp_removed);
}

static __always_inline int evdi_event_wait_ready(struct evdi_device *evdi,
						 struct drm_file *file)
{
	if (atomic_read(&evdi->events.queue_size) > 0)
		return 0;

	if (evdi_swap_file_pending(file))
		return 0;

	if (atomic_read(&evdi->events.stopping))
		return -ENODEV;

	if (signal_pending(current))
		return -ERESTARTSYS;

	return 1;
}

int evdi_event_wait(struct evdi_device *evdi, struct drm_file *file)
{
	DEFINE_WAIT(wait);
	int ret = 0;

	EVDI_PERF_INC64(&evdi_perf.poll_cycles);

	for (;;) {
		ret = evdi_event_wait_ready(evdi, file);
		if (ret != 1)
			break;
		prepare_to_wait(&evdi->events.wait_queue, &wait, TASK_INTERRUPTIBLE);

		if (atomic_read_acquire(&evdi->events.wake_pending) ||
		    atomic_read_acquire(&evdi->events.mailbox_wake_pending)) {
#ifdef EVDI_HAVE_ATOMIC_CMPXCHG_RELAXED
			atomic_xchg_relaxed(&evdi->events.wake_pending, 0);
			atomic_xchg_relaxed(&evdi->events.mailbox_wake_pending, 0);
#else
			atomic_set(&evdi->events.wake_pending, 0);
			atomic_set(&evdi->events.mailbox_wake_pending, 0);
#endif
		}

		evdi_smp_mb();
		ret = evdi_event_wait_ready(evdi, file);
		if (ret != 1)
			break;
		schedule();
	}
	finish_wait(&evdi->events.wait_queue, &wait);

	return ret;
}
