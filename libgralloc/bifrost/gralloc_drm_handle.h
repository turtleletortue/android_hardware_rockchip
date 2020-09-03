/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * 定义了表征 graphic_buffer_handle 的 类型 gralloc_drm_handle_t, 对应 arm_gralloc 中的 private_handle_t.
 */

#ifndef _GRALLOC_DRM_HANDLE_H_
#define _GRALLOC_DRM_HANDLE_H_

// #define ENABLE_DEBUG_LOG
#include <log/custom_log.h>

#include <cutils/native_handle.h>
#include <system/graphics.h>
#include <hardware/gralloc.h>

#include <errno.h>
#include <pthread.h>

#include "rk_drm_gralloc_config.h"
#include "mali_gralloc_usages.h"
#include "mali_gralloc_formats.h"
#include "mali_gralloc_private_interface_types.h"


#ifdef __cplusplus
extern "C" {
#endif

#define GRALLOC_UN_USED(arg)     (arg=arg)

typedef enum
{
	MALI_DPY_TYPE_UNKNOWN = 0,
	MALI_DPY_TYPE_CLCD,
	MALI_DPY_TYPE_HDLCD
} mali_dpy_type;

#if MALI_AFBC_GRALLOC != 1
#define MALI_AFBC_GRALLOC 1
#endif

struct gralloc_drm_bo_t;

/*
 * Maximum number of pixel format planes.
 * Plane [0]: Single plane formats (inc. RGB, YUV) and Y
 * Plane [1]: U/V, UV
 * Plane [2]: V/U
 */
#define MAX_PLANES 3

typedef struct plane_info {

	/*
	 * Offset to plane (in bytes),
	 * from the start of the allocation.
	 */
	uint32_t offset;

	/*
	 * Byte Stride: number of bytes between two vertically adjacent
	 * pixels in given plane. This can be mathematically described by:
	 *
	 * byte_stride = ALIGN((alloc_width * bpp)/8, alignment)
	 *
	 * where,
	 *
	 * alloc_width: width of plane in pixels (c.f. pixel_stride)
	 * bpp: average bits per pixel
	 * alignment (in bytes): dependent upon pixel format and usage
	 *
	 * For uncompressed allocations, byte_stride might contain additional
	 * padding beyond the alloc_width. For AFBC, alignment is zero.
	 */
	uint32_t byte_stride;

	/*
	 * Dimensions of plane (in pixels).
	 *
	 * For single plane formats, pixels equates to luma samples.
	 * For multi-plane formats, pixels equates to the number of sample sites
	 * for the corresponding plane, even if subsampled.
	 *
	 * AFBC compressed formats: requested width/height are rounded-up
	 * to a whole AFBC superblock/tile (next superblock at minimum).
	 * Uncompressed formats: dimensions typically match width and height
	 * but might require pixel stride alignment.
	 *
	 * See 'byte_stride' for relationship between byte_stride and alloc_width.
	 *
	 * Any crop rectangle defined by GRALLOC_ARM_BUFFER_ATTR_CROP_RECT must
	 * be wholly within the allocation dimensions. The crop region top-left
	 * will be relative to the start of allocation.
	 */
	uint32_t alloc_width;
	uint32_t alloc_height;
} plane_info_t;


/**
 * 对应 arm_gralloc 中的 private_handle_t.
 */
struct gralloc_drm_handle_t {
    /* 基类子对象. */
	native_handle_t base;

    /*-------------------------------------------------------*/

	/* file descriptors of the underlying dma_buf. */
	int prime_fd;

#if MALI_AFBC_GRALLOC == 1
    /**
     * 用于存储和 AFBC 有关的 attributes 的 shared_memory 的 fd.
     * 对该 buffer 的创建和访问的接口 定义在 gralloc_buffer_priv.h 中.
     */
	int     share_attr_fd;
#else
#error
#endif
#ifdef USE_HWC2
    /**
     * 用于存储和 rk 平台相关的 attributes 的 shared_memory 的 fd.
     * 由庄晓亮仿照 'share_attr_fd' 实现,
     * 对应 buffer 的具体类型是 rk_ashmem_t,
     *      具体定义在 义在 hardware/libhardware/include/hardware/gralloc.h 中.
     * 对该 buffer 的创建和访问的接口, 也定义在 gralloc_buffer_priv.h 中.
     */
	int ashmem_fd;
#endif

    /*-------------------------------------------------------*/
	/* integers */

	int magic;

	/*
	 * Input properties.
	 *
	 * width/height: Buffer dimensions.
	 * producer/consumer_usage: Buffer usage (indicates IP)
	 */
	int width;
	int height;
	int format;
	int usage;
    uint64_t consumer_usage;
    uint64_t producer_usage;

	/*
	 * DEPRECATED members.
	 * Equivalent information can be obtained from other fields:
	 *
	 * - 'internal_format' --> alloc_format
	 * - 'stride', 
     *      mali_so 实际上不引用本成员, 
     *      这里按照 rk_drm_gralloc 的传统, 其语义是 byte_stride.
     *      而在 arm_gralloc 中 private_handle_t::stride 是 pixel_stride.
     *
	 * - 'byte_stride' ~= plane_info[0].byte_stride
	 * - 'internalWidth' ~= plane_info[0].alloc_width
	 * - 'internalHeight' ~= plane_info[0].alloc_height
	 *
	 * '~=' (approximately equal) is used because the fields were either previously
	 * incorrectly populated by gralloc or the meaning has slightly changed.
	 *
	 * NOTE: 'stride' values sometimes vary significantly from plane_info[0].alloc_width.
	 */
    uint64_t   internal_format;
	int stride;
    int        byte_stride;
    int        internalWidth;
    int        internalHeight;

	/*
	 * Allocation properties.
	 *
	 * alloc_format: Pixel format (base + modifiers). NOTE: base might differ from requested
	 *               format (req_format) where fallback to single-plane format was required.
	 * plane_info:   Per plane allocation information.
	 * size:         Total bytes allocated for buffer (inc. all planes, layers. etc.).
	 * layer_count:  Number of layers allocated to buffer.
	 *               All layers are the same size (in bytes).
	 *               Multi-layers supported in v1.0, where GRALLOC1_CAPABILITY_LAYERED_BUFFERS is enabled.
	 *               Layer size: 'size' / 'layer_count'.
	 *               Layer (n) offset: n * ('size' / 'layer_count'), n=0 for the first layer.
	 *
	 */
	uint64_t alloc_format;
	plane_info_t plane_info[MAX_PLANES];
    int        size;
	uint32_t layer_count;

    mali_dpy_type dpy_type;
    int        ref;
    int        pixel_stride;

    union {
        off_t    offset;
        uint64_t padding4;
    };

#if MALI_AFBC_GRALLOC == 1
	// locally mapped shared attribute area
	union {
		void*    attr_base;
		uint64_t padding3;
	};
#endif

#ifdef USE_HWC2
	union {
		void*	 ashmem_base;
		uint64_t padding5;
	};
#endif

	/*
	 * Deprecated.
	 * Use GRALLOC_ARM_BUFFER_ATTR_DATASPACE
	 * instead.
	 */
	mali_gralloc_yuv_info yuv_info;

	int name;   /* the name of the bo */
	uint32_t phy_addr;
	uint32_t reserve0;
	uint32_t reserve1;
	uint32_t reserve2;

    /* 表征 'this' 在当前进程中的 buffer_object. */
	struct gralloc_drm_bo_t *data; /* pointer to struct gralloc_drm_bo_t */

	// FIXME: the attributes below should be out-of-line
	uint64_t unknown __attribute__((aligned(8)));

	int data_owner; /* owner of data (for validation) */
        // value 是 pid, buffer 被 alloc 的时候 首次有效设置.

    /*-------------------------------------------------------*/

	bool is_multi_plane() const
	{
		/* For multi-plane, the byte stride for the second plane will always be non-zero. */
		return (plane_info[1].byte_stride != 0);
	}
};

/**
 * gralloc_drm_handle_t::magic 的固定取值.
 *
 * @see create_bo_handle().
 */
#define GRALLOC_DRM_HANDLE_MAGIC 0x12345678
#ifdef USE_HWC2
#if MALI_AFBC_GRALLOC == 1
#define GRALLOC_DRM_HANDLE_NUM_FDS 3
#else
#define GRALLOC_DRM_HANDLE_NUM_FDS 2
#endif

#else
#if MALI_AFBC_GRALLOC == 1
#define GRALLOC_DRM_HANDLE_NUM_FDS 2
#else
#define GRALLOC_DRM_HANDLE_NUM_FDS 1
#endif

#endif

#define GRALLOC_DRM_HANDLE_NUM_INTS (						\
	((sizeof(struct gralloc_drm_handle_t) - sizeof(native_handle_t))/sizeof(int))	\
	 - GRALLOC_DRM_HANDLE_NUM_FDS)
