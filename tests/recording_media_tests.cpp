#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "../src/include/recording_sink.h"

using namespace agora::rtc;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct MediaReader {
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwrContext* resampler = nullptr;
    int stream = -1;

    ~MediaReader() {
        swr_free(&resampler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&input);
    }

    bool open(const std::filesystem::path& path, AVMediaType type) {
        if (!packet || !frame || avformat_open_input(&input, path.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(input, nullptr) < 0)
            return false;
        stream = av_find_best_stream(input, type, -1, -1, nullptr, 0);
        if (stream < 0) return false;
        decoder = avcodec_alloc_context3(
            avcodec_find_decoder(input->streams[stream]->codecpar->codec_id));
        return decoder &&
               avcodec_parameters_to_context(decoder, input->streams[stream]->codecpar) >= 0 &&
               avcodec_open2(decoder, decoder->codec, nullptr) >= 0;
    }
};

double toneAmplitude(const std::vector<float>& samples, double frequency, double begin,
                     double end) {
    size_t first = static_cast<size_t>(begin * 48000);
    size_t last = std::min(samples.size(), static_cast<size_t>(end * 48000));
    if (last <= first) return 0;
    double real = 0, imaginary = 0;
    for (size_t i = first; i < last; ++i) {
        double phase = 2 * kPi * frequency * i / 48000;
        real += samples[i] * std::cos(phase);
        imaginary += samples[i] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / (last - first);
}
}  // namespace

class RecordingMediaTest : public ::testing::Test {
   protected:
    void SetUp() override {
        outputDir_ = std::filesystem::temp_directory_path() /
                     ("rtc_media_test_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(outputDir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(outputDir_);
    }

    RecordingSink::Config config() const {
        RecordingSink::Config result;
        result.outputDir = outputDir_.string();
        result.videoWidth = 160;
        result.videoHeight = 120;
        result.videoFps = 25;
        result.useRealTimeScheduling = false;
        return result;
    }

    std::filesystem::path recording() const {
        for (const auto& entry : std::filesystem::directory_iterator(outputDir_)) {
            if (entry.path().extension() == ".mp4") return entry.path();
        }
        return {};
    }

    void sendTone(RecordingSink& sink, const std::string& user, double frequency, int tick,
                  int rate = 48000, int channels = 2, uint64_t origin = 1000) {
        int count = rate / 100;
        std::vector<int16_t> samples(count * channels);
        for (int i = 0; i < count; ++i) {
            int16_t value = static_cast<int16_t>(
                6000 * std::sin(2 * kPi * frequency * (tick * count + i) / rate));
            for (int ch = 0; ch < channels; ++ch) samples[i * channels + ch] = value;
        }
        sink.onAudioFrame(reinterpret_cast<const uint8_t*>(samples.data()), count, rate, channels,
                          origin + tick * 10, user);
    }

    void recordAndCheckLayout(const std::string& layout) {
        auto settings = config();
        settings.layout = layout;
        settings.targetUsers = {"z_speaker", "a_guest"};
        settings.recordAudio = false;
        settings.taskId = "layout_media";
        if (layout == "customized")
            settings.regions = {{"z_speaker", 0, 0, 160, 120, 0}, {"a_guest", 80, 60, 80, 60, 1}};
        RecordingSink sink;
        ASSERT_TRUE(sink.initialize(settings));
        ASSERT_TRUE(sink.start());
        std::vector<uint8_t> y(160 * 120), uv(80 * 60, 128);
        const auto start = std::chrono::steady_clock::now();
        for (int tick = 0; tick < 18; ++tick) {
            for (const auto& user : settings.targetUsers) {
                std::fill(y.begin(), y.end(), user == "z_speaker" ? 180 : 80);
                sink.onVideoFrame(y.data(), uv.data(), uv.data(), 160, 80, 80, 160, 120,
                                  1000 + tick * 40, user);
            }
            std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 40));
        }
        sink.stop();
        MediaReader reader;
        ASSERT_TRUE(reader.open(recording(), AVMEDIA_TYPE_VIDEO));
        int matching = 0;
        auto receive = [&] {
            while (avcodec_receive_frame(reader.decoder, reader.frame) == 0) {
                int primary = reader.frame->data[0][40 * reader.frame->linesize[0] + 60];
                int x = layout == "spotlight" ? 140 : 120;
                int y = layout == "spotlight" ? 60 : 90;
                int guest = reader.frame->data[0][y * reader.frame->linesize[0] + x];
                if (std::abs(primary - 180) < 5 && std::abs(guest - 80) < 5) ++matching;
            }
        };
        while (av_read_frame(reader.input, reader.packet) >= 0) {
            if (reader.packet->stream_index == reader.stream) {
                ASSERT_GE(avcodec_send_packet(reader.decoder, reader.packet), 0);
                receive();
            }
            av_packet_unref(reader.packet);
        }
        ASSERT_GE(avcodec_send_packet(reader.decoder, nullptr), 0);
        receive();
        EXPECT_GE(matching, 10);
        nlohmann::json metadata;
        for (const auto& entry : std::filesystem::directory_iterator(outputDir_)) {
            if (entry.path().extension() == ".json") {
                std::ifstream input(entry.path());
                input >> metadata;
            }
        }
        EXPECT_EQ(metadata["layout"], layout);
        EXPECT_TRUE(metadata.value("sessionCompleted", false));
    }

    void decodeAudio(std::vector<float>& samples) {
        MediaReader reader;
        ASSERT_TRUE(reader.open(recording(), AVMEDIA_TYPE_AUDIO));
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        ASSERT_GE(swr_alloc_set_opts2(&reader.resampler, &mono, AV_SAMPLE_FMT_FLT, 48000,
                                      &reader.decoder->ch_layout, reader.decoder->sample_fmt,
                                      reader.decoder->sample_rate, 0, nullptr),
                  0);
        ASSERT_GE(swr_init(reader.resampler), 0);
        auto receive = [&] {
            int ret;
            while ((ret = avcodec_receive_frame(reader.decoder, reader.frame)) >= 0) {
                std::vector<float> converted(reader.frame->nb_samples + 64);
                uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
                int count = swr_convert(reader.resampler, output, converted.size(),
                                        const_cast<const uint8_t**>(reader.frame->extended_data),
                                        reader.frame->nb_samples);
                ASSERT_GE(count, 0);
                samples.insert(samples.end(), converted.begin(), converted.begin() + count);
                av_frame_unref(reader.frame);
            }
            EXPECT_TRUE(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
        };
        while (av_read_frame(reader.input, reader.packet) >= 0) {
            if (reader.packet->stream_index == reader.stream) {
                ASSERT_GE(avcodec_send_packet(reader.decoder, reader.packet), 0);
                receive();
            }
            av_packet_unref(reader.packet);
        }
        ASSERT_GE(avcodec_send_packet(reader.decoder, nullptr), 0);
        receive();
    }

    void sendEncodedKeyframe(RecordingSink& sink) {
        AVCodecContext* encoder = avcodec_alloc_context3(avcodec_find_encoder_by_name("libx264"));
        ASSERT_NE(encoder, nullptr);
        encoder->width = 160;
        encoder->height = 120;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = {1, 25};
        encoder->max_b_frames = 0;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "preset", "ultrafast", 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
        ASSERT_GE(avcodec_open2(encoder, encoder->codec, &options), 0);
        av_dict_free(&options);
        AVFrame* frame = av_frame_alloc();
        frame->width = 160;
        frame->height = 120;
        frame->format = AV_PIX_FMT_YUV420P;
        ASSERT_GE(av_frame_get_buffer(frame, 32), 0);
        for (int ch = 0; ch < 3; ++ch)
            for (int y = 0; y < (ch == 0 ? 120 : 60); ++y)
                std::fill_n(frame->data[ch] + y * frame->linesize[ch], ch == 0 ? 160 : 80, 128);
        ASSERT_GE(avcodec_send_frame(encoder, frame), 0);
        AVPacket* packet = av_packet_alloc();
        ASSERT_EQ(avcodec_receive_packet(encoder, packet), 0);
        EncodedVideoFrameInfo info;
        info.codecType = VIDEO_CODEC_H264;
        info.width = 160;
        info.height = 120;
        info.frameType = VIDEO_FRAME_TYPE_KEY_FRAME;
        sink.onEncodedVideoFrame(1001, packet->data, packet->size, info);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&encoder);
    }

    std::filesystem::path outputDir_;
};

