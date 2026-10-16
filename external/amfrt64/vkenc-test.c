/*
 * vkenc-test.c - native Linux test for the vkenc backend (no Wine involved).
 *
 * Copyright 2026 the Neutron project. LGPL-2.1-or-later.
 *
 * Encodes a generated moving pattern and checks every access unit: one picture per packet, the
 * IDR flag matches the slice type, IDR frames fall where they were asked for and carry the
 * parameter sets. Optionally writes the source pattern as raw video for ffmpeg's psnr filter.
 *
 *   vkenc-test [--backend vulkan|vaapi] --caps
 *   vkenc-test [--backend vulkan|vaapi] --codec h264 --profile high --size 1920x1080 --fps 30 \
 *              --frames 300 --rc cbr --bitrate 20000000 --idr 60 --out out.h264 [--raw-out src.yuv]
 *              [--threads 2] [--bench] [--force-idr 100] [--pitch-pad 64] [--headers 30]
 *   vkenc-test --vaapi-headers FILE --codec ... --frames N --idr N   (no GPU needed)
 *
 * --backend forces one backend (it sets NEUTRON_AMF_BACKEND); without it the backend is chosen as in
 * amfrt64: Vulkan Video when it offers the codec, VA-API otherwise. VA-API encoding needs an AMD GPU
 * (or Intel with NEUTRON_AMF_ANY_GPU=1); NVIDIA's VA-API driver only decodes.
 * --vaapi-headers writes the packed parameter sets and slice headers the VA-API backend would send
 * for the stream, for checking with ffmpeg's trace_headers.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vkenc.h"
#include "vkenc_backend.h"

struct options
{
    vkenc_params params;
    uint32_t frames;
    const char *out;
    const char *raw_out;
    int threads;
    int bench;
    int force_idr[16];
    int force_idr_count;
    uint32_t pitch_pad;
};

struct stats
{
    uint64_t bytes;
    uint32_t packets;
    uint32_t idr_packets;
    uint32_t errors;
    uint32_t idr_at[4096];
    uint32_t idr_count;
    uint32_t last_idr;
    uint32_t last_headers;
};

struct run
{
    const struct options *opt;
    vkenc *enc;
    FILE *out;
    struct stats stats;
    volatile int failed;
};

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- the pattern ---------------------------------------------------------------------------- */

static int16_t sin_tab[1024];

