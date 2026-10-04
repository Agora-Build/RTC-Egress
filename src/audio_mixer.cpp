#include "audio_mixer.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace agora::rtc {

AudioMixer::AudioMixer(int rate, int channels) : sampleRate_(rate), channels_(channels) {}

void AudioMixer::configure(int rate, int channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    sampleRate_ = rate;
    channels_ = channels;
    inputs_.clear();
    nextSample_ = -1;
}

void AudioMixer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    inputs_.clear();
    nextSample_ = -1;
}

bool AudioMixer::empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputs_.empty();
}

bool AudioMixer::push(const AudioFrame& frame, const std::string& userId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame.valid || frame.channels < 1 || frame.sampleRate < 1 || frame.data.empty())
        return false;
    auto& input = inputs_[userId];
    const auto now = std::chrono::steady_clock::now();
    const int channels = channels_;
    const int rate = sampleRate_;
    const int count = frame.data.size() / sizeof(int16_t) / frame.channels;
    const auto* data = reinterpret_cast<const int16_t*>(frame.data.data());
    std::vector<int16_t> converted;

    if (frame.sampleRate != rate || frame.channels != channels) {
        if (!input.resampler || input.sampleRate != frame.sampleRate ||
            input.channels != frame.channels) {
            input.resampler.reset();
            SwrContext* resampler = nullptr;
            AVChannelLayout source, destination;
            av_channel_layout_default(&source, frame.channels);
            av_channel_layout_default(&destination, channels);
            int ret = swr_alloc_set_opts2(&resampler, &destination, AV_SAMPLE_FMT_S16, rate,
                                          &source, AV_SAMPLE_FMT_S16, frame.sampleRate, 0, nullptr);
            av_channel_layout_uninit(&source);
            av_channel_layout_uninit(&destination);
            input.resampler.reset(resampler);
            if (ret < 0 || !resampler || swr_init(resampler) < 0) return false;
            input.sampleRate = frame.sampleRate;
            input.channels = frame.channels;
        }
        int capacity = swr_get_out_samples(input.resampler.get(), count);
        if (capacity < 0) return false;
        converted.resize(static_cast<size_t>(capacity) * channels);
        const uint8_t* source[] = {frame.data.data()};
        uint8_t* destination[] = {reinterpret_cast<uint8_t*>(converted.data())};
        int produced = swr_convert(input.resampler.get(), destination, capacity, source, count);
        if (produced < 0) return false;
        converted.resize(static_cast<size_t>(produced) * channels);
    } else {
        input.resampler.reset();
        converted.assign(data, data + count * channels);
    }

    int64_t timestampSample = av_rescale_q(frame.timestamp, {1, 1000}, {1, rate});
    if (nextSample_ < 0) {
        nextSample_ = timestampSample;
        start_ = now;
    }
    int64_t end = input.firstSample + input.samples.size() / channels;
    // Preserve PCM continuity across callback jitter; re-anchor a publisher after a real gap.
    if (input.firstSample < 0 || timestampSample > end + rate / 10) {
        input.samples.clear();
        input.firstSample = timestampSample;
    }
    input.lastArrival = now;
    input.samples.insert(input.samples.end(), converted.begin(), converted.end());

    // Bound per-user storage to half a second, even if a producer outruns the encoder.
    size_t limit = static_cast<size_t>(rate / 2) * channels;
    while (input.samples.size() > limit) {
        for (int ch = 0; ch < channels; ++ch) input.samples.pop_front();
        ++input.firstSample;
    }
    return true;
}

bool AudioMixer::drain(int count, bool flush,
                       const std::function<bool(const AudioFrame&)>& consume) {
    std::unique_lock<std::mutex> mixLock(mutex_);
    if (nextSample_ < 0 || inputs_.empty()) return true;
    const auto now = std::chrono::steady_clock::now();
    const auto wait = std::chrono::milliseconds(WAIT_MS);
    if (!flush && now - start_ < wait) return true;
    const int channels = channels_;
    const int rate = sampleRate_;
    if (count <= 0) return false;

    // Drain only the current backlog so a continuous publisher cannot starve video output.
    int64_t drainEnd = nextSample_;
    for (const auto& pair : inputs_) {
        const auto& input = pair.second;
        if (!input.samples.empty())
            drainEnd =
                std::max(drainEnd,
                         input.firstSample + static_cast<int64_t>(input.samples.size() / channels));
    }
    while (nextSample_ < drainEnd) {
        int64_t latest = nextSample_;
        int64_t earliest = INT64_MAX;
        for (auto& pair : inputs_) {
            auto& input = pair.second;
            while (!input.samples.empty() && input.firstSample < nextSample_) {
                for (int ch = 0; ch < channels; ++ch) input.samples.pop_front();
                ++input.firstSample;
            }
            if (!input.samples.empty()) {
                earliest = std::min(earliest, input.firstSample);
                latest = std::max(latest, input.firstSample + static_cast<int64_t>(
                                                                  input.samples.size() / channels));
            }
        }
        if (latest <= nextSample_) break;
        if (earliest >= drainEnd) break;
        // Leave a timestamp gap when every publisher has been silent for a long time.
        if (earliest > nextSample_ + rate) nextSample_ = earliest;
        int64_t end = nextSample_ + count;
        if (!flush && latest < end) break;
        if (!flush) {
            for (const auto& pair : inputs_) {
                const auto& input = pair.second;
                int64_t inputEnd = input.firstSample + input.samples.size() / channels;
                if (input.firstSample < end && inputEnd < end && now - input.lastArrival < wait)
                    return true;
            }
        }

        std::vector<int32_t> mixed(count * channels, 0);
        for (auto& pair : inputs_) {
            auto& input = pair.second;
            while (!input.samples.empty() && input.firstSample < end) {
                size_t offset = (input.firstSample - nextSample_) * channels;
                for (int ch = 0; ch < channels; ++ch) {
                    mixed[offset + ch] += input.samples.front();
                    input.samples.pop_front();
                }
                ++input.firstSample;
            }
        }
        int32_t peak = 32767;
        for (int32_t sample : mixed) peak = std::max(peak, std::abs(sample));
        std::vector<int16_t> pcm(mixed.size());
        for (size_t i = 0; i < mixed.size(); ++i)
            pcm[i] = static_cast<int16_t>(static_cast<int64_t>(mixed[i]) * 32767 / peak);

        AudioFrame frame;
        frame.data.resize(pcm.size() * sizeof(int16_t));
        std::memcpy(frame.data.data(), pcm.data(), frame.data.size());
        frame.sampleRate = rate;
        frame.channels = channels;
        frame.timestamp = av_rescale_q(nextSample_, {1, rate}, {1, 1000});
        frame.valid = true;
        nextSample_ = end;
        // Encoding or network writes must not block the RTC audio callback's bounded input buffer.
        mixLock.unlock();
        bool consumed = consume(frame);
        mixLock.lock();
        if (!consumed) return false;
    }

    for (auto it = inputs_.begin(); it != inputs_.end();) {
        if (it->second.samples.empty() && now - it->second.lastArrival > std::chrono::seconds(1))
            it = inputs_.erase(it);
        else
            ++it;
    }
    return true;
}

}  // namespace agora::rtc
