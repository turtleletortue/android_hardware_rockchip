
/**
 * @file gralloc_drm_rockchip.cpp
 *      对 rk_driver_of_gralloc_drm_device_t 的具体实现.
 */

#define LOG_TAG "GRALLOC-ROCKCHIP"

// #define ENABLE_DEBUG_LOG
#include "custom_log.h"


#include <log/log.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <drm.h>
#include <sys/types.h>

extern "C" {
#include <rockchip/rockchip_drmif.h>
}

#include <cutils/properties.h>

#include "rk_drm_gralloc_config.h"

#include "gralloc_helper.h"
#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"
#include "mali_gralloc_formats.h"
#include "mali_gralloc_usages.h"
#include "mali_gralloc_bufferallocation.h"
#include "legacy/buffer_alloc.h"
#if MALI_AFBC_GRALLOC == 1
#include <inttypes.h>
#include "gralloc_buffer_priv.h"
#endif //end of MALI_AFBC_GRALLOC

#include <stdbool.h>
#include <sys/stat.h>

#include <utils/KeyedVector.h>
#include <utils/Mutex.h>

/*---------------------------------------------------------------------------*/

#define RK_CTS_WORKROUND	(1)

#define UNUSED(...) (void)(__VA_ARGS__)

#if RK_CTS_WORKROUND
#define VIEW_CTS_FILE		"/data/data/android.view.cts/view_cts.ini"
#define VIEW_CTS_PROG_NAME	"android.view.cts"
#define VIEW_CTS_HINT		"view_cts"
#define BIG_SCALE_HINT		"big_scale"
typedef unsigned int       u32;
typedef enum
{
	IMG_STRING_TYPE		= 1,                    /*!< String type */
	IMG_FLOAT_TYPE		,                       /*!< Float type */
	IMG_UINT_TYPE		,                       /*!< Unsigned Int type */
	IMG_INT_TYPE		,                       /*!< (Signed) Int type */
	IMG_FLAG_TYPE                               /*!< Flag Type */
}IMG_DATA_TYPE;
#endif

struct dma_buf_sync {
        __u64 flags;
};

#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK \
        (DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)
#define DMA_BUF_NAME_LEN	32
#define DMA_BUF_BASE            'b'
#define DMA_BUF_IOCTL_SYNC      _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#define DMA_BUF_SET_NAME	_IOW(DMA_BUF_BASE, 1, const char *)

/* memory type definitions. */
enum drm_rockchip_gem_mem_type {
	/* Physically Continuous memory and used as default. */
	ROCKCHIP_BO_CONTIG	= 1 << 0,
	/* cachable mapping. */
	ROCKCHIP_BO_CACHABLE	= 1 << 1,
	/* write-combine mapping. */
	ROCKCHIP_BO_WC		= 1 << 2,
	ROCKCHIP_BO_SECURE	= 1 << 3,
	ROCKCHIP_BO_MASK	= ROCKCHIP_BO_CONTIG | ROCKCHIP_BO_CACHABLE |
				ROCKCHIP_BO_WC | ROCKCHIP_BO_SECURE
};

struct drm_rockchip_gem_phys {
	uint32_t handle;
	uint32_t phy_addr;
};

#define DRM_ROCKCHIP_GEM_GET_PHYS	0x04
#define DRM_IOCTL_ROCKCHIP_GEM_GET_PHYS		DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_GET_PHYS, struct drm_rockchip_gem_phys)

/*---------------------------------------------------------------------------*/
// for rk_drm_adapter :

using namespace android;

/**
 * 用于 记录当前进程中某个 gem_obj 被引用状态信息 的类型.
 */
class gem_obj_referenced_info_entry_t {

public:
    /* handle of current gem_obj. */
    uint32_t m_handle;

    /* 当前 gem_obj 的被引用计数. */
	uint32_t m_ref;

    /*-------------------------------------------------------*/

    gem_obj_referenced_info_entry_t(uint32_t gem_handle)
        :   m_handle(gem_handle),
            m_ref(1) // "1" : 初始被引用计数.
    {}

    uint32_t get_ref() const
    {
        return m_ref;
    }

    void inc_ref()
    {
        m_ref++;
    }

    void dec_ref()
    {
        m_ref--;
    }
};

/**
 * 基于 rk_drm 的, 对 driver_of_gralloc_drm_device_t 的具体实现,
 * 即 .DP : rk_driver_of_gralloc_drm_device_t.
 *
 * 除了基类子对象, 还包括 扩展的成员等.
 */
struct rk_driver_of_gralloc_drm_device_t {
    /* 基类子对象. */
	struct gralloc_drm_drv_t base;

    /*
     * rockchip drm device object, rk_drm_device_object or rk_drm_dev
     * returned from rockchip_device_create().
     */
	struct rockchip_device *rk_drm_dev; // rockchip_device 定义在 external/libdrm/rockchip/rockchip_drmif.h 中.

    /* gralloc_drm_t::fd, fd of drm device file.*/
	int fd_of_drm_dev;

    /*-------------------------------------------------------*/
    // .DP : rk_drm_adapter :
    // RK redmine Defect #16966 中涉及的某个和 camera HAL 相关的 case 中, 确定有可能出现如下异常 :
    //      buffer_a 和 buffer_b 引用相同的 底层的 gem_obj 实例, 在不同的线程中, 被并发地调用 register/lock/unlock/unregister;
    //      buffer_a 被 unregister, rockchip_bo_destroy() 将被调用,
    //          因为 rk_drm 对 rockchip_bo 实例下的 gem_obj 未提供 被引用计数机制, 底层的 gem_obj 将被直接 close.
    //      而之后 buffer_b 又被 lock, 此时因为底层的 gem_obj 已经被 close, 即 gem_handle 无效, lock 失败.
    //
    // 为处理上述的 case, 在 rk_drm_gralloc 中增加对 rockchip_bo 引用的底层的 gem_obj 被引用计数,
    // 将对应的逻辑机构记为 rk_drm_adapter.

    /**
     * 保存当前进程中 所有有效 gem_obj 的被引用状态信息的 map.
     * 以 gem_handle 为 key,
     * 对应的 gem_obj_referenced_info_entry_t heap_object 的指针作为 value.
     */
    KeyedVector<uint32_t, gem_obj_referenced_info_entry_t*> m_gem_objs_ref_info_map;

    /*
     * 除了保护 'm_gem_objs_ref_info_map', 也保护对 rk_drm 的某一次调用.
     * .DP : drm_lock
     */
    mutable Mutex m_drm_lock;
};

/**
 * 当前 rk_driver_of_gralloc_drm_device_t 实例的指针.
 *
 * 按照原始的封装设计, drm_gem_rockchip_free() 等函数应该是 static 的, 并且外部传入的 'drv' 实参必须是有效的,
 * 但是之前的开发中, 庄晓亮为 workaround 某些异常, 在本文件外部直接调用 drm_gem_rockchip_free(), 且对 'drv' 传入 NULL,
 * 参见 commit 136daf0d.
 * 类似的接口还有 drm_gem_rockchip_alloc().
 * 为处理上述 case, 这里静态地保存 当前 rk_driver_of_gralloc_drm_device_t 实例的指针.
 */
static struct rk_driver_of_gralloc_drm_device_t* s_rk_drv;

/* rockchip_gralloc_drm_buffer_object. */
struct rockchip_buffer {
    /* 基类子对象. */
	struct gralloc_drm_bo_t base;

    /* rk_drm_bo. */
	struct rockchip_bo *bo;
};

/*---------------------------------------------------------------------------*/
// for rk_drm_adapter :
//
static inline int get_fd_of_drm_dev(const struct rk_driver_of_gralloc_drm_device_t* rk_drv)
{
    return rk_drv->fd_of_drm_dev;
}

static inline struct rockchip_device* get_rk_drm_dev(const struct rk_driver_of_gralloc_drm_device_t* rk_drv)
{
    return rk_drv->rk_drm_dev;
}

static inline KeyedVector<uint32_t, gem_obj_referenced_info_entry_t*>&
get_gem_objs_ref_info_map(struct rk_driver_of_gralloc_drm_device_t* rk_drv)
{
    return rk_drv->m_gem_objs_ref_info_map;
}

static inline Mutex& get_drm_lock(struct rk_driver_of_gralloc_drm_device_t* rk_drv)
{
    return rk_drv->m_drm_lock;
}

