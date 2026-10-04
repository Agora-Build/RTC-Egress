#define AG_LOG_TAG "StreamingSink"
#include "streaming_sink.h"

#include <algorithm>
#include <cstring>
#include <system_error>

#include "common/log.h"

extern "C" {
#include <libavutil/time.h>
}

namespace agora::rtc {

StreamingSink::~StreamingSink() {
    stop();
}

void StreamingSink::setCompletionCallback(CompletionCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

bool StreamingSink::initialize(const Config& config) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (active_ || thread_.joinable()) return false;
    initialized_ = false;
    if (!StreamingOutput::validate(config.output) || config.width < 2 || config.height < 2 ||
        config.width > 8192 || config.height > 8192 || config.width % 2 || config.height % 2 ||
        config.fps < 1 || config.fps > 60 || config.maxDurationSeconds < 1 ||
        config.reconnectAttempts < 0 || config.reconnectAttempts > 20 ||
        config.reconnectDelayMs < 100 || config.reconnectDelayMs > 10000)
        return false;
    config_ = config;
    VideoCompositor::Config layout;
    layout.outputWidth = config.width;
    layout.outputHeight = config.height;
    layout.layout = config.layout;
    layout.regions = config.regions;
    layout.userOrder = config.targetUsers;
    layout.minCompositeIntervalMs = 1000 / config.fps;
    if (!compositor_.initialize(layout)) return false;
    compositor_.setComposedVideoFrameCallback([this](const AVFrame* frame) {
        if (!isStreaming()) return;
        FramePtr copy(av_frame_clone(frame));
        if (!copy) {
            failed_ = true;
            cv_.notify_all();
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pendingVideo_ = std::move(copy);
        cv_.notify_all();
    });
    mixer_.configure(48000, 2);
    failed_ = false;
    initialized_ = true;
    return true;
}

void StreamingSink::releaseCodecs() {
    avcodec_free_context(&video_);
    avcodec_free_context(&audio_);
}

bool StreamingSink::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    if (!initialized_ || active_ || thread_.joinable()) return false;
    stopping_ = false;
    failed_ = false;
    reconnecting_ = false;
    failureMessage_ = "Streaming media processing failed";
    taskDeadline_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(config_.maxDurationSeconds);
    taskDeadlineUs_ =
        av_gettime_relative() + static_cast<int64_t>(config_.maxDurationSeconds) * 1000000;
    active_ = true;
    try {
        thread_ = std::thread(&StreamingSink::run, this);
    } catch (const std::system_error& error) {
        active_ = false;
        failed_ = true;
        AG_LOG(ERROR, "Cannot start publishing thread: %s", error.what());
        return false;
    }
    return true;
}

bool StreamingSink::openSession() {
    outputFailed_ = false;
    if (stopping_ || durationReached()) return false;
    hasOrigin_ = false;
    lastVideoPts_ = -1;
    nextAudioPts_ = AV_NOPTS_VALUE;
    MediaEncoder::VideoConfig video;
    video.width = config_.width;
    video.height = config_.height;
    video.fps = config_.fps;
    video.bitrate = config_.videoBitrate;
    video.allowHardware = false;
    video.baseline = config_.output.protocol == StreamingOutput::Protocol::WHIP;
    MediaEncoder::AudioConfig audio;
    audio.bitrate = config_.audioBitrate;
    audio.timeBase = {1, 48000};
    if (config_.output.protocol == StreamingOutput::Protocol::WHIP) {
        audio.codec = "libopus";
        audio.sampleFormat = AV_SAMPLE_FMT_FLT;
    }
    if (!MediaEncoder::openVideo(&video_, video) || !MediaEncoder::openAudio(&audio_, audio))
        return false;
    if (!output_.open(config_.output, video_, audio_, &stopping_, taskDeadlineUs_)) {
        outputFailed_ = true;
        return false;
    }
    // Never replay audio accumulated while the destination was unavailable.
    mixer_.reset();
    return true;
}

bool StreamingSink::durationReached() const {
    return std::chrono::steady_clock::now() >= taskDeadline_;
}

bool StreamingSink::reconnect() {
    reconnecting_ = true;
    output_.close();
    releaseCodecs();
    int delayMs = config_.reconnectDelayMs;
    for (int attempt = 1; attempt <= config_.reconnectAttempts; ++attempt) {
        AG_LOG(WARN, "Streaming task %s reconnect attempt %d/%d in %d ms", config_.taskId.c_str(),
               attempt, config_.reconnectAttempts, delayMs);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock,
                           std::min(taskDeadline_, std::chrono::steady_clock::now() +
                                                       std::chrono::milliseconds(delayMs)),
                           [this] { return stopping_.load() || failed_.load(); });
        }
        if (stopping_ || failed_ || durationReached()) break;
        if (openSession()) {
            reconnecting_ = false;
            AG_LOG(INFO, "Streaming task %s reconnected on attempt %d", config_.taskId.c_str(),
                   attempt);
            return true;
        }
        output_.close();
        releaseCodecs();
        if (!outputFailed_) break;
        delayMs = std::min(delayMs * 2, 10000);
    }
    reconnecting_ = false;
    if (!stopping_ && !durationReached() && !failed_) {
        failureMessage_ = outputFailed_
                              ? "Streaming destination reconnect exhausted after " +
                                    std::to_string(config_.reconnectAttempts) + " attempts"
                              : "Streaming media encoder restart failed";
        failed_ = true;
    }
    return false;
}

