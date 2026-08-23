// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtual connector implementation
 *
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include "evdi_drv.h"
#include <drm/drm_modes.h>

int evdi_connector_slot(const struct evdi_device *evdi,
						const struct drm_connector *conn)
{
	int i;
	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		if (evdi->connector[i] == conn)
			return i;
	}
	return -ENOENT;
}

static enum drm_connector_status
evdi_connector_detect(struct drm_connector *connector, bool force)
{
	int id;

	struct evdi_device *evdi = connector->dev->dev_private;

	if (unlikely(!evdi))
		return connector_status_disconnected;

	id = evdi_connector_slot(evdi, connector);
	if (unlikely(id < 0))
		return connector_status_disconnected;

	return evdi_likely_connected(evdi, id) ?
	   connector_status_connected :
	   connector_status_disconnected;
}

static int evdi_connector_get_modes(struct drm_connector *connector)
{
	struct evdi_device *evdi = connector->dev->dev_private;
	struct drm_display_mode *mode;
	int id;

	id = evdi_connector_slot(evdi, connector);
	if (unlikely(id < 0) || !evdi_likely_connected(evdi, id))
		return 0;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->hdisplay = evdi->displays[id].width;
	mode->vdisplay = evdi->displays[id].height;

	mode->hsync_start = mode->hdisplay + 1;
	mode->hsync_end = mode->hsync_start + 1;
	mode->htotal = mode->hsync_end + 1;

	mode->vsync_start = mode->vdisplay + 1;
	mode->vsync_end = mode->vsync_start + 1;
	mode->vtotal = mode->vsync_end + 1;

	mode->clock = mode->htotal * mode->vtotal * evdi->displays[id].refresh_rate / 1000;

	mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	evdi_debug("Created mode %ux%u@%uHz for device %d",
		   mode->hdisplay, mode->vdisplay,
		   drm_mode_vrefresh(mode), evdi->dev_index);

	return 1;
}

static enum drm_mode_status
evdi_connector_mode_valid(struct drm_connector *connector,
			 struct drm_display_mode *mode)
{
	int vrefresh = drm_mode_vrefresh(mode);
	if (mode->hdisplay < 640 || mode->hdisplay > 8192)
		return MODE_BAD_HVALUE;

	if (mode->vdisplay < 480 || mode->vdisplay > 8192)
		return MODE_BAD_VVALUE;

	if (vrefresh < 23 || vrefresh > 240)
		return MODE_BAD;

	return MODE_OK;
}

static const struct drm_connector_helper_funcs evdi_connector_helper_funcs = {
	.get_modes = evdi_connector_get_modes,
	.mode_valid = evdi_connector_mode_valid,
};

#if EVDI_HAVE_ATOMIC_HELPERS
static const struct drm_connector_funcs evdi_connector_funcs = {
	.detect = evdi_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};
#else
static int evdi_connector_dpms(struct drm_connector *connector, int mode)
{
	return 0;
}

static int evdi_connector_set_property(struct drm_connector *connector,
				       struct drm_property *property,
				       uint64_t val)
{
	return -EINVAL;
}

static void evdi_atomic_helper_connector_reset(struct drm_connector *connector)
{
	struct drm_connector_state *state;

	if (connector->state) {
		kfree(connector->state);
		connector->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return;
	state->connector = connector;
	connector->state = state;
}

static struct drm_connector_state *
evdi_atomic_helper_connector_duplicate_state(struct drm_connector *connector)
{
	struct drm_connector_state *state, *copy;

	state = connector->state;
	if (!state)
		return NULL;

	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (!copy)
		return NULL;
	copy->connector = connector;
	return copy;
}

static void
evdi_atomic_helper_connector_destroy_state(struct drm_connector *connector,
					   struct drm_connector_state *state)
{
	kfree(state);
}

static const struct drm_connector_funcs evdi_connector_funcs = {
	.detect = evdi_connector_detect,
	.dpms = evdi_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.set_property = evdi_connector_set_property,
	.reset = evdi_atomic_helper_connector_reset,
	.atomic_duplicate_state = evdi_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = evdi_atomic_helper_connector_destroy_state,
};
#endif

int evdi_connector_init(struct drm_device *dev, struct evdi_device *evdi)
{
	struct drm_connector *connector;
	int ret = 0, i, j;

	if (unlikely(!dev || !evdi))
		return -EINVAL;

	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		connector = kzalloc(sizeof(*connector), GFP_KERNEL);
		if (!connector) {
			ret = -ENOMEM;
			goto err_free_prev;
		}
#if EVDI_HAVE_CONNECTOR_INIT_WITH_DDC
		ret = drm_connector_init_with_ddc(dev, connector,
						  &evdi_connector_funcs,
						  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
#else
		ret = drm_connector_init(dev, connector, &evdi_connector_funcs,
					 DRM_MODE_CONNECTOR_VIRTUAL);
#endif
		if (ret) {
			evdi_err("Failed to initialize connector[%d]: %d", i, ret);
			kfree(connector);
			goto err_free_prev;
		}

		connector->interlace_allowed = false;
		connector->doublescan_allowed = false;
		connector->polled = DRM_CONNECTOR_POLL_CONNECT |
				    DRM_CONNECTOR_POLL_DISCONNECT;
		drm_connector_helper_add(connector, &evdi_connector_helper_funcs);
		evdi->connector[i] = connector;
	}

	evdi_debug("Connector initialized for device %d", evdi->dev_index);
	return 0;

err_free_prev:
	for (j = 0; j < i; j++) {
		if (evdi->connector[j]) {
			drm_connector_cleanup(evdi->connector[j]);
			kfree(evdi->connector[j]);
			evdi->connector[j] = NULL;
		}
	}
	return ret;
}

void evdi_connector_cleanup(struct evdi_device *evdi)
{
	int i;
	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		if (evdi->connector[i]) {
			drm_connector_cleanup(evdi->connector[i]);
			kfree(evdi->connector[i]);
			evdi->connector[i] = NULL;
		}
	}
	evdi_debug("Connector cleaned up for device %d", evdi->dev_index);
}
