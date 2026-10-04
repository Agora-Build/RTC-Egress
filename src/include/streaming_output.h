#pragma once

#include <atomic>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace agora::rtc {

// Owns publishing transport and muxing; never creates recording files or metadata.
class StreamingOutput {
   public:
    enum class Protocol { RTMP, WHIP };
    struct Config {
        Protocol protocol = Protocol::RTMP;
        std::string url;
        std::string token;
        int timeoutMs = 5000;
    };

    ~StreamingOutput();
    static bool validate(const Config& config);
    bool open(const Config& config, const AVCodecContext* video, const AVCodecContext* audio);
    bool write(AVPacket* packet, bool video, AVRational timeBase);
    void cancel();
    void close();

   private:
    static int interrupt(void* opaque);
    void setDeadline(int timeoutMs);
    Config config_;
    AVFormatContext* context_ = nullptr;
    AVStream* video_ = nullptr;
    AVStream* audio_ = nullptr;
    bool headerWritten_ = false;
    std::atomic<bool> cancelled_{false};
    std::atomic<int64_t> deadlineUs_{0};
};

}  // namespace agora::rtc
