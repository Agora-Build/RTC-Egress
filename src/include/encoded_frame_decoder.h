// encoded_frame_decoder.h
// Receives encoded H264 frames from Agora SDK, decodes them using ffmpeg,
// and forwards decoded YUV frames to the existing VideoFrameCallback.
// This bypasses the SDK's internal decoder which crashes with multiple streams.
#pragma once

#include <AgoraBase.h>
#include <AgoraMediaBase.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

namespace agora {
namespace rtc {

class EncodedFrameDecoder : public agora::media::IVideoEncodedFrameObserver {
   public:
    using VideoFrameCallback =
        std::function<void(const agora::media::base::VideoFrame&, const std::string& userId)>;

    explicit EncodedFrameDecoder(VideoFrameCallback callback);
    ~EncodedFrameDecoder() override;

    // IVideoEncodedFrameObserver interface
    bool onEncodedVideoFrameReceived(
        agora::rtc::uid_t uid, const uint8_t* imageBuffer, size_t length,
        const agora::rtc::EncodedVideoFrameInfo& videoEncodedFrameInfo) override;

   private:
    struct UserDecoder {
        AVCodecContext* codecCtx = nullptr;
        AVFrame* decodedFrame = nullptr;
        AVPacket* pkt = nullptr;

        ~UserDecoder();
        bool initialize();
    };

    VideoFrameCallback callback_;
    std::mutex decoderMutex_;
    std::map<agora::rtc::uid_t, std::unique_ptr<UserDecoder>> decoders_;

    UserDecoder* getOrCreateDecoder(agora::rtc::uid_t uid);
    void decodeAndForward(agora::rtc::uid_t uid, UserDecoder* decoder, const uint8_t* data,
                          size_t length, const agora::rtc::EncodedVideoFrameInfo& info);
};

}  // namespace rtc
}  // namespace agora
