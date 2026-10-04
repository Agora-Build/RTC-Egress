#pragma once
#include <string>
#include <vector>

#include "../nlohmann/json.hpp"
#include "native_layout.h"

// UDSMessage defines the protocol for communication between Go (egress) and C++ (eg_worker)
struct UDSMessage {
    std::string task_id;             // Task ID for tracking completion
    std::string cmd;                 // "snapshot", "record", "rtmp", or "whip"
    std::string action;              // "start", "stop", "status"
    std::string layout = "flat";     // "flat", "spotlight", "customized", or "freestyle"
    std::string freestyleCanvasUrl;  // URL for custom canvas, used if layout is "freestyle"
    std::vector<std::string> uid;    // User IDs, if empty, all users will be included
    std::string channel;             // Channel Name
    std::string access_token;        // Access token for authentication
    int workerUid = 0;               // Worker UID
    int interval_in_ms = 0;          // Interval in milliseconds
    int videoDecodeMode = -1;        // -1=auto, 0=passthrough, 1=ffmpeg, 2=sdk
    std::vector<agora::rtc::LayoutRegion> regions;
    int width = 0;
    int height = 0;
    std::string output_url;
    std::string output_token;
    int output_timeout_ms = 5000;
};

// UDSCompletionMessage defines the completion response from C++ worker to Go manager
struct UDSCompletionMessage {
    std::string task_id;  // Task ID that completed
    std::string status;   // "success" or "failed"
    std::string error;    // Error message if status is "failed"
    std::string message;  // Additional completion message
};

inline void to_json(nlohmann::json& j, const UDSMessage& m) {
    j = nlohmann::json{{"task_id", m.task_id},
                       {"cmd", m.cmd},
                       {"action", m.action},
                       {"layout", m.layout},
                       {"freestyleCanvasUrl", m.freestyleCanvasUrl},
                       {"uid", m.uid},
                       {"channel", m.channel},
                       {"access_token", m.access_token},
                       {"workerUid", m.workerUid},
                       {"interval_in_ms", m.interval_in_ms},
                       {"videoDecodeMode", m.videoDecodeMode},
                       {"width", m.width},
                       {"height", m.height}};
    j["regions"] = nlohmann::json::array();
    j["output_url"] = m.output_url;
    j["output_token"] = m.output_token;
    j["output_timeout_ms"] = m.output_timeout_ms;
    for (const auto& region : m.regions)
        j["regions"].push_back({{"uid", region.uid},
                                {"x", region.x},
                                {"y", region.y},
                                {"width", region.width},
                                {"height", region.height},
                                {"z", region.z}});
}

inline void to_json(nlohmann::json& j, const UDSCompletionMessage& m) {
    j = nlohmann::json{
        {"task_id", m.task_id}, {"status", m.status}, {"error", m.error}, {"message", m.message}};
}

inline void from_json(const nlohmann::json& j, UDSMessage& m) {
    if (j.contains("task_id")) j.at("task_id").get_to(m.task_id);
    j.at("cmd").get_to(m.cmd);
    if (j.contains("action"))
        j.at("action").get_to(m.action);
    else
        m.action = "start";
    if (j.contains("layout")) j.at("layout").get_to(m.layout);
    if (j.contains("freestyleCanvasUrl")) j.at("freestyleCanvasUrl").get_to(m.freestyleCanvasUrl);
    if (j.contains("uid")) j.at("uid").get_to(m.uid);
    j.at("channel").get_to(m.channel);
    j.at("access_token").get_to(m.access_token);
    if (j.contains("workerUid")) j.at("workerUid").get_to(m.workerUid);
    if (j.contains("interval_in_ms")) j.at("interval_in_ms").get_to(m.interval_in_ms);
    if (j.contains("videoDecodeMode")) j.at("videoDecodeMode").get_to(m.videoDecodeMode);
    if (j.contains("width")) j.at("width").get_to(m.width);
    if (j.contains("height")) j.at("height").get_to(m.height);
    if (j.contains("output_url")) j.at("output_url").get_to(m.output_url);
    if (j.contains("output_token")) j.at("output_token").get_to(m.output_token);
    if (j.contains("output_timeout_ms")) j.at("output_timeout_ms").get_to(m.output_timeout_ms);
    m.regions.clear();
    if (j.contains("regions")) {
        for (const auto& value : j.at("regions")) {
            agora::rtc::LayoutRegion region;
            value.at("uid").get_to(region.uid);
            value.at("x").get_to(region.x);
            value.at("y").get_to(region.y);
            value.at("width").get_to(region.width);
            value.at("height").get_to(region.height);
            region.z = value.value("z", 0);
            m.regions.push_back(region);
        }
    }
}

inline void from_json(const nlohmann::json& j, UDSCompletionMessage& m) {
    j.at("task_id").get_to(m.task_id);
    j.at("status").get_to(m.status);
    if (j.contains("error")) j.at("error").get_to(m.error);
    if (j.contains("message")) j.at("message").get_to(m.message);
}
