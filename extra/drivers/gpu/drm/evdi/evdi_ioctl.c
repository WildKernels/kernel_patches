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
#include "uapi/evdi_drm.h"
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/prefetch.h>
#include <linux/completion.h>
#include <linux/compat.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>

static int evdi_queue_create_event_with_id(struct evdi_device *evdi, struct drm_evdi_gbm_create_buff *params, struct drm_file *owner, int poll_id);
int evdi_queue_destroy_event(struct evdi_device *evdi, int id, struct drm_file *owner);

struct evdi_gralloc_buf_stack {
	struct evdi_gralloc_buf_user buf;
	int installed_fds[EVDI_MAX_FDS];
};

static __always_inline void evdi_put_unused_fds(int *fds, int nfd)
{
	int i;

	for (i = 0; i < nfd; i++)
		put_unused_fd(fds[i]);
}

static __always_inline int evdi_get_unused_fds_batch(int n, int flags, int *fds)
{
	int i, fd;

	prefetchw(fds);

	if (unlikely(!fds || n <= 0))
		return 0;

	for (i = 0; i < n; i++) {
		if (i + 1 < n)
			prefetchw(&fds[i+1]);
		fd = get_unused_fd_flags(flags);
		if (unlikely(fd < 0)) {
			evdi_put_unused_fds(fds, i);
			return fd;
		}
		fds[i] = fd;
	}

	return 0;
}

static __always_inline int evdi_gralloc_num_fds(const struct evdi_gralloc_data *gralloc)
{
	int nfd;

	if (unlikely(!gralloc))
		return 0;

	nfd = READ_ONCE(gralloc->numFds);
	if (nfd < 0)
		return 0;
	if (nfd > EVDI_MAX_FDS)
		return EVDI_MAX_FDS;
	return nfd;
}

static __always_inline void evdi_gralloc_put_files(struct evdi_gralloc_data *gralloc)
{
	int i, nfd = evdi_gralloc_num_fds(gralloc);

	for (i = 0; i < nfd; i++) {
		if (gralloc->data_files[i]) {
			fput(gralloc->data_files[i]);
			gralloc->data_files[i] = NULL;
		}
	}
}

static __always_inline void evdi_gralloc_install_files(struct evdi_gralloc_data *gralloc,
			   const int *installed_fds, int nfd)
{
	int i;

	for (i = 0; i < nfd; i++) {
		fd_install(installed_fds[i], gralloc->data_files[i]);
		gralloc->data_files[i] = NULL;
	}
}

static int evdi_process_gralloc_buffer(struct evdi_inflight_req *req,
					int *installed_fds,
					struct evdi_gralloc_buf_user *gralloc_buf)
{
	struct evdi_gralloc_data *gralloc;
	int fd_tmp, nfd;

	gralloc = req->reply.get_buf.gralloc_buf.gralloc;
	if (!gralloc)
		return -EINVAL;

	if (unlikely(gralloc->numFds < 0 || gralloc->numFds > EVDI_MAX_FDS ||
		     gralloc->numInts < 0 || gralloc->numInts > EVDI_MAX_INTS))
		return -EINVAL;

	gralloc_buf->version = gralloc->version;
	gralloc_buf->numFds = gralloc->numFds;
	gralloc_buf->numInts = gralloc->numInts;
	nfd = gralloc_buf->numFds;
	if (gralloc_buf->numInts) {
		memcpy(&gralloc_buf->data[gralloc_buf->numFds],
		       gralloc->data_ints,
		       sizeof(int) * gralloc_buf->numInts);
	}

	fd_tmp = evdi_get_unused_fds_batch(nfd, O_RDWR, installed_fds);
	if (unlikely(fd_tmp < 0))
		return fd_tmp;

	if (nfd)
		memcpy(gralloc_buf->data, installed_fds, sizeof(int) * nfd);

	return 0;
}

//Allow partial progress; return -EFAULT only if zero progress
static int evdi_copy_from_user_allow_partial(void *dst, const void __user *src, size_t len)
{
	size_t not;

	if (!len)
		return 0;

	prefetchw(dst);
	not = copy_from_user(dst, src, len);
	if (not == len)
		return -EFAULT;

	return 0;
}

static int evdi_copy_to_user_allow_partial(void __user *dst, const void *src, size_t len)
{
	size_t not;

	if (!len)
		return 0;

	not = copy_to_user(dst, src, len);
	if (not == len)
		return -EFAULT;

	return 0;
}

