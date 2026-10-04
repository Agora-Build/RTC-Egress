#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <thread>
#include <vector>

#include "streaming_sink.h"

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) {
    stopRequested = 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 10) {
        std::cerr << "Usage: streaming_media_fixture rtmp|whip URL USERS SECONDS "
                     "[ATTEMPTS DELAY_MS MAX_SECONDS TIMEOUT_MS IDLE_AFTER_MS]\n";
        return 2;
    }
    agora::rtc::StreamingSink sink;
    agora::rtc::StreamingSink::Config config;
    config.output.protocol = std::string(argv[1]) == "whip"
                                 ? agora::rtc::StreamingOutput::Protocol::WHIP
                                 : agora::rtc::StreamingOutput::Protocol::RTMP;
    config.output.url = argv[2];
    config.output.timeoutMs = 2000;
    config.taskId = "fixture";
    if (argc > 5) config.reconnectAttempts = std::stoi(argv[5]);
    if (argc > 6) config.reconnectDelayMs = std::stoi(argv[6]);
    if (argc > 7) config.maxDurationSeconds = std::stoi(argv[7]);
    if (argc > 8) config.output.timeoutMs = std::stoi(argv[8]);
    config.width = 640;
    config.height = 360;
    config.fps = 25;
    int users = std::stoi(argv[3]);
    int seconds = std::stoi(argv[4]);
    int idleAfterMs = argc > 9 ? std::stoi(argv[9]) : seconds * 1000;
    if (users < 1 || users > 2 || seconds < 1 || seconds > 60) return 2;
    config.targetUsers =
        users == 1 ? std::vector<std::string>{"1001"} : std::vector<std::string>{"1001", "1002"};
    std::atomic<bool> completed{false};
    sink.setCompletionCallback(
        [&](const std::string&, const std::string& status, const std::string& message) {
            std::cout << "complete " << status << " " << message << "\n" << std::flush;
            completed = true;
        });
    std::signal(SIGTERM, requestStop);
    std::signal(SIGINT, requestStop);
    if (!sink.initialize(config) || !sink.start()) return 1;
    std::cout << "ready\n" << std::flush;
    const auto start = std::chrono::steady_clock::now();
    bool reconnecting = false;
    for (int tick = 0; tick < seconds * 100 && sink.isStreaming() && !stopRequested; ++tick) {
        if (sink.isReconnecting() != reconnecting) {
            reconnecting = !reconnecting;
            std::cout << (reconnecting ? "reconnecting\n" : "recovered\n") << std::flush;
        }
        uint64_t timestamp = 1000 + tick * 10;
        for (int user = 0; user < users && tick * 10 < idleAfterMs; ++user) {
            // RTC audio often starts before the first decoded video keyframe.
            if (tick >= 40 && tick % 4 == 0) {
                std::vector<uint8_t> y(320 * 180, (user ? 41 : 81) + (tick / 4) % 8);
                std::vector<uint8_t> u(160 * 90, user ? 240 : 90);
                std::vector<uint8_t> v(160 * 90, user ? 110 : 240);
                // Duplicate occasional RTC timestamps to exercise FLV's millisecond precision.
                uint64_t videoTimestamp = tick % 8 == 4 ? timestamp - 40 : timestamp;
                sink.onVideoFrame(y.data(), u.data(), v.data(), 320, 160, 160, 320, 180,
                                  videoTimestamp, config.targetUsers[user]);
            }
            std::vector<int16_t> pcm(480 * 2);
            for (int i = 0; i < 480; ++i) {
                int16_t value = 4000 * std::sin(2 * 3.141592653589793 * (user ? 880 : 440) *
                                                (tick * 480 + i) / 48000);
                pcm[i * 2] = pcm[i * 2 + 1] = value;
            }
            sink.onAudioFrame(reinterpret_cast<const uint8_t*>(pcm.data()), 480, 48000, 2,
                              timestamp, config.targetUsers[user]);
        }
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 10));
    }
    // Let terminal callbacks finish before explicit stop can suppress them.
    if (!stopRequested && !sink.isStreaming()) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!completed && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto stopping = std::chrono::steady_clock::now();
    sink.stop();
    std::cout << "stop_ms "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - stopping)
                     .count()
              << "\n"
              << std::flush;
    return sink.hasFailed() ? 1 : 0;
}
