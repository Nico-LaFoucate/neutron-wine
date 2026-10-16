/*
 * vaenc.c - H.264/HEVC encoding through VA-API, the second backend behind vkenc.h.
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 *
 * Why it exists
 *
 * Mesa's Vulkan driver (RADV) hides its video encoder below a VCN firmware version (linux-firmware
 * reverted the RDNA2/RDNA3 VCN firmware twice in 2026), on the Steam Deck, and never offers it on
 * Polaris, Vega or VCN 1. Mesa's VA-API driver (radeonsi) drives the same hardware with no firmware
 * gate. encoder.c uses this backend when Vulkan Video does not offer the codec.
 *
 * How it works
 *
 * libva.so.2 and libva-drm.so.2 are loaded with dlopen(), so amfrt64 loads without them. Each encoder
 * opens the AMD render node (/dev/dri/renderD*, PCI vendor 0x1002 in sysfs; any GPU with
 * NEUTRON_AMF_ANY_GPU=1) and makes its own VADisplay, config and context.
 *
 * The parameters follow FFmpeg's vaapi_encode_h264.c / vaapi_encode_h265.c, the VA-API client
 * radeonsi is known to work with: IDR and P frames, one slice, one reference (the previous frame),
 * POC type 2 for H.264, three reconstructed surfaces used in turn. Rate control, HRD and frame rate
 * are misc parameter buffers sent with every IDR, like the sequence parameters.
 *
 * Parameter sets: like FFmpeg, the sequence header (SPS+PPS, or VPS+SPS+PPS) is written here, with
 * emulation prevention, and sent as a packed header on every IDR; a packed slice header goes with
 * every frame. Mesa reads the VUI (color, aspect ratio, timing) from the packed SPS. Mesa 24.3 and
 * later write the parameter sets in front of the slice only when the frame also has a packed slice
 * header (radeon_vcn_enc_encode_headers), which is why the slice header must be sent; Mesa 24.2 and
 * older advertise only packed sequence headers and write SPS/PPS on IDR frames themselves. Either
 * way the parameter sets in the output are the driver's own (it rewrites them from what it parsed),
 * so the ones from the last IDR are cached and put in front of frames that need them between IDRs
 * (header_spacing), and our own are used only if a driver writes none at all.
 *
 * Frames: vaenc_submit() copies the caller's pixels into a VAImage, vaPutImage()s it into the
 * slot's surface and issues the encode (vaBeginPicture/vaRenderPicture/vaEndPicture) without
 * waiting. A worker thread waits for each frame in submission order (vaSyncBuffer where the driver
 * implements it, otherwise vaSyncSurface with one frame in flight, as FFmpeg does), copies the
 * coded buffer out and queues the access unit for vaenc_receive().
 *
 * NOT TESTED ON AMD HARDWARE when written: the development machine's VA driver (NVIDIA) only
 * decodes. vkenc-test --backend vaapi and test-vkenc.sh (NEUTRON_AMF_BACKEND=vaapi) run it on AMD.
 */

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vkenc_backend.h"

#define VAENC_MAX_SLOTS   4     /* frames in flight */
#define VAENC_RECON       3     /* reconstructed surfaces, used in turn */
#define VAENC_MAX_QUEUED  16    /* finished access units waiting for vaenc_receive */
#define VAENC_MAX_NODES   16
#define VAENC_TIMEOUT_S   10    /* per frame, in vaenc_drain */

#define H264_LOG2_MAX_FRAME_NUM 8
#define HEVC_LOG2_MAX_POC_LSB   12
#define H264_INIT_QP            26  /* FFmpeg's QPs outside CQP mode; rate control overrides them */
#define HEVC_INIT_QP            30

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

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
    fputs("vaenc: ", stderr);
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

static const char *codec_name(vkenc_codec codec)
{
    return codec == VKENC_CODEC_HEVC ? "HEVC" : "H.264";
}

/* ---- loading libva -------------------------------------------------------------------------- */

#define VA_FUNCS \
    X(vaErrorStr) \
    X(vaSetErrorCallback) \
    X(vaSetInfoCallback) \
    X(vaInitialize) \
    X(vaTerminate) \
    X(vaQueryVendorString) \
    X(vaMaxNumProfiles) \
    X(vaMaxNumEntrypoints) \
    X(vaQueryConfigProfiles) \
    X(vaQueryConfigEntrypoints) \
    X(vaGetConfigAttributes) \
    X(vaCreateConfig) \
    X(vaDestroyConfig) \
    X(vaQuerySurfaceAttributes) \
    X(vaCreateSurfaces) \
    X(vaDestroySurfaces) \
    X(vaCreateContext) \
    X(vaDestroyContext) \
    X(vaCreateBuffer) \
    X(vaDestroyBuffer) \
    X(vaMapBuffer) \
    X(vaUnmapBuffer) \
    X(vaMaxNumImageFormats) \
    X(vaQueryImageFormats) \
    X(vaCreateImage) \
    X(vaDestroyImage) \
    X(vaPutImage) \
    X(vaBeginPicture) \
    X(vaRenderPicture) \
    X(vaEndPicture) \
    X(vaSyncSurface)

static struct
{
#define X(name) __typeof__(name) *name;
    VA_FUNCS
#undef X
    __typeof__(vaSyncBuffer) *vaSyncBuffer;         /* libva 2.9; NULL when older */
    __typeof__(vaGetDisplayDRM) *vaGetDisplayDRM;   /* libva-drm */
} va;

static pthread_once_t va_once = PTHREAD_ONCE_INIT;
static int va_loaded;

static void load_va(void)
{
    void *lib, *drm;

    if (!(lib = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL))) return;
    if (!(drm = dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL))) return;
