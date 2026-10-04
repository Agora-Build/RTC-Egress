#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <thread>

#include "streaming_sink.h"
#include "uds_message.h"

using agora::rtc::StreamingOutput;
using agora::rtc::StreamingSink;

TEST(StreamingSinkTest, RejectsInvalidReconnectPolicy) {
    for (int attempts : {-1, 21}) {
        StreamingSink sink;
        StreamingSink::Config config;
        config.output.url = "rtmp://127.0.0.1:1/live/test";
        config.reconnectAttempts = attempts;
        EXPECT_FALSE(sink.initialize(config));
    }
    for (int delay : {99, 10001}) {
        StreamingSink sink;
        StreamingSink::Config config;
        config.output.url = "rtmp://127.0.0.1:1/live/test";
        config.reconnectDelayMs = delay;
        EXPECT_FALSE(sink.initialize(config));
    }
}

TEST(StreamingSinkTest, ReconnectDefaultsAndExplicitZeroSurviveUds) {
    auto defaults = nlohmann::json{{"cmd", "rtmp"}, {"channel", "test"}, {"access_token", ""}}
                        .get<UDSMessage>();
    EXPECT_EQ(defaults.output_reconnect_attempts, 5);
    EXPECT_EQ(defaults.output_reconnect_delay_ms, 1000);
    defaults.output_reconnect_attempts = 0;
    defaults.output_reconnect_delay_ms = 100;
    auto disabled = nlohmann::json(defaults).get<UDSMessage>();
    EXPECT_EQ(disabled.output_reconnect_attempts, 0);
    EXPECT_EQ(disabled.output_reconnect_delay_ms, 100);
}

TEST(StreamingSinkTest, StopInterruptsStalledInitialHandshake) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(server, 1), 0);
    socklen_t size = sizeof(address);
    ASSERT_EQ(getsockname(server, reinterpret_cast<sockaddr*>(&address), &size), 0);
    std::promise<void> accepted;
    auto ready = accepted.get_future();
    auto peer = std::async(std::launch::async, [&] {
        int client = accept(server, nullptr, nullptr);
        accepted.set_value();
        if (client >= 0) {
            char data[4096];
            while (recv(client, data, sizeof(data), 0) > 0) {
            }
            close(client);
        }
        close(server);
    });
    StreamingSink sink;
    StreamingSink::Config config;
    config.width = 320;
    config.height = 180;
    config.output.url =
        "rtmp://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/live/test";
    config.output.timeoutMs = 30000;
    ASSERT_TRUE(sink.initialize(config));
    auto starting = std::async(std::launch::async, [&] { return sink.start(); });
    if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        shutdown(server, SHUT_RDWR);
        sink.stop();
        FAIL() << "Publisher did not enter the handshake";
    }
    auto stopped = std::chrono::steady_clock::now();
    sink.stop();
    EXPECT_FALSE(starting.get());
    EXPECT_FALSE(sink.hasFailed());
    EXPECT_LT(std::chrono::steady_clock::now() - stopped, std::chrono::seconds(2));
    peer.get();
}

TEST(StreamingSinkTest, RejectsFileDestination) {
    StreamingSink sink;
    StreamingSink::Config config;
    config.output.url = "/tmp/recording.mp4";
    EXPECT_FALSE(sink.initialize(config));
}

TEST(StreamingSinkTest, RejectsInvalidCanvasBeforeConnecting) {
    StreamingSink sink;
    StreamingSink::Config config;
    config.output.url = "rtmp://127.0.0.1:1/live/test";
    config.width = 1279;
    EXPECT_FALSE(sink.initialize(config));
}

TEST(StreamingSinkTest, ConnectionFailureIsBoundedAndDoesNotBecomeActive) {
    StreamingSink sink;
    StreamingSink::Config config;
    config.width = 320;
    config.height = 180;
    config.output.url = "rtmp://127.0.0.1:1/live/test";
    config.output.timeoutMs = 1000;
    ASSERT_TRUE(sink.initialize(config));
    auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(sink.start());
    EXPECT_TRUE(sink.hasFailed());
    EXPECT_FALSE(sink.isStreaming());
    sink.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
}

TEST(StreamingSinkTest, RejectsWhipTokenOnRtmp) {
    StreamingSink sink;
    StreamingSink::Config config;
    config.output.url = "rtmp://127.0.0.1:1/live/test";
    config.output.token = "test-token";
    EXPECT_FALSE(sink.initialize(config));
}

TEST(StreamingSinkTest, RejectsUnsupportedLayoutBeforeConnecting) {
    StreamingSink sink;
    StreamingSink::Config config;
    config.output.url = "rtmp://127.0.0.1:1/live/test";
    config.layout = "freestyle";
    EXPECT_FALSE(sink.initialize(config));
}

TEST(StreamingSinkTest, StalledRtmpHandshakeHonorsDeadline) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(server, 1), 0);
    socklen_t size = sizeof(address);
    ASSERT_EQ(getsockname(server, reinterpret_cast<sockaddr*>(&address), &size), 0);
    auto peer = std::async(std::launch::async, [server] {
        int client = accept(server, nullptr, nullptr);
        if (client >= 0) {
            char data[4096];
            while (recv(client, data, sizeof(data), 0) > 0) {
            }
            close(client);
        }
        close(server);
    });
    StreamingSink sink;
    StreamingSink::Config config;
    config.width = 320;
    config.height = 180;
    config.output.url =
        "rtmp://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/live/test";
    config.output.timeoutMs = 1000;
    ASSERT_TRUE(sink.initialize(config));
    auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(sink.start());
    sink.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    peer.get();
}

TEST(AudioMixerTest, PublisherCanContinueWhileOutputIsBusy) {
    agora::rtc::AudioMixer mixer;
    agora::rtc::AudioFrame frame;
    frame.valid = true;
    frame.timestamp = 1000;
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.data.resize(480 * 2 * sizeof(int16_t));
    ASSERT_TRUE(mixer.push(frame, "1001"));
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto unblock = release.get_future();
    bool first = true;
    auto consumer = std::async(std::launch::async, [&] {
        return mixer.drain(480, true, [&](const agora::rtc::AudioFrame&) {
            if (first) {
                first = false;
                entered.set_value();
                unblock.wait();
            }
            return true;
        });
    });
    ready.wait();
    auto publisher = std::async(std::launch::async, [&] {
        frame.timestamp += 10;
        return mixer.push(frame, "1002");
    });
    auto status = publisher.wait_for(std::chrono::milliseconds(250));
    release.set_value();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(publisher.get());
    EXPECT_TRUE(consumer.get());
}

TEST(AudioMixerTest, DrainYieldsWhenPublisherKeepsAppending) {
    agora::rtc::AudioMixer mixer;
    agora::rtc::AudioFrame frame;
    frame.valid = true;
    frame.timestamp = 1000;
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.data.resize(480 * 2 * sizeof(int16_t));
    ASSERT_TRUE(mixer.push(frame, "1001"));
    int consumed = 0;
    bool drained = mixer.drain(480, true, [&](const agora::rtc::AudioFrame&) {
        ++consumed;
        frame.timestamp += 10;
        if (!mixer.push(frame, "1001")) return false;
        return consumed < 5;
    });
    EXPECT_TRUE(drained);
    EXPECT_EQ(consumed, 1);
}