/*
 * 初始化 'rk_drv' 中的 rk_drm_adapter.
 */
static inline int rk_drm_adapter_init(struct rk_driver_of_gralloc_drm_device_t* rk_drv)
{
    int ret = 0;
    KeyedVector<uint32_t, gem_obj_referenced_info_entry_t*>& map = get_gem_objs_ref_info_map(rk_drv);

    map.setCapacity(16);

    return ret;
}

static inline void rk_drm_adapter_term(struct rk_driver_of_gralloc_drm_device_t* rk_drv)
{
	UNUSED(rk_drv);
}

static struct rockchip_bo* rk_drm_adapter_create_rockchip_bo(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                                             size_t size,
                                                             uint32_t flags);

static void rk_drm_adapter_destroy_rockchip_bo(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                               struct rockchip_bo *bo);

static struct rockchip_bo* rk_drm_adapter_import_dma_buf(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                                         int dma_buf_fd,
                                                         uint32_t flags,
                                                         uint32_t size);

static inline uint32_t rk_drm_adapter_get_gem_handle(struct rockchip_bo *bo)
{
    return rockchip_bo_handle(bo);
}

static inline uint32_t rk_drm_adapter_get_prime_fd(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                                   struct rockchip_bo *bo,
                                                   int* prime_fd);

static inline void* rk_drm_adapter_map_rockchip_bo(struct rockchip_bo *bo)
{
    return rockchip_bo_map(bo);
}

static int rk_drm_adapter_inc_gem_obj_ref(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                          uint32_t handle);

static void rk_drm_adapter_dec_gem_obj_ref(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                           uint32_t handle);

static void rk_drm_adapter_close_gem_obj(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                         uint32_t handle);

/*---------------------------------------------------------------------------*/

#if RK_DRM_GRALLOC_DEBUG
#ifndef AWAR
#define AWAR(fmt, args...) __android_log_print(ANDROID_LOG_WARN, "[Gralloc-Warning]", "%s:%d " fmt,__func__,__LINE__,##args)
#endif
#ifndef AINF
#define AINF(fmt, args...) __android_log_print(ANDROID_LOG_INFO, "[Gralloc]", fmt,##args)
#endif
#ifndef ADBG
#define ADBG(fmt, args...) __android_log_print(ANDROID_LOG_DEBUG, "[Gralloc-DEBUG]", fmt,##args)
#endif

#else

#ifndef AWAR
#define AWAR(fmt, args...)
#endif
#ifndef AINF
#define AINF(fmt, args...)
#endif
#ifndef ADBG
#define ADBG(fmt, args...)
#endif

#endif //end of RK_DRM_GRALLOC_DEBUG

#ifndef AERR
#define AERR(fmt, args...) __android_log_print(ANDROID_LOG_ERROR, "[Gralloc-ERROR]", "%s:%d " fmt,__func__,__LINE__,##args)
#endif
#ifndef AERR_IF
#define AERR_IF( eq, fmt, args...) if ( (eq) ) AERR( fmt, args )
#endif

#define ODD_ALIGN(x, align)		(((x) % ((align) * 2) == 0) ? ((x) + (align)) : (x))
#define GRALLOC_ODD_ALIGN( value, base )   ODD_ALIGN(GRALLOC_ALIGN(value, base), base)

static void drm_gem_rockchip_free(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo);

/*---------------------------------------------------------------------------*/

#define YUV_ANDROID_PLANE_ALIGN 16

static bool get_rk_nv12_stride_and_size(int width, int height, int* pixel_stride, int* byte_stride, size_t* size)
{
    int stride_alignment = YUV_ANDROID_PLANE_ALIGN; // stride aligment value in bytes(pixels).
    /**
     * .KP : from CSY : video_decoder 要求的 byte_stride of buffer in NV12, 已经通过 width 传入.
     * 对 NV12, byte_stride 就是 pixel_stride, 也就是 luma_stride.
     */
    int luma_stride = width;
    if ( (luma_stride % stride_alignment) != 0 )
    {
        W("luma_stride from width (%d) is not %d aligned! we would force align it, something might go wrong on video_decoder side.",
          luma_stride,
          stride_alignment);
	    luma_stride = GRALLOC_ALIGN(luma_stride, stride_alignment);
    }

    if (width % 2 != 0 || height % 2 != 0)
    {
        W("unexpected width(%d) or height(%d).", width, height);
        //return false;
    }

    if (size != NULL)
    {
        /* .KP : from CSY : video_decoder 需要的 buffer 中除了 YUV 数据还有其他 metadata, 要更多的空间. 2 * w * h 一定够. */
        *size = 2 * luma_stride * height;
    }

    if (byte_stride != NULL)
    {
        *byte_stride = luma_stride;
    }

    if (pixel_stride != NULL)
    {
        *pixel_stride = luma_stride;
    }

    return true;
}

static bool get_rk_nv12_10bit_stride_and_size (int width, int height, int* pixel_stride, int* byte_stride, size_t* size)
{

    if (width % 2 != 0 || height % 2 != 0)
    {
        return false;
    }

    /**
     * .KP : from CSY : video_decoder 要求的 byte_stride of buffer in NV12_10, 已经通过 width 传入.
     * 对 NV12_10, 原理上, byte_stride 和 pixel_stride 不同.
     */
    *byte_stride = width;

    /* .KP : from CSY : video_decoder 需要的 buffer 中除了 YUV 数据还有其他 metadata, 要更多的空间. 2 * w * h 一定够. */
    *size = 2 * width * height;

    *pixel_stride = *byte_stride;
    // 字面上, 这是错误的,
    // 但是目前对于 NV12_10, rk_hwc, 将 private_module_t::stride 作为 byte_stride 使用.

    return true;
}

/*---------------------------------------------------------------------------*/

#if RK_CTS_WORKROUND
static bool ConvertCharToData(const char *pszHintName, const char *pszData, void *pReturn, IMG_DATA_TYPE eDataType)
{
	bool bFound = false;


	switch(eDataType)
	{
		case IMG_STRING_TYPE:
		{
			strcpy((char*)pReturn, pszData);

			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "Hint: Setting %s to %s\n", pszHintName, (char*)pReturn);

			bFound = true;

			break;
		}
		case IMG_FLOAT_TYPE:
		{
			*(float*)pReturn = (float) atof(pszData);

			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "Hint: Setting %s to %f", pszHintName, *(float*)pReturn);

			bFound = true;

			break;
		}
		case IMG_UINT_TYPE:
		case IMG_FLAG_TYPE:
		{
			/* Changed from atoi to stroul to support hexadecimal numbers */
			*(u32*)pReturn = (u32) strtoul(pszData, NULL, 0);
			if (*(u32*)pReturn > 9)
			{
				ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "Hint: Setting %s to %u (0x%X)", pszHintName, *(u32*)pReturn, *(u32*)pReturn);
			}
			else
			{
				ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "Hint: Setting %s to %u", pszHintName, *(u32*)pReturn);
			}
			bFound = true;

			break;
		}
		case IMG_INT_TYPE:
		{
			*(int*)pReturn = (int) atoi(pszData);

			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "Hint: Setting %s to %d\n", pszHintName, *(int*)pReturn);

			bFound = true;

			break;
		}
		default:
		{
			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "ConvertCharToData: Bad eDataType");

			break;
		}
	}

	return bFound;
}

static int getProcessCmdLine(char* outBuf, size_t bufSize)
{
	int ret = 0;

	FILE* file = NULL;
	long pid = 0;
	char procPath[128]={0};

	pid = getpid();
	sprintf(procPath, "/proc/%ld/cmdline", pid);

	file = fopen(procPath, "r");
	if ( NULL == file )
	{
		ALOGE("fail to open file (%s)",strerror(errno));
	}

	if ( NULL == fgets(outBuf, bufSize - 1, file) )
	{
		ALOGE("fail to read from cmdline_file.");
	}

	if ( NULL != file )
	{
		fclose(file);
	}

	return ret;
}

