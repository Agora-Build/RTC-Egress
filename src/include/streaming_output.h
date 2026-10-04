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
    // The optional stop flag must outlive the session; deadlines use av_gettime_relative().
    bool open(const Config& config, const AVCodecContext* video, const AVCodecContext* audio,
              const std::atomic<bool>* stopping = nullptr, int64_t taskDeadlineUs = 0);
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
    const std::atomic<bool>* stopping_ = nullptr;
    int64_t taskDeadlineUs_ = 0;
    bool closing_ = false;
};

}  // namespace agora::rtc
