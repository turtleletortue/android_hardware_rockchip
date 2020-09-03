/*
 * Copyright (C) 2016-2019 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * @file mali_gralloc_formats.cpp : 
 *          来自 arm_gralloc, 仅保留少量内容.
 */

#include <string.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <log/log.h>
#include <assert.h>
#include <vector>

#if GRALLOC_VERSION_MAJOR == 1
#include <hardware/gralloc1.h>
#elif GRALLOC_VERSION_MAJOR == 0
#include <hardware/gralloc.h>
#endif

#include "mali_gralloc_bufferallocation.h"
#include "format_info.h"
#include "gralloc_helper.h"

#if GRALLOC_USE_LEGACY_CALCS == 1
#include "legacy/buffer_alloc.h"
#endif


#if GRALLOC_USE_LEGACY_CALCS == 1
namespace legacy
{

// 相对 arm_gralloc 中的原始版本有简化.
void mali_gralloc_adjust_dimensions(const uint64_t internal_format,
                                    const uint64_t usage,
                                    const alloc_type_t type,
                                    const uint32_t width,
                                    const uint32_t height,
                                    int * const internal_width,
                                    int * const internal_height)
{
	/*
	 * Default: define internal dimensions the same as public.
	 */
	*internal_width = width;
	*internal_height = height;

	get_afbc_alignment(*internal_width, *internal_height, type,
	                   internal_width, internal_height);

	ALOGV("%s: internal_format=0x%" PRIx64 " usage=0x%" PRIx64
	      " width=%u, height=%u, internal_width=%d, internal_height=%d",
	      __FUNCTION__, internal_format, usage, width, height, *internal_width, *internal_height);
}

}
#endif /* end of legacy */


/*
 * Update buffer dimensions for producer/consumer constraints. This process is
 * not valid with CPU producer/consumer since the new resolution cannot be
 * communicated to generic clients through the public APIs. Adjustments are
 * likely to be related to AFBC.
 *
 * @param alloc_format   [in]    Format (inc. modifiers) to be allocated.
 * @param usage          [in]    Buffer usage.
 * @param width          [inout] Buffer width (in pixels).
 * @param height         [inout] Buffer height (in pixels).
 *
 * @return none.
 *
 * 相对 arm_gralloc 中的原始版本有简化.
 */
void mali_gralloc_adjust_dimensions(const uint64_t alloc_format,
                                    const uint64_t usage,
                                    int* const width,
                                    int* const height)
{
	// if (producers & MALI_GRALLOC_PRODUCER_GPU)
	{
		/* Pad all AFBC allocations to multiple of GPU tile size. */
		if (alloc_format & MALI_GRALLOC_INTFMT_AFBC_BASIC)
		{
			*width = GRALLOC_ALIGN(*width, 16);
			*height = GRALLOC_ALIGN(*height, 16);
		}
	}

	ALOGV("%s: alloc_format=0x%" PRIx64 " usage=0x%" PRIx64
	      " alloc_width=%u, alloc_height=%u",
	      __FUNCTION__, alloc_format, usage, *width, *height);
}

/*
 * Determines whether a base format is subsampled YUV, where each
 * chroma channel has fewer samples than the luma channel. The
 * sub-sampling is always a power of 2.
 *
 * @param base_format   [in]    Base format (internal).
 *
 * @return 1, where format is subsampled YUV;
 *         0, otherwise
 */
bool is_subsampled_yuv(const uint32_t base_format)
{
	unsigned long i;

	for (i = 0; i < num_formats; i++)
	{
		if (formats[i].id == (base_format & MALI_GRALLOC_INTFMT_FMT_MASK))
		{
			if (formats[i].is_yuv == true &&
			    (formats[i].hsub > 1 || formats[i].vsub > 1))
			{
				return true;
			}
		}
	}
	return false;
}