void StreamingSink::stop() {
    stopping_ = true;
    output_.cancel();
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workerId_ == std::this_thread::get_id()) return;
    }
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    // Completion callbacks may request cleanup from the publishing thread itself.
    if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) return;
    if (thread_.joinable()) thread_.join();
    active_ = false;
    compositor_.cleanup();
    output_.close();
    releaseCodecs();
    mixer_.reset();
    std::lock_guard<std::mutex> lock(mutex_);
    pendingVideo_.reset();
    initialized_ = false;
}

bool StreamingSink::selected(const std::string& userId) const {
    return config_.targetUsers.empty() ||
           std::find(config_.targetUsers.begin(), config_.targetUsers.end(), userId) !=
               config_.targetUsers.end();
}

void StreamingSink::onVideoFrame(const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t ys,
                                 int32_t us, int32_t vs, uint32_t width, uint32_t height,
                                 uint64_t timestamp, const std::string& userId) {
    if (!isStreaming() || !selected(userId)) return;
    VideoFrame frame;
    if (frame.initializeFromYUV(y, u, v, ys, us, vs, width, height, timestamp, userId))
        compositor_.addUserFrame(frame, userId);
}

void StreamingSink::onAudioFrame(const uint8_t* data, int samples, int sampleRate, int channels,
                                 uint64_t timestamp, const std::string& userId) {
    if (!isStreaming() || !selected(userId) || !data || samples < 1 || samples > 48000 ||
        sampleRate < 8000 || sampleRate > 192000 || channels < 1 || channels > 8)
        return;
    AudioFrame frame;
    frame.data.assign(data, data + static_cast<size_t>(samples) * channels * sizeof(int16_t));
    frame.timestamp = timestamp;
    frame.sampleRate = sampleRate;
    frame.channels = channels;
    frame.valid = true;
    if (!mixer_.push(frame, userId)) failed_ = true;
    cv_.notify_all();
}

