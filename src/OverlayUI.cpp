#include "OverlayUI.hpp"

#include "Application.hpp"
#include "D3DRenderer.hpp"
#include "Settings.hpp"

#include "imgui.h"
#include "widgets/imgui_toggle.h"
#include "backends/imgui_impl_dx12.h"
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>

extern const unsigned char* const udsgr;
extern const unsigned int udsgr_size;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    struct OverlayDx12DescriptorContext
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    };

    OverlayDx12DescriptorContext g_overlayDx12DescriptorContext{};

    void overlaySrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
    {
        auto* ctx = static_cast<OverlayDx12DescriptorContext*>(info->UserData);
        *outCpu = ctx->cpu;
        *outGpu = ctx->gpu;
    }

    void overlaySrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
    }
}

OverlayUI::~OverlayUI()
{
    shutdown();
}

bool OverlayUI::initialize(HWND hwnd, D3DRenderer& renderer)
{
    if (initialized_)
    {
        return true;
    }

    hwnd_ = hwnd;
    renderer_ = &renderer;
    srvHeap_ = renderer.srvHeap();
    fontCpuHandle_ = renderer.imguiSrvCpuHandle();
    fontGpuHandle_ = renderer.imguiSrvGpuHandle();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (ImFont* menuFont = io.Fonts->AddFontFromMemoryCompressedTTF(udsgr, static_cast<int>(udsgr_size), 15.0f))
    {
        io.FontDefault = menuFont;
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    const ImVec4 switchBg(28.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f, 0.97f);
    const ImVec4 switchBg2(46.0f / 255.0f, 46.0f / 255.0f, 46.0f / 255.0f, 0.97f);
    const ImVec4 switchBg3(60.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 0.97f);
    const ImVec4 switchText(231.0f / 255.0f, 231.0f / 255.0f, 231.0f / 255.0f, 1.0f);
    const ImVec4 switchTextDisabled(150.0f / 255.0f, 150.0f / 255.0f, 150.0f / 255.0f, 1.0f);
    const ImVec4 switchAccent(45.0f / 255.0f, 177.0f / 255.0f, 228.0f / 255.0f, 1.0f);
    const ImVec4 switchAccent2(35.0f / 255.0f, 137.0f / 255.0f, 177.0f / 255.0f, 1.0f);
    const ImVec4 switchAccent3(50.0f / 255.0f, 196.0f / 255.0f, 253.0f / 255.0f, 1.0f);
    const ImVec4 switchScrollbar(96.0f / 255.0f, 96.0f / 255.0f, 96.0f / 255.0f, 1.0f);

    // --- 1. Sizing and Spacing ---
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(7.0f, 7.0f);
    style.CellPadding = ImVec2(8.0f, 8.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 8.0f;
    style.GrabMinSize = 12.0f;

    // --- 2. Borders ---
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    // --- 3. Rounding ---
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.LogSliderDeadzone = 4.0f;
    style.TabRounding = 6.0f;

    // Text
    colors[ImGuiCol_Text] = switchText;
    colors[ImGuiCol_TextDisabled] = switchTextDisabled;

    // Backgrounds
    colors[ImGuiCol_WindowBg] = switchBg;
    colors[ImGuiCol_ChildBg] = switchBg;
    colors[ImGuiCol_PopupBg] = switchBg2;

    // Borders
    colors[ImGuiCol_Border] = switchBg2;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.1f, 0.1f, 0.1f, 0.5f);

    // Frames
    colors[ImGuiCol_FrameBg] = switchBg2;
    colors[ImGuiCol_FrameBgHovered] = switchScrollbar;
    colors[ImGuiCol_FrameBgActive] = switchBg3;

    // Title Bars and Menus
    colors[ImGuiCol_TitleBg] = switchBg;
    colors[ImGuiCol_TitleBgActive] = switchBg2;
    colors[ImGuiCol_TitleBgCollapsed] = switchBg;
    colors[ImGuiCol_MenuBarBg] = switchBg;

    // Scrollbars
    colors[ImGuiCol_ScrollbarBg] = switchBg;
    colors[ImGuiCol_ScrollbarGrab] = switchScrollbar;
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(106.0f / 255.0f, 106.0f / 255.0f, 106.0f / 255.0f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(122.0f / 255.0f, 122.0f / 255.0f, 122.0f / 255.0f, 1.0f);

    // Interactables
    colors[ImGuiCol_CheckMark] = switchAccent;
    colors[ImGuiCol_SliderGrab] = switchAccent;
    colors[ImGuiCol_SliderGrabActive] = switchText;
    colors[ImGuiCol_Button] = switchAccent;
    colors[ImGuiCol_ButtonHovered] = switchAccent3;
    colors[ImGuiCol_ButtonActive] = switchAccent;
    colors[ImGuiCol_Header] = switchAccent2;
    colors[ImGuiCol_HeaderHovered] = switchAccent3;
    colors[ImGuiCol_HeaderActive] = switchAccent;

    // Tabs and misc
    colors[ImGuiCol_Tab] = switchBg2;
    colors[ImGuiCol_TabHovered] = switchScrollbar;
    colors[ImGuiCol_TabSelected] = switchAccent;
    colors[ImGuiCol_PlotLines] = switchAccent;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(switchAccent.x, switchAccent.y, switchAccent.z, 0.35f);

    ImGui_ImplWin32_Init(hwnd_);

    g_overlayDx12DescriptorContext.cpu = fontCpuHandle_;
    g_overlayDx12DescriptorContext.gpu = fontGpuHandle_;

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = renderer.device();
    initInfo.CommandQueue = renderer.commandQueue();
    initInfo.NumFramesInFlight = static_cast<int>(renderer.frameCount());
    initInfo.RTVFormat = renderer.renderTargetFormat();
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = srvHeap_;
    initInfo.SrvDescriptorAllocFn = overlaySrvAlloc;
    initInfo.SrvDescriptorFreeFn = overlaySrvFree;
    initInfo.UserData = &g_overlayDx12DescriptorContext;

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    initialized_ = true;
    return true;
}

void OverlayUI::shutdown()
{
    if (!initialized_)
    {
        return;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
    menuVisible_ = false;
    srvHeap_ = nullptr;
    renderer_ = nullptr;
    drawData_ = nullptr;
    drawDataValid_ = false;
}

void OverlayUI::newFrame()
{
    if (!initialized_)
    {
        return;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
}

void OverlayUI::buildUI(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    if (app.settings().showLatencyOverlay)
    {
        drawPerformanceOverlay(app);
    }
    if (menuVisible_)
    {
        drawMenuWindow(app);
    }
}

void OverlayUI::endFrame()
{
    if (!initialized_)
    {
        return;
    }

    ImGui::Render();
    drawData_ = ImGui::GetDrawData();
    drawDataValid_ = (drawData_ != nullptr) && (drawData_->CmdListsCount > 0);
}

void OverlayUI::render(ID3D12GraphicsCommandList* commandList)
{
    if (!initialized_ || !drawData_)
    {
        drawDataValid_ = false;
        return;
    }

    if (drawData_->CmdListsCount > 0)
    {
        ID3D12DescriptorHeap* heaps[] = {srvHeap_};
        commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(drawData_, commandList);
    }

    drawData_ = nullptr;
    drawDataValid_ = false;
}

bool OverlayUI::processEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!initialized_)
    {
        return false;
    }
    bool handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    return menuVisible_ && handled;
}

void OverlayUI::toggleMenu(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    if (menuVisible_)
    {
        hideMenu(app);
    }
    else
    {
        showMenu(app);
    }
}

void OverlayUI::hideMenu(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    if (!menuVisible_)
    {
        return;
    }
    menuVisible_ = false;
    drawDataValid_ = false;
    SetCursor(nullptr);
    app.requestImmediateRender();
}

void OverlayUI::showMenu(Application& app)
{
    if (!initialized_)
    {
        return;
    }
    menuVisible_ = true;
    refreshDeviceLists(app);
    app.requestImmediateRender();
}

void OverlayUI::refreshDeviceLists(Application& app)
{
    videoDevices_ = enumerateVideoCaptureDevices();
    audioDevices_ = enumerateAudioCaptureDevices();
    audioRenderDevices_ = enumerateAudioRenderDevices();

    refreshVideoModes(app);
    refreshVideoFormats(app);
}

void OverlayUI::refreshVideoModes(Application& app)
{
    videoModes_.clear();
    const std::string& moniker = app.settings().videoDeviceMoniker;
    if (moniker.empty())
    {
        return;
    }

    videoModes_ = enumerateVideoModes(moniker);
}

void OverlayUI::refreshVideoFormats(Application& app)
{
    videoFormats_.clear();
    const std::string& moniker = app.settings().videoDeviceMoniker;
    if (moniker.empty())
    {
        return;
    }

    videoFormats_ = enumerateVideoFormats(moniker);
}

void OverlayUI::drawPerformanceOverlay(Application& app)
{
    const auto& timing = app.frameTiming_;
    const auto& rates = app.frameRates_;
    const auto now = std::chrono::steady_clock::now();
    const bool stale = timing.hasSamples() &&
        now - timing.lastReceivedAt() > std::chrono::seconds(1);
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, ImGui::GetIO().DisplaySize.x - 24.0f), 0.0f));
    ImGui::SetNextWindowBgAlpha(0.78f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Performance overlay", nullptr, flags))
    {
        if (rates.hasSample())
        {
            ImGui::TextWrapped("CAPTURE  %.1f FPS    PREVIEW  %.1f FPS    SKIPPED  %llu",
                               rates.captureFps(), rates.previewFps(),
                               static_cast<unsigned long long>(app.skippedFrameCount_));
        }
        else
        {
            ImGui::TextWrapped("CAPTURE  -- FPS    PREVIEW  -- FPS    SKIPPED  %llu",
                               static_cast<unsigned long long>(app.skippedFrameCount_));
        }
        if (timing.hasSamples() && !stale)
        {
            ImGui::TextWrapped("APP LATENCY  %.2f ms    Average  %.2f ms    Peak  %.2f ms",
                               timing.latestMs(), timing.averageMs(), timing.maximumMs());
        }
        else
        {
            ImGui::TextUnformatted(stale ? "APP LATENCY  --  No recent preview frames" :
                                                   "APP LATENCY  --  Waiting for video frames");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("App latency only; excludes hardware/screen delay.    M: Settings    F10: Overlay    F11: Fullscreen");
        ImGui::PopStyleColor();
        performanceOverlayHeight_ = ImGui::GetWindowHeight();
    }
    ImGui::End();
}

void OverlayUI::drawMenuWindow(Application& app)
{
    ImGuiIO& io = ImGui::GetIO();

    const float panelWidth = std::max(1.0f, std::min(600.0f, io.DisplaySize.x - 24.0f));
    const float panelTop = app.settings().showLatencyOverlay ? performanceOverlayHeight_ + 24.0f : 12.0f;
    const float panelHeight = std::max(1.0f, std::min(760.0f, io.DisplaySize.y - panelTop - 12.0f));
    ImVec2 panelPos((io.DisplaySize.x - panelWidth) * 0.5f,
                   std::max(panelTop, (io.DisplaySize.y - panelHeight) * 0.5f));
    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight));

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CaptureViewer Settings", &menuVisible_, windowFlags))
    {
        ImGui::End();
        if (!menuVisible_)
        {
            hideMenu(app);
        }
        return;
    }

    ImGui::TextDisabled("M / Esc: Close    F11: Fullscreen    F10: Performance overlay");
    if (ImGui::BeginTabBar("SettingsTabs"))
    {
    if (ImGui::BeginTabItem("Display"))
    {
        ImGui::Spacing();
        const char* windowModes[] = {"Windowed", "Borderless window", "Fullscreen (borderless)"};
        int windowMode = app.settings().videoFullscreen ? 2 : (app.settings().videoBorderlessWindowed ? 1 : 0);
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x * 0.55f));
        if (ImGui::Combo("Window mode", &windowMode, windowModes, IM_ARRAYSIZE(windowModes)))
        {
            app.setWindowMode(windowMode);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 5.0f));
        bool allowResizing = app.settings().videoAllowResizing;
        if (ImGui::Toggle("Allow window resizing", &allowResizing, ImGuiToggleFlags_Animated))
        {
            app.setVideoAllowResizing(allowResizing);
        }
        bool vsyncEnabled = app.settings().vsyncEnabled;
        if (ImGui::Toggle("VSync", &vsyncEnabled, ImGuiToggleFlags_Animated))
        {
            app.setVSyncEnabled(vsyncEnabled);
        }
        bool showOverlay = app.settings().showLatencyOverlay;
        if (ImGui::Toggle("Latency and FPS overlay", &showOverlay, ImGuiToggleFlags_Animated))
        {
            app.setShowLatencyOverlay(showOverlay);
        }
        ImGui::PopStyleVar();
        ImGui::TextWrapped("Latest frame only: old frames are skipped. Disable VSync for lower latency; enable it to reduce tearing.");
        ImGui::Spacing();
        ImGui::SeparatorText("Resolution scaling");
        const char* scalingModes[] = {"Stretch to window", "Fit (keep aspect ratio)", "Native (downscale only)",
                                      "Fill (crop edges)", "Custom source scale"};
        int scaling = static_cast<int>(app.settings().videoAspectMode);
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x * 0.55f));
        if (ImGui::Combo("Scaling mode", &scaling, scalingModes, IM_ARRAYSIZE(scalingModes)))
        {
            app.setVideoAspectMode(static_cast<VideoAspectMode>(std::clamp(scaling, 0, 4)));
        }
        if (app.settings().videoAspectMode == VideoAspectMode::Custom)
        {
            int percent = static_cast<int>(app.settings().videoScalePercent);
            ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x * 0.55f));
            if (ImGui::SliderInt("Source scale", &percent, 25, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            {
                app.setVideoScalePercent(static_cast<unsigned int>(percent));
            }
            if (ImGui::BeginTable("ScalePresets", 3, ImGuiTableFlags_SizingStretchSame))
            {
                for (const unsigned int preset : {50u, 75u, 100u, 125u, 150u, 200u})
                {
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(preset));
                    const std::string label = std::to_string(preset) + "%";
                    if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                    {
                        app.setVideoScalePercent(preset);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::TextWrapped("Preview scaling only; the Capture tab changes the input format. Custom 100%% = one source pixel per display pixel. Larger images are cropped.");
        RECT client{};
        if (GetClientRect(app.hwnd(), &client))
        {
            bool valid = false;
            const RECT viewport = app.computeVideoViewport(client, valid);
            if (valid)
            {
                ImGui::TextDisabled("Source %ux%u  ->  Preview %ldx%ld  |  Window %ldx%ld",
                    app.currentCaptureWidth(), app.currentCaptureHeight(),
                    viewport.right - viewport.left, viewport.bottom - viewport.top,
                    client.right - client.left, client.bottom - client.top);
            }
        }
        ImGui::BeginDisabled(app.settings().videoFullscreen);
        if (ImGui::Button("Recenter window")) { app.recenterWindow(); }
        ImGui::EndDisabled();
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("About performance readings"))
        {
        ImGui::TextWrapped("App latency measures receipt of a video frame through the return of its Present call. Average and peak cover up to 120 completed video frames. It is not HDMI-to-screen latency.");
        ImGui::TextWrapped("Capture FPS counts video callbacks; Preview FPS counts new video frames successfully submitted for presentation. The overlay refreshes idle status at most four times per second, and FPS updates every half second. GPU scanout is not measured.");
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Capture"))
    {
    ImGui::Spacing();
    if (ImGui::Button("Refresh devices and modes")) { refreshDeviceLists(app); }
    if (!app.captureStatus_.empty())
    {
        ImGui::TextWrapped("Capture unavailable: %s", app.captureStatus_.c_str());
        ImGui::TextWrapped("Connect a device, refresh the list, then select a capture source.");
        if (ImGui::Button("Retry capture")) { app.restartVideoCapture(); }
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Video Capture Devices");
    ImGui::BeginChild("VideoDevices", ImVec2(0.0f, 80), ImGuiChildFlags_Borders);
    const std::string& currentVideo = app.settings().videoDeviceMoniker;
    if (videoDevices_.empty())
    {
        ImGui::TextDisabled("No video capture devices detected");
    }
    else
    {
        for (const auto& device : videoDevices_)
        {
            std::string label = !device.friendlyName.empty() ? device.friendlyName : device.monikerDisplayName;
            bool selected = (!currentVideo.empty() && currentVideo == device.monikerDisplayName);
            if (ImGui::Selectable(label.c_str(), selected))
            {
                app.selectVideoDevice(device.monikerDisplayName);
                refreshVideoModes(app);
                refreshVideoFormats(app);
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();

    std::string resolutionLabel;
    if (app.settings().videoPreferredWidth == 0 || app.settings().videoPreferredHeight == 0)
    {
        resolutionLabel = "1920x1080";
    }
    else
    {
        resolutionLabel = std::to_string(app.settings().videoPreferredWidth) + "x" + std::to_string(app.settings().videoPreferredHeight);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> resolutionOptions;
    resolutionOptions.reserve(videoModes_.size());
    for (const auto& mode : videoModes_)
    {
        const auto exists = std::find_if(resolutionOptions.begin(), resolutionOptions.end(), [&](const auto& entry) {
            return entry.first == mode.width && entry.second == mode.height;
        });
        if (exists == resolutionOptions.end())
        {
            resolutionOptions.emplace_back(mode.width, mode.height);
        }
    }

    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.5f);
    if (ImGui::BeginCombo("Capture Resolution", resolutionLabel.c_str()))
    {
        if (resolutionOptions.empty())
        {
			ImGui::TextDisabled("No resolutions detected for this device");
        }
        else {
            for (const auto& [width, height] : resolutionOptions)
            {
                const std::string modeLabel = std::to_string(width) + "x" + std::to_string(height);
                const bool selected = app.settings().videoPreferredWidth == width &&
                    app.settings().videoPreferredHeight == height;
                if (ImGui::Selectable(modeLabel.c_str(), selected))
                {
                    app.setVideoResolution(width, height);

                    std::vector<std::uint32_t> ratesForResolution;
                    for (const auto& candidate : videoModes_)
                    {
                        if (candidate.width == width && candidate.height == height)
                        {
                            const std::uint32_t rate100 = static_cast<std::uint32_t>(std::llround(candidate.frameRate * 100.0));
                            if (rate100 != 0 && std::find(ratesForResolution.begin(), ratesForResolution.end(), rate100) == ratesForResolution.end())
                            {
                                ratesForResolution.push_back(rate100);
                            }
                        }
                    }
                    if (!ratesForResolution.empty())
                    {
                        std::sort(ratesForResolution.begin(), ratesForResolution.end());
                        std::uint32_t preferred = ratesForResolution.front();
                        if (std::find(ratesForResolution.begin(), ratesForResolution.end(), 6000) != ratesForResolution.end())
                        {
                            preferred = 6000;
                        }
                        else if (std::find(ratesForResolution.begin(), ratesForResolution.end(), app.settings().videoPreferredFrameRate100) != ratesForResolution.end())
                        {
                            preferred = app.settings().videoPreferredFrameRate100;
                        }
                        app.setVideoFrameRate100(preferred);
                    }
                }
            }
        }

        ImGui::EndCombo();
    }

    const std::uint32_t selectedWidth = app.settings().videoPreferredWidth;
    const std::uint32_t selectedHeight = app.settings().videoPreferredHeight;
    std::vector<std::uint32_t> refreshRateOptions100;
    refreshRateOptions100.reserve(videoModes_.size());
    for (const auto& mode : videoModes_)
    {
        if (mode.width == selectedWidth && mode.height == selectedHeight)
        {
            const std::uint32_t rate100 = static_cast<std::uint32_t>(std::llround(mode.frameRate * 100.0));
            if (rate100 == 0)
            {
                continue;
            }
            if (std::find(refreshRateOptions100.begin(), refreshRateOptions100.end(), rate100) == refreshRateOptions100.end())
            {
                refreshRateOptions100.push_back(rate100);
            }
        }
    }
    std::sort(refreshRateOptions100.begin(), refreshRateOptions100.end());

    const auto frameRateLabel = [&](std::uint32_t rate100) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (static_cast<double>(rate100) / 100.0) << " Hz";
        return oss.str();
    };

    std::string refreshLabel = frameRateLabel(app.settings().videoPreferredFrameRate100 != 0 ? app.settings().videoPreferredFrameRate100 : 6000);
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.5f);
    if (ImGui::BeginCombo("Capture Refresh Rate", refreshLabel.c_str()))
    {
        if (refreshRateOptions100.empty())
        {
            ImGui::TextDisabled("No refresh rates detected for this resolution");
        }
        else
        {
            for (const std::uint32_t rate100 : refreshRateOptions100)
            {
                const std::string rateText = frameRateLabel(rate100);
                const bool selected = app.settings().videoPreferredFrameRate100 == rate100;
                if (ImGui::Selectable(rateText.c_str(), selected))
                {
                    app.setVideoFrameRate100(rate100);
                }
            }
        }

        ImGui::EndCombo();
    }

    const VideoFormatPreference currentFormat = app.settings().videoFormatPreference;
    const auto formatLabel = [](VideoFormatPreference value) {
        switch (value)
        {
        case VideoFormatPreference::XRGB:
            return "XRGB";
        case VideoFormatPreference::NV12:
            return "NV12";
        default:
            return "Auto";
        }
    };

    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.5f);
    if (ImGui::BeginCombo("Capture Format", formatLabel(currentFormat)))
    {
        const bool autoSelected = currentFormat == VideoFormatPreference::Auto;
        if (ImGui::Selectable("Auto", autoSelected))
        {
            app.setVideoFormatPreference(VideoFormatPreference::Auto);
        }

        bool hasXrgb = false;
        bool hasNv12 = false;
        for (VideoFormatPreference format : videoFormats_)
        {
            hasXrgb = hasXrgb || (format == VideoFormatPreference::XRGB);
            hasNv12 = hasNv12 || (format == VideoFormatPreference::NV12);
        }

        if (hasXrgb)
        {
            const bool selected = currentFormat == VideoFormatPreference::XRGB;
            if (ImGui::Selectable("XRGB", selected))
            {
                app.setVideoFormatPreference(VideoFormatPreference::XRGB);
            }
        }

        if (hasNv12)
        {
            const bool selected = currentFormat == VideoFormatPreference::NV12;
            if (ImGui::Selectable("NV12", selected))
            {
                app.setVideoFormatPreference(VideoFormatPreference::NV12);
            }
        }

        if (!hasXrgb && !hasNv12)
        {
            ImGui::TextDisabled("No supported formats detected for this device");
		}

        ImGui::EndCombo();
    }

    ImGui::Spacing();

    const std::uint32_t signalWidth = app.currentCaptureWidth();
    const std::uint32_t signalHeight = app.currentCaptureHeight();
    const std::uint32_t signalRate100 = app.currentCaptureFrameRate100();
    if (signalWidth != 0 && signalHeight != 0)
    {
        if (signalRate100 != 0)
        {
            ImGui::TextDisabled("Current Signal: %ux%u @ %.2f Hz", signalWidth, signalHeight, static_cast<double>(signalRate100) / 100.0);
        }
        else
        {
            ImGui::TextDisabled("Current Signal: %ux%u", signalWidth, signalHeight);
        }
    }
    else
    {
        ImGui::TextDisabled("Current Signal: awaiting frames");
    }

    ImGui::Spacing();
    ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Audio"))
    {
    if (ImGui::Button("Refresh audio devices")) { refreshDeviceLists(app); }
    ImGui::TextUnformatted("Audio Settings");
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 5.0f));
    bool audioPlayback = app.settings().audioPlaybackEnabled;
    if (ImGui::Toggle("Enable Audio Playback", &audioPlayback, ImGuiToggleFlags_Animated))
    {
        app.setAudioPlaybackEnabled(audioPlayback);
    }

    if (!audioPlayback)
    {
        ImGui::BeginDisabled();
	}
    ImGui::PopStyleVar();
    ImGui::TextUnformatted("Audio Capture Input");
    ImGui::BeginChild("AudioDevices", ImVec2(0.0f, 80), ImGuiChildFlags_Borders);
    const std::string& currentAudio = app.settings().audioDeviceMoniker;
    bool useVideoAudio = currentAudio == "@video" || currentAudio.empty();
    if (ImGui::Selectable("Use Video Source Audio", useVideoAudio))
    {
        app.selectAudioDevice("@video");
    }
    if (audioDevices_.empty())
    {
        ImGui::TextDisabled("No dedicated audio capture devices detected");
    }
    else
    {
        for (const auto& device : audioDevices_)
        {
            std::string label = !device.friendlyName.empty() ? device.friendlyName : device.monikerDisplayName;
            bool selected = (!currentAudio.empty() && currentAudio == device.monikerDisplayName);
            if (ImGui::Selectable(label.c_str(), selected))
            {
                app.selectAudioDevice(device.monikerDisplayName);
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextUnformatted("Audio Capture Output");
    bool defaultOnly = app.settings().audioOutputUseDefaultOnly;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 5.0f));
    if (ImGui::Toggle("Use Default Output Device", &defaultOnly, ImGuiToggleFlags_Animated))
    {
        app.setAudioOutputUseDefaultOnly(defaultOnly);
    }
    ImGui::PopStyleVar();

    ImGui::BeginChild("AudioOutputDevices", ImVec2(0.0f, 125), ImGuiChildFlags_Borders);
    if (defaultOnly)
    {
        ImGui::BeginDisabled();
    }

    if (audioRenderDevices_.empty())
    {
        ImGui::TextDisabled("No audio output devices detected");
    }
    else
    {
        const auto& selectedOutputs = app.settings().audioOutputDeviceMonikers;
        for (const auto& device : audioRenderDevices_)
        {
            std::string label = !device.friendlyName.empty() ? device.friendlyName : device.monikerDisplayName;
            bool selected = std::find(selectedOutputs.begin(), selectedOutputs.end(), device.monikerDisplayName) != selectedOutputs.end();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 5.0f));
            if (ImGui::Toggle(label.c_str(), &selected, ImGuiToggleFlags_Animated))
            {
                app.setAudioOutputDeviceSelected(device.monikerDisplayName, selected);
            }
			ImGui::PopStyleVar();
        }
    }

    if (defaultOnly)
    {
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
	if (!audioPlayback)
    {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();

    ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    }

    if (ImGui::IsKeyReleased(ImGuiKey_Escape))
    {
        hideMenu(app);
    }

    ImGui::End();

    if (!menuVisible_)
    {
        hideMenu(app);
    }
}
