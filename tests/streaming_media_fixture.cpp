#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

#include "streaming_sink.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: streaming_media_fixture rtmp|whip URL USERS SECONDS\n";
        return 2;
    }
    agora::rtc::StreamingSink sink;
    agora::rtc::StreamingSink::Config config;
    config.output.protocol = std::string(argv[1]) == "whip"
                                 ? agora::rtc::StreamingOutput::Protocol::WHIP
                                 : agora::rtc::StreamingOutput::Protocol::RTMP;
    config.output.url = argv[2];
    config.output.timeoutMs = 2000;
    config.width = 640;
    config.height = 360;
    config.fps = 25;
    int users = std::stoi(argv[3]);
    int seconds = std::stoi(argv[4]);
    if (users < 1 || users > 2 || seconds < 1 || seconds > 60) return 2;
    config.targetUsers =
        users == 1 ? std::vector<std::string>{"1001"} : std::vector<std::string>{"1001", "1002"};
    if (!sink.initialize(config) || !sink.start()) return 1;
    std::cout << "ready\n" << std::flush;
    const auto start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < seconds * 100 && !sink.hasFailed(); ++tick) {
        uint64_t timestamp = 1000 + tick * 10;
        for (int user = 0; user < users; ++user) {
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
    sink.stop();
    return sink.hasFailed() ? 1 : 0;
}