static inline struct evdi_inflight_req *evdi_inflight_alloc(struct evdi_device *evdi,
						     struct drm_file *owner,
						     int type,
						     int *out_id)
{
	struct evdi_inflight_req *req;
	int id;

	req = evdi_inflight_req_alloc(evdi);
	if (unlikely(!req))
		return NULL;

	if (atomic_read(&req->from_percpu))
		EVDI_PERF_INC64(&evdi_perf.inflight_percpu_hits);
	else
		EVDI_PERF_INC64(&evdi_perf.inflight_percpu_misses);

	req->type = type;
	req->owner = owner;

#ifdef EVDI_HAVE_XARRAY
	{
		u32 xid;
#ifndef EVDI_HAVE_XA_ALLOC_CYCLIC
		u32 start_id;
#endif
		int ret;
#ifdef EVDI_HAVE_XA_ALLOC_CYCLIC
		xid = READ_ONCE(evdi->inflight_next_id);
		if (unlikely(!xid))
			xid = 1;

		ret = xa_alloc_cyclic(&evdi->inflight_xa,
				      &xid, req,
				      XA_LIMIT(1, INT_MAX),
				      &evdi->inflight_next_id,
				      GFP_NOWAIT);
		if (ret == -EBUSY || ret == -ENOMEM || ret == -EEXIST) {
			WRITE_ONCE(evdi->inflight_next_id, 1);
			xid = 1;
			ret = xa_alloc_cyclic(&evdi->inflight_xa,
					      &xid, req,
					      XA_LIMIT(1, INT_MAX),
					      &evdi->inflight_next_id,
					      GFP_NOWAIT);
		}
		if (ret) {
			evdi_inflight_req_put(req);
			return NULL;
		}
		evdi_inflight_req_get(req);
		id = (int)xid;
#else
		xid = 0;
		start_id = READ_ONCE(evdi->inflight_next_id);
		if (unlikely(!start_id))
			start_id = 1;
		ret = xa_alloc(&evdi->inflight_xa, &xid, req,
			       XA_LIMIT(start_id, INT_MAX), GFP_NOWAIT);
		if (ret == -EBUSY && start_id > 1) {
			ret = xa_alloc(&evdi->inflight_xa, &xid, req,
				       XA_LIMIT(1, EVDI_MAX_INFLIGHT_REQUESTS), GFP_NOWAIT);
		}
		if (ret) {
			evdi_inflight_req_put(req);
			return NULL;
		}
		evdi_inflight_req_get(req);
		id = (int)xid;
#endif
	}
#else
	spin_lock(&evdi->inflight_lock);
	id = idr_alloc(&evdi->inflight_idr, req, 1, EVDI_MAX_INFLIGHT_REQUESTS, GFP_ATOMIC);
	spin_unlock(&evdi->inflight_lock);
	if (id < 0) {
		evdi_inflight_req_put(req);
		return NULL;
	}
	evdi_inflight_req_get(req);
#endif
	*out_id = id;
	return req;
}

static struct evdi_inflight_req *evdi_inflight_take(struct evdi_device *evdi, int id)
{
	struct evdi_inflight_req *req = NULL;
	if (unlikely(!evdi))
		return NULL;

#ifdef EVDI_HAVE_XARRAY
	req = xa_erase(&evdi->inflight_xa, id);
#else
	spin_lock(&evdi->inflight_lock);
	req = idr_find(&evdi->inflight_idr, id);
	if (req)
		idr_remove(&evdi->inflight_idr, id);

	spin_unlock(&evdi->inflight_lock);
#endif
	return req;
}

void evdi_inflight_discard_owner(struct evdi_device *evdi, struct drm_file *owner)
{
	struct evdi_inflight_req *req;

	if (unlikely(!evdi || !owner))
		return;

#ifdef EVDI_HAVE_XARRAY
	{
		XA_STATE(xas, &evdi->inflight_xa, 0);

		rcu_read_lock();
		xas_for_each(&xas, req, ULONG_MAX) {
			if (req->owner != owner)
				continue;

			if (xa_cmpxchg(&evdi->inflight_xa,
				       xas.xa_index, req, NULL, GFP_NOWAIT) != req)
				continue;
			rcu_read_unlock();
			complete_all(&req->done);
			evdi_inflight_req_put(req);
			cond_resched();
			rcu_read_lock();
		}
		rcu_read_unlock();
	}
#else
	{
		struct evdi_inflight_req *batch[16];
		int nr, i, id;

		do {
			nr = 0;
			id = 0;
			spin_lock(&evdi->inflight_lock);
			while (nr < 64) {
				req = idr_get_next(&evdi->inflight_idr, &id);
				if (!req)
					break;
				if (req->owner == owner) {
					idr_remove(&evdi->inflight_idr, id);
					batch[nr++] = req;
				}
				id++;
			}
			spin_unlock(&evdi->inflight_lock);

			for (i = 0; i < nr; i++) {
				complete_all(&batch[i]->done);
				evdi_inflight_req_put(batch[i]);
				cond_resched();
			}
		} while (nr == 16);
	}
#endif
}

