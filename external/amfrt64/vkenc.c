/*
 * vkenc.c - H.264/HEVC encoding through Vulkan Video, the Linux half of Neutron's amfrt64.
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 *
 * How it works
 *
 * Each encoder owns a private VkInstance and VkDevice (no window system) on the GPU whose Vulkan
 * driver has a video encode queue for the codec: Mesa RADV on AMD machines, or any vendor when
 * NEUTRON_AMF_ANY_GPU=1 (development on NVIDIA). libvulkan.so.1 is loaded with dlopen(), so
 * amfrt64 loads fine on machines without Vulkan.
 *
 * Every frame in flight gets a "slot": a source image, a bitstream buffer, a feedback query,
 * command buffers and a fence. vkenc_submit() copies the caller's pixels into the slot's staging
 * buffer (a transfer-capable queue copies it into the source image), records the encode and
 * submits it without waiting. vkenc_receive() hands finished slots back in submission order.
 *
 * Frames are IDR or P frames with one reference, the previous frame. The DPB is one image with
 * two array layers that the frames use in turn: RADV before VCN 5 cannot take separate reference
 * images and addresses DPB slot n at array layer n, so slot index and layer index are the same.
 *
 * Parameter sets come from the driver (vkGetEncodedVideoSessionParametersKHR), because drivers
 * override fields they cannot honor (RADV rewrites the HEVC coding block sizes and parts of every
 * PPS), and are put in front of every IDR frame (and every header_spacing frames).
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vkenc.h"

#define VKENC_MAX_SLOTS     8
#define VKENC_DPB_SLOTS     2
#define VKENC_FENCE_TIMEOUT 10000000000ull /* 10 s */

/* Vulkan entry points, X-macro lists. */
#define VKENC_INSTANCE_FUNCS \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties2) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceVideoCapabilitiesKHR) \
    X(vkGetPhysicalDeviceVideoFormatPropertiesKHR) \
    X(vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

#define VKENC_DEVICE_FUNCS \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkGetImageSubresourceLayout) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkResetFences) \
    X(vkGetFenceStatus) \
    X(vkWaitForFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkGetQueryPoolResults) \
    X(vkResetQueryPool) \
    X(vkCmdResetQueryPool) \
    X(vkCmdBeginQuery) \
    X(vkCmdEndQuery) \
    X(vkQueueSubmit2) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdCopyBufferToImage) \
    X(vkCreateVideoSessionKHR) \
    X(vkDestroyVideoSessionKHR) \
    X(vkGetVideoSessionMemoryRequirementsKHR) \
    X(vkBindVideoSessionMemoryKHR) \
    X(vkCreateVideoSessionParametersKHR) \
    X(vkDestroyVideoSessionParametersKHR) \
    X(vkGetEncodedVideoSessionParametersKHR) \
    X(vkCmdBeginVideoCodingKHR) \
    X(vkCmdEndVideoCodingKHR) \
    X(vkCmdControlVideoCodingKHR) \
    X(vkCmdEncodeVideoKHR)

struct vkenc_instance
{
    VkInstance instance;
#define X(name) PFN_##name name;
    VKENC_INSTANCE_FUNCS
#undef X
};

/* The GPU and queue families an encoder runs on. */
struct vkenc_gpu
{
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t encode_family;
    uint32_t upload_family;      /* family for buffer-to-image copies; choose_upload() may make it
                                    encode_family, ~0u when there is none */
    VkQueueFlags encode_flags;
    int query_status;            /* the encode family reports query result status */
    int host_query_reset;
};

/* A video profile with its codec-specific chain. Never copied: the structures point at each other. */
struct vkenc_profile
{
    VkVideoProfileInfoKHR info;
    VkVideoEncodeH264ProfileInfoKHR h264;
    VkVideoEncodeH265ProfileInfoKHR h265;
    VkVideoEncodeUsageInfoKHR usage;
    VkVideoProfileListInfoKHR list;
};

struct vkenc_codec_caps
{
    VkVideoCapabilitiesKHR video;
    VkVideoEncodeCapabilitiesKHR encode;
    VkVideoEncodeH264CapabilitiesKHR h264;
    VkVideoEncodeH265CapabilitiesKHR h265;
};

enum upload_mode
{
    UPLOAD_LINEAR,       /* host writes into a linear source image */
    UPLOAD_COPY,         /* staging buffer, copied on the encode queue */
    UPLOAD_COPY_QUEUE,   /* staging buffer, copied on another queue, semaphore to the encode */
};

struct vkenc_slot
{
    VkImage src_image;
    VkDeviceMemory src_memory;
    VkImageView src_view;
    VkImageLayout src_layout;            /* layout of the source image while the slot is idle */
    uint8_t *src_map;                    /* UPLOAD_LINEAR */
    VkSubresourceLayout src_planes[2];   /* UPLOAD_LINEAR */
    VkBuffer staging;                    /* UPLOAD_COPY* */
    VkDeviceMemory staging_memory;
    uint8_t *staging_map;
    VkBuffer bitstream;
    VkDeviceMemory bitstream_memory;
    uint8_t *bitstream_map;
    VkQueryPool query;
    VkCommandBuffer upload_cmd;          /* UPLOAD_COPY_QUEUE */
    VkCommandBuffer encode_cmd;
    VkSemaphore upload_done;             /* UPLOAD_COPY_QUEUE */
    VkFence fence;
    int is_idr;
    int with_headers;
};

struct vkenc
{
    vkenc_params params;
    struct vkenc_instance vk;
    struct vkenc_gpu gpu;
    VkDevice device;
    struct
    {
#define X(name) PFN_##name name;
        VKENC_DEVICE_FUNCS
#undef X
    } fn;
    VkQueue encode_queue;
    VkQueue upload_queue;
    VkCommandPool encode_pool;
    VkCommandPool upload_pool;

    struct vkenc_profile profile;
    struct vkenc_codec_caps caps;
    enum upload_mode upload;
    VkFormat src_format;
    VkFormat dpb_format;
    uint32_t bytes_per_sample;            /* 1 for NV12, 2 for P010 */
    uint32_t src_width, src_height;       /* caller's frame size */
    uint32_t width, height;               /* visible size in the bitstream (even) */
    uint32_t coded_width, coded_height;   /* aligned size the hardware encodes */
    uint32_t fps_num, fps_den;
    uint32_t idr_period;
    uint32_t quality_level;
    int bitstream_coherent;
    VkDeviceSize bitstream_size;
    VkDeviceSize staging_chroma_offset;

    VkVideoSessionKHR session;
    VkDeviceMemory session_memory[16];
    uint32_t session_memory_count;
    VkVideoSessionParametersKHR session_params;
    VkImage dpb_image;
    VkDeviceMemory dpb_memory;
    VkImageView dpb_view;
    uint8_t *headers;                     /* parameter sets, Annex-B */
    size_t headers_size;

    /* Rate control state. The same structures go to vkCmdControlVideoCodingKHR and every
     * vkCmdBeginVideoCodingKHR, which must repeat the state that is in effect. */
    VkVideoEncodeRateControlModeFlagBitsKHR rc_mode;
    VkVideoEncodeRateControlLayerInfoKHR rc_layer;
    VkVideoEncodeH264RateControlLayerInfoKHR h264_rc_layer;
    VkVideoEncodeH265RateControlLayerInfoKHR h265_rc_layer;
    VkVideoEncodeRateControlInfoKHR rc_info;
    VkVideoEncodeH264RateControlInfoKHR h264_rc;
    VkVideoEncodeH265RateControlInfoKHR h265_rc;
    VkVideoEncodeQualityLevelInfoKHR quality_info;
    int use_gop_remaining;

    /* Codec parameter sets. */
    StdVideoH264SequenceParameterSetVui h264_vui;
    StdVideoH264SequenceParameterSet h264_sps;
    StdVideoH264PictureParameterSet h264_pps;
    StdVideoH265ProfileTierLevel h265_ptl;
    StdVideoH265DecPicBufMgr h265_dpbm;
    StdVideoH265SequenceParameterSetVui h265_vui;
    StdVideoH265VideoParameterSet h265_vps;
    StdVideoH265SequenceParameterSet h265_sps;
    StdVideoH265PictureParameterSet h265_pps;
    int h265_tmvp;

    /* Sequence state, only touched with submit_lock held. */
    int session_started;
    int have_ref;
    uint32_t ref_slot;                    /* DPB slot holding the reference picture */
    uint32_t gop_frame;                   /* frames since the last IDR */
    uint32_t frames_since_headers;
    uint32_t idr_count;
    uint64_t frames_submitted;

    /* Slot ring. */
    struct vkenc_slot slots[VKENC_MAX_SLOTS];
    uint32_t slot_count;
    uint32_t head, tail, pending;         /* ring_lock */
    pthread_mutex_t submit_lock;
    pthread_mutex_t receive_lock;
    pthread_mutex_t ring_lock;
    int failed;
};

/* ---- logging and small helpers -------------------------------------------------------------- */

static int debug_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *env = getenv("NEUTRON_VKENC_DEBUG");
        enabled = env && *env && *env != '0';
    }
    return enabled;
}

