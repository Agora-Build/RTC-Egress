#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agora::rtc {

struct AudioFrame {
    std::vector<uint8_t> data;
    uint64_t timestamp = 0;
    int sampleRate = 0;
    int channels = 0;
    bool valid = false;
    std::string userId;
};

}  // namespace agora::rtc