#define X(name) \
    if (!(va.name = (__typeof__(va.name))dlsym(lib, #name))) \
    { \
        INFO("libva lacks %s", #name); \
        return; \
    }
    VA_FUNCS
#undef X
    va.vaSyncBuffer = (__typeof__(va.vaSyncBuffer))dlsym(lib, "vaSyncBuffer");
    if (!(va.vaGetDisplayDRM = (__typeof__(va.vaGetDisplayDRM))dlsym(drm, "vaGetDisplayDRM"))) return;
    va_loaded = 1;
}

/* libva prints its messages to stderr unless told otherwise; keep them for NEUTRON_VKENC_DEBUG. */
static void va_message(void *context, const char *message)
{
    size_t len = strlen(message);

    if (!debug_enabled()) return;
    while (len && (message[len - 1] == '\n' || message[len - 1] == '\r')) len--;
    log_msg(1, "libva %s: %.*s", context ? (const char *)context : "info", (int)len, message);
}

/* ---- devices -------------------------------------------------------------------------------- */

struct va_node
{
    char path[64];
    uint32_t vendor_id, device_id;
    unsigned int minor;
};

struct va_device
{
    int fd;
    VADisplay dpy;
    struct va_node node;
    char name[256];               /* from the driver's vendor string */
};

static uint32_t read_hex(const char *path)
{
    unsigned int value = 0;
    FILE *f = fopen(path, "r");

    if (!f) return 0;
    if (fscanf(f, "%x", &value) != 1) value = 0;
    fclose(f);
    return value;
}

static int node_compare(const void *a, const void *b)
{
    const struct va_node *x = a, *y = b;
    int amd_x = x->vendor_id == 0x1002, amd_y = y->vendor_id == 0x1002;

    if (amd_x != amd_y) return amd_y - amd_x;
    return (x->minor > y->minor) - (x->minor < y->minor);
}

/* Render nodes to try: AMD only, or every GPU (AMD first) with NEUTRON_AMF_ANY_GPU=1. */
static unsigned int find_nodes(struct va_node *nodes, unsigned int max)
{
    int any = env_flag("NEUTRON_AMF_ANY_GPU");
    unsigned int count = 0, minor;
    struct dirent *de;
    DIR *dir;

    if (!(dir = opendir("/sys/class/drm"))) return 0;
    while ((de = readdir(dir)) && count < max)
    {
        char path[128 + sizeof(de->d_name)];
        struct va_node *node = &nodes[count];
        char rest;

        if (sscanf(de->d_name, "renderD%u%c", &minor, &rest) != 1) continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", de->d_name);
        node->vendor_id = read_hex(path);
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device/device", de->d_name);
        node->device_id = read_hex(path);
        TRACE("%s: PCI %04x:%04x", de->d_name, node->vendor_id, node->device_id);
        if (node->vendor_id != 0x1002 && !any) continue;
        snprintf(node->path, sizeof(node->path), "/dev/dri/renderD%u", minor);
        node->minor = minor;
        count++;
    }
    closedir(dir);
    qsort(nodes, count, sizeof(*nodes), node_compare);
    return count;
}

/* "Mesa Gallium driver 26.2.4 for AMD Radeon RX 6750 XT (radeonsi, navi22, ...)" -> the GPU name. */
static void name_from_vendor_string(const char *vendor, char *name, size_t len)
{
    const char *start = vendor ? strstr(vendor, " for ") : NULL, *end;

    if (!vendor)
    {
        snprintf(name, len, "unknown GPU");
        return;
    }
    if (!start)
    {
        snprintf(name, len, "%s", vendor);
        return;
    }
    start += 5;
    if (!(end = strstr(start, " ("))) end = start + strlen(start);
    snprintf(name, len, "%.*s", (int)(end - start), start);
}

static void device_close(struct va_device *dev)
{
    if (dev->dpy) va.vaTerminate(dev->dpy);
    if (dev->fd >= 0) close(dev->fd);
    dev->dpy = NULL;
    dev->fd = -1;
}

static int device_open(struct va_device *dev, const struct va_node *node)
{
    int major, minor;
    VAStatus status;

    memset(dev, 0, sizeof(*dev));
    dev->node = *node;
    snprintf(dev->name, sizeof(dev->name), "GPU %04x:%04x", node->vendor_id, node->device_id);
    if ((dev->fd = open(node->path, O_RDWR | O_CLOEXEC)) < 0)
    {
        TRACE("%s: %s", node->path, strerror(errno));
        return -1;
    }
    if (!(dev->dpy = va.vaGetDisplayDRM(dev->fd)))
    {
        TRACE("%s: no VA display", node->path);
        device_close(dev);
        return -1;
    }
    va.vaSetErrorCallback(dev->dpy, va_message, (void *)"error");
    va.vaSetInfoCallback(dev->dpy, va_message, NULL);
    if ((status = va.vaInitialize(dev->dpy, &major, &minor)) != VA_STATUS_SUCCESS)
    {
        TRACE("%s: vaInitialize failed: %s", node->path, va.vaErrorStr(status));
        device_close(dev);
        return -1;
    }
    name_from_vendor_string(va.vaQueryVendorString(dev->dpy), dev->name, sizeof(dev->name));
    TRACE("%s: VA-API %d.%d, %s", node->path, major, minor, va.vaQueryVendorString(dev->dpy));
    return 0;
}

/* ---- capabilities --------------------------------------------------------------------------- */

static const VAProfile h264_profiles[] = {VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline};

static int has_profile(const VAProfile *list, int count, VAProfile profile)
{
    int i;
    for (i = 0; i < count; i++)
        if (list[i] == profile) return 1;
    return 0;
}

/* The encode entry point for a profile: EncSlice (radeonsi), else the low-power one. */
static int find_entrypoint(const struct va_device *dev, VAProfile profile, VAEntrypoint *out)
{
    VAEntrypoint *list;
    int count = 0, i, found = 0;

    if (!(list = calloc(va.vaMaxNumEntrypoints(dev->dpy) + 1, sizeof(*list)))) return 0;
    if (va.vaQueryConfigEntrypoints(dev->dpy, profile, list, &count) == VA_STATUS_SUCCESS)
    {
        for (i = 0; i < count && !found; i++)
            if (list[i] == VAEntrypointEncSlice) found = 1, *out = list[i];
        for (i = 0; i < count && !found; i++)
            if (list[i] == VAEntrypointEncSliceLP) found = 1, *out = list[i];
    }
    free(list);
    return found;
}

static uint32_t get_attrib(const struct va_device *dev, VAProfile profile, VAEntrypoint entrypoint,
                           VAConfigAttribType type)
{
    VAConfigAttrib attr = {type, VA_ATTRIB_NOT_SUPPORTED};

    if (va.vaGetConfigAttributes(dev->dpy, profile, entrypoint, &attr, 1) != VA_STATUS_SUCCESS)
        return VA_ATTRIB_NOT_SUPPORTED;
    return attr.value;
}

struct surface_limits
{
    uint32_t min_width, min_height, max_width, max_height;
    uint32_t align_width, align_height;   /* 1 when the driver says nothing */
};

/* Surface limits of a profile, from a temporary config (as FFmpeg reads the alignment). */
static void get_surface_limits(const struct va_device *dev, VAProfile profile, VAEntrypoint entrypoint,
                               struct surface_limits *limits)
{
    VASurfaceAttrib *attribs = NULL;
    unsigned int count = 0, i;
    VAConfigID config;

    memset(limits, 0, sizeof(*limits));
    limits->align_width = limits->align_height = 1;
    if (va.vaCreateConfig(dev->dpy, profile, entrypoint, NULL, 0, &config) != VA_STATUS_SUCCESS) return;
    if (va.vaQuerySurfaceAttributes(dev->dpy, config, NULL, &count) == VA_STATUS_SUCCESS && count
            && (attribs = calloc(count, sizeof(*attribs)))
            && va.vaQuerySurfaceAttributes(dev->dpy, config, attribs, &count) == VA_STATUS_SUCCESS)
    {
        for (i = 0; i < count; i++)
        {
            uint32_t value = attribs[i].value.value.i;

            if (attribs[i].value.type != VAGenericValueTypeInteger) continue;
            switch ((int)attribs[i].type)
            {
            case VASurfaceAttribMinWidth: limits->min_width = value; break;
            case VASurfaceAttribMinHeight: limits->min_height = value; break;
            case VASurfaceAttribMaxWidth: limits->max_width = value; break;
            case VASurfaceAttribMaxHeight: limits->max_height = value; break;
#if VA_CHECK_VERSION(1, 21, 0)
            case VASurfaceAttribAlignmentSize:
                limits->align_width = 1u << (value & 0xf);
                limits->align_height = 1u << ((value & 0xf0) >> 4);
                break;
#endif
            default: break;
            }
        }
    }
    free(attribs);
    va.vaDestroyConfig(dev->dpy, config);
}

/* What one device can do with one codec. */
static void probe_device(const struct va_device *dev, vkenc_codec codec, struct enc_probe *probe)
{
    vkenc_caps *caps = &probe->caps;
    VAProfile *profiles, profile = VAProfileNone;
    VAEntrypoint entrypoint = VAEntrypointEncSlice, ep;
    int count = 0, i, h264_listed = 0, hevc_listed = 0, any_encode = 0;
    struct surface_limits limits;
    uint32_t rt, rc, refs, value;

    memset(probe, 0, sizeof(*probe));
    caps->vendor_id = dev->node.vendor_id;
    caps->device_id = dev->node.device_id;
    snprintf(caps->device_name, sizeof(caps->device_name), "%s", dev->name);

    if (!(profiles = calloc(va.vaMaxNumProfiles(dev->dpy) + 1, sizeof(*profiles)))
            || va.vaQueryConfigProfiles(dev->dpy, profiles, &count) != VA_STATUS_SUCCESS)
        count = 0;
    for (i = 0; i < count; i++)
    {
        if (profiles[i] == VAProfileH264ConstrainedBaseline || profiles[i] == VAProfileH264Main
                || profiles[i] == VAProfileH264High)
            h264_listed = 1;
        if (profiles[i] == VAProfileHEVCMain || profiles[i] == VAProfileHEVCMain10) hevc_listed = 1;
        if (profiles[i] != VAProfileNone && find_entrypoint(dev, profiles[i], &ep)) any_encode = 1;
    }

    if (codec == VKENC_CODEC_HEVC)
    {
        if (has_profile(profiles, count, VAProfileHEVCMain) && find_entrypoint(dev, VAProfileHEVCMain, &entrypoint))
            profile = VAProfileHEVCMain;
    }
    else
    {
        for (i = 0; i < (int)ARRAY_SIZE(h264_profiles) && profile == VAProfileNone; i++)
            if (has_profile(profiles, count, h264_profiles[i]) && find_entrypoint(dev, h264_profiles[i], &entrypoint))
                profile = h264_profiles[i];
    }

    if (profile == VAProfileNone)
    {
        if (!h264_listed && !hevc_listed)
        {
            /* Every AMD GPU VA-API drives decodes H.264, so neither codec listed at all means Mesa
             * was built with video-codecs=all_free. */
            probe->problem = ENC_PROBLEM_CODEC_NOT_BUILT;
            set_reason(probe->clause, sizeof(probe->clause), "VA-API has no %s encoder (the distro builds Mesa "
                       "without %s, as Fedora and openSUSE do)", codec_name(codec), codec_name(codec));
        }
        else if (!any_encode)
        {
            probe->problem = ENC_PROBLEM_ENCODE_HIDDEN;
            set_reason(probe->clause, sizeof(probe->clause), "VA-API cannot encode video on this GPU");
        }
        else
        {
            probe->problem = ENC_PROBLEM_NO_CODEC;
            set_reason(probe->clause, sizeof(probe->clause), "VA-API has no %s encoder for this GPU",
                       codec_name(codec));
        }
        free(profiles);
        return;
    }

    rt = get_attrib(dev, profile, entrypoint, VAConfigAttribRTFormat);
    rc = get_attrib(dev, profile, entrypoint, VAConfigAttribRateControl);
    refs = get_attrib(dev, profile, entrypoint, VAConfigAttribEncMaxRefFrames);
    if (rt == VA_ATTRIB_NOT_SUPPORTED || !(rt & VA_RT_FORMAT_YUV420))
        set_reason(probe->clause, sizeof(probe->clause), "VA-API's %s encoder takes no 4:2:0 frames",
                   codec_name(codec));
    else if (rc == VA_ATTRIB_NOT_SUPPORTED || !(rc & (VA_RC_CBR | VA_RC_VBR)))
        set_reason(probe->clause, sizeof(probe->clause), "VA-API's %s encoder has no bit rate control",
                   codec_name(codec));
    else if (refs != VA_ATTRIB_NOT_SUPPORTED && !(refs & 0xffff))
        set_reason(probe->clause, sizeof(probe->clause), "VA-API's %s encoder cannot use reference frames",
                   codec_name(codec));
    if (probe->clause[0])
    {
        probe->problem = ENC_PROBLEM_OTHER;
        free(profiles);
        return;
    }

    get_surface_limits(dev, profile, entrypoint, &limits);
    caps->available = 1;
    caps->min_width = limits.min_width ? limits.min_width : 64;
    caps->min_height = limits.min_height ? limits.min_height : 64;
    value = get_attrib(dev, profile, entrypoint, VAConfigAttribMaxPictureWidth);
    caps->max_width = value != VA_ATTRIB_NOT_SUPPORTED && value ? value : limits.max_width ? limits.max_width : 4096;
    value = get_attrib(dev, profile, entrypoint, VAConfigAttribMaxPictureHeight);
    caps->max_height = value != VA_ATTRIB_NOT_SUPPORTED && value ? value : limits.max_height ? limits.max_height : 4096;
    caps->max_bitrate = 0;   /* VA-API does not say */
    caps->supports_cbr = !!(rc & VA_RC_CBR);
    caps->supports_vbr = !!(rc & VA_RC_VBR);
    if (codec == VKENC_CODEC_HEVC && has_profile(profiles, count, VAProfileHEVCMain10)
            && find_entrypoint(dev, VAProfileHEVCMain10, &ep))
    {
        rt = get_attrib(dev, VAProfileHEVCMain10, ep, VAConfigAttribRTFormat);
        caps->supports_10bit = rt != VA_ATTRIB_NOT_SUPPORTED && (rt & VA_RT_FORMAT_YUV420_10);
    }
    free(profiles);
}

static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static struct enc_probe probe_cache[3];
static int probe_done;

static void finish_reason(struct enc_probe *probe)
{
    char *reason = probe->caps.reason;
    size_t len;

    /* The full sentence: the clause with a capital and a period. */
    if (probe->caps.available) return;
    len = strnlen(probe->clause, sizeof(probe->clause));
    if (len > sizeof(probe->caps.reason) - 2) len = sizeof(probe->caps.reason) - 2;
    memcpy(reason, probe->clause, len);
    reason[len] = '.';
    reason[len + 1] = 0;
    if (reason[0] >= 'a' && reason[0] <= 'z') reason[0] -= 'a' - 'A';
}

static void probe_all(void)
{
    struct va_node nodes[VAENC_MAX_NODES];
    unsigned int count, i, codec;
    int opened = 0;

    for (codec = VKENC_CODEC_H264; codec <= VKENC_CODEC_HEVC; codec++)
        memset(&probe_cache[codec], 0, sizeof(probe_cache[codec]));

    pthread_once(&va_once, load_va);
    if (!va_loaded)
    {
        for (codec = VKENC_CODEC_H264; codec <= VKENC_CODEC_HEVC; codec++)
        {
            probe_cache[codec].problem = ENC_PROBLEM_NO_LIBRARY;
            set_reason(probe_cache[codec].clause, sizeof(probe_cache[codec].clause),
                       "VA-API (libva) is not installed");
        }
        goto done;
    }

    count = find_nodes(nodes, ARRAY_SIZE(nodes));
    for (i = 0; i < count; i++)
    {
        struct va_device dev;

        if (device_open(&dev, &nodes[i])) continue;
        opened++;
        for (codec = VKENC_CODEC_H264; codec <= VKENC_CODEC_HEVC; codec++)
        {
            struct enc_probe probe;

            if (probe_cache[codec].caps.available) continue;
            probe_device(&dev, codec, &probe);
            TRACE("%s: %s %s", dev.node.path, codec_name(codec), probe.caps.available ? "available" : probe.clause);
            /* Keep the device that got furthest. */
            if (probe.caps.available || !probe_cache[codec].caps.vendor_id
                    || probe.problem > probe_cache[codec].problem)
                probe_cache[codec] = probe;
        }
        device_close(&dev);
    }

    for (codec = VKENC_CODEC_H264; codec <= VKENC_CODEC_HEVC; codec++)
    {
        struct enc_probe *probe = &probe_cache[codec];

        if (probe->caps.available || probe->caps.vendor_id) continue;
        if (count && !opened)
        {
            probe->problem = ENC_PROBLEM_NO_DRIVER;
            set_reason(probe->clause, sizeof(probe->clause), "VA-API has no working driver for this GPU");
            probe->caps.vendor_id = nodes[0].vendor_id;
            probe->caps.device_id = nodes[0].device_id;
            snprintf(probe->caps.device_name, sizeof(probe->caps.device_name), "GPU %04x:%04x",
                     nodes[0].vendor_id, nodes[0].device_id);
        }
        else
        {
            probe->problem = ENC_PROBLEM_NO_GPU;
            set_reason(probe->clause, sizeof(probe->clause), env_flag("NEUTRON_AMF_ANY_GPU")
                       ? "VA-API found no GPU" : "VA-API found no AMD GPU");
        }
    }

done:
    for (codec = VKENC_CODEC_H264; codec <= VKENC_CODEC_HEVC; codec++) finish_reason(&probe_cache[codec]);
}

int vaenc_probe(vkenc_codec codec, struct enc_probe *probe)
{
    if (!probe) return -1;
    if (codec != VKENC_CODEC_H264 && codec != VKENC_CODEC_HEVC)
    {
        memset(probe, 0, sizeof(*probe));
        probe->problem = ENC_PROBLEM_OTHER;
        set_reason(probe->caps.reason, sizeof(probe->caps.reason), "This codec is not supported.");
        set_reason(probe->clause, sizeof(probe->clause), "this codec is not supported");
        return 0;
    }
    pthread_mutex_lock(&probe_lock);
    if (!probe_done)
    {
        probe_all();
        probe_done = 1;
    }
    *probe = probe_cache[codec];
    pthread_mutex_unlock(&probe_lock);
    return 0;
}

/* ---- levels (same tables as vkenc.c) -------------------------------------------------------- */

struct h264_level
{
    int idc;                           /* level x 10 */
    uint32_t max_mbps, max_fs, max_br; /* macroblocks/s, macroblocks, kbit/s (Baseline/Main) */
};

static const struct h264_level h264_levels[] =
{
    {10, 1485, 99, 64}, {11, 3000, 396, 192}, {12, 6000, 396, 384}, {13, 11880, 396, 768},
    {20, 11880, 396, 2000}, {21, 19800, 792, 4000}, {22, 20250, 1620, 4000}, {30, 40500, 1620, 10000},
    {31, 108000, 3600, 14000}, {32, 216000, 5120, 20000}, {40, 245760, 8192, 20000},
    {41, 245760, 8192, 50000}, {42, 522240, 8704, 50000}, {50, 589824, 22080, 135000},
    {51, 983040, 36864, 240000}, {52, 2073600, 36864, 240000}, {60, 4177920, 139264, 240000},
    {61, 8355840, 139264, 480000}, {62, 16711680, 139264, 800000},
};

struct h265_level
{
    int idc;                           /* level x 30 */
    uint64_t max_luma_ps, max_luma_sr;
    uint32_t max_br_main, max_br_high; /* kbit/s */
};

static const struct h265_level h265_levels[] =
{
    {30, 36864, 552960, 128, 0}, {60, 122880, 3686400, 1500, 0}, {63, 245760, 7372800, 3000, 0},
    {90, 552960, 16588800, 6000, 0}, {93, 983040, 33177600, 10000, 0},
    {120, 2228224, 66846720, 12000, 30000}, {123, 2228224, 133693440, 20000, 50000},
    {150, 8912896, 267386880, 25000, 100000}, {153, 8912896, 534773760, 40000, 160000},
    {156, 8912896, 1069547520, 60000, 240000}, {180, 35651584, 1069547520, 60000, 240000},
    {183, 35651584, 2139095040, 120000, 480000}, {186, 35651584, 4278190080ull, 240000, 800000},
};

/* ---- the encoder ---------------------------------------------------------------------------- */

enum slot_state
{
    SLOT_FREE,
    SLOT_ENCODING,   /* vaEndPicture done, the worker has not copied the output yet */
};

struct va_slot
{
    VASurfaceID surface;
    VAImage image;
    VABufferID coded;
    enum slot_state state;
    int is_idr;
    int with_headers;
    uint64_t frame;
};

struct va_packet
{
    struct va_packet *next;
    uint8_t *data;
    size_t size;
    int is_idr;
    int error;
};

/* Everything the parameter sets and slice headers say, chosen once in vaenc_create. */
struct va_seq
{
    int profile_idc;              /* H.264 profile_idc / HEVC general_profile_idc */
    int constraint_flags;         /* H.264: constraint_set0..5 as the six bits of the SPS */
    int level_idc;                /* H.264: level x 10; HEVC: level x 30 */
    int high_tier;
    int cabac, transform_8x8;
    uint32_t crop_right, crop_bottom;   /* H.264 frame_crop_* / HEVC conf_win_* (chroma units) */
    /* HEVC */
    uint32_t log2_min_cb, log2_ctb, log2_min_tb, log2_max_tb, depth_inter, depth_intra, ctu_size;
    int amp, sao, cu_qp_delta, diff_cu_qp_delta_depth, transform_skip;
    /* VUI */
    int aspect_ratio_idc;
    uint16_t sar_width, sar_height;
    int video_signal_type, full_range, colour_description, primaries, transfer, matrix;
};

/* Where one frame sits in the sequence. */
struct frame_state
{
    int idr;
    uint32_t gop_frame;           /* frames since the last IDR; POC (HEVC) or POC / 2 (H.264) */
    uint32_t idr_pic_id;
    uint32_t cur_recon, ref_recon;
};

struct vaenc
{
    vkenc_params params;
    struct va_device dev;
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigID config;
    VAContextID context;
    VASurfaceID recon[VAENC_RECON];
    int have_config, have_context, have_recon;
    uint32_t packed;              /* packed headers sent: VA_ENC_PACKED_HEADER_SEQUENCE / _SLICE */
    uint32_t rc_mode;             /* VA_RC_CBR or VA_RC_VBR */
    int quality_level;            /* 0: none sent */
    int async;                    /* vaSyncBuffer works: several frames in flight */
    struct va_seq seq;
    uint32_t bytes_per_sample;
    uint32_t src_width, src_height;     /* caller's frame size */
    uint32_t width, height;             /* visible size (even) */
    uint32_t coded_width, coded_height; /* surface size */
    uint32_t fps_num, fps_den, idr_period;
    uint64_t target_bitrate, peak_bitrate;
    size_t coded_size;

    /* Our own parameter sets (Annex-B), and the driver's from the last IDR (worker only). */
    uint8_t *own_headers;
    size_t own_headers_size;
    uint8_t *driver_headers;
    size_t driver_headers_size;
    int warned_no_headers;

    /* Sequence state, submit_lock. */
    int have_ref;
    uint32_t gop_frame, idr_count, frames_since_headers, recon_next, ref_recon;
    uint64_t frames_submitted;

    /* Slots and output, ring_lock. */
    struct va_slot slots[VAENC_MAX_SLOTS];
    uint32_t slot_count, submit_index, sync_index, encoding;
    struct va_packet *out_head, *out_tail;
    uint32_t out_count;
    int failed, stop;

    pthread_mutex_t submit_lock;
    pthread_mutex_t va_lock;      /* every VA call but the waits */
    pthread_mutex_t ring_lock;
    pthread_cond_t ring_cond;
    pthread_t worker;
    int worker_started;
};

/* ---- bit writer and NAL units --------------------------------------------------------------- */

struct bits
{
    uint8_t buf[1024];            /* parameter sets and slice headers are far smaller */
    size_t count;                 /* bits written */
    int overflow;
};

static void put_bits(struct bits *b, uint64_t value, unsigned int n)
{
    while (n--)
    {
        size_t byte = b->count >> 3;

        if (byte >= sizeof(b->buf))
        {
            b->overflow = 1;
            return;
        }
        if (!(b->count & 7)) b->buf[byte] = 0;
        if ((value >> n) & 1) b->buf[byte] |= 0x80 >> (b->count & 7);
        b->count++;
    }
}

static void put_ue(struct bits *b, uint32_t value)
{
    uint64_t x = (uint64_t)value + 1;
    unsigned int len = 0;

    while ((x >> len) > 1) len++;
    put_bits(b, 0, len);
    put_bits(b, x, len + 1);
}

static void put_se(struct bits *b, int32_t value)
{
    put_ue(b, value > 0 ? 2 * (uint32_t)value - 1 : 2 * (uint32_t)(-(int64_t)value));
}

/* rbsp_trailing_bits(), and HEVC's byte_alignment(): a one, then zeros to the byte boundary. */
static void put_trailing_bits(struct bits *b)
{
    put_bits(b, 1, 1);
    while (b->count & 7) put_bits(b, 0, 1);
}

struct nal_buffer
{
    uint8_t *data;
    size_t size, capacity;
    size_t padding_bits;          /* unused bits in the last byte (unaligned slice headers) */
    int failed;
};

static void nal_append(struct nal_buffer *out, const uint8_t *data, size_t size)
{
    if (out->failed) return;
    if (out->size + size > out->capacity)
    {
        size_t capacity = (out->size + size) * 2 + 64;
        uint8_t *grown = realloc(out->data, capacity);

        if (!grown)
        {
            out->failed = 1;
            return;
        }
        out->data = grown;
        out->capacity = capacity;
    }
    memcpy(out->data + out->size, data, size);
    out->size += size;
}

/* Appends a NAL unit (header included in `b`) with a four-byte start code and emulation prevention. */
static void nal_write(struct nal_buffer *out, const struct bits *b)
{
    static const uint8_t start_code[4] = {0, 0, 0, 1}, epb = 3;
    size_t bytes = (b->count + 7) / 8, i, zeros = 0;

    if (b->overflow) out->failed = 1;
    nal_append(out, start_code, sizeof(start_code));
    for (i = 0; i < bytes; i++)
    {
        if (zeros >= 2 && b->buf[i] <= 3)
        {
            nal_append(out, &epb, 1);
            zeros = 0;
        }
        nal_append(out, &b->buf[i], 1);
        zeros = b->buf[i] ? 0 : zeros + 1;
    }
    out->padding_bits = bytes * 8 - b->count;
}

/* ---- H.264 syntax --------------------------------------------------------------------------- */

static void write_h264_vui(struct bits *b, const struct vaenc *enc)
{
    const struct va_seq *s = &enc->seq;

    put_bits(b, s->aspect_ratio_idc != 0, 1);       /* aspect_ratio_info_present_flag */
    if (s->aspect_ratio_idc)
    {
        put_bits(b, s->aspect_ratio_idc, 8);
        if (s->aspect_ratio_idc == 255)
        {
            put_bits(b, s->sar_width, 16);
            put_bits(b, s->sar_height, 16);
        }
    }
    put_bits(b, 0, 1);                              /* overscan_info_present_flag */
    put_bits(b, s->video_signal_type, 1);
    if (s->video_signal_type)
    {
        put_bits(b, 5, 3);                          /* video_format: unspecified */
        put_bits(b, s->full_range, 1);
        put_bits(b, s->colour_description, 1);
        if (s->colour_description)
        {
            put_bits(b, s->primaries, 8);
            put_bits(b, s->transfer, 8);
            put_bits(b, s->matrix, 8);
        }
    }
    put_bits(b, 0, 1);                              /* chroma_loc_info_present_flag */
    put_bits(b, 1, 1);                              /* timing_info_present_flag */
    put_bits(b, enc->fps_den, 32);                  /* num_units_in_tick */
    put_bits(b, (uint64_t)enc->fps_num * 2, 32);    /* time_scale */
    put_bits(b, 1, 1);                              /* fixed_frame_rate_flag */
    put_bits(b, 0, 1);                              /* nal_hrd_parameters_present_flag */
    put_bits(b, 0, 1);                              /* vcl_hrd_parameters_present_flag */
    put_bits(b, 0, 1);                              /* pic_struct_present_flag */
    put_bits(b, 1, 1);                              /* bitstream_restriction_flag, as FFmpeg */
    put_bits(b, 1, 1);                              /* motion_vectors_over_pic_boundaries_flag */
    put_ue(b, 0);                                   /* max_bytes_per_pic_denom */
    put_ue(b, 0);                                   /* max_bits_per_mb_denom */
    put_ue(b, 15);                                  /* log2_max_mv_length_horizontal */
    put_ue(b, 15);                                  /* log2_max_mv_length_vertical */
    put_ue(b, 0);                                   /* max_num_reorder_frames */
    put_ue(b, 1);                                   /* max_dec_frame_buffering */
}

static void write_h264_sps(struct nal_buffer *out, const struct vaenc *enc)
{
    const struct va_seq *s = &enc->seq;
    struct bits b = {{0}};

    put_bits(&b, 0x67, 8);                          /* nal_ref_idc 3, nal_unit_type 7 */
    put_bits(&b, s->profile_idc, 8);
    put_bits(&b, s->constraint_flags, 6);
    put_bits(&b, 0, 2);                             /* reserved_zero_2bits */
    put_bits(&b, s->level_idc, 8);
    put_ue(&b, 0);                                  /* seq_parameter_set_id */
    if (s->profile_idc == 100)
    {
        put_ue(&b, 1);                              /* chroma_format_idc: 4:2:0 */
        put_ue(&b, 0);                              /* bit_depth_luma_minus8 */
        put_ue(&b, 0);                              /* bit_depth_chroma_minus8 */
        put_bits(&b, 0, 1);                         /* qpprime_y_zero_transform_bypass_flag */
        put_bits(&b, 0, 1);                         /* seq_scaling_matrix_present_flag */
    }
    put_ue(&b, H264_LOG2_MAX_FRAME_NUM - 4);
    put_ue(&b, 2);                                  /* pic_order_cnt_type: no reordering */
    put_ue(&b, 1);                                  /* max_num_ref_frames */
    put_bits(&b, 0, 1);                             /* gaps_in_frame_num_value_allowed_flag */
    put_ue(&b, enc->coded_width / 16 - 1);
    put_ue(&b, enc->coded_height / 16 - 1);
    put_bits(&b, 1, 1);                             /* frame_mbs_only_flag */
    put_bits(&b, 1, 1);                             /* direct_8x8_inference_flag */
    put_bits(&b, s->crop_right || s->crop_bottom, 1);
    if (s->crop_right || s->crop_bottom)
    {
        put_ue(&b, 0);
        put_ue(&b, s->crop_right);
        put_ue(&b, 0);
        put_ue(&b, s->crop_bottom);
    }
    put_bits(&b, 1, 1);                             /* vui_parameters_present_flag */
    write_h264_vui(&b, enc);
    put_trailing_bits(&b);
    nal_write(out, &b);
}

static void write_h264_pps(struct nal_buffer *out, const struct vaenc *enc)
{
    const struct va_seq *s = &enc->seq;
    struct bits b = {{0}};

    put_bits(&b, 0x68, 8);                          /* nal_ref_idc 3, nal_unit_type 8 */
    put_ue(&b, 0);                                  /* pic_parameter_set_id */
    put_ue(&b, 0);                                  /* seq_parameter_set_id */
    put_bits(&b, s->cabac, 1);                      /* entropy_coding_mode_flag */
    put_bits(&b, 0, 1);                             /* bottom_field_pic_order_in_frame_present_flag */
    put_ue(&b, 0);                                  /* num_slice_groups_minus1 */
    put_ue(&b, 0);                                  /* num_ref_idx_l0_default_active_minus1 */
    put_ue(&b, 0);                                  /* num_ref_idx_l1_default_active_minus1 */
    put_bits(&b, 0, 1);                             /* weighted_pred_flag */
    put_bits(&b, 0, 2);                             /* weighted_bipred_idc */
    put_se(&b, H264_INIT_QP - 26);                  /* pic_init_qp_minus26 */
    put_se(&b, 0);                                  /* pic_init_qs_minus26 */
    put_se(&b, 0);                                  /* chroma_qp_index_offset */
    put_bits(&b, 0, 1);                             /* deblocking_filter_control_present_flag */
    put_bits(&b, 0, 1);                             /* constrained_intra_pred_flag */
    put_bits(&b, 0, 1);                             /* redundant_pic_cnt_present_flag */
    if (s->profile_idc == 100)
    {
        put_bits(&b, s->transform_8x8, 1);          /* transform_8x8_mode_flag */
        put_bits(&b, 0, 1);                         /* pic_scaling_matrix_present_flag */
        put_se(&b, 0);                              /* second_chroma_qp_index_offset */
    }
    put_trailing_bits(&b);
    nal_write(out, &b);
}

/* The slice header, unaligned (the encoder appends the slice data); FFmpeg sends the same.
 * fake_data (header dump only) adds what a parser needs to accept the NAL unit: CABAC alignment, a
 * byte of slice data and the trailing bits. */
static void write_h264_slice_header(struct nal_buffer *out, const struct vaenc *enc, const struct frame_state *f,
                                    int fake_data)
{
    struct bits b = {{0}};

    put_bits(&b, f->idr ? 0x65 : 0x21, 8);          /* IDR: nal_ref_idc 3, type 5; P: nal_ref_idc 1, type 1 */
    put_ue(&b, 0);                                  /* first_mb_in_slice */
    put_ue(&b, f->idr ? 7 : 5);                     /* slice_type: I or P, every slice of the picture */
    put_ue(&b, 0);                                  /* pic_parameter_set_id */
    put_bits(&b, f->gop_frame & ((1u << H264_LOG2_MAX_FRAME_NUM) - 1), H264_LOG2_MAX_FRAME_NUM);
    if (f->idr) put_ue(&b, f->idr_pic_id);
    if (!f->idr)
    {
        put_bits(&b, 0, 1);                         /* num_ref_idx_active_override_flag */
        put_bits(&b, 0, 1);                         /* ref_pic_list_modification_flag_l0 */
    }
    if (f->idr)
    {
        put_bits(&b, 0, 1);                         /* no_output_of_prior_pics_flag */
        put_bits(&b, 0, 1);                         /* long_term_reference_flag */
    }
    else put_bits(&b, 0, 1);                        /* adaptive_ref_pic_marking_mode_flag: sliding window */
    if (enc->seq.cabac && !f->idr) put_ue(&b, 0);   /* cabac_init_idc */
    put_se(&b, 0);                                  /* slice_qp_delta */
    if (fake_data)
    {
        while (b.count & 7) put_bits(&b, 1, 1);     /* cabac_alignment_one_bit */
        put_bits(&b, 0xff, 8);
        put_trailing_bits(&b);
    }
    nal_write(out, &b);
}

/* ---- HEVC syntax ---------------------------------------------------------------------------- */

static void write_hevc_nal_header(struct bits *b, int type)
{
    put_bits(b, 0, 1);                              /* forbidden_zero_bit */
    put_bits(b, type, 6);
    put_bits(b, 0, 6);                              /* nuh_layer_id */
    put_bits(b, 1, 3);                              /* nuh_temporal_id_plus1 */
}

static void write_hevc_ptl(struct bits *b, const struct va_seq *s)
{
    unsigned int i;

    put_bits(b, 0, 2);                              /* general_profile_space */
    put_bits(b, s->high_tier, 1);
    put_bits(b, s->profile_idc, 5);
    for (i = 0; i < 32; i++)                        /* general_profile_compatibility_flag, as FFmpeg */
        put_bits(b, i == (unsigned int)s->profile_idc || (s->profile_idc == 1 && i == 2), 1);
    put_bits(b, 1, 1);                              /* general_progressive_source_flag */
    put_bits(b, 0, 1);                              /* general_interlaced_source_flag */
    put_bits(b, 1, 1);                              /* general_non_packed_constraint_flag */
    put_bits(b, 1, 1);                              /* general_frame_only_constraint_flag */
    put_bits(b, 0, 43);                             /* Main/Main10: reserved and one_picture_only, zero */
    put_bits(b, 0, 1);                              /* general_inbld_flag */
    put_bits(b, s->level_idc, 8);
}

static void write_hevc_vps(struct nal_buffer *out, const struct vaenc *enc)
{
    struct bits b = {{0}};

    write_hevc_nal_header(&b, 32);
    put_bits(&b, 0, 4);                             /* vps_video_parameter_set_id */
    put_bits(&b, 1, 1);                             /* vps_base_layer_internal_flag */
    put_bits(&b, 1, 1);                             /* vps_base_layer_available_flag */
    put_bits(&b, 0, 6);                             /* vps_max_layers_minus1 */
    put_bits(&b, 0, 3);                             /* vps_max_sub_layers_minus1 */
    put_bits(&b, 1, 1);                             /* vps_temporal_id_nesting_flag */
    put_bits(&b, 0xffff, 16);                       /* vps_reserved_0xffff_16bits */
    write_hevc_ptl(&b, &enc->seq);
    put_bits(&b, 0, 1);                             /* vps_sub_layer_ordering_info_present_flag */
    put_ue(&b, 1);                                  /* vps_max_dec_pic_buffering_minus1 */
    put_ue(&b, 0);                                  /* vps_max_num_reorder_pics */
    put_ue(&b, 0);                                  /* vps_max_latency_increase_plus1 */
    put_bits(&b, 0, 6);                             /* vps_max_layer_id */
    put_ue(&b, 0);                                  /* vps_num_layer_sets_minus1 */
    put_bits(&b, 1, 1);                             /* vps_timing_info_present_flag */
    put_bits(&b, enc->fps_den, 32);
    put_bits(&b, enc->fps_num, 32);
    put_bits(&b, 1, 1);                             /* vps_poc_proportional_to_timing_flag */
    put_ue(&b, 0);                                  /* vps_num_ticks_poc_diff_one_minus1 */
    put_ue(&b, 0);                                  /* vps_num_hrd_parameters */
    put_bits(&b, 0, 1);                             /* vps_extension_flag */
    put_trailing_bits(&b);
    nal_write(out, &b);
}

static void write_hevc_sps(struct nal_buffer *out, const struct vaenc *enc)
{
    const struct va_seq *s = &enc->seq;
    struct bits b = {{0}};

    write_hevc_nal_header(&b, 33);
    put_bits(&b, 0, 4);                             /* sps_video_parameter_set_id */
    put_bits(&b, 0, 3);                             /* sps_max_sub_layers_minus1 */
    put_bits(&b, 1, 1);                             /* sps_temporal_id_nesting_flag */
    write_hevc_ptl(&b, s);
    put_ue(&b, 0);                                  /* sps_seq_parameter_set_id */
    put_ue(&b, 1);                                  /* chroma_format_idc: 4:2:0 */
    put_ue(&b, enc->coded_width);
    put_ue(&b, enc->coded_height);
    put_bits(&b, s->crop_right || s->crop_bottom, 1);
    if (s->crop_right || s->crop_bottom)
    {
        put_ue(&b, 0);
        put_ue(&b, s->crop_right);
        put_ue(&b, 0);
        put_ue(&b, s->crop_bottom);
    }
    put_ue(&b, enc->bytes_per_sample == 2 ? 2 : 0); /* bit_depth_luma_minus8 */
    put_ue(&b, enc->bytes_per_sample == 2 ? 2 : 0); /* bit_depth_chroma_minus8 */
    put_ue(&b, HEVC_LOG2_MAX_POC_LSB - 4);
    put_bits(&b, 0, 1);                             /* sps_sub_layer_ordering_info_present_flag */
    put_ue(&b, 1);                                  /* sps_max_dec_pic_buffering_minus1 */
    put_ue(&b, 0);                                  /* sps_max_num_reorder_pics */
    put_ue(&b, 0);                                  /* sps_max_latency_increase_plus1 */
    put_ue(&b, s->log2_min_cb - 3);
    put_ue(&b, s->log2_ctb - s->log2_min_cb);
    put_ue(&b, s->log2_min_tb - 2);
    put_ue(&b, s->log2_max_tb - s->log2_min_tb);
    put_ue(&b, s->depth_inter);
    put_ue(&b, s->depth_intra);
    put_bits(&b, 0, 1);                             /* scaling_list_enabled_flag */
    put_bits(&b, s->amp, 1);
    put_bits(&b, s->sao, 1);
    put_bits(&b, 0, 1);                             /* pcm_enabled_flag */
    put_ue(&b, 0);                                  /* num_short_term_ref_pic_sets: all in slice headers */
    put_bits(&b, 0, 1);                             /* long_term_ref_pics_present_flag */
    put_bits(&b, 0, 1);                             /* sps_temporal_mvp_enabled_flag */
    put_bits(&b, 0, 1);                             /* strong_intra_smoothing_enabled_flag */
    put_bits(&b, 1, 1);                             /* vui_parameters_present_flag */

    put_bits(&b, s->aspect_ratio_idc != 0, 1);
    if (s->aspect_ratio_idc)
    {
        put_bits(&b, s->aspect_ratio_idc, 8);
        if (s->aspect_ratio_idc == 255)
        {
            put_bits(&b, s->sar_width, 16);
            put_bits(&b, s->sar_height, 16);
        }
    }
    put_bits(&b, 0, 1);                             /* overscan_info_present_flag */
    put_bits(&b, s->video_signal_type, 1);
    if (s->video_signal_type)
    {
        put_bits(&b, 5, 3);                         /* video_format: unspecified */
        put_bits(&b, s->full_range, 1);
        put_bits(&b, s->colour_description, 1);
        if (s->colour_description)
        {
            put_bits(&b, s->primaries, 8);
            put_bits(&b, s->transfer, 8);
            put_bits(&b, s->matrix, 8);
        }
    }
    put_bits(&b, 0, 1);                             /* chroma_loc_info_present_flag */
    put_bits(&b, 0, 1);                             /* neutral_chroma_indication_flag */
    put_bits(&b, 0, 1);                             /* field_seq_flag */
    put_bits(&b, 0, 1);                             /* frame_field_info_present_flag */
    put_bits(&b, 0, 1);                             /* default_display_window_flag */
    put_bits(&b, 1, 1);                             /* vui_timing_info_present_flag */
    put_bits(&b, enc->fps_den, 32);
    put_bits(&b, enc->fps_num, 32);
    put_bits(&b, 1, 1);                             /* vui_poc_proportional_to_timing_flag */
    put_ue(&b, 0);                                  /* vui_num_ticks_poc_diff_one_minus1 */
    put_bits(&b, 0, 1);                             /* vui_hrd_parameters_present_flag */
    put_bits(&b, 1, 1);                             /* bitstream_restriction_flag, as FFmpeg */
    put_bits(&b, 0, 1);                             /* tiles_fixed_structure_flag */
    put_bits(&b, 1, 1);                             /* motion_vectors_over_pic_boundaries_flag */
    put_bits(&b, 1, 1);                             /* restricted_ref_pic_lists_flag */
    put_ue(&b, 0);                                  /* min_spatial_segmentation_idc */
    put_ue(&b, 0);                                  /* max_bytes_per_pic_denom */
    put_ue(&b, 0);                                  /* max_bits_per_min_cu_denom */
    put_ue(&b, 15);                                 /* log2_max_mv_length_horizontal */
    put_ue(&b, 15);                                 /* log2_max_mv_length_vertical */

    put_bits(&b, 0, 1);                             /* sps_extension_present_flag */
    put_trailing_bits(&b);
    nal_write(out, &b);
}

static void write_hevc_pps(struct nal_buffer *out, const struct vaenc *enc)
{
    const struct va_seq *s = &enc->seq;
    struct bits b = {{0}};

    write_hevc_nal_header(&b, 34);
    put_ue(&b, 0);                                  /* pps_pic_parameter_set_id */
    put_ue(&b, 0);                                  /* pps_seq_parameter_set_id */
    put_bits(&b, 0, 1);                             /* dependent_slice_segments_enabled_flag */
    put_bits(&b, 0, 1);                             /* output_flag_present_flag */
    put_bits(&b, 0, 3);                             /* num_extra_slice_header_bits */
    put_bits(&b, 0, 1);                             /* sign_data_hiding_enabled_flag */
    put_bits(&b, 0, 1);                             /* cabac_init_present_flag */
    put_ue(&b, 0);                                  /* num_ref_idx_l0_default_active_minus1 */
    put_ue(&b, 0);                                  /* num_ref_idx_l1_default_active_minus1 */
    put_se(&b, HEVC_INIT_QP - 26);                  /* init_qp_minus26 */
    put_bits(&b, 0, 1);                             /* constrained_intra_pred_flag */
    put_bits(&b, s->transform_skip, 1);
    put_bits(&b, s->cu_qp_delta, 1);
    if (s->cu_qp_delta) put_ue(&b, s->diff_cu_qp_delta_depth);
    put_se(&b, 0);                                  /* pps_cb_qp_offset */
    put_se(&b, 0);                                  /* pps_cr_qp_offset */
    put_bits(&b, 0, 1);                             /* pps_slice_chroma_qp_offsets_present_flag */
    put_bits(&b, 0, 1);                             /* weighted_pred_flag */
    put_bits(&b, 0, 1);                             /* weighted_bipred_flag */
    put_bits(&b, 0, 1);                             /* transquant_bypass_enabled_flag */
    put_bits(&b, 0, 1);                             /* tiles_enabled_flag */
    put_bits(&b, 0, 1);                             /* entropy_coding_sync_enabled_flag */
    put_bits(&b, 1, 1);                             /* pps_loop_filter_across_slices_enabled_flag */
    put_bits(&b, 0, 1);                             /* deblocking_filter_control_present_flag */
    put_bits(&b, 0, 1);                             /* pps_scaling_list_data_present_flag */
    put_bits(&b, 0, 1);                             /* lists_modification_present_flag */
    put_ue(&b, 0);                                  /* log2_parallel_merge_level_minus2 */
    put_bits(&b, 0, 1);                             /* slice_segment_header_extension_present_flag */
    put_bits(&b, 0, 1);                             /* pps_extension_present_flag */
    put_trailing_bits(&b);
    nal_write(out, &b);
}

static void write_hevc_slice_header(struct nal_buffer *out, const struct vaenc *enc, const struct frame_state *f)
{
    const struct va_seq *s = &enc->seq;
    struct bits b = {{0}};

    write_hevc_nal_header(&b, f->idr ? 19 : 1);     /* IDR_W_RADL or TRAIL_R */
    put_bits(&b, 1, 1);                             /* first_slice_segment_in_pic_flag */
    if (f->idr) put_bits(&b, 0, 1);                 /* no_output_of_prior_pics_flag */
    put_ue(&b, 0);                                  /* slice_pic_parameter_set_id */
    put_ue(&b, f->idr ? 2 : 1);                     /* slice_type: I or P */
    if (!f->idr)
    {
        put_bits(&b, f->gop_frame & ((1u << HEVC_LOG2_MAX_POC_LSB) - 1), HEVC_LOG2_MAX_POC_LSB);
        put_bits(&b, 0, 1);                         /* short_term_ref_pic_set_sps_flag */
        put_ue(&b, 1);                              /* num_negative_pics: the previous picture */
        put_ue(&b, 0);                              /* num_positive_pics */
        put_ue(&b, 0);                              /* delta_poc_s0_minus1 */
        put_bits(&b, 1, 1);                         /* used_by_curr_pic_s0_flag */
    }
    if (s->sao)
    {
        put_bits(&b, 1, 1);                         /* slice_sao_luma_flag */
        put_bits(&b, 1, 1);                         /* slice_sao_chroma_flag */
    }
    if (!f->idr)
    {
        put_bits(&b, 0, 1);                         /* num_ref_idx_active_override_flag */
        put_ue(&b, 0);                              /* five_minus_max_num_merge_cand */
    }
    put_se(&b, 0);                                  /* slice_qp_delta */
    put_bits(&b, 0, 1);                             /* slice_loop_filter_across_slices_enabled_flag */
    put_trailing_bits(&b);                          /* byte_alignment() */
    nal_write(out, &b);
}

/* The packed sequence header: SPS+PPS, or VPS+SPS+PPS. */
static void write_sequence_header(struct nal_buffer *out, const struct vaenc *enc)
{
    if (enc->params.codec == VKENC_CODEC_HEVC)
    {
        write_hevc_vps(out, enc);
        write_hevc_sps(out, enc);
        write_hevc_pps(out, enc);
    }
    else
    {
        write_h264_sps(out, enc);
        write_h264_pps(out, enc);
    }
}

/* ---- choosing the parameters ---------------------------------------------------------------- */

static int choose_h264_level(const struct vaenc *enc, uint64_t rate, int high)
{
    uint64_t mbs = (uint64_t)(enc->coded_width / 16) * (enc->coded_height / 16);
    uint64_t mbps = (mbs * enc->fps_num + enc->fps_den - 1) / enc->fps_den;
    uint32_t w = enc->coded_width / 16, h = enc->coded_height / 16;
    unsigned int i, need = ARRAY_SIZE(h264_levels) - 1, want = 0;

    for (i = 0; i < ARRAY_SIZE(h264_levels); i++)
    {
        const struct h264_level *l = &h264_levels[i];
        if (mbs <= l->max_fs && mbps <= l->max_mbps && rate <= (uint64_t)l->max_br * (high ? 1250 : 1000)
                && (uint64_t)w * w <= 8ull * l->max_fs && (uint64_t)h * h <= 8ull * l->max_fs)
        {
            need = i;
            break;
        }
    }
    for (i = 0; i < ARRAY_SIZE(h264_levels); i++)
        if (h264_levels[i].idc == enc->params.level) want = i;
    return h264_levels[want > need ? want : need].idc;
}

static int choose_h265_level(const struct vaenc *enc, uint64_t rate, int high_tier)
{
    uint64_t ps = (uint64_t)enc->coded_width * enc->coded_height;
    uint64_t sr = (ps * enc->fps_num + enc->fps_den - 1) / enc->fps_den;
    unsigned int i, need = ARRAY_SIZE(h265_levels) - 1, want = 0;

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
        if (h265_levels[i].idc == enc->params.level) want = i;
    return h265_levels[want > need ? want : need].idc;
}

static void setup_vui(struct vaenc *enc)
{
    const vkenc_params *p = &enc->params;
    struct va_seq *s = &enc->seq;
    uint32_t num = p->sar_num, den = p->sar_den, g;

    s->aspect_ratio_idc = 0;
    if (num && den)
    {
        g = gcd32(num, den);
        num /= g;
        den /= g;
        if (num == 1 && den == 1) s->aspect_ratio_idc = 1;
        else
        {
            while (num > 0xffff || den > 0xffff)
            {
                num = (num + 1) / 2;
                den = (den + 1) / 2;
            }
            s->aspect_ratio_idc = 255;  /* Extended_SAR */
            s->sar_width = num;
            s->sar_height = den;
        }
    }
    s->colour_description = p->color_primaries >= 0 || p->transfer_characteristics >= 0 || p->matrix_coefficients >= 0;
    s->video_signal_type = p->full_range == 1 || s->colour_description;
    s->full_range = p->full_range == 1;
    s->primaries = p->color_primaries >= 0 ? p->color_primaries : 2;
    s->transfer = p->transfer_characteristics >= 0 ? p->transfer_characteristics : 2;
    s->matrix = p->matrix_coefficients >= 0 ? p->matrix_coefficients : 2;
}

static uint64_t level_rate(const struct vaenc *enc)
{
    return enc->rc_mode == VA_RC_VBR ? enc->peak_bitrate : enc->target_bitrate;
}

static void setup_h264(struct vaenc *enc)
{
    struct va_seq *s = &enc->seq;

    switch (enc->profile)
    {
    case VAProfileH264ConstrainedBaseline:
        s->profile_idc = 66;
        s->constraint_flags = 0x30;                 /* set0 and set1: constrained baseline */
        break;
    case VAProfileH264Main:
        s->profile_idc = 77;
        s->constraint_flags = 0x10 | 0x02 | 0x01;   /* set1, set4 (frames only), set5 (no B), as FFmpeg */
        break;
    default:
        s->profile_idc = 100;
        s->constraint_flags = 0x02 | 0x01;          /* set4, set5 */
        break;
    }
    s->cabac = s->profile_idc != 66;
    s->transform_8x8 = s->profile_idc == 100;
    s->level_idc = choose_h264_level(enc, level_rate(enc), s->profile_idc == 100);
    s->crop_right = (enc->coded_width - enc->width) / 2;
    s->crop_bottom = (enc->coded_height - enc->height) / 2;
}

/* features / block_sizes: VAConfigAttribEncHEVCFeatures / BlockSizes, VA_ATTRIB_NOT_SUPPORTED if absent. */
static void setup_hevc(struct vaenc *enc, uint32_t features, uint32_t block_sizes)
{
    struct va_seq *s = &enc->seq;

    s->profile_idc = enc->bytes_per_sample == 2 ? 2 : 1;
    s->high_tier = enc->params.tier == 1;
    s->level_idc = choose_h265_level(enc, level_rate(enc), s->high_tier);

    /* FFmpeg's defaults (from the first Intel encoder) when the driver says nothing. */
    s->log2_min_cb = 3;
    s->log2_ctb = 5;
    s->log2_min_tb = 2;
    s->log2_max_tb = 5;
    s->depth_inter = s->depth_intra = 3;
    s->amp = 1;
    s->sao = 0;
    s->cu_qp_delta = 1;
    s->diff_cu_qp_delta_depth = 0;
    s->transform_skip = 0;
    s->ctu_size = 32;
#if VA_CHECK_VERSION(1, 13, 0)
    if (features != VA_ATTRIB_NOT_SUPPORTED)
    {
        VAConfigAttribValEncHEVCFeatures f = {.value = features};
        s->amp = !!f.bits.amp;
        s->sao = !!f.bits.sao;
        s->cu_qp_delta = !!f.bits.cu_qp_delta;
        s->transform_skip = !!f.bits.transform_skip;
    }
    if (block_sizes != VA_ATTRIB_NOT_SUPPORTED)
    {
        VAConfigAttribValEncHEVCBlockSizes bs = {.value = block_sizes};
        s->log2_ctb = bs.bits.log2_max_coding_tree_block_size_minus3 + 3;
        s->log2_min_cb = bs.bits.log2_min_luma_coding_block_size_minus3 + 3;
        s->log2_min_tb = bs.bits.log2_min_luma_transform_block_size_minus2 + 2;
        s->log2_max_tb = bs.bits.log2_max_luma_transform_block_size_minus2 + 2;
        s->depth_inter = bs.bits.max_max_transform_hierarchy_depth_inter;
        s->depth_intra = bs.bits.max_max_transform_hierarchy_depth_intra;
        s->ctu_size = 1u << s->log2_ctb;
    }
    if (features != VA_ATTRIB_NOT_SUPPORTED && s->cu_qp_delta)
        s->diff_cu_qp_delta_depth = s->log2_ctb - s->log2_min_cb;
#else
    (void)features;
    (void)block_sizes;
#endif
    if (s->log2_min_cb > s->log2_ctb) s->log2_min_cb = s->log2_ctb;
    if (s->log2_max_tb < s->log2_min_tb) s->log2_max_tb = s->log2_min_tb;
    s->crop_right = (enc->coded_width - enc->width) / 2;
    s->crop_bottom = (enc->coded_height - enc->height) / 2;
}

/* Sizes and rates shared by vaenc_create and the header dump. */
static void setup_common(struct vaenc *enc, const vkenc_params *params)
{
    enc->params = *params;
    enc->bytes_per_sample = params->format == VKENC_FMT_P010 ? 2 : 1;
    enc->src_width = params->width;
    enc->src_height = params->height;
    enc->width = align_up(params->width, 2);
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
    enc->target_bitrate = params->target_bitrate ? params->target_bitrate : 10000000;
    if (enc->target_bitrate > 0xffffffffu) enc->target_bitrate = 0xffffffffu;
    enc->peak_bitrate = params->peak_bitrate > enc->target_bitrate ? params->peak_bitrate : enc->target_bitrate;
    if (enc->peak_bitrate > 0xffffffffu) enc->peak_bitrate = 0xffffffffu;
}

/* ---- VA parameter buffers ------------------------------------------------------------------- */

struct va_buffers
{
    VABufferID ids[16];
    unsigned int count;
    int failed;
};

static void add_buffer(struct vaenc *enc, struct va_buffers *list, VABufferType type, const void *data, size_t size)
{
    VAStatus status;

    if (list->failed) return;
    if (list->count >= ARRAY_SIZE(list->ids))
    {
        list->failed = 1;
        return;
    }
    if ((status = va.vaCreateBuffer(enc->dev.dpy, enc->context, type, size, 1, (void *)data,
                                    &list->ids[list->count])) != VA_STATUS_SUCCESS)
    {
        INFO("vaCreateBuffer(type %d) failed: %s", type, va.vaErrorStr(status));
        list->failed = 1;
        return;
    }
    list->count++;
}

static void add_misc(struct vaenc *enc, struct va_buffers *list, VAEncMiscParameterType type, const void *data,
                     size_t size)
{
    uint8_t buffer[512];
    VAEncMiscParameterBuffer header = {.type = type};

    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), data, size);
    add_buffer(enc, list, VAEncMiscParameterBufferType, buffer, sizeof(header) + size);
}

static void add_packed(struct vaenc *enc, struct va_buffers *list, uint32_t type, const struct nal_buffer *nal)
{
    VAEncPackedHeaderParameterBuffer param = {0};

    if (nal->failed || !nal->size)
    {
        list->failed = 1;
        return;
    }
    param.type = type;
    param.bit_length = nal->size * 8 - nal->padding_bits;
    param.has_emulation_bytes = 1;
    add_buffer(enc, list, VAEncPackedHeaderParameterBufferType, &param, sizeof(param));
    add_buffer(enc, list, VAEncPackedHeaderDataBufferType, nal->data, nal->size);
}

/* Rate control, HRD, frame rate and quality: FFmpeg's global parameters, sent with every IDR. */
static void add_global_params(struct vaenc *enc, struct va_buffers *list)
{
    VAEncMiscParameterRateControl rc = {0};
    VAEncMiscParameterHRD hrd = {0};
    VAEncMiscParameterFrameRate fr = {0};
    VAEncMiscParameterBufferQualityLevel quality = {0};
    uint32_t num = enc->fps_num, den = enc->fps_den, g = gcd32(num, den);
    uint64_t rate = enc->rc_mode == VA_RC_VBR ? enc->peak_bitrate : enc->target_bitrate;

    rc.bits_per_second = rate;
    rc.target_percentage = enc->rc_mode == VA_RC_VBR ? (uint32_t)(enc->target_bitrate * 100 / rate) : 100;
    if (!rc.target_percentage) rc.target_percentage = 1;
    rc.window_size = 1000;
    /* Bit stuffing keeps CBR at its rate (Adobe asks for filler data with CBR); Mesa reads the flag. */
    rc.rc_flags.bits.disable_bit_stuffing = enc->rc_mode != VA_RC_CBR;
    rc.rc_flags.bits.mb_rate_control = 2;   /* FFmpeg's default: no block-level rate control */
    add_misc(enc, list, VAEncMiscParameterTypeRateControl, &rc, sizeof(rc));

    hrd.buffer_size = rate;                 /* one second, as FFmpeg without rc_buffer_size */
    hrd.initial_buffer_fullness = rate * 3 / 4;
    add_misc(enc, list, VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd));

    num /= g;
    den /= g;
    while (num > 0xffff || den > 0xffff)
    {
        num = (num + 1) / 2;
        den = (den + 1) / 2;
    }
    fr.framerate = den << 16 | num;
    add_misc(enc, list, VAEncMiscParameterTypeFrameRate, &fr, sizeof(fr));

    if (enc->quality_level)
    {
        quality.quality_level = enc->quality_level;
        add_misc(enc, list, VAEncMiscParameterTypeQualityLevel, &quality, sizeof(quality));
    }
}