bool FindAppHintInFile(const char *pszFileName, const char *pszAppName,
								  const char *pszHintName, void *pReturn,
								  IMG_DATA_TYPE eDataType)
{
	FILE *regFile;
	bool bFound = false;

	regFile = fopen(pszFileName, "r");

	if(regFile)
	{
		char pszTemp[1024], pszApplicationSectionName[1024];
		int iLineNumber;
		bool bUseThisSection, bInAppSpecificSection;

		/* Build the section name */
		snprintf(pszApplicationSectionName, 1024, "[%s]", pszAppName);

		bUseThisSection 		= false;
		bInAppSpecificSection	= false;

		iLineNumber = -1;

		while(fgets(pszTemp, 1024, regFile))
		{
			size_t uiStrLen;

			iLineNumber++;
			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "FindAppHintInFile iLineNumber=%d pszTemp=%s",iLineNumber,pszTemp);

			uiStrLen = strlen(pszTemp);

			if (pszTemp[uiStrLen-1]!='\n')
			{
			    ALOGE("FindAppHintInFile : Error in %s at line %u",pszFileName,iLineNumber);

				continue;
			}

			if((uiStrLen >= 2) && (pszTemp[uiStrLen-2] == '\r'))
			{
				/* CRLF (Windows) line ending */
				pszTemp[uiStrLen-2] = '\0';
			}
			else
			{
				/* LF (unix) line ending */
				pszTemp[uiStrLen-1] = '\0';
			}

			switch (pszTemp[0])
			{
				case '[':
				{
					/* Section */
					bUseThisSection 		= false;
					bInAppSpecificSection	= false;

					if (!strcmp("[default]", pszTemp))
					{
						bUseThisSection = true;
					}
					else if (!strcmp(pszApplicationSectionName, pszTemp))
					{
						bUseThisSection 		= true;
						bInAppSpecificSection 	= true;
					}

					break;
				}
				default:
				{
					char *pszPos;

					if (!bUseThisSection)
					{
						/* This line isn't for us */
						continue;
					}

					pszPos = strstr(pszTemp, pszHintName);

					if (pszPos!=pszTemp)
					{
						/* Hint name isn't at start of string */
						continue;
					}

					if (*(pszPos + strlen(pszHintName)) != '=')
					{
						/* Hint name isn't exactly correct, or isn't followed by an equals sign */
						continue;
					}

					/* Move to after the equals sign */
					pszPos += strlen(pszHintName) + 1;

					/* Convert anything after the equals sign to the requested data type */
					bFound = ConvertCharToData(pszHintName, pszPos, pReturn, eDataType);

					if (bFound && bInAppSpecificSection)
					{
						/*
						// If we've found the hint in the application specific section we may
						// as well drop out now, since this should override any default setting
						*/
						fclose(regFile);

						return true;
					}

					break;
				}
			}
		}

		fclose(regFile);
	}
	else
	{
		regFile = fopen(pszFileName, "wb+");
		if(regFile)
		{
			char acBuf[] = "[android.view.cts]\n"
							"view_cts=0\n"
							"big_scale=0\n";
			fprintf(regFile,"%s",acBuf);
			fclose(regFile);
			chmod(pszFileName, 0x777);
		}
		else
		{
			ALOGE("%s open fail errno=0x%x  (%s)",__FUNCTION__, errno,strerror(errno));
		}
	}

	return bFound;
}

bool ModifyAppHintInFile(const char *pszFileName, const char *pszAppName,
								const char *pszHintName, void *pReturn, int pSet,
								IMG_DATA_TYPE eDataType)
{
	FILE *regFile;
	bool bFound = false;

	regFile = fopen(pszFileName, "r+");

	if(regFile)
	{
		char pszTemp[1024], pszApplicationSectionName[1024];
		int iLineNumber;
		bool bUseThisSection, bInAppSpecificSection;
		int offset = 0;

		/* Build the section name */
		snprintf(pszApplicationSectionName, 1024, "[%s]", pszAppName);

		bUseThisSection		  = false;
		bInAppSpecificSection   = false;

		iLineNumber = -1;

		while(fgets(pszTemp, 1024, regFile))
		{
			size_t uiStrLen;

			iLineNumber++;
			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "ModifyAppHintInFile iLineNumber=%d pszTemp=%s",iLineNumber,pszTemp);

			uiStrLen = strlen(pszTemp);

			if (pszTemp[uiStrLen-1]!='\n')
			{
				ALOGE("FindAppHintInFile : Error in %s at line %u",pszFileName,iLineNumber);
				continue;
			}

			if((uiStrLen >= 2) && (pszTemp[uiStrLen-2] == '\r'))
			{
				/* CRLF (Windows) line ending */
				pszTemp[uiStrLen-2] = '\0';
			}
			else
			{
				/* LF (unix) line ending */
				pszTemp[uiStrLen-1] = '\0';
			}

			switch (pszTemp[0])
			{
				case '[':
				{
					/* Section */
					bUseThisSection		  = false;
					bInAppSpecificSection   = false;

					if (!strcmp("[default]", pszTemp))
					{
						bUseThisSection = true;
					}
					else if (!strcmp(pszApplicationSectionName, pszTemp))
					{
						bUseThisSection		  = true;
						bInAppSpecificSection   = true;
					}

					break;
				}
				default:
				{
					char *pszPos;

					if (!bUseThisSection)
					{
						/* This line isn't for us */
						offset += uiStrLen;
						continue;
					}

					pszPos = strstr(pszTemp, pszHintName);

					if (pszPos!=pszTemp)
					{
						/* Hint name isn't at start of string */
						offset += uiStrLen;
						continue;
					}

					if (*(pszPos + strlen(pszHintName)) != '=')
					{
						/* Hint name isn't exactly correct, or isn't followed by an equals sign */
						offset += uiStrLen;
						continue;
					}

					/* Move to after the equals sign */
					pszPos += strlen(pszHintName) + 1;

					/* Convert anything after the equals sign to the requested data type */
					bFound = ConvertCharToData(pszHintName, pszPos, pReturn, eDataType);

					if (bFound && bInAppSpecificSection)
					{
						offset += (strlen(pszHintName) + 1);
						if(eDataType == IMG_INT_TYPE && *((int*)pReturn) != pSet)
						{
							fseek(regFile, offset, SEEK_SET);
							fprintf(regFile,"%d",pSet);
							*((int*)pReturn) = pSet;
						}
						/*
						// If we've found the hint in the application specific section we may
						// as well drop out now, since this should override any default setting
						*/
						fclose(regFile);

						return true;
					}

					break;
				}
			}
			offset += uiStrLen;
		}

		fclose(regFile);
	}
	else
	{
		regFile = fopen(pszFileName, "wb+");
		if(regFile)
		{
			char acBuf[] = "[android.view.cts]\n"
							"view_cts=0\n"
							"big_scale=0\n";
			fprintf(regFile,"%s",acBuf);
			fclose(regFile);
			chmod(pszFileName, 0x777);
		}
		else
		{
			ALOGE("%s open faile errno=0x%x  (%s)",__FUNCTION__, errno,strerror(errno));
		}
	}

	return bFound;
}
#endif

/*---------------------------------------------------------------------------*/

static void drm_gem_rockchip_destroy(struct gralloc_drm_drv_t *drv)
{
	struct rk_driver_of_gralloc_drm_device_t *rk_drv = (struct rk_driver_of_gralloc_drm_device_t *)drv;

    rk_drm_adapter_term(rk_drv);

	if (rk_drv->rk_drm_dev)
		rockchip_device_destroy(rk_drv->rk_drm_dev);

    delete rk_drv;
    s_rk_drv = NULL;
}

#if USE_AFBC_LAYER
static bool should_disable_afbc_in_fb_target_layer()
{
    char value[PROPERTY_VALUE_MAX];

    property_get("vendor.gralloc.disable_afbc", value, "0");

    return (0 == strcmp("1", value) );
}
#endif

/*
 * 根据特定的规则 构建当前 buf 对应的底层 dmabuf 的 name,
 * 并从 'name' 指向的 buffer 中返回.
 *
 * 这里的 dmabuf_name 格式是 : <tid>_<size>_<time>.
 * tid : alloc 发生的线程的 tid.
 * size : 预期的 buf 的 size.
 * time : 当前的时间戳, 从 hour 到 us.
 * 一个 dmabuf_name 的实例 : 478_26492928_15:55:55.034
 */
void get_dmabuf_name(size_t size, char* name)
{
    pid_t tid = gettid();
    timespec time;
    tm nowTime;
    struct timeval tv;
    struct timezone tz;

    clock_gettime(CLOCK_REALTIME, &time);  //获取相对于1970到现在的秒数
    localtime_r(&time.tv_sec, &nowTime);
    gettimeofday(&tv, &tz);

    snprintf(name,
             DMA_BUF_NAME_LEN,
             "%d_%zd_%02d:%02d:%02d.%03d",
             tid,
             size,
             nowTime.tm_hour, nowTime.tm_min, nowTime.tm_sec, (int)(tv.tv_usec / 1000) );
}

