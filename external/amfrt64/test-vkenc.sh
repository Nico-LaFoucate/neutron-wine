#!/bin/bash
# test-vkenc.sh - builds vkenc-test and runs the backend tests, checking every output with
# ffprobe/ffmpeg. Native Linux, no Wine.
#
# Copyright 2026 the Neutron project. LGPL-2.1-or-later.
#
#   external/amfrt64/test-vkenc.sh [workdir]        (default /var/tmp/vkenc-work)
#
# Needs gcc, ffmpeg, ffprobe and Vulkan headers (VULKAN_HEADERS=<dir with vulkan/vulkan.h>, or one
# of the Vulkan-Headers checkouts the external build already has). On a machine without an AMD GPU
# it sets NEUTRON_AMF_ANY_GPU=1 so the tests run on whatever GPU encodes.

set -u
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-/var/tmp/vkenc-work}
mkdir -p "$work/logs"
cd "$work" || exit 1

fail=0
results=()
pass() { results+=("PASS  $1"); echo "PASS: $1"; }
bad() { results+=("FAIL  $1"); echo "FAIL: $1"; fail=1; }

# ---- build ----------------------------------------------------------------------------------------
headers=${VULKAN_HEADERS:-}
if [ -z "$headers" ]; then
    for d in /usr/include \
             /var/tmp/neutron-external/src/vkd3d-proton/khronos/Vulkan-Headers/include \
             /var/tmp/neutron-external/src/dxvk/include/vulkan/include \
             "$HOME/vkd3d-proton/khronos/Vulkan-Headers/include"; do
        if [ -f "$d/vulkan/vulkan_core.h" ] && grep -q VK_KHR_video_encode_h264 "$d/vulkan/vulkan_core.h"; then
            headers=$d
            break
        fi
    done
fi
if [ -z "$headers" ]; then
    echo "No Vulkan headers with video encode found; set VULKAN_HEADERS." >&2
    exit 1
fi
echo "Vulkan headers: $headers ($(grep -m1 '#define VK_HEADER_VERSION ' "$headers/vulkan/vulkan_core.h"))"

cflags="-std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -I$headers -I$here"
if ! gcc $cflags "$here/vkenc.c" "$here/vkenc-test.c" -o vkenc-test -ldl -lpthread 2> logs/build.log; then
    cat logs/build.log
    exit 1
fi
if [ -s logs/build.log ]; then cat logs/build.log; bad "build has warnings"; else pass "build (no warnings)"; fi
# The backend must not link libvulkan: it is dlopen()ed so amfrt64 loads without Vulkan.
gcc $cflags -shared "$here/vkenc.c" -o libvkenc-check.so -ldl -lpthread
if readelf -d libvkenc-check.so | grep NEEDED | grep -q vulkan; then bad "vkenc.c links libvulkan"; else pass "vkenc.c does not link libvulkan"; fi

# ---- helpers --------------------------------------------------------------------------------------
probe() { ffprobe -v error -select_streams v:0 -show_entries "stream=$2" -of default=nw=1:nk=1 "$1" | head -1; }

# check_stream NAME FILE FRAMES WIDTH HEIGHT PROFILE PIX_FMT IDR_PERIOD [FORCED_IDRS...]
check_stream() {
    local name=$1 file=$2 frames=$3 w=$4 h=$5 profile=$6 pixfmt=$7 period=$8
    shift 8
    local forced="$*" errs n got_w got_h got_profile got_pix keys expect last i f
    errs=$(ffmpeg -v error -i "$file" -f null - 2>&1)
    if [ -n "$errs" ]; then bad "$name: ffmpeg decode errors: $(echo "$errs" | head -3)"; return; fi
    n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$file")
    got_w=$(probe "$file" width); got_h=$(probe "$file" height)
    got_profile=$(probe "$file" profile); got_pix=$(probe "$file" pix_fmt)
    keys=$(ffprobe -v error -select_streams v:0 -show_entries frame=key_frame,pict_type -of csv=p=0 "$file" \
           | awk -F, '{ if ($1 == 1) k = k " " NR - 1; if ($2 != "I" && $2 != "P") b = b " " NR - 1 } END { print k "|" b }')
    expect=""
    last=-1000000
    for ((i = 0; i < frames; i++)); do
        f=0
        for x in $forced; do [ "$x" = "$i" ] && f=1; done
        if [ $i -eq 0 ] || [ $f -eq 1 ] || [ $((i - last)) -ge "$period" ]; then expect="$expect $i"; last=$i; fi
    done
    local ok=1 msg="frames $n, ${got_w}x${got_h}, $got_profile, $got_pix, keyframes at${keys%%|*}"
    [ "$n" = "$frames" ] || ok=0
    [ "$got_w" = "$w" ] && [ "$got_h" = "$h" ] || ok=0
    [ "$got_profile" = "$profile" ] || ok=0
    [ "$got_pix" = "$pixfmt" ] || ok=0
    [ "${keys%%|*}" = "$expect" ] || { ok=0; msg="$msg (expected keyframes at$expect)"; }
    [ -z "${keys##*|}" ] || { ok=0; msg="$msg, non-I/P frames at${keys##*|}"; }
    if [ $ok = 1 ]; then pass "$name: $msg"; else bad "$name: $msg"; fi
}

