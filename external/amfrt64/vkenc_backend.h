/*
 * vkenc_backend.h - the two encoder backends behind vkenc.h, and what the dispatcher (encoder.c)
 * needs from them.
 *
 * vkenc.h is the contract amfrt64.c uses. encoder.c implements it by picking a backend per codec:
 *   - vkvideo_* (vkenc.c): Vulkan Video, Mesa RADV on AMD;
 *   - vaenc_*   (vaenc.c): VA-API, Mesa radeonsi on AMD. It covers the cards and firmware where RADV
 *     hides its encoder (older VCN firmware, the Steam Deck, Polaris, Vega, VCN 1).
 * Vulkan Video is used when it offers the codec, VA-API otherwise. NEUTRON_AMF_BACKEND=vulkan|vaapi
 * forces one (testing).
 *
 * Both backends follow vkenc.h's rules: frames are copied before submit returns, submit and receive
 * may run on two threads, packets are malloc()ed (vkenc_free_packet frees them).
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 */
#ifndef NEUTRON_VKENC_BACKEND_H
#define NEUTRON_VKENC_BACKEND_H

#include "vkenc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why a backend cannot encode a codec, from "nothing there" to "almost". */
enum enc_problem
{
    ENC_PROBLEM_NONE = 0,
    ENC_PROBLEM_NO_LIBRARY,       /* libvulkan.so.1 / libva.so.2 is not installed */
    ENC_PROBLEM_NO_GPU,           /* no (AMD) GPU this backend can open */
    ENC_PROBLEM_NO_DRIVER,        /* the GPU has no working driver for this API, or it is too old */
    ENC_PROBLEM_ENCODE_HIDDEN,    /* the driver offers no video encoding at all on this GPU */
    ENC_PROBLEM_NO_CODEC,         /* the driver encodes, but not this codec on this GPU */
    ENC_PROBLEM_CODEC_NOT_BUILT,  /* Mesa was built without H.264 and HEVC (Fedora, openSUSE) */
    ENC_PROBLEM_OTHER,
};

/* What one backend can do with one codec. */
struct enc_probe
{
    vkenc_caps caps;              /* caps.reason is a full sentence when !caps.available; the device
                                     fields are filled whenever a GPU was found */
    enum enc_problem problem;
    char clause[192];             /* the reason as a clause for joining two backends' reasons:
                                     starts lowercase unless it starts with a name, no period */
};

/* Vulkan Video (vkenc.c). */
struct vkvideo;
int  vkvideo_probe(vkenc_codec codec, struct enc_probe *probe);
int  vkvideo_create(const vkenc_params *params, struct vkvideo **out, char *err, size_t errlen);
int  vkvideo_submit(struct vkvideo *enc, const uint8_t *data, uint32_t pitch, int force_idr);
int  vkvideo_receive(struct vkvideo *enc, uint8_t **buf, size_t *len, int *is_idr);
int  vkvideo_drain(struct vkvideo *enc);
void vkvideo_destroy(struct vkvideo *enc);

/* VA-API (vaenc.c). */
struct vaenc;
int  vaenc_probe(vkenc_codec codec, struct enc_probe *probe);
int  vaenc_create(const vkenc_params *params, struct vaenc **out, char *err, size_t errlen);
int  vaenc_submit(struct vaenc *enc, const uint8_t *data, uint32_t pitch, int force_idr);
int  vaenc_receive(struct vaenc *enc, uint8_t **buf, size_t *len, int *is_idr);
int  vaenc_drain(struct vaenc *enc);
void vaenc_destroy(struct vaenc *enc);

/* TEST ONLY (vkenc-test --vaapi-headers): writes, as one Annex-B file, the packed parameter sets and
 * slice headers the VA-API backend would send for `frames` frames with these parameters, using the
 * defaults it falls back to when the driver reports no HEVC block sizes. Every slice header gets an
 * rbsp_slice_trailing_bits so that a parser can read it. Needs no GPU. Returns 0 or negative. */
int  vaenc_dump_packed_headers(const vkenc_params *params, unsigned int frames, const char *path);

/* For amfrt64's status file: the backend vkenc_query_caps picked for the codec, "vulkan", "vaapi" or
 * "none". Probes (and caches) like vkenc_query_caps. */
const char *vkenc_backend_name(vkenc_codec codec);

#ifdef __cplusplus
}
#endif

#endif /* NEUTRON_VKENC_BACKEND_H */