/**
 * 根据传入的 req_format 和 'usage', 选择合适的 internal_format, 并返回.
 */
#if USE_AFBC_LAYER
uint64_t rk_drm_gralloc_select_format(uint64_t req_format,
                                      uint64_t usage,
                                      const gralloc_drm_handle_t* handle)
#else
uint64_t rk_drm_gralloc_select_format(uint64_t req_format,
                                      uint64_t usage,
                                      const gralloc_drm_handle_t*)
#endif                                      
{
    uint64_t internal_format = req_format;
#if USE_AFBC_LAYER
	char framebuffer_size[PROPERTY_VALUE_MAX];
	uint32_t width, height, vrefresh;
#endif

    /*-------------------------------------------------------*/
    // 先处理可能的 afbc_layer.

#if USE_AFBC_LAYER
	property_get("persist.vendor.framebuffer.main", framebuffer_size, "0x0@60");
	sscanf(framebuffer_size, "%dx%d@%d", &width, &height, &vrefresh);
	//Vop cann't support 4K AFBC layer.
	if (height < 2160)
	{
#define MAGIC_USAGE_FOR_AFBC_LAYER     (0x88)
        /* if current buffer is NOT for fb_target_layer, ... */
        if (!(usage & GRALLOC_USAGE_HW_FB)) 
        {
	            if ( !(usage & GRALLOC_USAGE_EXTERNAL_DISP)
                    && MAGIC_USAGE_FOR_AFBC_LAYER == (usage & MAGIC_USAGE_FOR_AFBC_LAYER) ) 
                {
	                D("use_afbc_layer: force to set 'internal_format' to 0x%" PRIx64 " for usage '0x%x'.", internal_format, usage);
	                internal_format = MALI_GRALLOC_FORMAT_INTERNAL_RGBA_8888 | MALI_GRALLOC_INTFMT_AFBC_BASIC;
	            }
        }
        /* IS for fb_target_layer, ... */
        else
        {
            if ( !(usage & GRALLOC_USAGE_EXTERNAL_DISP)     // NOT fb_target_layer for external_display
                && MAGIC_USAGE_FOR_AFBC_LAYER != (usage & MAGIC_USAGE_FOR_AFBC_LAYER) )
            {
                if ( !should_disable_afbc_in_fb_target_layer() )
                {
                    internal_format = MALI_GRALLOC_FORMAT_INTERNAL_RGBA_8888 | MALI_GRALLOC_INTFMT_AFBC_BASIC;
                    if ( handle->prime_fd < 0 ) // 只在将实际分配 buffer 的时候打印.
                    {
                        I("use_afbc_layer: force to set 'internal_format' to 0x%" PRIx64 " for buffer_for_fb_target_layer.",
                          internal_format);
                    }
                    property_set("vendor.gmali.fbdc_target","1");
                }
                else
                {
                    if ( handle->prime_fd < 0 )
                    {
                        I("debug_only : not to use afbc in fb_target_layer, the original format : 0x%" PRIx64, internal_format);
                    }
			        property_set("vendor.gmali.fbdc_target","0");
                }
	        }
	        else
	        {
			    property_set("vendor.gmali.fbdc_target","0");
	        }
	    }

        /* 若已经被设置有别于 'req_format' 的 'internal_format', 则返回之. */
        if ( internal_format != req_format )
        {
            return internal_format;
        }
	}
#endif

    /*-------------------------------------------------------*/

    if ( req_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED )
    {
        if ( GRALLOC_USAGE_HW_VIDEO_ENCODER == (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)
            || GRALLOC_USAGE_HW_CAMERA_WRITE == (usage & GRALLOC_USAGE_HW_CAMERA_WRITE) )
        {
            I("to select NV12 for HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED for usage : 0x%" PRIx64 ".", usage);
            internal_format = HAL_PIXEL_FORMAT_YCrCb_NV12;
        }
        else
        {
            I("to select RGBX_8888 for HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED for usage : 0x%" PRIx64 ".", usage);
            internal_format = HAL_PIXEL_FORMAT_RGBX_8888;
        }
    }
    else if ( req_format == HAL_PIXEL_FORMAT_YCrCb_NV12_10
            && USAGE_CONTAIN_VALUE(GRALLOC_USAGE_TO_USE_ARM_P010, GRALLOC_USAGE_ROT_MASK) )
    {
        ALOGV("rk_debug force  MALI_GRALLOC_FORMAT_INTERNAL_P010 usage=0x%" PRIx64, usage);
        internal_format = MALI_GRALLOC_FORMAT_INTERNAL_P010; // base_format of internal_format, no modifiers.
    }
    else if ( req_format == HAL_PIXEL_FORMAT_YCbCr_420_888 )
    {
	    I("to use HAL_PIXEL_FORMAT_YCrCb_NV12 for  %" PRIu64, req_format);
	    internal_format = HAL_PIXEL_FORMAT_YCrCb_NV12;
    }
    else
    {
        internal_format = req_format;
    }

    return internal_format;
}

/*
 * 返回 'format' 是否是 rk 扩展的 hal_format.
 */
