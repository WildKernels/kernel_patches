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
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/stat.h>
#include <linux/platform_device.h>
#include <linux/idr.h>

extern bool evdi_perf_on;
extern struct static_key_false evdi_perf_key;

static struct device *evdi_sysfs_dev;
static DEFINE_IDA(evdi_pdev_ida);

static ssize_t add_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&evdi_device_count));
}

static ssize_t add_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct platform_device *pdev;
	struct device *parent = evdi_sysfs_dev;
	int val, ret, id;

	ret = kstrtoint(buf, 10, &val);
	if (ret) {
		evdi_err("Invalid input for device creation: %s", buf);
		return ret;
	}

	if (val <= 0) {
		evdi_err("Device count must be positive: %d", val);
		return -EINVAL;
	}

#if KERNEL_VERSION(6, 0, 0) <= LINUX_VERSION_CODE
	id = ida_alloc_range(&evdi_pdev_ida, 0, INT_MAX, GFP_KERNEL);
#else
	id = ida_simple_get(&evdi_pdev_ida, 0, INT_MAX, GFP_KERNEL);
#endif
	if (id < 0) {
		evdi_err("Failed to allocate device id: %d", id);
		return id;
	}

	pdev = platform_device_alloc(DRIVER_NAME, id);
	if (!pdev) {
		evdi_err("Failed to allocate platform device");
#if KERNEL_VERSION(6, 0, 0) <= LINUX_VERSION_CODE
		ida_free(&evdi_pdev_ida, id);
#else
		ida_simple_remove(&evdi_pdev_ida, id);
#endif
		return -ENOMEM;
	}

	pdev->dev.parent = parent;
	ret = platform_device_add(pdev);
	if (ret) {
		evdi_err("Failed to add platform device: %d", ret);
		platform_device_put(pdev);
#if KERNEL_VERSION(6, 0, 0) <= LINUX_VERSION_CODE
		ida_free(&evdi_pdev_ida, id);
#else
		ida_simple_remove(&evdi_pdev_ida, id);
#endif
		return ret;
	}

	evdi_info("Created new device via sysfs (total: %d)",
		 atomic_read(&evdi_device_count));

	return count;
}

/* HACK: Make 'add' world-writable so users can add evdi devices. */
static struct device_attribute dev_attr_add_0666 = {
	.attr = {
		.name = "add",
		.mode = 0666,
	},
	.show = add_show,
	.store = add_store,
};

static ssize_t enable_perf_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", evdi_perf_on ? 1 : 0);
}

static ssize_t enable_perf_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val) {
		if (!evdi_perf_on) {
			evdi_perf_on = true;
			static_branch_enable(&evdi_perf_key);
		}
	} else {
		if (evdi_perf_on) {
			evdi_perf_on = false;
			static_branch_disable(&evdi_perf_key);
		}
	}
	return count;
}

static DEVICE_ATTR_RW(enable_perf);

static struct attribute *evdi_sysfs_attrs[] = {
	NULL,
};

static const struct attribute_group evdi_sysfs_attr_group = {
	.attrs = evdi_sysfs_attrs,
};

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf,
	"EVDI-Lindroid Performance Statistics\n"
	"=====================================\n"
	"IOCTL calls:\n"
	"  CONNECT: %lld\n"
	"  POLL: %lld\n"
	"  VSYNC: %lld\n"
	"  GET_BUFF_CALLBACK: %lld\n"
	"  DESTROY_BUFF_CALLBACK: %lld\n"
	"  CREATE_BUFF_CALLBACK: %lld\n"
	"  EVDI_GBM_GET_BUFF: %lld\n"
	"\n"
	"Event system:\n"
	"  Wakeups: %lld\n"
	"  Poll cycles: %lld\n"
	"  Queue operations: %lld\n"
	"  Dequeue operations: %lld\n"
	"  Event allocations: %lld\n"
	"  Swap updates: %lld\n"
	"  Swap delivered: %lld\n"
	"  Inflight per-CPU hits: %lld\n"
	"  Inflight per-CPU misses: %lld\n"
	"=====================================\n",
	(long long)atomic64_read(&evdi_perf.ioctl_calls[0]),
	(long long)atomic64_read(&evdi_perf.ioctl_calls[1]),
	(long long)atomic64_read(&evdi_perf.ioctl_calls[2]),
	(long long)atomic64_read(&evdi_perf.ioctl_calls[3]),
	(long long)atomic64_read(&evdi_perf.ioctl_calls[4]),
	(long long)atomic64_read(&evdi_perf.ioctl_calls[6]),
	(long long)atomic64_read(&evdi_perf.ioctl_calls[7]),
	(long long)atomic64_read(&evdi_perf.wakeup_count),
	(long long)atomic64_read(&evdi_perf.poll_cycles),
	(long long)atomic64_read(&evdi_perf.event_queue_ops),
	(long long)atomic64_read(&evdi_perf.event_dequeue_ops),
	(long long)atomic64_read(&evdi_perf.allocs),
	(long long)atomic64_read(&evdi_perf.swap_updates),
	(long long)atomic64_read(&evdi_perf.swap_delivered),
	(long long)atomic64_read(&evdi_perf.inflight_percpu_hits),
	(long long)atomic64_read(&evdi_perf.inflight_percpu_misses));
}

static DEVICE_ATTR_RO(stats);

static struct attribute *evdi_debug_attrs[] = {
	&dev_attr_stats.attr,
	&dev_attr_enable_perf.attr,
	NULL,
};

static const struct attribute_group evdi_debug_attr_group = {
	.name = "debug",
	.attrs = evdi_debug_attrs,
};

static const struct attribute_group *evdi_attr_groups[] = {
	&evdi_sysfs_attr_group,
	&evdi_debug_attr_group,
	NULL,
};

int evdi_sysfs_init(void)
{
	int ret;


	evdi_sysfs_dev = root_device_register(DRIVER_NAME);
	if (IS_ERR(evdi_sysfs_dev)) {
		ret = PTR_ERR(evdi_sysfs_dev);
		evdi_err("Failed to register sysfs device: %d", ret);
		return ret;
	}

	ret = sysfs_create_groups(&evdi_sysfs_dev->kobj, evdi_attr_groups);
	if (ret) {
		evdi_err("Failed to create sysfs attributes: %d", ret);
		goto err_device;
	}

	ret = device_create_file(evdi_sysfs_dev, &dev_attr_add_0666);
	if (ret) {
		evdi_err("Failed to create writable 'add' attribute: %d", ret);
		goto err_groups;
	}

	evdi_info("Sysfs interface created at /sys/devices/%s/", DRIVER_NAME);
	return 0;

err_groups:
	sysfs_remove_groups(&evdi_sysfs_dev->kobj, evdi_attr_groups);
err_device:
	root_device_unregister(evdi_sysfs_dev);
	evdi_sysfs_dev = NULL;
	return ret;
}

void evdi_sysfs_cleanup(void)
{
	if (evdi_sysfs_dev) {
		device_remove_file(evdi_sysfs_dev, &dev_attr_add_0666);
		sysfs_remove_groups(&evdi_sysfs_dev->kobj, evdi_attr_groups);
		root_device_unregister(evdi_sysfs_dev);
		evdi_sysfs_dev = NULL;
	}
	ida_destroy(&evdi_pdev_ida);

	evdi_info("Sysfs interface cleaned up");
}