# check_bitrate NAME FILE FRAMES FPS TARGET TOLERANCE_PERCENT
check_bitrate() {
    local name=$1 file=$2 frames=$3 fps=$4 target=$5 tol=$6 size rate dev
    size=$(stat -c %s "$file")
    rate=$(awk -v s="$size" -v n="$frames" -v f="$fps" 'BEGIN { printf "%.3f", s * 8 / (n / f) / 1e6 }')
    dev=$(awk -v r="$rate" -v t="$target" 'BEGIN { printf "%.1f", (r * 1e6 - t) * 100 / t }')
    if awk -v d="$dev" -v tol="$tol" 'BEGIN { exit !(d <= tol && d >= -tol) }'; then
        pass "$name: bitrate $rate Mbit/s, target $(awk -v t="$target" 'BEGIN { print t / 1e6 }') Mbit/s ($dev%)"
    else
        bad "$name: bitrate $rate Mbit/s is $dev% off the $(awk -v t="$target" 'BEGIN { print t / 1e6 }') Mbit/s target"
    fi
}

# check_psnr NAME FILE RAW PIX_FMT WxH MIN_AVG_DB
check_psnr() {
    local name=$1 file=$2 raw=$3 pixfmt=$4 size=$5 min=$6 line avg worst
    # Tag the raw source like the stream (BT.709, limited range) so ffmpeg compares like with like.
    line=$(ffmpeg -hide_banner -i "$file" -f rawvideo -pix_fmt "$pixfmt" -s "$size" -i "$raw" -lavfi \
        "[0:v]settb=1/30,setpts=N[a];[1:v]settb=1/30,setpts=N,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv[b];[a][b]psnr=stats_file=logs/$name.psnr" \
        -f null - 2>&1 | grep -o 'PSNR y:.*')
    avg=$(echo "$line" | sed -n 's/.*average:\([0-9.inf]*\).*/\1/p')
    worst=$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^psnr_avg:/) { split($i, a, ":"); if (m == "" || a[2] + 0 < m) m = a[2] + 0 } } END { print m }' "logs/$name.psnr")
    if awk -v a="$avg" -v m="$min" 'BEGIN { exit !(a + 0 >= m) }'; then
        pass "$name: $line, worst frame $worst dB"
    else
        bad "$name: PSNR too low: $line, worst frame $worst dB"
    fi
}

# count_nal FILE NAME -> how many "NAME" units (e.g. "Sequence Parameter Set") are in the packets;
# the copy ffmpeg puts in the extradata, before the first packet, does not count.
count_nal() {
    ffmpeg -hide_banner -loglevel trace -i "$1" -c copy -bsf:v trace_headers -f null - 2>&1 \
        | awk -v what="$2" '/trace_headers/ && / Packet: / { p = 1 } p && /trace_headers/ && index($0, what) { n++ } END { print n + 0 }'
}

# run NAME ARGS... : runs vkenc-test, logs to logs/NAME.log
run() {
    local name=$1
    shift
    if ./vkenc-test "$@" > "logs/$name.log" 2>&1; then
        return 0
    fi
    tail -5 "logs/$name.log"
    return 1
}

# ---- tests ----------------------------------------------------------------------------------------
./vkenc-test --caps > logs/caps-amd-only.log 2>&1
echo "Caps (AMD only):"; sed 's/^/  /' logs/caps-amd-only.log
if grep -q "available 0, reason: ." logs/caps-amd-only.log || grep -q "available 1" logs/caps-amd-only.log; then
    pass "caps without NEUTRON_AMF_ANY_GPU: available or a reason sentence"
else
    bad "caps without NEUTRON_AMF_ANY_GPU gave no reason"
fi
if ! grep -q "available 1" logs/caps-amd-only.log; then export NEUTRON_AMF_ANY_GPU=1; fi
./vkenc-test --caps > logs/caps.log 2>&1
echo "Caps:"; sed 's/^/  /' logs/caps.log

