#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "line_message_buffer.h"

TEST(UDSFramingTest, PreservesLargeCustomizedPayloadAcrossReads) {
    agora::egress::LineMessageBuffer buffer;
    std::vector<std::string> messages;
    auto consume = [&](const std::string& line) { messages.push_back(line); };
    std::string message = "{\"regions\":\"" + std::string(8000, 'x') + "\"}";
    ASSERT_TRUE(buffer.append(message.data(), 4095, consume));
    ASSERT_TRUE(messages.empty());
    ASSERT_TRUE(buffer.append(message.data() + 4095, message.size() - 4095, consume));
    ASSERT_TRUE(messages.empty());
    ASSERT_TRUE(buffer.append("\n{\"cmd\":\"stop\"}\n", 16, consume));
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], message);
    EXPECT_EQ(messages[1], "{\"cmd\":\"stop\"}");
}

TEST(UDSFramingTest, RejectsOversizedUnterminatedMessage) {
    agora::egress::LineMessageBuffer buffer;
    std::string message(65537, 'x');
    EXPECT_FALSE(buffer.append(message.data(), message.size(), [](const std::string&) {}));
}
