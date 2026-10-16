/*
 * encoder.c - vkenc.h's functions: picks Vulkan Video or VA-API per codec and forwards to it.
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 *
 * Vulkan Video (vkenc.c) is used when it offers the codec, VA-API (vaenc.c) otherwise. When neither
 * works, vkenc_caps.reason explains both in one sentence. NEUTRON_AMF_BACKEND=vulkan|vaapi forces one
 * backend (testing); its own reason is reported then.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkenc_backend.h"

enum backend
{
    BACKEND_NONE,
    BACKEND_VULKAN,
    BACKEND_VAAPI,
};

struct vkenc
{
    enum backend backend;
    struct vkvideo *vulkan;
    struct vaenc *vaapi;
};

static void log_info(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fputs("vkenc: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static const char *codec_name(vkenc_codec codec)
{
    return codec == VKENC_CODEC_HEVC ? "HEVC" : "H.264";
}

/* NEUTRON_AMF_BACKEND=vulkan|vaapi; anything else (or unset) chooses automatically. */
static enum backend forced_backend(void)
{
    const char *env = getenv("NEUTRON_AMF_BACKEND");

    if (!env) return BACKEND_NONE;
    if (!strcmp(env, "vulkan")) return BACKEND_VULKAN;
    if (!strcmp(env, "vaapi")) return BACKEND_VAAPI;
    return BACKEND_NONE;
}

/* ---- capabilities --------------------------------------------------------------------------- */

struct choice
{
    int done;
    enum backend backend;         /* the backend vkenc_create uses first; NONE when neither works */
    vkenc_caps caps;
    struct enc_probe vulkan, vaapi;
};

static pthread_mutex_t choice_lock = PTHREAD_MUTEX_INITIALIZER;
static struct choice choices[3];

static void capitalize(char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z') s[0] -= 'a' - 'A';
}

/* One sentence for "neither backend can encode". */
static void join_reasons(const struct choice *c, vkenc_codec codec, char *reason, size_t len)
{
    const struct enc_probe *vk = &c->vulkan, *va = &c->vaapi;

    if (va->problem == ENC_PROBLEM_CODEC_NOT_BUILT
            && (vk->problem == ENC_PROBLEM_NO_CODEC || vk->problem == ENC_PROBLEM_ENCODE_HIDDEN))
    {
        /* The root cause on Fedora and openSUSE: RADV then lacks the codec too, or its whole encoder. */
        snprintf(reason, len, "Mesa was built without %s (Fedora and openSUSE ship it that way), so neither "
                 "Vulkan Video nor VA-API can encode it.", codec_name(codec));
        return;
    }
    /* Each clause is well under 120 characters; the bound keeps the sentence within vkenc_caps.reason. */
    snprintf(reason, len, "%.120s and %.120s.", vk->clause, va->clause);
    capitalize(reason);
}

static void choose(vkenc_codec codec, struct choice *c)
{
    enum backend forced = forced_backend();

    memset(c, 0, sizeof(*c));
    if (forced != BACKEND_VAAPI)
    {
        vkvideo_probe(codec, &c->vulkan);
        if (c->vulkan.caps.available)
        {
            c->backend = BACKEND_VULKAN;
            c->caps = c->vulkan.caps;
            return;
        }
        if (forced == BACKEND_VULKAN)
        {
            c->caps = c->vulkan.caps;
            return;
        }
    }
    vaenc_probe(codec, &c->vaapi);
    if (c->vaapi.caps.available)
    {
        c->backend = BACKEND_VAAPI;
        c->caps = c->vaapi.caps;
        if (forced == BACKEND_NONE)
            log_info("%s through VA-API on %s: Vulkan Video cannot (%s)", codec_name(codec), c->caps.device_name,
                     c->vulkan.clause);
        return;
    }
    if (forced == BACKEND_VAAPI)
    {
        c->caps = c->vaapi.caps;
        return;
    }

    /* Neither: the GPU of whichever backend found one, and both reasons. */
    c->caps = c->vaapi.caps.vendor_id ? c->vaapi.caps : c->vulkan.caps;
    c->caps.available = 0;
    join_reasons(c, codec, c->caps.reason, sizeof(c->caps.reason));
}

static const struct choice *get_choice(vkenc_codec codec)
{
    struct choice *c = &choices[codec];

    pthread_mutex_lock(&choice_lock);
    if (!c->done)
    {
        choose(codec, c);
        c->done = 1;
    }
    pthread_mutex_unlock(&choice_lock);
    return c;
}