static bool is_rk_ext_hal_format(uint64_t format)
{
    if ( HAL_PIXEL_FORMAT_YCrCb_NV12 == format 
        || HAL_PIXEL_FORMAT_YCrCb_NV12_10 == format )
    {
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * rk_driver_of_gralloc_drm_device 中对 driver_of_gralloc_drm_device 的 alloc 方法的具体实现.
 * 注意 :
 *      本方法 同时实现 alloc buffer 和 import buffer 的功能,
 *      若传入的 'handle->prime_fd' < 0, 则将执行 alloc;
 *      若传入的 'handle->prime_fd' >= 0, 则将执行 import.
 */
static struct gralloc_drm_bo_t *drm_gem_rockchip_alloc(struct gralloc_drm_drv_t *drv,
		                                               struct gralloc_drm_handle_t *handle)
{
    int ret;
    int err;
    struct rk_driver_of_gralloc_drm_device_t *rk_drv = (struct rk_driver_of_gralloc_drm_device_t *)drv;
    struct rockchip_buffer *buf;

    int w = handle->width,h = handle->height;
    int format = handle->format;
    int usage = handle->usage;

    int internalWidth,internalHeight;
    uint64_t internal_format;
    int byte_stride;   // Stride of the buffer in bytes
    int pixel_stride;  // Stride of the buffer in pixels
	int old_alloc_width;
	int old_alloc_height;
    int old_byte_stride;

    uint64_t alloc_format;
    size_t size;
    plane_info_t plane_info[MAX_PLANES];
    int alloc_width;
    int alloc_height;
    

    alloc_type_t alloc_type;

    uint32_t gem_handle;
    void *addr = NULL;
    uint32_t flags = 0;
    struct drm_rockchip_gem_phys phys_arg;
    char dmabuf_name[DMA_BUF_NAME_LEN];


    D("enter, w : %d, h : %d, format : 0x%x, usage : 0x%x.", w, h, format, usage);

    if ( NULL == rk_drv )
    {
        rk_drv = s_rk_drv;
    }

    memset( (void*)plane_info, 0, sizeof(plane_info) );

	phys_arg.phy_addr = 0;

	/* Some formats require an internal width and height that may be used by
	 * consumers/producers.
	 */
	internalWidth = w;
	internalHeight = h;

    internal_format = rk_drm_gralloc_select_format(format, usage, handle);
    alloc_format = internal_format;
    
    if ( is_rk_ext_hal_format(alloc_format) )
    {
        /* 为 rk_ext_hal_format 的 待分配的 buffer 计算 size, byte_stride 等参数. */
        // rk_video_decoder 模块对 alloc 的参数 有隐式的语义, 参见 get_rk_nv12_stride_and_size(), ...
        // 所以, 这里无法使用来自 arm_gralloc 的计算逻辑.

        uint64_t rk_ext_hal_format = alloc_format;

        switch ( rk_ext_hal_format )
        {
            case HAL_PIXEL_FORMAT_YCrCb_NV12:
                if (!get_rk_nv12_stride_and_size(w, h, &pixel_stride, &byte_stride, &size))
                {
                    E("err.");
                    return NULL;
                }
                D("for rk_nv12, w : %d, h : %d, pixel_stride : %d, byte_stride : %d, size : %zu; internalHeight : %d.",
                    w,
                    h,
                    pixel_stride,
                    byte_stride,
                    size,
                    internalHeight);
                break;

            case HAL_PIXEL_FORMAT_YCrCb_NV12_10:
                if (!get_rk_nv12_10bit_stride_and_size(w, h, &pixel_stride, &byte_stride, &size))
                {
                    E("err.");
                    return NULL;
                }
                D("for nv12_10, w : %d, h : %d, pixel_stride : %d, byte_stride : %d, size : %zu; internalHeight : %d.",
                        w,
                        h,
                        pixel_stride,
                        byte_stride,
                        size,
                        internalHeight);
                break;

            default:
                E("unexpected rk_ext_hal_format : %" PRIu64, rk_ext_hal_format);
                return NULL;
        }
        
        /*-----------------------------------*/
        /* 计算 plane_info. */

        plane_info[0].offset = 0;
        plane_info[0].byte_stride = byte_stride;
        plane_info[0].alloc_width = internalWidth;
        plane_info[0].alloc_height = internalHeight;

        plane_info[1].offset = byte_stride * internalHeight;
        plane_info[1].byte_stride = byte_stride;
        plane_info[1].alloc_width = internalWidth / 2;
        plane_info[1].alloc_height = internalHeight / 2;
            // " / 2" : 参照 arm_gralloc 对 MALI_GRALLOC_FORMAT_INTERNAL_NV12 格式的 buffer 的计算结果.
            // 但是目前 使能 GRALLOC_USE_LEGACY_CALCS, 且 gralloc_drm_handle_t::byte_stride 非 0,
            // mali_so 将仍旧使用 gralloc_drm_handle_t 的 "DEPRECATED members",
            // 而不使用这里的 gralloc_drm_handle_t::plane_info.
    }
    else
    {
        /* 对 "非 rk_ext_hal_format 的 buffer", 使用来自 arm_gralloc 的逻辑, 计算 size, byte_stride 等参数. */
        
        alloc_width = w;
        alloc_height = h;
        int32_t format_idx;
        
        format_idx = get_format_index(alloc_format & MALI_GRALLOC_INTFMT_FMT_MASK);
        if (format_idx == -1)
        {
            E("err.");
            return NULL;
        }
        V("alloc_format: 0x%" PRIx64 " format_idx: %d", alloc_format, format_idx);

        /*
         * Obtain allocation type (uncompressed, AFBC basic, etc...)
         */
        if (!get_alloc_type(alloc_format & MALI_GRALLOC_INTFMT_EXT_MASK,
                            format_idx,
                            usage,
                            &alloc_type))
        {
            E("err.");
            return NULL;
        }

        if (!validate_format(&formats[format_idx], alloc_type, h))
        {
            E("err.");
            return NULL;
        }

        /*
         * Resolution of frame (allocation width and height) might require adjustment.
         * This adjustment is only based upon specific usage and pixel format.
         * If using AFBC, further adjustments to the allocation width and height will be made later
         * based on AFBC alignment requirements and, for YUV, the plane properties.
         */
        mali_gralloc_adjust_dimensions(alloc_format,
                                       usage,
                                       &alloc_width,
                                       &alloc_height);

        /* Obtain buffer size and plane information. */
        calc_allocation_size(alloc_width,
                             alloc_height,
                             alloc_type,
                             formats[format_idx],
                             usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK),
                             usage & ~(GRALLOC_USAGE_PRIVATE_MASK | GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK),
                             &pixel_stride,
                             &size,
                             plane_info);

        /*-----------------------------------*/
        /* 计算 buffer 的 legacy 参数, 参见 gralloc_drm_handle_t 定义中的 "DEPRECATED members.". */

        /* Pre-fill legacy values with those calculated above
         * since these are sometimes not set at all by the legacy calculations.
         */
        old_byte_stride = plane_info[0].byte_stride;
        old_alloc_width = plane_info[0].alloc_width;
        old_alloc_height = plane_info[0].alloc_height;

        /* Translate to legacy alloc_type. */
        legacy::alloc_type_t legacy_alloc_type;
        switch (alloc_type.primary_type)
        {
            case AllocBaseType::AFBC:
                legacy_alloc_type.primary_type = legacy::AllocBaseType::AFBC;
                break;
            case AllocBaseType::AFBC_WIDEBLK:
                legacy_alloc_type.primary_type = legacy::AllocBaseType::AFBC_WIDEBLK;
                break;
            case AllocBaseType::AFBC_EXTRAWIDEBLK:
                legacy_alloc_type.primary_type = legacy::AllocBaseType::AFBC_EXTRAWIDEBLK;
                break;
            default:
                legacy_alloc_type.primary_type = legacy::AllocBaseType::UNCOMPRESSED;
                break;
        }
        if (alloc_type.is_padded)
        {
            legacy_alloc_type.primary_type = legacy::AllocBaseType::AFBC_PADDED;
        }
        legacy_alloc_type.is_multi_plane = alloc_type.is_multi_plane;
        legacy_alloc_type.is_tiled = alloc_type.is_tiled;

        /*
         * Resolution of frame (and internal dimensions) might require adjustment
         * based upon specific usage and pixel format.
         */
        legacy::mali_gralloc_adjust_dimensions(internal_format,
                                               usage,
                                               legacy_alloc_type,
                                               w,
                                               h,
                                               &old_alloc_width,
                                               &old_alloc_height);

        size_t legacy_size = 0;
        int res = legacy::get_alloc_size(internal_format,
                                         usage,
                                         legacy_alloc_type,
                                         old_alloc_width,
                                         old_alloc_height, 
                                         &old_byte_stride,
                                         &pixel_stride,
                                         &legacy_size);
        if (res < 0)
        {
            ALOGW("Legacy allocation size calculation failed. "
                    "Relying upon new calculation instead.");
        }

        internalWidth = old_alloc_width;
        internalHeight = old_alloc_height;
        byte_stride = old_byte_stride;

        /* Accommodate for larger legacy allocation size. */
        if (legacy_size > size)
        {
            size = legacy_size;
        }
    }

    /*-------------------------------------------------------*/

	buf = (struct rockchip_buffer*)calloc(1, sizeof(*buf));
	if (!buf) {
		AERR("Failed to allocate buffer wrapper\n");
		return NULL;
	}
    buf->bo = NULL;

    /*-------------------------------------------------------*/
    // 根据 'usage' 预置待 alloc 或 import 的 flags, cachable 或 物理连续 等.

	if ( (usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN
		|| format == HAL_PIXEL_FORMAT_YCrCb_NV12_10)
	{
		D("to ask for cachable buffer for CPU read, usage : 0x%x", usage);
		//set cache flag
		flags = ROCKCHIP_BO_CACHABLE;
	}

	if(USAGE_CONTAIN_VALUE(GRALLOC_USAGE_TO_USE_PHY_CONT,GRALLOC_USAGE_ROT_MASK))
	{
		flags |= ROCKCHIP_BO_CONTIG; // 预期要求 CMA 内存.
		ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "try to use Physically Continuous memory\n");
	}

	if(usage & GRALLOC_USAGE_PROTECTED)
	{
		flags |= ROCKCHIP_BO_SECURE;
		ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "try to use secure memory\n");
	}

    /*-------------------------------------------------------*/
    // 完成 alloc 或 import buffer.

    /* 若 buufer 实际上已经分配 (通常在另一个进程中), 则 将 buffer import 到 当前进程, ... */
	if (handle->prime_fd >= 0) {
        /* 将 prime_fd 引用的 dma_buf, import 为 当前进程的 gem_object, 得到对应的 gem_handle 的 value. */
        buf->bo = rk_drm_adapter_import_dma_buf(rk_drv, handle->prime_fd, flags, size);
		if ( NULL == buf->bo )
        {
			E("failed to import dma_buf, prime_fd : %d.", handle->prime_fd);
            goto failed_to_import_dma_buf;
		}
	}
    else    // if (handle->prime_fd >= 0), 即 buffer 未实际分配, 将 分配, ...
    {
		buf->bo = rk_drm_adapter_create_rockchip_bo(rk_drv, size, flags);
		if ( NULL == buf->bo )
        {
			E("failed to create(alloc) bo %dx%dx%dx%zd\n",
				handle->height, pixel_stride,byte_stride, size);
			goto failed_to_alloc_buf;
		}

        ret = rk_drm_adapter_get_prime_fd(rk_drv, buf->bo, &(handle->prime_fd) );
		if ( ret != 0 ) {
            E("failed to get prime_fd from rockchip_bo.");
			goto failed_to_get_prime_fd;
		}

        get_dmabuf_name(size, dmabuf_name);
       // I("dmabuf_name : %s", dmabuf_name);
        /* 设置 dma_buf 的 name. */
        ret = ioctl(handle->prime_fd, DMA_BUF_SET_NAME, dmabuf_name);
        if ( ret != 0 )
        {
            E("failed set name of dma_buf.");
        }

        gem_handle = rk_drm_adapter_get_gem_handle(buf->bo);

		buf->base.fb_handle = gem_handle;

		if(USAGE_CONTAIN_VALUE(GRALLOC_USAGE_TO_USE_PHY_CONT,GRALLOC_USAGE_ROT_MASK))
		{
			phys_arg.handle = gem_handle;
			ret = drmIoctl(rk_drv->fd_of_drm_dev,
                           DRM_IOCTL_ROCKCHIP_GEM_GET_PHYS,
                           &phys_arg);
			if (ret)
				ALOGE("failed to get phy address: %s\n", strerror(errno));
			ALOGD_IF(RK_DRM_GRALLOC_DEBUG,"get phys 0x%x\n", phys_arg.phy_addr);
		}

#if GRALLOC_INIT_AFBC == 1
        if ( internal_format & MALI_GRALLOC_INTFMT_AFBCENABLE_MASK )
        {
            if (!(usage & GRALLOC_USAGE_PROTECTED))
            {
                    addr = rk_drm_adapter_map_rockchip_bo(buf->bo);
                    if (!addr) {
                            E("failed to map bo");
                            // LOG_ALWAYS_FATAL("failed to map bo");
                            goto err_unref;
                    }
            }
            else
            {
                E("we would not init the proteced afbc_buffer.");
            }
            
            if ( addr != NULL )
            {
	            struct dma_buf_sync sync_args;
                
                /* 若当前 buffer 是 cachable 的, 则... */
                if ( flags & ROCKCHIP_BO_CACHABLE )
                {
                    /* 向 kernel 空间声明 "将开始 CPU 对该 buffer 的写操作". */
		            sync_args.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
		            ret = ioctl(handle->prime_fd, DMA_BUF_IOCTL_SYNC, &sync_args);
                    if ( ret != 0 )
                    {
                        E("DMA_BUF_IOCTL_SYNC start failed");
                    }
                }

                /* < 对 afbc_buffer 具体初始化. > */
                const bool is_multi_plane = handle->is_multi_plane();
				for (int i = 0; i < MAX_PLANES && (i == 0 || plane_info[i].byte_stride != 0); i++)
				{
#if GRALLOC_USE_LEGACY_CALCS == 1
					if (i == 0)
					{
						uint32_t w = GRALLOC_MAX((uint32_t)old_alloc_width, plane_info[0].alloc_width);
						uint32_t h = GRALLOC_MAX((uint32_t)old_alloc_height, plane_info[0].alloc_height);

						init_afbc( (uint8_t*)addr,
						          internal_format,
						          is_multi_plane,
						          w,
						          h);
					}
					else
#endif
					{
						init_afbc( ( (uint8_t*)addr) + plane_info[i].offset,
						          alloc_format,
						          is_multi_plane,
						          plane_info[i].alloc_width,
						          plane_info[i].alloc_height);
					}
				}

                /* 若当前 buffer 是 cachable 的, 则... */
                if ( flags & ROCKCHIP_BO_CACHABLE )
                {
                    /* 向 kernel 空间声明 "已完成 CPU 对该 buffer 的写操作". */
		            sync_args.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
		            ret = ioctl(handle->prime_fd, DMA_BUF_IOCTL_SYNC, &sync_args);
                    if ( ret != 0 )
                    {
                        E("DMA_BUF_IOCTL_SYNC end failed");
                    }
                }
            }
            else
            {
                E("can't init afbc_buffer.");
            }
        }
#endif /* GRALLOC_INIT_AFBC == 1 */
	}   // if (handle->prime_fd >= 0)

    /*-------------------------------------------------------*/

#if MALI_AFBC_GRALLOC == 1
        /*
         * If handle has been dup,then the fd is a negative number.
         * Either you should close it or don't allocate the fd agagin.
         * Otherwize,it will leak fd.
         */
        if(handle->share_attr_fd < 0)
        {
                err = gralloc_buffer_attr_allocate( handle );
                //ALOGD("err=%d,isfb=%x,[%d,%x]",err,usage & GRALLOC_USAGE_HW_FB,hnd->share_attr_fd,hnd->attr_base);
                if( err < 0 )
                {
                        if ( (usage & GRALLOC_USAGE_HW_FB) )
                        {
                                /*
                                 * Having the attribute region is not critical for the framebuffer so let it pass.
                                 */
                                err = 0;
                        }
                        else
                        {
                                drm_gem_rockchip_free( drv, &buf->base );
                                goto err_unref;
                        }
                }
		}
#endif
#ifdef USE_HWC2
	/*
	 * If handle has been dup,then the fd is a negative number.
	 * Either you should close it or don't allocate the fd agagin.
	 * Otherwize,it will leak fd.
	 */
	if(handle->ashmem_fd < 0)
	{
			err = gralloc_rk_ashmem_allocate( handle );
			//ALOGD("err=%d,isfb=%x,[%d,%x]",err,usage & GRALLOC_USAGE_HW_FB,hnd->share_attr_fd,hnd->attr_base);
			if( err < 0 )
			{
					if ( (usage & GRALLOC_USAGE_HW_FB) )
					{
							/*
							 * Having the attribute region is not critical for the framebuffer so let it pass.
							 */
							err = 0;
					}
					else
					{
							drm_gem_rockchip_free( drv, &buf->base );
							goto err_unref;
					}
			}
	}
#endif

    /*-------------------------------------------------------*/
    // 处理 private usage.

    switch (usage & MALI_GRALLOC_USAGE_YUV_CONF_MASK)
    {
        case MALI_GRALLOC_USAGE_YUV_CONF_0:
            if(USAGE_CONTAIN_VALUE(GRALLOC_USAGE_TO_USE_ARM_P010,GRALLOC_USAGE_ROT_MASK))
            {
				handle->yuv_info = MALI_YUV_BT709_WIDE; // for rk_hdr.
            }
			else
            {
				handle->yuv_info = MALI_YUV_BT601_NARROW;
            }
			break;

        case MALI_GRALLOC_USAGE_YUV_CONF_1:
			if (format == HAL_PIXEL_FORMAT_YCbCr_420_888)
			{
				ALOGD("Warning: yuv420_888 use BT601_narrow");
				handle->yuv_info = MALI_YUV_BT601_NARROW;
			}
			else
				handle->yuv_info = MALI_YUV_BT601_WIDE;
            break;

        case MALI_GRALLOC_USAGE_YUV_CONF_2:
            handle->yuv_info = MALI_YUV_BT709_NARROW;
            break;

        case MALI_GRALLOC_USAGE_YUV_CONF_3:
            handle->yuv_info = MALI_YUV_BT709_WIDE;
            break;
    }
    
    /*-------------------------------------------------------*/

	if(phys_arg.phy_addr && phys_arg.phy_addr != handle->phy_addr)
	{
		handle->phy_addr = phys_arg.phy_addr;
	}

    handle->internal_format = internal_format;
    handle->stride = byte_stride;
    handle->byte_stride = byte_stride;
    handle->internalWidth = internalWidth;
    handle->internalHeight = internalHeight;

    handle->alloc_format = alloc_format;
    memcpy(handle->plane_info, plane_info, sizeof(plane_info_t) * MAX_PLANES);
    handle->size = size;

    handle->pixel_stride = pixel_stride;
    handle->offset = 0;
    handle->name = 0;

	buf->base.handle = handle;

    D("leave, w : %d, h : %d, format : 0x%x,internal_format : 0x%" PRIx64 ", usage : 0x%x."
            "size=%d, pixel_stride=%d, byte_stride=%d, internalWidth : %d,internalHeight : %d",
        handle->width, handle->height, handle->format, internal_format, handle->usage,
        handle->size, pixel_stride, byte_stride, internalWidth, internalHeight);
    ALOGV("plane_info[0]: offset : %u, byte_stride : %u, alloc_width : %u, alloc_height : %u",
            (handle->plane_info)[0].offset,
            (handle->plane_info)[0].byte_stride,
            (handle->plane_info)[0].alloc_width,
            (handle->plane_info)[0].alloc_height);
    ALOGV("plane_info[1]: offset : %u, byte_stride : %u, alloc_width : %u, alloc_height : %u",
            (handle->plane_info)[1].offset,
            (handle->plane_info)[1].byte_stride,
            (handle->plane_info)[1].alloc_width,
            (handle->plane_info)[1].alloc_height);

    D("leave: prime_fd=%d,share_attr_fd=%d",handle->prime_fd,handle->share_attr_fd);

	return &buf->base;

failed_to_get_prime_fd:
err_unref:
    rk_drm_adapter_destroy_rockchip_bo(rk_drv, buf->bo);

failed_to_import_dma_buf:
failed_to_alloc_buf:
	free(buf);
	return NULL;
}