#if !EVDI_HAVE_KMEM_USERCOPY
static inline size_t evdi_event_serialize_payload(struct evdi_event *e,
						  void *out_buf,
						  size_t out_buf_size)
{
	size_t copy_size;

	if (unlikely(!e || !out_buf || !out_buf_size))
		return 0;

	copy_size = min_t(size_t, e->payload_size, out_buf_size);
	if (copy_size)
		memcpy(out_buf, e->payload, copy_size);

	return copy_size;
}
#endif

static int evdi_queue_create_event_with_id(struct evdi_device *evdi,
	   struct drm_evdi_gbm_create_buff *params,
	   struct drm_file *owner,
	   int poll_id)
{
	struct evdi_event *event = evdi_event_alloc(evdi, create_buf, poll_id,
		(void *)params, sizeof(*params), owner);
	if (!event)
		return -ENOMEM;

	evdi_event_queue(evdi, event);
	return 0;
}

static int evdi_queue_get_buf_event_with_id(struct evdi_device *evdi,
	struct drm_evdi_gbm_get_buff *params,
	struct drm_file *owner,
	int poll_id)
{
	struct evdi_event *event;

	event = evdi_event_alloc(evdi, get_buf, poll_id,
		(void *)params, sizeof(*params), owner);
	if (!event)
		return -ENOMEM;

	evdi_event_queue(evdi, event);
	return 0;
}

static inline void evdi_flush_work(struct evdi_device *evdi)
{
	if (unlikely(!evdi))
		return;

	atomic_set(&evdi->events.stopping, 1);
	evdi_smp_wmb();
	wake_up_interruptible(&evdi->events.wait_queue);
}

static inline void evdi_display_bump_generation(struct evdi_display *st)
{
	st->generation++;
	if (unlikely(st->generation == 0))
		st->generation = 1;
}

int evdi_ioctl_connect(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_connect *cmd = data;
	struct drm_file *stale_owner = NULL;
	struct evdi_display *st;
	bool geometry_changed = false;
	bool need_mailbox_invalidate = false;
	bool client_changed = false;
	int i, any = 0;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[0]);

	if (!cmd->connected) {
		if (cmd->display_id >= LINDROID_MAX_CONNECTORS)
			return -EINVAL;
		evdi_flush_work(evdi);
		stale_owner = READ_ONCE(evdi->drm_client);

		mutex_lock(&evdi->config_mutex);
		st = &evdi->displays[cmd->display_id];
		st->connected = false;
		evdi_display_bump_generation(st);
		mutex_unlock(&evdi->config_mutex);

		evdi_swap_mailbox_invalidate_display(evdi, cmd->display_id);

		for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
			any |= evdi->displays[i].connected;
		if (!any)
			WRITE_ONCE(evdi->drm_client, NULL);
		evdi_smp_wmb();

		evdi_info("Device %d disconnected", evdi->dev_index);
#ifdef EVDI_HAVE_KMS_HELPER
		drm_kms_helper_hotplug_event(dev);
#else
		drm_helper_hpd_irq_event(dev);
#endif
		return 0;
	}

	if (evdi->drm_client && evdi->drm_client != file) {
		evdi_warn("Device %d forcefully disconnecting previous client", evdi->dev_index);
		need_mailbox_invalidate = true;
		client_changed = true;
		atomic_set(&evdi->events.stopping, 1);
		evdi_smp_wmb();
		wake_up_interruptible(&evdi->events.wait_queue);
	}

	if (cmd->display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	stale_owner = READ_ONCE(evdi->drm_client);
	if (!stale_owner)
		stale_owner = file;

	mutex_lock(&evdi->config_mutex);
	st = &evdi->displays[cmd->display_id];
	geometry_changed = (st->width != cmd->width) ||
				(st->height != cmd->height);

	if (client_changed || geometry_changed)
		evdi_display_bump_generation(st);

	st->connected = true;
	st->width = cmd->width;
	st->height = cmd->height;
	st->refresh_rate = cmd->refresh_rate;

	if (client_changed || geometry_changed)
		need_mailbox_invalidate = true;

	mutex_unlock(&evdi->config_mutex);

	if (need_mailbox_invalidate) {
		evdi_swap_mailbox_invalidate_display(evdi, cmd->display_id);
		if (stale_owner && stale_owner != file) {
			evdi_event_cleanup_file(evdi, stale_owner);
			evdi_inflight_discard_owner(evdi, stale_owner);
		}
	}

	evdi_smp_wmb();
	WRITE_ONCE(evdi->drm_client, file);

	evdi_info("Device %d connected: %ux%u@%uHz id:%u",
		  evdi->dev_index, cmd->width, cmd->height, cmd->refresh_rate, cmd->display_id);

	atomic_set(&evdi->events.stopping, 0);

#ifdef EVDI_HAVE_KMS_HELPER
	drm_kms_helper_hotplug_event(dev);
#else
	drm_helper_hpd_irq_event(dev);
#endif
	return 0;
}