enum
{
       /* Buffer won't be allocated as AFBC */
       GRALLOC_ARM_USAGE_NO_AFBC = GRALLOC_USAGE_PRIVATE_1 | GRALLOC_USAGE_PRIVATE_2
};

static pthread_mutex_t handle_mutex = PTHREAD_MUTEX_INITIALIZER;

// .R : "buffer_handle_t" : ./include/system/window.h:60:typedef const native_handle_t* buffer_handle_t;
static inline struct gralloc_drm_handle_t *gralloc_drm_handle(buffer_handle_t _handle)
{
	struct gralloc_drm_handle_t *handle = (struct gralloc_drm_handle_t *) _handle;

	pthread_mutex_lock(&handle_mutex);
	if(handle)
	{
		handle->ref++;
	}

	if (handle && (handle->base.version != sizeof(handle->base) ||
		handle->base.numInts != GRALLOC_DRM_HANDLE_NUM_INTS ||
		handle->base.numFds != GRALLOC_DRM_HANDLE_NUM_FDS ||
		handle->magic != GRALLOC_DRM_HANDLE_MAGIC)) {
		ALOGE("invalid handle: version=%d, numInts=%d, numFds=%d, magic=%x",
				handle->base.version, handle->base.numInts,
				handle->base.numFds, handle->magic);
		ALOGE("invalid handle: right version=%zu, numInts=%zu, numFds=%d, magic=%x",
				sizeof(handle->base),GRALLOC_DRM_HANDLE_NUM_INTS,GRALLOC_DRM_HANDLE_NUM_FDS,
				GRALLOC_DRM_HANDLE_MAGIC);
		handle = NULL;
	}
	pthread_mutex_unlock(&handle_mutex);
	return handle;
}