TEST_F(RecordingMediaTest, CompositeSinglePublisherPreservesToneAndDuration) {
    auto settings = config();
    settings.recordVideo = false;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());
    auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 150; ++tick) {
        sendTone(sink, "1001", 440, tick);
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sink.stop();
    std::vector<float> samples;
    decodeAudio(samples);
    EXPECT_NEAR(samples.size() / 48000.0, 1.5, 0.06);
    EXPECT_GT(toneAmplitude(samples, 440, 0.25, 1.25), 0.15);
}

TEST_F(RecordingMediaTest, CompositeTwoPublishersPreserveBothTonesAndDuration) {
    auto settings = config();
    settings.recordVideo = false;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());
    auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 150; ++tick) {
        // Swap arrival order to exercise independent SDK callbacks.
        if (tick % 2) sendTone(sink, "1002", 880, tick);
        sendTone(sink, "1001", 440, tick);
        if (!(tick % 2)) sendTone(sink, "1002", 880, tick);
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sink.stop();
    std::vector<float> samples;
    decodeAudio(samples);
    EXPECT_NEAR(samples.size() / 48000.0, 1.5, 0.06);
    EXPECT_GT(toneAmplitude(samples, 440, 0.25, 1.25), 0.15);
    EXPECT_GT(toneAmplitude(samples, 880, 0.25, 1.25), 0.15);
}