# 1. H.264 High 1080p30, 300 frames, CBR 20 Mbit/s, IDR every 60 (raw source kept for PSNR).
if run t1-h264-1080p --codec h264 --profile high --size 1920x1080 --fps 30 --frames 300 --rc cbr \
        --bitrate 20000000 --idr 60 --out t1.h264 --raw-out src-1080p.nv12; then
    check_stream t1-h264-1080p t1.h264 300 1920 1080 High yuv420p 60
    check_bitrate t1-h264-1080p t1.h264 300 30 20000000 10
    check_psnr t1-h264-1080p t1.h264 src-1080p.nv12 nv12 1920x1080 40
else bad "t1-h264-1080p: encode failed"; fi

# 2. H.264 High 2160p60, 300 frames, CBR 50 Mbit/s; then the same from 8 pre-made frames for throughput.
if run t2-h264-2160p --codec h264 --profile high --size 3840x2160 --fps 60 --frames 300 --rc cbr \
        --bitrate 50000000 --idr 120 --out t2.h264; then
    check_stream t2-h264-2160p t2.h264 300 3840 2160 High yuv420p 120
    check_bitrate t2-h264-2160p t2.h264 300 60 50000000 10
else bad "t2-h264-2160p: encode failed"; fi
for t in 1 2; do
    if run t2-bench-$t --codec h264 --profile high --size 3840x2160 --fps 60 --frames 600 --rc cbr \
            --bitrate 50000000 --idr 120 --bench --threads $t --out /dev/null; then
        pass "t2-bench ($t thread(s)): $(grep '^time' logs/t2-bench-$t.log)"
    else bad "t2-bench-$t: encode failed"; fi
done
if run t2-bench-hevc --codec hevc --profile main --size 3840x2160 --fps 60 --frames 600 --rc cbr \
        --bitrate 50000000 --idr 120 --bench --threads 2 --out /dev/null; then
    pass "t2-bench HEVC 2160p60: $(grep '^time' logs/t2-bench-hevc.log)"
else bad "t2-bench-hevc: encode failed"; fi

# 3. HEVC Main 1080p VBR 15/25 Mbit/s; HEVC Main10 from P010 at 2160p.
if run t3-hevc-1080p --codec hevc --profile main --size 1920x1080 --fps 30 --frames 300 --rc vbr \
        --bitrate 15000000 --peak 25000000 --idr 60 --out t3.hevc --raw-out src-1080p-b.nv12; then
    check_stream t3-hevc-1080p t3.hevc 300 1920 1080 Main yuv420p 60
    check_bitrate t3-hevc-1080p t3.hevc 300 30 15000000 10
    check_psnr t3-hevc-1080p t3.hevc src-1080p-b.nv12 nv12 1920x1080 40
else bad "t3-hevc-1080p: encode failed"; fi
rm -f src-1080p-b.nv12
if run t3-hevc10-2160p --codec hevc --profile main10 --size 3840x2160 --fps 30 --frames 60 --rc vbr \
        --bitrate 40000000 --peak 60000000 --idr 30 --out t3b.hevc --raw-out src-2160p.p010; then
    check_stream t3-hevc10-2160p t3b.hevc 60 3840 2160 "Main 10" yuv420p10le 30
    check_psnr t3-hevc10-2160p t3b.hevc src-2160p.p010 p010le 3840x2160 40
else bad "t3-hevc10-2160p: encode failed"; fi
rm -f src-2160p.p010

# 5. Parameter sets in front of every IDR (vkenc-test checks each packet's NAL units too), with a
#    forced IDR off the regular grid and parameter sets every 25 frames.
n="$(count_nal t1.h264 'Sequence Parameter Set') SPS, $(count_nal t1.h264 'Picture Parameter Set') PPS"
if [ "$n" = "5 SPS, 5 PPS" ]; then pass "t5: t1.h264 has $n for 5 IDR frames"; else bad "t5: t1.h264 has $n for 5 IDR frames"; fi
n="$(count_nal t3.hevc 'Video Parameter Set') VPS, $(count_nal t3.hevc 'Sequence Parameter Set') SPS, $(count_nal t3.hevc 'Picture Parameter Set') PPS"
if [ "$n" = "5 VPS, 5 SPS, 5 PPS" ]; then pass "t5: t3.hevc has $n for 5 IDR frames"; else bad "t5: t3.hevc has $n for 5 IDR frames"; fi
if run t5-forced --codec h264 --profile main --size 1280x720 --fps 30 --frames 150 --rc cbr --bitrate 8000000 \
        --idr 60 --force-idr 45 --headers 25 --out t5.h264; then
    check_stream t5-forced t5.h264 150 1280 720 Main yuv420p 60 45
    n=$(count_nal t5.h264 'Sequence Parameter Set')
    # IDRs at 0, 45, 105 and headers 25 frames after each insertion: 0 25 45 70 95 105 130
    if [ "$n" = 7 ]; then pass "t5-forced: 7 SPS (IDR at 0/45/105 plus every 25 frames)"; else bad "t5-forced: $n SPS, expected 7"; fi