static __always_inline u64 evdi_swap_mailbox_seq_current(struct evdi_device *evdi,
							 int display_id)
{
	if (unlikely(!evdi))
		return 0;
	if (unlikely(display_id < 0 || display_id >= LINDROID_MAX_CONNECTORS))
		return 0;

	return (u64)atomic64_read(&evdi->swap_mailbox[display_id].seq);
}

static __always_inline bool evdi_swap_mailbox_read_stable(struct evdi_device *evdi,
							 int display_id,
							 u64 *seq,
							 u64 *payload,
							 int *poll_id,
							 struct drm_file **owner)
{
	struct evdi_swap_mailbox *mb;
	u64 s1, s2, p;
	int pid;
	struct drm_file *o;
	int tries = 0;

	if (unlikely(!evdi))
		return false;

	if (unlikely(display_id < 0 || display_id >= LINDROID_MAX_CONNECTORS))
		return false;

	mb = &evdi->swap_mailbox[display_id];

	for (;;) {
		s1 = (u64)atomic64_read(&mb->seq);
		if (s1 & 1)
			goto retry;

		evdi_smp_rmb();
		p = (u64)atomic64_read(&mb->payload);
		pid = atomic_read(&mb->poll_id);
		o = READ_ONCE(mb->owner);
		evdi_smp_rmb();

		s2 = (u64)atomic64_read(&mb->seq);
		if (likely(s1 == s2 && !(s2 & 1)))
			break;

retry:
		if (++tries >= 8)
			return false;
		cpu_relax();
	}

	*seq = s2;
	*payload = p;
	*poll_id = pid;
	*owner = o;
	return true;
}

static __always_inline bool evdi_swap_try_dequeue_display(struct evdi_device *evdi,
							  struct drm_file *file,
							  struct evdi_file_priv *priv,
							  int d,
							  struct evdi_swap *out,
							  int *out_poll_id)
{
	struct drm_file *owner;
	int poll_id;
	u64 seq, payload, latest;

	if (!evdi_swap_mailbox_read_stable(evdi, d, &seq, &payload, &poll_id,
					   &owner))
		return false;

	if (owner != file) {
		clear_bit(d, &priv->pending_swaps);
		return false;
	}

	if (seq == priv->last_swap_seq[d]) {
		latest = evdi_swap_mailbox_seq_current(evdi, d);
		if (!(latest & 1) && latest == seq)
			clear_bit(d, &priv->pending_swaps);
		return false;
	}

	priv->last_swap_seq[d] = seq;
	priv->swap_rr = (u8)(d + 1);
	if (priv->swap_rr >= LINDROID_MAX_CONNECTORS)
		priv->swap_rr = 0;

	out->id = (int)(u32)(payload >> 32);
	out->display_id = (int)(u32)payload;
	*out_poll_id = poll_id;

	latest = evdi_swap_mailbox_seq_current(evdi, d);
	if (!(latest & 1) && latest == seq)
		clear_bit(d, &priv->pending_swaps);

	return true;
}

static __always_inline bool evdi_swap_dequeue_for_file(struct evdi_device *evdi,
						       struct drm_file *file,
						       struct evdi_swap *out,
						       int *out_poll_id)
{
	unsigned long pending;
	struct evdi_file_priv *priv;
	int start, d;

	if (unlikely(!evdi || !file || !out || !out_poll_id))
		return false;

	priv = file->driver_priv;
	if (unlikely(!priv))
		return false;

	pending = READ_ONCE(priv->pending_swaps);
	if (!pending)
		return false;

	start = (int)READ_ONCE(priv->swap_rr);
	if (unlikely(start >= LINDROID_MAX_CONNECTORS))
		start = 0;

	for (d = start; d < LINDROID_MAX_CONNECTORS; d++) {
		if (!test_bit(d, &pending))
			continue;
		if (evdi_swap_try_dequeue_display(evdi, file, priv, d, out,
						  out_poll_id))
			return true;
	}

	for (d = 0; d < start; d++) {
		if (!test_bit(d, &pending))
			continue;
		if (evdi_swap_try_dequeue_display(evdi, file, priv, d, out,
						  out_poll_id))
			return true;
	}

	return false;
}

