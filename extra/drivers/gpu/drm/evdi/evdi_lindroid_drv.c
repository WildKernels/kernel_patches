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
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/wait.h>

extern int evdi_event_system_init(void);
extern void evdi_event_system_cleanup(void);
extern void evdi_inflight_discard_owner(struct evdi_device *evdi, struct drm_file *owner);

atomic_t evdi_device_count = ATOMIC_INIT(0);

static int evdi_driver_open(struct drm_device *dev, struct drm_file *file);
static void evdi_driver_postclose(struct drm_device *dev, struct drm_file *file);

#if EVDI_HAVE_DRM_OPEN_CLOSE
static const struct file_operations evdi_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = evdi_drm_gem_mmap,
	.llseek = noop_llseek,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
};
#endif

#define EVDI_IOCTL_FLAGS (DRM_UNLOCKED | DRM_RENDER_ALLOW)

static const struct drm_ioctl_desc evdi_ioctls[] = {
	DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_ioctl_connect,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_POLL, evdi_ioctl_poll,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_CREATE_BUFF, evdi_ioctl_gbm_create_buff,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_GET_BUFF, evdi_ioctl_gbm_get_buff,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GET_BUFF_CALLBACK, evdi_ioctl_get_buff_callback,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_DESTROY_BUFF_CALLBACK, evdi_ioctl_destroy_buff_callback,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_CREATE_BUFF_CALLBACK, evdi_ioctl_create_buff_callback,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_GBM_DEL_BUFF, evdi_ioctl_gbm_del_buff,
			 EVDI_IOCTL_FLAGS),
	DRM_IOCTL_DEF_DRV(EVDI_VSYNC, evdi_ioctl_vsync,
			DRM_RENDER_ALLOW),
};

static struct drm_driver evdi_driver = {
	.driver_features = DRIVER_MODESET |
			  DRIVER_RENDER |
#if EVDI_HAVE_ATOMIC_HELPERS
			  DRIVER_ATOMIC |
#endif
#ifdef DRIVER_PRIME
			  DRIVER_PRIME |
#endif
			  DRIVER_GEM,

	.dumb_create = evdi_dumb_create,
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
	.gem_create_object = NULL,
#endif
#if KERNEL_VERSION(5, 11, 0) > LINUX_VERSION_CODE
	.gem_free_object_unlocked = evdi_gem_free_object,
	.gem_vm_ops = &evdi_gem_vm_ops,
	.gem_prime_get_sg_table = evdi_prime_get_sg_table,
#endif
	.gem_prime_import = evdi_gem_prime_import,
	.prime_handle_to_fd = evdi_prime_handle_to_fd,
	.prime_fd_to_handle = evdi_prime_fd_to_handle,

	.open = evdi_driver_open,
	.postclose = evdi_driver_postclose,

#if EVDI_HAVE_DRM_OPEN_CLOSE
	.fops = &evdi_fops,
#endif

	.ioctls = evdi_ioctls,
	.num_ioctls = ARRAY_SIZE(evdi_ioctls),

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static int evdi_driver_open(struct drm_device *dev, struct drm_file *file)
{
	struct evdi_file_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
#ifdef EVDI_HAVE_XARRAY
	xa_init_flags(&priv->bufid_to_handle, XA_FLAGS_ALLOC);
	xa_init_flags(&priv->handle_to_bufid, XA_FLAGS_ALLOC);
	priv->next_handle = 1;
#else
	idr_init(&priv->bufid_to_handle);
	idr_init(&priv->handle_to_bufid);
	priv->next_handle = 1;
#endif
	priv->swap_rr = 0;
	priv->pending_swaps = 0;
	file->driver_priv = priv;

	return 0;
}

static void evdi_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct evdi_file_priv *priv = file->driver_priv;

	if (unlikely(!evdi))
		return;

	if (READ_ONCE(evdi->drm_client) == file) {
		WRITE_ONCE(evdi->drm_client, NULL);
	}

	evdi_smp_wmb();
	evdi_inflight_discard_owner(evdi, file);
	evdi_smp_mb();

	evdi_event_cleanup_file(evdi, file);

