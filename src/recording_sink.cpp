#define AG_LOG_TAG "RecordingSink"

#include "include/recording_sink.h"

#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "common/ffmpeg_utils.h"
#include "common/file_utils.h"
#include "common/log.h"
#include "include/ts_segment_manager.h"

namespace agora {
namespace rtc {

namespace fs = std::filesystem;

RecordingSink::RecordingSink() {
    // Initialize FFmpeg
    av_log_set_level(AV_LOG_ERROR);

    // Initialize performance monitoring variables
    lastPerformanceCheck_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
}

RecordingSink::~RecordingSink() {
    stop();
}

bool RecordingSink::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        AG_LOG_FAST(ERROR, "Cannot initialize while recording");
        return false;
    }

    config_ = config;

    // Create output directory if it doesn't exist
    if (!createOutputDirectory()) {
        return false;
    }

    // Initialize TS segment manager for TS format
    if (config_.format == OutputFormat::TS) {
        tsSegmentManager_ = std::make_unique<TSSegmentManager>();
        TSSegmentManager::Config tsConfig;
        tsConfig.outputDir = config_.outputDir;
        tsConfig.segmentDurationSeconds = config_.tsSegmentDurationSeconds;
        tsConfig.maxSegmentsPerSession =
            (config_.maxDurationSeconds + config_.tsSegmentDurationSeconds - 1) /
            config_.tsSegmentDurationSeconds;
        tsConfig.generatePlaylist = config_.tsGeneratePlaylist;
        tsConfig.keepIncompleteSegments = config_.tsKeepIncompleteSegments;
        tsConfig.videoWidth = config_.videoWidth;
        tsConfig.videoHeight = config_.videoHeight;
        tsConfig.videoFps = config_.videoFps;

        if (!tsSegmentManager_->initialize(tsConfig)) {
            AG_LOG_FAST(ERROR, "Failed to initialize TSSegmentManager");
            return false;
        }

        AG_LOG_FAST(INFO, "TSSegmentManager initialized for TS recording");
    }

    // Initialize MetadataManager if taskId is provided
    if (!config_.taskId.empty()) {
        metadataManager_ = std::make_unique<MetadataManager>();
        currentOutputFilePrefix_ =
            metadataManager_->generateOutputFilePrefixWithoutLock(config_.taskId);
        AG_LOG_FAST(INFO, "MetadataManager initialized for recording task: %s",
                    config_.taskId.c_str());
    }

    // Initialize VideoCompositor for composite mode
    if (config_.mode == VideoCompositor::Mode::Composite) {
        videoCompositor_ = std::make_unique<VideoCompositor>();
        VideoCompositor::Config compositorConfig;
        compositorConfig.outputWidth = config_.videoWidth;
        compositorConfig.outputHeight = config_.videoHeight;
        compositorConfig.preserveAspectRatio = true;
        compositorConfig.minCompositeIntervalMs = 1000 / config_.videoFps;  // Match video FPS

        if (!videoCompositor_->initialize(compositorConfig)) {
            AG_LOG_FAST(ERROR, "Failed to initialize VideoCompositor");
            return false;
        }

        // Set callback to receive composed frames for encoding
        videoCompositor_->setComposedVideoFrameCallback(
            [this](const AVFrame* composedFrame) { this->onComposedFrame(composedFrame); });

        AG_LOG_TS(INFO, "VideoCompositor initialized for composite mode");
    }

    return true;
}

bool RecordingSink::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        AG_LOG_FAST(WARN, "Already recording");
        return false;
    }

    stopRequested_ = false;
    isRecording_ = true;
    startTime_ = std::chrono::steady_clock::now();

    // Start TS session if using TS format
    if (config_.format == OutputFormat::TS && tsSegmentManager_) {
        if (!tsSegmentManager_->startNewSession()) {
            AG_LOG_FAST(ERROR, "Failed to start TS session");
            isRecording_ = false;
            return false;
        }
    }

    // Start metadata session if metadata manager is available
    if (metadataManager_) {
        MetadataManager::TaskSession session;
        session.taskId = config_.taskId;
        session.description = "recording_session";
        session.channel = config_.channel;
        session.users = config_.targetUsers;
        session.compositionMode = (config_.mode == VideoCompositor::Mode::Individual)
                                      ? MetadataManager::CompositionMode::Individual
                                      : MetadataManager::CompositionMode::Composite;
        session.layout = MetadataManager::Layout::Flat;  // Default, can be enhanced later
        session.width = config_.videoWidth;
        session.height = config_.videoHeight;
        session.fps = config_.videoFps;
        session.videoCodec = config_.videoCodec;
        session.audioSampleRate = config_.audioSampleRate;
        session.audioChannels = config_.audioChannels;
        session.audioCodec = config_.audioCodec;

        if (config_.format == OutputFormat::TS) {
            session.outputFormat = "ts";
            session.tsSegmentDuration = config_.tsSegmentDurationSeconds;
            session.tsGeneratePlaylist = config_.tsGeneratePlaylist;
        } else if (config_.format == OutputFormat::MP4) {
            session.outputFormat = "mp4";
        } else if (config_.format == OutputFormat::AVI) {
            session.outputFormat = "avi";
        } else if (config_.format == OutputFormat::MKV) {
            session.outputFormat = "mkv";
        }

        if (!metadataManager_->startSession(config_.taskId, session, config_.outputDir)) {
            AG_LOG_FAST(WARN, "Failed to start metadata session");
        }
    }

    // Create recording thread
    recordingThread_ = std::make_unique<std::thread>(&RecordingSink::recordingThread, this);

    AG_LOG_FAST(INFO, "Started recording in %s mode",
                (config_.mode == VideoCompositor::Mode::Composite ? "composite" : "individual"));

    return true;
}

void RecordingSink::stop() {
    AG_LOG_TS(INFO, "RecordingSink::stop() called - requesting thread shutdown");

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!isRecording_.load()) {
            AG_LOG_TS(INFO, "Already stopped, returning early");
            return;
        }

        stopRequested_ = true;

        cv_.notify_all();
        videoQueueCv_.notify_all();
        audioQueueCv_.notify_all();
    }

    // Give the recording thread a moment to see the stop flag
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    flushAllEncoders();

    if (videoCompositor_) {
        videoCompositor_->cleanup();
    }

    // End TS session if using TS format
    if (config_.format == OutputFormat::TS && tsSegmentManager_) {
        tsSegmentManager_->endCurrentSession();

        // Add final completed TS segments to metadata before ending session
        if (metadataManager_ && !config_.taskId.empty()) {
            auto completedSegments = tsSegmentManager_->getCurrentSession().segments;
            for (const auto& segment : completedSegments) {
                if (segment.isComplete) {
                    MetadataManager::FileInfo fileInfo;
                    fileInfo.filename = segment.filename;
                    fileInfo.fullPath =
                        tsSegmentManager_->getCurrentSession().sessionDir + "/" + segment.filename;
                    fileInfo.type = MetadataManager::FileType::TS;
                    // Get file size
                    struct stat st;
                    if (stat(fileInfo.fullPath.c_str(), &st) == 0) {
                        fileInfo.sizeBytes = st.st_size;
                    } else {
                        fileInfo.sizeBytes = 0;
                        AG_LOG_FAST(WARN, "Could not get file size for: %s",
                                    fileInfo.fullPath.c_str());
                    }
                    fileInfo.createdAt = std::chrono::system_clock::time_point(
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            segment.startTime.time_since_epoch()));
                    fileInfo.completedAt = std::chrono::system_clock::now();
                    fileInfo.durationSeconds = segment.duration;
                    fileInfo.isComplete = true;
                    fileInfo.segmentNumber = segment.segmentNumber;
                    fileInfo.isKeyframeAligned = true;  // TS segments are always keyframe-aligned

                    std::string outputPrefix = config_.outputDir + "/" + currentOutputFilePrefix_;
                    if (!metadataManager_->appendFileToMetadata(config_.taskId, fileInfo,
                                                                outputPrefix)) {
                        AG_LOG_FAST(WARN,
                                    "Failed to add TS segment to metadata during session end: %s",
                                    fileInfo.filename.c_str());
                    } else {
                        AG_LOG_FAST(INFO, "Added final TS segment to metadata: %s",
                                    fileInfo.filename.c_str());
                    }
                }
            }
        }
    }

    // End metadata session
    if (metadataManager_ && !config_.taskId.empty()) {
        metadataManager_->endSession(config_.taskId, "normal", config_.outputDir);
    }

    if (recordingThread_ && recordingThread_->joinable()) {
        AG_LOG_TS(INFO, "About to join recording thread...");

        recordingThread_->join();

        AG_LOG_TS(INFO, "Recording thread joined successfully");

        recordingThread_.reset();
    }

    // Ensure all frames are processed before cleanup
    AG_LOG_TS(INFO, "Processing remaining frames before cleanup...");

    processVideoFrames();
    AG_LOG_TS(INFO, "Final video frames processed");

    processAudioFrames();
    AG_LOG_TS(INFO, "Final audio frames processed");

    // Cleanup all user contexts
    AG_LOG_TS(INFO, "Starting cleanup of user contexts...");

    {
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        // Process composite context first
        if (compositeContext_) {
            AG_LOG_TS(INFO, "Cleaning up composite context...");
            cleanupEncoder("");
            compositeContext_.reset();
            AG_LOG_TS(INFO, "Composite context cleaned up");
        }

        // Process individual user contexts
        AG_LOG_FAST(INFO, "Cleaning up %zu user contexts...", userContexts_.size());

        for (auto& pair : userContexts_) {
            AG_LOG_FAST(INFO, "Cleaning up encoder for user: %s", pair.first.c_str());
            cleanupEncoder(pair.first);
            AG_LOG_TS(INFO, "Encoder cleaned up for user: %s", pair.first.c_str());
        }
        userContexts_.clear();

        // Cleanup passthrough contexts
        for (auto& pair : passthroughContexts_) {
            cleanupPassthroughContext(pair.first);
        }
        passthroughContexts_.clear();

        AG_LOG_TS(INFO, "All user contexts cleared");
    }

    // Cleanup performance caches
    AG_LOG_TS(INFO, "Cleaning up composite resources...");

    cleanupCompositeResources();

    AG_LOG_TS(INFO, "Composite resources cleaned up");

    isRecording_ = false;
    AG_LOG_TS(INFO, "Stopped recording and saved files");
}

void RecordingSink::onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                 const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                 int32_t vStride, uint32_t width, uint32_t height,
                                 uint64_t timestamp, const std::string& userId) {
    if (!isRecording()) {
        return;
    }

    // Check if this user should be recorded
    if (!shouldRecordUser(userId)) {
        return;
    }

    // Validate input parameters using shared utility
    if (!agora::common::validateYUVBuffers(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride,
                                           width, height)) {
        AG_LOG_FAST(ERROR, "YUV buffer validation failed");
        return;
    }

    VideoFrame frame;
    if (!frame.initializeFromYUV(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride, width,
                                 height, timestamp, userId)) {
        AG_LOG_FAST(ERROR, "Failed to initialize VideoFrame");
        return;
    }

    try {
        {
            std::unique_lock<std::mutex> lock(videoQueueMutex_);
            // Drop oldest frames if buffer is full to prevent unbounded memory growth
            while (videoFrameQueue_.size() >= config_.videoBufferSize) {
                videoFrameQueue_.pop();
            }
            videoFrameQueue_.push(std::move(frame));  // Use move to avoid extra copy
        }

        videoQueueCv_.notify_one();

    } catch (const std::bad_alloc& e) {
        AG_LOG_FAST(ERROR, "Memory allocation failed: %s", e.what());
        return;
    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Error processing video frame: %s", e.what());
        return;
    }
}

void RecordingSink::onAudioFrame(const uint8_t* audioBuffer, int samples, int sampleRate,
                                 int channels, uint64_t timestamp, const std::string& userId) {
    if (!isRecording()) {
        return;
    }

    // Check if this user should be recorded
    if (!shouldRecordUser(userId)) {
        return;
    }

    static int audio_log_count = 0;
    if (audio_log_count % 200 == 0) {  // Log every 50th audio frame to reduce spam
        AG_LOG_FAST(INFO, "Received audio frame: %d samples, %dHz, %d channels, user: %s", samples,
                    sampleRate, channels, userId.c_str());
    }
    audio_log_count++;

    // Validate input parameters
    if (!audioBuffer) {
        AG_LOG_FAST(ERROR, "Invalid audio buffer pointer");
        return;
    }

    if (samples <= 0 || sampleRate <= 0 || channels <= 0 || channels > 8) {
        AG_LOG_FAST(ERROR, "Invalid audio parameters: samples=%d, sampleRate=%d, channels=%d",
                    samples, sampleRate, channels);
        return;
    }

    AudioFrame frame;
    frame.sampleRate = sampleRate;
    frame.channels = channels;
    frame.timestamp = timestamp;
    frame.userId = userId;

    try {
        // Calculate buffer size with overflow protection
        size_t dataSize =
            static_cast<size_t>(samples) * static_cast<size_t>(channels) * sizeof(int16_t);

        // Sanity check: prevent excessive memory allocation (max 10MB audio frame)
        const size_t MAX_AUDIO_FRAME_SIZE = 10 * 1024 * 1024;
        if (dataSize > MAX_AUDIO_FRAME_SIZE) {
            AG_LOG_FAST(ERROR, "Audio frame size too large: %zu bytes", dataSize);
            return;
        }

        frame.data.reserve(dataSize);
        frame.data.resize(dataSize);
        std::memcpy(frame.data.data(), audioBuffer, dataSize);
        frame.valid = true;

        {
            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            // Drop oldest frames if buffer is full to prevent unbounded memory growth
            while (audioFrameQueue_.size() >= config_.audioBufferSize) {
                audioFrameQueue_.pop();
            }
            audioFrameQueue_.push(std::move(frame));  // Use move to avoid extra copy
        }

        audioQueueCv_.notify_one();

    } catch (const std::bad_alloc& e) {
        AG_LOG_FAST(ERROR, "Audio memory allocation failed: %s", e.what());
        return;
    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Error processing audio frame: %s", e.what());
        return;
    }
}

