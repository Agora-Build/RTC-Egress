#define AG_LOG_TAG "EncodedFrameDecoder"

#include "include/encoded_frame_decoder.h"

#include "common/log.h"

namespace agora {
namespace rtc {

// --- UserDecoder ---

EncodedFrameDecoder::UserDecoder::~UserDecoder() {
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (decodedFrame) {
        av_frame_free(&decodedFrame);
    }
    if (codecCtx) {
        avcodec_free_context(&codecCtx);
    }
}

bool EncodedFrameDecoder::UserDecoder::initialize() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        AG_LOG(ERROR, "H264 decoder not found");
        return false;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        AG_LOG(ERROR, "Failed to allocate decoder context");
        return false;
    }

    // Allow multithreaded decoding
    codecCtx->thread_count = 1;
    codecCtx->thread_type = FF_THREAD_SLICE;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        AG_LOG(ERROR, "Failed to open H264 decoder");
        avcodec_free_context(&codecCtx);
        return false;
    }

    decodedFrame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!decodedFrame || !pkt) {
        AG_LOG(ERROR, "Failed to allocate frame/packet");
        return false;
    }

    return true;
}

// --- EncodedFrameDecoder ---

EncodedFrameDecoder::EncodedFrameDecoder(VideoFrameCallback callback)
    : callback_(std::move(callback)) {}

EncodedFrameDecoder::~EncodedFrameDecoder() {
    std::lock_guard<std::mutex> lock(decoderMutex_);
    decoders_.clear();
}

bool EncodedFrameDecoder::onEncodedVideoFrameReceived(
    agora::rtc::uid_t uid, const uint8_t* imageBuffer, size_t length,
    const agora::rtc::EncodedVideoFrameInfo& videoEncodedFrameInfo) {
    if (!imageBuffer || length == 0 || !callback_) {
        return true;
    }

    // Only handle H264
    if (videoEncodedFrameInfo.codecType != VIDEO_CODEC_H264) {
        static int unsupported_count = 0;
        if (unsupported_count++ % 100 == 0) {
            AG_LOG(WARN, "Unsupported codec type: %d from uid %u", videoEncodedFrameInfo.codecType,
                   uid);
        }
        return true;
    }

    std::lock_guard<std::mutex> lock(decoderMutex_);

    UserDecoder* decoder = getOrCreateDecoder(uid);
    if (!decoder) {
        return true;
    }

    decodeAndForward(uid, decoder, imageBuffer, length, videoEncodedFrameInfo);
    return true;
}

EncodedFrameDecoder::UserDecoder* EncodedFrameDecoder::getOrCreateDecoder(agora::rtc::uid_t uid) {
    auto it = decoders_.find(uid);
    if (it != decoders_.end()) {
        return it->second.get();
    }

    AG_LOG(INFO, "Creating H264 decoder for user %u", uid);
    auto decoder = std::make_unique<UserDecoder>();
    if (!decoder->initialize()) {
        AG_LOG(ERROR, "Failed to initialize decoder for user %u", uid);
        return nullptr;
    }

    auto* ptr = decoder.get();
    decoders_[uid] = std::move(decoder);
    return ptr;
}

void EncodedFrameDecoder::decodeAndForward(agora::rtc::uid_t uid, UserDecoder* decoder,
                                           const uint8_t* data, size_t length,
                                           const agora::rtc::EncodedVideoFrameInfo& info) {
    // Feed encoded data to decoder
    decoder->pkt->data = const_cast<uint8_t*>(data);
    decoder->pkt->size = static_cast<int>(length);

    int ret = avcodec_send_packet(decoder->codecCtx, decoder->pkt);
    if (ret < 0) {
        static int send_err_count = 0;
        if (send_err_count++ % 100 == 0) {
            AG_LOG(WARN, "avcodec_send_packet failed for uid %u: %d (count: %d)", uid, ret,
                   send_err_count);
        }
        return;
    }

    // Receive all decoded frames
    while (ret >= 0) {
        ret = avcodec_receive_frame(decoder->codecCtx, decoder->decodedFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            AG_LOG(WARN, "avcodec_receive_frame failed for uid %u: %d", uid, ret);
            break;
        }

        AVFrame* f = decoder->decodedFrame;

        // Build VideoFrame from decoded YUV420P data
        agora::media::base::VideoFrame videoFrame;
        videoFrame.type = agora::media::base::VIDEO_PIXEL_I420;
        videoFrame.width = f->width;
        videoFrame.height = f->height;
        videoFrame.yStride = f->linesize[0];
        videoFrame.uStride = f->linesize[1];
        videoFrame.vStride = f->linesize[2];
        videoFrame.yBuffer = f->data[0];
        videoFrame.uBuffer = f->data[1];
        videoFrame.vBuffer = f->data[2];
        videoFrame.rotation = 0;
        // Use current system time for renderTimeMs. The SDK's captureTimeMs from encoded
        // frames is an internal RTC timestamp, not epoch milliseconds, which would cause
        // incorrect PTS in the recording container.
        videoFrame.renderTimeMs =
            static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());

        std::string userId = std::to_string(uid);
        callback_(videoFrame, userId);

        av_frame_unref(decoder->decodedFrame);
    }
}

}  // namespace rtc
}  // namespace agora