static void init_tables(void)
{
    int i;
    for (i = 0; i < 1024; i++)
    {
        double x = i * 6.283185307179586 / 1024.0;
        double v = 0, term = x, x2;
        int n;
        /* sin by series after range reduction, to stay away from libm */
        if (x > 3.141592653589793) x -= 6.283185307179586;
        x2 = x * x;
        term = x;
        for (n = 1; n < 12; n++)
        {
            v += term;
            term *= -x2 / ((2 * n) * (2 * n + 1));
        }
        sin_tab[i] = (int16_t)(v * 1024);
    }
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* A moving scene: drifting gradients, a textured box crossing the frame, a band of fine noise. */
static void gen_frame(uint8_t *buf, uint32_t pitch, uint32_t w, uint32_t h, int p010, uint32_t t)
{
    uint32_t box = (w < h ? w : h) / 4, bx = (t * 7) % (w > box ? w - box : 1), by = (t * 3) % (h > box ? h - box : 1);
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2, x, y;
    uint8_t *chroma = buf + (size_t)pitch * h;

    for (y = 0; y < h; y++)
    {
        int row = sin_tab[(y * 2 + t * 3) & 1023] * 40 / 1024;
        for (x = 0; x < w; x++)
        {
            int v = 120 + row + sin_tab[(x + t * 5) & 1023] * 48 / 1024 + (int)((x + y) / 64 % 8) * 2;
            if (x >= bx && x < bx + box && y >= by && y < by + box)
                v = (((x - bx) / 12 + (y - by) / 12) & 1) ? 200 : 50;
            if (y > h / 2 && y < h / 2 + h / 16) v += (int)(hash32(x * 7919 + (y + t) * 104729) & 15) - 8;
            v = clamp(v, 16, 235);
            if (p010)
            {
                uint16_t s = (uint16_t)(clamp(v * 4 + (int)((x + t) & 3), 64, 940) << 6);
                memcpy(buf + y * pitch + x * 2, &s, 2);
            }
            else buf[y * pitch + x] = v;
        }
    }
    for (y = 0; y < ch; y++)
    {
        for (x = 0; x < cw; x++)
        {
            int u = 128 + sin_tab[(x * 3 + t * 4) & 1023] * 40 / 1024;
            int v = 128 + sin_tab[(y * 5 - t * 2 + 256) & 1023] * 40 / 1024;
            if (x * 2 >= bx && x * 2 < bx + box && y * 2 >= by && y * 2 < by + box)
            {
                u = 90;
                v = 200;
            }
            u = clamp(u, 16, 240);
            v = clamp(v, 16, 240);
            if (p010)
            {
                uint16_t su = (uint16_t)(u * 4 << 6), sv = (uint16_t)(v * 4 << 6);
                memcpy(chroma + y * pitch + x * 4, &su, 2);
                memcpy(chroma + y * pitch + x * 4 + 2, &sv, 2);
            }
            else
            {
                chroma[y * pitch + x * 2] = u;
                chroma[y * pitch + x * 2 + 1] = v;
            }
        }
    }
}

static void write_raw(FILE *f, const uint8_t *buf, uint32_t pitch, uint32_t w, uint32_t h, int bps)
{
    uint32_t y, ch = (h + 1) / 2;
    const uint8_t *chroma = buf + (size_t)pitch * h;

    for (y = 0; y < h; y++) fwrite(buf + (size_t)y * pitch, 1, (size_t)w * bps, f);
    for (y = 0; y < ch; y++) fwrite(chroma + (size_t)y * pitch, 1, (size_t)(w + 1) / 2 * 2 * bps, f);
}

/* ---- packet checks -------------------------------------------------------------------------- */

static int next_nal(const uint8_t *buf, size_t len, size_t *pos, size_t *start, size_t *end)
{
    size_t i = *pos;

    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) i++;
    if (i + 3 > len) return 0;
    *start = i + 3;
    i = *start;
    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && (buf[i + 2] == 1 || buf[i + 2] == 0))) i++;
    if (i + 3 > len) i = len;
    *end = i;
    *pos = i;
    return 1;
}

static void check_packet(struct run *run, const uint8_t *buf, size_t len, int is_idr, uint32_t index)
{
    int hevc = run->opt->params.codec == VKENC_CODEC_HEVC;
    int have_vps = 0, have_sps = 0, have_pps = 0, vcl = 0, idr_slice = 0, params_before_slice = 1;
    size_t pos = 0, start, end;
    char types[256] = "";
    int expect_idr = !index || index - run->stats.last_idr >= run->opt->params.idr_period, i;

    for (i = 0; i < run->opt->force_idr_count; i++)
        if ((uint32_t)run->opt->force_idr[i] == index) expect_idr = 1;
    if (len < 4 || buf[0] || buf[1] || (buf[2] != 1 && !(buf[2] == 0 && buf[3] == 1)))
    {
        printf("packet %u: does not start with a start code\n", index);
        run->stats.errors++;
    }
    while (next_nal(buf, len, &pos, &start, &end))
    {
        int type;
        char t[16];
        if (start >= len) break;
        type = hevc ? (buf[start] >> 1) & 0x3f : buf[start] & 0x1f;
        snprintf(t, sizeof(t), "%s%d", types[0] ? "," : "", type);
        if (strlen(types) + strlen(t) < sizeof(types)) strcat(types, t);
        if (hevc)
        {
            if (type == 32) have_vps = 1;
            else if (type == 33) have_sps = 1;
            else if (type == 34) have_pps = 1;
            else if (type < 32)
            {
                if (!vcl && (type == 19 || type == 20)) idr_slice = 1;
                if (!(have_vps && have_sps && have_pps)) params_before_slice = 0;
                vcl++;
            }
        }
        else
        {
            if (type == 7) have_sps = 1;
            else if (type == 8) have_pps = 1;
            else if (type >= 1 && type <= 5)
            {
                if (!vcl && type == 5) idr_slice = 1;
                if (!(have_sps && have_pps)) params_before_slice = 0;
                vcl++;
            }
        }
        (void)end;
    }
    if (vcl != 1)
    {
        printf("packet %u: %d slices, expected 1 (NAL types %s)\n", index, vcl, types);
        run->stats.errors++;
    }
    if (idr_slice != is_idr)
    {
        printf("packet %u: is_idr %d but the slice NAL says %d (NAL types %s)\n", index, is_idr, idr_slice, types);
        run->stats.errors++;
    }
    if (is_idr != expect_idr)
    {
        printf("packet %u: is_idr %d, expected %d\n", index, is_idr, expect_idr);
        run->stats.errors++;
    }
    if (is_idr && !params_before_slice)
    {
        printf("packet %u: IDR without %s in front (NAL types %s)\n", index, hevc ? "VPS/SPS/PPS" : "SPS/PPS",
               types);
        run->stats.errors++;
    }
    if (run->opt->params.header_spacing && index - run->stats.last_headers >= run->opt->params.header_spacing
            && !have_sps)
    {
        printf("packet %u: parameter sets missing at header spacing %u\n", index, run->opt->params.header_spacing);
        run->stats.errors++;
    }
    if (have_sps) run->stats.last_headers = index;
    if (is_idr) run->stats.last_idr = index;
    if (is_idr && run->stats.idr_count < 4096) run->stats.idr_at[run->stats.idr_count++] = index;
    if (index < 2 || getenv("VKENC_TEST_VERBOSE"))
        printf("packet %u: %zu bytes, idr %d, NAL types %s\n", index, len, is_idr, types);
}

