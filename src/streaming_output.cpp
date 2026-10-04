#define AG_LOG_TAG "StreamingOutput"
#include "streaming_output.h"

#include <cctype>

#include "common/log.h"

extern "C" {
#include <libavutil/time.h>
}

namespace agora::rtc {

StreamingOutput::~StreamingOutput() {
    close();
}

bool StreamingOutput::validate(const Config& config) {
    const auto& url = config.url;
    if (url.empty() || url.size() > 1024 || url.find('#') != std::string::npos ||
        config.token.size() > 512 || config.timeoutMs < 1000 || config.timeoutMs > 30000)
        return false;
    for (unsigned char ch : url)
        if (std::isspace(ch) || std::iscntrl(ch)) return false;
    for (unsigned char ch : config.token)
        if (std::isspace(ch) || std::iscntrl(ch)) return false;
    bool scheme = config.protocol == Protocol::RTMP
                      ? url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0
                      : url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    if (!scheme || (config.protocol == Protocol::RTMP && !config.token.empty())) return false;
    auto authorityStart = url.find("://") + 3;
    auto authorityEnd = url.find_first_of("/?", authorityStart);
    auto authority = url.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.empty() || authority.front() == ':' || authority.front() == '@') return false;
    return av_guess_format(config.protocol == Protocol::RTMP ? "flv" : "whip", nullptr, nullptr);
}

void StreamingOutput::setDeadline(int timeoutMs) {
    deadlineUs_ = av_gettime_relative() + static_cast<int64_t>(timeoutMs) * 1000;
}

int StreamingOutput::interrupt(void* opaque) {
    auto* output = static_cast<StreamingOutput*>(opaque);
    return output->cancelled_.load() || av_gettime_relative() >= output->deadlineUs_.load();
}

bool StreamingOutput::open(const Config& config, const AVCodecContext* video,
                           const AVCodecContext* audio) {
    close();
    if (!validate(config) || !video || !audio) return false;
    config_ = config;
    cancelled_ = false;
    setDeadline(config_.timeoutMs);
    const char* muxer = config.protocol == Protocol::RTMP ? "flv" : "whip";
    if (avformat_alloc_output_context2(&context_, nullptr, muxer, config.url.c_str()) < 0)
        return false;
    context_->interrupt_callback = {interrupt, this};
    context_->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    context_->max_interleave_delta = 100000;
    video_ = avformat_new_stream(context_, nullptr);
    audio_ = avformat_new_stream(context_, nullptr);
    if (!video_ || !audio_ || avcodec_parameters_from_context(video_->codecpar, video) < 0 ||
        avcodec_parameters_from_context(audio_->codecpar, audio) < 0) {
        close();
        return false;
    }
    video_->time_base = video->time_base;
    video_->avg_frame_rate = video->framerate;
    audio_->time_base = audio->time_base;
    AVDictionary* options = nullptr;
    av_dict_set_int(&options, "rw_timeout", static_cast<int64_t>(config.timeoutMs) * 1000, 0);
    av_dict_set(&options, "tls_verify", "1", 0);
    if (!(context_->oformat->flags & AVFMT_NOFILE)) {
        int result = avio_open2(&context_->pb, config.url.c_str(), AVIO_FLAG_WRITE,
                                &context_->interrupt_callback, &options);
        av_dict_free(&options);
        if (result < 0) {
            close();
            return false;
        }
    }
    if (config.protocol == Protocol::WHIP) {
        context_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        av_dict_set_int(&options, "handshake_timeout", config.timeoutMs, 0);
        if (!config.token.empty()) av_dict_set(&options, "authorization", config.token.c_str(), 0);
    }
    setDeadline(config.timeoutMs);
    int result = avformat_write_header(context_, &options);
    av_dict_free(&options);
    if (result < 0) {
        close();
        return false;
    }
    headerWritten_ = true;
    return true;
}

bool StreamingOutput::write(AVPacket* packet, bool video, AVRational timeBase) {
    if (!headerWritten_ || cancelled_) return false;
    auto* stream = video ? video_ : audio_;
    av_packet_rescale_ts(packet, timeBase, stream->time_base);
    packet->stream_index = stream->index;
    setDeadline(config_.timeoutMs);
    return av_interleaved_write_frame(context_, packet) >= 0;
}

void StreamingOutput::cancel() {
    cancelled_ = true;
}

void StreamingOutput::close() {
    if (!context_) return;
    // Allow a bounded final transport teardown, including WHIP's session DELETE.
    cancelled_ = false;
    setDeadline(1000);
    if (headerWritten_) av_write_trailer(context_);
    if (context_->pb && !(context_->oformat->flags & AVFMT_NOFILE)) avio_closep(&context_->pb);
    avformat_free_context(context_);
    context_ = nullptr;
    video_ = audio_ = nullptr;
    headerWritten_ = false;
}

}  // namespace agora::rtc