	if (priv) {
		mutex_lock(&priv->lock);
#ifdef EVDI_HAVE_XARRAY
		xa_destroy(&priv->handle_to_bufid);
		xa_destroy(&priv->bufid_to_handle);
#else
		idr_destroy(&priv->handle_to_bufid);
		idr_destroy(&priv->bufid_to_handle);
#endif
		WRITE_ONCE(priv->pending_swaps, 0);
		memset(priv->last_swap_seq, 0, sizeof(priv->last_swap_seq));
		priv->swap_rr = 0;
		mutex_unlock(&priv->lock);

		kfree(priv);
		file->driver_priv = NULL;
	}

	evdi_debug("Device %d closed by process %d", evdi->dev_index, current->pid);
}

int evdi_device_init(struct evdi_device *evdi, struct platform_device *pdev)
{
	int ret = 0;
	int i;

	evdi->dev_index = atomic_inc_return(&evdi_device_count) - 1;
	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		evdi->displays[i].connected = false;
		evdi->displays[i].width = 1920;
		evdi->displays[i].height = 1080;
		evdi->displays[i].refresh_rate = 60;
		evdi->displays[i].generation = 1;
		evdi->connector[i] = NULL;
	}
	evdi->drm_client = NULL;

	mutex_init(&evdi->config_mutex);

#ifdef EVDI_HAVE_XARRAY
	xa_init_flags(&evdi->file_xa, XA_FLAGS_ALLOC);
	xa_init_flags(&evdi->inflight_xa, XA_FLAGS_ALLOC);
	evdi->inflight_next_id = 1;
#else
	idr_init(&evdi->file_idr);
	spin_lock_init(&evdi->file_lock);
	idr_init(&evdi->inflight_idr);
	spin_lock_init(&evdi->inflight_lock);
#endif

	evdi->pdev = pdev;

	ret = evdi_event_init(evdi);
	if (ret) {
		evdi_err("Failed to initialize event system: %d", ret);
		goto err_cleanup_locks;
	}

	evdi_smp_wmb();

	evdi_info("Device %d initialized", evdi->dev_index);
	return 0;

err_cleanup_locks:
	evdi_event_cleanup(evdi);
#ifdef EVDI_HAVE_XARRAY
	xa_destroy(&evdi->file_xa);
	xa_destroy(&evdi->inflight_xa);
#else
	idr_destroy(&evdi->file_idr);
	idr_destroy(&evdi->inflight_idr);
#endif
	mutex_destroy(&evdi->config_mutex);
	return ret;
}

void evdi_device_cleanup(struct evdi_device *evdi)
{
	if (unlikely(!evdi))
		return;

	evdi_debug("Starting cleanup for device %d", evdi->dev_index);

	atomic_set(&evdi->events.cleanup_in_progress, 1);
	atomic_set(&evdi->events.stopping, 1);

	WRITE_ONCE(evdi->drm_client, NULL);

#ifdef EVDI_HAVE_XARRAY
	{
		struct evdi_inflight_req *req;
		unsigned long index;
		xa_for_each(&evdi->inflight_xa, index, req) {
			xa_erase(&evdi->inflight_xa, index);
			complete_all(&req->done);
			evdi_inflight_req_put(req);
		}
	}
#else
	{
		struct evdi_inflight_req *req;
		int id;
		spin_lock(&evdi->inflight_lock);
		idr_for_each_entry(&evdi->inflight_idr, req, id) {
			idr_remove(&evdi->inflight_idr, id);
			complete_all(&req->done);
			evdi_inflight_req_put(req);
		}
		spin_unlock(&evdi->inflight_lock);
	}
#endif

	evdi_smp_wmb();

	evdi_debug("Cleaning up device %d", evdi->dev_index);

	evdi_event_cleanup(evdi);
	evdi_smp_mb();
#ifdef EVDI_HAVE_XARRAY
	xa_destroy(&evdi->file_xa);
	xa_destroy(&evdi->inflight_xa);
#else
	idr_destroy(&evdi->file_idr);
	idr_destroy(&evdi->inflight_idr);
#endif
	mutex_destroy(&evdi->config_mutex);

	evdi_debug("Device %d cleaned up", evdi->dev_index);
}