int evdi_ioctl_poll(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_poll *cmd = data;
	struct evdi_event *event;
	struct evdi_swap sw;
	size_t payload_size;
	int ret, poll_id;

#if !EVDI_HAVE_KMEM_USERCOPY
	u8 payload_buf[EVDI_EVENT_PAYLOAD_MAX];
#endif

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[1]);

	/* swap mailbox fast path */
	if (evdi_swap_dequeue_for_file(evdi, file, &sw, &poll_id)) {
		cmd->event = swap_to;
		cmd->poll_id = poll_id;
		if (cmd->data) {
			if (evdi_copy_to_user_allow_partial(cmd->data, &sw, sizeof(sw)))
				return -EFAULT;
		}
		EVDI_PERF_INC64(&evdi_perf.swap_delivered);
		return 0;
	}

	event = evdi_event_dequeue(evdi);
	if (event) {
		goto deliver;
	}

	ret = evdi_event_wait(evdi, file);
	if (ret)
		return ret;

	/* Check mailbox again after wake */
	if (evdi_swap_dequeue_for_file(evdi, file, &sw, &poll_id)) {
		cmd->event = swap_to;
		cmd->poll_id = poll_id;
		if (cmd->data) {
			if (evdi_copy_to_user_allow_partial(cmd->data, &sw, sizeof(sw)))
				return -EFAULT;
		}
		EVDI_PERF_INC64(&evdi_perf.swap_delivered);
		return 0;
	}

	event = evdi_event_dequeue(evdi);
	if (!event)
		return -EAGAIN;

deliver:
	cmd->event = event->type;
	cmd->poll_id = event->poll_id;

#if EVDI_HAVE_KMEM_USERCOPY
	payload_size = min_t(size_t, READ_ONCE(event->payload_size),
			     sizeof(event->payload));
#else
	payload_size = evdi_event_serialize_payload(event,
			payload_buf, sizeof(payload_buf));
#endif

	if (payload_size && cmd->data) {
#if EVDI_HAVE_KMEM_USERCOPY
		if (evdi_copy_to_user_allow_partial(cmd->data,
						    event->payload,
						    payload_size)) {
#else
		if (evdi_copy_to_user_allow_partial(cmd->data,
						    payload_buf,
						    payload_size)) {
#endif
		evdi_event_free(event);
		return -EFAULT;
		}
	}

	evdi_event_free(event);
	return 0;
}

int evdi_ioctl_gbm_get_buff(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_get_buff *cmd = data;
	struct evdi_inflight_req *req;
	struct drm_evdi_gbm_get_buff evt_params;
	struct evdi_gralloc_buf_stack stack_buf;
	struct evdi_gralloc_buf_user *gralloc_buf;
	struct evdi_gralloc_data *gralloc;
	void __user *u_native_handle;
	int poll_id;
	long ret;
	int nfd, copy_size;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[7]);

	u_native_handle = cmd->native_handle;
	if (!u_native_handle ||
	    !evdi_access_ok_write(u_native_handle,
				  sizeof(struct evdi_gralloc_buf_user)))
		return -EFAULT;

	req = evdi_inflight_alloc(evdi, file, get_buf, &poll_id);
	if (!req)
		return -ENOMEM;

	memset(&evt_params, 0, sizeof(evt_params));
	evt_params.id = cmd->id;
	evt_params.native_handle = NULL;

	if (evdi_queue_get_buf_event_with_id(evdi, &evt_params, file, poll_id)) {
		struct evdi_inflight_req *tmp = evdi_inflight_take(evdi, poll_id);
		if (tmp)
			evdi_inflight_req_put(tmp);

		evdi_inflight_req_put(req);
		return -ENOMEM;
	}

	ret = wait_for_completion_interruptible_timeout(&req->done, EVDI_WAIT_TIMEOUT);
	if (ret == 0) {
			evdi_inflight_req_put(req);
			return -ETIMEDOUT;
	}
	if (ret < 0) {
			evdi_inflight_req_put(req);
			return (int)ret;
	}

	gralloc_buf = &stack_buf.buf;

	ret = evdi_process_gralloc_buffer(req, stack_buf.installed_fds, gralloc_buf);
	if (ret) {
		evdi_inflight_req_put(req);
		return ret;
	}

	gralloc = req->reply.get_buf.gralloc_buf.gralloc;
	nfd = gralloc_buf->numFds;
	copy_size = sizeof(int) * (3 + nfd + gralloc_buf->numInts);
	if (gralloc)
		prefetch(gralloc);

	if (evdi_copy_to_user_allow_partial(u_native_handle, gralloc_buf, copy_size)) {
		evdi_put_unused_fds(stack_buf.installed_fds, nfd);
		ret = -EFAULT;
		goto out_put_files;
	}

	evdi_gralloc_install_files(gralloc, stack_buf.installed_fds, nfd);

	ret = 0;
	goto out_req_put;

out_put_files:
	evdi_gralloc_put_files(gralloc);