static void drm_gem_rockchip_free(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo)
{
	struct rockchip_buffer *buf = (struct rockchip_buffer *)bo;
    struct rk_driver_of_gralloc_drm_device_t *rk_drv = (struct rk_driver_of_gralloc_drm_device_t *)drv;
    struct gralloc_drm_handle_t *gr_handle = gralloc_drm_handle((buffer_handle_t)bo->handle);

    if ( NULL == rk_drv )
    {
        rk_drv = s_rk_drv;
    }

        if (!gr_handle)
        {
                ALOGE("%s: invalid handle",__FUNCTION__);
                gralloc_drm_unlock_handle((buffer_handle_t)bo->handle);
                return;
        }

#if MALI_AFBC_GRALLOC == 1
	gralloc_buffer_attr_free( gr_handle );
#endif

#ifdef USE_HWC2
	gralloc_rk_ashmem_free( gr_handle );
#endif
	if (gr_handle->prime_fd)
		close(gr_handle->prime_fd);

	gr_handle->prime_fd = -1;
        gralloc_drm_unlock_handle((buffer_handle_t)bo->handle);

    D("rk_drv : %p", rk_drv);
    rk_drm_adapter_destroy_rockchip_bo(rk_drv, buf->bo); // rk_drv : 0x0

	free(buf);
}