static int evdi_platform_probe(struct platform_device *pdev)
{
	struct evdi_device *evdi;
	struct drm_device *ddev;
	int ret;

	evdi = kzalloc(sizeof(*evdi), GFP_KERNEL);
	if (!evdi)
		return -ENOMEM;

	ret = evdi_device_init(evdi, pdev);
	if (ret)
		goto err_init;

#ifdef EVDI_HAVE_DRM_MANAGED
	ddev = drm_dev_alloc(&evdi_driver, &pdev->dev);
#else
	ddev = drm_dev_alloc(&evdi_driver, &pdev->dev);
#endif
	if (IS_ERR(ddev)) {
		ret = PTR_ERR(ddev);
		evdi_err("Failed to allocate DRM device: %d", ret);
		goto err_drm_alloc;
	}

	ddev->dev_private = evdi;
	evdi->ddev = ddev;

	ret = evdi_modeset_init(ddev);
	if (ret) {
		evdi_err("Failed to initialize modeset: %d", ret);
		goto err_modeset;
	}

#if !EVDI_HAVE_ATOMIC_HELPERS
	ret = drm_vblank_init(ddev, LINDROID_MAX_CONNECTORS);
	if (ret)
		evdi_warn("vblank init failed: %d", ret);
#endif

	drm_kms_helper_poll_init(ddev);

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		evdi_err("Failed to register DRM device: %d", ret);
		goto err_register;
	}

	platform_set_drvdata(pdev, evdi);

	evdi_info("Platform device probed successfully, DRM device registered");
	return 0;

err_register:
	drm_kms_helper_poll_fini(ddev);
	evdi_modeset_cleanup(ddev);
err_modeset:
#ifndef EVDI_HAVE_DRM_MANAGED
	drm_dev_put(ddev);
#endif
err_drm_alloc:
	evdi_device_cleanup(evdi);
err_init:
	kfree(evdi);
	return ret;
}

static int evdi_platform_remove(struct platform_device *pdev)
{
	struct evdi_device *evdi = platform_get_drvdata(pdev);
	struct drm_device *ddev = evdi->ddev;

	evdi_info("Removing platform device");

	drm_dev_unregister(ddev);

#if EVDI_HAVE_ATOMIC_HELPERS
	drm_atomic_helper_shutdown(ddev);
#endif

	drm_kms_helper_poll_fini(ddev);

	evdi_modeset_cleanup(ddev);

	evdi_device_cleanup(evdi);

#ifndef EVDI_HAVE_DRM_MANAGED
	drm_dev_put(ddev);
#endif

	kfree(evdi);

	return 0;
}

static struct platform_driver evdi_platform_driver = {
	.probe = evdi_platform_probe,
	.remove = evdi_platform_remove,
	.driver = {
	.name = DRIVER_NAME,
	.owner = THIS_MODULE,
	},
};

static int __init evdi_init(void)
{
	int ret;

	evdi_info("Loading EVDI-Lindroid driver v%d.%d.%d",
		 DRIVER_MAJOR, DRIVER_MINOR, DRIVER_PATCHLEVEL);

	ret = evdi_event_system_init();
	if (ret) {
		evdi_err("Failed to initialize event system: %d", ret);
		return ret;
	}

	ret = evdi_fb_cache_init();
	if (ret) {
		evdi_err("Failed to initialize framebuffer cache: %d", ret);
		evdi_event_system_cleanup();
		return ret;
	}

	ret = evdi_gem_cache_init();
	if (ret) {
		evdi_err("Failed to initialize GEM cache: %d", ret);
		evdi_event_system_cleanup();
		return ret;
	}

	ret = platform_driver_register(&evdi_platform_driver);
	if (ret) {
		evdi_err("Failed to register platform driver: %d", ret);
		evdi_fb_cache_cleanup();
		evdi_gem_cache_cleanup();
		evdi_event_system_cleanup();
		return ret;
	}

	ret = evdi_sysfs_init();
	if (ret) {
		evdi_err("Failed to initialize sysfs: %d", ret);
		platform_driver_unregister(&evdi_platform_driver);
		evdi_fb_cache_cleanup();
		evdi_gem_cache_cleanup();
		evdi_event_system_cleanup();
		return ret;
	}

	evdi_info("Driver loaded successfully");
	return 0;
}

static void __exit evdi_exit(void)
{
	evdi_info("Unloading EVDI-Lindroid driver");

	evdi_sysfs_cleanup();

	platform_driver_unregister(&evdi_platform_driver);

	evdi_fb_cache_cleanup();

	evdi_gem_cache_cleanup();

	evdi_event_system_cleanup();

	evdi_info("Driver unloaded");
}

module_init(evdi_init);
module_exit(evdi_exit);

MODULE_AUTHOR("EVDI-Lindroid Project");
MODULE_DESCRIPTION("High-performance virtual display driver for Lindroid");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");