static void log_msg(int debug, const char *fmt, ...)
{
    va_list args;

    if (debug && !debug_enabled()) return;
    va_start(args, fmt);
    fputs("vkenc: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

#define TRACE(...) log_msg(1, __VA_ARGS__)
#define INFO(...)  log_msg(0, __VA_ARGS__)

static void set_reason(char *buf, size_t len, const char *fmt, ...)
{
    va_list args;

    if (!buf || !len) return;
    va_start(args, fmt);
    vsnprintf(buf, len, fmt, args);
    va_end(args);
}

static int env_flag(const char *name)
{
    const char *env = getenv(name);
    return env && !strcmp(env, "1");
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

static uint64_t align_up64(uint64_t value, uint64_t alignment)
{
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

static uint32_t gcd32(uint32_t a, uint32_t b)
{
    while (b)
    {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint32_t lcm32(uint32_t a, uint32_t b)
{
    if (!a) return b;
    if (!b) return a;
    return a / gcd32(a, b) * b;
}

static const char *codec_name(vkenc_codec codec)
{
    return codec == VKENC_CODEC_HEVC ? "HEVC" : "H.264";
}

/* ---- loading Vulkan ------------------------------------------------------------------------- */

static pthread_once_t loader_once = PTHREAD_ONCE_INIT;
static void *libvulkan;
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumerateInstanceVersion p_vkEnumerateInstanceVersion;

static void load_vulkan(void)
{
    if (!(libvulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)))
        libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) return;
    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(libvulkan, "vkGetInstanceProcAddr");
    if (!p_vkGetInstanceProcAddr) return;
    p_vkCreateInstance = (PFN_vkCreateInstance)p_vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    p_vkEnumerateInstanceVersion =
            (PFN_vkEnumerateInstanceVersion)p_vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
}

static void instance_destroy(struct vkenc_instance *vk)
{
    if (vk->instance && vk->vkDestroyInstance) vk->vkDestroyInstance(vk->instance, NULL);
    memset(vk, 0, sizeof(*vk));
}

static int instance_create(struct vkenc_instance *vk, char *reason, size_t len)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    uint32_t version = VK_API_VERSION_1_0;
    VkResult res;

    memset(vk, 0, sizeof(*vk));
    pthread_once(&loader_once, load_vulkan);
    if (!p_vkGetInstanceProcAddr || !p_vkCreateInstance)
    {
        set_reason(reason, len, "Vulkan is not installed (libvulkan.so.1 was not found).");
        return -1;
    }
    if (p_vkEnumerateInstanceVersion) p_vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3)
    {
        set_reason(reason, len, "The Vulkan loader is too old (Vulkan 1.3 is needed for video encoding).");
        return -1;
    }

    app.pApplicationName = "Neutron amfrt64";
    app.pEngineName = "neutron-vkenc";
    app.apiVersion = VK_API_VERSION_1_3;
    info.pApplicationInfo = &app;
    if ((res = p_vkCreateInstance(&info, NULL, &vk->instance)) != VK_SUCCESS)
    {
        if (res == VK_ERROR_INCOMPATIBLE_DRIVER)
            set_reason(reason, len, "No Vulkan 1.3 driver is installed for any GPU.");
        else
            set_reason(reason, len, "Vulkan could not be started (vkCreateInstance failed with error %d).", res);
        vk->instance = VK_NULL_HANDLE;
        return -1;
    }

#define X(name) \
    if (!(vk->name = (PFN_##name)p_vkGetInstanceProcAddr(vk->instance, #name))) \
    { \
        set_reason(reason, len, "The Vulkan loader lacks %s.", #name); \
        if (vk->vkDestroyInstance) vk->vkDestroyInstance(vk->instance, NULL); \
        memset(vk, 0, sizeof(*vk)); \
        return -1; \
    }
    X(vkDestroyInstance)
    VKENC_INSTANCE_FUNCS
#undef X
    return 0;
}

/* ---- profiles and capabilities -------------------------------------------------------------- */

static int std_profile_for(vkenc_codec codec, int profile, int bit_depth)
{
    if (codec == VKENC_CODEC_HEVC)
        return bit_depth == 10 ? STD_VIDEO_H265_PROFILE_IDC_MAIN_10 : STD_VIDEO_H265_PROFILE_IDC_MAIN;
    switch (profile)
    {
    case 66:
    case 256: /* AMF constrained baseline */
        return STD_VIDEO_H264_PROFILE_IDC_BASELINE;
    case 77:
        return STD_VIDEO_H264_PROFILE_IDC_MAIN;
    default:
        return STD_VIDEO_H264_PROFILE_IDC_HIGH;
    }
}

static const char *profile_name(vkenc_codec codec, int std_profile)
{
    if (codec == VKENC_CODEC_HEVC) return std_profile == STD_VIDEO_H265_PROFILE_IDC_MAIN_10 ? "Main 10" : "Main";
    if (std_profile == STD_VIDEO_H264_PROFILE_IDC_BASELINE) return "Baseline";
    if (std_profile == STD_VIDEO_H264_PROFILE_IDC_MAIN) return "Main";
    return "High";
}

static void profile_init(struct vkenc_profile *p, vkenc_codec codec, int std_profile, int bit_depth)
{
    VkVideoComponentBitDepthFlagsKHR depth = bit_depth == 10 ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR
                                                             : VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    memset(p, 0, sizeof(*p));
    p->usage.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR;
    p->usage.videoUsageHints = VK_VIDEO_ENCODE_USAGE_TRANSCODING_BIT_KHR;
    p->usage.videoContentHints = VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR;
    p->usage.tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR;

    p->info.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    p->info.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    p->info.lumaBitDepth = depth;
    p->info.chromaBitDepth = depth;
    if (codec == VKENC_CODEC_HEVC)
    {
        p->h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR;
        p->h265.pNext = &p->usage;
        p->h265.stdProfileIdc = std_profile;
        p->info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
        p->info.pNext = &p->h265;
    }
    else
    {
        p->h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
        p->h264.pNext = &p->usage;
        p->h264.stdProfileIdc = std_profile;
        p->info.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
        p->info.pNext = &p->h264;
    }
    p->list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
    p->list.profileCount = 1;
    p->list.pProfiles = &p->info;
}

static VkResult get_codec_caps(const struct vkenc_instance *vk, VkPhysicalDevice phys,
                               const struct vkenc_profile *profile, struct vkenc_codec_caps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->video.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    caps->video.pNext = &caps->encode;
    caps->encode.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    caps->h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;
    caps->h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR;
    if (profile->info.videoCodecOperation == VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR)
        caps->encode.pNext = &caps->h265;
    else
        caps->encode.pNext = &caps->h264;
    return vk->vkGetPhysicalDeviceVideoCapabilitiesKHR(phys, &profile->info, &caps->video);
}

/* Finds a format for `usage` with the profile. Prefers `want` (and `tiling` when not -1). */
static int find_video_format(const struct vkenc_instance *vk, VkPhysicalDevice phys,
                             const struct vkenc_profile *profile, VkImageUsageFlags usage, VkFormat want,
                             int tiling, VkVideoFormatPropertiesKHR *out)
{
    VkPhysicalDeviceVideoFormatInfoKHR info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    VkVideoFormatPropertiesKHR *props;
    uint32_t count = 0, i;
    int found = -1;

    info.pNext = &profile->list;
    info.imageUsage = usage;
    if (vk->vkGetPhysicalDeviceVideoFormatPropertiesKHR(phys, &info, &count, NULL) != VK_SUCCESS || !count)
        return 0;
    if (!(props = calloc(count, sizeof(*props)))) return 0;
    for (i = 0; i < count; i++) props[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    if (vk->vkGetPhysicalDeviceVideoFormatPropertiesKHR(phys, &info, &count, props) < 0)
    {
        free(props);
        return 0;
    }
    for (i = 0; i < count && found < 0; i++)
        if ((want == VK_FORMAT_UNDEFINED || props[i].format == want)
                && (tiling < 0 || props[i].imageTiling == (VkImageTiling)tiling))
            found = i;
    /* A DPB may use another format than the source; take the first one offered. */
    if (found < 0 && want != VK_FORMAT_UNDEFINED && (usage & VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR))
        for (i = 0; i < count && found < 0; i++)
            if (tiling < 0 || props[i].imageTiling == (VkImageTiling)tiling) found = i;
    if (found >= 0)
    {
        *out = props[found];
        out->pNext = NULL;
    }
    free(props);
    return found >= 0;
}

/* ---- GPU selection -------------------------------------------------------------------------- */

static int has_extension(const VkExtensionProperties *exts, uint32_t count, const char *name)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        if (!strcmp(exts[i].extensionName, name)) return 1;
    return 0;
}

enum gpu_problem
{
    GPU_OK,
    GPU_NONE,
    GPU_OLD_API,
    GPU_NO_VIDEO_ENCODE,
    GPU_NO_CODEC,
    GPU_NO_PROFILE,
};

/* Checks one device; fills `gpu` and returns GPU_OK when it can encode the profile. */
static enum gpu_problem check_gpu(const struct vkenc_instance *vk, VkPhysicalDevice phys, vkenc_codec codec,
                                  const struct vkenc_profile *profile, struct vkenc_gpu *gpu)
{
    VkVideoCodecOperationFlagBitsKHR op = profile->info.videoCodecOperation;
    const char *codec_ext = codec == VKENC_CODEC_HEVC ? VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME
                                                      : VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkQueueFamilyProperties2 *families = NULL;
    VkQueueFamilyVideoPropertiesKHR *video_props = NULL;
    VkQueueFamilyQueryResultStatusPropertiesKHR *status_props = NULL;
    VkExtensionProperties *exts = NULL;
    struct vkenc_codec_caps caps;
    uint32_t count = 0, i, best_upload_score = 0;
    enum gpu_problem problem = GPU_OK;

    memset(gpu, 0, sizeof(*gpu));
    gpu->phys = phys;
    vk->vkGetPhysicalDeviceProperties2(phys, &props);
    gpu->props = props.properties;
    vk->vkGetPhysicalDeviceMemoryProperties(phys, &gpu->memory);
    if (gpu->props.apiVersion < VK_API_VERSION_1_3) return GPU_OLD_API;

    features.pNext = &f12;
    f12.pNext = &f13;
    vk->vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!f13.synchronization2) return GPU_OLD_API;
    gpu->host_query_reset = !!f12.hostQueryReset;

    vk->vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL);
    if (!(exts = calloc(count ? count : 1, sizeof(*exts)))) return GPU_NONE;
    vk->vkEnumerateDeviceExtensionProperties(phys, NULL, &count, exts);
    if (!has_extension(exts, count, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME)
            || !has_extension(exts, count, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME))
        problem = GPU_NO_VIDEO_ENCODE;
    else if (!has_extension(exts, count, codec_ext))
        problem = GPU_NO_CODEC;
    free(exts);
    if (problem != GPU_OK) return problem;

    count = 0;
    vk->vkGetPhysicalDeviceQueueFamilyProperties2(phys, &count, NULL);
    families = calloc(count, sizeof(*families));
    video_props = calloc(count, sizeof(*video_props));
    status_props = calloc(count, sizeof(*status_props));
    if (!families || !video_props || !status_props)
    {
        free(families);
        free(video_props);
        free(status_props);
        return GPU_NONE;
    }
    for (i = 0; i < count; i++)
    {
        families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        families[i].pNext = &video_props[i];
        video_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        video_props[i].pNext = &status_props[i];
        status_props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR;
    }
    vk->vkGetPhysicalDeviceQueueFamilyProperties2(phys, &count, families);

    gpu->encode_family = ~0u;
    for (i = 0; i < count; i++)
    {
        const VkQueueFamilyProperties *f = &families[i].queueFamilyProperties;
        if ((f->queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) && f->queueCount
                && (video_props[i].videoCodecOperations & op))
        {
            gpu->encode_family = i;
            gpu->encode_flags = f->queueFlags;
            gpu->query_status = !!status_props[i].queryResultStatusSupport;
            break;
        }
    }

    /* Pick where buffer-to-image copies run when the encode family cannot do them (RADV's encode
     * family only has the encode bit): a transfer-only family first, then compute, then graphics. */
    gpu->upload_family = ~0u;
    for (i = 0; i < count; i++)
    {
        const VkQueueFamilyProperties *f = &families[i].queueFamilyProperties;
        uint32_t score;

        if (!f->queueCount || (f->queueFlags & (VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_VIDEO_DECODE_BIT_KHR)))
            continue;
        if (f->queueFlags & VK_QUEUE_GRAPHICS_BIT) score = 1;
        else if (f->queueFlags & VK_QUEUE_COMPUTE_BIT) score = 2;
        else if (f->queueFlags & VK_QUEUE_TRANSFER_BIT) score = 3;
        else continue;
        if (score > best_upload_score)
        {
            best_upload_score = score;
            gpu->upload_family = i;
        }
    }
    free(families);
    free(video_props);
    free(status_props);

    if (gpu->encode_family == ~0u) return GPU_NO_CODEC;
    if (get_codec_caps(vk, phys, profile, &caps) != VK_SUCCESS) return GPU_NO_PROFILE;
    return GPU_OK;
}

/* Finds the GPU to encode `profile` on. Returns 0, or -1 with a plain sentence in `reason`. */
static int pick_gpu(const struct vkenc_instance *vk, vkenc_codec codec, const struct vkenc_profile *profile,
                    struct vkenc_gpu *out, char *reason, size_t len)
{
    VkPhysicalDevice *devices = NULL;
    enum gpu_problem worst = GPU_NONE;
    char problem_name[256] = "";
    uint32_t count = 0, i;
    int any = env_flag("NEUTRON_AMF_ANY_GPU"), best_score = -1;

    if (vk->vkEnumeratePhysicalDevices(vk->instance, &count, NULL) != VK_SUCCESS || !count)
    {
        set_reason(reason, len, "No Vulkan driver was found for any GPU.");
        return -1;
    }
    if (!(devices = calloc(count, sizeof(*devices)))) return -1;
    vk->vkEnumeratePhysicalDevices(vk->instance, &count, devices);

    for (i = 0; i < count; i++)
    {
        struct vkenc_gpu gpu;
        enum gpu_problem problem;
        VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        int score;

        vk->vkGetPhysicalDeviceProperties2(devices[i], &props);
        if (props.properties.vendorID != 0x1002 && !any) continue;
        problem = check_gpu(vk, devices[i], codec, profile, &gpu);
        TRACE("device %u: %s (%04x:%04x), result %d", i, props.properties.deviceName,
              props.properties.vendorID, props.properties.deviceID, problem);
        if (problem != GPU_OK)
        {
            /* Report the problem of the device that got furthest. */
            if (worst == GPU_NONE || problem > worst)
            {
                worst = problem;
                snprintf(problem_name, sizeof(problem_name), "%s", props.properties.deviceName);
            }
            continue;
        }
        score = (props.properties.vendorID == 0x1002) * 4
                + (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) * 2
                + (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        if (score > best_score)
        {
            best_score = score;
            *out = gpu;
        }
    }
    free(devices);
    if (best_score >= 0) return 0;

    switch (worst)
    {
    case GPU_NONE:
        set_reason(reason, len, any ? "No GPU with a working Vulkan driver was found."
                                    : "No AMD GPU was found (AMD hardware encoding needs an AMD graphics card).");
        break;
    case GPU_OLD_API:
        set_reason(reason, len, "The Vulkan driver for %s is too old (Vulkan 1.3 is needed for video encoding).",
                   problem_name);
        break;
    case GPU_NO_VIDEO_ENCODE:
        set_reason(reason, len, "The Vulkan driver for %s does not offer video encoding (Mesa may be too old, "
                   "or this GPU's firmware needs RADV_PERFTEST=video_encode).", problem_name);
        break;
    case GPU_NO_CODEC:
        set_reason(reason, len, "The Vulkan driver has no %s encoder (the distro may build Mesa without %s).",
                   codec_name(codec), codec_name(codec));
        break;
    case GPU_NO_PROFILE:
        set_reason(reason, len, "The Vulkan driver's %s encoder does not support the %s profile.",
                   codec_name(codec), profile_name(codec, codec == VKENC_CODEC_HEVC
                           ? (int)profile->h265.stdProfileIdc : (int)profile->h264.stdProfileIdc));
        break;
    default:
        break;
    }
    return -1;
}

/* ---- vkenc_query_caps ----------------------------------------------------------------------- */

static pthread_mutex_t caps_lock = PTHREAD_MUTEX_INITIALIZER;
static vkenc_caps caps_cache[3];
static int caps_cached[3];

static void probe_caps(vkenc_codec codec, vkenc_caps *caps)
{
    struct vkenc_instance vk;
    struct vkenc_profile profile, profile10;
    struct vkenc_codec_caps codec_caps;
    struct vkenc_gpu gpu;
    VkVideoFormatPropertiesKHR fmt;

    memset(caps, 0, sizeof(*caps));
    if (instance_create(&vk, caps->reason, sizeof(caps->reason))) return;

    profile_init(&profile, codec, std_profile_for(codec, 100, 8), 8);
    if (pick_gpu(&vk, codec, &profile, &gpu, caps->reason, sizeof(caps->reason)) && codec == VKENC_CODEC_H264)
    {
        /* No High profile: Main is still useful. */
        char reason[256];
        profile_init(&profile, codec, STD_VIDEO_H264_PROFILE_IDC_MAIN, 8);
        if (!pick_gpu(&vk, codec, &profile, &gpu, reason, sizeof(reason))) caps->reason[0] = 0;
    }
    if (caps->reason[0])
    {
        instance_destroy(&vk);
        return;
    }

    caps->vendor_id = gpu.props.vendorID;
    caps->device_id = gpu.props.deviceID;
    snprintf(caps->device_name, sizeof(caps->device_name), "%s", gpu.props.deviceName);
    if (get_codec_caps(&vk, gpu.phys, &profile, &codec_caps) != VK_SUCCESS)
    {
        set_reason(caps->reason, sizeof(caps->reason), "The Vulkan driver's %s encoder reported no capabilities.",
                   codec_name(codec));
        instance_destroy(&vk);
        return;
    }
    if (!find_video_format(&vk, gpu.phys, &profile, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                           VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, -1, &fmt))
    {
        set_reason(caps->reason, sizeof(caps->reason), "The Vulkan driver's %s encoder does not take NV12 frames.",
                   codec_name(codec));
        instance_destroy(&vk);
        return;
    }
    caps->available = 1;
    caps->min_width = codec_caps.video.minCodedExtent.width;
    caps->min_height = codec_caps.video.minCodedExtent.height;
    caps->max_width = codec_caps.video.maxCodedExtent.width;
    caps->max_height = codec_caps.video.maxCodedExtent.height;
    caps->max_bitrate = codec_caps.encode.maxBitrate;
    caps->supports_cbr = !!(codec_caps.encode.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR);
    caps->supports_vbr = !!(codec_caps.encode.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR);

    if (codec == VKENC_CODEC_HEVC)
    {
        profile_init(&profile10, codec, STD_VIDEO_H265_PROFILE_IDC_MAIN_10, 10);
        caps->supports_10bit = get_codec_caps(&vk, gpu.phys, &profile10, &codec_caps) == VK_SUCCESS
                && find_video_format(&vk, gpu.phys, &profile10, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                                     VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, -1, &fmt);
    }
    instance_destroy(&vk);
}

int vkenc_query_caps(vkenc_codec codec, vkenc_caps *caps)
{
    if (!caps) return -1;
    if (codec != VKENC_CODEC_H264 && codec != VKENC_CODEC_HEVC)
    {
        memset(caps, 0, sizeof(*caps));
        set_reason(caps->reason, sizeof(caps->reason), "This codec is not supported.");
        return 0;
    }
    pthread_mutex_lock(&caps_lock);
    if (!caps_cached[codec])
    {
        probe_caps(codec, &caps_cache[codec]);
        caps_cached[codec] = 1;
    }
    *caps = caps_cache[codec];
    pthread_mutex_unlock(&caps_lock);
    return 0;
}

/* ---- levels --------------------------------------------------------------------------------- */

struct h264_level
{
    int value;                  /* level x 10, as AMF and level_idc use it */
    StdVideoH264LevelIdc idc;
    uint32_t max_mbps, max_fs, max_br; /* macroblocks/s, macroblocks, kbit/s (Baseline/Main) */
};

static const struct h264_level h264_levels[] =
{
    {10, STD_VIDEO_H264_LEVEL_IDC_1_0, 1485, 99, 64},
    {11, STD_VIDEO_H264_LEVEL_IDC_1_1, 3000, 396, 192},
    {12, STD_VIDEO_H264_LEVEL_IDC_1_2, 6000, 396, 384},
    {13, STD_VIDEO_H264_LEVEL_IDC_1_3, 11880, 396, 768},
    {20, STD_VIDEO_H264_LEVEL_IDC_2_0, 11880, 396, 2000},
    {21, STD_VIDEO_H264_LEVEL_IDC_2_1, 19800, 792, 4000},
    {22, STD_VIDEO_H264_LEVEL_IDC_2_2, 20250, 1620, 4000},
    {30, STD_VIDEO_H264_LEVEL_IDC_3_0, 40500, 1620, 10000},
    {31, STD_VIDEO_H264_LEVEL_IDC_3_1, 108000, 3600, 14000},
    {32, STD_VIDEO_H264_LEVEL_IDC_3_2, 216000, 5120, 20000},
    {40, STD_VIDEO_H264_LEVEL_IDC_4_0, 245760, 8192, 20000},
    {41, STD_VIDEO_H264_LEVEL_IDC_4_1, 245760, 8192, 50000},
    {42, STD_VIDEO_H264_LEVEL_IDC_4_2, 522240, 8704, 50000},
    {50, STD_VIDEO_H264_LEVEL_IDC_5_0, 589824, 22080, 135000},
    {51, STD_VIDEO_H264_LEVEL_IDC_5_1, 983040, 36864, 240000},
    {52, STD_VIDEO_H264_LEVEL_IDC_5_2, 2073600, 36864, 240000},
    {60, STD_VIDEO_H264_LEVEL_IDC_6_0, 4177920, 139264, 240000},
    {61, STD_VIDEO_H264_LEVEL_IDC_6_1, 8355840, 139264, 480000},
    {62, STD_VIDEO_H264_LEVEL_IDC_6_2, 16711680, 139264, 800000},
};

struct h265_level
{
    int value;                  /* level x 30, as AMF and general_level_idc use it */
    StdVideoH265LevelIdc idc;
    uint64_t max_luma_ps, max_luma_sr;
    uint32_t max_br_main, max_br_high; /* kbit/s */
};

static const struct h265_level h265_levels[] =
{
    {30, STD_VIDEO_H265_LEVEL_IDC_1_0, 36864, 552960, 128, 0},
    {60, STD_VIDEO_H265_LEVEL_IDC_2_0, 122880, 3686400, 1500, 0},
    {63, STD_VIDEO_H265_LEVEL_IDC_2_1, 245760, 7372800, 3000, 0},
    {90, STD_VIDEO_H265_LEVEL_IDC_3_0, 552960, 16588800, 6000, 0},
    {93, STD_VIDEO_H265_LEVEL_IDC_3_1, 983040, 33177600, 10000, 0},
    {120, STD_VIDEO_H265_LEVEL_IDC_4_0, 2228224, 66846720, 12000, 30000},
    {123, STD_VIDEO_H265_LEVEL_IDC_4_1, 2228224, 133693440, 20000, 50000},
    {150, STD_VIDEO_H265_LEVEL_IDC_5_0, 8912896, 267386880, 25000, 100000},
    {153, STD_VIDEO_H265_LEVEL_IDC_5_1, 8912896, 534773760, 40000, 160000},
    {156, STD_VIDEO_H265_LEVEL_IDC_5_2, 8912896, 1069547520, 60000, 240000},
    {180, STD_VIDEO_H265_LEVEL_IDC_6_0, 35651584, 1069547520, 60000, 240000},
    {183, STD_VIDEO_H265_LEVEL_IDC_6_1, 35651584, 2139095040, 120000, 480000},
    {186, STD_VIDEO_H265_LEVEL_IDC_6_2, 35651584, 4278190080ull, 240000, 800000},
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static uint64_t level_bitrate(const struct vkenc *enc)
{
    uint64_t rate = enc->params.target_bitrate;
    if (enc->params.rc == VKENC_RC_VBR && enc->params.peak_bitrate > rate) rate = enc->params.peak_bitrate;
    return rate;
}

/* The requested level, raised to the lowest one that holds the stream and capped at the device's
 * maximum. A level that is too low makes hardware decoders refuse the file. */
static StdVideoH264LevelIdc choose_h264_level(const struct vkenc *enc, int high)
{
    uint64_t mbs = (uint64_t)(enc->coded_width / 16) * (enc->coded_height / 16);
    uint64_t mbps = (mbs * enc->fps_num + enc->fps_den - 1) / enc->fps_den;
    uint64_t rate = level_bitrate(enc);
    uint32_t w = enc->coded_width / 16, h = enc->coded_height / 16;
    unsigned int i, need = ARRAY_SIZE(h264_levels) - 1, want = 0, chosen;

    for (i = 0; i < ARRAY_SIZE(h264_levels); i++)
    {
        const struct h264_level *l = &h264_levels[i];
        uint64_t max_br = (uint64_t)l->max_br * (high ? 1250 : 1000);
        if (mbs <= l->max_fs && mbps <= l->max_mbps && rate <= max_br
                && (uint64_t)w * w <= 8ull * l->max_fs && (uint64_t)h * h <= 8ull * l->max_fs)
        {
            need = i;
            break;
        }
    }
    for (i = 0; i < ARRAY_SIZE(h264_levels); i++)
        if (h264_levels[i].value == enc->params.level) want = i;
    chosen = want > need ? want : need;
    if (enc->params.level && h264_levels[chosen].value != enc->params.level)
        INFO("H.264 level %d.%d cannot hold %ux%u at %u/%u fps; using level %d.%d", enc->params.level / 10,
             enc->params.level % 10, enc->width, enc->height, enc->fps_num, enc->fps_den,
             h264_levels[chosen].value / 10, h264_levels[chosen].value % 10);
    if ((uint32_t)h264_levels[chosen].idc > (uint32_t)enc->caps.h264.maxLevelIdc)
    {
        INFO("the device's H.264 encoder stops at level idc %d", enc->caps.h264.maxLevelIdc);
        return enc->caps.h264.maxLevelIdc;
    }
    return h264_levels[chosen].idc;
}

static StdVideoH265LevelIdc choose_h265_level(const struct vkenc *enc, int high_tier)
{
    uint64_t ps = (uint64_t)enc->coded_width * enc->coded_height;
    uint64_t sr = (ps * enc->fps_num + enc->fps_den - 1) / enc->fps_den;
    uint64_t rate = level_bitrate(enc);
    unsigned int i, need = ARRAY_SIZE(h265_levels) - 1, want = 0, chosen;

    for (i = 0; i < ARRAY_SIZE(h265_levels); i++)
    {
        const struct h265_level *l = &h265_levels[i];
        uint64_t max_br = (uint64_t)(high_tier && l->max_br_high ? l->max_br_high : l->max_br_main) * 1000;
        if (ps <= l->max_luma_ps && sr <= l->max_luma_sr && rate <= max_br
                && (uint64_t)enc->coded_width * enc->coded_width <= 8 * l->max_luma_ps
                && (uint64_t)enc->coded_height * enc->coded_height <= 8 * l->max_luma_ps
                && (!high_tier || l->max_br_high))
        {
            need = i;
            break;
        }
    }
    for (i = 0; i < ARRAY_SIZE(h265_levels); i++)
        if (h265_levels[i].value == enc->params.level) want = i;
    chosen = want > need ? want : need;
    if (enc->params.level && h265_levels[chosen].value != enc->params.level)
        INFO("HEVC level %d.%d cannot hold %ux%u at %u/%u fps; using level %d.%d", enc->params.level / 30,
             enc->params.level % 30 / 3, enc->width, enc->height, enc->fps_num, enc->fps_den,
             h265_levels[chosen].value / 30, h265_levels[chosen].value % 30 / 3);
    if ((uint32_t)h265_levels[chosen].idc > (uint32_t)enc->caps.h265.maxLevelIdc)
    {
        INFO("the device's HEVC encoder stops at level idc %d", enc->caps.h265.maxLevelIdc);
        return enc->caps.h265.maxLevelIdc;
    }
    return h265_levels[chosen].idc;
}

/* ---- parameter sets ------------------------------------------------------------------------- */

static int vui_signal_type(const vkenc_params *p)
{
    return p->full_range == 1 || p->color_primaries >= 0 || p->transfer_characteristics >= 0
            || p->matrix_coefficients >= 0;
}

static int vui_color_description(const vkenc_params *p)
{
    return p->color_primaries >= 0 || p->transfer_characteristics >= 0 || p->matrix_coefficients >= 0;
}

/* Pixel aspect ratio for the VUI: returns aspect_ratio_idc (0 = none), fills w/h for Extended_SAR. */
static int vui_sar(const vkenc_params *p, uint16_t *w, uint16_t *h)
{
    uint32_t num = p->sar_num, den = p->sar_den, g;

    if (!num || !den) return 0;
    g = gcd32(num, den);
    num /= g;
    den /= g;
    if (num == 1 && den == 1) return 1;
    while (num > 0xffff || den > 0xffff)
    {
        num = (num + 1) / 2;
        den = (den + 1) / 2;
    }
    *w = num;
    *h = den;
    return 255; /* Extended_SAR */
}

static void build_h264_params(struct vkenc *enc, int std_profile)
{
    const vkenc_params *p = &enc->params;
    const VkVideoEncodeH264CapabilitiesKHR *caps = &enc->caps.h264;
    StdVideoH264SequenceParameterSetVui *vui = &enc->h264_vui;
    StdVideoH264SequenceParameterSet *sps = &enc->h264_sps;
    StdVideoH264PictureParameterSet *pps = &enc->h264_pps;
    int high = std_profile == STD_VIDEO_H264_PROFILE_IDC_HIGH;
    int cabac = std_profile != STD_VIDEO_H264_PROFILE_IDC_BASELINE
            && (caps->stdSyntaxFlags & VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR);
    uint16_t sar_w = 0, sar_h = 0;

    memset(vui, 0, sizeof(*vui));
    vui->aspect_ratio_idc = vui_sar(p, &sar_w, &sar_h);
    vui->flags.aspect_ratio_info_present_flag = vui->aspect_ratio_idc != 0;
    vui->sar_width = sar_w;
    vui->sar_height = sar_h;
    vui->flags.video_signal_type_present_flag = vui_signal_type(p);
    vui->video_format = 5; /* unspecified */
    vui->flags.video_full_range_flag = p->full_range == 1;
    vui->flags.color_description_present_flag = vui_color_description(p);
    vui->colour_primaries = p->color_primaries >= 0 ? p->color_primaries : 2;
    vui->transfer_characteristics = p->transfer_characteristics >= 0 ? p->transfer_characteristics : 2;
    vui->matrix_coefficients = p->matrix_coefficients >= 0 ? p->matrix_coefficients : 2;
    vui->flags.timing_info_present_flag = 1;
    vui->num_units_in_tick = enc->fps_den;
    vui->time_scale = enc->fps_num * 2;
    vui->flags.fixed_frame_rate_flag = 1;
    /* No bitstream_restriction: Mesa's SPS writer fills its motion vector limits with zeros. POC
     * type 2 already tells decoders there is no reordering. */

    memset(sps, 0, sizeof(*sps));
    sps->profile_idc = std_profile;
    sps->flags.constraint_set0_flag = std_profile == STD_VIDEO_H264_PROFILE_IDC_BASELINE;
    sps->flags.constraint_set1_flag = std_profile != STD_VIDEO_H264_PROFILE_IDC_HIGH; /* constrained baseline */
    if (p->profile == 257) /* AMF constrained high: no B frames */
        sps->flags.constraint_set4_flag = sps->flags.constraint_set5_flag = 1;
    sps->flags.frame_mbs_only_flag = 1;
    sps->flags.direct_8x8_inference_flag = 1;
    sps->flags.vui_parameters_present_flag = 1;
    sps->level_idc = choose_h264_level(enc, high);
    sps->chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
    sps->seq_parameter_set_id = 0;
    sps->log2_max_frame_num_minus4 = 4;
    sps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
    sps->max_num_ref_frames = 1;
    sps->pic_width_in_mbs_minus1 = enc->coded_width / 16 - 1;
    sps->pic_height_in_map_units_minus1 = enc->coded_height / 16 - 1;
    if (enc->coded_width != enc->width || enc->coded_height != enc->height)
    {
        sps->flags.frame_cropping_flag = 1;
        sps->frame_crop_right_offset = (enc->coded_width - enc->width) / 2;
        sps->frame_crop_bottom_offset = (enc->coded_height - enc->height) / 2;
    }
    sps->pSequenceParameterSetVui = vui;

    memset(pps, 0, sizeof(*pps));
    pps->flags.entropy_coding_mode_flag = !!cabac;
    pps->flags.transform_8x8_mode_flag = high
            && (caps->stdSyntaxFlags & VK_VIDEO_ENCODE_H264_STD_TRANSFORM_8X8_MODE_FLAG_SET_BIT_KHR);
    pps->flags.deblocking_filter_control_present_flag = 1;
    pps->seq_parameter_set_id = 0;
    pps->pic_parameter_set_id = 0;
    pps->num_ref_idx_l0_default_active_minus1 = 0;
    pps->num_ref_idx_l1_default_active_minus1 = 0;
}

static void build_h265_params(struct vkenc *enc, int std_profile)
{
    const vkenc_params *p = &enc->params;
    const VkVideoEncodeH265CapabilitiesKHR *caps = &enc->caps.h265;
    StdVideoH265SequenceParameterSetVui *vui = &enc->h265_vui;
    StdVideoH265SequenceParameterSet *sps = &enc->h265_sps;
    StdVideoH265PictureParameterSet *pps = &enc->h265_pps;
    StdVideoH265VideoParameterSet *vps = &enc->h265_vps;
    uint32_t ctb_log2, min_cb_log2, min_tb_log2, max_tb_log2;
    uint16_t sar_w = 0, sar_h = 0;
    int high_tier = p->tier == 1;

    /* Largest CTB the device offers; 8x8 minimum coding blocks; transform sizes from the caps. */
    if (caps->ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR) ctb_log2 = 6;
    else if (caps->ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR) ctb_log2 = 5;
    else ctb_log2 = 4;
    if (caps->transformBlockSizes & VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_4_BIT_KHR) min_tb_log2 = 2;
    else if (caps->transformBlockSizes & VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_8_BIT_KHR) min_tb_log2 = 3;
    else if (caps->transformBlockSizes & VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_16_BIT_KHR) min_tb_log2 = 4;
    else min_tb_log2 = 5;
    if (caps->transformBlockSizes & VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_32_BIT_KHR) max_tb_log2 = 5;
    else if (caps->transformBlockSizes & VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_16_BIT_KHR) max_tb_log2 = 4;
    else if (caps->transformBlockSizes & VK_VIDEO_ENCODE_H265_TRANSFORM_BLOCK_SIZE_8_BIT_KHR) max_tb_log2 = 3;
    else max_tb_log2 = 2;
    if (max_tb_log2 > ctb_log2) max_tb_log2 = ctb_log2;
    min_cb_log2 = min_tb_log2 + 1 > 3 ? min_tb_log2 + 1 : 3;
    if (min_cb_log2 > ctb_log2) min_cb_log2 = ctb_log2;

    memset(&enc->h265_ptl, 0, sizeof(enc->h265_ptl));
    enc->h265_ptl.flags.general_tier_flag = high_tier;
    enc->h265_ptl.flags.general_progressive_source_flag = 1;
    enc->h265_ptl.flags.general_frame_only_constraint_flag = 1;
    enc->h265_ptl.general_profile_idc = std_profile;
    enc->h265_ptl.general_level_idc = choose_h265_level(enc, high_tier);

    memset(&enc->h265_dpbm, 0, sizeof(enc->h265_dpbm));
    enc->h265_dpbm.max_dec_pic_buffering_minus1[0] = 1; /* the reference and the current picture */
    enc->h265_dpbm.max_num_reorder_pics[0] = 0;
    enc->h265_dpbm.max_latency_increase_plus1[0] = 0;

    memset(vps, 0, sizeof(*vps));
    vps->flags.vps_temporal_id_nesting_flag = 1;
    vps->flags.vps_sub_layer_ordering_info_present_flag = 1;
    vps->flags.vps_timing_info_present_flag = 1;
    vps->vps_video_parameter_set_id = 0;
    vps->vps_max_sub_layers_minus1 = 0;
    vps->vps_num_units_in_tick = enc->fps_den;
    vps->vps_time_scale = enc->fps_num;
    vps->pDecPicBufMgr = &enc->h265_dpbm;
    vps->pProfileTierLevel = &enc->h265_ptl;

    memset(vui, 0, sizeof(*vui));
    vui->aspect_ratio_idc = vui_sar(p, &sar_w, &sar_h);
    vui->flags.aspect_ratio_info_present_flag = vui->aspect_ratio_idc != 0;
    vui->sar_width = sar_w;
    vui->sar_height = sar_h;
    vui->flags.video_signal_type_present_flag = vui_signal_type(p);
    vui->video_format = 5;
    vui->flags.video_full_range_flag = p->full_range == 1;
    vui->flags.colour_description_present_flag = vui_color_description(p);
    vui->colour_primaries = p->color_primaries >= 0 ? p->color_primaries : 2;
    vui->transfer_characteristics = p->transfer_characteristics >= 0 ? p->transfer_characteristics : 2;
    vui->matrix_coeffs = p->matrix_coefficients >= 0 ? p->matrix_coefficients : 2;
    vui->flags.vui_timing_info_present_flag = 1;
    vui->vui_num_units_in_tick = enc->fps_den;
    vui->vui_time_scale = enc->fps_num;

    /* Temporal MV prediction only where the driver says it can set the flag (RADV cannot). */
    enc->h265_tmvp = !!(caps->stdSyntaxFlags & VK_VIDEO_ENCODE_H265_STD_SPS_TEMPORAL_MVP_ENABLED_FLAG_SET_BIT_KHR);

    memset(sps, 0, sizeof(*sps));
    sps->flags.sps_temporal_id_nesting_flag = 1;
    sps->flags.sps_sub_layer_ordering_info_present_flag = 1;
    sps->flags.amp_enabled_flag = 1;
    sps->flags.sample_adaptive_offset_enabled_flag =
            !!(caps->stdSyntaxFlags & VK_VIDEO_ENCODE_H265_STD_SAMPLE_ADAPTIVE_OFFSET_ENABLED_FLAG_SET_BIT_KHR);
    sps->flags.sps_temporal_mvp_enabled_flag = enc->h265_tmvp;
    sps->flags.strong_intra_smoothing_enabled_flag = 1;
    sps->flags.vui_parameters_present_flag = 1;
    sps->chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
    sps->pic_width_in_luma_samples = enc->coded_width;
    sps->pic_height_in_luma_samples = enc->coded_height;
    sps->sps_video_parameter_set_id = 0;
    sps->sps_max_sub_layers_minus1 = 0;
    sps->sps_seq_parameter_set_id = 0;
    sps->bit_depth_luma_minus8 = enc->bytes_per_sample == 2 ? 2 : 0;
    sps->bit_depth_chroma_minus8 = sps->bit_depth_luma_minus8;
    sps->log2_max_pic_order_cnt_lsb_minus4 = 4;
    sps->log2_min_luma_coding_block_size_minus3 = min_cb_log2 - 3;
    sps->log2_diff_max_min_luma_coding_block_size = ctb_log2 - min_cb_log2;
    sps->log2_min_luma_transform_block_size_minus2 = min_tb_log2 - 2;
    sps->log2_diff_max_min_luma_transform_block_size = max_tb_log2 - min_tb_log2;
    /* The deepest transform tree the CTB allows; RADV passes no depth to the firmware and writes
     * this value as-is, and it matches what radeonsi writes for VCN. */
    sps->max_transform_hierarchy_depth_inter = ctb_log2 - min_tb_log2;
    sps->max_transform_hierarchy_depth_intra = ctb_log2 - min_tb_log2;
    if (enc->coded_width != enc->width || enc->coded_height != enc->height)
    {
        sps->flags.conformance_window_flag = 1;
        sps->conf_win_right_offset = (enc->coded_width - enc->width) / 2;
        sps->conf_win_bottom_offset = (enc->coded_height - enc->height) / 2;
    }
    sps->pProfileTierLevel = &enc->h265_ptl;
    sps->pDecPicBufMgr = &enc->h265_dpbm;
    sps->pSequenceParameterSetVui = vui;

    memset(pps, 0, sizeof(*pps));
    pps->flags.cu_qp_delta_enabled_flag = 1; /* rate control adapts QP per block; RADV forces it too */
    pps->pps_pic_parameter_set_id = 0;
    pps->pps_seq_parameter_set_id = 0;
    pps->sps_video_parameter_set_id = 0;
    pps->num_ref_idx_l0_default_active_minus1 = 0;
    pps->num_ref_idx_l1_default_active_minus1 = 0;
    pps->diff_cu_qp_delta_depth = 0;
}

/* ---- device objects ------------------------------------------------------------------------- */

static int find_memory_type(const struct vkenc *enc, uint32_t type_bits, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided)
{
    const VkPhysicalDeviceMemoryProperties *mem = &enc->gpu.memory;
    int best = -1, best_score = -1;
    uint32_t i;

    for (i = 0; i < mem->memoryTypeCount; i++)
    {
        VkMemoryPropertyFlags flags = mem->memoryTypes[i].propertyFlags;
        int score;

        if (!(type_bits & (1u << i)) || (flags & required) != required) continue;
        score = 4 * ((flags & preferred) == preferred) + 2 * !(flags & avoided);
        if (score > best_score)
        {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static VkResult allocate_memory(struct vkenc *enc, const VkMemoryRequirements *req, VkMemoryPropertyFlags required,
                                VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided,
                                VkDeviceMemory *memory, VkMemoryPropertyFlags *flags)
{
    VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    int type = find_memory_type(enc, req->memoryTypeBits, required, preferred, avoided);

    if (type < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    info.allocationSize = req->size;
    info.memoryTypeIndex = type;
    if (flags) *flags = enc->gpu.memory.memoryTypes[type].propertyFlags;
    return enc->fn.vkAllocateMemory(enc->device, &info, NULL, memory);
}

static VkResult create_buffer(struct vkenc *enc, VkDeviceSize size, VkBufferUsageFlags usage, const void *pnext,
                              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                              VkMemoryPropertyFlags avoided, VkBuffer *buffer, VkDeviceMemory *memory,
                              uint8_t **map, VkMemoryPropertyFlags *flags)
{
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements req;
    VkResult res;

    info.pNext = pnext;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((res = enc->fn.vkCreateBuffer(enc->device, &info, NULL, buffer)) != VK_SUCCESS) return res;
    enc->fn.vkGetBufferMemoryRequirements(enc->device, *buffer, &req);
    if ((res = allocate_memory(enc, &req, required, preferred, avoided, memory, flags)) != VK_SUCCESS) return res;
    if ((res = enc->fn.vkBindBufferMemory(enc->device, *buffer, *memory, 0)) != VK_SUCCESS) return res;
    if (map) return enc->fn.vkMapMemory(enc->device, *memory, 0, VK_WHOLE_SIZE, 0, (void **)map);
    return VK_SUCCESS;
}

static VkResult create_view(struct vkenc *enc, VkImage image, VkFormat format, VkImageViewType type,
                            uint32_t layers, VkImageView *view)
{
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

    info.image = image;
    info.viewType = type;
    info.format = format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = layers;
    return enc->fn.vkCreateImageView(enc->device, &info, NULL, view);
}

static int load_device_functions(struct vkenc *enc)
{
#define X(name) \
    if (!(enc->fn.name = (PFN_##name)enc->vk.vkGetDeviceProcAddr(enc->device, #name))) \
    { \
        INFO("the driver lacks %s", #name); \
        return -1; \
    }
    VKENC_DEVICE_FUNCS
#undef X
    return 0;
}

static VkResult create_device(struct vkenc *enc)
{
    const char *exts[] =
    {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        enc->params.codec == VKENC_CODEC_HEVC ? VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME
                                              : VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
    };
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkDeviceQueueCreateInfo queues[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO},
                                         {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    float priority = 1.0f;
    VkResult res;

    f12.pNext = &f13;
    f12.hostQueryReset = enc->gpu.host_query_reset;
    f13.synchronization2 = VK_TRUE;

    queues[0].queueFamilyIndex = enc->gpu.encode_family;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    info.queueCreateInfoCount = 1;
    if (enc->upload == UPLOAD_COPY_QUEUE)
    {
        queues[1].queueFamilyIndex = enc->gpu.upload_family;
        queues[1].queueCount = 1;
        queues[1].pQueuePriorities = &priority;
        info.queueCreateInfoCount = 2;
    }
    info.pNext = &f12;
    info.pQueueCreateInfos = queues;
    info.enabledExtensionCount = ARRAY_SIZE(exts);
    info.ppEnabledExtensionNames = exts;
    if ((res = enc->vk.vkCreateDevice(enc->gpu.phys, &info, NULL, &enc->device)) != VK_SUCCESS)
    {
        enc->device = VK_NULL_HANDLE;
        return res;
    }
    if (load_device_functions(enc))
    {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)enc->vk.vkGetDeviceProcAddr(enc->device, "vkDestroyDevice");
        if (destroy) destroy(enc->device, NULL);
        enc->device = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    enc->fn.vkGetDeviceQueue(enc->device, enc->gpu.encode_family, 0, &enc->encode_queue);
    if (enc->upload == UPLOAD_COPY_QUEUE)
        enc->fn.vkGetDeviceQueue(enc->device, enc->gpu.upload_family, 0, &enc->upload_queue);
    return VK_SUCCESS;
}

/* Decides how pixels reach the source image. Default: a staging buffer copied into an optimal
 * image, on the encode queue when it can copy (NVIDIA), otherwise on a transfer/compute queue with
 * a semaphore (RADV's encode queue has no transfer bit). That is also how ffmpeg's Vulkan encoder
 * feeds RADV. NEUTRON_VKENC_UPLOAD=linear writes into a linear host-visible image instead, where
 * the driver offers one; NEUTRON_VKENC_UPLOAD=queue forces the separate-queue copy for testing. */
static int choose_upload(struct vkenc *enc, VkFormat want, VkVideoFormatPropertiesKHR *fmt)
{
    const char *force = getenv("NEUTRON_VKENC_UPLOAD");
    int encode_can_copy = !!(enc->gpu.encode_flags & VK_QUEUE_TRANSFER_BIT);

    if (force && !strcmp(force, "linear"))
    {
        if (find_video_format(&enc->vk, enc->gpu.phys, &enc->profile, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
                              want, VK_IMAGE_TILING_LINEAR, fmt))
        {
            enc->upload = UPLOAD_LINEAR;
            return 0;
        }
    }
    if (!find_video_format(&enc->vk, enc->gpu.phys, &enc->profile,
                           VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           want, VK_IMAGE_TILING_OPTIMAL, fmt))
        return -1;
    if (force && !strcmp(force, "queue") && enc->gpu.upload_family != ~0u) encode_can_copy = 0;
    if (encode_can_copy)
    {
        enc->upload = UPLOAD_COPY;
        enc->gpu.upload_family = enc->gpu.encode_family;
        return 0;
    }
    if (enc->gpu.upload_family == ~0u) return -1;
    enc->upload = UPLOAD_COPY_QUEUE;
    return 0;
}

static VkResult create_source_image(struct vkenc *enc, struct vkenc_slot *slot)
{
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    uint32_t families[2] = {enc->gpu.encode_family, enc->gpu.upload_family};
    VkMemoryRequirements req;
    VkMemoryPropertyFlags flags;
    VkResult res;

    info.pNext = &enc->profile.list;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = enc->src_format;
    info.extent.width = enc->coded_width;
    info.extent.height = enc->coded_height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (enc->upload == UPLOAD_LINEAR)
    {
        info.tiling = VK_IMAGE_TILING_LINEAR;
        info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
        info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    }
    else
    {
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (enc->upload == UPLOAD_COPY_QUEUE)
        {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        }
    }
    slot->src_layout = info.initialLayout;
    if ((res = enc->fn.vkCreateImage(enc->device, &info, NULL, &slot->src_image)) != VK_SUCCESS) return res;
    enc->fn.vkGetImageMemoryRequirements(enc->device, slot->src_image, &req);
    if (enc->upload == UPLOAD_LINEAR)
    {
        /* Plain write-combined system memory: the host streams in, the encoder reads once. */
        res = allocate_memory(enc, &req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              &slot->src_memory, &flags);
    }
    else
    {
        res = allocate_memory(enc, &req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &slot->src_memory, &flags);
    }
    if (res != VK_SUCCESS) return res;
    if ((res = enc->fn.vkBindImageMemory(enc->device, slot->src_image, slot->src_memory, 0)) != VK_SUCCESS)
        return res;
    if (enc->upload == UPLOAD_LINEAR)
    {
        VkImageSubresource sub = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0};

        enc->fn.vkGetImageSubresourceLayout(enc->device, slot->src_image, &sub, &slot->src_planes[0]);
        sub.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        enc->fn.vkGetImageSubresourceLayout(enc->device, slot->src_image, &sub, &slot->src_planes[1]);
        if ((res = enc->fn.vkMapMemory(enc->device, slot->src_memory, 0, VK_WHOLE_SIZE, 0,
                                       (void **)&slot->src_map)) != VK_SUCCESS)
            return res;
    }
    return create_view(enc, slot->src_image, enc->src_format, VK_IMAGE_VIEW_TYPE_2D, 1, &slot->src_view);
}

static VkResult create_slot(struct vkenc *enc, struct vkenc_slot *slot)
{
    VkCommandBufferAllocateInfo cmd = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback = {
            VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
    VkQueryPoolCreateInfo query = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkMemoryPropertyFlags flags = 0;
    VkResult res;

    if ((res = create_source_image(enc, slot)) != VK_SUCCESS) return res;
    if (enc->upload != UPLOAD_LINEAR)
    {
        VkDeviceSize size = enc->staging_chroma_offset
                + (VkDeviceSize)enc->coded_width * enc->bytes_per_sample * (enc->coded_height / 2);
        if ((res = create_buffer(enc, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, NULL,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 &slot->staging, &slot->staging_memory, &slot->staging_map, NULL)) != VK_SUCCESS)
            return res;
    }
    /* The bitstream is read back by the CPU: cached memory if there is any. */
    if ((res = create_buffer(enc, enc->bitstream_size, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
                             &enc->profile.list, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             &slot->bitstream, &slot->bitstream_memory, &slot->bitstream_map, &flags)) != VK_SUCCESS)
        return res;
    enc->bitstream_coherent = !!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    feedback.pNext = &enc->profile.info;
    feedback.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR
            | VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
    query.pNext = &feedback;
    query.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
    query.queryCount = 1;
    if ((res = enc->fn.vkCreateQueryPool(enc->device, &query, NULL, &slot->query)) != VK_SUCCESS) return res;

    cmd.commandPool = enc->encode_pool;
    cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd.commandBufferCount = 1;
    if ((res = enc->fn.vkAllocateCommandBuffers(enc->device, &cmd, &slot->encode_cmd)) != VK_SUCCESS) return res;
    if (enc->upload == UPLOAD_COPY_QUEUE)
    {
        cmd.commandPool = enc->upload_pool;
        if ((res = enc->fn.vkAllocateCommandBuffers(enc->device, &cmd, &slot->upload_cmd)) != VK_SUCCESS) return res;
        if ((res = enc->fn.vkCreateSemaphore(enc->device, &sem, NULL, &slot->upload_done)) != VK_SUCCESS) return res;
    }
    return enc->fn.vkCreateFence(enc->device, &fence, NULL, &slot->fence);
}

static VkResult create_dpb(struct vkenc *enc)
{
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements req;
    VkResult res;

    info.pNext = &enc->profile.list;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = enc->dpb_format;
    info.extent.width = enc->coded_width;
    info.extent.height = enc->coded_height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = VKENC_DPB_SLOTS;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((res = enc->fn.vkCreateImage(enc->device, &info, NULL, &enc->dpb_image)) != VK_SUCCESS) return res;
    enc->fn.vkGetImageMemoryRequirements(enc->device, enc->dpb_image, &req);
    if ((res = allocate_memory(enc, &req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &enc->dpb_memory, NULL)))
        return res;
    if ((res = enc->fn.vkBindImageMemory(enc->device, enc->dpb_image, enc->dpb_memory, 0)) != VK_SUCCESS)
        return res;
    return create_view(enc, enc->dpb_image, enc->dpb_format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VKENC_DPB_SLOTS,
                       &enc->dpb_view);
}

static VkResult create_session(struct vkenc *enc)
{
    VkVideoSessionCreateInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    VkVideoEncodeH264SessionCreateInfoKHR h264 = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_CREATE_INFO_KHR};
    VkVideoEncodeH265SessionCreateInfoKHR h265 = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_CREATE_INFO_KHR};
    VkVideoSessionMemoryRequirementsKHR reqs[16];
    VkBindVideoSessionMemoryInfoKHR binds[16];
    uint32_t count = 0, i;
    VkResult res;

    if (enc->params.codec == VKENC_CODEC_HEVC)
    {
        h265.useMaxLevelIdc = VK_TRUE;
        h265.maxLevelIdc = enc->h265_ptl.general_level_idc;
        info.pNext = &h265;
    }
    else
    {
        h264.useMaxLevelIdc = VK_TRUE;
        h264.maxLevelIdc = enc->h264_sps.level_idc;
        info.pNext = &h264;
    }
    info.queueFamilyIndex = enc->gpu.encode_family;
    info.pVideoProfile = &enc->profile.info;
    info.pictureFormat = enc->src_format;
    info.maxCodedExtent.width = enc->coded_width;
    info.maxCodedExtent.height = enc->coded_height;
    info.referencePictureFormat = enc->dpb_format;
    info.maxDpbSlots = VKENC_DPB_SLOTS;
    info.maxActiveReferencePictures = 1;
    info.pStdHeaderVersion = &enc->caps.video.stdHeaderVersion;
    if ((res = enc->fn.vkCreateVideoSessionKHR(enc->device, &info, NULL, &enc->session)) != VK_SUCCESS) return res;

    if ((res = enc->fn.vkGetVideoSessionMemoryRequirementsKHR(enc->device, enc->session, &count, NULL)) < 0)
        return res;
    if (count > ARRAY_SIZE(reqs)) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < count; i++)
    {
        memset(&reqs[i], 0, sizeof(reqs[i]));
        reqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    if ((res = enc->fn.vkGetVideoSessionMemoryRequirementsKHR(enc->device, enc->session, &count, reqs)) < 0)
        return res;
    for (i = 0; i < count; i++)
    {
        if ((res = allocate_memory(enc, &reqs[i].memoryRequirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                                   &enc->session_memory[i], NULL)) != VK_SUCCESS)
            return res;
        enc->session_memory_count++;
        memset(&binds[i], 0, sizeof(binds[i]));
        binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
        binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
        binds[i].memory = enc->session_memory[i];
        binds[i].memorySize = reqs[i].memoryRequirements.size;
    }
    if (count && (res = enc->fn.vkBindVideoSessionMemoryKHR(enc->device, enc->session, count, binds)) != VK_SUCCESS)
        return res;
    return VK_SUCCESS;
}

/* Creates the session parameters and fetches their bitstream (with start codes) from the driver. */
static VkResult create_session_params(struct vkenc *enc)
{
    VkVideoSessionParametersCreateInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    VkVideoEncodeQualityLevelInfoKHR quality = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR};
    VkVideoEncodeH264SessionParametersAddInfoKHR h264_add = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    VkVideoEncodeH264SessionParametersCreateInfoKHR h264 = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    VkVideoEncodeH265SessionParametersAddInfoKHR h265_add = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
    VkVideoEncodeH265SessionParametersCreateInfoKHR h265 = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR};
    VkVideoEncodeSessionParametersGetInfoKHR get = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR};
    VkVideoEncodeH264SessionParametersGetInfoKHR h264_get = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR};
    VkVideoEncodeH265SessionParametersGetInfoKHR h265_get = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR};
    VkVideoEncodeSessionParametersFeedbackInfoKHR feedback = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
    VkVideoEncodeH264SessionParametersFeedbackInfoKHR h264_feedback = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
    VkVideoEncodeH265SessionParametersFeedbackInfoKHR h265_feedback = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
    size_t size = 0;
    VkResult res;

    quality.qualityLevel = enc->quality_level;
    if (enc->params.codec == VKENC_CODEC_HEVC)
    {
        h265_add.stdVPSCount = 1;
        h265_add.pStdVPSs = &enc->h265_vps;
        h265_add.stdSPSCount = 1;
        h265_add.pStdSPSs = &enc->h265_sps;
        h265_add.stdPPSCount = 1;
        h265_add.pStdPPSs = &enc->h265_pps;
        h265.maxStdVPSCount = 1;
        h265.maxStdSPSCount = 1;
        h265.maxStdPPSCount = 1;
        h265.pParametersAddInfo = &h265_add;
        h265.pNext = &quality;
        info.pNext = &h265;

        h265_get.writeStdVPS = h265_get.writeStdSPS = h265_get.writeStdPPS = VK_TRUE;
        get.pNext = &h265_get;
        feedback.pNext = &h265_feedback;
    }
    else
    {
        h264_add.stdSPSCount = 1;
        h264_add.pStdSPSs = &enc->h264_sps;
        h264_add.stdPPSCount = 1;
        h264_add.pStdPPSs = &enc->h264_pps;
        h264.maxStdSPSCount = 1;
        h264.maxStdPPSCount = 1;
        h264.pParametersAddInfo = &h264_add;
        h264.pNext = &quality;
        info.pNext = &h264;

        h264_get.writeStdSPS = h264_get.writeStdPPS = VK_TRUE;
        get.pNext = &h264_get;
        feedback.pNext = &h264_feedback;
    }
    info.videoSession = enc->session;
    if ((res = enc->fn.vkCreateVideoSessionParametersKHR(enc->device, &info, NULL, &enc->session_params)))
        return res;

    get.videoSessionParameters = enc->session_params;
    if ((res = enc->fn.vkGetEncodedVideoSessionParametersKHR(enc->device, &get, NULL, &size, NULL)) != VK_SUCCESS)
        return res;
    if (!size || !(enc->headers = malloc(size))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = enc->fn.vkGetEncodedVideoSessionParametersKHR(enc->device, &get, &feedback, &size, enc->headers)))
        return res;
    enc->headers_size = size;
    if (size < 4 || enc->headers[0] || enc->headers[1] || (enc->headers[2] != 1 && enc->headers[3] != 1))
    {
        INFO("the driver's parameter sets have no Annex-B start code");
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (enc->params.codec == VKENC_CODEC_HEVC)
        TRACE("parameter sets: %zu bytes, overrides: any %d vps %d sps %d pps %d", size, feedback.hasOverrides,
              h265_feedback.hasStdVPSOverrides, h265_feedback.hasStdSPSOverrides, h265_feedback.hasStdPPSOverrides);
    else
        TRACE("parameter sets: %zu bytes, overrides: any %d sps %d pps %d", size, feedback.hasOverrides,
              h264_feedback.hasStdSPSOverrides, h264_feedback.hasStdPPSOverrides);
    return VK_SUCCESS;
}

static void setup_rate_control(struct vkenc *enc)
{
    VkVideoEncodeRateControlModeFlagsKHR modes = enc->caps.encode.rateControlModes;
    uint64_t max_rate = enc->caps.encode.maxBitrate ? enc->caps.encode.maxBitrate : ~0ull;
    uint64_t target = enc->params.target_bitrate, peak = enc->params.peak_bitrate;
    int hrd;

    if (enc->params.rc == VKENC_RC_VBR && (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR))
        enc->rc_mode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    else if (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
        enc->rc_mode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    else if (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
        enc->rc_mode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR; /* CBR asked: VBR with peak = target */
    else
        enc->rc_mode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
    if (enc->params.rc == VKENC_RC_VBR && enc->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
        INFO("the device has no VBR rate control; using %s", enc->rc_mode ? "CBR" : "the driver's default");
    if (enc->params.rc == VKENC_RC_CBR && enc->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
        INFO("the device has no CBR rate control; using %s", enc->rc_mode ? "VBR" : "the driver's default");

    if (!target) target = 10000000;
    if (target > max_rate) target = max_rate;
    if (enc->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR || enc->params.rc != VKENC_RC_VBR
            || peak < target)
        peak = target;
    if (peak > max_rate) peak = max_rate;

    memset(&enc->rc_layer, 0, sizeof(enc->rc_layer));
    enc->rc_layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
    enc->rc_layer.averageBitrate = target;
    enc->rc_layer.maxBitrate = peak;
    enc->rc_layer.frameRateNumerator = enc->fps_num;
    enc->rc_layer.frameRateDenominator = enc->fps_den;

    memset(&enc->rc_info, 0, sizeof(enc->rc_info));
    enc->rc_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
    enc->rc_info.rateControlMode = enc->rc_mode;
    if (enc->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
    {
        enc->rc_info.layerCount = 1;
        enc->rc_info.pLayers = &enc->rc_layer;
        enc->rc_info.virtualBufferSizeInMs = 1000;
        enc->rc_info.initialVirtualBufferSizeInMs = 500;
    }

    if (enc->params.codec == VKENC_CODEC_HEVC)
    {
        hrd = enc->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
                && (enc->caps.h265.flags & VK_VIDEO_ENCODE_H265_CAPABILITY_HRD_COMPLIANCE_BIT_KHR);
        memset(&enc->h265_rc, 0, sizeof(enc->h265_rc));
        enc->h265_rc.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR;
        enc->h265_rc.flags = VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_KHR
                | VK_VIDEO_ENCODE_H265_RATE_CONTROL_REFERENCE_PATTERN_FLAT_BIT_KHR
                | (hrd ? VK_VIDEO_ENCODE_H265_RATE_CONTROL_ATTEMPT_HRD_COMPLIANCE_BIT_KHR : 0);
        enc->h265_rc.gopFrameCount = enc->idr_period;
        enc->h265_rc.idrPeriod = enc->idr_period;
        enc->h265_rc.subLayerCount = 1;
        enc->rc_info.pNext = &enc->h265_rc;
        /* The codec layer structure is optional, but NVIDIA's driver fails the encode without it. */
        memset(&enc->h265_rc_layer, 0, sizeof(enc->h265_rc_layer));
        enc->h265_rc_layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR;
        enc->rc_layer.pNext = &enc->h265_rc_layer;
        enc->use_gop_remaining = enc->caps.h265.prefersGopRemainingFrames || enc->caps.h265.requiresGopRemainingFrames;
    }
    else
    {
        hrd = enc->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
                && (enc->caps.h264.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_HRD_COMPLIANCE_BIT_KHR);
        memset(&enc->h264_rc, 0, sizeof(enc->h264_rc));
        enc->h264_rc.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR;
        enc->h264_rc.flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR
                | VK_VIDEO_ENCODE_H264_RATE_CONTROL_REFERENCE_PATTERN_FLAT_BIT_KHR
                | (hrd ? VK_VIDEO_ENCODE_H264_RATE_CONTROL_ATTEMPT_HRD_COMPLIANCE_BIT_KHR : 0);
        enc->h264_rc.gopFrameCount = enc->idr_period;
        enc->h264_rc.idrPeriod = enc->idr_period;
        enc->h264_rc.temporalLayerCount = 1;
        enc->rc_info.pNext = &enc->h264_rc;
        memset(&enc->h264_rc_layer, 0, sizeof(enc->h264_rc_layer));
        enc->h264_rc_layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR;
        enc->rc_layer.pNext = &enc->h264_rc_layer;
        enc->use_gop_remaining = enc->caps.h264.prefersGopRemainingFrames || enc->caps.h264.requiresGopRemainingFrames;
    }
    if (enc->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) enc->use_gop_remaining = 0;

    memset(&enc->quality_info, 0, sizeof(enc->quality_info));
    enc->quality_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR;
    enc->quality_info.qualityLevel = enc->quality_level;
}

/* Quality level: NEUTRON_AMF_QUALITY_LEVEL picks one; otherwise the middle of the device's range,
 * rounded up (RADV: 4 levels -> 2 "quality", 3 levels -> 1 "balanced"). */
static uint32_t choose_quality_level(const struct vkenc *enc)
{
    uint32_t count = enc->caps.encode.maxQualityLevels, level;
    const char *env = getenv("NEUTRON_AMF_QUALITY_LEVEL");

    if (!count) return 0;
    level = count / 2;
    if (env && *env) level = strtoul(env, NULL, 10);
    if (level >= count) level = count - 1;
    return level;
}

static void log_quality_level(struct vkenc *enc)
{
    VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR info = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR};
    VkVideoEncodeQualityLevelPropertiesKHR props = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR};
    VkVideoEncodeH264QualityLevelPropertiesKHR h264 = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR};
    VkVideoEncodeH265QualityLevelPropertiesKHR h265 = {
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR};

    if (!debug_enabled()) return;
    info.pVideoProfile = &enc->profile.info;
    info.qualityLevel = enc->quality_level;
    props.pNext = enc->params.codec == VKENC_CODEC_HEVC ? (void *)&h265 : (void *)&h264;
    if (enc->vk.vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(enc->gpu.phys, &info, &props))
        return;
    if (enc->params.codec == VKENC_CODEC_HEVC)
        TRACE("quality level %u of %u: preferred rc mode %#x, rc flags %#x, gop %u, idr %u, refs %u",
              enc->quality_level, enc->caps.encode.maxQualityLevels, props.preferredRateControlMode,
              h265.preferredRateControlFlags, h265.preferredGopFrameCount, h265.preferredIdrPeriod,
              h265.preferredMaxL0ReferenceCount);
    else
        TRACE("quality level %u of %u: preferred rc mode %#x, rc flags %#x, gop %u, idr %u, refs %u, cabac %u",
              enc->quality_level, enc->caps.encode.maxQualityLevels, props.preferredRateControlMode,
              h264.preferredRateControlFlags, h264.preferredGopFrameCount, h264.preferredIdrPeriod,
              h264.preferredMaxL0ReferenceCount, h264.preferredStdEntropyCodingModeFlag);
}

/* ---- create / destroy ----------------------------------------------------------------------- */

static void destroy_slot(struct vkenc *enc, struct vkenc_slot *slot)
{
    if (slot->fence) enc->fn.vkDestroyFence(enc->device, slot->fence, NULL);
    if (slot->upload_done) enc->fn.vkDestroySemaphore(enc->device, slot->upload_done, NULL);
    if (slot->query) enc->fn.vkDestroyQueryPool(enc->device, slot->query, NULL);
    if (slot->bitstream) enc->fn.vkDestroyBuffer(enc->device, slot->bitstream, NULL);
    if (slot->bitstream_memory) enc->fn.vkFreeMemory(enc->device, slot->bitstream_memory, NULL);
    if (slot->staging) enc->fn.vkDestroyBuffer(enc->device, slot->staging, NULL);
    if (slot->staging_memory) enc->fn.vkFreeMemory(enc->device, slot->staging_memory, NULL);
    if (slot->src_view) enc->fn.vkDestroyImageView(enc->device, slot->src_view, NULL);
    if (slot->src_image) enc->fn.vkDestroyImage(enc->device, slot->src_image, NULL);
    if (slot->src_memory) enc->fn.vkFreeMemory(enc->device, slot->src_memory, NULL);
    memset(slot, 0, sizeof(*slot));
}

void vkenc_destroy(vkenc *enc)
{
    uint32_t i;

    if (!enc) return;
    if (enc->device)
    {
        enc->fn.vkDeviceWaitIdle(enc->device);
        for (i = 0; i < enc->slot_count; i++) destroy_slot(enc, &enc->slots[i]);
        if (enc->encode_pool) enc->fn.vkDestroyCommandPool(enc->device, enc->encode_pool, NULL);
        if (enc->upload_pool) enc->fn.vkDestroyCommandPool(enc->device, enc->upload_pool, NULL);
        if (enc->session_params) enc->fn.vkDestroyVideoSessionParametersKHR(enc->device, enc->session_params, NULL);
        if (enc->session) enc->fn.vkDestroyVideoSessionKHR(enc->device, enc->session, NULL);
        for (i = 0; i < enc->session_memory_count; i++) enc->fn.vkFreeMemory(enc->device, enc->session_memory[i], NULL);
        if (enc->dpb_view) enc->fn.vkDestroyImageView(enc->device, enc->dpb_view, NULL);
        if (enc->dpb_image) enc->fn.vkDestroyImage(enc->device, enc->dpb_image, NULL);
        if (enc->dpb_memory) enc->fn.vkFreeMemory(enc->device, enc->dpb_memory, NULL);
        enc->fn.vkDestroyDevice(enc->device, NULL);
    }
    instance_destroy(&enc->vk);
    pthread_mutex_destroy(&enc->submit_lock);
    pthread_mutex_destroy(&enc->receive_lock);
    pthread_mutex_destroy(&enc->ring_lock);
    free(enc->headers);
    free(enc);
}

#define FAIL(...) \
    do { \
        set_reason(err, errlen, __VA_ARGS__); \
        goto fail; \
    } while (0)

int vkenc_create(const vkenc_params *params, vkenc **out, char *err, size_t errlen)
{
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkVideoFormatPropertiesKHR src_fmt, dpb_fmt;
    VkFormat want;
    uint32_t align_w, align_h, i;
    int bit_depth, std_profile;
    vkenc *enc;
    VkResult res;

    if (!params || !out) return -1;
    *out = NULL;
    if (params->codec != VKENC_CODEC_H264 && params->codec != VKENC_CODEC_HEVC)
    {
        set_reason(err, errlen, "Only H.264 and HEVC can be encoded.");
        return -1;
    }
    if (params->format != VKENC_FMT_NV12 && params->format != VKENC_FMT_P010)
    {
        set_reason(err, errlen, "Only NV12 and P010 frames can be encoded.");
        return -1;
    }
    if (params->codec == VKENC_CODEC_H264 && params->format == VKENC_FMT_P010)
    {
        set_reason(err, errlen, "10-bit H.264 is not supported; use HEVC Main10 for 10-bit video.");
        return -1;
    }
    if (!params->width || !params->height)
    {
        set_reason(err, errlen, "The frame size is zero.");
        return -1;
    }
    if (!(enc = calloc(1, sizeof(*enc))))
    {
        set_reason(err, errlen, "Out of memory.");
        return -1;
    }
    pthread_mutex_init(&enc->submit_lock, NULL);
    pthread_mutex_init(&enc->receive_lock, NULL);
    pthread_mutex_init(&enc->ring_lock, NULL);
    enc->params = *params;
    enc->src_width = params->width;
    enc->src_height = params->height;
    enc->width = align_up(params->width, 2);   /* 4:2:0 crops in steps of two */
    enc->height = align_up(params->height, 2);
    enc->fps_num = params->fps_num;
    enc->fps_den = params->fps_den;
    if (!enc->fps_num || !enc->fps_den)
    {
        enc->fps_num = 30;
        enc->fps_den = 1;
    }
    enc->idr_period = params->idr_period;
    if (!enc->idr_period) enc->idr_period = (2ull * enc->fps_num + enc->fps_den / 2) / enc->fps_den;
    if (!enc->idr_period) enc->idr_period = 1;
    if (params->interlaced) INFO("interlaced encoding is not supported; encoding progressive frames");
    if (enc->width != params->width || enc->height != params->height)
        INFO("4:2:0 needs an even frame size; encoding %ux%u as %ux%u", params->width, params->height,
             enc->width, enc->height);

    bit_depth = params->format == VKENC_FMT_P010 ? 10 : 8;
    enc->bytes_per_sample = bit_depth == 10 ? 2 : 1;
    want = bit_depth == 10 ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    std_profile = std_profile_for(params->codec, params->profile, bit_depth);
    profile_init(&enc->profile, params->codec, std_profile, bit_depth);

    if (instance_create(&enc->vk, err, errlen)) goto fail;
    if (pick_gpu(&enc->vk, params->codec, &enc->profile, &enc->gpu, err, errlen)) goto fail;
    if (get_codec_caps(&enc->vk, enc->gpu.phys, &enc->profile, &enc->caps) != VK_SUCCESS)
        FAIL("The Vulkan driver's %s encoder does not support the %s profile.", codec_name(params->codec),
             profile_name(params->codec, std_profile));
    if (enc->caps.video.maxDpbSlots < VKENC_DPB_SLOTS || enc->caps.video.maxActiveReferencePictures < 1
            || (params->codec == VKENC_CODEC_H264 ? !enc->caps.h264.maxPPictureL0ReferenceCount
                                                  : !enc->caps.h265.maxPPictureL0ReferenceCount))
        FAIL("The Vulkan driver's %s encoder cannot use reference frames.", codec_name(params->codec));

    /* Coded size: the codec's block size, the driver's picture access granularity and its input
     * granularity. The SPS crops back to the visible size. */
    align_w = lcm32(params->codec == VKENC_CODEC_HEVC ? 8 : 16, enc->caps.video.pictureAccessGranularity.width);
    align_h = lcm32(params->codec == VKENC_CODEC_HEVC ? 8 : 16, enc->caps.video.pictureAccessGranularity.height);
    align_w = lcm32(align_w, enc->caps.encode.encodeInputPictureGranularity.width);
    align_h = lcm32(align_h, enc->caps.encode.encodeInputPictureGranularity.height);
    enc->coded_width = align_up(enc->width, align_w);
    enc->coded_height = align_up(enc->height, align_h);
    if (enc->coded_width < enc->caps.video.minCodedExtent.width)
        enc->coded_width = align_up(enc->caps.video.minCodedExtent.width, align_w);
    if (enc->coded_height < enc->caps.video.minCodedExtent.height)
        enc->coded_height = align_up(enc->caps.video.minCodedExtent.height, align_h);
    if (enc->coded_width > enc->caps.video.maxCodedExtent.width
            || enc->coded_height > enc->caps.video.maxCodedExtent.height)
        FAIL("The GPU's %s encoder cannot encode %ux%u (its limit is %ux%u).", codec_name(params->codec),
             enc->width, enc->height, enc->caps.video.maxCodedExtent.width, enc->caps.video.maxCodedExtent.height);

    if (choose_upload(enc, want, &src_fmt))
        FAIL("The Vulkan driver's %s encoder does not take %s frames.", codec_name(params->codec),
             bit_depth == 10 ? "P010" : "NV12");
    enc->src_format = src_fmt.format;
    if (!find_video_format(&enc->vk, enc->gpu.phys, &enc->profile, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, want,
                           VK_IMAGE_TILING_OPTIMAL, &dpb_fmt)
            && !find_video_format(&enc->vk, enc->gpu.phys, &enc->profile, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
                                  want, -1, &dpb_fmt))
        FAIL("The Vulkan driver's %s encoder offers no reference picture format.", codec_name(params->codec));
    enc->dpb_format = dpb_fmt.format;

    enc->quality_level = choose_quality_level(enc);
    if (params->codec == VKENC_CODEC_HEVC) build_h265_params(enc, std_profile);
    else build_h264_params(enc, std_profile);
    setup_rate_control(enc);

    if ((res = create_device(enc)) != VK_SUCCESS)
        FAIL("The Vulkan device for video encoding could not be created (error %d).", res);
    log_quality_level(enc);

    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = enc->gpu.encode_family;
    if ((res = enc->fn.vkCreateCommandPool(enc->device, &pool, NULL, &enc->encode_pool)) != VK_SUCCESS)
        FAIL("Vulkan command pool creation failed (error %d).", res);
    if (enc->upload == UPLOAD_COPY_QUEUE)
    {
        pool.queueFamilyIndex = enc->gpu.upload_family;
        if ((res = enc->fn.vkCreateCommandPool(enc->device, &pool, NULL, &enc->upload_pool)) != VK_SUCCESS)
            FAIL("Vulkan command pool creation failed (error %d).", res);
    }

    if ((res = create_session(enc)) != VK_SUCCESS)
        FAIL("The %s video session could not be created (error %d).", codec_name(params->codec), res);
    if ((res = create_session_params(enc)) != VK_SUCCESS)
        FAIL("The %s parameter sets could not be created (error %d).", codec_name(params->codec), res);
    if ((res = create_dpb(enc)) != VK_SUCCESS)
        FAIL("The reference picture buffer could not be created (error %d).", res);

    /* The bitstream buffer holds one frame: an uncompressed 8-bit 4:2:0 frame's worth is far more
     * than any encoder writes at sane quantizers. */
    enc->bitstream_size = align_up64((uint64_t)enc->coded_width * enc->coded_height * 3 / 2 + 65536,
                                     enc->caps.video.minBitstreamBufferSizeAlignment);
    enc->staging_chroma_offset = align_up64((uint64_t)enc->coded_width * enc->bytes_per_sample * enc->coded_height,
                                            256);
    enc->slot_count = (uint64_t)enc->coded_width * enc->coded_height > 1920 * 1088 ? 4 : VKENC_MAX_SLOTS;
    for (i = 0; i < enc->slot_count; i++)
        if ((res = create_slot(enc, &enc->slots[i])) != VK_SUCCESS)
            FAIL("Video encoder buffers could not be created (error %d).", res);

    INFO("%s %s %ux%u (coded %ux%u) %u/%u fps, %s %llu/%llu bit/s, IDR every %u, quality level %u/%u, "
         "%s upload, on %s", codec_name(params->codec), profile_name(params->codec, std_profile), enc->width,
         enc->height, enc->coded_width, enc->coded_height, enc->fps_num, enc->fps_den,
         enc->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR ? "CBR"
         : enc->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR ? "VBR" : "default rate control",
         (unsigned long long)enc->rc_layer.averageBitrate, (unsigned long long)enc->rc_layer.maxBitrate,
         enc->idr_period, enc->quality_level, enc->caps.encode.maxQualityLevels,
         enc->upload == UPLOAD_LINEAR ? "linear" : enc->upload == UPLOAD_COPY ? "copy" : "queue-copy",
         enc->gpu.props.deviceName);
    *out = enc;
    return 0;

fail:
    if (err && errlen && !err[0]) set_reason(err, errlen, "The video encoder could not be created.");
    INFO("%s", err ? err : "create failed");
    vkenc_destroy(enc);
    return -1;
}

/* ---- per frame ------------------------------------------------------------------------------ */

/* Writes one plane row: the caller's samples, then the last sample repeated into the padding. */
static void write_row(uint8_t *dst, const uint8_t *src, uint32_t row_bytes, uint32_t pad_bytes, uint32_t elem)
{
    uint32_t i;

    memcpy(dst, src, row_bytes);
    if (!pad_bytes) return;
    if (elem == 1)
    {
        memset(dst + row_bytes, src[row_bytes - 1], pad_bytes);
        return;
    }
    for (i = 0; i < pad_bytes; i += elem) memcpy(dst + row_bytes + i, src + row_bytes - elem, elem);
}

static void write_plane(uint8_t *dst, size_t dst_pitch, const uint8_t *src, size_t src_pitch, uint32_t row_bytes,
                        uint32_t rows, uint32_t pad_bytes, uint32_t total_rows, uint32_t elem)
{
    uint32_t y;

    for (y = 0; y < total_rows; y++)
        write_row(dst + y * dst_pitch, src + (y < rows ? y : rows - 1) * src_pitch, row_bytes, pad_bytes, elem);
}

static void fill_slot(struct vkenc *enc, struct vkenc_slot *slot, const uint8_t *data, uint32_t pitch)
{
    uint32_t bps = enc->bytes_per_sample;
    uint32_t luma_bytes = enc->src_width * bps, chroma_bytes = (enc->src_width + 1) / 2 * 2 * bps;
    uint32_t luma_pad = enc->coded_width * bps - luma_bytes, chroma_pad = enc->coded_width * bps - chroma_bytes;
    const uint8_t *chroma = data + (size_t)pitch * enc->src_height;
    uint8_t *luma_dst, *chroma_dst;
    size_t luma_pitch, chroma_pitch;

    if (enc->upload == UPLOAD_LINEAR)
    {
        luma_dst = slot->src_map + slot->src_planes[0].offset;
        luma_pitch = slot->src_planes[0].rowPitch;
        chroma_dst = slot->src_map + slot->src_planes[1].offset;
        chroma_pitch = slot->src_planes[1].rowPitch;
    }
    else
    {
        luma_dst = slot->staging_map;
        luma_pitch = (size_t)enc->coded_width * bps;
        chroma_dst = slot->staging_map + enc->staging_chroma_offset;
        chroma_pitch = luma_pitch;
    }
    write_plane(luma_dst, luma_pitch, data, pitch, luma_bytes, enc->src_height, luma_pad, enc->coded_height, bps);
    write_plane(chroma_dst, chroma_pitch, chroma, pitch, chroma_bytes, (enc->src_height + 1) / 2, chroma_pad,
                enc->coded_height / 2, 2 * bps);
}

static VkImageMemoryBarrier2 image_barrier(VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                           VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                           VkImageLayout old_layout, VkImageLayout new_layout, uint32_t layers)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = layers;
    return b;
}

static void cmd_barriers(struct vkenc *enc, VkCommandBuffer cmd, const VkImageMemoryBarrier2 *images, uint32_t count,
                         const VkBufferMemoryBarrier2 *buffer)
{
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};

    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = images;
    dep.bufferMemoryBarrierCount = buffer ? 1 : 0;
    dep.pBufferMemoryBarriers = buffer;
    enc->fn.vkCmdPipelineBarrier2(cmd, &dep);
}

static void cmd_copy_to_source(struct vkenc *enc, struct vkenc_slot *slot, VkCommandBuffer cmd)
{
    VkBufferImageCopy regions[2];
    VkImageMemoryBarrier2 barrier;

    barrier = image_barrier(slot->src_image, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    cmd_barriers(enc, cmd, &barrier, 1, NULL);

    memset(regions, 0, sizeof(regions));
    regions[0].bufferRowLength = enc->coded_width;
    regions[0].bufferImageHeight = enc->coded_height;
    regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regions[0].imageSubresource.layerCount = 1;
    regions[0].imageExtent.width = enc->coded_width;
    regions[0].imageExtent.height = enc->coded_height;
    regions[0].imageExtent.depth = 1;
    regions[1].bufferOffset = enc->staging_chroma_offset;
    regions[1].bufferRowLength = enc->coded_width / 2;
    regions[1].bufferImageHeight = enc->coded_height / 2;
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regions[1].imageSubresource.layerCount = 1;
    regions[1].imageExtent.width = enc->coded_width / 2;
    regions[1].imageExtent.height = enc->coded_height / 2;
    regions[1].imageExtent.depth = 1;
    enc->fn.vkCmdCopyBufferToImage(cmd, slot->staging, slot->src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                                   regions);
}

/* Everything one encode needs, kept in one place so the pointers stay valid while recording. */
struct frame_info
{
    VkVideoPictureResourceInfoKHR src;
    VkVideoPictureResourceInfoKHR dpb[VKENC_DPB_SLOTS];
    VkVideoReferenceSlotInfoKHR setup;
    VkVideoReferenceSlotInfoKHR ref;
    VkVideoReferenceSlotInfoKHR begin_slots[2];
    VkVideoEncodeH264DpbSlotInfoKHR h264_setup_slot, h264_ref_slot;
    StdVideoEncodeH264ReferenceInfo h264_setup_info, h264_ref_info;
    StdVideoEncodeH264ReferenceListsInfo h264_lists;
    StdVideoEncodeH264SliceHeader h264_slice;
    VkVideoEncodeH264NaluSliceInfoKHR h264_nalu;
    StdVideoEncodeH264PictureInfo h264_pic;
    VkVideoEncodeH264PictureInfoKHR h264_info;
    VkVideoEncodeH264GopRemainingFrameInfoKHR h264_gop;
    VkVideoEncodeH265DpbSlotInfoKHR h265_setup_slot, h265_ref_slot;
    StdVideoEncodeH265ReferenceInfo h265_setup_info, h265_ref_info;
    StdVideoEncodeH265ReferenceListsInfo h265_lists;
    StdVideoH265ShortTermRefPicSet h265_rps;
    StdVideoEncodeH265SliceSegmentHeader h265_slice;
    VkVideoEncodeH265NaluSliceSegmentInfoKHR h265_nalu;
    StdVideoEncodeH265PictureInfo h265_pic;
    VkVideoEncodeH265PictureInfoKHR h265_info;
    VkVideoEncodeH265GopRemainingFrameInfoKHR h265_gop;
};

static void fill_h264_frame(struct vkenc *enc, struct frame_info *f, int idr)
{
    uint32_t max_frame_num = 1u << (enc->h264_sps.log2_max_frame_num_minus4 + 4);
    uint32_t frame_num = enc->gop_frame % max_frame_num;
    int32_t poc = 2 * (int32_t)enc->gop_frame;  /* POC type 2: twice the decoding order */

    memset(&f->h264_lists, 0, sizeof(f->h264_lists));
    memset(f->h264_lists.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(f->h264_lists.RefPicList0));
    memset(f->h264_lists.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(f->h264_lists.RefPicList1));
    if (!idr) f->h264_lists.RefPicList0[0] = enc->ref_slot;

    memset(&f->h264_slice, 0, sizeof(f->h264_slice));
    f->h264_slice.slice_type = idr ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
    f->h264_slice.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
    f->h264_slice.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;

    memset(&f->h264_nalu, 0, sizeof(f->h264_nalu));
    f->h264_nalu.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
    f->h264_nalu.pStdSliceHeader = &f->h264_slice;

    memset(&f->h264_pic, 0, sizeof(f->h264_pic));
    f->h264_pic.flags.IdrPicFlag = idr;
    f->h264_pic.flags.is_reference = 1;
    f->h264_pic.seq_parameter_set_id = 0;
    f->h264_pic.pic_parameter_set_id = 0;
    f->h264_pic.idr_pic_id = enc->idr_count & 1;
    f->h264_pic.primary_pic_type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    f->h264_pic.frame_num = frame_num;
    f->h264_pic.PicOrderCnt = poc;
    f->h264_pic.pRefLists = &f->h264_lists;

    memset(&f->h264_info, 0, sizeof(f->h264_info));
    f->h264_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
    f->h264_info.naluSliceEntryCount = 1;
    f->h264_info.pNaluSliceEntries = &f->h264_nalu;
    f->h264_info.pStdPictureInfo = &f->h264_pic;

    memset(&f->h264_setup_info, 0, sizeof(f->h264_setup_info));
    f->h264_setup_info.primary_pic_type = f->h264_pic.primary_pic_type;
    f->h264_setup_info.FrameNum = frame_num;
    f->h264_setup_info.PicOrderCnt = poc;
    memset(&f->h264_setup_slot, 0, sizeof(f->h264_setup_slot));
    f->h264_setup_slot.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
    f->h264_setup_slot.pStdReferenceInfo = &f->h264_setup_info;
    f->setup.pNext = &f->h264_setup_slot;

    if (!idr)
    {
        uint32_t prev = enc->gop_frame - 1;
        memset(&f->h264_ref_info, 0, sizeof(f->h264_ref_info));
        f->h264_ref_info.primary_pic_type = prev ? STD_VIDEO_H264_PICTURE_TYPE_P : STD_VIDEO_H264_PICTURE_TYPE_IDR;
        f->h264_ref_info.FrameNum = prev % max_frame_num;
        f->h264_ref_info.PicOrderCnt = 2 * (int32_t)prev;
        memset(&f->h264_ref_slot, 0, sizeof(f->h264_ref_slot));
        f->h264_ref_slot.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
        f->h264_ref_slot.pStdReferenceInfo = &f->h264_ref_info;
        f->ref.pNext = &f->h264_ref_slot;
    }

    memset(&f->h264_gop, 0, sizeof(f->h264_gop));
    f->h264_gop.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_GOP_REMAINING_FRAME_INFO_KHR;
    f->h264_gop.useGopRemainingFrames = VK_TRUE;
    f->h264_gop.gopRemainingI = idr ? 1 : 0;
    f->h264_gop.gopRemainingP = enc->idr_period - enc->gop_frame - (idr ? 1 : 0);
}

static void fill_h265_frame(struct vkenc *enc, struct frame_info *f, int idr)
{
    int32_t poc = (int32_t)enc->gop_frame;

    memset(&f->h265_lists, 0, sizeof(f->h265_lists));
    memset(f->h265_lists.RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(f->h265_lists.RefPicList0));
    memset(f->h265_lists.RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(f->h265_lists.RefPicList1));
    if (!idr) f->h265_lists.RefPicList0[0] = enc->ref_slot;

    /* One short-term reference, the previous picture, sent in the slice header. */
    memset(&f->h265_rps, 0, sizeof(f->h265_rps));
    if (!idr)
    {
        f->h265_rps.num_negative_pics = 1;
        f->h265_rps.delta_poc_s0_minus1[0] = 0;
        f->h265_rps.used_by_curr_pic_s0_flag = 1;
    }

    memset(&f->h265_slice, 0, sizeof(f->h265_slice));
    f->h265_slice.flags.first_slice_segment_in_pic_flag = 1;
    f->h265_slice.flags.slice_sao_luma_flag = enc->h265_sps.flags.sample_adaptive_offset_enabled_flag;
    f->h265_slice.flags.slice_sao_chroma_flag = enc->h265_sps.flags.sample_adaptive_offset_enabled_flag;
    f->h265_slice.slice_type = idr ? STD_VIDEO_H265_SLICE_TYPE_I : STD_VIDEO_H265_SLICE_TYPE_P;
    f->h265_slice.MaxNumMergeCand = 5;

    memset(&f->h265_nalu, 0, sizeof(f->h265_nalu));
    f->h265_nalu.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR;
    f->h265_nalu.pStdSliceSegmentHeader = &f->h265_slice;

    memset(&f->h265_pic, 0, sizeof(f->h265_pic));
    f->h265_pic.flags.is_reference = 1;
    f->h265_pic.flags.IrapPicFlag = idr;
    f->h265_pic.flags.pic_output_flag = 1;
    f->h265_pic.flags.slice_temporal_mvp_enabled_flag = !idr && enc->h265_tmvp;
    f->h265_pic.pic_type = idr ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
    f->h265_pic.sps_video_parameter_set_id = 0;
    f->h265_pic.pps_seq_parameter_set_id = 0;
    f->h265_pic.pps_pic_parameter_set_id = 0;
    f->h265_pic.PicOrderCntVal = poc;
    f->h265_pic.pRefLists = &f->h265_lists;
    f->h265_pic.pShortTermRefPicSet = &f->h265_rps;

    memset(&f->h265_info, 0, sizeof(f->h265_info));
    f->h265_info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR;
    f->h265_info.naluSliceSegmentEntryCount = 1;
    f->h265_info.pNaluSliceSegmentEntries = &f->h265_nalu;
    f->h265_info.pStdPictureInfo = &f->h265_pic;

    memset(&f->h265_setup_info, 0, sizeof(f->h265_setup_info));
    f->h265_setup_info.pic_type = f->h265_pic.pic_type;
    f->h265_setup_info.PicOrderCntVal = poc;
    memset(&f->h265_setup_slot, 0, sizeof(f->h265_setup_slot));
    f->h265_setup_slot.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
    f->h265_setup_slot.pStdReferenceInfo = &f->h265_setup_info;
    f->setup.pNext = &f->h265_setup_slot;

    if (!idr)
    {
        memset(&f->h265_ref_info, 0, sizeof(f->h265_ref_info));
        f->h265_ref_info.pic_type = poc > 1 ? STD_VIDEO_H265_PICTURE_TYPE_P : STD_VIDEO_H265_PICTURE_TYPE_IDR;
        f->h265_ref_info.PicOrderCntVal = poc - 1;
        memset(&f->h265_ref_slot, 0, sizeof(f->h265_ref_slot));
        f->h265_ref_slot.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
        f->h265_ref_slot.pStdReferenceInfo = &f->h265_ref_info;
        f->ref.pNext = &f->h265_ref_slot;
    }

    memset(&f->h265_gop, 0, sizeof(f->h265_gop));
    f->h265_gop.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_GOP_REMAINING_FRAME_INFO_KHR;
    f->h265_gop.useGopRemainingFrames = VK_TRUE;
    f->h265_gop.gopRemainingI = idr ? 1 : 0;
    f->h265_gop.gopRemainingP = enc->idr_period - enc->gop_frame - (idr ? 1 : 0);
}

static VkResult record_encode(struct vkenc *enc, struct vkenc_slot *slot, int idr)
{
    VkCommandBuffer cmd = slot->encode_cmd;
    VkCommandBufferBeginInfo begin_cmd = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkVideoBeginCodingInfoKHR begin = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    VkVideoCodingControlInfoKHR control = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    VkVideoEncodeInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    VkVideoEndCodingInfoKHR end = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VkBufferMemoryBarrier2 bitstream_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    VkImageMemoryBarrier2 barriers[2];
    VkAccessFlags2 dpb_access;
    uint32_t setup_slot = enc->have_ref ? enc->ref_slot ^ 1 : 0, i, nbarriers = 0;
    struct frame_info f;
    VkResult res;

    memset(&f, 0, sizeof(f));
    f.src.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
    f.src.codedExtent.width = enc->coded_width;
    f.src.codedExtent.height = enc->coded_height;
    f.src.imageViewBinding = slot->src_view;
    for (i = 0; i < VKENC_DPB_SLOTS; i++)
    {
        f.dpb[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        f.dpb[i].codedExtent = f.src.codedExtent;
        f.dpb[i].baseArrayLayer = i; /* DPB slot i lives in layer i (RADV before VCN 5 requires it) */
        f.dpb[i].imageViewBinding = enc->dpb_view;
    }
    f.setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    f.setup.slotIndex = setup_slot;
    f.setup.pPictureResource = &f.dpb[setup_slot];
    f.ref.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    f.ref.slotIndex = enc->ref_slot;
    f.ref.pPictureResource = &f.dpb[enc->ref_slot];
    if (enc->params.codec == VKENC_CODEC_HEVC) fill_h265_frame(enc, &f, idr);
    else fill_h264_frame(enc, &f, idr);

    begin_cmd.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if ((res = enc->fn.vkResetCommandBuffer(cmd, 0)) != VK_SUCCESS) return res;
    if ((res = enc->fn.vkBeginCommandBuffer(cmd, &begin_cmd)) != VK_SUCCESS) return res;
    if (!enc->gpu.host_query_reset) enc->fn.vkCmdResetQueryPool(cmd, slot->query, 0, 1);

    if (enc->upload == UPLOAD_COPY) cmd_copy_to_source(enc, slot, cmd);

    /* Source image to the encode-source layout. */
    switch (enc->upload)
    {
    case UPLOAD_LINEAR:
        /* Host writes are made visible by the queue submission itself. */
        barriers[nbarriers++] = image_barrier(slot->src_image, VK_PIPELINE_STAGE_2_NONE, 0,
                                              VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                                              VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR, slot->src_layout,
                                              VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, 1);
        break;
    case UPLOAD_COPY:
        barriers[nbarriers++] = image_barrier(slot->src_image, VK_PIPELINE_STAGE_2_COPY_BIT,
                                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                                              VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, 1);
        break;
    case UPLOAD_COPY_QUEUE:
        /* Chained to the semaphore wait, which waits at the video encode stage. */
        barriers[nbarriers++] = image_barrier(slot->src_image, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, 0,
                                              VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                                              VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, 1);
        break;
    }
    /* The DPB: initialized once, then each encode waits for the previous one's reconstruction. */
    dpb_access = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
    if (!enc->session_started)
        barriers[nbarriers++] = image_barrier(enc->dpb_image, VK_PIPELINE_STAGE_2_NONE, 0,
                                              VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, dpb_access,
                                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
                                              VKENC_DPB_SLOTS);
    else
        barriers[nbarriers++] = image_barrier(enc->dpb_image, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
                                              VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
                                              VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, dpb_access,
                                              VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
                                              VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR, VKENC_DPB_SLOTS);
    cmd_barriers(enc, cmd, barriers, nbarriers, NULL);

    /* Bound pictures: the reference (P frames) and the picture being reconstructed (no slot yet). */
    i = 0;
    if (!idr) f.begin_slots[i++] = f.ref;
    f.begin_slots[i] = f.setup;
    f.begin_slots[i++].slotIndex = -1;
    begin.videoSession = enc->session;
    begin.videoSessionParameters = enc->session_params;
    begin.referenceSlotCount = i;
    begin.pReferenceSlots = f.begin_slots;
    if (enc->session_started && enc->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
    {
        /* Repeat the rate control state that is in effect, as vkCmdBeginVideoCodingKHR requires. */
        if (enc->use_gop_remaining)
        {
            if (enc->params.codec == VKENC_CODEC_HEVC)
            {
                f.h265_gop.pNext = &enc->rc_info;
                begin.pNext = &f.h265_gop;
            }
            else
            {
                f.h264_gop.pNext = &enc->rc_info;
                begin.pNext = &f.h264_gop;
            }
        }
        else begin.pNext = &enc->rc_info;
    }
    enc->fn.vkCmdBeginVideoCodingKHR(cmd, &begin);

    if (!enc->session_started)
    {
        control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR | VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
        control.pNext = &enc->quality_info;
        if (enc->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
        {
            control.flags |= VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
            enc->quality_info.pNext = &enc->rc_info;
        }
        enc->fn.vkCmdControlVideoCodingKHR(cmd, &control);
    }

    info.pNext = enc->params.codec == VKENC_CODEC_HEVC ? (void *)&f.h265_info : (void *)&f.h264_info;
    info.dstBuffer = slot->bitstream;
    info.dstBufferOffset = 0;
    info.dstBufferRange = enc->bitstream_size;
    info.srcPictureResource = f.src;
    info.pSetupReferenceSlot = &f.setup;
    if (!idr)
    {
        info.referenceSlotCount = 1;
        info.pReferenceSlots = &f.ref;
    }
    enc->fn.vkCmdBeginQuery(cmd, slot->query, 0, 0);
    enc->fn.vkCmdEncodeVideoKHR(cmd, &info);
    enc->fn.vkCmdEndQuery(cmd, slot->query, 0);
    enc->fn.vkCmdEndVideoCodingKHR(cmd, &end);

    /* Make the bitstream visible to the host, and hand a linear source back to the host. */
    bitstream_barrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    bitstream_barrier.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
    bitstream_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    bitstream_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    bitstream_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bitstream_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bitstream_barrier.buffer = slot->bitstream;
    bitstream_barrier.size = VK_WHOLE_SIZE;
    nbarriers = 0;
    if (enc->upload == UPLOAD_LINEAR)
    {
        barriers[nbarriers++] = image_barrier(slot->src_image, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, 0,
                                              VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                                              VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL, 1);
        slot->src_layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    cmd_barriers(enc, cmd, barriers, nbarriers, &bitstream_barrier);
    return enc->fn.vkEndCommandBuffer(cmd);
}

static VkResult record_upload(struct vkenc *enc, struct vkenc_slot *slot)
{
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkResult res;

    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if ((res = enc->fn.vkResetCommandBuffer(slot->upload_cmd, 0)) != VK_SUCCESS) return res;
    if ((res = enc->fn.vkBeginCommandBuffer(slot->upload_cmd, &begin)) != VK_SUCCESS) return res;
    cmd_copy_to_source(enc, slot, slot->upload_cmd);
    return enc->fn.vkEndCommandBuffer(slot->upload_cmd);
}

static VkResult submit_slot(struct vkenc *enc, struct vkenc_slot *slot)
{
    VkCommandBufferSubmitInfo upload_cmd = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    VkCommandBufferSubmitInfo encode_cmd = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkResult res;

    if (enc->upload == UPLOAD_COPY_QUEUE)
    {
        upload_cmd.commandBuffer = slot->upload_cmd;
        signal.semaphore = slot->upload_done;
        signal.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &upload_cmd;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        if ((res = enc->fn.vkQueueSubmit2(enc->upload_queue, 1, &submit, VK_NULL_HANDLE)) != VK_SUCCESS) return res;

        memset(&submit, 0, sizeof(submit));
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        wait.semaphore = slot->upload_done;
        wait.stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
    }
    encode_cmd.commandBuffer = slot->encode_cmd;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &encode_cmd;
    return enc->fn.vkQueueSubmit2(enc->encode_queue, 1, &submit, slot->fence);
}

int vkenc_submit(vkenc *enc, const uint8_t *data, uint32_t pitch, int force_idr)
{
    struct vkenc_slot *slot;
    int idr, headers;
    VkResult res;

    if (!enc || !data) return -1;
    if (pitch < enc->src_width * enc->bytes_per_sample) return -1;
    pthread_mutex_lock(&enc->submit_lock);
    if (enc->failed)
    {
        pthread_mutex_unlock(&enc->submit_lock);
        return -1;
    }
    pthread_mutex_lock(&enc->ring_lock);
    if (enc->pending == enc->slot_count)
    {
        pthread_mutex_unlock(&enc->ring_lock);
        pthread_mutex_unlock(&enc->submit_lock);
        return 1;
    }
    slot = &enc->slots[enc->tail];
    pthread_mutex_unlock(&enc->ring_lock);

    /* The slot is idle: its last encode finished and was received. */
    fill_slot(enc, slot, data, pitch);

    idr = force_idr || !enc->have_ref || enc->gop_frame >= enc->idr_period;
    if (idr) enc->gop_frame = 0;
    headers = idr || (enc->params.header_spacing && enc->frames_since_headers >= enc->params.header_spacing);

    if (enc->gpu.host_query_reset) enc->fn.vkResetQueryPool(enc->device, slot->query, 0, 1);
    if ((res = enc->fn.vkResetFences(enc->device, 1, &slot->fence)) != VK_SUCCESS
            || (enc->upload == UPLOAD_COPY_QUEUE && (res = record_upload(enc, slot)) != VK_SUCCESS)
            || (res = record_encode(enc, slot, idr)) != VK_SUCCESS
            || (res = submit_slot(enc, slot)) != VK_SUCCESS)
    {
        INFO("frame %llu could not be %s (error %d)", (unsigned long long)enc->frames_submitted,
             res == VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR ? "encoded: the driver rejected its codec parameters"
                                                              : "submitted", res);
        enc->failed = 1;
        pthread_mutex_unlock(&enc->submit_lock);
        return -1;
    }
    slot->is_idr = idr;
    slot->with_headers = headers;

    /* This frame becomes the reference for the next one. */
    enc->ref_slot = enc->have_ref ? enc->ref_slot ^ 1 : 0;
    enc->have_ref = 1;
    enc->session_started = 1;
    enc->gop_frame++;
    enc->frames_since_headers = headers ? 1 : enc->frames_since_headers + 1;
    if (idr) enc->idr_count++;
    enc->frames_submitted++;

    pthread_mutex_lock(&enc->ring_lock);
    enc->tail = (enc->tail + 1) % enc->slot_count;
    enc->pending++;
    pthread_mutex_unlock(&enc->ring_lock);
    pthread_mutex_unlock(&enc->submit_lock);
    return 0;
}

int vkenc_receive(vkenc *enc, uint8_t **buf, size_t *len, int *is_idr)
{
    VkQueryResultFlags flags = 0;
    struct vkenc_slot *slot;
    uint32_t results[3] = {0};
    uint64_t offset, size;
    uint8_t *out;
    size_t headers;
    VkResult res;
    int ret = -1;

    if (!enc || !buf || !len) return -1;
    pthread_mutex_lock(&enc->receive_lock);
    pthread_mutex_lock(&enc->ring_lock);
    if (!enc->pending)
    {
        pthread_mutex_unlock(&enc->ring_lock);
        pthread_mutex_unlock(&enc->receive_lock);
        return 0;
    }
    slot = &enc->slots[enc->head];
    pthread_mutex_unlock(&enc->ring_lock);

    res = enc->fn.vkGetFenceStatus(enc->device, slot->fence);
    if (res == VK_NOT_READY)
    {
        pthread_mutex_unlock(&enc->receive_lock);
        return 0;
    }
    if (res != VK_SUCCESS)
    {
        INFO("the encode failed (fence status %d)", res);
        goto done;
    }

    /* 32-bit results: NVIDIA (615.71) packs offset and size into the first 64-bit result when
     * VK_QUERY_RESULT_64_BIT is set. Both values fit easily. */
    if (enc->gpu.query_status) flags |= VK_QUERY_RESULT_WITH_STATUS_BIT_KHR;
    res = enc->fn.vkGetQueryPoolResults(enc->device, slot->query, 0, 1, sizeof(results), results, sizeof(results),
                                        flags);
    if (res != VK_SUCCESS)
    {
        INFO("the encode feedback could not be read (error %d)", res);
        goto done;
    }
    if (enc->gpu.query_status && (int32_t)results[2] <= 0)
    {
        INFO("the encode failed (status %d%s)", (int32_t)results[2],
             (int32_t)results[2] == VK_QUERY_RESULT_STATUS_INSUFFICIENT_BITSTREAM_BUFFER_RANGE_KHR
             ? ", the bitstream buffer was too small" : "");
        goto done;
    }
    offset = results[0];
    size = results[1];
    if (offset + size > enc->bitstream_size)
    {
        INFO("the encoder reported %llu bytes at offset %llu, past the buffer", (unsigned long long)size,
             (unsigned long long)offset);
        goto done;
    }
    if (!enc->bitstream_coherent)
    {
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = slot->bitstream_memory;
        range.size = VK_WHOLE_SIZE;
        enc->fn.vkInvalidateMappedMemoryRanges(enc->device, 1, &range);
    }

    headers = slot->with_headers ? enc->headers_size : 0;
    if (!(out = malloc(headers + size + 1)))
        goto done;
    if (headers) memcpy(out, enc->headers, headers);
    memcpy(out + headers, slot->bitstream_map + offset, size);
    *buf = out;
    *len = headers + size;
    if (is_idr) *is_idr = slot->is_idr;
    ret = 1;

done:
    /* The slot is done either way; a failed frame must not wedge the ring. */
    pthread_mutex_lock(&enc->ring_lock);
    enc->head = (enc->head + 1) % enc->slot_count;
    enc->pending--;
    pthread_mutex_unlock(&enc->ring_lock);
    pthread_mutex_unlock(&enc->receive_lock);
    return ret;
}

void vkenc_free_packet(uint8_t *buf)
{
    free(buf);
}

int vkenc_drain(vkenc *enc)
{
    VkFence fences[VKENC_MAX_SLOTS];
    uint32_t count, i, idx;
    VkResult res;

    if (!enc) return -1;
    pthread_mutex_lock(&enc->submit_lock);
    pthread_mutex_lock(&enc->ring_lock);
    count = enc->pending;
    for (i = 0, idx = enc->head; i < count; i++, idx = (idx + 1) % enc->slot_count)
        fences[i] = enc->slots[idx].fence;
    pthread_mutex_unlock(&enc->ring_lock);
    /* Only vkenc_submit resets fences and we hold its lock, so these stay signaled once done. */
    res = count ? enc->fn.vkWaitForFences(enc->device, count, fences, VK_TRUE, VKENC_FENCE_TIMEOUT) : VK_SUCCESS;
    pthread_mutex_unlock(&enc->submit_lock);
    if (res != VK_SUCCESS)
    {
        INFO("draining the encoder failed (error %d)", res);
        return -1;
    }
    return 0;
}
