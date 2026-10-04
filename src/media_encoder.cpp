#define AG_LOG_TAG "MediaEncoder"
#include "media_encoder.h"

#include <vector>

#include "common/log.h"

namespace agora::rtc {

bool MediaEncoder::openVideo(AVCodecContext** videoCodecContext, const VideoConfig& config) {
    // Try hardware-accelerated encoders first, then fall back to software
    struct EncoderCandidate {
        const char* name;
        AVPixelFormat pixFmt;
        bool isHwAccel;
    };

    std::vector<EncoderCandidate> candidates;
    candidates.reserve(4);

    if (config.codec == "libx264" || config.codec == "h264") {
        candidates.push_back({"h264_nvenc", AV_PIX_FMT_YUV420P, true});
        candidates.push_back({"h264_vaapi", AV_PIX_FMT_VAAPI, true});
        candidates.push_back({"h264_qsv", AV_PIX_FMT_NV12, true});
        candidates.push_back({"libx264", AV_PIX_FMT_YUV420P, false});
    } else {
        // Non-H264 codec: use as-is without hwaccel probing
        candidates.push_back({config.codec.c_str(), AV_PIX_FMT_YUV420P, false});
    }

    const AVCodec* codec = nullptr;
    AVPixelFormat selectedPixFmt = AV_PIX_FMT_YUV420P;
    bool usingHwAccel = false;

    for (const auto& candidate : candidates) {
        if (candidate.isHwAccel && !config.allowHardware) continue;
        codec = avcodec_find_encoder_by_name(candidate.name);
        if (!codec) continue;

        // For hardware encoders, do a quick open test to verify the device is available
        if (candidate.isHwAccel) {
            AVCodecContext* testCtx = avcodec_alloc_context3(codec);
            if (!testCtx) {
                codec = nullptr;
                continue;
            }
            testCtx->width = config.width;
            testCtx->height = config.height;
            testCtx->time_base = {1, 90000};
            testCtx->pix_fmt = candidate.pixFmt;
            testCtx->bit_rate = config.bitrate;

            AVDictionary* testOpts = nullptr;
            av_dict_set(&testOpts, "preset", "fast", 0);
            int ret = avcodec_open2(testCtx, codec, &testOpts);
            av_dict_free(&testOpts);
            avcodec_free_context(&testCtx);

            if (ret < 0) {
                AG_LOG_FAST(INFO, "HW encoder %s not available, trying next", candidate.name);
                codec = nullptr;
                continue;
            }
        }

        selectedPixFmt = candidate.pixFmt;
        usingHwAccel = candidate.isHwAccel;
        AG_LOG_FAST(INFO, "Selected video encoder: %s%s", candidate.name,
                    usingHwAccel ? " (hardware accelerated)" : " (software)");
        break;
    }

    if (!codec) {
        AG_LOG_FAST(ERROR, "No suitable video encoder found");
        return false;
    }

    *videoCodecContext = avcodec_alloc_context3(codec);
    if (!*videoCodecContext) {
        AG_LOG_FAST(ERROR, "Failed to allocate video codec context");
        return false;
    }

    (*videoCodecContext)->bit_rate = config.bitrate;
    (*videoCodecContext)->width = config.width;
    (*videoCodecContext)->height = config.height;
    (*videoCodecContext)->time_base = {1, 90000};
    (*videoCodecContext)->framerate = {config.fps, 1};
    (*videoCodecContext)->gop_size = config.fps;
    (*videoCodecContext)->max_b_frames = 0;
    (*videoCodecContext)->pix_fmt = selectedPixFmt;
    (*videoCodecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    (*videoCodecContext)->thread_count = 1;
    (*videoCodecContext)->thread_type = FF_THREAD_SLICE;

    AVDictionary* opts = nullptr;
    if (usingHwAccel) {
        av_dict_set(&opts, "preset", "fast", 0);
    } else {
        av_dict_set(&opts, "preset", "fast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "x264-params", "force-cfr=1", 0);
    }
    av_dict_set(&opts, "fflags", "+flush_packets", 0);
    if (config.baseline) av_dict_set(&opts, "profile", "baseline", 0);

    if (avcodec_open2(*videoCodecContext, codec, &opts) < 0) {
        AG_LOG_FAST(ERROR, "Failed to open video codec: %s", codec->name);
        av_dict_free(&opts);
        avcodec_free_context(videoCodecContext);
        return false;
    }

    av_dict_free(&opts);
    return true;
}

bool MediaEncoder::openAudio(AVCodecContext** audioCodecContext, const AudioConfig& config) {
    const AVCodec* codec = avcodec_find_encoder_by_name(config.codec.c_str());
    if (!codec) {
        AG_LOG_FAST(ERROR, "Audio codec not found: %s", config.codec.c_str());
        return false;
    }

    *audioCodecContext = avcodec_alloc_context3(codec);
    if (!*audioCodecContext) {
        return false;
    }

    (*audioCodecContext)->bit_rate = config.bitrate;
    (*audioCodecContext)->sample_fmt = config.sampleFormat;
    // Use the target audio parameters from config (48kHz stereo)
    (*audioCodecContext)->sample_rate = config.sampleRate;
    av_channel_layout_default(&(*audioCodecContext)->ch_layout, config.channels);
    (*audioCodecContext)->time_base = config.timeBase;
    (*audioCodecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(*audioCodecContext, codec, nullptr) < 0) {
        avcodec_free_context(audioCodecContext);
        return false;
    }

    return true;
}

bool MediaEncoder::encode(AVCodecContext* context, const AVFrame* frame,
                          const std::function<bool(AVPacket*)>& consume) {
    if (avcodec_send_frame(context, frame) < 0) return false;
    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;
    bool ok = true;
    while (true) {
        int result = avcodec_receive_packet(context, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0 || !consume(packet)) {
            ok = false;
            break;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return ok;
}

}  // namespace agora::rtc
