# Copyright (C) 2010 Chia-I Wu <olvaffe@gmail.com>
# Copyright (C) 2010-2011 LunarG Inc.
# 
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

# Android.mk for drm_gralloc

ifeq ($(strip $(TARGET_BOARD_PLATFORM_GPU)), mali-tDVx)

DRM_GPU_DRIVERS := $(strip $(filter-out swrast, $(BOARD_GPU_DRIVERS)))
DRM_GPU_DRIVERS := rockchip
intel_drivers := i915 i965 i915g ilo
radeon_drivers := r300g r600g
rockchip_drivers := rockchip
nouveau_drivers := nouveau
vmwgfx_drivers := vmwgfx

valid_drivers := \
	$(intel_drivers) \
	$(radeon_drivers) \
	$(rockchip_drivers) \
	$(nouveau_drivers) \
	$(vmwgfx_drivers)

# warn about invalid drivers
invalid_drivers := $(filter-out $(valid_drivers), $(DRM_GPU_DRIVERS))
ifneq ($(invalid_drivers),)
$(warning invalid GPU drivers: $(invalid_drivers))
# tidy up
DRM_GPU_DRIVERS := $(filter-out $(invalid_drivers), $(DRM_GPU_DRIVERS))
endif

ifneq ($(filter $(vmwgfx_drivers), $(DRM_GPU_DRIVERS)),)
DRM_USES_PIPE := true
else
DRM_USES_PIPE := false
endif

ifneq ($(strip $(DRM_GPU_DRIVERS)),)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libgralloc_drm
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_TAGS := optional

LOCAL_C_INCLUDES := \
	external/libdrm \
	external/libdrm/include/drm \
	system/core/liblog/include \

LOCAL_HEADER_LIBRARIES += \
	libhardware_headers \
	liblog_headers \
	libutils_headers \
	libcutils_headers

LOCAL_CPPFLAGS := -Wunused-variable
LOCAL_SRC_FILES := \
	gralloc_drm.cpp

LOCAL_SHARED_LIBRARIES := \
	libdrm \
	liblog \
	libcutils \
	libutils \

	# libhardware_legacy \

ifneq ($(filter $(rockchip_drivers), $(DRM_GPU_DRIVERS)),)
MALI_ARCHITECTURE_UTGARD := 0
MALI_SUPPORT_AFBC_WIDEBLK?=0
MALI_USE_YUV_AFBC_WIDEBLK?=0

# AFBC buffers should be initialised after allocation in all rk platforms.
GRALLOC_INIT_AFBC?=1

# for bifrost GPU, use afbc layer by default.
USE_AFBC_LAYER = 0

# enable AFBC by default
MALI_AFBC_GRALLOC := 1

GRALLOC_VERSION_MAJOR:=0
GRALLOC_USE_LEGACY_CALCS:=1

DISABLE_FRAMEBUFFER_HAL?=1
GRALLOC_DEPTH?=GRALLOC_32_BITiS
GRALLOC_FB_SWAP_RED_BLUE?=0

#If cropping should be enabled for AFBC YUV420 buffers
AFBC_YUV420_EXTRA_MB_ROW_NEEDED ?= 0

ifeq ($(GRALLOC_ARM_NO_EXTERNAL_AFBC),1)
LOCAL_CFLAGS += -DGRALLOC_ARM_NO_EXTERNAL_AFBC=1
endif

ifeq ($(MALI_AFBC_GRALLOC), 1)
AFBC_FILES = gralloc_buffer_priv.cpp
else
MALI_AFBC_GRALLOC := 0
AFBC_FILES =
endif

LOCAL_C_INCLUDES += \
	hardware/rockchip/librkvpu \

LOCAL_SRC_FILES += gralloc_drm_rockchip.cpp \
	format_info.cpp \
	mali_gralloc_bufferallocation.cpp \
	mali_gralloc_formats.cpp \
	legacy/buffer_alloc.cpp \
	$(AFBC_FILES)

LOCAL_SHARED_LIBRARIES += libdrm_rockchip

#RK_DRM_GRALLOC_DEBUG for rockchip drm gralloc debug.
MAJOR_VERSION := "RK_GRAPHICS_VER=commit-id:$(shell cd $(LOCAL_PATH) && git log  -1 --oneline | awk '{print $$1}')"
LOCAL_CFLAGS +=-DRK_DRM_GRALLOC_DEBUG=0 -DENABLE_ROCKCHIP -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION) -D$(GRALLOC_DEPTH) -DMALI_ARCHITECTURE_UTGARD=$(MALI_ARCHITECTURE_UTGARD) -DDISABLE_FRAMEBUFFER_HAL=$(DISABLE_FRAMEBUFFER_HAL) -DMALI_AFBC_GRALLOC=$(MALI_AFBC_GRALLOC) -DMALI_SUPPORT_AFBC_WIDEBLK=$(MALI_SUPPORT_AFBC_WIDEBLK) -DAFBC_YUV420_EXTRA_MB_ROW_NEEDED=$(AFBC_YUV420_EXTRA_MB_ROW_NEEDED) -DGRALLOC_INIT_AFBC=$(GRALLOC_INIT_AFBC) -DRK_GRAPHICS_VER=\"$(MAJOR_VERSION)\" -DUSE_AFBC_LAYER=$(USE_AFBC_LAYER) -DGRALLOC_USE_LEGACY_CALCS=$(GRALLOC_USE_LEGACY_CALCS) -DGRALLOC_VERSION_MAJOR=$(GRALLOC_VERSION_MAJOR)

ifeq ($(TARGET_USES_HWC2),true)
    LOCAL_CFLAGS += -DUSE_HWC2
endif

# disable arm_format_selection on rk platforms, by default.
LOCAL_CFLAGS += -DGRALLOC_ARM_FORMAT_SELECTION_DISABLE
LOCAL_CFLAGS += -DGRALLOC_LIBRARY_BUILD=1 -DGRALLOC_USE_GRALLOC1_API=0

ifeq ($(GRALLOC_FB_SWAP_RED_BLUE),1)
LOCAL_CFLAGS += -DGRALLOC_FB_SWAP_RED_BLUE
endif

ifeq ($(GRALLOC_ARM_NO_EXTERNAL_AFBC),1)
LOCAL_CFLAGS += -DGRALLOC_ARM_NO_EXTERNAL_AFBC=1
endif

ifdef PLATFORM_CFLAGS
LOCAL_CFLAGS += $(PLATFORM_CFLAGS)
endif

endif

include $(BUILD_SHARED_LIBRARY)

# ------------ #

include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
	gralloc.cpp

LOCAL_CPPFLAGS := -Wunused-variable
LOCAL_C_INCLUDES := \
	external/libdrm \
	external/libdrm/include/drm \
	hardware/libhardware/include \
	external/libdrm/include/drm \
	system/core/liblog/include

LOCAL_HEADER_LIBRARIES += \
	libutils_headers \
	liblog_headers \
	libhardware_headers \
	libcutils_headers

LOCAL_SHARED_LIBRARIES := \
	libgralloc_drm \
	liblog \
	libutils

# for glFlush/glFinish
LOCAL_SHARED_LIBRARIES += \
	libGLESv1_CM
LOCAL_CFLAGS +=-DRK_DRM_GRALLOC_DEBUG=0 -DMALI_AFBC_GRALLOC=1

ifeq ($(TARGET_USES_HWC2),true)
    LOCAL_CFLAGS += -DUSE_HWC2
endif

LOCAL_MODULE := gralloc.$(TARGET_BOARD_HARDWARE)
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw
include $(BUILD_SHARED_LIBRARY)

endif # DRM_GPU_DRIVERS

endif
