#!/usr/bin/env bash
set -euo pipefail

prefix=${1:?Usage: build-ffmpeg-whip.sh INSTALL_PREFIX}
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
version=8.0.1
checksum=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/rtc-egress-ffmpeg.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT

pkg-config --exists x264 opus openssl
command -v nasm >/dev/null
archive="$build_dir/ffmpeg.tar.xz"
if [[ -n ${RTC_EGRESS_FFMPEG_ARCHIVE:-} ]]; then
    cp "$RTC_EGRESS_FFMPEG_ARCHIVE" "$archive"
else
    curl --fail --location --silent --show-error \
        "https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz" --output "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
tar -xf "$archive" -C "$build_dir"
cd "$build_dir/ffmpeg-${version}"
patch --batch --fuzz=0 -p1 < "$script_dir/patches/ffmpeg-8-dtls-certificates.patch"
patch --batch --fuzz=0 -p1 < "$script_dir/patches/ffmpeg-8-whip-https-verification.patch"

./configure --prefix="$prefix" --enable-shared --disable-static --disable-programs \
    --disable-doc --disable-autodetect --disable-avdevice --disable-avfilter \
    --disable-everything --enable-gpl --enable-version3 --enable-libx264 \
    --enable-libopus --enable-openssl --enable-network \
    --enable-encoder=libx264,aac,libopus,mjpeg \
    --enable-decoder=h264,hevc,aac,opus,mjpeg \
    --enable-parser=h264,hevc,aac,opus,mjpeg \
    --enable-muxer=mp4,avi,matroska,mpegts,flv,whip,rtp \
    --enable-demuxer=mov,avi,matroska,mpegts,flv \
    --enable-protocol=file,pipe,tcp,udp,rtmp,rtmps,http,https,tls,dtls,rtp \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,aac_adtstoasc
make -j "${RTC_EGRESS_BUILD_JOBS:-4}"
make install
printf 'FFmpeg with WHIP installed in %s\n' "$prefix"
