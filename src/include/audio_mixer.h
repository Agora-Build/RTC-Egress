#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "audio_frame.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace agora::rtc {

// Aligns per-publisher PCM on the RTC timeline before summing and limiting peaks.
class AudioMixer {
   public:
    AudioMixer(int sampleRate = 48000, int channels = 2);
    void configure(int sampleRate, int channels);
    bool push(const AudioFrame& frame, const std::string& userId);
    bool drain(int sampleCount, bool flush, const std::function<bool(const AudioFrame&)>& consume);
    void reset();
    bool empty();

   private:
    struct ResamplerDeleter {
        void operator()(SwrContext* context) const {
            swr_free(&context);
        }
    };
    struct Input {
        std::deque<int16_t> samples;
        int64_t firstSample = -1;
        int sampleRate = 0;
        int channels = 0;
        std::unique_ptr<SwrContext, ResamplerDeleter> resampler;
        std::chrono::steady_clock::time_point lastArrival;
    };
    std::map<std::string, Input> inputs_;
    std::mutex mutex_;
    int sampleRate_;
    int channels_;
    int64_t nextSample_ = -1;
    std::chrono::steady_clock::time_point start_;
    static constexpr int WAIT_MS = 40;
};

}  // namespace agora::rtc