else bad "t5-forced: encode failed (see logs/t5-forced.log)"; fi

# 6. Two threads, 600 frames: same bytes as one thread (the encoder is deterministic), frames in order.
if run t6-1thread --codec h264 --profile high --size 1920x1080 --fps 30 --frames 600 --rc cbr \
        --bitrate 12000000 --idr 60 --threads 1 --out t6-1.h264 \
   && run t6-2threads --codec h264 --profile high --size 1920x1080 --fps 30 --frames 600 --rc cbr \
        --bitrate 12000000 --idr 60 --threads 2 --out t6-2.h264 --raw-out src-t6.nv12; then
    check_stream t6-2threads t6-2.h264 600 1920 1080 High yuv420p 60
    if cmp -s t6-1.h264 t6-2.h264; then pass "t6: two-thread output is byte-identical to one-thread output"
    else bad "t6: two-thread output differs from one-thread output"; fi
    check_psnr t6-2threads t6-2.h264 src-t6.nv12 nv12 1920x1080 40
else bad "t6: encode failed"; fi
rm -f src-t6.nv12

# 7. Sizes that need cropping or odd macroblock counts.
for spec in "h264 high 1280x720 1:1 High" "h264 high 1440x1080 4:3 High" "h264 main 720x480 10:11 Main" \
            "h264 high 1366x768 1:1 High" "hevc main 1280x720 1:1 Main" "hevc main 1440x1080 4:3 Main" \
            "hevc main 720x480 10:11 Main"; do
    set -- $spec
    codec=$1 prof=$2 size=$3 sar=$4 pname=$5
    w=${size%x*} h=${size#*x}
    ext=$codec; [ $codec = hevc ] && ext=hevc
    name=t7-$codec-$size
    if run $name --codec $codec --profile $prof --size $size --fps 30000/1001 --frames 90 --rc cbr \
            --bitrate 6000000 --idr 30 --sar $sar --out $name.$ext; then
        check_stream $name $name.$ext 90 $w $h "$pname" yuv420p 30
        got=$(probe $name.$ext sample_aspect_ratio)
        if [ "$got" = "$sar" ]; then pass "$name: sample aspect ratio $got"; else bad "$name: sample aspect ratio $got, expected $sar"; fi
    else bad "$name: encode failed"; fi
done

# Extras: other H.264 profiles, padded pitch, HDR color tags with full range, the separate-queue
# upload (RADV's path), and the frame rate in the VUI.
if run x-baseline --codec h264 --profile baseline --size 1280x720 --fps 25 --frames 50 --rc vbr --bitrate 4000000 \
        --peak 6000000 --idr 25 --pitch-pad 192 --out x-baseline.h264; then
    check_stream x-baseline x-baseline.h264 50 1280 720 "Constrained Baseline" yuv420p 25
else bad "x-baseline: encode failed"; fi
if run x-color --codec hevc --profile main10 --size 1920x1080 --fps 24 --frames 24 --rc cbr --bitrate 10000000 \
        --idr 24 --color 9,16,9,1 --out x-color.hevc; then
    c="$(probe x-color.hevc color_primaries)/$(probe x-color.hevc color_transfer)/$(probe x-color.hevc color_space)/$(probe x-color.hevc color_range)"
    if [ "$c" = "bt2020/smpte2084/bt2020nc/pc" ]; then pass "x-color: VUI color $c"; else bad "x-color: VUI color $c"; fi
    fr=$(probe x-color.hevc r_frame_rate)
    if [ "$fr" = "24/1" ]; then pass "x-color: frame rate $fr"; else bad "x-color: frame rate $fr"; fi
else bad "x-color: encode failed"; fi
export NEUTRON_VKENC_UPLOAD=queue
if run x-queue --codec h264 --profile high --size 1920x1080 --fps 30 --frames 300 \
        --rc cbr --bitrate 20000000 --idr 60 --out x-queue.h264; then
    grep -q "queue-copy upload" logs/x-queue.log || bad "x-queue: did not use the separate upload queue"
    if cmp -s t1.h264 x-queue.h264; then pass "x-queue: separate-queue upload output is byte-identical to t1"
    else bad "x-queue: separate-queue upload output differs from t1"; fi
else bad "x-queue: encode failed"; fi
unset NEUTRON_VKENC_UPLOAD
rm -f src-1080p.nv12

echo
echo "==== summary ($work) ===="
printf '%s\n' "${results[@]}"
exit $fail
