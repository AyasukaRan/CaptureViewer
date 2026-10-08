#pragma once

#include <string>
#include <filesystem>
#include <vector>

enum class VideoAspectMode : unsigned int {
    Stretch = 0,
    Maintain = 1,
    Capture = 2,
    Fill = 3,
    Custom = 4,
};

enum class VideoFormatPreference : unsigned int {
    Auto = 0,
    XRGB = 1,
    NV12 = 2,
};

struct AppSettings {
    std::string videoDeviceMoniker;
    std::string audioDeviceMoniker;
    bool audioPlaybackEnabled = true;
    unsigned int videoPreferredWidth = 1920;
    unsigned int videoPreferredHeight = 1080;
    unsigned int videoPreferredFrameRate100 = 6000;
    bool videoAllowResizing = true;
    bool videoBorderlessWindowed = true;
    bool videoFullscreen = false;
    bool vsyncEnabled = false;
    VideoAspectMode videoAspectMode = VideoAspectMode::Maintain;
    unsigned int videoScalePercent = 100;
    bool showLatencyOverlay = true;
    VideoFormatPreference videoFormatPreference = VideoFormatPreference::Auto;
    int windowPosX = 0;
    int windowPosY = 0;
    unsigned int windowClientWidth = 0;
    unsigned int windowClientHeight = 0;
    bool hasWindowPlacement = false;
    bool windowWasMaximized = false;
    bool audioOutputUseDefaultOnly = true;
    std::vector<std::string> audioOutputDeviceMonikers;
};

class SettingsManager {
public:
    SettingsManager();

    AppSettings load();
    void save(const AppSettings& settings) const;

    [[nodiscard]] const std::filesystem::path& settingsFile() const noexcept { return settingsFile_; }


private:
    std::filesystem::path settingsFile_;
    static std::filesystem::path determineSettingsPath();
};
