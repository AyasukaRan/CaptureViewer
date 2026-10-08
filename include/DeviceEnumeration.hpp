#pragma once

#include "Settings.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct VideoDeviceInfo {
    std::string monikerDisplayName;
    std::string friendlyName;
};

struct AudioCaptureDeviceInfo {
    std::string monikerDisplayName;
    std::string friendlyName;
};

struct AudioRenderDeviceInfo {
    std::string monikerDisplayName;
    std::string friendlyName;
};

struct VideoModeInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frameRate = 0.0;
    VideoFormatPreference format = VideoFormatPreference::Auto;
};

struct IAMStreamConfig;

std::vector<VideoDeviceInfo> enumerateVideoCaptureDevices();
std::vector<AudioCaptureDeviceInfo> enumerateAudioCaptureDevices();
std::vector<AudioRenderDeviceInfo> enumerateAudioRenderDevices();
std::vector<VideoModeInfo> enumerateVideoModes(const std::string& monikerDisplayName);
// Reads capabilities from the already opened capture source; does not open the device again.
std::vector<VideoModeInfo> enumerateVideoModes(IAMStreamConfig* streamConfig);
std::vector<VideoFormatPreference> enumerateVideoFormats(const std::string& monikerDisplayName);
