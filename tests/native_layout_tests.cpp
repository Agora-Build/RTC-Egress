#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../src/include/uds_message.h"
#include "../src/include/video_compositor.h"

using namespace agora::rtc;

class NativeLayoutTest : public ::testing::Test {
   protected:
    VideoCompositor compositor;
    std::vector<uint8_t> canvas;
    mutable std::mutex canvasMutex;
    int width = 320;
    int height = 180;

    void TearDown() override {
        compositor.cleanup();
    }

    VideoCompositor::Config settings(const std::string& layout) {
        VideoCompositor::Config config;
        config.outputWidth = width;
        config.outputHeight = height;
        config.layout = layout;
        config.minCompositeIntervalMs = 0;
        return config;
    }

    bool start(const VideoCompositor::Config& config) {
        compositor.setComposedVideoFrameCallback([this](const AVFrame* frame) {
            std::lock_guard<std::mutex> lock(canvasMutex);
            canvas.resize(width * height);
            for (int y = 0; y < height; ++y)
                std::copy_n(frame->data[0] + y * frame->linesize[0], width,
                            canvas.begin() + y * width);
        });
        return compositor.initialize(config);
    }

    bool send(const std::string& uid, uint8_t luma) {
        std::vector<uint8_t> y(320 * 180, luma), uv(160 * 90, 128);
        return compositor.addUserFrame(y.data(), uv.data(), uv.data(), 320, 160, 160, 320, 180,
                                       1000, uid);
    }

    int pixel(int x, int y) const {
        std::lock_guard<std::mutex> lock(canvasMutex);
        return canvas.at(y * width + x);
    }
};

TEST_F(NativeLayoutTest, FlatKeepsEqualTilesInRequestedOrder) {
    auto config = settings("flat");
    config.userOrder = {"speaker", "guest"};
    ASSERT_TRUE(start(config));
    ASSERT_TRUE(send("guest", 80));
    ASSERT_TRUE(send("speaker", 180));
    EXPECT_EQ(pixel(80, 90), 180);
    EXPECT_EQ(pixel(240, 90), 80);
}

TEST_F(NativeLayoutTest, CanRenderAfterCleanupAndReinitialize) {
    ASSERT_TRUE(start(settings("flat")));
    ASSERT_TRUE(send("first", 180));
    compositor.cleanup();
    ASSERT_TRUE(start(settings("flat")));
    ASSERT_TRUE(send("second", 80));
    EXPECT_EQ(pixel(160, 90), 80);
}

TEST_F(NativeLayoutTest, SpotlightUsesRequestedSpeakerAndSideThumbnails) {
    auto config = settings("spotlight");
    config.userOrder = {"z_speaker", "a_guest", "b_guest"};
    ASSERT_TRUE(start(config));
    ASSERT_TRUE(send("a_guest", 80));
    ASSERT_TRUE(send("b_guest", 120));
    ASSERT_TRUE(send("z_speaker", 180));
    EXPECT_EQ(pixel(160, 90), 180);
    EXPECT_EQ(pixel(280, 45), 80);
    EXPECT_EQ(pixel(280, 135), 120);
}

TEST_F(NativeLayoutTest, SpotlightAllUsersKeepsFirstPublisherProminent) {
    ASSERT_TRUE(start(settings("spotlight")));
    ASSERT_TRUE(send("z_first", 180));
    ASSERT_TRUE(send("a_second", 80));
    EXPECT_EQ(pixel(160, 90), 180);
    EXPECT_EQ(pixel(280, 90), 80);
}

TEST_F(NativeLayoutTest, SpotlightPortraitUsesBottomThumbnails) {
    width = 180;
    height = 320;
    auto config = settings("spotlight");
    config.userOrder = {"speaker", "guest1", "guest2"};
    ASSERT_TRUE(start(config));
    ASSERT_TRUE(send("speaker", 180));
    ASSERT_TRUE(send("guest1", 80));
    ASSERT_TRUE(send("guest2", 120));
    EXPECT_EQ(pixel(90, 120), 180);
    EXPECT_EQ(pixel(45, 280), 80);
    EXPECT_EQ(pixel(135, 280), 120);
}

TEST_F(NativeLayoutTest, SpotlightPromotesGuestWhileSpeakerIsAwayAndRestoresSpeaker) {
    auto config = settings("spotlight");
    config.userOrder = {"speaker", "guest"};
    ASSERT_TRUE(start(config));
    ASSERT_TRUE(send("speaker", 180));
    ASSERT_TRUE(send("guest", 80));
    compositor.removeUser("speaker");
    ASSERT_TRUE(send("guest", 80));
    EXPECT_EQ(pixel(160, 90), 80);
    ASSERT_TRUE(send("speaker", 180));
    EXPECT_EQ(pixel(160, 90), 180);
    EXPECT_EQ(pixel(280, 90), 80);
}

TEST_F(NativeLayoutTest, CustomizedHonorsCoordinatesAndZOrder) {
    auto config = settings("customized");
    config.regions = {{"overlay", 160, 90, 160, 90, 10}, {"background", 0, 0, 320, 180, -1}};
    ASSERT_TRUE(start(config));
    ASSERT_TRUE(send("overlay", 180));
    ASSERT_TRUE(send("background", 80));
    ASSERT_TRUE(send("unlisted", 220));
    EXPECT_EQ(pixel(80, 45), 80);
    EXPECT_EQ(pixel(240, 135), 180);
}

TEST_F(NativeLayoutTest, CustomizedKeepsEmptyRegionsBlack) {
    auto config = settings("customized");
    config.regions = {{"guest", 160, 90, 160, 90, 0}};
    ASSERT_TRUE(start(config));
    ASSERT_TRUE(send("guest", 180));
    EXPECT_EQ(pixel(80, 45), 0);
    EXPECT_EQ(pixel(240, 135), 180);
}

TEST_F(NativeLayoutTest, CustomizedRejectsOutOfCanvasAndUnalignedRegions) {
    auto config = settings("customized");
    config.regions = {{"guest", 200, 0, 160, 90, 0}};
    EXPECT_FALSE(start(config));
    config.regions = {{"guest", 1, 0, 160, 90, 0}};
    EXPECT_FALSE(start(config));
}

TEST_F(NativeLayoutTest, CustomizedRequiresRegions) {
    EXPECT_FALSE(start(settings("customized")));
}

TEST(NativeLayoutProtocolTest, WorkerJsonRetainsRegionsAndCanvasDimensions) {
    auto input = nlohmann::json::parse(R"({"cmd":"record","channel":"layouttest",
        "access_token":"testtoken123","layout":"customized","width":320,"height":180,
        "regions":[{"uid":"speaker","x":0,"y":0,"width":320,"height":180,"z":-1},
                   {"uid":"guest","x":160,"y":90,"width":160,"height":90,"z":10}]})");
    auto message = input.get<UDSMessage>();
    nlohmann::json output = message;
    EXPECT_EQ(output["regions"], input["regions"]);
    EXPECT_EQ(output["width"], 320);
    EXPECT_EQ(output["height"], 180);
}
