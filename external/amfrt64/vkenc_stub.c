/*
 * vkenc_stub.c - TEST-ONLY fake vkenc backend. NEVER SHIP IT.
 *
 * It encodes nothing. Each "access unit" is a handful of fake NAL units with Annex-B start codes:
 * parameter sets (H.264 SPS+PPS, HEVC VPS+SPS+PPS) and an IDR slice on IDR frames, a non-IDR slice
 * otherwise, padded to the requested bit rate. That is enough for the amfrt64 object layer and
 * Adobe's call pattern (amfprobe) to be tested end to end before, or without, a GPU encoder.
 *
 * It reads every pixel of every submitted frame, so a bridge that hands it freed memory crashes or
 * shows up in the "read as freed memory" count (amfprobe overwrites each frame with 0xCD after use).
 *
 * Built only with `meson -Dtest_stub=true`. The marker string NEUTRON_VKENC_TEST_STUB is in every
 * log line it writes, so a packaging check can make sure no shipped amfrt64.dll contains it.
 *
 * Environment (stub only):
 *   VKENC_STUB_FAIL=all|h264|hevc   report both codecs, or one, as unavailable
 *   VKENC_STUB_NO_10BIT=1           report HEVC without Main10
 *   VKENC_STUB_LATENCY_MS=N         time from submit until a frame is ready (default 4)
 *   VKENC_STUB_DEPTH=N              frames in flight before vkenc_submit reports full (default 6)
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vkenc.h"

#define STUB "vkenc_stub (NEUTRON_VKENC_TEST_STUB, encodes nothing)"

struct pending
{
    uint8_t *buf;
    size_t len;
    int is_idr;
    uint64_t ready_ns;
};

struct vkenc
{
    vkenc_params params;
    pthread_mutex_t lock;
    struct pending *queue;
    unsigned int head, count, depth;
    uint32_t idr_period;
    uint64_t latency_ns;
    size_t frame_bytes;
    uint64_t frames_in, frames_out, freed_frames, full_returns;
    uint32_t checksum;
    int drained;
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static long env_long(const char *name, long def)
{
    const char *value = getenv(name);

    return value && *value ? strtol(value, NULL, 10) : def;
}

static const char *codec_name(vkenc_codec codec)
{
    return codec == VKENC_CODEC_H264 ? "H.264" : codec == VKENC_CODEC_HEVC ? "HEVC" : "?";
}

int vkenc_query_caps(vkenc_codec codec, vkenc_caps *caps)
{
    const char *fail = getenv("VKENC_STUB_FAIL");

    if (!caps) return -1;
    memset(caps, 0, sizeof(*caps));
    if (codec != VKENC_CODEC_H264 && codec != VKENC_CODEC_HEVC)
    {
        snprintf(caps->reason, sizeof(caps->reason), "%s: unknown codec %d", STUB, codec);
        return 0;
    }
    caps->vendor_id = 0x1002;
    caps->device_id = 0x0000;
    snprintf(caps->device_name, sizeof(caps->device_name), "%s", STUB);
    if (fail && (!strcmp(fail, "all") || !strcmp(fail, codec == VKENC_CODEC_H264 ? "h264" : "hevc")))
    {
        snprintf(caps->reason, sizeof(caps->reason), "%s: VKENC_STUB_FAIL=%s says %s is unavailable", STUB, fail,
                 codec_name(codec));
        return 0;
    }
    caps->available = 1;
    caps->min_width = caps->min_height = 128;
    caps->max_width = codec == VKENC_CODEC_H264 ? 4096 : 8192;
    caps->max_height = codec == VKENC_CODEC_H264 ? 2304 : 4352;
    caps->max_bitrate = codec == VKENC_CODEC_H264 ? 400000000 : 800000000;
    caps->supports_10bit = codec == VKENC_CODEC_HEVC && !env_long("VKENC_STUB_NO_10BIT", 0);
    caps->supports_cbr = caps->supports_vbr = 1;
    return 0;
}

static int fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s: %s", STUB, msg);
    return -1;
}

int vkenc_create(const vkenc_params *p, vkenc **out, char *err, size_t errlen)
{
    vkenc_caps caps;
    vkenc *enc;
    uint64_t bytes;

    if (!p || !out) return fail(err, errlen, "NULL argument");
    *out = NULL;
    if (vkenc_query_caps(p->codec, &caps) < 0 || !caps.available)
        return fail(err, errlen, caps.reason[0] ? caps.reason : "codec unavailable");
    if (p->format != VKENC_FMT_NV12 && p->format != VKENC_FMT_P010) return fail(err, errlen, "bad format");
    if (p->format == VKENC_FMT_P010 && !caps.supports_10bit) return fail(err, errlen, "no 10-bit");
    if (p->width < caps.min_width || p->width > caps.max_width || p->height < caps.min_height
            || p->height > caps.max_height)
        return fail(err, errlen, "size out of range");
    if (!p->fps_num || !p->fps_den) return fail(err, errlen, "bad frame rate");
    if (!p->target_bitrate) return fail(err, errlen, "bit rate 0");
    if (p->codec == VKENC_CODEC_H264 && p->profile != 66 && p->profile != 77 && p->profile != 100)
        return fail(err, errlen, "bad H.264 profile");
    if (p->codec == VKENC_CODEC_HEVC && p->profile != (p->format == VKENC_FMT_P010 ? 2 : 1))
        return fail(err, errlen, "HEVC profile does not match the format");

    if (!(enc = calloc(1, sizeof(*enc)))) return fail(err, errlen, "out of memory");
    enc->params = *p;
    pthread_mutex_init(&enc->lock, NULL);
    enc->depth = (unsigned int)env_long("VKENC_STUB_DEPTH", 6);
    if (enc->depth < 1 || enc->depth > 64) enc->depth = 6;
    enc->latency_ns = (uint64_t)env_long("VKENC_STUB_LATENCY_MS", 4) * 1000000ull;
    if (!(enc->queue = calloc(enc->depth, sizeof(*enc->queue))))
    {
        free(enc);
        return fail(err, errlen, "out of memory");
    }
    enc->idr_period = p->idr_period ? p->idr_period
            : (uint32_t)((2ull * p->fps_num + p->fps_den - 1) / p->fps_den);
    if (!enc->idr_period) enc->idr_period = 1;
    bytes = p->target_bitrate / 8 * p->fps_den / p->fps_num;
    enc->frame_bytes = bytes < 64 ? 64 : bytes > (4u << 20) ? (4u << 20) : (size_t)bytes;

    fprintf(stderr, "%s: create %s %s %ux%u %u/%u fps SAR %u:%u %s %llu/%llu bps%s IDR %u headers %u "
            "profile %d level %d tier %d%s VUI %d/%d/%d%s\n", STUB, codec_name(p->codec),
            p->format == VKENC_FMT_P010 ? "P010" : "NV12", p->width, p->height, p->fps_num, p->fps_den,
            p->sar_num, p->sar_den, p->rc == VKENC_RC_CBR ? "CBR" : "VBR",
            (unsigned long long)p->target_bitrate, (unsigned long long)p->peak_bitrate,
            p->filler_data ? " filler" : "", p->idr_period, p->header_spacing, p->profile, p->level, p->tier,
            p->interlaced ? " interlaced" : "", p->color_primaries, p->transfer_characteristics,
            p->matrix_coefficients, p->full_range ? " full-range" : "");
    *out = enc;
    return 0;
}

/* Reads every pixel (that is the point: freed memory faults or shows up) and tells whether the frame
 * looks like memory amfprobe already overwrote with 0xCD. */
