#pragma once

#include <functional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace agora::rtc {

// Codec configuration and packet draining shared by file and streaming sinks.
class MediaEncoder {
   public:
    struct VideoConfig {
        std::string codec = "libx264";
        int width = 1280;
        int height = 720;
        int fps = 30;
        int bitrate = 2000000;
        bool allowHardware = true;
        bool baseline = false;
    };
    struct AudioConfig {
        std::string codec = "aac";
        int sampleRate = 48000;
        int channels = 2;
        int bitrate = 128000;
        AVSampleFormat sampleFormat = AV_SAMPLE_FMT_FLTP;
        AVRational timeBase = {1, 90000};
    };

    static bool openVideo(AVCodecContext** context, const VideoConfig& config);
    static bool openAudio(AVCodecContext** context, const AudioConfig& config);
    static bool encode(AVCodecContext* context, const AVFrame* frame,
                       const std::function<bool(AVPacket*)>& consume);
};

}  // namespace agora::rtc