static int take_packet(struct run *run, uint8_t *buf, size_t len, int is_idr)
{
    uint32_t index = run->stats.packets;

    check_packet(run, buf, len, is_idr, index);
    if (run->out && fwrite(buf, 1, len, run->out) != len)
    {
        printf("write failed\n");
        run->failed = 1;
    }
    run->stats.bytes += len;
    run->stats.packets++;
    run->stats.idr_packets += !!is_idr;
    vkenc_free_packet(buf);
    return 0;
}

/* Waits for one packet; returns 1, or -1 on error. */
static int receive_one(struct run *run, double timeout)
{
    double end = now() + timeout;
    uint8_t *buf;
    size_t len;
    int idr, r;

    for (;;)
    {
        r = vkenc_receive(run->enc, &buf, &len, &idr);
        if (r == 1) return take_packet(run, buf, len, idr), 1;
        if (r < 0) return -1;
        if (now() > end) return -1;
        usleep(100);
    }
}

static void *receiver_thread(void *arg)
{
    struct run *run = arg;

    while (run->stats.packets < run->opt->frames && !run->failed)
    {
        if (receive_one(run, 20.0) < 0)
        {
            printf("receive failed after %u packets\n", run->stats.packets);
            run->failed = 1;
        }
    }
    return NULL;
}

/* ---- main ----------------------------------------------------------------------------------- */

static void print_caps(vkenc_codec codec)
{
    vkenc_caps caps;
    int r = vkenc_query_caps(codec, &caps);

    printf("%s: query %d, available %d", codec == VKENC_CODEC_HEVC ? "HEVC" : "H.264", r, caps.available);
    if (!caps.available)
    {
        printf(", reason: %s\n", caps.reason);
        return;
    }
    printf(", backend %s, device %04x:%04x \"%s\", %ux%u..%ux%u, max %llu bit/s, 10-bit %d, CBR %d, VBR %d\n",
           vkenc_backend_name(codec), caps.vendor_id, caps.device_id, caps.device_name, caps.min_width,
           caps.min_height, caps.max_width, caps.max_height, (unsigned long long)caps.max_bitrate,
           caps.supports_10bit, caps.supports_cbr, caps.supports_vbr);
}