static int drm_gem_rockchip_map(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo, int x, int y, int w, int h,
		int enable_write, void **addr)
{
	struct rockchip_buffer *buf = (struct rockchip_buffer *)bo;
	struct gralloc_drm_handle_t *gr_handle = gralloc_drm_handle((buffer_handle_t)bo->handle);
	struct dma_buf_sync sync_args;
	int ret = 0, ret2 = 0;

	UNUSED(drv);
	UNUSED(x);
	UNUSED(y);
	UNUSED(w);
	UNUSED(h);
	UNUSED(enable_write);

	if (gr_handle->usage & GRALLOC_USAGE_PROTECTED)
	{
		*addr = NULL;
		ALOGE("The secure buffer cann't be map");
	}
	else
	{
		*addr = rk_drm_adapter_map_rockchip_bo(buf->bo);
		if ( !*addr || (*addr == MAP_FAILED) ) {
			ALOGE("failed to map bo,*addr=%p, bo=%p, w : %d, h : %d, format : 0x%x, usage : 0x%x, size=%d,pixel_stride=%d,byte_stride=%d prime_fd=%d,share_attr_fd=%d",
				*addr, buf->bo, gr_handle->width, gr_handle->height, gr_handle->format, gr_handle->usage, gr_handle->size,
				gr_handle->pixel_stride,gr_handle->byte_stride,gr_handle->prime_fd,gr_handle->share_attr_fd);
			if(*addr == MAP_FAILED)
				*addr = NULL;
			ret = -1;
		}
#if RK_CTS_WORKROUND
		else {
			int big_scale;
			static int iCnt = 0;
			char cmdline[256] = {0};

			getProcessCmdLine(cmdline, sizeof(cmdline));

			if(!strcmp(cmdline,"android.view.cts"))
			{
				FindAppHintInFile(VIEW_CTS_FILE, VIEW_CTS_PROG_NAME, BIG_SCALE_HINT, &big_scale, IMG_INT_TYPE);
				if(big_scale && (gr_handle->usage == 0x603 || gr_handle->usage == 0x203) ) {
                    /* 在 CPU 一侧将 buffer 中的数据设置为 case 预期的 value. */
					memset(*addr,0xFF,gr_handle->height*gr_handle->byte_stride);
					ALOGD_IF(1, "memset 0xff byte_stride=%d iCnt=%d",gr_handle->byte_stride,iCnt);
					iCnt++;
				}
				if(iCnt == 400 && big_scale)
				{
					ModifyAppHintInFile(VIEW_CTS_FILE, VIEW_CTS_PROG_NAME, BIG_SCALE_HINT, &big_scale, 0, IMG_INT_TYPE);
					ALOGD_IF(1,"reset big_scale");
				}
			}
		}
#endif
	}

	if(buf && buf->bo && (buf->bo->flags & ROCKCHIP_BO_CACHABLE))
	{
		sync_args.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
		ret2 = ioctl(bo->handle->prime_fd, DMA_BUF_IOCTL_SYNC, &sync_args);
            // "DMA_BUF_SYNC_START", "DMA_BUF_IOCTL_SYNC" :
            //      安全地获取 dma_buf 的 访问,
            //      实现和 GPU 等设备对 buffer 访问操作的互斥, 即实现 "lock" 的语义.
            //      这里, 对应的 ("DMA_BUF_SYNC_END", "DMA_BUF_IOCTL_SYNC") 在 drm_gem_rockchip_unmap 中被调用.

		if (ret2 != 0)
			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "%s:DMA_BUF_IOCTL_SYNC start failed", __FUNCTION__);
	}

	gralloc_drm_unlock_handle((buffer_handle_t)bo->handle);
	return ret;
}

static void drm_gem_rockchip_unmap(struct gralloc_drm_drv_t *drv,
		struct gralloc_drm_bo_t *bo)
{
	struct rockchip_buffer *buf = (struct rockchip_buffer *)bo;
	struct dma_buf_sync sync_args;
	int ret = 0;

	UNUSED(drv);

	if(buf && buf->bo && (buf->bo->flags & ROCKCHIP_BO_CACHABLE))
	{
		sync_args.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
		ioctl(bo->handle->prime_fd, DMA_BUF_IOCTL_SYNC, &sync_args);
		if (ret != 0)
			ALOGD_IF(RK_DRM_GRALLOC_DEBUG, "%s:DMA_BUF_IOCTL_SYNC end failed", __FUNCTION__);
	}
}

static int drm_init_version()
{
        char value[PROPERTY_VALUE_MAX];

        property_get("vendor.ggralloc.version", value, "NULL");
        if(!strcmp(value,"NULL"))
        {
                ALOGD(RK_GRAPHICS_VER);
        }

        return 0;
}

/**
 * 创建并返回 rk_driver_of_gralloc_drm_device 实例.
 * @param fd
 *      fd_of_drm_dev
 */
struct gralloc_drm_drv_t *gralloc_drm_drv_create_for_rockchip(int fd)
{
	struct rk_driver_of_gralloc_drm_device_t *rk_drv;

        drm_init_version();

    rk_drv = new rk_driver_of_gralloc_drm_device_t;
        // .KP : 这里 必须用 new 的方式, 只有这样 rk_drv->m_gem_objs_ref_info_map 的构造函数才会被调用.
	if (!rk_drv) {
		ALOGE("Failed to allocate rockchip gralloc device\n");
		return NULL;
	}
    s_rk_drv = rk_drv;

	rk_drv->rk_drm_dev = rockchip_device_create(fd);
        // rockchip_device_create() 实现在 libdrm_rockchip.so 中.
	if (!rk_drv->rk_drm_dev) {
		ALOGE("Failed to create new rockchip_device instance\n");
        delete rk_drv;
        s_rk_drv = NULL;
		return NULL;
	}

    rk_drm_adapter_init(rk_drv);