bool StreamingSink::encodeAudio(const AudioFrame& pcm) {
    if (!hasOrigin_) {
        originMs_ = pcm.timestamp;
        hasOrigin_ = true;
    }
    int64_t timestamp =
        av_rescale_q(static_cast<int64_t>(pcm.timestamp) - static_cast<int64_t>(originMs_),
                     {1, 1000}, audio_->time_base);
    if (nextAudioPts_ == AV_NOPTS_VALUE || timestamp > nextAudioPts_ + 4800)
        nextAudioPts_ = std::max<int64_t>(0, timestamp);
    FramePtr frame(av_frame_alloc());
    if (!frame) return false;
    frame->format = audio_->sample_fmt;
    frame->sample_rate = 48000;
    frame->nb_samples = audio_->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &audio_->ch_layout);
    frame->pts = nextAudioPts_;
    nextAudioPts_ += frame->nb_samples;
    if (av_frame_get_buffer(frame.get(), 0) < 0) return false;
    const auto* source = reinterpret_cast<const int16_t*>(pcm.data.data());
    for (int i = 0; i < frame->nb_samples; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            auto* out = reinterpret_cast<float*>(
                frame->data[audio_->sample_fmt == AV_SAMPLE_FMT_FLTP ? ch : 0]);
            out[audio_->sample_fmt == AV_SAMPLE_FMT_FLTP ? i : i * 2 + ch] =
                source[i * 2 + ch] / 32768.0f;
        }
    }
    return MediaEncoder::encode(audio_, frame.get(), [this](AVPacket* packet) {
        bool written = output_.write(packet, false, audio_->time_base);
        if (!written) outputFailed_ = true;
        return written;
    });
}

void StreamingSink::run() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerId_ = std::this_thread::get_id();
    }
    bool connected = openSession();
    if (!connected && outputFailed_ && !stopping_ && !durationReached()) connected = reconnect();
    if (!connected && !stopping_ && !durationReached()) failed_ = true;
    while (connected && !stopping_ && !failed_) {
        if (durationReached()) break;
        FramePtr frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10),
                         [this] { return stopping_.load() || failed_.load() || pendingVideo_; });
            frame = std::move(pendingVideo_);
        }
        if (stopping_ || failed_) break;
        bool mediaOk = true;
        if (frame) {
            uint64_t timestamp = frame->pts;
            if (!hasOrigin_) {
                originMs_ = timestamp;
                hasOrigin_ = true;
            }
            int64_t pts =
                av_rescale_q(static_cast<int64_t>(timestamp) - static_cast<int64_t>(originMs_),
                             {1, 1000}, video_->time_base);
            // FLV timestamps have millisecond precision; avoid quantizing RTC jitter to duplicate
            // DTS.
            int64_t increment = av_rescale_q(1, {1, 1000}, video_->time_base);
            frame->pts = std::max<int64_t>(lastVideoPts_ < 0 ? 0 : lastVideoPts_ + increment, pts);
            lastVideoPts_ = frame->pts;
            mediaOk = MediaEncoder::encode(video_, frame.get(), [this](AVPacket* packet) {
                bool written = output_.write(packet, true, video_->time_base);
                if (!written) outputFailed_ = true;
                return written;
            });
        }
        if (mediaOk && !failed_ && !stopping_)
            mediaOk = mixer_.drain(audio_->frame_size, false, [this](const AudioFrame& pcm) {
                return !stopping_.load() && encodeAudio(pcm);
            });
        if (mediaOk && !failed_ && !stopping_ && !output_.poll()) {
            outputFailed_ = true;
            mediaOk = false;
        }
        if (!mediaOk && !stopping_ && !durationReached()) {
            // Recover after AudioMixer::drain unwinds, before replacing codecs or clearing PCM.
            if (outputFailed_) {
                if (!reconnect()) break;
            } else {
                failed_ = true;
            }
        }
    }
    output_.close();
    releaseCodecs();
    active_ = false;
    CompletionCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
    }
    if (!stopping_ && callback && (failed_ || durationReached())) {
        callback(config_.taskId, failed_ ? "failed" : "success",
                 failed_ ? failureMessage_ : "Streaming duration reached");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerId_ = {};
    }
}

}  // namespace agora::rtc
