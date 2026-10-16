/*
 * vkenc.h - Neutron's video encoder backend for amfrt64 (the Linux half).
 *
 * amfrt64.dll answers Adobe's AMF calls (Premiere Pro, Media Encoder, After Effects) and hands the
 * frames to this backend, which encodes them through the host's Vulkan Video driver (Mesa RADV on
 * AMD; the NVIDIA driver works too, for development) or, where RADV hides its encoder (older VCN
 * firmware, Polaris/Vega, the Steam Deck), through Mesa's VA-API driver (encoder.c picks; vkenc.c is
 * the Vulkan backend, vaenc.c the VA-API one). Plain C, Linux calling convention, no Wine types: the
 * backend can be built and tested on its own.
 *
 * What Adobe needs (RE 2026-10-16, ~/neutron-handoffs/amf-adobe-usage-2026-10-16.md):
 *   - host-memory NV12 (8-bit) or P010 (10-bit) frames, top-down, chroma plane at data + pitch * height;
 *   - H.264 (Baseline/Main/High, levels 3.2-5.2) and HEVC (Main, Main10), no B-frames;
 *   - CBR or peak-constrained VBR, IDR period N with parameter sets in band;
 *   - Annex-B output, one access unit per frame, in submission order.
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 */
#ifndef NEUTRON_VKENC_H
#define NEUTRON_VKENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vkenc_codec
{
    VKENC_CODEC_H264 = 1,
    VKENC_CODEC_HEVC = 2,
} vkenc_codec;

/* Same values as AMF_SURFACE_NV12 / AMF_SURFACE_P010. */
typedef enum vkenc_format
{
    VKENC_FMT_NV12 = 1,
    VKENC_FMT_P010 = 10,
} vkenc_format;

typedef enum vkenc_rc
{
    VKENC_RC_CBR = 1,
    VKENC_RC_VBR = 2,   /* peak-constrained VBR: target_bitrate average, peak_bitrate maximum */
} vkenc_rc;

typedef struct vkenc_caps
{
    int      available;          /* 1 when this codec can be encoded on the chosen device */
    uint32_t vendor_id;          /* PCI vendor of the chosen device (0x1002 = AMD) */
    uint32_t device_id;
    char     device_name[256];
    uint32_t min_width, min_height;
    uint32_t max_width, max_height;
    uint64_t max_bitrate;        /* bits per second */
    int      supports_10bit;     /* HEVC Main10 from P010 */
    int      supports_cbr;
    int      supports_vbr;
    char     reason[256];        /* when !available: why, in one plain sentence, for logs and doctor */
} vkenc_caps;

typedef struct vkenc_params
{
    vkenc_codec  codec;
    vkenc_format format;          /* NV12 for H.264 and HEVC Main; P010 for HEVC Main10 */
    uint32_t     width, height;   /* visible size; the backend aligns the coded size and crops */
    uint32_t     fps_num, fps_den;
    uint32_t     sar_num, sar_den;      /* pixel aspect ratio; 0/0 = unspecified */
    vkenc_rc     rc;
    uint64_t     target_bitrate;        /* bits per second */
    uint64_t     peak_bitrate;          /* bits per second; VBR only */
    uint32_t     idr_period;            /* frames between IDR frames; 0 = one IDR every 2 s */
    uint32_t     header_spacing;        /* parameter sets every N frames; 0 = on every IDR */
    int          filler_data;           /* CBR filler NAL units requested (may be ignored) */
    int          profile;     /* H.264: 66 Baseline, 77 Main, 100 High. HEVC: 1 Main, 2 Main10 */
    int          level;       /* H.264: AMF/spec value x10 (32..52). HEVC: AMF value = level x 30 (e.g. 153 = 5.1) */
    int          tier;        /* HEVC: 0 Main, 1 High */
    int          interlaced;  /* Adobe can ask; encode progressive and report it (not supported) */
    /* VUI, ISO/IEC 23091-2 code points; -1 = unspecified (omit from VUI) */
    int          color_primaries;
    int          transfer_characteristics;
    int          matrix_coefficients;
    int          full_range;
} vkenc_params;

typedef struct vkenc vkenc;

/* One-time probe. Finds a Vulkan device that can encode `codec` and fills `caps`. Only AMD devices
 * count unless the environment has NEUTRON_AMF_ANY_GPU=1 (development on NVIDIA). Safe to call
 * repeatedly and from any thread; results may be cached. Returns 0 on success (caps->available
 * says whether the codec is usable), negative on internal error. */
int vkenc_query_caps(vkenc_codec codec, vkenc_caps *caps);

/* Creates an encoder. On failure returns negative and writes one plain sentence to err. */
int vkenc_create(const vkenc_params *params, vkenc **out, char *err, size_t errlen);

/* Encodes one frame. `data` is the luma plane; the interleaved chroma plane starts at
 * data + pitch * height. `pitch` is in bytes (P010: 2 bytes per sample). The pixels are copied
 * before this returns, so the caller may free them at once. force_idr makes this frame an IDR.
 * Returns 0 when accepted, 1 when the encoder is full (the caller retries after receiving output),
 * negative on error. Thread-safe with respect to vkenc_receive. */
int vkenc_submit(vkenc *enc, const uint8_t *data, uint32_t pitch, int force_idr);

/* Takes the next finished access unit (Annex-B, parameter sets in band where due), in submission
 * order. Returns 1 and sets *buf, *len and *is_idr (free *buf with vkenc_free_packet), 0 when nothing is
 * ready yet, negative on error. Thread-safe with respect to vkenc_submit. */
int vkenc_receive(vkenc *enc, uint8_t **buf, size_t *len, int *is_idr);
void vkenc_free_packet(uint8_t *buf);

/* No more input: after this returns 0, every submitted frame is available from vkenc_receive. */
int vkenc_drain(vkenc *enc);

void vkenc_destroy(vkenc *enc);

#ifdef __cplusplus
}
#endif

#endif /* NEUTRON_VKENC_H */
