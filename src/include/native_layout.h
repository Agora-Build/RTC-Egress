#pragma once

#include <string>

namespace agora {
namespace rtc {

struct LayoutRegion {
    std::string uid;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int z = 0;
};

}  // namespace rtc
}  // namespace agora