out_req_put:
	evdi_inflight_req_put(req);
	return ret;
}

static inline void evdi_file_track_buffer(struct drm_file *file, int id)
{
	struct evdi_file_priv *priv;
	int ret = 0;
	u32 handle = 0;
	void *entry;

	if (unlikely(!file || id <= 0))
		return;

	priv = file->driver_priv;
	if (unlikely(!priv))
		return;

	mutex_lock(&priv->lock);

#ifdef EVDI_HAVE_XARRAY
#ifdef EVDI_HAVE_XA_ALLOC_CYCLIC
	entry = xa_load(&priv->bufid_to_handle, id);
	if (entry)
		goto out_unlock;

	ret = xa_alloc_cyclic(&priv->handle_to_bufid, &handle,
			      xa_mk_value((unsigned long)id),
			      XA_LIMIT(1, INT_MAX),
			      &priv->next_handle, GFP_KERNEL);
	if (ret)
		goto out_unlock;

	ret = xa_err(xa_store(&priv->bufid_to_handle, id,
			      xa_mk_value((unsigned long)handle),
			      GFP_KERNEL));
	if (ret) {
		xa_erase(&priv->handle_to_bufid, (unsigned long)handle);
		goto out_unlock;
	}
#else
	{
		u32 start;

		entry = xa_load(&priv->bufid_to_handle, id);
		if (entry)
			goto out_unlock;

		start = READ_ONCE(priv->next_handle);
		if (unlikely(!start))
			start = 1;

		handle = start;
		ret = xa_alloc(&priv->handle_to_bufid, &handle,
			       xa_mk_value((unsigned long)id),
			       XA_LIMIT(start, INT_MAX), GFP_KERNEL);
		if (ret == -EBUSY && start > 1) {
			handle = 1;
			ret = xa_alloc(&priv->handle_to_bufid, &handle,
				       xa_mk_value((unsigned long)id),
				       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
		}
		if (ret)
			goto out_unlock;

		ret = xa_err(xa_store(&priv->bufid_to_handle, id,
				      xa_mk_value((unsigned long)handle),
				      GFP_KERNEL));
		if (ret) {
			xa_erase(&priv->handle_to_bufid, (unsigned long)handle);
			goto out_unlock;
		}

		WRITE_ONCE(priv->next_handle, handle + 1);
	}
#endif
#else
	{
		int start;
		int hret;

		entry = idr_find(&priv->bufid_to_handle, id);
		if (entry)
			goto out_unlock;

		start = (int)READ_ONCE(priv->next_handle);
		if (start <= 0)
			start = 1;

		hret = idr_alloc(&priv->handle_to_bufid, (void *)(uintptr_t)id,
				 start, INT_MAX, GFP_KERNEL);
		if (hret == -ENOSPC && start > 1)
			hret = idr_alloc(&priv->handle_to_bufid,
					 (void *)(uintptr_t)id,
					 1, INT_MAX, GFP_KERNEL);
		if (hret < 0) {
			ret = hret;
			goto out_unlock;
		}

		handle = (u32)hret;
		ret = idr_alloc(&priv->bufid_to_handle,
				(void *)(uintptr_t)handle,
				id, id + 1, GFP_KERNEL);
		if (ret != id) {
			if (ret >= 0)
				ret = -EINVAL;
			idr_remove(&priv->handle_to_bufid, handle);
			goto out_unlock;
		}

		WRITE_ONCE(priv->next_handle, handle + 1);
		ret = 0;
	}
#endif

out_unlock:
	mutex_unlock(&priv->lock);

	if (unlikely(ret))
		evdi_warn("Failed to track buffer %d (%d)", id, ret);
}

static inline void evdi_file_untrack_buffer(struct drm_file *file, int id)
{
	struct evdi_file_priv *priv;
	void *entry;
	u32 handle;


	if (unlikely(!file || id <= 0))
		return;

	priv = file->driver_priv;
	if (unlikely(!priv))
		return;

	mutex_lock(&priv->lock);

#ifdef EVDI_HAVE_XARRAY
#ifdef EVDI_HAVE_XA_ALLOC_CYCLIC
	entry = xa_load(&priv->bufid_to_handle, id);
	if (!entry)
		goto out_unlock;

	handle = (u32)xa_to_value(entry);
	xa_erase(&priv->bufid_to_handle, id);
	xa_erase(&priv->handle_to_bufid, (unsigned long)handle);
#else
	entry = xa_load(&priv->bufid_to_handle, id);
	if (!entry)
		goto out_unlock;

	handle = (u32)xa_to_value(entry);
	xa_erase(&priv->bufid_to_handle, id);
	xa_erase(&priv->handle_to_bufid, (unsigned long)handle);
#endif
#else
	entry = idr_find(&priv->bufid_to_handle, id);
	if (!entry)
		goto out_unlock;

	handle = (u32)(uintptr_t)entry;
	idr_remove(&priv->bufid_to_handle, id);
	idr_remove(&priv->handle_to_bufid, handle);
#endif

out_unlock:
	mutex_unlock(&priv->lock);
}

int evdi_ioctl_gbm_create_buff(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_create_buff *cmd = data;
	struct evdi_inflight_req *req;
	struct evdi_inflight_req *tmp;
	struct drm_evdi_gbm_create_buff evt_params;
	int __user *u_id;
	__u32 __user *u_stride;
	int poll_id;
	long wret;

	u_id = cmd->id;
	u_stride = cmd->stride;
	if (u_id && !evdi_access_ok_write(u_id, sizeof(*u_id)))
		return -EFAULT;

	if (u_stride && !evdi_access_ok_write(u_stride, sizeof(*u_stride)))
		return -EFAULT;

	req = evdi_inflight_alloc(evdi, file, create_buf, &poll_id);
	if (!req)
		return -ENOMEM;

	memset(&evt_params, 0, sizeof(evt_params));
	evt_params.format = cmd->format;
	evt_params.width = cmd->width;
	evt_params.height = cmd->height;
	evt_params.id = NULL;
	evt_params.stride = NULL;

	if (evdi_queue_create_event_with_id(evdi, &evt_params, file, poll_id)) {
		tmp = evdi_inflight_take(evdi, poll_id);
		if (tmp)
			evdi_inflight_req_put(tmp);

		evdi_inflight_req_put(req);

		return -ENOMEM;
	}

	wret = wait_for_completion_interruptible_timeout(&req->done, EVDI_WAIT_TIMEOUT);
	if (wret == 0) {
		evdi_inflight_req_put(req);
		return -ETIMEDOUT;
	}
	if (wret < 0) {
		evdi_inflight_req_put(req);
		return (int)wret;
	}

	if (u_id) {
		if (evdi_copy_to_user_allow_partial(u_id, &req->reply.create.id, sizeof(*u_id))) {
			evdi_inflight_req_put(req);
			return -EFAULT;
		}
	}

	evdi_file_track_buffer(file, req->reply.create.id);

	if (u_stride) {
		if (evdi_copy_to_user_allow_partial(u_stride, &req->reply.create.stride, sizeof(*u_stride))) {
			evdi_inflight_req_put(req);
			return -EFAULT;
		}
	}

	evdi_inflight_req_put(req);
	return 0;
}

int evdi_ioctl_get_buff_callback(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_get_buff_callabck *cb = data;
	struct evdi_inflight_req *req;
	struct evdi_gralloc_data *gralloc = NULL;
	struct file *f;
	int i, nfd, nint;
	int fds_local[EVDI_MAX_FDS];

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[3]);

	req = evdi_inflight_take(evdi, cb->poll_id);
	if (!req)
		return 0;

	if (cb->numFds < 0 || cb->numInts < 0 ||
	    cb->numFds > EVDI_MAX_FDS || cb->numInts > EVDI_MAX_INTS)
		goto out_complete;

	nfd = cb->numFds;
	nint = cb->numInts;

	gralloc = mempool_alloc(global_event_pool.gralloc_data_pool, GFP_KERNEL);
	if (!gralloc)
		goto out_complete;

	gralloc->version = cb->version;
	gralloc->numFds = 0;
	gralloc->numInts = 0;

	if (nint) {
		if (evdi_copy_from_user_allow_partial(gralloc->data_ints,
						      cb->data_ints,
						      sizeof(int) * nint)) {
			mempool_free(gralloc, global_event_pool.gralloc_data_pool);
			gralloc = NULL;
			goto out_complete;
		}
		gralloc->numInts = nint;
	}

	if (nfd) {
		if (evdi_copy_from_user_allow_partial(fds_local, cb->fd_ints,
						      sizeof(int) * nfd)) {
			mempool_free(gralloc, global_event_pool.gralloc_data_pool);
			gralloc = NULL;
			goto out_complete;
		}
		for (i = 0; i < nfd; i++) {
			f = fget(fds_local[i]);
			if (!f) {
				evdi_gralloc_put_files(gralloc);
				mempool_free(gralloc, global_event_pool.gralloc_data_pool);
				gralloc = NULL;
				goto out_complete;
			}
			gralloc->data_files[gralloc->numFds++] = f;
		}
	}

	req->reply.get_buf.gralloc_buf.gralloc = gralloc;

out_complete:
	complete_all(&req->done);
	evdi_inflight_req_put(req);
	return 0;
}

