#
# Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
#
# Copyright (c) 2024 Lindroid Project.
#
# This file is subject to the terms and conditions of the GNU General Public
# License v2. See the file COPYING in the main directory of this archive for
# more details.
#
ifneq ($(DKMS_BUILD),)
# DKMS
KERN_DIR := /lib/modules/$(KERNELRELEASE)/build

ccflags-y := -isystem include/uapi/drm $(CFLAGS) $(EL8FLAG) $(EL9FLAG) $(RPIFLAG)
evdi-lindroid-y := evdi_connector.o evdi_event.o evdi_fb.o evdi_gem.o evdi_ioctl.o evdi_lindroid_drv.o evdi_modeset.o evdi_sysfs.o
obj-m := evdi-lindroid.o

KBUILD_VERBOSE ?= 1

all:
	$(MAKE) KBUILD_VERBOSE=$(KBUILD_VERBOSE) M=$(CURDIR) SUBDIRS=$(CURDIR) SRCROOT=$(CURDIR) CONFIG_MODULE_SIG= -C $(KERN_DIR) modules

clean:
	@echo $(KERN_DIR)
	$(MAKE) KBUILD_VERBOSE=$(KBUILD_VERBOSE) M=$(CURDIR) SUBDIRS=$(CURDIR) SRCROOT=$(CURDIR) -C $(KERN_DIR) clean

else

ccflags-y := -isystem include/uapi/drm $(CFLAGS) $(EL8FLAG) $(EL9FLAG) $(RPIFLAG)
evdi-y := evdi_connector.o evdi_event.o evdi_fb.o evdi_gem.o evdi_ioctl.o evdi_lindroid_drv.o evdi_modeset.o evdi_sysfs.o
obj-$(CONFIG_DRM_LINDROID_EVDI) := evdi.o
#obj-y += tests/

endif # ifneq ($(DKMS_BUILD),)