void RecordingSink::recordingThread() {
    AG_LOG_FAST(INFO, "Recording thread started");

    while (!stopRequested_.load()) {
        // Process video frames
        if (config_.recordVideo) {
            processVideoFrames();
        }

        // Process audio frames
        if (config_.recordAudio) {
            processAudioFrames();
        }

        // Check for TS segment rotation
        if (config_.format == OutputFormat::TS && tsSegmentManager_) {
            checkAndRotateSegmentIfNeeded();
        }

        // Check for timeout
        auto elapsed = std::chrono::steady_clock::now() - startTime_;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
            config_.maxDurationSeconds) {
            AG_LOG_FAST(INFO, "Max duration reached, breaking from recording loop");

            // Notify task completion when max duration is reached
            if (completionCallback_ && !config_.taskId.empty()) {
                AG_LOG_FAST(INFO, "Notifying task completion for max duration reached: %s",
                            config_.taskId.c_str());
                completionCallback_(config_.taskId, "success",
                                    "Recording completed - max duration reached");
            }

            break;
        }

        // std::cout
        //     << "[RecordingSink] Recording thread loop iteration completed, waiting with
        //     timeout..."
        //     << std::endl;
        // std::flush(std::cout);

        // Use condition variable with timeout instead of sleep_for
        // This allows the thread to be woken up immediately when stop is requested
        std::unique_lock<std::mutex> lock(mutex_);
        bool shouldStop = cv_.wait_for(lock, std::chrono::milliseconds(10),
                                       [this] { return stopRequested_.load(); });

        // If we were woken up due to stop request, exit immediately
        if (shouldStop || stopRequested_.load()) {
            AG_LOG_TS(INFO, "Stop condition detected after wait, breaking from loop");
            break;
        }
    }
}

void RecordingSink::processVideoFrames() {
    // Early exit if stop is requested - don't even try to process frames
    if (stopRequested_.load()) {
        AG_LOG_FAST(INFO,
                    "processVideoFrames() - stopRequested detected at entry, exiting immediately");
        return;
    }

    std::unique_lock<std::mutex> lock(videoQueueMutex_);

    static int process_log_count = 0;
    if (process_log_count % 600 == 0) {  // Log every 600th encode to reduce spam
        AG_LOG_FAST(INFO, "processVideoFrames() - queue size: %zu", videoFrameQueue_.size());
    }
    process_log_count++;

    while (!videoFrameQueue_.empty()) {
        // Check stop condition at the beginning of each iteration
        if (stopRequested_.load()) {
            AG_LOG_FAST(INFO, "processVideoFrames() - stopRequested detected, breaking");
            break;
        }

        VideoFrame frame = videoFrameQueue_.front();
        videoFrameQueue_.pop();
        lock.unlock();

        if (frame.valid() && !stopRequested_.load()) {
            encodeVideoFrame(frame, frame.userId());
        }

        lock.lock();

        // Check stop again after potentially blocking encode operation
        if (stopRequested_.load()) {
            AG_LOG_FAST(INFO, "processVideoFrames() - stopRequested after encode, breaking");
            break;
        }
    }
}

void RecordingSink::processAudioFrames() {
    // Early exit if stop is requested - don't even try to process frames
    if (stopRequested_.load()) {
        // std::cout << "[RecordingSink] processAudioFrames() - stopRequested detected at entry, "
        //              "exiting immediately"
        //           << std::endl;
        // std::flush(std::cout);
        return;
    }

    std::unique_lock<std::mutex> lock(audioQueueMutex_);

    // std::cout << "[RecordingSink] processAudioFrames() - queue size: " << audioFrameQueue_.size()
    //           << std::endl;
    // std::flush(std::cout);

    while (!audioFrameQueue_.empty()) {
        // Check stop condition at the beginning of each iteration
        if (stopRequested_.load()) {
            // std::cout << "[RecordingSink] processAudioFrames() - stopRequested detected,
            // breaking"
            // << std::endl;
            // std::flush(std::cout);
            break;
        }

        AudioFrame frame = audioFrameQueue_.front();
        audioFrameQueue_.pop();
        lock.unlock();

        // std::cout << "[RecordingSink] processAudioFrames() - about to encode frame" << std::endl;
        // std::flush(std::cout);

        if (frame.valid && !stopRequested_.load()) {
            encodeAudioFrame(frame, frame.userId);
        }

        // std::cout << "[RecordingSink] processAudioFrames() - frame encoded, checking stop"
        //           << std::endl;
        // std::flush(std::cout);

        lock.lock();

        // Check stop again after potentially blocking encode operation
        if (stopRequested_.load()) {
            AG_LOG_FAST(INFO, "processAudioFrames() - stopRequested after encode, breaking");
            break;
        }
    }

    // std::cout << "[RecordingSink] processAudioFrames() - exiting" << std::endl;
    // std::flush(std::cout);
}

bool RecordingSink::initializeEncoder(const std::string& userId) {
    std::unique_ptr<UserContext> context = std::make_unique<UserContext>();
    context->startTime = std::chrono::steady_clock::now();

    // For TS format, use segment manager to get filename
    if (config_.format == OutputFormat::TS && tsSegmentManager_) {
        // Always create first segment when initializing encoder
        if (!tsSegmentManager_->rotateToNewSegment()) {
            AG_LOG_FAST(ERROR, "Failed to create initial TS segment");
            return false;
        }
        context->filename = tsSegmentManager_->getCurrentTempSegmentPath();
        AG_LOG_FAST(INFO, "Created initial TS segment: %s", context->filename.c_str());
    } else {
        context->filename = generateOutputFilename(userId);
    }

    // Setup output format
    if (!setupOutputFormat(&context->formatContext, context->filename)) {
        AG_LOG_FAST(ERROR, "Failed to setup output format for user %s", userId.c_str());
        return false;
    }

    // Setup video encoder if enabled
    if (config_.recordVideo) {
        if (!setupVideoEncoder(&context->videoCodecContext, userId)) {
            AG_LOG_FAST(ERROR, "Failed to setup video encoder for user %s", userId.c_str());
            return false;
        }

        context->videoStream =
            avformat_new_stream(context->formatContext, context->videoCodecContext->codec);
        if (!context->videoStream) {
            AG_LOG_FAST(ERROR, "Failed to create video stream for user %s", userId.c_str());
            return false;
        }

        avcodec_parameters_from_context(context->videoStream->codecpar, context->videoCodecContext);
        // Set stream time base to codec time base - MP4 will still override but this helps
        context->videoStream->time_base = context->videoCodecContext->time_base;
        context->videoStream->avg_frame_rate = {config_.videoFps, 1};
        context->videoStream->r_frame_rate = {config_.videoFps, 1};

        // Allocate video frame
        context->videoFrame = av_frame_alloc();
        if (!context->videoFrame) {
            AG_LOG_FAST(ERROR, "Failed to allocate video frame for user %s", userId.c_str());
            return false;
        }

        context->videoFrame->format = context->videoCodecContext->pix_fmt;
        context->videoFrame->width = context->videoCodecContext->width;
        context->videoFrame->height = context->videoCodecContext->height;

        if (av_frame_get_buffer(context->videoFrame, 32) < 0) {
            AG_LOG_FAST(ERROR, "Failed to allocate video frame buffer for user %s", userId.c_str());
            return false;
        }

        // Note: Scaling context will be created dynamically when we know actual frame dimensions
        context->swsContext = nullptr;
    }

    // Setup audio encoder if enabled
    if (config_.recordAudio) {
        AG_LOG_FAST(INFO, "Setting up audio encoder for user %s", userId.c_str());
        if (!setupAudioEncoder(&context->audioCodecContext, userId)) {
            AG_LOG_FAST(ERROR, "Failed to setup audio encoder for user %s", userId.c_str());
            return false;
        }
        AG_LOG_FAST(INFO, "Audio encoder setup successful for user %s", userId.c_str());

        context->audioStream =
            avformat_new_stream(context->formatContext, context->audioCodecContext->codec);
        if (!context->audioStream) {
            AG_LOG_FAST(ERROR, "Failed to create audio stream for user %s", userId.c_str());
            return false;
        }

        avcodec_parameters_from_context(context->audioStream->codecpar, context->audioCodecContext);
        context->audioStream->time_base = context->audioCodecContext->time_base;

        // Allocate audio frame for output
        context->audioFrame = av_frame_alloc();
        if (!context->audioFrame) {
            AG_LOG_FAST(ERROR, "Failed to allocate audio frame for user %s", userId.c_str());
            return false;
        }

        // Initialize audio resampling context for format conversion
        // We'll create this dynamically when we receive the first audio frame
        context->swrContext = nullptr;
    }

    // Write header
    if (avformat_write_header(context->formatContext, nullptr) < 0) {
        AG_LOG_FAST(ERROR, "Failed to write header for user %s", userId.c_str());
        return false;
    }

    // Log the actual time bases after header is written
    AG_LOG_FAST(INFO, "Stream time base: %d/%d, Codec time base: %d/%d",
                context->videoStream->time_base.num, context->videoStream->time_base.den,
                context->videoCodecContext->time_base.num,
                context->videoCodecContext->time_base.den);

    // Save filename before moving context
    std::string filename = context->filename;

    // Store context (mutex already held by caller)
    if (config_.mode == VideoCompositor::Mode::Individual) {
        userContexts_[userId] = std::move(context);
    } else {
        compositeContext_ = std::move(context);
    }

    AG_LOG_FAST(INFO, "Initialized encoder for user %s, output file: %s", userId.c_str(),
                filename.c_str());

    return true;
}

bool RecordingSink::setupOutputFormat(AVFormatContext** formatContext,
                                      const std::string& filename) {
    const char* format_name = nullptr;

    // Determine format based on extension or config
    if (config_.format == OutputFormat::TS) {
        format_name = "mpegts";
    }

    if (avformat_alloc_output_context2(formatContext, nullptr, format_name, filename.c_str()) < 0) {
        return false;
    }

    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            return false;
        }
    }

    return true;
}