static uint32_t read_frame(const vkenc *enc, const uint8_t *data, uint32_t pitch, int *freed)
{
    size_t bps = enc->params.format == VKENC_FMT_P010 ? 2 : 1;
    size_t luma_row = enc->params.width * bps, chroma_row = (size_t)((enc->params.width + 1) / 2) * 2 * bps;
    uint32_t h = enc->params.height, ch = (h + 1) / 2, y, sum = 2166136261u;
    const uint8_t *chroma = data + (size_t)pitch * h;
    size_t x, cd = 0, total = 0;

    for (y = 0; y < h; y++)
    {
        const uint8_t *row = data + (size_t)y * pitch;
        for (x = 0; x < luma_row; x++) sum = (sum ^ row[x]) * 16777619u;
        if (y == 0 || y == h - 1)
            for (x = 0; x < luma_row; x++, total++) cd += row[x] == 0xcd;
    }
    for (y = 0; y < ch; y++)
    {
        const uint8_t *row = chroma + (size_t)y * pitch;
        for (x = 0; x < chroma_row; x++) sum = (sum ^ row[x]) * 16777619u;
        if (y == ch - 1)
            for (x = 0; x < chroma_row; x++, total++) cd += row[x] == 0xcd;
    }
    *freed = cd == total;
    return sum;
}

/* One NAL unit: start code, header, payload bytes that can never form a start code. */
static size_t put_nal(uint8_t *p, const uint8_t *hdr, size_t hdr_len, size_t payload, uint32_t seed)
{
    size_t i;

    p[0] = p[1] = p[2] = 0;
    p[3] = 1;
    memcpy(p + 4, hdr, hdr_len);
    for (i = 0; i < payload; i++) p[4 + hdr_len + i] = 0x80 | ((seed + i * 37) & 0x7f);
    return 4 + hdr_len + payload;
}

