#define AG_LOG_TAG "VideoCompositor"

#include "include/video_compositor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

#include "common/ffmpeg_utils.h"
#include "common/log.h"

extern "C" {
#include <libavutil/imgutils.h>
}

namespace agora {
namespace rtc {

VideoCompositor::VideoCompositor() = default;

VideoCompositor::~VideoCompositor() {
    cleanup();
}

bool VideoCompositor::initialize(const Config& config) {
    cleanup();
    std::lock_guard<std::mutex> lock(frameBufferMutex_);

    if (config.outputWidth < 2 || config.outputHeight < 2 || config.outputWidth > 8192 ||
        config.outputHeight > 8192 || config.outputWidth % 2 || config.outputHeight % 2) {
        AG_LOG_FAST(ERROR, "Layout canvas dimensions must be even and between 2 and 8192");
        return false;
    }
    if (config.layout != "flat" && config.layout != "spotlight" && config.layout != "customized") {
        AG_LOG_FAST(ERROR, "Unsupported native layout: %s", config.layout.c_str());
        return false;
    }
    if ((config.layout == "customized" && config.regions.empty()) || config.regions.size() > 32 ||
        (config.layout != "customized" && !config.regions.empty())) {
        AG_LOG_FAST(ERROR, "Customized layouts require between 1 and 32 regions");
        return false;
    }
    for (const auto& region : config.regions) {
        if (region.uid.empty() || region.x < 0 || region.y < 0 || region.width < 2 ||
            region.height < 2 || region.x % 2 || region.y % 2 || region.width % 2 ||
            region.height % 2 ||
            static_cast<int64_t>(region.x) + region.width > config.outputWidth ||
            static_cast<int64_t>(region.y) + region.height > config.outputHeight) {
            AG_LOG_FAST(ERROR, "Invalid or out-of-canvas region for user %s", region.uid.c_str());
            return false;
        }
    }

    config_ = config;
    stopRequested_ = false;

    if (!setupCompositeFrame()) {
        return false;
    }

    // Initialize LayoutDetector (always enabled for layout stability)
    layoutDetector_ = std::make_unique<LayoutDetector>();
    if (!layoutDetector_->initialize(config_.layoutDetectorConfig)) {
        AG_LOG_FAST(ERROR, "Failed to initialize LayoutDetector");
        return false;
    }

    // Set layout change callback
    layoutDetector_->setLayoutChangeCallback(
        [this](const std::vector<std::string>& activeUsers) { this->onLayoutChange(activeUsers); });

    layoutDetector_->start();
    AG_LOG_FAST(INFO, "LayoutDetector started (always enabled)");

    initialized_ = true;
    lastCompositeTime_ = getCurrentTimeMs();

    AG_LOG_FAST(INFO, "Initialized with %dx%d output resolution", config_.outputWidth,
                config_.outputHeight);

    return true;
}

void VideoCompositor::setComposedVideoFrameCallback(ComposedVideoFrameCallback callback) {
    videoFrameCallback_ = std::move(callback);
}

bool VideoCompositor::addUserFrame(const VideoFrame& frame, const std::string& userId) {
    if (!initialized_ || !frame.valid() || stopRequested_.load()) {
        return false;
    }

    uint64_t currentTime = getCurrentTimeMs();

    // Quick check if LayoutDetector is still running before potentially blocking operations
    // This prevents hanging during shutdown when the LayoutDetector may be stopping
    if (!layoutDetector_ || stopRequested_.load()) {
        return false;
    }

    // Notify LayoutDetector of frame activity (always enabled)
    auto activeUsers = layoutDetector_->onUserFrame(userId, currentTime);

    std::lock_guard<std::mutex> lock(frameBufferMutex_);

    // Store the frame with current timestamp
    if (std::find(arrivalOrder_.begin(), arrivalOrder_.end(), userId) == arrivalOrder_.end())
        arrivalOrder_.push_back(userId);
    frameBuffer_[userId] = {frame, currentTime};
    // Clean up old frames
    cleanupOldFramesWithoutLock();

    // Performance-based frame rate control - only reduce frames when needed
    uint64_t minInterval = config_.minCompositeIntervalMs;
    uint64_t interval = currentTime - lastCompositeTime_;

    // Always try to create composite frame at configured rate
    // Only skip if we're processing too fast or if performance is poor
    if (interval >= minInterval) {
        bool result = createCompositeFrameWithoutLock(activeUsers);
        static int create_composite_frame_log_count = 0;
        if (create_composite_frame_log_count % 30 == 0) {
            AG_LOG_FAST(INFO, "addUserFrame() createCompositeFrame() returned %d", result);
        }
        create_composite_frame_log_count++;
        return result;
    }

    // Frame rate limiting - only skip if we're processing faster than configured rate
    static int skip_frame_log_count = 0;
    if (skip_frame_log_count % 100 == 0) {  // Log every 100 skips to avoid spam
        AG_LOG_FAST(INFO, "addUserFrame() frame rate limiting (interval %lu < %lu)", interval,
                    minInterval);
    }
    skip_frame_log_count++;
    return true;
}

bool VideoCompositor::addUserFrame(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                   const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                   int32_t vStride, uint32_t width, uint32_t height,
                                   uint64_t timestamp, const std::string& userId) {
    // Validate input parameters using shared utility
    if (!agora::common::validateYUVBuffers(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride,
                                           width, height)) {
        AG_LOG_FAST(ERROR, "YUV buffer validation failed");
        return false;
    }

    VideoFrame frame;
    if (!frame.initializeFromYUV(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride, width,
                                 height, timestamp, userId)) {
        return false;
    }

    return addUserFrame(frame, userId);
}

bool VideoCompositor::createCompositeFrameWithoutLock(const std::vector<std::string>& activeUsers) {
    if (!initialized_ || !compositeFrame_ || frameBuffer_.empty() || stopRequested_.load()) {
        return false;
    }

    // Check if LayoutDetector is available before calling potentially blocking operations
    if (!layoutDetector_ || stopRequested_.load()) {
        return false;
    }

    uint64_t currentTime = getCurrentTimeMs();

    // Filter to only users that have frames in buffer
    std::vector<std::string> usersWithFrames;
    for (const auto& userId : activeUsers) {
        if (frameBuffer_.find(userId) != frameBuffer_.end()) {
            usersWithFrames.push_back(userId);
        }
    }

    if (usersWithFrames.empty()) {
        return true;  // No frames to composite
    }

    std::vector<std::string> ordered;
    const auto& order = config_.userOrder.empty() ? arrivalOrder_ : config_.userOrder;
    for (const auto& user : order)
        if (std::find(usersWithFrames.begin(), usersWithFrames.end(), user) !=
            usersWithFrames.end())
            ordered.push_back(user);
    for (const auto& user : usersWithFrames)
        if (std::find(ordered.begin(), ordered.end(), user) == ordered.end())
            ordered.push_back(user);
    auto regions = calculateRegions(ordered);

    // Clear composite frame to black
    if (av_frame_make_writable(compositeFrame_) < 0) {
        AG_LOG_FAST(ERROR, "Failed to make composite frame writable");
        return false;
    }

    // Fill with black (Y=0, U=128, V=128 for YUV420P)
    memset(compositeFrame_->data[0], 0, compositeFrame_->linesize[0] * config_.outputHeight);
    memset(compositeFrame_->data[1], 128, compositeFrame_->linesize[1] * config_.outputHeight / 2);
    memset(compositeFrame_->data[2], 128, compositeFrame_->linesize[2] * config_.outputHeight / 2);

    // Composite each user's frame
    for (const auto& region : regions) {
        if (region.width < 2 || region.height < 2) continue;
        // Custom regions are opaque, including letterboxes and unavailable publishers.
        if (config_.layout == "customized") {
            for (int plane = 0; plane < 3; ++plane) {
                int divisor = plane == 0 ? 1 : 2;
                for (int y = 0; y < region.height / divisor; ++y)
                    memset(compositeFrame_->data[plane] +
                               (region.y / divisor + y) * compositeFrame_->linesize[plane] +
                               region.x / divisor,
                           plane == 0 ? 0 : 128, region.width / divisor);
            }
        }
        const std::string& userId = region.uid;
        if (std::find(usersWithFrames.begin(), usersWithFrames.end(), userId) ==
            usersWithFrames.end())
            continue;
        const VideoFrame& userFrame = frameBuffer_.at(userId).frame;

        // Calculate scaled dimensions and position
        ScaledFrameInfo scaleInfo =
            calculateScaledFrameInfo(userFrame, region.width, region.height);

        // Get or create scaling context
        std::string contextKey =
            getScalingContextKey(userId, scaleInfo.scaledWidth, scaleInfo.scaledHeight) + "_" +
            std::to_string(userFrame.width_) + "x" + std::to_string(userFrame.height_);
        SwsContext* swsContext = scalingContexts_[contextKey];

        if (!swsContext) {
            swsContext =
                sws_getContext(userFrame.width_, userFrame.height_, AV_PIX_FMT_YUV420P,
                               scaleInfo.scaledWidth, scaleInfo.scaledHeight, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);

            if (!swsContext) {
                AG_LOG_FAST(ERROR, "Failed to create scaling context for user %s", userId.c_str());
                continue;
            }

            scalingContexts_[contextKey] = swsContext;
        }

        // Create temporary scaled frame
        AVFrame* scaledFrame = av_frame_alloc();
        if (!scaledFrame) {
            continue;
        }

        scaledFrame->format = AV_PIX_FMT_YUV420P;
        scaledFrame->width = scaleInfo.scaledWidth;
        scaledFrame->height = scaleInfo.scaledHeight;

        if (av_frame_get_buffer(scaledFrame, 32) < 0) {
            av_frame_free(&scaledFrame);
            continue;
        }

        // Scale the user frame
        const uint8_t* srcData[3] = {userFrame.yData_.data(), userFrame.uData_.data(),
                                     userFrame.vData_.data()};
        int srcLinesize[3] = {userFrame.yStride_, userFrame.uStride_, userFrame.vStride_};

        sws_scale(swsContext, srcData, srcLinesize, 0, userFrame.height_, scaledFrame->data,
                  scaledFrame->linesize);

        // Calculate final position in composite
        int destX = region.x + scaleInfo.offsetX;
        int destY = region.y + scaleInfo.offsetY;

        // Copy scaled frame to composite
        for (int plane = 0; plane < 3; ++plane) {
            int planeHeight = (plane == 0) ? scaleInfo.scaledHeight : scaleInfo.scaledHeight / 2;
            int planeWidth = (plane == 0) ? scaleInfo.scaledWidth : scaleInfo.scaledWidth / 2;
            int planeDstX = (plane == 0) ? destX : destX / 2;
            int planeDstY = (plane == 0) ? destY : destY / 2;

            // Bounds checking
            int canvasPlaneWidth = (plane == 0) ? config_.outputWidth : config_.outputWidth / 2;
            int canvasPlaneHeight = (plane == 0) ? config_.outputHeight : config_.outputHeight / 2;

            if (planeDstX + planeWidth <= canvasPlaneWidth &&
                planeDstY + planeHeight <= canvasPlaneHeight) {
                for (int y = 0; y < planeHeight; ++y) {
                    memcpy(compositeFrame_->data[plane] +
                               (planeDstY + y) * compositeFrame_->linesize[plane] + planeDstX,
                           scaledFrame->data[plane] + y * scaledFrame->linesize[plane], planeWidth);
                }
            }
        }

        av_frame_free(&scaledFrame);
    }

    // Best Approach: RTC-Anchored with Monotonic Safety for Video Composition
    // Always use the most recent RTC timestamp from frame buffer for perfect A/V sync
    uint64_t latestRtcTimestamp = 0;
    for (const auto& pair : frameBuffer_) {
        latestRtcTimestamp = std::max(latestRtcTimestamp, pair.second.frame.timestamp_);
    }

    // For VideoCompositor, we use the RTC timestamp directly since it's the primary composition
    // engine The RecordingSink will handle any additional monotonic safety if needed during
    // encoding
    compositeFrame_->pts = latestRtcTimestamp;

    static int create_composite_frame_callback_log_count = 0;
    if (create_composite_frame_callback_log_count % 30 == 0) {
        AG_LOG_FAST(INFO, "Composite frame timestamp: %lu from %zu users", latestRtcTimestamp,
                    usersWithFrames.size());
    }
    create_composite_frame_callback_log_count++;

    lastCompositeTime_ = currentTime;

    // Call the callback with the composed frame
    if (videoFrameCallback_) {
        videoFrameCallback_(compositeFrame_);
    }

    return true;
}

void VideoCompositor::removeUser(const std::string& userId) {
    std::lock_guard<std::mutex> lock(frameBufferMutex_);

    frameBuffer_.erase(userId);

    // Clean up scaling contexts for this user
    auto it = scalingContexts_.begin();
    while (it != scalingContexts_.end()) {
        if (it->first.find(userId + "_") == 0) {
            if (it->second) {
                sws_freeContext(it->second);
            }
            it = scalingContexts_.erase(it);
        } else {
            ++it;
        }
    }
}

void VideoCompositor::clearAllUsers() {
    std::lock_guard<std::mutex> lock(frameBufferMutex_);
    frameBuffer_.clear();
    arrivalOrder_.clear();
    cleanupScalingContexts();
}

std::pair<int, int> VideoCompositor::calculateOptimalLayout(int numUsers) const {
    if (numUsers <= 0) return {1, 1};

    // Get canvas aspect ratio
    float canvasAspect = static_cast<float>(config_.outputWidth) / config_.outputHeight;

    // Special cases for better layouts
    switch (numUsers) {
        case 1:
            return {1, 1};
        case 2:
            return canvasAspect >= 1.5f ? std::make_pair(2, 1) : std::make_pair(1, 2);
        case 3:
            return canvasAspect >= 1.5f ? std::make_pair(3, 1) : std::make_pair(2, 2);
        case 4:
            return {2, 2};
    }

    // For more users, calculate optimal grid considering aspect ratio
    int bestCols = 1, bestRows = numUsers;
    float bestScore = std::numeric_limits<float>::max();

    for (int cols = 1; cols <= numUsers; cols++) {
        int rows = (numUsers + cols - 1) / cols;  // Ceiling division

        // Calculate cell dimensions
        float cellWidth = static_cast<float>(config_.outputWidth) / cols;
        float cellHeight = static_cast<float>(config_.outputHeight) / rows;
        float cellAspect = cellWidth / cellHeight;

        // Prefer cells close to 16:9 aspect ratio
        float aspectDiff = std::abs(cellAspect - 1.778f);

        // Penalty for empty cells
        int totalCells = cols * rows;
        int emptyCells = totalCells - numUsers;
        float wastePenalty = emptyCells * 0.1f;

        float totalScore = aspectDiff + wastePenalty;

        if (totalScore < bestScore) {
            bestScore = totalScore;
            bestCols = cols;
            bestRows = rows;
        }
    }

    return {bestCols, bestRows};
}

size_t inline VideoCompositor::getActiveUserCountWithoutLock() const {
    return frameBuffer_.size();
}

void VideoCompositor::cleanup() {
    // Set stop flag first to prevent new operations
    stopRequested_ = true;

    // Stop LayoutDetector (always present)
    if (layoutDetector_) {
        layoutDetector_->stop();
        layoutDetector_.reset();
    }

    std::lock_guard<std::mutex> lock(frameBufferMutex_);

    frameBuffer_.clear();
    arrivalOrder_.clear();
    cleanupScalingContexts();

    if (compositeFrame_) {
        av_frame_free(&compositeFrame_);
    }

    initialized_ = false;
}

bool VideoCompositor::setupCompositeFrame() {
    if (compositeFrame_) {
        av_frame_free(&compositeFrame_);
    }

    compositeFrame_ = av_frame_alloc();
    if (!compositeFrame_) {
        return false;
    }

    compositeFrame_->format = AV_PIX_FMT_YUV420P;
    compositeFrame_->width = config_.outputWidth;
    compositeFrame_->height = config_.outputHeight;

    if (av_frame_get_buffer(compositeFrame_, 32) < 0) {
        av_frame_free(&compositeFrame_);
        return false;
    }

    return true;
}

void VideoCompositor::cleanupOldFramesWithoutLock() {
    uint64_t currentTime = getCurrentTimeMs();

    auto it = frameBuffer_.begin();
    while (it != frameBuffer_.end()) {
        if (currentTime - it->second.receivedTime > config_.frameTimeoutMs) {
            AG_LOG_FAST(INFO, "Removing old frame for user %s (age: %lums)", it->first.c_str(),
                        (currentTime - it->second.receivedTime));

            // Clean up scaling contexts for this user
            auto scaleIt = scalingContexts_.begin();
            while (scaleIt != scalingContexts_.end()) {
                if (scaleIt->first.find(it->first + "_") == 0) {
                    if (scaleIt->second) {
                        sws_freeContext(scaleIt->second);
                    }
                    scaleIt = scalingContexts_.erase(scaleIt);
                } else {
                    ++scaleIt;
                }
            }

            it = frameBuffer_.erase(it);
        } else {
            ++it;
        }
    }
}

void VideoCompositor::cleanupScalingContexts() {
    for (auto& pair : scalingContexts_) {
        if (pair.second) {
            sws_freeContext(pair.second);
        }
    }
    scalingContexts_.clear();
}

uint64_t VideoCompositor::getCurrentTimeMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

VideoCompositor::ScaledFrameInfo VideoCompositor::calculateScaledFrameInfo(const VideoFrame& frame,
                                                                           int cellWidth,
                                                                           int cellHeight) const {
    ScaledFrameInfo info;

    if (config_.preserveAspectRatio) {
        float sourceAspect = static_cast<float>(frame.width_) / frame.height_;
        float cellAspect = static_cast<float>(cellWidth) / cellHeight;

        if (sourceAspect > cellAspect) {
            // Source is wider - fit to cell width
            info.scaledWidth = cellWidth;
            info.scaledHeight = static_cast<int>(cellWidth / sourceAspect);
            info.offsetX = 0;
            info.offsetY = (cellHeight - info.scaledHeight) / 2;
        } else {
            // Source is taller - fit to cell height
            info.scaledWidth = static_cast<int>(cellHeight * sourceAspect);
            info.scaledHeight = cellHeight;
            info.offsetX = (cellWidth - info.scaledWidth) / 2;
            info.offsetY = 0;
        }

        // Ensure dimensions are even (required for YUV420P)
        info.scaledWidth = std::max(2, (info.scaledWidth / 2) * 2);
        info.scaledHeight = std::max(2, (info.scaledHeight / 2) * 2);
        info.offsetX = ((cellWidth - info.scaledWidth) / 4) * 2;
        info.offsetY = ((cellHeight - info.scaledHeight) / 4) * 2;
    } else {
        // Stretch to fill cell
        info.scaledWidth = cellWidth;
        info.scaledHeight = cellHeight;
        info.offsetX = 0;
        info.offsetY = 0;
    }

    return info;
}

std::string VideoCompositor::getScalingContextKey(const std::string& userId, int width,
                                                  int height) const {
    return userId + "_" + std::to_string(width) + "x" + std::to_string(height);
}

void VideoCompositor::setExpectedUsers(const std::vector<std::string>& users) {
    layoutDetector_->setExpectedUsers(users);
}

void VideoCompositor::onLayoutChange(const std::vector<std::string>& activeUsers) {
    AG_LOG_FAST(INFO, "Layout change detected: %zu stable users", activeUsers.size());

    // Optionally trigger immediate composition when layout changes
    if (initialized_ && !activeUsers.empty()) {
        // Create composite frame with new layout
        std::lock_guard<std::mutex> lock(frameBufferMutex_);
        if (videoFrameCallback_) {
            createCompositeFrameWithoutLock(activeUsers);
        }
    }
    AG_LOG_FAST(INFO, "Layout change completed");
}

std::vector<LayoutRegion> VideoCompositor::calculateRegions(
    const std::vector<std::string>& users) const {
    if (config_.layout == "customized") {
        auto regions = config_.regions;
        std::stable_sort(regions.begin(), regions.end(),
                         [](const LayoutRegion& a, const LayoutRegion& b) { return a.z < b.z; });
        return regions;
    }
    std::vector<LayoutRegion> regions;
    const int width = config_.outputWidth, height = config_.outputHeight;
    const int count = users.size();
    if (count == 0) return regions;
    if (config_.layout == "spotlight" && count > 1) {
        if (width >= height) {
            int edge = std::max(2, (width / 8) * 2);
            regions.push_back({users[0], 0, 0, width - edge, height, 0});
            for (int i = 1; i < count; ++i) {
                int top = ((i - 1) * height / (count - 1)) & ~1;
                int bottom = (i * height / (count - 1)) & ~1;
                regions.push_back({users[i], width - edge, top, edge, bottom - top, 0});
            }
        } else {
            int edge = std::max(2, (height / 8) * 2);
            regions.push_back({users[0], 0, 0, width, height - edge, 0});
            for (int i = 1; i < count; ++i) {
                int left = ((i - 1) * width / (count - 1)) & ~1;
                int right = (i * width / (count - 1)) & ~1;
                regions.push_back({users[i], left, height - edge, right - left, edge, 0});
            }
        }
    } else {
        auto grid = calculateOptimalLayout(count);
        for (int i = 0; i < count; ++i) {
            int col = i % grid.first, row = i / grid.first;
            int left = (col * width / grid.first) & ~1;
            int top = (row * height / grid.second) & ~1;
            int right = ((col + 1) * width / grid.first) & ~1;
            int bottom = ((row + 1) * height / grid.second) & ~1;
            regions.push_back({users[i], left, top, right - left, bottom - top, 0});
        }
    }
    return regions;
}

}  // namespace rtc
}  // namespace agora