static void add_h264_params(struct vaenc *enc, struct va_buffers *list, const struct frame_state *f,
                            const struct va_slot *slot)
{
    const struct va_seq *s = &enc->seq;
    VAEncSequenceParameterBufferH264 seq;
    VAEncPictureParameterBufferH264 pic;
    VAEncSliceParameterBufferH264 slice;
    VAPictureH264 ref;
    unsigned int i;

    if (f->idr)
    {
        memset(&seq, 0, sizeof(seq));
        seq.seq_parameter_set_id = 0;
        seq.level_idc = s->level_idc;
        seq.intra_period = enc->idr_period;
        seq.intra_idr_period = enc->idr_period;
        seq.ip_period = 1;
        seq.bits_per_second = enc->rc_mode == VA_RC_VBR ? enc->peak_bitrate : enc->target_bitrate;
        seq.max_num_ref_frames = 1;
        seq.picture_width_in_mbs = enc->coded_width / 16;
        seq.picture_height_in_mbs = enc->coded_height / 16;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        seq.seq_fields.bits.direct_8x8_inference_flag = 1;
        seq.seq_fields.bits.log2_max_frame_num_minus4 = H264_LOG2_MAX_FRAME_NUM - 4;
        seq.seq_fields.bits.pic_order_cnt_type = 2;
        seq.frame_cropping_flag = s->crop_right || s->crop_bottom;
        seq.frame_crop_right_offset = s->crop_right;
        seq.frame_crop_bottom_offset = s->crop_bottom;
        seq.vui_parameters_present_flag = 1;
        seq.vui_fields.bits.aspect_ratio_info_present_flag = s->aspect_ratio_idc != 0;
        seq.vui_fields.bits.timing_info_present_flag = 1;
        seq.vui_fields.bits.bitstream_restriction_flag = 1;
        seq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        seq.vui_fields.bits.log2_max_mv_length_vertical = 15;
        seq.vui_fields.bits.fixed_frame_rate_flag = 1;
        seq.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
        seq.aspect_ratio_idc = s->aspect_ratio_idc;
        seq.sar_width = s->sar_width;
        seq.sar_height = s->sar_height;
        seq.num_units_in_tick = enc->fps_den;
        seq.time_scale = enc->fps_num * 2;
        add_buffer(enc, list, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        add_global_params(enc, list);
    }

    memset(&pic, 0, sizeof(pic));
    pic.CurrPic.picture_id = enc->recon[f->cur_recon];
    pic.CurrPic.frame_idx = f->gop_frame;
    pic.CurrPic.TopFieldOrderCnt = pic.CurrPic.BottomFieldOrderCnt = 2 * f->gop_frame;
    for (i = 0; i < ARRAY_SIZE(pic.ReferenceFrames); i++)
    {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_ID;
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    memset(&ref, 0, sizeof(ref));
    if (!f->idr)
    {
        ref.picture_id = enc->recon[f->ref_recon];
        ref.frame_idx = f->gop_frame - 1;
        ref.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        ref.TopFieldOrderCnt = ref.BottomFieldOrderCnt = 2 * (f->gop_frame - 1);
        pic.ReferenceFrames[0] = ref;
    }
    pic.coded_buf = slot->coded;
    pic.frame_num = f->gop_frame;
    pic.pic_init_qp = H264_INIT_QP;
    pic.pic_fields.bits.idr_pic_flag = f->idr;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = s->cabac;
    pic.pic_fields.bits.transform_8x8_mode_flag = s->transform_8x8;
    add_buffer(enc, list, VAEncPictureParameterBufferType, &pic, sizeof(pic));

    if (f->idr && (enc->packed & VA_ENC_PACKED_HEADER_SEQUENCE))
    {
        struct nal_buffer nal = {0};
        write_sequence_header(&nal, enc);
        add_packed(enc, list, VAEncPackedHeaderSequence, &nal);
        free(nal.data);
    }
    if (enc->packed & VA_ENC_PACKED_HEADER_SLICE)
    {
        struct nal_buffer nal = {0};
        write_h264_slice_header(&nal, enc, f, 0);
        add_packed(enc, list, VAEncPackedHeaderH264_Slice, &nal);
        free(nal.data);
    }

    memset(&slice, 0, sizeof(slice));
    slice.macroblock_address = 0;
    slice.num_macroblocks = (enc->coded_width / 16) * (enc->coded_height / 16);
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = f->idr ? 2 : 0;
    slice.idr_pic_id = f->idr_pic_id;
    slice.direct_spatial_mv_pred_flag = 1;
    for (i = 0; i < ARRAY_SIZE(slice.RefPicList0); i++)
    {
        slice.RefPicList0[i].picture_id = slice.RefPicList1[i].picture_id = VA_INVALID_ID;
        slice.RefPicList0[i].flags = slice.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }
    if (!f->idr) slice.RefPicList0[0] = ref;
    add_buffer(enc, list, VAEncSliceParameterBufferType, &slice, sizeof(slice));
}

static void add_hevc_params(struct vaenc *enc, struct va_buffers *list, const struct frame_state *f,
                            const struct va_slot *slot)
{
    const struct va_seq *s = &enc->seq;
    VAEncSequenceParameterBufferHEVC seq;
    VAEncPictureParameterBufferHEVC pic;
    VAEncSliceParameterBufferHEVC slice;
    VAPictureHEVC ref;
    uint32_t ctus;
    unsigned int i;

    if (f->idr)
    {
        memset(&seq, 0, sizeof(seq));
        seq.general_profile_idc = s->profile_idc;
        seq.general_level_idc = s->level_idc;
        seq.general_tier_flag = s->high_tier;
        seq.intra_period = enc->idr_period;
        seq.intra_idr_period = enc->idr_period;
        seq.ip_period = 1;
        seq.bits_per_second = enc->rc_mode == VA_RC_VBR ? enc->peak_bitrate : enc->target_bitrate;
        seq.pic_width_in_luma_samples = enc->coded_width;
        seq.pic_height_in_luma_samples = enc->coded_height;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.bit_depth_luma_minus8 = enc->bytes_per_sample == 2 ? 2 : 0;
        seq.seq_fields.bits.bit_depth_chroma_minus8 = enc->bytes_per_sample == 2 ? 2 : 0;
        seq.seq_fields.bits.amp_enabled_flag = s->amp;
        seq.seq_fields.bits.sample_adaptive_offset_enabled_flag = s->sao;
        seq.log2_min_luma_coding_block_size_minus3 = s->log2_min_cb - 3;
        seq.log2_diff_max_min_luma_coding_block_size = s->log2_ctb - s->log2_min_cb;
        seq.log2_min_transform_block_size_minus2 = s->log2_min_tb - 2;
        seq.log2_diff_max_min_transform_block_size = s->log2_max_tb - s->log2_min_tb;
        seq.max_transform_hierarchy_depth_inter = s->depth_inter;
        seq.max_transform_hierarchy_depth_intra = s->depth_intra;
        /* FFmpeg leaves the VUI to the packed SPS; Mesa reads it there too. Filled anyway, for a
         * driver that takes no packed headers. */
        seq.vui_parameters_present_flag = 1;
        seq.vui_fields.bits.aspect_ratio_info_present_flag = s->aspect_ratio_idc != 0;
        seq.vui_fields.bits.vui_timing_info_present_flag = 1;
        seq.vui_fields.bits.bitstream_restriction_flag = 1;
        seq.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
        seq.vui_fields.bits.restricted_ref_pic_lists_flag = 1;
        seq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        seq.vui_fields.bits.log2_max_mv_length_vertical = 15;
        seq.aspect_ratio_idc = s->aspect_ratio_idc;
        seq.sar_width = s->sar_width;
        seq.sar_height = s->sar_height;
        seq.vui_num_units_in_tick = enc->fps_den;
        seq.vui_time_scale = enc->fps_num;
        add_buffer(enc, list, VAEncSequenceParameterBufferType, &seq, sizeof(seq));
        add_global_params(enc, list);
    }

    memset(&pic, 0, sizeof(pic));
    pic.decoded_curr_pic.picture_id = enc->recon[f->cur_recon];
    pic.decoded_curr_pic.pic_order_cnt = f->gop_frame;
    for (i = 0; i < ARRAY_SIZE(pic.reference_frames); i++)
    {
        pic.reference_frames[i].picture_id = VA_INVALID_ID;
        pic.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    memset(&ref, 0, sizeof(ref));
    if (!f->idr)
    {
        ref.picture_id = enc->recon[f->ref_recon];
        ref.pic_order_cnt = f->gop_frame - 1;
        ref.flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        pic.reference_frames[0] = ref;
    }
    pic.coded_buf = slot->coded;
    pic.collocated_ref_pic_index = 0xff;            /* no temporal MV prediction */
    pic.pic_init_qp = HEVC_INIT_QP;
    pic.diff_cu_qp_delta_depth = s->diff_cu_qp_delta_depth;
    pic.nal_unit_type = f->idr ? 19 : 1;            /* IDR_W_RADL / TRAIL_R */
    pic.pic_fields.bits.idr_pic_flag = f->idr;
    pic.pic_fields.bits.coding_type = f->idr ? 1 : 2;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.transform_skip_enabled_flag = s->transform_skip;
    pic.pic_fields.bits.cu_qp_delta_enabled_flag = s->cu_qp_delta;
    pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    add_buffer(enc, list, VAEncPictureParameterBufferType, &pic, sizeof(pic));

    if (f->idr && (enc->packed & VA_ENC_PACKED_HEADER_SEQUENCE))
    {
        struct nal_buffer nal = {0};
        write_sequence_header(&nal, enc);
        add_packed(enc, list, VAEncPackedHeaderSequence, &nal);
        free(nal.data);
    }
    if (enc->packed & VA_ENC_PACKED_HEADER_SLICE)
    {
        struct nal_buffer nal = {0};
        write_hevc_slice_header(&nal, enc, f);
        add_packed(enc, list, VAEncPackedHeaderHEVC_Slice, &nal);
        free(nal.data);
    }

    ctus = ((enc->coded_width + s->ctu_size - 1) / s->ctu_size) * ((enc->coded_height + s->ctu_size - 1) / s->ctu_size);
    memset(&slice, 0, sizeof(slice));
    slice.slice_segment_address = 0;
    slice.num_ctu_in_slice = ctus;
    slice.slice_type = f->idr ? 2 : 1;
    for (i = 0; i < ARRAY_SIZE(slice.ref_pic_list0); i++)
    {
        slice.ref_pic_list0[i].picture_id = slice.ref_pic_list1[i].picture_id = VA_INVALID_ID;
        slice.ref_pic_list0[i].flags = slice.ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    if (!f->idr) slice.ref_pic_list0[0] = ref;
    slice.max_num_merge_cand = 5;
    slice.slice_fields.bits.last_slice_of_pic_flag = 1;
    slice.slice_fields.bits.slice_sao_luma_flag = s->sao;
    slice.slice_fields.bits.slice_sao_chroma_flag = s->sao;
    slice.slice_fields.bits.collocated_from_l0_flag = 1;
    add_buffer(enc, list, VAEncSliceParameterBufferType, &slice, sizeof(slice));
}

/* ---- create / destroy ----------------------------------------------------------------------- */

static void free_packets(struct vaenc *enc)
{
    while (enc->out_head)
    {
        struct va_packet *next = enc->out_head->next;
        free(enc->out_head->data);
        free(enc->out_head);
        enc->out_head = next;
    }
    enc->out_tail = NULL;
    enc->out_count = 0;
}

void vaenc_destroy(struct vaenc *enc)
{
    uint32_t i;

    if (!enc) return;
    if (enc->worker_started)
    {
        /* The worker finishes the frames still encoding before it leaves. */
        pthread_mutex_lock(&enc->ring_lock);
        enc->stop = 1;
        pthread_cond_broadcast(&enc->ring_cond);
        pthread_mutex_unlock(&enc->ring_lock);
        pthread_join(enc->worker, NULL);
    }
    if (enc->dev.dpy)
    {
        for (i = 0; i < enc->slot_count; i++)
        {
            struct va_slot *slot = &enc->slots[i];
            if (slot->coded != VA_INVALID_ID) va.vaDestroyBuffer(enc->dev.dpy, slot->coded);
            if (slot->image.image_id != VA_INVALID_ID) va.vaDestroyImage(enc->dev.dpy, slot->image.image_id);
            if (slot->surface != VA_INVALID_SURFACE) va.vaDestroySurfaces(enc->dev.dpy, &slot->surface, 1);
        }
        if (enc->have_context) va.vaDestroyContext(enc->dev.dpy, enc->context);
        if (enc->have_recon) va.vaDestroySurfaces(enc->dev.dpy, enc->recon, VAENC_RECON);
        if (enc->have_config) va.vaDestroyConfig(enc->dev.dpy, enc->config);
    }
    device_close(&enc->dev);
    free_packets(enc);
    pthread_mutex_destroy(&enc->submit_lock);
    pthread_mutex_destroy(&enc->va_lock);
    pthread_mutex_destroy(&enc->ring_lock);
    pthread_cond_destroy(&enc->ring_cond);
    free(enc->own_headers);
    free(enc->driver_headers);
    free(enc);
}

static struct vaenc *alloc_encoder(void)
{
    pthread_condattr_t attr;
    struct vaenc *enc;
    uint32_t i;

    if (!(enc = calloc(1, sizeof(*enc)))) return NULL;
    enc->dev.fd = -1;
    for (i = 0; i < VAENC_MAX_SLOTS; i++)
    {
        enc->slots[i].surface = VA_INVALID_SURFACE;
        enc->slots[i].image.image_id = VA_INVALID_ID;
        enc->slots[i].coded = VA_INVALID_ID;
    }
    pthread_mutex_init(&enc->submit_lock, NULL);
    pthread_mutex_init(&enc->va_lock, NULL);
    pthread_mutex_init(&enc->ring_lock, NULL);
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&enc->ring_cond, &attr);
    pthread_condattr_destroy(&attr);
    return enc;
}

static void *worker_main(void *arg);

#define FAIL(...) \
    do { \
        set_reason(err, errlen, __VA_ARGS__); \
        goto fail; \
    } while (0)

/* Opens the first usable device for the profile. */
static int open_device(struct vaenc *enc, char *err, size_t errlen)
{
    struct va_node nodes[VAENC_MAX_NODES];
    unsigned int count, i;
    struct enc_probe probe;

    vaenc_probe(enc->params.codec, &probe);
    if (!probe.caps.available)
    {
        set_reason(err, errlen, "%s", probe.caps.reason);
        return -1;
    }
    count = find_nodes(nodes, ARRAY_SIZE(nodes));
    for (i = 0; i < count; i++)
    {
        struct enc_probe dev_probe;

        if (device_open(&enc->dev, &nodes[i])) continue;
        probe_device(&enc->dev, enc->params.codec, &dev_probe);
        if (dev_probe.caps.available) return 0;
        device_close(&enc->dev);
    }
    set_reason(err, errlen, "The GPU's VA-API encoder could not be opened.");
    return -1;
}

static int choose_profile(struct vaenc *enc, char *err, size_t errlen)
{
    const vkenc_params *p = &enc->params;
    unsigned int first, i;

    if (p->codec == VKENC_CODEC_HEVC)
    {
        enc->profile = p->format == VKENC_FMT_P010 ? VAProfileHEVCMain10 : VAProfileHEVCMain;
        if (find_entrypoint(&enc->dev, enc->profile, &enc->entrypoint)) return 0;
        set_reason(err, errlen, "VA-API's HEVC encoder does not support the %s profile.",
                   p->format == VKENC_FMT_P010 ? "Main 10" : "Main");
        return -1;
    }
    /* The requested H.264 profile, else the next one down. */
    first = p->profile == 66 || p->profile == 256 ? 2 : p->profile == 77 ? 1 : 0;
    for (i = first; i < ARRAY_SIZE(h264_profiles); i++)
    {
        if (!find_entrypoint(&enc->dev, h264_profiles[i], &enc->entrypoint)) continue;
        enc->profile = h264_profiles[i];
        if (i != first) INFO("VA-API has no encoder for the requested H.264 profile; using profile %d",
                             enc->profile);
        return 0;
    }
    set_reason(err, errlen, "VA-API's H.264 encoder does not support the requested profile.");
    return -1;
}

static int find_image_format(struct vaenc *enc, uint32_t fourcc, VAImageFormat *out)
{
    VAImageFormat *formats;
    int count = 0, i, found = 0;

    if (!(formats = calloc(va.vaMaxNumImageFormats(enc->dev.dpy) + 1, sizeof(*formats)))) return 0;
    if (va.vaQueryImageFormats(enc->dev.dpy, formats, &count) == VA_STATUS_SUCCESS)
        for (i = 0; i < count && !found; i++)
            if (formats[i].fourcc == fourcc) found = 1, *out = formats[i];
    free(formats);
    return found;
}

int vaenc_create(const vkenc_params *params, struct vaenc **out, char *err, size_t errlen)
{
    VASurfaceAttrib surface_attr;
    VAConfigAttrib attribs[3];
    VAImageFormat image_format;
    struct surface_limits limits;
    uint32_t rt_format, fourcc, rc, packed, features = VA_ATTRIB_NOT_SUPPORTED;
    uint32_t block_sizes = VA_ATTRIB_NOT_SUPPORTED, quality_range, align_w, align_h, i;
    unsigned int nattribs = 0;
    const char *env;
    struct vaenc *enc;
    VAStatus status;

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
    if (!(enc = alloc_encoder()))
    {
        set_reason(err, errlen, "Out of memory.");
        return -1;
    }
    setup_common(enc, params);
    if (params->interlaced) INFO("interlaced encoding is not supported; encoding progressive frames");

    if (open_device(enc, err, errlen)) goto fail;
    if (choose_profile(enc, err, errlen)) goto fail;

    rt_format = enc->bytes_per_sample == 2 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    fourcc = enc->bytes_per_sample == 2 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    if (!(get_attrib(&enc->dev, enc->profile, enc->entrypoint, VAConfigAttribRTFormat) & rt_format))
        FAIL("VA-API's %s encoder does not take %s frames.", codec_name(params->codec),
             enc->bytes_per_sample == 2 ? "P010" : "NV12");

    rc = get_attrib(&enc->dev, enc->profile, enc->entrypoint, VAConfigAttribRateControl);
    if (rc == VA_ATTRIB_NOT_SUPPORTED) rc = 0;
    if (params->rc == VKENC_RC_VBR && (rc & VA_RC_VBR)) enc->rc_mode = VA_RC_VBR;
    else if (rc & VA_RC_CBR) enc->rc_mode = VA_RC_CBR;
    else if (rc & VA_RC_VBR) enc->rc_mode = VA_RC_VBR;   /* CBR asked: VBR with peak = target */
    else FAIL("VA-API's %s encoder has no bit rate control.", codec_name(params->codec));
    if (params->rc == VKENC_RC_VBR && enc->rc_mode != VA_RC_VBR) INFO("the driver has no VBR; using CBR");
    if (params->rc != VKENC_RC_VBR && enc->rc_mode != VA_RC_CBR)
    {
        INFO("the driver has no CBR; using VBR");
        enc->peak_bitrate = enc->target_bitrate;
    }

    packed = get_attrib(&enc->dev, enc->profile, enc->entrypoint, VAConfigAttribEncPackedHeaders);
    if (packed == VA_ATTRIB_NOT_SUPPORTED) packed = 0;
    enc->packed = packed & (VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE);

    quality_range = get_attrib(&enc->dev, enc->profile, enc->entrypoint, VAConfigAttribEncQualityRange);
    if ((env = getenv("NEUTRON_AMF_QUALITY_LEVEL")) && *env) enc->quality_level = strtoul(env, NULL, 10);
    else if (quality_range != VA_ATTRIB_NOT_SUPPORTED && quality_range >= 1)
        enc->quality_level = 1;   /* VA-API's best quality; radeonsi: balanced preset, pre-encode, VBAQ */

#if VA_CHECK_VERSION(1, 13, 0)
    if (params->codec == VKENC_CODEC_HEVC)
    {
        features = get_attrib(&enc->dev, enc->profile, enc->entrypoint, VAConfigAttribEncHEVCFeatures);
        block_sizes = get_attrib(&enc->dev, enc->profile, enc->entrypoint, VAConfigAttribEncHEVCBlockSizes);
    }
#endif

    /* Coded size: macroblocks for H.264 (as FFmpeg); for HEVC the minimum coding block and the
     * driver's surface alignment. The SPS crops back to the visible size. */
    get_surface_limits(&enc->dev, enc->profile, enc->entrypoint, &limits);
    if (params->codec == VKENC_CODEC_HEVC)
    {
        setup_hevc(enc, features, block_sizes);  /* block sizes only; sizes are redone below */
        align_w = 1u << enc->seq.log2_min_cb;
        align_h = align_w;
        if (block_sizes == VA_ATTRIB_NOT_SUPPORTED) align_w = align_h = 16;  /* FFmpeg's default min CB */
        if (limits.align_width > align_w) align_w = limits.align_width;
        if (limits.align_height > align_h) align_h = limits.align_height;
    }
    else align_w = align_h = 16;
    enc->coded_width = align_up(enc->width, align_w);
    enc->coded_height = align_up(enc->height, align_h);
    if (limits.min_width && enc->coded_width < limits.min_width) enc->coded_width = align_up(limits.min_width, align_w);
    if (limits.min_height && enc->coded_height < limits.min_height)
        enc->coded_height = align_up(limits.min_height, align_h);
    if ((limits.max_width && enc->coded_width > limits.max_width)
            || (limits.max_height && enc->coded_height > limits.max_height))
        FAIL("The GPU's %s encoder cannot encode %ux%u (its limit is %ux%u).", codec_name(params->codec),
             enc->width, enc->height, limits.max_width, limits.max_height);
    setup_vui(enc);
    if (params->codec == VKENC_CODEC_HEVC) setup_hevc(enc, features, block_sizes);
    else setup_h264(enc);

    attribs[nattribs].type = VAConfigAttribRTFormat;
    attribs[nattribs++].value = rt_format;
    attribs[nattribs].type = VAConfigAttribRateControl;
    attribs[nattribs++].value = enc->rc_mode;
    if (enc->packed)
    {
        attribs[nattribs].type = VAConfigAttribEncPackedHeaders;
        attribs[nattribs++].value = enc->packed;
    }
    if ((status = va.vaCreateConfig(enc->dev.dpy, enc->profile, enc->entrypoint, attribs, nattribs,
                                    &enc->config)) != VA_STATUS_SUCCESS)
        FAIL("The VA-API %s encoder could not be configured (%s).", codec_name(params->codec), va.vaErrorStr(status));
    enc->have_config = 1;

    enc->slot_count = (uint64_t)enc->coded_width * enc->coded_height > 1920 * 1088 ? 3 : VAENC_MAX_SLOTS;
    /* vaSyncBuffer reports "unimplemented" where the driver cannot keep several frames in flight
     * (Mesa before 25.0 on VCE/UVD: Polaris and Vega); FFmpeg checks it the same way. */
    enc->async = va.vaSyncBuffer && va.vaSyncBuffer(enc->dev.dpy, VA_INVALID_ID, 0) != VA_STATUS_ERROR_UNIMPLEMENTED;
    if (!enc->async) enc->slot_count = 1;

    memset(&surface_attr, 0, sizeof(surface_attr));
    surface_attr.type = VASurfaceAttribPixelFormat;
    surface_attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
    surface_attr.value.type = VAGenericValueTypeInteger;
    surface_attr.value.value.i = fourcc;
    if ((status = va.vaCreateSurfaces(enc->dev.dpy, rt_format, enc->coded_width, enc->coded_height, enc->recon,
                                      VAENC_RECON, &surface_attr, 1)) != VA_STATUS_SUCCESS)
        FAIL("The encoder's reference surfaces could not be created (%s).", va.vaErrorStr(status));
    enc->have_recon = 1;
    if ((status = va.vaCreateContext(enc->dev.dpy, enc->config, enc->coded_width, enc->coded_height, VA_PROGRESSIVE,
                                     enc->recon, VAENC_RECON, &enc->context)) != VA_STATUS_SUCCESS)
        FAIL("The VA-API %s encoder could not be created (%s).", codec_name(params->codec), va.vaErrorStr(status));
    enc->have_context = 1;

    if (!find_image_format(enc, fourcc, &image_format))
        FAIL("The VA-API driver cannot upload %s frames.", enc->bytes_per_sample == 2 ? "P010" : "NV12");
    /* FFmpeg's output buffer size: far more than any frame at a sane quantizer. */
    enc->coded_size = (size_t)3 * enc->coded_width * enc->coded_height + (1 << 16);
    for (i = 0; i < enc->slot_count; i++)
    {
        struct va_slot *slot = &enc->slots[i];

        if ((status = va.vaCreateSurfaces(enc->dev.dpy, rt_format, enc->coded_width, enc->coded_height,
                                          &slot->surface, 1, &surface_attr, 1)) != VA_STATUS_SUCCESS)
        {
            slot->surface = VA_INVALID_SURFACE;
            FAIL("The encoder's input surfaces could not be created (%s).", va.vaErrorStr(status));
        }
        if ((status = va.vaCreateImage(enc->dev.dpy, &image_format, enc->coded_width, enc->coded_height,
                                       &slot->image)) != VA_STATUS_SUCCESS)
        {
            slot->image.image_id = VA_INVALID_ID;
            FAIL("The encoder's upload images could not be created (%s).", va.vaErrorStr(status));
        }
        if ((status = va.vaCreateBuffer(enc->dev.dpy, enc->context, VAEncCodedBufferType, enc->coded_size, 1, NULL,
                                        &slot->coded)) != VA_STATUS_SUCCESS)
        {
            slot->coded = VA_INVALID_ID;
            FAIL("The encoder's output buffers could not be created (%s).", va.vaErrorStr(status));
        }
    }

    {
        struct nal_buffer nal = {0};
        write_sequence_header(&nal, enc);
        if (nal.failed) FAIL("Out of memory.");
        enc->own_headers = nal.data;
        enc->own_headers_size = nal.size;
    }

    if (pthread_create(&enc->worker, NULL, worker_main, enc)) FAIL("The encoder thread could not be started.");
    enc->worker_started = 1;

    INFO("%s profile %d %ux%u (coded %ux%u) %u/%u fps, %s %llu/%llu bit/s, IDR every %u, packed headers %#x, "
         "%s, %u in flight, on %s (%s)", codec_name(params->codec), enc->profile, enc->width, enc->height,
         enc->coded_width, enc->coded_height, enc->fps_num, enc->fps_den, enc->rc_mode == VA_RC_VBR ? "VBR" : "CBR",
         (unsigned long long)enc->target_bitrate, (unsigned long long)enc->peak_bitrate, enc->idr_period,
         enc->packed, enc->async ? "vaSyncBuffer" : "vaSyncSurface", enc->slot_count, enc->dev.name,
         enc->dev.node.path);
    *out = enc;
    return 0;

fail:
    if (err && errlen && !err[0]) set_reason(err, errlen, "The VA-API encoder could not be created.");
    INFO("%s", err ? err : "create failed");
    vaenc_destroy(enc);
    return -1;
}

/* ---- per frame ------------------------------------------------------------------------------ */

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
    for (i = 0; i + elem <= pad_bytes; i += elem) memcpy(dst + row_bytes + i, src + row_bytes - elem, elem);
}

/* Copies the caller's frame into the image, repeating the last column and row into the padding. */
static void fill_image(struct vaenc *enc, uint8_t *map, const VAImage *image, const uint8_t *data, uint32_t pitch)
{
    uint32_t bps = enc->bytes_per_sample, y;
    uint32_t luma_bytes = enc->src_width * bps, chroma_bytes = (enc->src_width + 1) / 2 * 2 * bps;
    uint32_t luma_pad = enc->coded_width * bps - luma_bytes, chroma_pad = enc->coded_width * bps - chroma_bytes;
    uint32_t chroma_rows = (enc->src_height + 1) / 2;
    const uint8_t *chroma = data + (size_t)pitch * enc->src_height;
    uint8_t *luma_dst = map + image->offsets[0], *chroma_dst = map + image->offsets[1];

    for (y = 0; y < enc->coded_height; y++)
        write_row(luma_dst + (size_t)y * image->pitches[0],
                  data + (size_t)(y < enc->src_height ? y : enc->src_height - 1) * pitch, luma_bytes, luma_pad, bps);
    for (y = 0; y < enc->coded_height / 2; y++)
        write_row(chroma_dst + (size_t)y * image->pitches[1],
                  chroma + (size_t)(y < chroma_rows ? y : chroma_rows - 1) * pitch, chroma_bytes, chroma_pad, 2 * bps);
}

static int upload(struct vaenc *enc, struct va_slot *slot, const uint8_t *data, uint32_t pitch)
{
    VAStatus status;
    void *map;

    pthread_mutex_lock(&enc->va_lock);
    status = va.vaMapBuffer(enc->dev.dpy, slot->image.buf, &map);
    pthread_mutex_unlock(&enc->va_lock);
    if (status != VA_STATUS_SUCCESS)
    {
        INFO("vaMapBuffer(image) failed: %s", va.vaErrorStr(status));
        return -1;
    }
    fill_image(enc, map, &slot->image, data, pitch);
    pthread_mutex_lock(&enc->va_lock);
    va.vaUnmapBuffer(enc->dev.dpy, slot->image.buf);
    status = va.vaPutImage(enc->dev.dpy, slot->surface, slot->image.image_id, 0, 0, enc->coded_width,
                           enc->coded_height, 0, 0, enc->coded_width, enc->coded_height);
    pthread_mutex_unlock(&enc->va_lock);
    if (status != VA_STATUS_SUCCESS)
    {
        INFO("vaPutImage failed: %s", va.vaErrorStr(status));
        return -1;
    }
    return 0;
}

static int issue(struct vaenc *enc, struct va_slot *slot, const struct frame_state *f)
{
    struct va_buffers list = {{0}};
    VAStatus status = VA_STATUS_SUCCESS;
    unsigned int i;

    pthread_mutex_lock(&enc->va_lock);
    if (enc->params.codec == VKENC_CODEC_HEVC) add_hevc_params(enc, &list, f, slot);
    else add_h264_params(enc, &list, f, slot);
    if (!list.failed)
    {
        if ((status = va.vaBeginPicture(enc->dev.dpy, enc->context, slot->surface)) == VA_STATUS_SUCCESS)
        {
            status = va.vaRenderPicture(enc->dev.dpy, enc->context, list.ids, list.count);
            /* End the picture even when rendering failed, so the context is not left in a picture. */
            if (status == VA_STATUS_SUCCESS) status = va.vaEndPicture(enc->dev.dpy, enc->context);
            else va.vaEndPicture(enc->dev.dpy, enc->context);
        }
    }
    for (i = 0; i < list.count; i++) va.vaDestroyBuffer(enc->dev.dpy, list.ids[i]);
    pthread_mutex_unlock(&enc->va_lock);
    if (list.failed) return -1;
    if (status != VA_STATUS_SUCCESS)
    {
        INFO("frame %llu could not be encoded: %s", (unsigned long long)enc->frames_submitted, va.vaErrorStr(status));
        return -1;
    }
    return 0;
}

int vaenc_submit(struct vaenc *enc, const uint8_t *data, uint32_t pitch, int force_idr)
{
    struct frame_state f;
    struct va_slot *slot;
    int headers;

    if (!enc || !data) return -1;
    if (pitch < enc->src_width * enc->bytes_per_sample) return -1;
    pthread_mutex_lock(&enc->submit_lock);
    pthread_mutex_lock(&enc->ring_lock);
    if (enc->failed)
    {
        pthread_mutex_unlock(&enc->ring_lock);
        pthread_mutex_unlock(&enc->submit_lock);
        return -1;
    }
    slot = &enc->slots[enc->submit_index];
    if (slot->state != SLOT_FREE || enc->out_count + enc->encoding >= VAENC_MAX_QUEUED)
    {
        pthread_mutex_unlock(&enc->ring_lock);
        pthread_mutex_unlock(&enc->submit_lock);
        return 1;
    }
    pthread_mutex_unlock(&enc->ring_lock);

    memset(&f, 0, sizeof(f));
    f.idr = force_idr || !enc->have_ref || enc->gop_frame >= enc->idr_period;
    if (f.idr) enc->gop_frame = 0;
    f.gop_frame = enc->gop_frame;
    f.idr_pic_id = enc->idr_count & 0xffff;
    f.cur_recon = enc->recon_next;
    f.ref_recon = enc->ref_recon;
    headers = f.idr || (enc->params.header_spacing && enc->frames_since_headers >= enc->params.header_spacing);

    if (upload(enc, slot, data, pitch) || issue(enc, slot, &f))
    {
        pthread_mutex_lock(&enc->ring_lock);
        enc->failed = 1;
        pthread_mutex_unlock(&enc->ring_lock);
        pthread_mutex_unlock(&enc->submit_lock);
        return -1;
    }

    /* This frame becomes the reference for the next one. */
    enc->ref_recon = f.cur_recon;
    enc->recon_next = (f.cur_recon + 1) % VAENC_RECON;
    enc->have_ref = 1;
    enc->gop_frame++;
    enc->frames_since_headers = headers ? 1 : enc->frames_since_headers + 1;
    if (f.idr) enc->idr_count++;

    pthread_mutex_lock(&enc->ring_lock);
    slot->is_idr = f.idr;
    slot->with_headers = headers;
    slot->frame = enc->frames_submitted++;
    slot->state = SLOT_ENCODING;
    enc->encoding++;
    enc->submit_index = (enc->submit_index + 1) % enc->slot_count;
    pthread_cond_broadcast(&enc->ring_cond);
    pthread_mutex_unlock(&enc->ring_lock);
    pthread_mutex_unlock(&enc->submit_lock);
    return 0;
}

/* ---- output --------------------------------------------------------------------------------- */

struct nal_span
{
    size_t start;                 /* start code */
    size_t end;                   /* next start code or the end */
    int type;
};

static int is_aud(const struct vaenc *enc, int type)
{
    return enc->params.codec == VKENC_CODEC_HEVC ? type == 35 : type == 9;
}

static int is_parameter_set(const struct vaenc *enc, int type)
{
    return enc->params.codec == VKENC_CODEC_HEVC ? type >= 32 && type <= 34 : type == 7 || type == 8;
}

static int is_sps(const struct vaenc *enc, int type)
{
    return enc->params.codec == VKENC_CODEC_HEVC ? type == 33 : type == 7;
}

/* Splits an Annex-B access unit; returns the number of NAL units (at most max). */
static unsigned int split_nals(const struct vaenc *enc, const uint8_t *data, size_t size, struct nal_span *nals,
                               unsigned int max)
{
    unsigned int count = 0;
    size_t i = 0;

    while (i + 3 <= size)
    {
        size_t start, payload;

        if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
        {
            i++;
            continue;
        }
        start = i > 0 && data[i - 1] == 0 ? i - 1 : i;
        payload = i + 3;
        if (count && nals[count - 1].end > start) nals[count - 1].end = start;
        if (count == max) break;
        nals[count].start = start;
        nals[count].end = size;
        nals[count].type = payload < size ? (enc->params.codec == VKENC_CODEC_HEVC ? (data[payload] >> 1) & 0x3f
                                                                                  : data[payload] & 0x1f) : -1;
        count++;
        i = payload;
    }
    return count;
}

/* Builds the access unit vaenc_receive hands out: the driver's output, with parameter sets put in
 * where they are due and missing. */
static struct va_packet *make_packet(struct vaenc *enc, const struct va_slot *slot, uint8_t *raw, size_t size)
{
    struct nal_span nals[64];
    unsigned int count = split_nals(enc, raw, size, nals, ARRAY_SIZE(nals)), i;
    const uint8_t *insert = NULL;
    size_t insert_size = 0, insert_at = 0, ps_size = 0;
    struct va_packet *packet;
    int has_sps = 0;

    for (i = 0; i < count; i++)
    {
        if (is_sps(enc, nals[i].type)) has_sps = 1;
        if (is_parameter_set(enc, nals[i].type)) ps_size += nals[i].end - nals[i].start + 1;
    }

    if (slot->is_idr && has_sps)
    {
        /* Keep the driver's parameter sets for frames between IDRs (header_spacing). */
        uint8_t *cache = malloc(ps_size);
        size_t pos = 0;

        if (cache)
        {
            for (i = 0; i < count; i++)
            {
                size_t len = nals[i].end - nals[i].start;
                const uint8_t *nal = raw + nals[i].start;

                if (!is_parameter_set(enc, nals[i].type)) continue;
                if (nal[2] == 1) cache[pos++] = 0;  /* four-byte start codes */
                memcpy(cache + pos, nal, len);
                pos += len;
            }
            free(enc->driver_headers);
            enc->driver_headers = cache;
            enc->driver_headers_size = pos;
        }
    }
    if (slot->with_headers && !has_sps)
    {
        if (enc->driver_headers)
        {
            insert = enc->driver_headers;
            insert_size = enc->driver_headers_size;
        }
        else
        {
            if (!enc->warned_no_headers++)
                INFO("the driver wrote no parameter sets; using our own");
            insert = enc->own_headers;
            insert_size = enc->own_headers_size;
        }
        /* After the access unit delimiter, which must come first. */
        if (count && is_aud(enc, nals[0].type)) insert_at = nals[0].end;
    }

    if (!(packet = calloc(1, sizeof(*packet))) || !(packet->data = malloc(size + insert_size + 1)))
    {
        free(packet);
        return NULL;
    }
    memcpy(packet->data, raw, insert_at);
    if (insert_size) memcpy(packet->data + insert_at, insert, insert_size);
    memcpy(packet->data + insert_at + insert_size, raw + insert_at, size - insert_at);
    packet->size = size + insert_size;
    packet->is_idr = slot->is_idr;
    return packet;
}

/* Waits for the slot's frame and copies its output; NULL data on failure. */
static struct va_packet *finish_slot(struct vaenc *enc, struct va_slot *slot)
{
    VACodedBufferSegment *segments, *seg;
    struct va_packet *packet = NULL;
    uint8_t *raw = NULL;
    size_t size = 0;
    VAStatus status;

    /* Waits run without va_lock: Mesa drops its own lock while it waits. */
    if (enc->async)
    {
        status = va.vaSyncBuffer(enc->dev.dpy, slot->coded, VA_TIMEOUT_INFINITE);
        if (status == VA_STATUS_ERROR_UNIMPLEMENTED) status = va.vaSyncSurface(enc->dev.dpy, slot->surface);
    }
    else status = va.vaSyncSurface(enc->dev.dpy, slot->surface);
    if (status != VA_STATUS_SUCCESS)
    {
        INFO("frame %llu failed to encode: %s", (unsigned long long)slot->frame, va.vaErrorStr(status));
        return NULL;
    }

    pthread_mutex_lock(&enc->va_lock);
    if ((status = va.vaMapBuffer(enc->dev.dpy, slot->coded, (void **)&segments)) != VA_STATUS_SUCCESS)
    {
        pthread_mutex_unlock(&enc->va_lock);
        INFO("frame %llu: the output could not be read: %s", (unsigned long long)slot->frame, va.vaErrorStr(status));
        return NULL;
    }
    for (seg = segments; seg; seg = seg->next) size += seg->size;
    if ((raw = malloc(size + 1)))
    {
        size = 0;
        for (seg = segments; seg; seg = seg->next)
        {
            if (seg->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK)
                INFO("frame %llu overflowed the output buffer", (unsigned long long)slot->frame);
            memcpy(raw + size, seg->buf, seg->size);
            size += seg->size;
        }
    }
    va.vaUnmapBuffer(enc->dev.dpy, slot->coded);
    pthread_mutex_unlock(&enc->va_lock);

    if (raw && size) packet = make_packet(enc, slot, raw, size);
    else if (raw) INFO("frame %llu: the encoder wrote nothing", (unsigned long long)slot->frame);
    free(raw);
    return packet;
}

static void *worker_main(void *arg)
{
    struct vaenc *enc = arg;

    for (;;)
    {
        struct va_slot *slot;
        struct va_packet *packet;

        pthread_mutex_lock(&enc->ring_lock);
        while (!enc->stop && enc->slots[enc->sync_index].state != SLOT_ENCODING)
            pthread_cond_wait(&enc->ring_cond, &enc->ring_lock);
        slot = &enc->slots[enc->sync_index];
        if (slot->state != SLOT_ENCODING)
        {
            pthread_mutex_unlock(&enc->ring_lock);
            break;
        }
        pthread_mutex_unlock(&enc->ring_lock);

        if (!(packet = finish_slot(enc, slot)) && (packet = calloc(1, sizeof(*packet))))
            packet->error = 1;

        pthread_mutex_lock(&enc->ring_lock);
        if (!packet || packet->error) enc->failed = 1;
        if (packet)
        {
            if (enc->out_tail) enc->out_tail->next = packet;
            else enc->out_head = packet;
            enc->out_tail = packet;
            enc->out_count++;
        }
        slot->state = SLOT_FREE;
        enc->encoding--;
        enc->sync_index = (enc->sync_index + 1) % enc->slot_count;
        pthread_cond_broadcast(&enc->ring_cond);
        pthread_mutex_unlock(&enc->ring_lock);
    }
    return NULL;
}

int vaenc_receive(struct vaenc *enc, uint8_t **buf, size_t *len, int *is_idr)
{
    struct va_packet *packet;

    if (!enc || !buf || !len) return -1;
    pthread_mutex_lock(&enc->ring_lock);
    if (!(packet = enc->out_head))
    {
        pthread_mutex_unlock(&enc->ring_lock);
        return 0;
    }
    if (!(enc->out_head = packet->next)) enc->out_tail = NULL;
    enc->out_count--;
    pthread_mutex_unlock(&enc->ring_lock);

    if (packet->error)
    {
        free(packet);
        return -1;
    }
    *buf = packet->data;
    *len = packet->size;
    if (is_idr) *is_idr = packet->is_idr;
    free(packet);
    return 1;
}

int vaenc_drain(struct vaenc *enc)
{
    struct timespec deadline;
    int ret = 0;

    if (!enc) return -1;
    pthread_mutex_lock(&enc->submit_lock);
    pthread_mutex_lock(&enc->ring_lock);
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)VAENC_TIMEOUT_S * (enc->encoding + 1);
    while (enc->encoding && !ret)
        ret = pthread_cond_timedwait(&enc->ring_cond, &enc->ring_lock, &deadline);
    ret = enc->encoding ? -1 : 0;
    pthread_mutex_unlock(&enc->ring_lock);
    pthread_mutex_unlock(&enc->submit_lock);
    if (ret) INFO("draining the encoder timed out");
    return ret;
}

/* ---- test only ------------------------------------------------------------------------------ */

int vaenc_dump_packed_headers(const vkenc_params *params, unsigned int frames, const char *path)
{
    struct nal_buffer nal = {0};
    struct vaenc *enc;
    struct frame_state f;
    unsigned int i;
    FILE *file;
    int ret = 0;

    if (!params || !path || (params->codec != VKENC_CODEC_H264 && params->codec != VKENC_CODEC_HEVC)) return -1;
    if (!(enc = calloc(1, sizeof(*enc)))) return -1;
    setup_common(enc, params);
    enc->rc_mode = params->rc == VKENC_RC_VBR ? VA_RC_VBR : VA_RC_CBR;
    if (params->codec == VKENC_CODEC_HEVC)
    {
        enc->coded_width = align_up(enc->width, 16);
        enc->coded_height = align_up(enc->height, 16);
        setup_hevc(enc, VA_ATTRIB_NOT_SUPPORTED, VA_ATTRIB_NOT_SUPPORTED);
    }
    else
    {
        enc->profile = params->profile == 66 || params->profile == 256 ? VAProfileH264ConstrainedBaseline
                       : params->profile == 77 ? VAProfileH264Main : VAProfileH264High;
        enc->coded_width = align_up(enc->width, 16);
        enc->coded_height = align_up(enc->height, 16);
        setup_h264(enc);
    }
    setup_vui(enc);

    memset(&f, 0, sizeof(f));
    for (i = 0; i < frames; i++)
    {
        f.idr = !i || f.gop_frame + 1 >= enc->idr_period;
        f.gop_frame = f.idr ? 0 : f.gop_frame + 1;
        if (f.idr)
        {
            f.idr_pic_id = i ? (f.idr_pic_id + 1) & 0xffff : 0;
            write_sequence_header(&nal, enc);
        }
        if (params->codec == VKENC_CODEC_HEVC)
        {
            static const uint8_t fake_data[2] = {0xff, 0x80};   /* a byte of slice data, trailing bits */
            write_hevc_slice_header(&nal, enc, &f);
            nal_append(&nal, fake_data, sizeof(fake_data));
        }
        else write_h264_slice_header(&nal, enc, &f, 1);
    }
    if (nal.failed || !(file = fopen(path, "wb")))
    {
        free(nal.data);
        free(enc);
        return -1;
    }
    if (fwrite(nal.data, 1, nal.size, file) != nal.size) ret = -1;
    if (fclose(file)) ret = -1;
    free(nal.data);
    free(enc);
    return ret;
}