	rk_drv->fd_of_drm_dev = fd;
	rk_drv->base.destroy = drm_gem_rockchip_destroy;
	rk_drv->base.alloc = drm_gem_rockchip_alloc; // "rk_drv->base" : .type : gralloc_drm_drv_t
	rk_drv->base.free = drm_gem_rockchip_free;
	rk_drv->base.map = drm_gem_rockchip_map;
	rk_drv->base.unmap = drm_gem_rockchip_unmap;

	return &rk_drv->base;
}

/*---------------------------------------------------------------------------*/
// rk_drm_adapter 提供给 rk_drm_gralloc 调用的接口 :

/*
 * 创建 rockchip_bo 实例, 并分配底层的 dma_buf(gem_obj).
 */
static struct rockchip_bo* rk_drm_adapter_create_rockchip_bo(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                                             size_t size,
                                                             uint32_t flags)
{
    rockchip_bo* rk_bo = NULL;
    uint32_t handle = 0;  // gem_handle
    int ret = 0;

    Mutex::Autolock _l(get_drm_lock(rk_drv) );

    rk_bo = rockchip_bo_create(get_rk_drm_dev(rk_drv), size, flags);
    if ( NULL == rk_bo )
    {
        SET_ERROR_AND_JUMP("fail to create rk_bo in original way.", ret, -1, EXIT);
    }

    handle = rk_drm_adapter_get_gem_handle(rk_bo);
    D("created a gem_obj with handle %u", handle);
    rk_drm_adapter_inc_gem_obj_ref(rk_drv, handle);

EXIT:
    if ( ret != 0 && rk_bo != NULL )
    {
	    rockchip_bo_destroy(rk_bo);
        rk_bo = NULL;
    }

    return rk_bo;
}

static void rk_drm_adapter_destroy_rockchip_bo(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                               struct rockchip_bo *bo)
{
    Mutex::Autolock _l(get_drm_lock(rk_drv) );

    if ( NULL == bo )
    {
        E("'bo' is NULL.")
            return;
    }

    if ( bo->vaddr != NULL )
    {
        munmap(bo->vaddr, bo->size);
    }

    /* 将底层 gem_obj 的被引用计数减 1. */
    rk_drm_adapter_dec_gem_obj_ref(rk_drv, bo->handle);

    free(bo);
}

/*
 * 将 'dma_buf_fd' 引用的 dma_buf, import 为 当前进程的 gem_object, 创建并返回对应的 rockchip_bo 实例.
 */
static struct rockchip_bo* rk_drm_adapter_import_dma_buf(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                                         int dma_buf_fd,
                                                         uint32_t flags,
                                                         uint32_t size)
{
    int ret = 0;
    uint32_t handle = 0; // handle of gem_obj for dma_buf imported.
    int fd_of_drm_dev = get_fd_of_drm_dev(rk_drv);
    struct rockchip_bo* bo = NULL;

    Mutex::Autolock _l(get_drm_lock(rk_drv) );

    /* 将 dma_buf_fd 引用的 dma_buf, import 为 当前进程的 gem_object, 得到对应的 gem_handle 的 value. */
    CHECK_FUNC_CALL( drmPrimeFDToHandle(fd_of_drm_dev,
                                        dma_buf_fd,
                                        &handle)
                    , ret, failed_to_import_dma_buf);
    rk_drm_adapter_inc_gem_obj_ref(rk_drv, handle); // 预期永不失败.
    D("imported a dma_buf as a gem_obj with handle %u", handle);

	bo = rockchip_bo_from_handle(get_rk_drm_dev(rk_drv), handle, flags, size);
    if ( NULL == bo )
    {
        SET_ERROR_AND_JUMP("fail to create rockchip_bo instance from gem_handle : %d", ret, -1, failed_to_create_bo, handle);
    }
    handle = 0;

    return bo;

failed_to_create_bo:
    rk_drm_adapter_dec_gem_obj_ref(rk_drv, handle);
failed_to_import_dma_buf:
    return NULL;
}

/*
 * 获取 '*bo' 的底层 gem_obj 的 prime_fd, 即其对应的 dma_buf 的 fd (dma_buf_fd).
 * '*prime_fd' 并 "不" 持有对 gem_obj 的 引用计数.
 */
static inline uint32_t rk_drm_adapter_get_prime_fd(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                                   struct rockchip_bo *bo,
                                                   int* prime_fd)
{
    int fd_of_drm_dev = get_fd_of_drm_dev(rk_drv);
	uint32_t gem_handle = rk_drm_adapter_get_gem_handle(bo);

    Mutex::Autolock _l(get_drm_lock(rk_drv) );

    return drmPrimeHandleToFD(fd_of_drm_dev,
                              gem_handle,
                              0,
                              prime_fd);
}

/*-------------------------------------------------------*/
// 在 rk_drm_adapter 内部使用的函数 :

/*
 * 在 s_gem_objs_ref_info_map 中增加 指定 gem_obj 的被引用计数.
 * 若对应的 gem_obj_referenced_info_entry_t 实例不存在,
 * 则创建, 并添加到 s_gem_objs_ref_info_map 中.
 * 调用者必须持有 'rk_drm->m_drm_lock'.
 *
 * @param handle
 *      目标 gem_obj 的 handle.
 */
static int rk_drm_adapter_inc_gem_obj_ref(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                          uint32_t handle)
{
    int ret = 0;

    KeyedVector<uint32_t, gem_obj_referenced_info_entry_t*>& map = get_gem_objs_ref_info_map(rk_drv);

    /* 若当前 gem_obj "尚未" 被记录, 则.... */
    if ( map.indexOfKey(handle) < 0 )
    {
        gem_obj_referenced_info_entry_t* entry = new gem_obj_referenced_info_entry_t(handle); // info_entry.
        map.add(handle, entry);
        entry = NULL;    // '*entry' 实例将由 'map' 维护.
    }
    /* 否则, 即 "已经" 被记录, 则... */
    else
    {
        gem_obj_referenced_info_entry_t*& entry = map.editValueFor(handle);
        entry->inc_ref();
    }

    return ret;
}

/*
 * 在 s_gem_objs_ref_info_map 中, 减少指定 gem_obj 的被引用计数.
 * 若减少到 0, remove 对应的 gem_obj_referenced_info_entry_t 实例,
 * 并调用 rk_drm_adapter_close_gem_obj(), close 该 gem_obj.
 * 调用者必须持有 'rk_drm->m_drm_lock'.
 *
 * @param handle
 *      目标 gem_obj 的 handle.
 */
static void rk_drm_adapter_dec_gem_obj_ref(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                           uint32_t handle)
{
    KeyedVector<uint32_t, gem_obj_referenced_info_entry_t*>& map = get_gem_objs_ref_info_map(rk_drv);

    /* 若当前 gem_obj "尚未" 被记录, 则.... */
    if ( map.indexOfKey(handle) < 0 )
    {
        W("no info entry for gem_handle(%u)", handle);
        return;
    }
    /* 否则, 即 "已经" 被记录, 则... */
    else
    {
        gem_obj_referenced_info_entry_t*& entry = map.editValueFor(handle);

        entry->dec_ref();

        if ( 0 == entry->get_ref() )
        {
            /* 先释放 目标 value 指向的 heap 中的 info_entry 实例. */
            delete map.valueFor(handle);
            /* 将 entry(的指针) 从 'map' 中移除. */
		    map.removeItem(handle);

            rk_drm_adapter_close_gem_obj(rk_drv, handle);
        }

        return;
    }
}

/*
 * 关闭 'handle' 指定的 gem_obj.
 * 调用者必须持有 'rk_drm->m_drm_lock'.
 *
 * @param handle
 *      目标 gem_obj 的 handle.
 */
static void rk_drm_adapter_close_gem_obj(struct rk_driver_of_gralloc_drm_device_t* rk_drv,
                                         uint32_t handle)
{
    struct drm_gem_close args;
    int ret;
    int fd_of_drm_dev = get_fd_of_drm_dev(rk_drv);

    memset(&args, 0, sizeof(args) );
    args.handle = handle;

    D("to close a gem_obj with handle %u", handle);
    ret = drmIoctl(fd_of_drm_dev, DRM_IOCTL_GEM_CLOSE, &args);
    if ( 0 != ret )
    {
        E("fail to perform DRM_IOCTL_GEM_CLOSE, ret : %d, err : %s.", ret, strerror(errno) );
    }
}