int evdi_ioctl_destroy_buff_callback(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[4]);

	evdi_wakeup_pollers(evdi);

	return 0;
}

int evdi_ioctl_create_buff_callback(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_create_buff_callabck *cb = data;
	struct evdi_inflight_req *req;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[6]);

	req = evdi_inflight_take(evdi, cb->poll_id);
	if (req) {
		if (cb->id < 0 || cb->stride < 0) {
			req->reply.create.id = 0;
			req->reply.create.stride = 0;
		} else {
			req->reply.create.id = cb->id;
			req->reply.create.stride = cb->stride;
		}
		complete_all(&req->done);
		evdi_inflight_req_put(req);
	} else {
		evdi_warn("create_buff_callback: poll_id %d not found", cb->poll_id);
	}

	return 0;
}

int evdi_ioctl_gbm_del_buff(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_del_buff *cmd = data;
	long ret;

	ret = evdi_queue_destroy_event(evdi, cmd->id, file);
	if (!ret)
		evdi_file_untrack_buffer(file, cmd->id);

	return ret;
}

static int evdi_queue_int_event(struct evdi_device *evdi,
	enum poll_event_type type, int v, struct drm_file *owner)
{
	struct evdi_event *event;

	event = evdi_event_alloc(evdi, type,
		atomic_inc_return(&evdi->events.next_poll_id),
		(void *)&v, sizeof(int), owner);