int vkenc_submit(vkenc *enc, const uint8_t *data, uint32_t pitch, int force_idr)
{
    static const uint8_t h264_sps[] = {0x67}, h264_pps[] = {0x68}, h264_idr[] = {0x65}, h264_p[] = {0x41};
    static const uint8_t hevc_vps[] = {0x40, 0x01}, hevc_sps[] = {0x42, 0x01}, hevc_pps[] = {0x44, 0x01};
    static const uint8_t hevc_idr[] = {0x26, 0x01}, hevc_p[] = {0x02, 0x01};
    int hevc, is_idr, headers, freed;
    struct pending *slot;
    uint32_t sum;
    size_t len;
    uint8_t *buf;

    if (!enc || !data || !pitch) return -1;
    pthread_mutex_lock(&enc->lock);
    if (enc->drained)
    {
        pthread_mutex_unlock(&enc->lock);
        fprintf(stderr, "%s: submit after drain\n", STUB);
        return -1;
    }
    if (enc->count == enc->depth)
    {
        enc->full_returns++;
        pthread_mutex_unlock(&enc->lock);
        return 1;
    }

    sum = read_frame(enc, data, pitch, &freed);
    if (freed && enc->freed_frames++ < 3)
        fprintf(stderr, "%s: frame %llu reads as freed memory (0xCD)\n", STUB, (unsigned long long)enc->frames_in);
    enc->checksum ^= sum;

    hevc = enc->params.codec == VKENC_CODEC_HEVC;
    is_idr = force_idr || enc->frames_in % enc->idr_period == 0;
    headers = is_idr || (enc->params.header_spacing && enc->frames_in % enc->params.header_spacing == 0);
    if (!(buf = malloc(enc->frame_bytes + 64)))
    {
        pthread_mutex_unlock(&enc->lock);
        return -1;
    }
    len = 0;
    if (headers)
    {
        if (hevc) len += put_nal(buf + len, hevc_vps, sizeof(hevc_vps), 8, sum);
        len += put_nal(buf + len, hevc ? hevc_sps : h264_sps, hevc ? sizeof(hevc_sps) : sizeof(h264_sps), 8, sum);
        len += put_nal(buf + len, hevc ? hevc_pps : h264_pps, hevc ? sizeof(hevc_pps) : sizeof(h264_pps), 4, sum);
    }
    if (hevc) len += put_nal(buf + len, is_idr ? hevc_idr : hevc_p, 2, enc->frame_bytes - len, sum);
    else len += put_nal(buf + len, is_idr ? h264_idr : h264_p, 1, enc->frame_bytes - len, sum);

    slot = &enc->queue[(enc->head + enc->count++) % enc->depth];
    slot->buf = buf;
    slot->len = len;
    slot->is_idr = is_idr;
    slot->ready_ns = now_ns() + enc->latency_ns;
    enc->frames_in++;
    pthread_mutex_unlock(&enc->lock);
    return 0;
}

int vkenc_receive(vkenc *enc, uint8_t **buf, size_t *len, int *is_idr)
{
    struct pending *slot;

    if (!enc || !buf || !len) return -1;
    pthread_mutex_lock(&enc->lock);
    slot = &enc->queue[enc->head];
    if (!enc->count || (!enc->drained && now_ns() < slot->ready_ns))
    {
        pthread_mutex_unlock(&enc->lock);
        return 0;
    }
    *buf = slot->buf;
    *len = slot->len;
    if (is_idr) *is_idr = slot->is_idr;
    slot->buf = NULL;
    enc->head = (enc->head + 1) % enc->depth;
    enc->count--;
    enc->frames_out++;
    pthread_mutex_unlock(&enc->lock);
    return 1;
}

void vkenc_free_packet(uint8_t *buf)
{
    free(buf);
}

int vkenc_drain(vkenc *enc)
{
    if (!enc) return -1;
    pthread_mutex_lock(&enc->lock);
    enc->drained = 1;
    pthread_mutex_unlock(&enc->lock);
    return 0;
}

void vkenc_destroy(vkenc *enc)
{
    unsigned int i;

    if (!enc) return;
    fprintf(stderr, "%s: destroy: %llu frames in, %llu out, %u dropped, %llu full returns, "
            "%llu frames read as freed memory (0xCD), checksum %08x\n", STUB, (unsigned long long)enc->frames_in,
            (unsigned long long)enc->frames_out, enc->count, (unsigned long long)enc->full_returns,
            (unsigned long long)enc->freed_frames, enc->checksum);
    for (i = 0; i < enc->count; i++) free(enc->queue[(enc->head + i) % enc->depth].buf);
    free(enc->queue);
    pthread_mutex_destroy(&enc->lock);
    free(enc);
}