bool RecordingSink::setupVideoEncoder(AVCodecContext** videoCodecContext,
                                      const std::string& userId) {
    // Try hardware-accelerated encoders first, then fall back to software
    struct EncoderCandidate {
        const char* name;
        AVPixelFormat pixFmt;
        bool isHwAccel;
    };

    std::vector<EncoderCandidate> candidates;

    if (config_.videoCodec == "libx264" || config_.videoCodec == "h264") {
        candidates = {
            {"h264_nvenc", AV_PIX_FMT_YUV420P, true},  // NVIDIA GPU
            {"h264_vaapi", AV_PIX_FMT_VAAPI, true},    // Intel/AMD VA-API
            {"h264_qsv", AV_PIX_FMT_NV12, true},       // Intel Quick Sync
            {"libx264", AV_PIX_FMT_YUV420P, false},    // Software fallback
        };
    } else {
        // Non-H264 codec: use as-is without hwaccel probing
        candidates = {{config_.videoCodec.c_str(), AV_PIX_FMT_YUV420P, false}};
    }

    const AVCodec* codec = nullptr;
    AVPixelFormat selectedPixFmt = AV_PIX_FMT_YUV420P;
    bool usingHwAccel = false;

    for (const auto& candidate : candidates) {
        codec = avcodec_find_encoder_by_name(candidate.name);
        if (!codec) continue;

        // For hardware encoders, do a quick open test to verify the device is available
        if (candidate.isHwAccel) {
            AVCodecContext* testCtx = avcodec_alloc_context3(codec);
            if (!testCtx) continue;
            testCtx->width = config_.videoWidth;
            testCtx->height = config_.videoHeight;
            testCtx->time_base = {1, 90000};
            testCtx->pix_fmt = candidate.pixFmt;
            testCtx->bit_rate = config_.videoBitrate;

            AVDictionary* testOpts = nullptr;
            av_dict_set(&testOpts, "preset", "fast", 0);
            int ret = avcodec_open2(testCtx, codec, &testOpts);
            av_dict_free(&testOpts);
            avcodec_free_context(&testCtx);

            if (ret < 0) {
                AG_LOG_FAST(INFO, "HW encoder %s not available, trying next", candidate.name);
                continue;
            }
        }

        selectedPixFmt = candidate.pixFmt;
        usingHwAccel = candidate.isHwAccel;
        AG_LOG_FAST(INFO, "Selected video encoder: %s%s", candidate.name,
                    usingHwAccel ? " (hardware accelerated)" : " (software)");
        break;
    }

    if (!codec) {
        AG_LOG_FAST(ERROR, "No suitable video encoder found");
        return false;
    }

    *videoCodecContext = avcodec_alloc_context3(codec);
    if (!*videoCodecContext) {
        AG_LOG_FAST(ERROR, "Failed to allocate video codec context");
        return false;
    }

    (*videoCodecContext)->bit_rate = config_.videoBitrate;
    (*videoCodecContext)->width = config_.videoWidth;
    (*videoCodecContext)->height = config_.videoHeight;
    (*videoCodecContext)->time_base = {1, 90000};
    (*videoCodecContext)->framerate = {config_.videoFps, 1};
    (*videoCodecContext)->gop_size = config_.videoFps;
    (*videoCodecContext)->max_b_frames = 0;
    (*videoCodecContext)->pix_fmt = selectedPixFmt;
    (*videoCodecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    (*videoCodecContext)->thread_count = 1;
    (*videoCodecContext)->thread_type = FF_THREAD_SLICE;

    AVDictionary* opts = nullptr;
    if (usingHwAccel) {
        av_dict_set(&opts, "preset", "fast", 0);
    } else {
        av_dict_set(&opts, "preset", "fast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "x264-params", "force-cfr=1", 0);
    }
    av_dict_set(&opts, "fflags", "+flush_packets", 0);

    if (avcodec_open2(*videoCodecContext, codec, &opts) < 0) {
        AG_LOG_FAST(ERROR, "Failed to open video codec: %s", codec->name);
        av_dict_free(&opts);
        return false;
    }

    av_dict_free(&opts);
    return true;
}

bool RecordingSink::setupAudioEncoder(AVCodecContext** audioCodecContext,
                                      const std::string& userId) {
    const AVCodec* codec = avcodec_find_encoder_by_name(config_.audioCodec.c_str());
    if (!codec) {
        AG_LOG_FAST(ERROR, "Audio codec not found: %s", config_.audioCodec.c_str());
        return false;
    }

    *audioCodecContext = avcodec_alloc_context3(codec);
    if (!*audioCodecContext) {
        return false;
    }

    (*audioCodecContext)->bit_rate = config_.audioBitrate;
    (*audioCodecContext)->sample_fmt = AV_SAMPLE_FMT_FLTP;
    // Use the target audio parameters from config (48kHz stereo)
    (*audioCodecContext)->sample_rate = config_.audioSampleRate;
    av_channel_layout_default(&(*audioCodecContext)->ch_layout, config_.audioChannels);
    // Modern best practice time base for audio: 1/90000 (MPEG standard)
    // This provides high precision while avoiding overflow and is MP4/H.264 standard
    (*audioCodecContext)->time_base = {1, 90000};

    if (avcodec_open2(*audioCodecContext, codec, nullptr) < 0) {
        return false;
    }

    return true;
}

bool RecordingSink::encodeVideoFrame(const VideoFrame& frame, const std::string& userId) {
    // Check if stop was requested before doing expensive video encoding
    if (stopRequested_.load()) {
        // std::cout << "[RecordingSink] encodeVideoFrame() - stopRequested detected, returning
        // early"
        //           << std::endl;
        // std::flush(std::cout);
        return false;
    }

    // std::cout << "[RecordingSink] encodeVideoFrame() - stopRequested check passed" << std::endl;
    // std::flush(std::cout);

    if (config_.mode == VideoCompositor::Mode::Individual) {
        // std::cout << "[RecordingSink] encodeVideoFrame() - using INDIVIDUAL mode" << std::endl;
        // std::flush(std::cout);
        // Individual mode - encode each user separately
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        auto it = userContexts_.find(userId);
        if (it == userContexts_.end()) {
            // Initialize encoder for new user
            if (!initializeEncoder(userId)) {
                return false;
            }
            it = userContexts_.find(userId);
        }

        return encodeIndividualFrame(frame, it->second.get());
    } else {
        // Composite mode - update composite buffer and potentially create composite frame
        bool result = updateCompositeFrame(frame, userId);
        return result;
    }
}

bool RecordingSink::encodeIndividualFrame(const VideoFrame& frame, UserContext* context) {
    if (!context || !context->videoCodecContext || !context->videoFrame) {
        return false;
    }

    // Create scaling context dynamically if not already created or if frame dimensions changed
    if (!context->swsContext) {
        context->swsContext = sws_getContext(
            frame.width(), frame.height(),
            AV_PIX_FMT_YUV420P,  // Use actual incoming frame dimensions
            context->videoCodecContext->width, context->videoCodecContext->height,
            context->videoCodecContext->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!context->swsContext) {
            AG_LOG_FAST(ERROR, "Failed to create scaling context for frame %ux%u -> %dx%d",
                        frame.width(), frame.height(), context->videoCodecContext->width,
                        context->videoCodecContext->height);
            return false;
        }
    }

    // Convert VideoFrame to AVFrame for processing
    AVFrame* srcFrame = frame.toAVFrame();
    if (!srcFrame) {
        AG_LOG_FAST(ERROR, "Failed to convert VideoFrame to AVFrame");
        return false;
    }

    // Check if input resolution has changed, need to recreate swsContext
    // Input resolution tracking fields are added to UserContext
    if (context->swsContext && (srcFrame->width != context->lastInputWidth ||
                                srcFrame->height != context->lastInputHeight)) {
        AG_LOG_FAST(INFO, "Input resolution changed: %dx%d -> %dx%d, rebuilding swsContext",
                    context->lastInputWidth, context->lastInputHeight, srcFrame->width,
                    srcFrame->height);

        sws_freeContext(context->swsContext);
        context->swsContext = nullptr;
    }

    // If swsContext doesn't exist or input size changed, recreate it
    if (!context->swsContext) {
        context->swsContext = sws_getContext(
            srcFrame->width, srcFrame->height, (AVPixelFormat)srcFrame->format,
            context->videoCodecContext->width, context->videoCodecContext->height,
            context->videoCodecContext->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);

        if (!context->swsContext) {
            AG_LOG_FAST(ERROR, "Failed to create swsContext for %dx%d -> %dx%d", srcFrame->width,
                        srcFrame->height, context->videoCodecContext->width,
                        context->videoCodecContext->height);
            av_frame_free(&srcFrame);
            return false;
        }

        AG_LOG_FAST(INFO, "Successfully created swsContext: %dx%d -> %dx%d", srcFrame->width,
                    srcFrame->height, context->videoCodecContext->width,
                    context->videoCodecContext->height);

        // Record current input resolution
        context->lastInputWidth = srcFrame->width;
        context->lastInputHeight = srcFrame->height;
    }

    // Ensure frame is writable before scaling
    if (av_frame_make_writable(context->videoFrame) < 0) {
        AG_LOG_FAST(ERROR, "Failed to make frame writable");
        av_frame_free(&srcFrame);
        return false;
    }

    // Safe scaling call, check parameter validity
    if (srcFrame->height <= 0 || context->videoFrame->height <= 0) {
        AG_LOG_FAST(ERROR, "Invalid frame height: src=%d, dst=%d", srcFrame->height,
                    context->videoFrame->height);
        av_frame_free(&srcFrame);
        return false;
    }

    int swsRet =
        sws_scale(context->swsContext, srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
                  context->videoFrame->data, context->videoFrame->linesize);

    if (swsRet < 0) {
        AG_LOG_FAST(ERROR, "sws_scale failed: %d", swsRet);
        av_frame_free(&srcFrame);
        return false;
    }

    av_frame_free(&srcFrame);

    // Update stream state and detect restart
    updateStreamState(context, true, frame.timestamp());
    if (detectStreamRestart(context, true, frame.timestamp())) {
        // Stream restart: reset time origin for both formats
        context->hasTimeOrigin = false;

        // Format-specific PTS handling
        if (config_.format == OutputFormat::TS) {
            // For TS: maintain PTS continuity, don't reset to -1
            AG_LOG_FAST(WARN,
                        "Detected video stream restart, resetting time origin (TS: maintaining PTS "
                        "continuity)");
        } else {
            // For MP4/AVI/MKV: can safely reset PTS
            context->lastVideoPts = -1;
            AG_LOG_FAST(WARN, "Detected video stream restart, resetting time origin and PTS");
        }
    }

    // Use new unified PTS calculation method
    int64_t pts = calculateVideoPTS(context, frame.timestamp());

    // Convert to encoder's time base
    context->videoFrame->pts = av_rescale_q(pts, {1, 90000}, context->videoCodecContext->time_base);

    context->videoFrameCount++;

    // Encode frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    AG_LOG_FAST(INFO, "About to call avcodec_send_frame()");

    int sendRet = avcodec_send_frame(context->videoCodecContext, context->videoFrame);

    AG_LOG_FAST(INFO, "avcodec_send_frame() returned: %d", sendRet);

    if (sendRet < 0) {
        av_packet_free(&packet);
        return false;
    }

    AG_LOG_FAST(INFO, "Entering avcodec_receive_packet() loop");

    while (sendRet >= 0) {
        // Check if stop was requested during encoding loop
        if (stopRequested_.load()) {
            AG_LOG_FAST(
                INFO,
                "encodeIndividualFrame() - stopRequested detected in encoding loop, breaking");
            av_packet_free(&packet);
            return false;
        }

        AG_LOG_FAST(INFO, "About to call avcodec_receive_packet()");

        int recRet = avcodec_receive_packet(context->videoCodecContext, packet);

        AG_LOG_FAST(INFO, "avcodec_receive_packet() returned: %d", recRet);

        if (recRet == AVERROR(EAGAIN) || recRet == AVERROR_EOF) {
            break;
        } else if (recRet < 0) {
            av_packet_free(&packet);
            return false;
        }

        // Set the stream index
        packet->stream_index = context->videoStream->index;

        // Video PTS already correctly set in frame stage, just need to convert time base
        if (packet->pts != AV_NOPTS_VALUE) {
            // Convert encoder time base to stream time base
            packet->pts = av_rescale_q(packet->pts, context->videoCodecContext->time_base,
                                       context->videoStream->time_base);
        }

        // DTS = PTS (no B-frame reordering)
        packet->dts = packet->pts;

        // Debug: Log PTS/DTS values
        static int rescale_log_count = 0;
        if (rescale_log_count % 30 == 0) {
            AG_LOG_FAST(INFO, "Video packet - PTS: %ld, DTS: %ld, frame: %lu", packet->pts,
                        packet->dts, context->videoFrameCount);
        }
        rescale_log_count++;

        // Set packet duration for proper MP4 timing
        if (packet->duration <= 0) {
            packet->duration =
                av_rescale_q(1, (AVRational){1, config_.videoFps}, context->videoStream->time_base);
        }

        // Capture PTS value before writing (muxer may modify packet)
        int64_t original_pts = packet->pts;

        // TS-specific: Detect keyframes and handle segment rotation (only for TS format)
        if (config_.format == OutputFormat::TS) {
            if (detectKeyframe(packet, context)) {
                // Check if we need to rotate segments after keyframe detection
                if (tsPendingSegmentRotation_.load() && tsSegmentManager_) {
                    if (switchToNewTSSegment(context)) {
                        tsPendingSegmentRotation_ = false;
                        AG_LOG_FAST(INFO, "Successfully rotated to new TS segment on keyframe");
                    }
                }
            }
        }

        // Write packet - use av_interleaved_write_frame for proper timestamp ordering
        int write_ret = av_interleaved_write_frame(context->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            AG_LOG_FAST(ERROR, "Failed to write video frame: %s", errbuf);
        } else {
            static int video_log_count = 0;
            if (video_log_count % 30 == 0) {  // Log every 30th frame to reduce spam
                AG_LOG_FAST(INFO, "Successfully wrote video packet %d, pts: %ld", video_log_count,
                            original_pts);
            }
            video_log_count++;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return true;
}

bool RecordingSink::encodeAudioFrame(const AudioFrame& frame, const std::string& userId) {
    static int encode_log_count = 0;
    if (encode_log_count % 50 == 0) {  // Log every 50th encode to reduce spam
        AG_LOG_FAST(INFO,
                    "Encoding audio frame for user: %s, input: %dHz, %d channels, target: %dHz, %d "
                    "channels",
                    userId.c_str(), frame.sampleRate, frame.channels, config_.audioSampleRate,
                    config_.audioChannels);
    }
    encode_log_count++;

    if (config_.mode == VideoCompositor::Mode::Individual) {
        // In passthrough mode, route audio to passthrough context (if it exists)
        if (config_.videoDecodeMode == 0) {
            auto it = passthroughContexts_.find(userId);
            if (it != passthroughContexts_.end()) {
                return encodePassthroughAudioFrame(frame, it->second.get(), userId);
            }
            // Passthrough context not yet created (waiting for first video frame) — drop audio
            return true;
        }

        // Individual mode - encode each user separately
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        auto it = userContexts_.find(userId);
        if (it == userContexts_.end()) {
            // Initialize encoder for new user if not already done
            if (!initializeEncoder(userId)) {
                return false;
            }
            it = userContexts_.find(userId);
        }
        UserContext* context = it->second.get();
        return encodeIndividualAudioFrame(frame, context, userId);
    } else {
        // Composite mode - mix audio from multiple users
        // std::cout << "[RecordingSink] Composite mode: mixing audio from user " << userId
        //           << std::endl;
        return mixAudioFromMultipleUsers(frame, userId);
    }
}

bool RecordingSink::encodeIndividualAudioFrame(const AudioFrame& frame, UserContext* context,
                                               const std::string& userId) {
    if (!context || !context->audioCodecContext || !context->audioFrame) {
        return false;
    }

    // Initialize resampling context if needed (when format doesn't match)
    bool needsResampling =
        (frame.sampleRate != config_.audioSampleRate || frame.channels != config_.audioChannels);

    // std::cout << "[RecordingSink] Audio format check: input=" << frame.sampleRate << "Hz/"
    //           << frame.channels << "ch, target=" << config_.audioSampleRate << "Hz/"
    //           << config_.audioChannels << "ch, needsResampling=" << (needsResampling ? "YES" :
    //           "NO")
    //           << std::endl;

    if (needsResampling && !context->swrContext) {
        // Create resampling context to convert input format to target format
        AVChannelLayout in_ch_layout, out_ch_layout;
        av_channel_layout_default(&in_ch_layout, frame.channels);
        av_channel_layout_default(&out_ch_layout, config_.audioChannels);

        int ret = swr_alloc_set_opts2(&context->swrContext, &out_ch_layout, AV_SAMPLE_FMT_FLTP,
                                      config_.audioSampleRate,                             // output
                                      &in_ch_layout, AV_SAMPLE_FMT_S16, frame.sampleRate,  // input
                                      0, nullptr);

        if (ret < 0 || !context->swrContext) {
            AG_LOG_FAST(ERROR, "Failed to allocate resampling context");
            return false;
        }

        ret = swr_init(context->swrContext);
        if (ret < 0) {
            AG_LOG_FAST(ERROR, "Failed to initialize resampling context");
            swr_free(&context->swrContext);
            return false;
        }

        AG_LOG_FAST(INFO, "Initialized audio resampler: %dHz %dch -> %dHz %dch", frame.sampleRate,
                    frame.channels, config_.audioSampleRate, config_.audioChannels);
    }

    // Calculate input samples from the frame data
    int input_samples = frame.data.size() / sizeof(int16_t) / frame.channels;
    const int16_t* input_data = reinterpret_cast<const int16_t*>(frame.data.data());

    int samples_per_frame = context->audioCodecContext->frame_size;
    if (samples_per_frame == 0) {
        samples_per_frame = 1024;  // Default frame size for AAC
    }

    // Add incoming samples to buffer
    size_t current_buffer_size = context->audioSampleBuffer.size();
    size_t new_samples_count = input_samples * frame.channels;
    context->audioSampleBuffer.resize(current_buffer_size + new_samples_count);

    // Copy new samples to buffer
    std::memcpy(context->audioSampleBuffer.data() + current_buffer_size, input_data,
                new_samples_count * sizeof(int16_t));

    // Update buffered timestamp (use latest frame timestamp)
    context->lastBufferedTimestamp = frame.timestamp;

    // Check if we have enough samples for AAC encoding (1024 samples per channel)
    size_t required_samples = samples_per_frame * frame.channels;  // e.g., 1024 * 1 = 1024 for mono
    if (context->audioSampleBuffer.size() < required_samples) {
        // Not enough samples yet, return and wait for more
        static int buffer_log_count = 0;
        if (buffer_log_count % 50 == 0) {
            AG_LOG_FAST(INFO, "Buffering audio: %zu/%zu samples", context->audioSampleBuffer.size(),
                        required_samples);
        }
        buffer_log_count++;
        return true;  // Successfully buffered, but not ready to encode yet
    }

    // std::cout << "[RecordingSink] Ready to encode: " << context->audioSampleBuffer.size()
    //           << " samples (need " << required_samples << ")" << std::endl;

    // Update stream state and detect restart
    updateStreamState(context, false, frame.timestamp);
    if (detectStreamRestart(context, false, frame.timestamp)) {
        // Stream restart: reset time origin for both formats
        context->hasTimeOrigin = false;

        // Format-specific PTS handling
        if (config_.format == OutputFormat::TS) {
            // For TS: maintain PTS continuity, don't reset to -1
            AG_LOG_FAST(WARN,
                        "Detected audio stream restart, resetting time origin (TS: maintaining PTS "
                        "continuity)");
        } else {
            // For MP4/AVI/MKV: can safely reset PTS
            context->lastAudioPts = -1;
            AG_LOG_FAST(WARN, "Detected audio stream restart, resetting time origin and PTS");
        }
    }

    // Set up audio frame properties for output
    context->audioFrame->nb_samples = samples_per_frame;
    context->audioFrame->format = context->audioCodecContext->sample_fmt;
    context->audioFrame->sample_rate = context->audioCodecContext->sample_rate;
    av_channel_layout_copy(&context->audioFrame->ch_layout, &context->audioCodecContext->ch_layout);

    // Use new unified PTS calculation method
    int64_t pts = calculateAudioPTS(context, frame.timestamp);

    // Convert to encoder's time base
    context->audioFrame->pts = av_rescale_q(pts, {1, 90000}, context->audioCodecContext->time_base);
    context->lastBufferedTimestamp = frame.timestamp;  // Track for debugging

    // std::cout << "[RecordingSink] Individual audio PTS: " << pts
    //           << " from timestamp: " << frame.timestamp << std::endl;

    // Allocate buffer for audio frame
    if (av_frame_get_buffer(context->audioFrame, 0) < 0) {
        AG_LOG_FAST(ERROR, "Failed to make audio frame writable");
        return false;
    }

    // Process exactly the required number of samples for AAC
    size_t samples_to_encode = samples_per_frame;  // 1024 samples for AAC

    if (needsResampling && context->swrContext) {
        // Use resampling to convert format from buffered samples
        const uint8_t* input[1] = {
            reinterpret_cast<const uint8_t*>(context->audioSampleBuffer.data())};

        // Calculate how many input samples we're using (need to account for channels)
        int input_samples_for_resampling =
            samples_to_encode * frame.channels / config_.audioChannels;

        int output_samples = swr_convert(context->swrContext, context->audioFrame->data,
                                         samples_per_frame, input, input_samples_for_resampling);

        if (output_samples < 0) {
            AG_LOG_FAST(ERROR, "Audio resampling failed");
            return false;
        }

        // Update the actual number of samples produced
        context->audioFrame->nb_samples = output_samples;

        static int resample_log_count = 0;
        if (resample_log_count % 100 == 0) {
            AG_LOG_FAST(INFO, "Resampled %d -> %d samples", input_samples_for_resampling,
                        output_samples);
        }
        resample_log_count++;

    } else {
        // Direct conversion without resampling (when formats match)
        if (context->audioCodecContext->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            // Convert to planar float format using buffered samples
            const int16_t* buffer_data = context->audioSampleBuffer.data();

            if (frame.channels == config_.audioChannels) {
                // Channels match - direct conversion
                for (int ch = 0; ch < config_.audioChannels; ch++) {
                    float* output_channel = (float*)context->audioFrame->data[ch];
                    for (int i = 0; i < samples_per_frame; i++) {
                        int16_t sample = buffer_data[i * frame.channels + ch];
                        output_channel[i] = static_cast<float>(sample) / 32768.0f;
                    }
                }
            } else {
                // Channel conversion needed (mono to stereo or vice versa)
                for (int ch = 0; ch < config_.audioChannels; ch++) {
                    float* output_channel = (float*)context->audioFrame->data[ch];
                    for (int i = 0; i < samples_per_frame; i++) {
                        int16_t sample;
                        if (frame.channels == 1) {
                            // Mono to stereo - duplicate channel
                            sample = buffer_data[i];
                        } else {
                            // Multi-channel to fewer channels - take first channel or mix
                            sample =
                                buffer_data[i * frame.channels + std::min(ch, frame.channels - 1)];
                        }
                        output_channel[i] = static_cast<float>(sample) / 32768.0f;
                    }
                }
            }

            context->audioFrame->nb_samples = samples_per_frame;
        }
    }

    // Remove processed samples from buffer
    size_t samples_consumed = samples_to_encode * frame.channels;
    if (context->audioSampleBuffer.size() > samples_consumed) {
        // Move remaining samples to beginning of buffer
        std::memmove(context->audioSampleBuffer.data(),
                     context->audioSampleBuffer.data() + samples_consumed,
                     (context->audioSampleBuffer.size() - samples_consumed) * sizeof(int16_t));
        context->audioSampleBuffer.resize(context->audioSampleBuffer.size() - samples_consumed);
    } else {
        // Used all samples
        context->audioSampleBuffer.clear();
    }

    context->audioFrameCount++;

    // Encode audio frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    int ret = avcodec_send_frame(context->audioCodecContext, context->audioFrame);
    if (ret < 0) {
        av_packet_free(&packet);
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(context->audioCodecContext, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            return false;
        }

        // Set proper timestamps for audio packet
        packet->stream_index = context->audioStream->index;

        // Audio PTS already correctly set in frame stage, just need to convert time base
        if (packet->pts != AV_NOPTS_VALUE) {
            // Convert encoder time base to stream time base
            packet->pts = av_rescale_q(packet->pts, context->audioCodecContext->time_base,
                                       context->audioStream->time_base);
        }

        // Audio DTS = PTS (no reordering)
        packet->dts = packet->pts;

        // Debug: Log audio packet info
        static int audio_rescale_log_count = 0;
        if (audio_rescale_log_count % 50 == 0) {
            AG_LOG_FAST(INFO, "Audio packet - PTS: %ld, DTS: %ld, frame: %lu", packet->pts,
                        packet->dts, context->audioFrameCount);
        }
        audio_rescale_log_count++;

        // Capture PTS value before writing (muxer may modify packet)
        int64_t original_pts = packet->pts;

        // Write packet using interleaved write for proper timestamp ordering
        int write_ret = av_interleaved_write_frame(context->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            AG_LOG_FAST(ERROR, "Failed to write audio frame: %s", errbuf);
        } else {
            static int audio_write_log_count = 0;
            if (audio_write_log_count % 50 == 0) {
                AG_LOG_FAST(INFO, "Successfully wrote audio packet %d, pts: %ld",
                            audio_write_log_count, original_pts);
            }
            audio_write_log_count++;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return true;
}

bool RecordingSink::mixAudioFromMultipleUsers(const AudioFrame& frame, const std::string& userId) {
    // std::cout << "[RecordingSink] mixAudioFromMultipleUsers called for user " << userId << " with
    // "
    //           << frame.data.size() << " bytes" << std::endl;

    // Convert audio frame to float format for mixing
    std::vector<float> audioSamples;
    int input_samples = frame.data.size() / sizeof(int16_t) / frame.channels;
    const int16_t* input_data = reinterpret_cast<const int16_t*>(frame.data.data());

    // Convert to float and normalize
    audioSamples.resize(input_samples * frame.channels);
    for (int i = 0; i < input_samples * frame.channels; i++) {
        audioSamples[i] = static_cast<float>(input_data[i]) / 32768.0f;
    }

    // Store user's audio in mixing buffer AND preserve the original RTC timestamp
    {
        std::lock_guard<std::mutex> lock(audioMixingMutex_);
        audioMixingBuffer_[userId] = audioSamples;

        // Store the most recent RTC timestamp for mixed audio frame generation
        // This preserves the original RTC client timing like lastBufferedTimestamp does
        lastAudioMixTime_ = frame.timestamp;

        // std::cout << "[RecordingSink] Audio mixing buffer now has " << audioMixingBuffer_.size()
        //           << " users, using RTC timestamp: " << frame.timestamp << std::endl;
    }

    // Trigger mixing immediately when new audio data is available
    return createMixedAudioFrame();
}

bool RecordingSink::createMixedAudioFrame() {
    std::lock_guard<std::mutex> mixLock(audioMixingMutex_);
    std::lock_guard<std::mutex> contextLock(userContextsMutex_);

    // std::cout << "[RecordingSink] createMixedAudioFrame called with " <<
    // audioMixingBuffer_.size()
    //           << " users in buffer" << std::endl;

    if (audioMixingBuffer_.empty()) {
        return true;  // No audio to mix
    }

    // Initialize composite context if needed
    if (!compositeContext_) {
        if (!initializeEncoder("")) {
            return false;
        }
    }

    // Find the maximum number of samples across all users
    size_t maxSamples = 0;
    for (const auto& pair : audioMixingBuffer_) {
        maxSamples = std::max(maxSamples, pair.second.size());
    }

    if (maxSamples == 0) {
        return true;  // No samples to mix
    }

    // Create mixed audio buffer
    std::vector<float> mixedAudio(maxSamples, 0.0f);

    // Mix all users' audio
    for (const auto& pair : audioMixingBuffer_) {
        const std::vector<float>& userAudio = pair.second;
        for (size_t i = 0; i < userAudio.size() && i < maxSamples; i++) {
            mixedAudio[i] += userAudio[i];
        }
    }

    // Normalize mixed audio to prevent clipping using a running average
    float current_max = 0.0f;
    for (float sample : mixedAudio) {
        current_max = std::max(current_max, std::abs(sample));
    }

    // Update running average of max audio level
    maxAudioLevel_ = (maxAudioLevel_ * 0.95f) + (current_max * 0.05f);

    if (maxAudioLevel_ > 1.0f) {
        float scale = 1.0f / maxAudioLevel_;
        for (float& sample : mixedAudio) {
            sample *= scale;
        }
    }

    // Convert back to int16_t format
    std::vector<int16_t> mixedAudioInt16(maxSamples);
    for (size_t i = 0; i < maxSamples; i++) {
        mixedAudioInt16[i] = static_cast<int16_t>(mixedAudio[i] * 32767.0f);
    }

    // Create AudioFrame from mixed data
    AudioFrame mixedFrame;
    mixedFrame.data.resize(maxSamples * sizeof(int16_t));
    std::memcpy(mixedFrame.data.data(), mixedAudioInt16.data(), maxSamples * sizeof(int16_t));
    mixedFrame.sampleRate = config_.audioSampleRate;
    mixedFrame.channels = config_.audioChannels;
    // Best Approach: RTC-Anchored with Monotonic Safety
    // Primary: Use RTC timestamp for perfect A/V sync
    // Fallback: Minimal increment only when RTC would go backwards
    uint64_t rtcTimestamp = lastAudioMixTime_;  // From latest RTC frame

    if (compositeContext_ && compositeContext_->lastBufferedTimestamp > 0) {
        // Check if RTC timestamp would violate monotonic requirement
        if (rtcTimestamp <= compositeContext_->lastBufferedTimestamp) {
            // Minimal safety increment (1ms) to maintain monotonic progression
            mixedFrame.timestamp = compositeContext_->lastBufferedTimestamp + 1;
            AG_LOG_FAST(
                INFO, "Mixed audio monotonic safety: %lu (RTC: %lu would go backwards, last: %lu)",
                mixedFrame.timestamp, rtcTimestamp, compositeContext_->lastBufferedTimestamp);
        } else {
            // Use real RTC timestamp for perfect sync
            mixedFrame.timestamp = rtcTimestamp;
            // std::cout << "[RecordingSink] Mixed audio using RTC timestamp: " <<
            // mixedFrame.timestamp
            //           << std::endl;
        }
    } else {
        // First frame: always use RTC timestamp
        mixedFrame.timestamp = rtcTimestamp;
        AG_LOG_TS(INFO, "Mixed audio first RTC timestamp: %ld", mixedFrame.timestamp);
    }
    mixedFrame.valid = true;

    // Update lastBufferedTimestamp for next frame
    if (compositeContext_) {
        compositeContext_->lastBufferedTimestamp = mixedFrame.timestamp;
    }

    // Clear the buffer after mixing
    audioMixingBuffer_.clear();

    // Encode the mixed audio frame
    return encodeIndividualAudioFrame(mixedFrame, compositeContext_.get(), "composite");
}

std::pair<int, int> RecordingSink::calculateOptimalLayout(int numUsers) {
    // Returns (cols, rows) for optimal grid layout
    switch (numUsers) {
        case 1:
            return {1, 1};  // Full screen
        case 2:
            return {2, 1};  // Half-half (side by side)
        case 3:
        case 4:
            return {2, 2};  // 2x2 grid
        case 5:
        case 6:
            return {3, 2};  // 3x2 grid
        case 7:
        case 8:
        case 9:
            return {3, 3};  // 3x3 grid
        case 10:
        case 11:
        case 12:
            return {4, 3};  // 4x3 grid
        case 13:
        case 14:
        case 15:
        case 16:
            return {4, 4};  // 4x4 grid
        case 17:
        case 18:
        case 19:
        case 20:
            return {5, 4};  // 5x4 grid
        case 21:
        case 22:
        case 23:
        case 24:
            return {6, 4};  // 6x4 grid
        default:
            // For more than 24 users, calculate dynamically
            int cols = static_cast<int>(std::ceil(std::sqrt(numUsers)));
            int rows = static_cast<int>(std::ceil(static_cast<double>(numUsers) / cols));
            return {cols, rows};
    }
}

bool RecordingSink::writePacket(AVPacket* packet, AVFormatContext* formatContext,
                                AVStream* stream) {
    // Write packet using interleaved write for proper timestamp ordering
    int ret = av_interleaved_write_frame(formatContext, packet);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        AG_LOG_FAST(ERROR, "Failed to write packet: %s", errbuf);
        return false;
    }
    return true;
}

void RecordingSink::flushAllEncoders() {
    AG_LOG_TS(INFO, "Flushing all encoders to prevent blocking...");

    std::lock_guard<std::mutex> lock(userContextsMutex_);

    // Flush composite encoder
    if (compositeContext_ && compositeContext_->videoCodecContext) {
        AG_LOG_TS(INFO, "Flushing composite video encoder");
        // Send NULL frame to signal end-of-stream and flush buffers
        avcodec_send_frame(compositeContext_->videoCodecContext, nullptr);

        // Drain remaining packets to unblock encoder
        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            while (avcodec_receive_packet(compositeContext_->videoCodecContext, pkt) == 0) {
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    if (compositeContext_ && compositeContext_->audioCodecContext) {
        AG_LOG_TS(INFO, "Flushing composite audio encoder");
        avcodec_send_frame(compositeContext_->audioCodecContext, nullptr);

        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            while (avcodec_receive_packet(compositeContext_->audioCodecContext, pkt) == 0) {
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    // Flush individual user encoders
    for (auto& pair : userContexts_) {
        const std::string& userId = pair.first;
        UserContext* context = pair.second.get();

        if (context && context->videoCodecContext) {
            AG_LOG_FAST(INFO, "Flushing video encoder for user: %s", userId.c_str());
            avcodec_send_frame(context->videoCodecContext, nullptr);

            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                while (avcodec_receive_packet(context->videoCodecContext, pkt) == 0) {
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }

        if (context && context->audioCodecContext) {
            AG_LOG_FAST(INFO, "Flushing audio encoder for user: %s", userId.c_str());
            avcodec_send_frame(context->audioCodecContext, nullptr);

            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                while (avcodec_receive_packet(context->audioCodecContext, pkt) == 0) {
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }
    }

    AG_LOG_TS(INFO, "All encoders flushed successfully");
}

void RecordingSink::cleanupEncoder(const std::string& userId) {
    UserContext* context = nullptr;
    bool isComposite = (userId.empty() || config_.mode == VideoCompositor::Mode::Composite);

    if (isComposite) {
        context = compositeContext_.get();
    } else {
        auto it = userContexts_.find(userId);
        if (it != userContexts_.end()) {
            context = it->second.get();
        }
    }

    if (!context) {
        return;
    }

    // Flush remaining audio samples first to prevent cutting
    if (context->audioCodecContext && !context->audioSampleBuffer.empty()) {
        AG_LOG_FAST(INFO, "Flushing remaining %zu audio samples for user %s",
                    context->audioSampleBuffer.size(), userId.c_str());

        int samples_per_frame = context->audioCodecContext->frame_size;
        if (samples_per_frame == 0) samples_per_frame = 1024;

        size_t required_samples = samples_per_frame * config_.audioChannels;

        // Pad with silence if necessary
        if (context->audioSampleBuffer.size() < required_samples) {
            size_t old_size = context->audioSampleBuffer.size();
            context->audioSampleBuffer.resize(required_samples, 0);  // Pad with silence
            AG_LOG_FAST(INFO, "Padded audio buffer from %zu to %zu samples", old_size,
                        required_samples);
        }

        // Create final audio frame
        AudioFrame finalFrame;
        finalFrame.data.resize(required_samples * sizeof(int16_t));
        std::memcpy(finalFrame.data.data(), context->audioSampleBuffer.data(),
                    finalFrame.data.size());
        finalFrame.sampleRate = config_.audioSampleRate;
        finalFrame.channels = config_.audioChannels;
        finalFrame.timestamp = context->lastBufferedTimestamp + 20;
        finalFrame.valid = true;
        finalFrame.userId = userId;

        encodeIndividualAudioFrame(finalFrame, context, userId);
        context->audioSampleBuffer.clear();
    }

    // Write any buffered frames
    if (context->videoCodecContext) {
        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            // Flush the encoder
            avcodec_send_frame(context->videoCodecContext, nullptr);
            while (avcodec_receive_packet(context->videoCodecContext, pkt) == 0) {
                // Set stream index and proper timestamps
                pkt->stream_index = context->videoStream->index;
                int64_t ticks_per_frame = context->videoStream->time_base.den / config_.videoFps;
                pkt->pts = context->videoFrameCount * ticks_per_frame;
                pkt->dts = pkt->pts;
                pkt->duration = ticks_per_frame;
                context->videoFrameCount++;

                av_interleaved_write_frame(context->formatContext, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    // Write trailer
    if (context->formatContext) {
        av_write_trailer(context->formatContext);

        // Add completed video file to metadata (MP4/AVI/MKV)
        if (metadataManager_ && !config_.taskId.empty() && config_.format != OutputFormat::TS) {
            MetadataManager::FileInfo fileInfo;
            fileInfo.filename = std::filesystem::path(context->filename).filename();
            fileInfo.fullPath = context->filename;

            if (config_.format == OutputFormat::MP4) {
                fileInfo.type = MetadataManager::FileType::MP4;
            } else if (config_.format == OutputFormat::AVI) {
                fileInfo.type = MetadataManager::FileType::MP4;  // Use MP4 enum for video files
            } else if (config_.format == OutputFormat::MKV) {
                fileInfo.type = MetadataManager::FileType::MP4;  // Use MP4 enum for video files
            }

            fileInfo.sizeBytes = getFileSize(context->filename);
            fileInfo.createdAt = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    context->startTime.time_since_epoch()));
            fileInfo.completedAt = std::chrono::system_clock::now();
            fileInfo.isComplete = true;
            fileInfo.width = config_.videoWidth;
            fileInfo.height = config_.videoHeight;

            // Calculate duration
            auto now = std::chrono::steady_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - context->startTime);
            fileInfo.durationSeconds = duration.count() / 1000.0;

            std::string outputPrefix = config_.outputDir + "/" + currentOutputFilePrefix_;
            if (!metadataManager_->appendFileToMetadata(config_.taskId, fileInfo, outputPrefix)) {
                AG_LOG_FAST(WARN, "Failed to add mp4 file to metadata: %s",
                            fileInfo.filename.c_str());
            }
        }
    }

    // Cleanup resources
    if (context->videoCodecContext) {
        avcodec_free_context(&context->videoCodecContext);
    }
    if (context->audioCodecContext) {
        avcodec_free_context(&context->audioCodecContext);
    }
    if (context->formatContext) {
        if (!(context->formatContext->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&context->formatContext->pb);
        }
        avformat_free_context(context->formatContext);
    }
    if (context->swsContext) {
        sws_freeContext(context->swsContext);
    }
    if (context->swrContext) {
        swr_free(&context->swrContext);
    }
}

std::string RecordingSink::generateOutputFilename(const std::string& userId) {
    std::string baseFilename = agora::common::generateTimestampedFilename(
        "recording",
        agora::common::getFileExtension(getFileExtension().substr(1)),  // Remove dot from extension
        userId.empty() ? "" : userId,
        true  // Include milliseconds
    );

    return config_.outputDir + "/" + baseFilename;
}

std::string RecordingSink::getFileExtension() const {
    switch (config_.format) {
        case OutputFormat::MP4:
            return ".mp4";
        case OutputFormat::AVI:
            return ".avi";
        case OutputFormat::MKV:
            return ".mkv";
        case OutputFormat::TS:
            return ".ts";
        default:
            return ".mp4";
    }
}

bool RecordingSink::createOutputDirectory() {
    return agora::common::createDirectoriesIfNotExists(config_.outputDir);
}

uint64_t RecordingSink::getFileSize(const std::string& filepath) const {
    try {
        std::filesystem::path path(filepath);
        if (std::filesystem::exists(path)) {
            return std::filesystem::file_size(path);
        }
    } catch (const std::filesystem::filesystem_error&) {
        // File doesn't exist or access error
    }
    return 0;
}

void RecordingSink::setMaxDuration(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.maxDurationSeconds = seconds;
}

void RecordingSink::setOutputFormat(OutputFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.format = format;
}

bool RecordingSink::shouldRecordUser(const std::string& userId) const {
    // If no target users specified, record all users
    if (config_.targetUsers.empty()) {
        return true;
    }

    // Check if user is in the target list
    return std::find(config_.targetUsers.begin(), config_.targetUsers.end(), userId) !=
           config_.targetUsers.end();
}

bool RecordingSink::updateCompositeFrame(const VideoFrame& frame, const std::string& userId) {
    // std::cout << "[RecordingSink] updateCompositeFrame() - ENTRY, userId=" << userId <<
    // std::endl; std::flush(std::cout);

    {
        // std::cout
        //     << "[RecordingSink] updateCompositeFrame() - about to acquire compositeBufferMutex_"
        //     << std::endl;
        // std::flush(std::cout);
        std::lock_guard<std::mutex> lock(compositeBufferMutex_);

        // Store the latest frame from this user
        compositeFrameBuffer_[userId] = frame;

        // Track when this frame was received
        uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        compositeFrameTimestamps_[userId] = currentTime;

        // Remove old frames that are too old (frame persistence cleanup)
        auto it = compositeFrameBuffer_.begin();
        while (it != compositeFrameBuffer_.end()) {
            const std::string& user = it->first;
            uint64_t frameTime = compositeFrameTimestamps_[user];

            if (currentTime - frameTime > COMPOSITE_FRAME_TIMEOUT_MS) {
                AG_LOG_FAST(INFO, "Removing old frame for user %s (age: %lums)", user.c_str(),
                            (currentTime - frameTime));
                compositeFrameTimestamps_.erase(user);
                it = compositeFrameBuffer_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Always try to create composite frame - don't drop frames too aggressively
    // This ensures we don't miss users' frames due to timing differences
    uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

    // Only skip if we're processing too fast (less than 16ms = 60fps max)
    if (currentTime - lastCompositeTime_ < 16) {
        droppedFrames_++;
        return true;  // Skip this frame to maintain reasonable frame rate
    }

    // Start performance measurement
    frameProcessingStartTime_ = currentTime;
    lastCompositeTime_ = currentTime;

    // Use VideoCompositor for all video composition
    // std::cout
    //     << "[RecordingSink] updateCompositeFrame() - about to call
    //     videoCompositor_->addUserFrame()"
    //     << std::endl;
    // std::flush(std::cout);

    if (videoCompositor_) {
        // std::cout
        //     << "[RecordingSink] updateCompositeFrame() - calling
        //     videoCompositor_->addUserFrame()"
        //     << std::endl;
        // std::flush(std::cout);

        // Skip VideoCompositor if stop was requested to avoid blocking
        if (stopRequested_.load()) {
            // std::cout << "[RecordingSink] updateCompositeFrame() - stopRequested detected, "
            //              "skipping VideoCompositor"
            //           << std::endl;
            // std::flush(std::cout);
            return true;
        }

        bool result = videoCompositor_->addUserFrame(frame, userId);
        // std::cout << "[RecordingSink] updateCompositeFrame() - videoCompositor_->addUserFrame() "
        //              "returned: "
        //           << result << std::endl;
        // std::flush(std::cout);
        return result;
    } else {
        AG_LOG_FAST(ERROR, "VideoCompositor not initialized for composite mode");
        return false;
    }
}

void RecordingSink::onComposedFrame(const AVFrame* composedFrame) {
    if (!composedFrame || config_.mode != VideoCompositor::Mode::Composite) {
        return;
    }

    static int debug_composed_count = 0;
    debug_composed_count++;
    if (debug_composed_count % 30 == 0) {
        AG_LOG_FAST(INFO, "DEBUG: onComposedFrame() #%d - Raw composedFrame->pts: %ld",
                    debug_composed_count, composedFrame->pts);
    }

    std::lock_guard<std::mutex> contextLock(userContextsMutex_);

    // Initialize composite context if needed
    if (!compositeContext_) {
        if (!initializeEncoder("")) {
            AG_LOG_FAST(ERROR, "Failed to initialize encoder for composite frame");
            return;
        }
    }

    UserContext* context = compositeContext_.get();
    if (!context || !context->videoCodecContext || !context->videoFrame) {
        AG_LOG_FAST(ERROR, "Invalid composite context for encoding");
        return;
    }

    // Copy the composed frame to our encoding frame
    if (av_frame_copy(context->videoFrame, composedFrame) < 0) {
        AG_LOG_FAST(ERROR, "Failed to copy composed frame");
        return;
    }

    // Composite mode: use unified PTS calculation
    uint64_t rtcTimestamp = composedFrame->pts;  // composite frame pts is the RTC timestamp

    // Update stream state and detect restart
    updateStreamState(context, true, rtcTimestamp);
    if (detectStreamRestart(context, true, rtcTimestamp)) {
        // Stream restart: reset time origin for both formats
        context->hasTimeOrigin = false;

        // Format-specific PTS handling
        if (config_.format == OutputFormat::TS) {
            // For TS: maintain PTS continuity, don't reset to -1
            AG_LOG_FAST(WARN,
                        "Detected composite video stream restart, resetting time origin (TS: "
                        "maintaining PTS continuity)");
        } else {
            // For MP4/AVI/MKV: can safely reset PTS
            context->lastVideoPts = -1;
            AG_LOG_FAST(WARN,
                        "Detected composite video stream restart, resetting time origin and PTS");
        }
    }

    // Use new unified PTS calculation method
    int64_t pts = calculateVideoPTS(context, rtcTimestamp);

    // Convert to encoder's time base
    context->videoFrame->pts = av_rescale_q(pts, {1, 90000}, context->videoCodecContext->time_base);

    context->videoFrameCount++;

    // Encode the composed frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        AG_LOG_FAST(ERROR, "Failed to allocate packet for composed frame");
        return;
    }

    // std::cout << "[RecordingSink] Composite: About to call avcodec_send_frame()" << std::endl;
    // std::flush(std::cout);

    int ret = avcodec_send_frame(context->videoCodecContext, context->videoFrame);

    // std::cout << "[RecordingSink] Composite: avcodec_send_frame() returned: " << ret <<
    // std::endl; std::flush(std::cout);

    if (ret < 0) {
        av_packet_free(&packet);
        AG_LOG_FAST(ERROR, "Failed to send composed frame to encoder");
        return;
    }

    // std::cout << "[RecordingSink] Composite: Entering avcodec_receive_packet() loop" <<
    // std::endl; std::flush(std::cout);

    while (ret >= 0) {
        // std::cout << "[RecordingSink] Composite: About to call avcodec_receive_packet()"
        //           << std::endl;
        // std::flush(std::cout);

        ret = avcodec_receive_packet(context->videoCodecContext, packet);

        // std::cout << "[RecordingSink] Composite: avcodec_receive_packet() returned: " << ret
        //           << std::endl;
        // std::flush(std::cout);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            AG_LOG_FAST(ERROR, "Failed to receive packet from encoder");
            return;
        }

        packet->stream_index = context->videoStream->index;

        static int rescale_log_count = 0;
        if (rescale_log_count % 30 == 0) {
            AG_LOG_FAST(
                INFO, "BEFORE rescale - PTS: %ld, codec time_base: %d/%d, stream time_base: %d/%d",
                packet->pts, context->videoCodecContext->time_base.num,
                context->videoCodecContext->time_base.den, context->videoStream->time_base.num,
                context->videoStream->time_base.den);
        }
        rescale_log_count++;

        // Composite video PTS already correctly set in frame stage, just need to convert time base
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts = av_rescale_q(packet->pts, context->videoCodecContext->time_base,
                                       context->videoStream->time_base);
        }

        // DTS = PTS (no B-frame reordering)
        packet->dts = packet->pts;

        if (rescale_log_count % 30 == 0) {
            AG_LOG_FAST(INFO, "AFTER rescale - PTS: %ld", packet->pts);
        }

        // TS-specific: Detect keyframes and handle segment rotation (only for TS format)
        if (config_.format == OutputFormat::TS) {
            if (detectKeyframe(packet, context)) {
                // Check if we need to rotate segments after keyframe detection
                if (tsPendingSegmentRotation_.load() && tsSegmentManager_) {
                    if (switchToNewTSSegment(context)) {
                        tsPendingSegmentRotation_ = false;
                        AG_LOG_FAST(INFO, "Successfully rotated to new TS segment on keyframe");
                    }
                }
            }
        }

        // Capture PTS value before writing (muxer may modify packet)
        int64_t original_pts = packet->pts;

        if (!writePacket(packet, context->formatContext, context->videoStream)) {
            AG_LOG_FAST(ERROR, "Failed to write composed frame packet");
        } else {
            static int composed_log_count = 0;
            if (composed_log_count % 30 == 0) {
                AG_LOG_FAST(INFO, "Successfully encoded composed frame %d, pts: %ld",
                            composed_log_count, original_pts);
            }
            composed_log_count++;
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

void RecordingSink::cleanupCompositeResources() {
    std::lock_guard<std::mutex> lock(compositeBufferMutex_);

    // Cleanup cached scaling contexts
    for (auto& pair : userScalingContexts_) {
        if (pair.second) {
            sws_freeContext(pair.second);
        }
    }
    userScalingContexts_.clear();

    // Cleanup pre-allocated frames
    for (auto& pair : scaledFramePool_) {
        if (pair.second) {
            av_frame_free(&pair.second);
        }
    }
    scaledFramePool_.clear();

    // Clear composite frame buffers
    compositeFrameBuffer_.clear();
    compositeFrameTimestamps_.clear();

    // Clear audio mixing buffers
    {
        std::lock_guard<std::mutex> audioLock(audioMixingMutex_);
        audioMixingBuffer_.clear();
    }

    AG_LOG_FAST(INFO, "Cleaned up composite performance caches");
}

// ========== RTC Timestamp Synchronization Core Implementation ==========

void RecordingSink::initializeRtcTimeOrigin(UserContext* context, uint64_t rtcTimestamp) {
    if (!context->hasTimeOrigin) {
        context->rtcTimeOrigin = rtcTimestamp;
        context->hasTimeOrigin = true;
        AG_LOG_FAST(INFO, "RTC time origin initialized: %lu ms", context->rtcTimeOrigin);
    }
}

int64_t RecordingSink::calculateVideoPTS(UserContext* context, uint64_t rtcTimestamp) {
    // Establish time origin
    initializeRtcTimeOrigin(context, rtcTimestamp);

    // Calculate relative time difference (milliseconds)
    int64_t relativeMs = rtcTimestamp - context->rtcTimeOrigin;

    // Convert to 90kHz time base (FFmpeg standard)
    int64_t pts = relativeMs * 90;  // 1ms = 90 ticks @ 90kHz

    // Ensure monotonic increment
    int64_t minIncrement = 90000 / config_.videoFps;  // Time interval per frame
    pts = ensureMonotonicPTS(pts, context->lastVideoPts, minIncrement);

    static int log_count = 0;
    if (log_count % 30 == 0) {
        AG_LOG_FAST(INFO, "Video PTS: %ld (RTC: %lu, relative: %ldms)", pts, rtcTimestamp,
                    relativeMs);
    }
    log_count++;

    return pts;
}

int64_t RecordingSink::calculateAudioPTS(UserContext* context, uint64_t rtcTimestamp) {
    // Establish time origin
    initializeRtcTimeOrigin(context, rtcTimestamp);

    // Calculate relative time difference (milliseconds)
    int64_t relativeMs = rtcTimestamp - context->rtcTimeOrigin;

    // Convert to 90kHz time base
    int64_t pts = relativeMs * 90;

    // Audio frame time interval (e.g., 1024 samples@48kHz = 21.33ms)
    int64_t samplesPerFrame = context->audioCodecContext->frame_size;
    int64_t sampleRate = context->audioCodecContext->sample_rate;
    int64_t minIncrement = (samplesPerFrame * 90000) / sampleRate;

    pts = ensureMonotonicPTS(pts, context->lastAudioPts, minIncrement);

    static int audio_log_count = 0;
    if (audio_log_count % 50 == 0) {
        AG_LOG_FAST(INFO, "Audio PTS: %ld (RTC: %lu, relative: %ldms)", pts, rtcTimestamp,
                    relativeMs);
    }
    audio_log_count++;

    return pts;
}

int64_t RecordingSink::ensureMonotonicPTS(int64_t newPts, int64_t& lastPts, int64_t minIncrement) {
    if (lastPts < 0) {
        // First frame: use directly
        return lastPts = newPts;
    }

    if (newPts > lastPts) {
        // Normal case: new PTS is larger
        return lastPts = newPts;
    } else {
        // Abnormal case: timestamp rollback or equal, force increment
        return lastPts = lastPts + minIncrement;
    }
}

void RecordingSink::updateStreamState(UserContext* context, bool isVideo, uint64_t rtcTimestamp) {
    if (isVideo) {
        context->videoStreamActive = true;
        context->lastVideoRtcTs = rtcTimestamp;
    } else {
        context->audioStreamActive = true;
        context->lastAudioRtcTs = rtcTimestamp;
    }
}

bool RecordingSink::detectStreamRestart(UserContext* context, bool isVideo, uint64_t rtcTimestamp) {
    // Detect if stream has restarted (large timestamp jump)
    uint64_t lastTs = isVideo ? context->lastVideoRtcTs : context->lastAudioRtcTs;

    if (lastTs > 0) {
        int64_t timeDiff = rtcTimestamp - lastTs;
        // If time difference exceeds 5 seconds, consider it a stream restart
        if (timeDiff > 5000 || timeDiff < -1000) {
            AG_LOG_FAST(WARN, "%s stream suspected restart: time jump %ldms",
                        isVideo ? "Video" : "Audio", timeDiff);
            return true;
        }
    }

    return false;
}

void RecordingSink::checkAndRotateSegmentIfNeeded() {
    if (!tsSegmentManager_) {
        static int null_mgr_log_count = 0;
        if (null_mgr_log_count++ == 0) {
            AG_LOG_FAST(WARN, "checkAndRotateSegmentIfNeeded: tsSegmentManager_ is null");
        }
        return;
    }

    if (config_.format != OutputFormat::TS) {
        static int non_ts_log_count = 0;
        if (non_ts_log_count++ == 0) {
            AG_LOG_FAST(WARN, "checkAndRotateSegmentIfNeeded: format is not TS (format=%d)",
                        (int)config_.format);
        }
        return;
    }

    auto currentTime = std::chrono::steady_clock::now();

    static int check_count = 0;
    check_count++;
    if (check_count % 500 == 0) {  // Log every 500 checks (every 5 seconds with 10ms loop)
        AG_LOG_FAST(INFO, "TS segment rotation check #%d, segment duration: %ds", check_count,
                    config_.tsSegmentDurationSeconds);
    }

    // Check if segment rotation is needed (time-based check only)
    // We'll wait for the next keyframe to actually perform the rotation
    if (tsSegmentManager_->shouldRotateSegment(currentTime, false)) {
        // Set pending rotation flag - actual rotation happens on next keyframe
        if (!tsPendingSegmentRotation_.load()) {
            tsPendingSegmentRotation_ = true;
            AG_LOG_FAST(INFO, "TS segment rotation requested after %ds, waiting for next keyframe",
                        config_.tsSegmentDurationSeconds);
        }
    }
}

bool RecordingSink::detectKeyframe(AVPacket* packet, UserContext* context) {
    if (!packet || packet->stream_index != context->videoStream->index) {
        return false;  // Not a video packet
    }

    // Check if this is a keyframe (I-frame)
    bool isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

    if (isKeyframe) {
        context->lastFrameWasKeyframe = true;
        context->lastKeyframeTime = std::chrono::steady_clock::now();

        static int keyframe_log_count = 0;
        if (keyframe_log_count % 10 == 0) {  // Log every 10th keyframe
            AG_LOG_FAST(INFO, "Detected keyframe #%d", keyframe_log_count);
        }
        keyframe_log_count++;
    } else {
        context->lastFrameWasKeyframe = false;
    }

    return isKeyframe;
}

bool RecordingSink::switchToNewTSSegment(UserContext* context) {
    std::lock_guard<std::mutex> rotationLock(tsRotationMutex_);

    if (!tsSegmentManager_ || !context || !context->formatContext) {
        return false;
    }

    // 1. Finalize current segment
    if (!tsSegmentManager_->finalizeCurrentSegment()) {
        AG_LOG_FAST(ERROR, "Failed to finalize current TS segment");
        return false;
    }

    // 2. Prepare new segment
    if (!tsSegmentManager_->rotateToNewSegment()) {
        AG_LOG_FAST(ERROR, "Failed to rotate to new TS segment");
        return false;
    }

    // 3. Close current format context (but keep encoders)
    if (context->formatContext) {
        av_write_trailer(context->formatContext);

        if (!(context->formatContext->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&context->formatContext->pb);
        }
        avformat_free_context(context->formatContext);
        context->formatContext = nullptr;
    }

    // 4. Create new format context for new segment
    std::string newSegmentPath = tsSegmentManager_->getCurrentTempSegmentPath();
    if (!setupOutputFormat(&context->formatContext, newSegmentPath)) {
        AG_LOG_FAST(ERROR, "Failed to setup output format for new TS segment");
        return false;
    }

    // 5. Add streams to new format context (reuse existing codec contexts)
    if (context->videoCodecContext) {
        context->videoStream =
            avformat_new_stream(context->formatContext, context->videoCodecContext->codec);
        if (context->videoStream) {
            avcodec_parameters_from_context(context->videoStream->codecpar,
                                            context->videoCodecContext);
            context->videoStream->time_base = context->videoCodecContext->time_base;
            context->videoStream->avg_frame_rate = {config_.videoFps, 1};
            context->videoStream->r_frame_rate = {config_.videoFps, 1};
        }
    }

    if (context->audioCodecContext) {
        context->audioStream =
            avformat_new_stream(context->formatContext, context->audioCodecContext->codec);
        if (context->audioStream) {
            avcodec_parameters_from_context(context->audioStream->codecpar,
                                            context->audioCodecContext);
            context->audioStream->time_base = context->audioCodecContext->time_base;
        }
    }

    // 6. Write header for new segment
    if (avformat_write_header(context->formatContext, nullptr) < 0) {
        AG_LOG_FAST(ERROR, "Failed to write header for new TS segment");
        return false;
    }

    // 7. CRITICAL: Maintain timestamp continuity for TS format
    // DO NOT reset PTS counters - TS segments need continuous timestamps
    // Keep context->lastVideoPts, context->lastAudioPts, and hasTimeOrigin intact
    AG_LOG_FAST(INFO, "Maintaining timestamp continuity - lastVideoPts: %ld, lastAudioPts: %ld",
                context->lastVideoPts, context->lastAudioPts);

    // Add completed segment to metadata
    if (metadataManager_ && !config_.taskId.empty() && tsSegmentManager_) {
        // Get completed segment info from the previous segment that was just finalized
        auto completedSegments = tsSegmentManager_->getCurrentSession().segments;
        if (!completedSegments.empty()) {
            auto& lastSegment = completedSegments.back();
            if (lastSegment.isComplete) {
                MetadataManager::FileInfo fileInfo;
                fileInfo.filename = lastSegment.filename;
                fileInfo.fullPath =
                    tsSegmentManager_->getCurrentSession().sessionDir + "/" + lastSegment.filename;
                fileInfo.type = MetadataManager::FileType::TS;
                fileInfo.sizeBytes = getFileSize(fileInfo.fullPath);
                fileInfo.createdAt = std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        lastSegment.startTime.time_since_epoch()));
                fileInfo.completedAt = std::chrono::system_clock::now();
                fileInfo.durationSeconds = lastSegment.duration;
                fileInfo.isComplete = true;
                fileInfo.segmentNumber = lastSegment.segmentNumber;
                fileInfo.isKeyframeAligned = true;  // TS segments are always keyframe-aligned

                std::string outputPrefix = config_.outputDir + "/" + currentOutputFilePrefix_;
                if (!metadataManager_->appendFileToMetadata(config_.taskId, fileInfo,
                                                            outputPrefix)) {
                    AG_LOG_FAST(WARN, "Failed to add TS segment to metadata: %s",
                                fileInfo.filename.c_str());
                }
            }
        }
    }

    AG_LOG_FAST(INFO, "Successfully switched to new TS segment: %s", newSegmentPath.c_str());
    return true;
}

// =============================================================================
// Passthrough (Encoded Frame) Recording — writes H264 directly to container
// =============================================================================

void RecordingSink::onEncodedVideoFrame(uint32_t uid, const uint8_t* data, size_t length,
                                        const agora::rtc::EncodedVideoFrameInfo& info) {
    if (!isRecording() || !data || length == 0) {
        return;
    }

    std::string userId = std::to_string(uid);

    if (!shouldRecordUser(userId)) {
        return;
    }

    static int encoded_count = 0;
    encoded_count++;
    if (encoded_count == 1 || encoded_count % 100 == 0) {
        AG_LOG_FAST(INFO, "Passthrough: encoded frame #%d uid=%u len=%zu keyframe=%d",
                    encoded_count, uid, length,
                    info.frameType == VIDEO_FRAME_TYPE_KEY_FRAME ? 1 : 0);
    }

    // Support H264 and H265/HEVC for passthrough
    bool isHevc = false;
    if (info.codecType == VIDEO_CODEC_H265) {
        isHevc = true;
    } else if (info.codecType != VIDEO_CODEC_H264) {
        static int unsupported_count = 0;
        if (unsupported_count++ % 100 == 0) {
            AG_LOG_FAST(WARN, "Passthrough: unsupported codec %d from uid %u", info.codecType, uid);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(userContextsMutex_);

    auto it = passthroughContexts_.find(userId);
    if (it == passthroughContexts_.end()) {
        if (!initializePassthroughContext(userId, info.width, info.height, isHevc)) {
            return;
        }
        it = passthroughContexts_.find(userId);
    }

    auto* ctx = it->second.get();
    bool isKeyframe = (info.frameType == VIDEO_FRAME_TYPE_KEY_FRAME);

    // Use system time for PTS (SDK's captureTimeMs is RTC internal, not epoch)
    uint64_t timestampMs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());

    // On first keyframe, extract SPS/PPS and set up extradata
    if (!ctx->extraDataSet && isKeyframe) {
        if (!setupPassthroughExtradata(ctx, data, length)) {
            AG_LOG_FAST(WARN, "Failed to extract SPS/PPS from keyframe for uid %u", uid);
            return;
        }
    }

    // Don't write non-keyframes before we have extradata
    if (!ctx->headerWritten) {
        return;
    }

    writePassthroughVideoPacket(ctx, data, length, isKeyframe, timestampMs);
}

bool RecordingSink::initializePassthroughContext(const std::string& userId, uint32_t width,
                                                 uint32_t height, bool isHevc) {
    auto ctx = std::make_unique<PassthroughContext>();
    ctx->isHevc = isHevc;
    ctx->filename = generateOutputFilename(userId);

    // Create output format context
    int ret = avformat_alloc_output_context2(&ctx->formatContext, nullptr, nullptr,
                                             ctx->filename.c_str());
    if (ret < 0 || !ctx->formatContext) {
        AG_LOG_FAST(ERROR, "Passthrough: failed to create output context for user %s",
                    userId.c_str());
        return false;
    }

    // Add video stream (codec copy — no encoding)
    ctx->videoStream = avformat_new_stream(ctx->formatContext, nullptr);
    if (!ctx->videoStream) {
        AG_LOG_FAST(ERROR, "Passthrough: failed to create video stream for user %s",
                    userId.c_str());
        avformat_free_context(ctx->formatContext);
        return false;
    }

    ctx->videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->videoStream->codecpar->codec_id = isHevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    ctx->videoStream->codecpar->width = width > 0 ? width : config_.videoWidth;
    ctx->videoStream->codecpar->height = height > 0 ? height : config_.videoHeight;
    ctx->videoStream->time_base = {1, 90000};

    // Add audio stream if audio recording is enabled
    if (config_.recordAudio) {
        if (!setupAudioEncoder(&ctx->audioCodecContext, userId)) {
            AG_LOG_FAST(WARN, "Passthrough: failed to set up audio encoder for user %s",
                        userId.c_str());
        } else {
            ctx->audioStream = avformat_new_stream(ctx->formatContext, nullptr);
            if (ctx->audioStream) {
                avcodec_parameters_from_context(ctx->audioStream->codecpar, ctx->audioCodecContext);
                ctx->audioStream->time_base = ctx->audioCodecContext->time_base;

                // Allocate audio frame
                ctx->audioFrame = av_frame_alloc();
                if (ctx->audioFrame) {
                    ctx->audioFrame->format = ctx->audioCodecContext->sample_fmt;
                    ctx->audioFrame->ch_layout = ctx->audioCodecContext->ch_layout;
                    ctx->audioFrame->sample_rate = ctx->audioCodecContext->sample_rate;
                    ctx->audioFrame->nb_samples = ctx->audioCodecContext->frame_size;
                    av_frame_get_buffer(ctx->audioFrame, 0);
                }
            }
        }
    }

    AG_LOG_FAST(INFO, "Passthrough: created context for user %s → %s (%ux%u)", userId.c_str(),
                ctx->filename.c_str(), ctx->videoStream->codecpar->width,
                ctx->videoStream->codecpar->height);

    passthroughContexts_[userId] = std::move(ctx);
    return true;
}

// Helper: scan Annex-B bitstream and collect NAL units by type
static std::vector<std::pair<int, std::vector<uint8_t>>> parseAnnexBNalUnits(const uint8_t* data,
                                                                             size_t length,
                                                                             bool isHevc) {
    std::vector<std::pair<int, std::vector<uint8_t>>> nalus;  // {nalType, nalData}
    size_t i = 0;

    while (i < length - 3) {
        int startCodeLen = 0;
        if (i + 3 < length && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
            data[i + 3] == 1) {
            startCodeLen = 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            startCodeLen = 3;
        }
        if (startCodeLen == 0) {
            i++;
            continue;
        }

        size_t nalStart = i + startCodeLen;
        if (nalStart >= length) break;

        int nalType = isHevc ? ((data[nalStart] >> 1) & 0x3F) : (data[nalStart] & 0x1F);

        size_t nalEnd = length;
        for (size_t j = nalStart + 1; j < length - 2; j++) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (j + 3 < length && data[j + 2] == 0 && data[j + 3] == 1))) {
                nalEnd = j;
                break;
            }
        }

        // Remove trailing zeros
        while (nalEnd > nalStart && data[nalEnd - 1] == 0) nalEnd--;

        nalus.push_back({nalType, std::vector<uint8_t>(data + nalStart, data + nalEnd)});
        i = nalEnd;
    }
    return nalus;
}

// Helper: append a NALU array entry for HEVCDecoderConfigurationRecord
static void appendHevcNaluArray(std::vector<uint8_t>& out, uint8_t naluType,
                                const std::vector<uint8_t>& nalu) {
    out.push_back(0x80 | naluType);  // array_completeness=1 | NAL_unit_type
    out.push_back(0);
    out.push_back(1);  // numNalus = 1
    out.push_back((nalu.size() >> 8) & 0xFF);
    out.push_back(nalu.size() & 0xFF);
    out.insert(out.end(), nalu.begin(), nalu.end());
}

// Extract parameter sets from an Annex-B keyframe and build codec-specific extradata
bool RecordingSink::setupPassthroughExtradata(PassthroughContext* ctx, const uint8_t* data,
                                              size_t length) {
    auto nalus = parseAnnexBNalUnits(data, length, ctx->isHevc);
    std::vector<uint8_t> extradata;

    if (ctx->isHevc) {
        // HEVC: find VPS (32), SPS (33), PPS (34)
        std::vector<uint8_t> vps, sps, pps;
        for (auto& [type, nalu] : nalus) {
            if (type == 32 && vps.empty())
                vps = nalu;
            else if (type == 33 && sps.empty())
                sps = nalu;
            else if (type == 34 && pps.empty())
                pps = nalu;
        }
        if (sps.empty() || pps.empty()) {
            AG_LOG_FAST(WARN, "Passthrough HEVC: could not find SPS/PPS in keyframe (%zu bytes)",
                        length);
            return false;
        }

        // Build HEVCDecoderConfigurationRecord (ISO 14496-15 Section 8.3.3.1)
        // Parse profile/level from SPS NAL (skip 2-byte NALU header)
        uint8_t generalProfileSpace = 0, generalTierFlag = 0, generalProfileIdc = 0;
        uint8_t generalLevelIdc = 0;
        uint32_t generalProfileCompatFlags = 0;
        uint8_t generalConstraintFlags[6] = {0};
        if (sps.size() > 15) {
            // SPS starts after 2-byte NALU header: profile_tier_level begins at byte 2
            generalProfileSpace = (sps[2] >> 6) & 0x03;
            generalTierFlag = (sps[2] >> 5) & 0x01;
            generalProfileIdc = sps[2] & 0x1F;
            generalProfileCompatFlags = (sps[3] << 24) | (sps[4] << 16) | (sps[5] << 8) | sps[6];
            for (int k = 0; k < 6; k++) generalConstraintFlags[k] = sps[7 + k];
            generalLevelIdc = sps[13];
        }

        // configurationVersion = 1
        extradata.push_back(1);
        // general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
        extradata.push_back((generalProfileSpace << 6) | (generalTierFlag << 5) |
                            generalProfileIdc);
        // general_profile_compatibility_flags (4 bytes)
        extradata.push_back((generalProfileCompatFlags >> 24) & 0xFF);
        extradata.push_back((generalProfileCompatFlags >> 16) & 0xFF);
        extradata.push_back((generalProfileCompatFlags >> 8) & 0xFF);
        extradata.push_back(generalProfileCompatFlags & 0xFF);
        // general_constraint_indicator_flags (6 bytes)
        for (int k = 0; k < 6; k++) extradata.push_back(generalConstraintFlags[k]);
        // general_level_idc
        extradata.push_back(generalLevelIdc);
        // min_spatial_segmentation_idc (4 bits reserved + 12 bits)
        extradata.push_back(0xF0);
        extradata.push_back(0x00);
        // parallelismType (6 bits reserved + 2 bits)
        extradata.push_back(0xFC);
        // chromaFormat (6 bits reserved + 2 bits) — assume 1 (4:2:0)
        extradata.push_back(0xFD);
        // bitDepthLumaMinus8 (5 bits reserved + 3 bits) — assume 2 for 10-bit
        extradata.push_back(0xFA);
        // bitDepthChromaMinus8 (5 bits reserved + 3 bits)
        extradata.push_back(0xFA);
        // avgFrameRate (0 = unspecified)
        extradata.push_back(0x00);
        extradata.push_back(0x00);
        // constantFrameRate(2) | numTemporalLayers(3) | temporalIdNested(1) |
        // lengthSizeMinusOne(2)
        extradata.push_back(0x03);  // lengthSizeMinusOne=3 (4-byte)
        // numOfArrays
        int numArrays = vps.empty() ? 2 : 3;
        extradata.push_back(numArrays);

        // NAL arrays: VPS, SPS, PPS
        if (!vps.empty()) appendHevcNaluArray(extradata, 32, vps);
        appendHevcNaluArray(extradata, 33, sps);
        appendHevcNaluArray(extradata, 34, pps);

        AG_LOG_FAST(INFO,
                    "Passthrough HEVC: extradata built (%zu bytes, VPS=%zu SPS=%zu PPS=%zu, "
                    "profile=%d level=%d)",
                    extradata.size(), vps.size(), sps.size(), pps.size(), generalProfileIdc,
                    generalLevelIdc);

    } else {
        // H264: find SPS (7) and PPS (8)
        std::vector<uint8_t> sps, pps;
        for (auto& [type, nalu] : nalus) {
            if (type == 7 && sps.empty())
                sps = nalu;
            else if (type == 8 && pps.empty())
                pps = nalu;
        }
        if (sps.empty() || pps.empty()) {
            AG_LOG_FAST(WARN, "Passthrough H264: could not find SPS/PPS in keyframe (%zu bytes)",
                        length);
            return false;
        }

        // Build AVCDecoderConfigurationRecord
        extradata.push_back(1);       // configurationVersion
        extradata.push_back(sps[1]);  // AVCProfileIndication
        extradata.push_back(sps[2]);  // profile_compatibility
        extradata.push_back(sps[3]);  // AVCLevelIndication
        extradata.push_back(0xFF);    // lengthSizeMinusOne = 3 (4-byte NAL length)
        extradata.push_back(0xE1);    // numOfSequenceParameterSets = 1
        extradata.push_back((sps.size() >> 8) & 0xFF);
        extradata.push_back(sps.size() & 0xFF);
        extradata.insert(extradata.end(), sps.begin(), sps.end());
        extradata.push_back(1);  // numOfPictureParameterSets = 1
        extradata.push_back((pps.size() >> 8) & 0xFF);
        extradata.push_back(pps.size() & 0xFF);
        extradata.insert(extradata.end(), pps.begin(), pps.end());

        AG_LOG_FAST(INFO, "Passthrough H264: extradata built (%zu bytes, SPS=%zu PPS=%zu)",
                    extradata.size(), sps.size(), pps.size());
    }

    // Set extradata on codec parameters
    ctx->videoStream->codecpar->extradata =
        (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->videoStream->codecpar->extradata, extradata.data(), extradata.size());
    ctx->videoStream->codecpar->extradata_size = extradata.size();

    ctx->extraDataSet = true;

    // Now open output file and write header
    if (!(ctx->formatContext->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&ctx->formatContext->pb, ctx->filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            AG_LOG_FAST(ERROR, "Passthrough: failed to open output file %s", ctx->filename.c_str());
            return false;
        }
    }

    AVDictionary* opts = nullptr;
    // For MP4, enable faststart for better streaming compatibility
    if (config_.format == OutputFormat::MP4) {
        av_dict_set(&opts, "movflags", "faststart", 0);
    }

    int ret = avformat_write_header(ctx->formatContext, opts ? &opts : nullptr);
    if (opts) av_dict_free(&opts);

    if (ret < 0) {
        AG_LOG_FAST(ERROR, "Passthrough: failed to write header for %s (error %d)",
                    ctx->filename.c_str(), ret);
        return false;
    }

    ctx->headerWritten = true;
    AG_LOG_FAST(INFO, "Passthrough: header written for %s (%s, extradata %d bytes)",
                ctx->filename.c_str(), ctx->isHevc ? "HEVC" : "H264",
                ctx->videoStream->codecpar->extradata_size);
    return true;
}

// Convert Annex-B (start codes) to length-prefixed format for MP4 container
// Skips parameter set NALUs (H264: SPS=7/PPS=8, HEVC: VPS=32/SPS=33/PPS=34)
static std::vector<uint8_t> annexBToAvcc(const uint8_t* data, size_t length, bool isHevc) {
    std::vector<uint8_t> avcc;
    avcc.reserve(length);
    size_t i = 0;

    while (i < length) {
        int startCodeLen = 0;
        if (i + 3 < length && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
            data[i + 3] == 1) {
            startCodeLen = 4;
        } else if (i + 2 < length && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            startCodeLen = 3;
        }

        if (startCodeLen == 0) {
            i++;
            continue;
        }

        size_t nalStart = i + startCodeLen;
        if (nalStart >= length) break;

        size_t nalEnd = length;
        for (size_t j = nalStart; j < length - 2; j++) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (j + 3 < length && data[j + 2] == 0 && data[j + 3] == 1))) {
                nalEnd = j;
                break;
            }
        }

        // Get NAL type and skip parameter sets (already in extradata)
        int nalType = isHevc ? ((data[nalStart] >> 1) & 0x3F) : (data[nalStart] & 0x1F);
        bool isParamSet = isHevc ? (nalType >= 32 && nalType <= 34)  // VPS, SPS, PPS
                                 : (nalType == 7 || nalType == 8);   // SPS, PPS

        if (!isParamSet) {
            uint32_t nalLen = static_cast<uint32_t>(nalEnd - nalStart);
            avcc.push_back((nalLen >> 24) & 0xFF);
            avcc.push_back((nalLen >> 16) & 0xFF);
            avcc.push_back((nalLen >> 8) & 0xFF);
            avcc.push_back(nalLen & 0xFF);
            avcc.insert(avcc.end(), data + nalStart, data + nalEnd);
        }

        i = nalEnd;
    }

    return avcc;
}

bool RecordingSink::writePassthroughVideoPacket(PassthroughContext* ctx, const uint8_t* data,
                                                size_t length, bool isKeyframe,
                                                uint64_t timestampMs) {
    // Initialize time origin
    if (!ctx->hasTimeOrigin) {
        ctx->rtcTimeOrigin = timestampMs;
        ctx->hasTimeOrigin = true;
    }

    // Calculate PTS in stream time base (90kHz)
    int64_t relativeMs = static_cast<int64_t>(timestampMs - ctx->rtcTimeOrigin);
    int64_t pts = av_rescale_q(relativeMs, {1, 1000}, ctx->videoStream->time_base);

    // Ensure monotonic PTS
    if (pts <= ctx->lastVideoPts) {
        pts = ctx->lastVideoPts + 1;
    }
    ctx->lastVideoPts = pts;

    // Convert Annex-B → AVCC (MP4 requires length-prefixed NALUs, not start codes)
    std::vector<uint8_t> avccData = annexBToAvcc(data, length, ctx->isHevc);
    if (avccData.empty()) {
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    pkt->data = avccData.data();
    pkt->size = static_cast<int>(avccData.size());
    pkt->stream_index = ctx->videoStream->index;
    pkt->pts = pts;
    pkt->dts = pts;
    if (isKeyframe) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    int ret = av_interleaved_write_frame(ctx->formatContext, pkt);
    av_packet_free(&pkt);

    if (ret < 0) {
        static int write_err_count = 0;
        if (write_err_count++ % 100 == 0) {
            AG_LOG_FAST(WARN, "Passthrough: write frame failed (error %d, count %d)", ret,
                        write_err_count);
        }
        return false;
    }

    ctx->videoFrameCount++;
    if (ctx->videoFrameCount % 100 == 0) {
        AG_LOG_FAST(INFO, "Passthrough: %lld video frames written for %s",
                    (long long)ctx->videoFrameCount, ctx->filename.c_str());
    }
    return true;
}

void RecordingSink::cleanupPassthroughContext(const std::string& userId) {
    auto it = passthroughContexts_.find(userId);
    if (it == passthroughContexts_.end()) return;

    auto* ctx = it->second.get();

    if (ctx->formatContext) {
        if (ctx->headerWritten) {
            av_write_trailer(ctx->formatContext);
            AG_LOG_FAST(INFO, "Passthrough: finalized %s (%lld video frames)",
                        ctx->filename.c_str(), (long long)ctx->videoFrameCount);
        }
        if (ctx->formatContext->pb && !(ctx->formatContext->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ctx->formatContext->pb);
        }
        avformat_free_context(ctx->formatContext);
        ctx->formatContext = nullptr;
    }

    if (ctx->audioCodecContext) {
        avcodec_free_context(&ctx->audioCodecContext);
    }
    if (ctx->audioFrame) {
        av_frame_free(&ctx->audioFrame);
    }
    if (ctx->swrContext) {
        swr_free(&ctx->swrContext);
    }
}

bool RecordingSink::encodePassthroughAudioFrame(const AudioFrame& frame, PassthroughContext* ctx,
                                                const std::string& userId) {
    if (!ctx || !ctx->audioCodecContext || !ctx->audioFrame || !ctx->headerWritten) {
        return false;  // Can't write audio until header is written (needs SPS/PPS first)
    }

    // Resampling setup (same logic as encodeIndividualAudioFrame)
    bool needsResampling =
        (frame.sampleRate != config_.audioSampleRate || frame.channels != config_.audioChannels);

    if (needsResampling && !ctx->swrContext) {
        AVChannelLayout in_ch_layout, out_ch_layout;
        av_channel_layout_default(&in_ch_layout, frame.channels);
        av_channel_layout_default(&out_ch_layout, config_.audioChannels);

        int ret = swr_alloc_set_opts2(&ctx->swrContext, &out_ch_layout, AV_SAMPLE_FMT_FLTP,
                                      config_.audioSampleRate, &in_ch_layout, AV_SAMPLE_FMT_S16,
                                      frame.sampleRate, 0, nullptr);
        if (ret < 0 || !ctx->swrContext) {
            AG_LOG_FAST(ERROR, "Passthrough: failed to allocate audio resampler for user %s",
                        userId.c_str());
            return false;
        }
        ret = swr_init(ctx->swrContext);
        if (ret < 0) {
            swr_free(&ctx->swrContext);
            return false;
        }
    }

    // Buffer incoming samples
    int input_samples = frame.data.size() / sizeof(int16_t) / frame.channels;
    const int16_t* input_data = reinterpret_cast<const int16_t*>(frame.data.data());

    int samples_per_frame = ctx->audioCodecContext->frame_size;
    if (samples_per_frame == 0) samples_per_frame = 1024;

    size_t current_size = ctx->audioSampleBuffer.size();
    size_t new_count = input_samples * frame.channels;
    ctx->audioSampleBuffer.resize(current_size + new_count);
    std::memcpy(ctx->audioSampleBuffer.data() + current_size, input_data,
                new_count * sizeof(int16_t));
    ctx->lastBufferedTimestamp = frame.timestamp;

    size_t required = samples_per_frame * frame.channels;
    if (ctx->audioSampleBuffer.size() < required) {
        return true;  // Buffering
    }

    // Initialize time origin if needed
    if (!ctx->hasTimeOrigin) {
        ctx->rtcTimeOrigin = frame.timestamp;
        ctx->hasTimeOrigin = true;
    }

    // Calculate PTS in 90kHz timebase
    uint64_t elapsed_ms = frame.timestamp - ctx->rtcTimeOrigin;
    int64_t pts = static_cast<int64_t>(elapsed_ms) * 90;  // ms → 90kHz

    // Set up audio frame
    ctx->audioFrame->nb_samples = samples_per_frame;
    ctx->audioFrame->format = ctx->audioCodecContext->sample_fmt;
    ctx->audioFrame->sample_rate = ctx->audioCodecContext->sample_rate;
    av_channel_layout_copy(&ctx->audioFrame->ch_layout, &ctx->audioCodecContext->ch_layout);
    ctx->audioFrame->pts = av_rescale_q(pts, {1, 90000}, ctx->audioCodecContext->time_base);

    if (av_frame_get_buffer(ctx->audioFrame, 0) < 0) {
        return false;
    }

    // Convert samples to planar float
    if (needsResampling && ctx->swrContext) {
        const uint8_t* input_arr[1] = {
            reinterpret_cast<const uint8_t*>(ctx->audioSampleBuffer.data())};
        int in_count = samples_per_frame * frame.channels / config_.audioChannels;
        int out = swr_convert(ctx->swrContext, ctx->audioFrame->data, samples_per_frame, input_arr,
                              in_count);
        if (out < 0) return false;
        ctx->audioFrame->nb_samples = out;
    } else if (ctx->audioCodecContext->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        const int16_t* buf = ctx->audioSampleBuffer.data();
        for (int ch = 0; ch < config_.audioChannels; ch++) {
            float* out = (float*)ctx->audioFrame->data[ch];
            for (int i = 0; i < samples_per_frame; i++) {
                int src_ch = (frame.channels == 1) ? 0 : std::min(ch, frame.channels - 1);
                out[i] = static_cast<float>(buf[i * frame.channels + src_ch]) / 32768.0f;
            }
        }
        ctx->audioFrame->nb_samples = samples_per_frame;
    }

    // Consume processed samples
    size_t consumed = samples_per_frame * frame.channels;
    if (ctx->audioSampleBuffer.size() > consumed) {
        std::memmove(ctx->audioSampleBuffer.data(), ctx->audioSampleBuffer.data() + consumed,
                     (ctx->audioSampleBuffer.size() - consumed) * sizeof(int16_t));
        ctx->audioSampleBuffer.resize(ctx->audioSampleBuffer.size() - consumed);
    } else {
        ctx->audioSampleBuffer.clear();
    }

    ctx->audioFrameCount++;

    // Encode and write
    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    int ret = avcodec_send_frame(ctx->audioCodecContext, ctx->audioFrame);
    if (ret < 0) {
        av_packet_free(&packet);
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx->audioCodecContext, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_packet_free(&packet);
            return false;
        }

        packet->stream_index = ctx->audioStream->index;
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts = av_rescale_q(packet->pts, ctx->audioCodecContext->time_base,
                                       ctx->audioStream->time_base);
        }
        packet->dts = packet->pts;

        int write_ret = av_interleaved_write_frame(ctx->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            AG_LOG_FAST(ERROR, "Passthrough: failed to write audio for %s: %s", userId.c_str(),
                        errbuf);
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return true;
}

}  // namespace rtc
}  // namespace agora