static inline int gralloc_drm_validate_handle(const native_handle *h)
{
	struct gralloc_drm_handle_t *handle = (struct gralloc_drm_handle_t *)h;

    if ( NULL == handle )
    {
        E("'handle' is NULL.")
        return -EINVAL;
    }
    else if ( handle->base.version != sizeof(handle->base) )
    {
        E("unexpected 'base.version' : %d; size of 'base' : %zu.", handle->base.version, sizeof(handle->base) );
        return -EINVAL;
    }
    else if ( handle->base.numInts != GRALLOC_DRM_HANDLE_NUM_INTS )
    {
        E("unexpected 'base.numInts' : %d; expect %zu.", handle->base.numInts, GRALLOC_DRM_HANDLE_NUM_INTS);
        return -EINVAL;
    }
    else if ( handle->base.numFds != GRALLOC_DRM_HANDLE_NUM_FDS )
    {
        E("unexpected 'base.numFds' : %d; expect %d.", handle->base.numFds, GRALLOC_DRM_HANDLE_NUM_FDS);
        return -EINVAL;
    }
    else if ( handle->magic != GRALLOC_DRM_HANDLE_MAGIC )
    {
        E("unexpected 'magic' : 0x%x; expect 0x%x.", handle->magic, GRALLOC_DRM_HANDLE_MAGIC);
        return -EINVAL;
    }

    return 0;
}

static inline void gralloc_drm_unlock_handle(buffer_handle_t _handle)
{
	struct gralloc_drm_handle_t *handle = (struct gralloc_drm_handle_t *) _handle;

	pthread_mutex_lock(&handle_mutex);
	if(handle)
	{
		handle->ref--;
	}
	pthread_mutex_unlock(&handle_mutex);
}

#ifdef __cplusplus
}
#endif
#endif /* _GRALLOC_DRM_HANDLE_H_ */
