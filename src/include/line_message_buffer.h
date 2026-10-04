#pragma once

#include <functional>
#include <string>

namespace agora::egress {

// UDS is a byte stream: JSON lines can span reads or arrive together.
class LineMessageBuffer {
   public:
    bool append(const char* data, size_t size,
                const std::function<void(const std::string&)>& consume) {
        pending_.append(data, size);
        size_t begin = 0;
        while (true) {
            size_t end = pending_.find('\n', begin);
            if (end == std::string::npos) break;
            if (end - begin > MAX_MESSAGE_SIZE) {
                pending_.clear();
                return false;
            }
            if (end != begin) consume(pending_.substr(begin, end - begin));
            begin = end + 1;
        }
        pending_.erase(0, begin);
        if (pending_.size() > MAX_MESSAGE_SIZE) {
            pending_.clear();
            return false;
        }
        return true;
    }

   private:
    static constexpr size_t MAX_MESSAGE_SIZE = 65536;
    std::string pending_;
};

}  // namespace agora::egress
