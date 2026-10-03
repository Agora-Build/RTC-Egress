#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>
#include <vector>

#include "../src/include/recording_sink.h"
#include "../src/include/snapshot_sink.h"

using namespace agora::rtc;

class RecordingThreadTest : public ::testing::Test {
   protected:
    void SetUp() override {
        outputDir_ = std::filesystem::temp_directory_path() /
                     ("rtc_thread_test_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(outputDir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(outputDir_);
    }

    RecordingSink::Config recordingConfig() const {
        RecordingSink::Config config;
        config.outputDir = outputDir_.string();
        config.mode = VideoCompositor::Mode::Individual;
        config.videoWidth = 160;
        config.videoHeight = 120;
        config.videoFps = 25;
        config.useRealTimeScheduling = false;
        return config;
    }

    void sendFrames(RecordingSink& sink, bool video, bool audio) {
        std::thread videoProducer([&] {
            if (!video) return;
            std::vector<uint8_t> y(160 * 120, 80);
            std::vector<uint8_t> u(80 * 60, 128);
            std::vector<uint8_t> v(80 * 60, 128);
            for (int i = 0; i < 50; ++i) {
                sink.onVideoFrame(y.data(), u.data(), v.data(), 160, 80, 80, 160, 120,
                                  1000 + i * 40, "42");
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        });
        std::thread audioProducer([&] {
            if (!audio) return;
            std::vector<int16_t> samples(1024 * 2, 1000);
            for (int i = 0; i < 50; ++i) {
                sink.onAudioFrame(reinterpret_cast<const uint8_t*>(samples.data()), 1024, 48000, 2,
                                  1000 + i * 22, "42");
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        });
        videoProducer.join();
        audioProducer.join();
    }

    void expectRecordedStreams(bool video, bool audio) {
        std::vector<std::filesystem::path> recordings;
        for (const auto& entry : std::filesystem::directory_iterator(outputDir_)) {
            if (entry.path().extension() == ".mp4") recordings.push_back(entry.path());
        }
        ASSERT_EQ(recordings.size(), 1u);

        AVFormatContext* input = nullptr;
        ASSERT_EQ(avformat_open_input(&input, recordings.front().c_str(), nullptr, nullptr), 0);
        ASSERT_EQ(avformat_find_stream_info(input, nullptr), 0);
        std::array<int, 2> packetCounts = {0, 0};
        std::vector<int64_t> lastDts(input->nb_streams, AV_NOPTS_VALUE);
        AVPacket* packet = av_packet_alloc();
        ASSERT_NE(packet, nullptr);
        while (av_read_frame(input, packet) >= 0) {
            auto type = input->streams[packet->stream_index]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO) ++packetCounts[0];
            if (type == AVMEDIA_TYPE_AUDIO) ++packetCounts[1];
            auto& previous = lastDts[packet->stream_index];
            if (packet->dts != AV_NOPTS_VALUE) {
                if (previous != AV_NOPTS_VALUE) EXPECT_GT(packet->dts, previous);
                previous = packet->dts;
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        avformat_close_input(&input);
        EXPECT_EQ(packetCounts[0] > 0, video);
        EXPECT_EQ(packetCounts[1] > 0, audio);
    }

    std::filesystem::path outputDir_;
};

TEST_F(RecordingThreadTest, RejectsInvalidAudioPriority) {
    auto config = recordingConfig();
    config.audioPriority = 0;
    RecordingSink sink;
    EXPECT_FALSE(sink.initialize(config));
}

TEST_F(RecordingThreadTest, RejectsInvalidVideoPriority) {
    auto config = recordingConfig();
    config.videoPriority = 100;
    RecordingSink sink;
    EXPECT_FALSE(sink.initialize(config));
}

TEST_F(RecordingThreadTest, RejectsInvalidSnapshotPriority) {
    SnapshotSink::Config config;
    config.outputDir = outputDir_.string();
    config.snapshotPriority = 100;
    SnapshotSink sink;
    EXPECT_FALSE(sink.initialize(config));
}

TEST_F(RecordingThreadTest, DurationExpiryStopsRecordingWithoutCompletionCallback) {
    auto config = recordingConfig();
    config.maxDurationSeconds = 1;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sink.isRecording() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(sink.isRecording());
    sink.stop();
}

TEST_F(RecordingThreadTest, ConcurrentIndividualAudioAndVideoProduceValidRecording) {
    auto config = recordingConfig();
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    sendFrames(sink, true, true);
    sink.stop();
    expectRecordedStreams(true, true);
}

TEST_F(RecordingThreadTest, ConcurrentCompositeAudioAndVideoProduceValidRecording) {
    auto config = recordingConfig();
    config.mode = VideoCompositor::Mode::Composite;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    sendFrames(sink, true, true);
    sink.stop();
    expectRecordedStreams(true, true);
}

TEST_F(RecordingThreadTest, AudioOnlyWorkerProducesValidRecording) {
    auto config = recordingConfig();
    config.recordVideo = false;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    sendFrames(sink, false, true);
    sink.stop();
    expectRecordedStreams(false, true);
}

TEST_F(RecordingThreadTest, VideoOnlyWorkerProducesValidRecording) {
    auto config = recordingConfig();
    config.recordAudio = false;
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    sendFrames(sink, true, false);
    sink.stop();
    expectRecordedStreams(true, false);
}

TEST_F(RecordingThreadTest, IdleWorkersWakeImmediatelyOnStop) {
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(recordingConfig()));
    ASSERT_TRUE(sink.start());
    auto started = std::chrono::steady_clock::now();
    sink.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

TEST_F(RecordingThreadTest, DurationCompletionCallbackCanStopSink) {
    auto config = recordingConfig();
    config.maxDurationSeconds = 1;
    config.taskId = "duration_stop";
    std::promise<void> completed;
    auto completion = completed.get_future();
    RecordingSink sink;
    sink.setCompletionCallback([&](const std::string&, const std::string&, const std::string&) {
        sink.stop();
        completed.set_value();
    });
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    EXPECT_EQ(completion.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    sink.stop();
    EXPECT_FALSE(sink.isRecording());
}

TEST_F(RecordingThreadTest, ConcurrentStopCallsJoinWorkersOnce) {
    RecordingSink sink;
    ASSERT_TRUE(sink.initialize(recordingConfig()));
    ASSERT_TRUE(sink.start());
    sendFrames(sink, true, true);
    std::thread first([&] { sink.stop(); });
    std::thread second([&] { sink.stop(); });
    first.join();
    second.join();
    expectRecordedStreams(true, true);
}

TEST_F(RecordingThreadTest, RestartsAfterDurationCompletionCallbackStopsSink) {
    auto config = recordingConfig();
    config.maxDurationSeconds = 1;
    config.taskId = "duration_restart";
    std::promise<void> completed;
    auto completion = completed.get_future();
    RecordingSink sink;
    sink.setCompletionCallback([&](const std::string&, const std::string&, const std::string&) {
        sink.stop();
        completed.set_value();
    });
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    ASSERT_EQ(completion.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    config.maxDurationSeconds = 60;
    ASSERT_TRUE(sink.initialize(config));
    ASSERT_TRUE(sink.start());
    sendFrames(sink, true, true);
    sink.stop();
    expectRecordedStreams(true, true);
}

TEST_F(RecordingThreadTest, ThreadedSnapshotEncoderWritesFirstImage) {
    SnapshotEncoder encoder;
    SnapshotEncoder::Config config;
    config.useThreads = true;
    ASSERT_TRUE(encoder.initialize(config));
    std::vector<uint8_t> y(160 * 120, 80);
    std::vector<uint8_t> u(80 * 60, 128);
    std::vector<uint8_t> v(80 * 60, 128);
    auto imagePath = outputDir_ / "first.jpg";
    ASSERT_TRUE(encoder.encodeYUVToJPEG(y.data(), u.data(), v.data(), 160, 80, 80, 160, 120,
                                        imagePath.string()));
    EXPECT_GT(std::filesystem::file_size(imagePath), 0u);
}