static void usage(void)
{
    fprintf(stderr, "usage: vkenc-test [--backend vulkan|vaapi] --caps | --codec h264|hevc\n"
            "  --profile baseline|main|high|main10 --size WxH --fps N[/D] --frames N --rc cbr|vbr\n"
            "  --bitrate BPS [--peak BPS] [--idr N] [--headers N] [--level N] [--tier 0|1] [--sar N:D]\n"
            "  [--color P,T,M,R] --out FILE [--raw-out FILE] [--threads 1|2] [--bench] [--force-idr N]\n"
            "  [--pitch-pad BYTES] | --vaapi-headers FILE (with the stream options)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    struct options opt;
    struct run run;
    vkenc_params *p = &opt.params;
    uint8_t *frames[8];
    uint32_t pitch, frame_size, nframes, i, f;
    double start, gen_time = 0, submit_time = 0, total;
    char err[256] = "";
    const char *headers_out = NULL;
    FILE *raw = NULL;
    pthread_t thread;
    int bps, r, a;

    memset(&opt, 0, sizeof(opt));
    memset(&run, 0, sizeof(run));
    p->codec = VKENC_CODEC_H264;
    p->format = VKENC_FMT_NV12;
    p->width = 1920;
    p->height = 1080;
    p->fps_num = 30;
    p->fps_den = 1;
    p->rc = VKENC_RC_CBR;
    p->target_bitrate = 20000000;
    p->profile = 100;
    p->color_primaries = p->transfer_characteristics = p->matrix_coefficients = 1; /* BT.709 */
    p->full_range = 0;
    opt.frames = 300;
    opt.threads = 1;

    /* The backend is chosen once, on the first vkenc call: set it before anything else. */
    for (a = 1; a + 1 < argc; a++)
    {
        if (strcmp(argv[a], "--backend")) continue;
        if (strcmp(argv[a + 1], "vulkan") && strcmp(argv[a + 1], "vaapi")) usage();
        setenv("NEUTRON_AMF_BACKEND", argv[a + 1], 1);
        printf("backend forced: %s\n", argv[a + 1]);
    }

    for (a = 1; a < argc; a++)
    {
        const char *arg = argv[a], *val = a + 1 < argc ? argv[a + 1] : NULL;
        if (!strcmp(arg, "--caps"))
        {
            print_caps(VKENC_CODEC_H264);
            print_caps(VKENC_CODEC_HEVC);
            return 0;
        }
        if (!strcmp(arg, "--bench"))
        {
            opt.bench = 1;
            continue;
        }
        if (!val) usage();
        a++;
        if (!strcmp(arg, "--codec")) p->codec = !strcmp(val, "hevc") ? VKENC_CODEC_HEVC : VKENC_CODEC_H264;
        else if (!strcmp(arg, "--profile"))
        {
            if (!strcmp(val, "baseline")) p->profile = 66;
            else if (!strcmp(val, "main")) p->profile = p->codec == VKENC_CODEC_HEVC ? 1 : 77;
            else if (!strcmp(val, "high")) p->profile = 100;
            else if (!strcmp(val, "main10"))
            {
                p->profile = 2;
                p->format = VKENC_FMT_P010;
            }
            else usage();
        }
        else if (!strcmp(arg, "--size")) { if (sscanf(val, "%ux%u", &p->width, &p->height) != 2) usage(); }
        else if (!strcmp(arg, "--fps"))
        {
            p->fps_den = 1;
            if (sscanf(val, "%u/%u", &p->fps_num, &p->fps_den) < 1) usage();
        }
        else if (!strcmp(arg, "--frames")) opt.frames = strtoul(val, NULL, 10);
        else if (!strcmp(arg, "--rc")) p->rc = !strcmp(val, "vbr") ? VKENC_RC_VBR : VKENC_RC_CBR;
        else if (!strcmp(arg, "--bitrate")) p->target_bitrate = strtoull(val, NULL, 10);
        else if (!strcmp(arg, "--peak")) p->peak_bitrate = strtoull(val, NULL, 10);
        else if (!strcmp(arg, "--idr")) p->idr_period = strtoul(val, NULL, 10);
        else if (!strcmp(arg, "--headers")) p->header_spacing = strtoul(val, NULL, 10);
        else if (!strcmp(arg, "--level")) p->level = strtol(val, NULL, 10);
        else if (!strcmp(arg, "--tier")) p->tier = strtol(val, NULL, 10);
        else if (!strcmp(arg, "--sar")) { if (sscanf(val, "%u:%u", &p->sar_num, &p->sar_den) != 2) usage(); }
        else if (!strcmp(arg, "--color"))
        {
            if (sscanf(val, "%d,%d,%d,%d", &p->color_primaries, &p->transfer_characteristics,
                       &p->matrix_coefficients, &p->full_range) != 4)
                usage();
        }
        else if (!strcmp(arg, "--out")) opt.out = val;
        else if (!strcmp(arg, "--raw-out")) opt.raw_out = val;
        else if (!strcmp(arg, "--threads")) opt.threads = strtol(val, NULL, 10);
        else if (!strcmp(arg, "--force-idr") && opt.force_idr_count < 16)
            opt.force_idr[opt.force_idr_count++] = strtol(val, NULL, 10);
        else if (!strcmp(arg, "--pitch-pad")) opt.pitch_pad = strtoul(val, NULL, 10);
        else if (!strcmp(arg, "--backend")) continue;   /* handled above */
        else if (!strcmp(arg, "--vaapi-headers")) headers_out = val;
        else usage();
    }
    if (!p->idr_period) p->idr_period = (2 * p->fps_num + p->fps_den / 2) / p->fps_den;
    if (p->codec == VKENC_CODEC_HEVC && p->profile != 2) p->profile = 1;

    if (headers_out)
    {
        r = vaenc_dump_packed_headers(p, opt.frames, headers_out);
        printf("VA-API packed headers for %u frames -> %s: %s\n", opt.frames, headers_out, r ? "FAILED" : "written");
        return r ? 1 : 0;
    }

    init_tables();
    bps = p->format == VKENC_FMT_P010 ? 2 : 1;
    pitch = (p->width + 1) / 2 * 2 * bps + opt.pitch_pad;
    frame_size = pitch * (p->height + (p->height + 1) / 2);
    nframes = opt.bench ? 8 : 1;
    for (i = 0; i < nframes; i++)
    {
        if (!(frames[i] = malloc(frame_size))) return 1;
        if (opt.bench) gen_frame(frames[i], pitch, p->width, p->height, bps == 2, i);
    }

    run.opt = &opt;
    if (vkenc_create(p, &run.enc, err, sizeof(err)))
    {
        printf("vkenc_create failed: %s\n", err);
        return 1;
    }
    if (opt.out && !(run.out = fopen(opt.out, "wb"))) return 1;
    if (opt.raw_out && !(raw = fopen(opt.raw_out, "wb"))) return 1;

    if (opt.threads > 1) pthread_create(&thread, NULL, receiver_thread, &run);
    start = now();
    for (f = 0; f < opt.frames && !run.failed; f++)
    {
        uint8_t *frame = frames[opt.bench ? f % nframes : 0];
        int force = 0;
        double t0;

        for (i = 0; i < (uint32_t)opt.force_idr_count; i++)
            if ((uint32_t)opt.force_idr[i] == f) force = 1;
        if (!opt.bench)
        {
            t0 = now();
            gen_frame(frame, pitch, p->width, p->height, bps == 2, f);
            if (raw) write_raw(raw, frame, pitch, p->width, p->height, bps);
            gen_time += now() - t0;
        }
        t0 = now();
        while ((r = vkenc_submit(run.enc, frame, pitch, force)) == 1)
        {
            if (opt.threads > 1) usleep(100);
            else if (receive_one(&run, 20.0) < 0)
            {
                run.failed = 1;
                break;
            }
        }
        submit_time += now() - t0;
        if (r < 0)
        {
            printf("vkenc_submit failed at frame %u\n", f);
            run.failed = 1;
        }
        if (opt.threads <= 1)
        {
            uint8_t *buf;
            size_t len;
            int idr;
            while (vkenc_receive(run.enc, &buf, &len, &idr) == 1) take_packet(&run, buf, len, idr);
        }
    }
    if (vkenc_drain(run.enc)) printf("vkenc_drain failed\n");
    if (opt.threads > 1) pthread_join(thread, NULL);
    else
        while (run.stats.packets < opt.frames && !run.failed)
            if (receive_one(&run, 20.0) < 0) run.failed = 1;
    total = now() - start;
    vkenc_destroy(run.enc);
    if (run.out) fclose(run.out);
    if (raw) fclose(raw);

    printf("frames %u, packets %u, IDR %u, bytes %llu, bitrate %.3f Mbit/s, errors %u\n", opt.frames,
           run.stats.packets, run.stats.idr_packets, (unsigned long long)run.stats.bytes,
           run.stats.packets ? run.stats.bytes * 8.0 * p->fps_num / p->fps_den / run.stats.packets / 1e6 : 0.0,
           run.stats.errors);
    printf("IDR at:");
    for (i = 0; i < run.stats.idr_count && i < 32; i++) printf(" %u", run.stats.idr_at[i]);
    printf("\n");
    printf("time %.3f s (pattern %.3f s), encode %.1f fps excluding the pattern, submit %.2f ms/frame\n", total,
           gen_time, opt.frames / (total - gen_time), submit_time * 1000 / opt.frames);
    for (i = 0; i < nframes; i++) free(frames[i]);
    if (run.failed || run.stats.errors || run.stats.packets != opt.frames)
    {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: OK\n");
    return 0;
}