	if (!event)
		return -ENOMEM;

	evdi_event_queue(evdi, event);
	return 0;
}

int evdi_queue_swap_event(struct evdi_device *evdi, int id, int display_id,
			  u32 generation, struct drm_file *owner)
{
	struct drm_file *client;
	struct evdi_swap_mailbox *mb;
	struct evdi_file_priv *priv;
	u32 current_generation;
	u64 payload;
	int poll_id;

	if (unlikely(!evdi))
		return -EINVAL;

	if (unlikely(display_id < 0 || display_id >= LINDROID_MAX_CONNECTORS))
		return -EINVAL;

	if (unlikely(atomic_read(&evdi->events.stopping)))
		return -ENODEV;

	if (unlikely(!READ_ONCE(evdi->displays[display_id].connected)))
		return -ENODEV;

	current_generation = READ_ONCE(evdi->displays[display_id].generation);
	if (unlikely(current_generation != generation))
		return -ESTALE;

	client = READ_ONCE(evdi->drm_client);

	if (client)
		owner = client;

	if (unlikely(!owner))
		return -ENODEV;

	mb = &evdi->swap_mailbox[display_id];
	payload = evdi_swap_pack(id, display_id);
	poll_id = atomic_inc_return(&evdi->events.next_poll_id);
	priv = owner ? owner->driver_priv : NULL;

	atomic64_inc(&mb->seq); // odd
	WRITE_ONCE(mb->owner, owner);
	atomic_set(&mb->poll_id, poll_id);
	atomic64_set(&mb->payload, payload);
	evdi_smp_wmb();
	atomic64_inc(&mb->seq); // even

	EVDI_PERF_INC64(&evdi_perf.swap_updates);

	if (priv) {
		set_bit(display_id, &priv->pending_swaps);
		smp_mb__after_atomic();
	}

	// Swap events do not use the standard event queue, so use a mailbox-specific helper
	evdi_wakeup_mailbox_pollers(evdi);

	return 0;
}

int evdi_queue_add_buf_event(struct evdi_device *evdi, int fd_data, struct drm_file *owner)
{
	return evdi_queue_int_event(evdi, add_buf, fd_data, owner);
}

int evdi_queue_get_buf_event(struct evdi_device *evdi, int id, struct drm_file *owner)
{
	return evdi_queue_int_event(evdi, get_buf, id, owner);
}

int evdi_queue_destroy_event(struct evdi_device *evdi, int id, struct drm_file *owner)
{
	return evdi_queue_int_event(evdi, destroy_buf, id, owner);
}

int evdi_queue_create_event(struct evdi_device *evdi,
			   struct drm_evdi_gbm_create_buff *params,
			   struct drm_file *owner)
{
	int poll_id = atomic_inc_return(&evdi->events.next_poll_id);
	return evdi_queue_create_event_with_id(evdi, params, owner, poll_id);
}

int evdi_ioctl_vsync(struct drm_device *dev,
                     void *data,
                     struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_vsync *vs = data;
	struct drm_crtc *crtc;
	int slot;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[2]);

	if (unlikely(atomic_read(&evdi->events.stopping)))
		return -ENODEV;

	slot = vs->display_id;

	if (slot < 0 || slot >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	crtc = &evdi->pipe[slot].crtc;

	if (drm_crtc_vblank_get(crtc) == 0) {
		drm_crtc_handle_vblank(crtc);
		drm_crtc_vblank_put(crtc);
	}

	return 0;
}