int vkenc_query_caps(vkenc_codec codec, vkenc_caps *caps)
{
    if (!caps) return -1;
    if (codec != VKENC_CODEC_H264 && codec != VKENC_CODEC_HEVC)
    {
        memset(caps, 0, sizeof(*caps));
        snprintf(caps->reason, sizeof(caps->reason), "This codec is not supported.");
        return 0;
    }
    *caps = get_choice(codec)->caps;
    return 0;
}

const char *vkenc_backend_name(vkenc_codec codec)
{
    if (codec != VKENC_CODEC_H264 && codec != VKENC_CODEC_HEVC) return "none";
    switch (get_choice(codec)->backend)
    {
    case BACKEND_VULKAN: return "vulkan";
    case BACKEND_VAAPI: return "vaapi";
    default: return "none";
    }
}

/* ---- encoders ------------------------------------------------------------------------------- */

int vkenc_create(const vkenc_params *params, vkenc **out, char *err, size_t errlen)
{
    enum backend forced = forced_backend();
    char vk_err[256] = "", va_err[256] = "";
    const struct choice *c;
    struct enc_probe va_probe;
    vkenc *enc;

    if (!params || !out) return -1;
    *out = NULL;
    if (params->codec != VKENC_CODEC_H264 && params->codec != VKENC_CODEC_HEVC)
    {
        if (err && errlen) snprintf(err, errlen, "Only H.264 and HEVC can be encoded.");
        return -1;
    }
    if (!(enc = calloc(1, sizeof(*enc))))
    {
        if (err && errlen) snprintf(err, errlen, "Out of memory.");
        return -1;
    }
    c = get_choice(params->codec);

    if (c->backend == BACKEND_VULKAN)
    {
        if (!vkvideo_create(params, &enc->vulkan, vk_err, sizeof(vk_err)))
        {
            enc->backend = BACKEND_VULKAN;
            *out = enc;
            return 0;
        }
        /* Vulkan Video offers the codec but not this encoder (a profile, a size, a device error):
         * VA-API may still manage it. */
        if (forced == BACKEND_NONE)
        {
            vaenc_probe(params->codec, &va_probe);
            if (va_probe.caps.available)
            {
                log_info("Vulkan Video could not create the encoder (%s); trying VA-API", vk_err);
                if (!vaenc_create(params, &enc->vaapi, va_err, sizeof(va_err)))
                {
                    enc->backend = BACKEND_VAAPI;
                    *out = enc;
                    return 0;
                }
                if (err && errlen) snprintf(err, errlen, "Vulkan Video: %s VA-API: %s", vk_err, va_err);
                free(enc);
                return -1;
            }
        }
        if (err && errlen) snprintf(err, errlen, "%s", vk_err);
    }
    else if (c->backend == BACKEND_VAAPI)
    {
        if (!vaenc_create(params, &enc->vaapi, va_err, sizeof(va_err)))
        {
            enc->backend = BACKEND_VAAPI;
            *out = enc;
            return 0;
        }
        if (err && errlen) snprintf(err, errlen, "%s", va_err);
    }
    else if (err && errlen) snprintf(err, errlen, "%s", c->caps.reason);
    free(enc);
    return -1;
}

int vkenc_submit(vkenc *enc, const uint8_t *data, uint32_t pitch, int force_idr)
{
    if (!enc) return -1;
    if (enc->backend == BACKEND_VAAPI) return vaenc_submit(enc->vaapi, data, pitch, force_idr);
    return vkvideo_submit(enc->vulkan, data, pitch, force_idr);
}

int vkenc_receive(vkenc *enc, uint8_t **buf, size_t *len, int *is_idr)
{
    if (!enc) return -1;
    if (enc->backend == BACKEND_VAAPI) return vaenc_receive(enc->vaapi, buf, len, is_idr);
    return vkvideo_receive(enc->vulkan, buf, len, is_idr);
}

void vkenc_free_packet(uint8_t *buf)
{
    free(buf);
}

int vkenc_drain(vkenc *enc)
{
    if (!enc) return -1;
    if (enc->backend == BACKEND_VAAPI) return vaenc_drain(enc->vaapi);
    return vkvideo_drain(enc->vulkan);
}

void vkenc_destroy(vkenc *enc)
{
    if (!enc) return;
    if (enc->backend == BACKEND_VAAPI) vaenc_destroy(enc->vaapi);
    else vkvideo_destroy(enc->vulkan);
    free(enc);
}
