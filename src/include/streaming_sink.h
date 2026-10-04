#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_mixer.h"
#include "media_encoder.h"
#include "streaming_output.h"
#include "video_compositor.h"

namespace agora::rtc {

class StreamingSink {
   public:
    struct Config {
        StreamingOutput::Config output;
        int width = 1280;
        int height = 720;
        int fps = 30;
        int videoBitrate = 2000000;
        int audioBitrate = 128000;
        int maxDurationSeconds = 28800;
        int reconnectAttempts = 5;
        int reconnectDelayMs = 1000;
        std::string taskId;
        std::vector<std::string> targetUsers;
        std::string layout = "flat";
        std::vector<LayoutRegion> regions;
    };
    using CompletionCallback =
        std::function<void(const std::string&, const std::string&, const std::string&)>;

    ~StreamingSink();
    bool initialize(const Config& config);
    bool start();
    void stop();
    bool isStreaming() const {
        return active_.load() && !stopping_.load();
    }
    bool hasFailed() const {
        return failed_.load();
    }
    bool isReconnecting() const {
        return reconnecting_.load();
    }
    void setCompletionCallback(CompletionCallback callback);
    void onVideoFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t ys, int32_t us,
                      int32_t vs, uint32_t width, uint32_t height, uint64_t timestamp,
                      const std::string& userId);
    void onAudioFrame(const uint8_t* data, int samples, int sampleRate, int channels,
                      uint64_t timestamp, const std::string& userId);

   private:
    struct FrameDeleter {
        void operator()(AVFrame* frame) const {
            av_frame_free(&frame);
        }
    };
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    bool selected(const std::string& userId) const;
    bool encodeAudio(const AudioFrame& frame);
    bool openSession();
    bool reconnect();
    bool durationReached() const;
    void run();
    void releaseCodecs();
    Config config_;
    VideoCompositor compositor_;
    AudioMixer mixer_;
    StreamingOutput output_;
    AVCodecContext* video_ = nullptr;
    AVCodecContext* audio_ = nullptr;
    FramePtr pendingVideo_;
    std::mutex mutex_;
    std::mutex lifecycleMutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::thread::id workerId_;  // Protected by mutex_.
    CompletionCallback callback_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> reconnecting_{false};
    bool initialized_ = false;
    bool outputFailed_ = false;  // Publishing thread only.
    int64_t taskDeadlineUs_ = 0;
    std::chrono::steady_clock::time_point taskDeadline_;
    std::string failureMessage_;
    uint64_t originMs_ = 0;
    bool hasOrigin_ = false;
    int64_t lastVideoPts_ = -1;
    int64_t nextAudioPts_ = AV_NOPTS_VALUE;
};

}  // namespace agora::rtc