TEST_F(RecordingMediaTest, CompositeLatePublisherResamplesAndStopsWithoutStallingOtherUser) {
    auto settings = config();
    settings.recordVideo = false;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());
    auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 180; ++tick) {
        sendTone(sink, "1001", 440, tick);
        if (tick >= 40 && tick < 120) sendTone(sink, "1002", 880, tick, 24000, 1);
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sink.stop();
    std::vector<float> samples;
    decodeAudio(samples);
    EXPECT_NEAR(samples.size() / 48000.0, 1.8, 0.06);
    EXPECT_GT(toneAmplitude(samples, 440, 0.65, 1.05), 0.15);
    EXPECT_GT(toneAmplitude(samples, 880, 0.65, 1.05), 0.15);
    EXPECT_GT(toneAmplitude(samples, 440, 1.4, 1.7), 0.15);
    EXPECT_LT(toneAmplitude(samples, 880, 1.4, 1.7), 0.01);
}

TEST_F(RecordingMediaTest, StopDrainsAcceptedAudioAndFlushesPartialMix) {
    auto settings = config();
    settings.recordVideo = false;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());
    for (int tick = 0; tick < 10; ++tick) {
        sendTone(sink, "1001", 440, tick);
        sendTone(sink, "1002", 880, tick);
    }
    sink.stop();
    std::vector<float> samples;
    decodeAudio(samples);
    EXPECT_NEAR(samples.size() / 48000.0, 0.1, 0.025);
    EXPECT_GT(toneAmplitude(samples, 440, 0.02, 0.08), 0.14);
    EXPECT_GT(toneAmplitude(samples, 880, 0.02, 0.08), 0.14);
}

TEST_F(RecordingMediaTest, PassthroughFinalizesRealH264AndFileMetadata) {
    auto settings = config();
    settings.mode = VideoCompositor::Mode::Individual;
    settings.videoDecodeMode = 0;
    settings.videoWidth = 320;  // Metadata must use the source dimensions.
    settings.videoHeight = 240;
    settings.taskId = "passthrough_media";
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());

    AVCodecContext* encoder = avcodec_alloc_context3(avcodec_find_encoder_by_name("libx264"));
    ASSERT_NE(encoder, nullptr);
    encoder->width = 160;
    encoder->height = 120;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = {1, 25};
    encoder->max_b_frames = 0;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "preset", "ultrafast", 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
    ASSERT_GE(avcodec_open2(encoder, encoder->codec, &options), 0);
    av_dict_free(&options);
    AVFrame* frame = av_frame_alloc();
    frame->width = 160;
    frame->height = 120;
    frame->format = AV_PIX_FMT_YUV420P;
    ASSERT_GE(av_frame_get_buffer(frame, 32), 0);
    AVPacket* packet = av_packet_alloc();
    uint64_t audioOrigin = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    for (int i = 0; i < 15; ++i) {
        ASSERT_GE(av_frame_make_writable(frame), 0);
        for (int y = 0; y < 120; ++y)
            std::fill_n(frame->data[0] + y * frame->linesize[0], 160, 70 + i);
        for (int ch = 1; ch < 3; ++ch)
            for (int y = 0; y < 60; ++y)
                std::fill_n(frame->data[ch] + y * frame->linesize[ch], 80, 128);
        frame->pts = i;
        ASSERT_GE(avcodec_send_frame(encoder, frame), 0);
        while (avcodec_receive_packet(encoder, packet) == 0) {
            EncodedVideoFrameInfo info;
            info.codecType = VIDEO_CODEC_H264;
            info.width = 160;
            info.height = 120;
            info.frameType = packet->flags & AV_PKT_FLAG_KEY ? VIDEO_FRAME_TYPE_KEY_FRAME
                                                             : VIDEO_FRAME_TYPE_DELTA_FRAME;
            sink.onEncodedVideoFrame(1001, packet->data, packet->size, info);
            av_packet_unref(packet);
        }
        for (int tick = i * 4; tick < (i + 1) * 4; ++tick) {
            sendTone(sink, "1001", 440, tick, 48000, 2, audioOrigin);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    sink.stop();

    MediaReader reader;
    ASSERT_TRUE(reader.open(recording(), AVMEDIA_TYPE_VIDEO));
    int frames = 0;
    while (av_read_frame(reader.input, reader.packet) >= 0) {
        if (reader.packet->stream_index == reader.stream) {
            ASSERT_GE(avcodec_send_packet(reader.decoder, reader.packet), 0);
            while (avcodec_receive_frame(reader.decoder, reader.frame) == 0) {
                ++frames;
                EXPECT_EQ(reader.frame->width, 160);
                EXPECT_EQ(reader.frame->height, 120);
            }
        }
        av_packet_unref(reader.packet);
    }
    ASSERT_GE(avcodec_send_packet(reader.decoder, nullptr), 0);
    while (avcodec_receive_frame(reader.decoder, reader.frame) == 0) ++frames;
    EXPECT_EQ(frames, 15);
    std::vector<float> audio;
    decodeAudio(audio);
    EXPECT_NEAR(audio.size() / 48000.0, 0.6, 0.025);
    EXPECT_GT(toneAmplitude(audio, 440, 0.1, 0.5), 0.15);
    nlohmann::json metadata;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir_)) {
        if (entry.path().extension() == ".json") {
            std::ifstream input(entry.path());
            input >> metadata;
        }
    }
    ASSERT_TRUE(metadata.value("sessionCompleted", false));
    ASSERT_EQ(metadata["files"].size(), 1u);
    const auto& file = metadata["files"][0];
    EXPECT_EQ(file["filename"], recording().filename().string());
    EXPECT_EQ(file["sizeBytes"], std::filesystem::file_size(recording()));
    EXPECT_EQ(file["width"], 160);
    EXPECT_EQ(file["height"], 120);
    EXPECT_TRUE(file["isComplete"].get<bool>());
    EXPECT_GT(file["durationSeconds"].get<double>(), 0.4);
}

TEST_F(RecordingMediaTest, CompositeRetainsBothUsersAcrossPairedCallbacksAndBriefVideoGap) {
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(config()));
    ASSERT_TRUE(sink.start());
    std::vector<uint8_t> redY(160 * 120, 81), redU(80 * 60, 90), redV(80 * 60, 240);
    std::vector<uint8_t> blueY(160 * 120, 41), blueU(80 * 60, 240), blueV(80 * 60, 110);
    auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 40; ++tick) {
        sink.onVideoFrame(redY.data(), redU.data(), redV.data(), 160, 80, 80, 160, 120,
                          1000 + tick * 40, "1001");
        if (tick < 8 || tick >= 38) {
            sink.onVideoFrame(blueY.data(), blueU.data(), blueV.data(), 160, 80, 80, 160, 120,
                              1000 + tick * 40, "1002");
        }
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 40));
    }
    sink.stop();
    MediaReader reader;
    ASSERT_TRUE(reader.open(recording(), AVMEDIA_TYPE_VIDEO));
    int framesWithBothUsers = 0, sampledFrames = 0;
    while (av_read_frame(reader.input, reader.packet) >= 0) {
        if (reader.packet->stream_index == reader.stream) {
            ASSERT_GE(avcodec_send_packet(reader.decoder, reader.packet), 0);
            while (avcodec_receive_frame(reader.decoder, reader.frame) == 0) {
                if (++sampledFrames < 5) continue;
                // The 4:3 canvas stacks the two panels vertically.
                int left = reader.frame->data[1][15 * reader.frame->linesize[1] + 40];
                int right = reader.frame->data[1][45 * reader.frame->linesize[1] + 40];
                if (left < 120 && right > 200) ++framesWithBothUsers;
            }
        }
        av_packet_unref(reader.packet);
    }
    ASSERT_GT(sampledFrames, 10);
    EXPECT_GE(framesWithBothUsers, sampledFrames - 5);
}

TEST_F(RecordingMediaTest, SpotlightRecordingPreservesSpeakerPlacementAndMetadata) {
    recordAndCheckLayout("spotlight");
}

TEST_F(RecordingMediaTest, CustomizedRecordingPreservesOverlayPlacementAndMetadata) {
    recordAndCheckLayout("customized");
}

TEST_F(RecordingMediaTest, PassthroughAudioResumeKeepsRtcGap) {
    auto settings = config();
    settings.mode = VideoCompositor::Mode::Individual;
    settings.videoDecodeMode = 0;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());
    sendEncodedKeyframe(sink);
    uint64_t origin = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    for (int tick = 0; tick < 10; ++tick) sendTone(sink, "1001", 440, tick, 48000, 2, origin);
    for (int tick = 60; tick < 70; ++tick) sendTone(sink, "1001", 440, tick, 48000, 2, origin);
    sink.stop();
    MediaReader reader;
    ASSERT_TRUE(reader.open(recording(), AVMEDIA_TYPE_AUDIO));
    double lastPts = 0;
    while (av_read_frame(reader.input, reader.packet) >= 0) {
        if (reader.packet->stream_index == reader.stream && reader.packet->pts != AV_NOPTS_VALUE)
            lastPts = reader.packet->pts * av_q2d(reader.input->streams[reader.stream]->time_base);
        av_packet_unref(reader.packet);
    }
    EXPECT_GT(lastPts, 0.65);
    EXPECT_LT(lastPts, 0.75);
    std::vector<float> samples;
    decodeAudio(samples);
    EXPECT_NEAR(samples.size() / 48000.0, 0.2, 0.05);
    EXPECT_GT(toneAmplitude(samples, 440, 0.02, 0.08), 0.14);
}

TEST_F(RecordingMediaTest, PassthroughResamplesLargeContinuousCallbacksWithoutAddingGaps) {
    auto settings = config();
    settings.mode = VideoCompositor::Mode::Individual;
    settings.videoDecodeMode = 0;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(settings));
    ASSERT_TRUE(sink.start());
    sendEncodedKeyframe(sink);
    std::vector<int16_t> pcm(4800);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(6000 * std::sin(2 * kPi * 440 * i / 24000));
    uint64_t origin = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    sink.onAudioFrame(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size(), 24000, 1, origin,
                      "1001");
    sink.onAudioFrame(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size(), 24000, 1,
                      origin + 200, "1001");
    sink.stop();
    std::vector<float> samples;
    decodeAudio(samples);
    EXPECT_GE(samples.size(), 19200u);
    EXPECT_NEAR(samples.size() / 48000.0, 0.4, 0.025);
    EXPECT_GT(toneAmplitude(samples, 440, 0.05, 0.15), 0.12);
}

TEST_F(RecordingMediaTest, CompositorHonorsFrameExpiryAndImmediatelyRestoresReturningPublisher) {
    VideoCompositor compositor;
    VideoCompositor::Config settings;
    settings.outputWidth = 160;
    settings.outputHeight = 120;
    settings.frameTimeoutMs = 100;
    settings.minCompositeIntervalMs = 0;
    settings.layoutDetectorConfig.framePresenceTimeoutMs = 100;
    settings.layoutDetectorConfig.userTimeoutMs = 400;
    settings.layoutDetectorConfig.layoutStabilityMs = 0;
    ASSERT_TRUE(compositor.initialize(settings));
    int left = 0, right = 0;
    compositor.setComposedVideoFrameCallback([&](const AVFrame* frame) {
        left = frame->data[1][15 * frame->linesize[1] + 40];
        right = frame->data[1][45 * frame->linesize[1] + 40];
    });
    std::vector<uint8_t> y(160 * 120, 81), u(80 * 60, 90), v(80 * 60, 240);
    auto send = [&](const std::string& user, uint8_t chroma) {
        std::fill(u.begin(), u.end(), chroma);
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
        return compositor.addUserFrame(y.data(), u.data(), v.data(), 160, 80, 80, 160, 120, now,
                                       user);
    };
    ASSERT_TRUE(send("1001", 90));
    ASSERT_TRUE(send("1002", 240));
    EXPECT_LT(left, 120);
    EXPECT_GT(right, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    ASSERT_TRUE(send("1001", 90));
    EXPECT_LT(right, 120);
    ASSERT_TRUE(send("1002", 240));
    EXPECT_LT(left, 120);
    EXPECT_GT(right, 200);
}
