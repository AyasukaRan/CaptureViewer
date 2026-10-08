#include "Application.hpp"
#include "DeviceEnumeration.hpp"
#include "VideoViewport.hpp"

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif


#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <cmath>
#include <shellapi.h>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"Viewer.Window";
    constexpr int kDefaultWidth = 1920;
    constexpr int kDefaultHeight = 1080;

    constexpr UINT_PTR kTimerRenderDuringInteraction = 0x7101;
    const std::string kAudioSourceVideoSentinel = "@video";

    DirectShowCapture::VideoFormatPreference toCaptureVideoFormat(VideoFormatPreference preference)
    {
        switch (preference)
        {
        case VideoFormatPreference::NV12:
            return DirectShowCapture::VideoFormatPreference::NV12;
        case VideoFormatPreference::Auto:
            return DirectShowCapture::VideoFormatPreference::Auto;
        case VideoFormatPreference::XRGB:
        default:
            return DirectShowCapture::VideoFormatPreference::XRGB;
        }
    }

    std::wstring utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return L"";
        }
        const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (required <= 0)
        {
            return L"";
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), required);
        return result;
    }

    void logApp(const std::string& message)
    {
        std::ofstream("viewer.log", std::ios::app) << message << '\n';
    }

    std::size_t frameBufferBytes(DirectShowCapture::PixelFormat format,
                                 std::uint32_t stride,
                                 std::uint32_t height)
    {
        const std::size_t rowStride = static_cast<std::size_t>(stride);
        switch (format)
        {
        case DirectShowCapture::PixelFormat::NV12:
            return rowStride * height + rowStride * ((height + 1u) / 2u);
        case DirectShowCapture::PixelFormat::BGRA8:
        default:
            return rowStride * height;
        }
    }

    D3DRenderer::FrameFormat toRendererFormat(DirectShowCapture::PixelFormat format)
    {
        switch (format)
        {
        case DirectShowCapture::PixelFormat::NV12:
            return D3DRenderer::FrameFormat::NV12;
        case DirectShowCapture::PixelFormat::BGRA8:
        default:
            return D3DRenderer::FrameFormat::BGRA8;
        }
    }

    RECT clampWindowRectToNearestWorkArea(const RECT& rect)
    {
        RECT result = rect;
        const int width = result.right - result.left;
        const int height = result.bottom - result.top;
        if (width <= 0 || height <= 0)
        {
            return result;
        }

        const HMONITOR monitor = MonitorFromRect(&result, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
        {
            return result;
        }

        const RECT& work = monitorInfo.rcWork;
        const int workWidth = work.right - work.left;
        const int workHeight = work.bottom - work.top;
        const int clampedWidth = std::min(width, workWidth);
        const int clampedHeight = std::min(height, workHeight);

        result.left = std::clamp(result.left, work.left, work.right - clampedWidth);
        result.top = std::clamp(result.top, work.top, work.bottom - clampedHeight);
        result.right = result.left + width;
        result.bottom = result.top + height;
        return result;
    }
}

Application::Application()
{
    frameReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Application::~Application()
{
    running_ = false;
    if (frameReadyEvent_)
    {
        SetEvent(frameReadyEvent_);
    }
    audioPlayback_.stop();
    directShowCapture_.stop();
    renderer_.shutdown();
    captureWindowPlacementForPersistence();
    destroyWindow();
    if (frameReadyEvent_)
    {
        CloseHandle(frameReadyEvent_);
        frameReadyEvent_ = nullptr;
    }
}

int Application::run()
{
    {
        std::ofstream("viewer.log", std::ios::trunc) << "[App] Launching viewer" << std::endl;
    }
    logApp("[App] Starting initialization");

    loadPersistentSettings();
    parseCommandLine();
    audioEnabled_ = shouldEnableCaptureAudio();
    logApp(std::string("[App] Audio capture ") + (audioEnabled_ ? "enabled" : "disabled"));

    if (!createWindow(kDefaultWidth, kDefaultHeight))
    {
        logApp("[App] Failed to create window");
        return EXIT_FAILURE;
    }

    if (!renderer_.initialize(hwnd_))
    {
        logApp("[App] Failed to initialize renderer");
        destroyWindow();
        return EXIT_FAILURE;
    }
    renderer_.setVSyncEnabled(settings_.vsyncEnabled);
    logApp(std::string("[App] VSync ") + (settings_.vsyncEnabled ? "enabled" : "disabled"));
    logApp("[App] Renderer initialized");

    if (!overlay_.initialize(hwnd_, renderer_))
    {
        logApp("[App] Failed to initialize ImGui overlay");
        // Continue without overlay
    }

    running_ = true;

    logApp("[App] Starting DirectShow capture");

    try
    {
        DirectShowCapture::Options captureOptions;
        captureOptions.deviceMoniker = settings_.videoDeviceMoniker;
        captureOptions.enableAudio = audioEnabled_;
        captureOptions.desiredWidth = settings_.videoPreferredWidth;
        captureOptions.desiredHeight = settings_.videoPreferredHeight;
        captureOptions.desiredFrameRate100 = settings_.videoPreferredFrameRate100;
        captureOptions.videoFormatPreference = toCaptureVideoFormat(settings_.videoFormatPreference);

        directShowCapture_.start([this](const DirectShowCapture::Frame& frame) {
            handleFrame(frame);
        }, captureOptions);
        captureStatus_.clear();
        logApp("[App] DirectShow capture started successfully");
    }
    catch (const std::exception& ex)
    {
        captureStatus_ = ex.what();
        logApp(std::string("[App] DirectShow capture start failed: ") + ex.what());
    }
    catch (...)
    {
        captureStatus_ = "Could not start video capture.";
        logApp("[App] DirectShow capture start failed: unknown exception");
    }
    syncCaptureDeviceInfo();
    if (!captureStatus_.empty()) { showSettingsMenu(); }

    applyAudioPlaybackSetting();

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    logApp("[App] Entering render loop");
    renderLoop();
    logApp("[App] Render loop exited");

    audioPlayback_.stop();

    directShowCapture_.stop();
    logApp("[App] DirectShow capture stopped");
    std::string captureError = directShowCapture_.consumeLastError();
    const bool anyFrames = frameCounter_.load(std::memory_order_acquire) > 0;

    overlay_.shutdown();
    renderer_.shutdown();
    logApp("[App] Renderer shutdown");

    if (captureError.empty() && !anyFrames)
    {
        const std::string deviceLabel = directShowCapture_.currentDeviceFriendlyName();
        captureError = "No video frames received from '" + (deviceLabel.empty() ? std::string("the selected capture device") : deviceLabel) + "'. Confirm a valid input signal and that no other application is using the device.";
    }

    if (!captureError.empty())
    {
        logApp(std::string("[App] Reporting error: ") + captureError);
    }

    captureWindowPlacementForPersistence();
    destroyWindow();
    logApp("[App] Window destroyed");

    SetThreadExecutionState(ES_CONTINUOUS);

    return captureError.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
}

void Application::parseCommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        return;
    }

    bool enableAudio = settings_.audioPlaybackEnabled;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring_view arg(argv[i]);
        if (arg == L"--enable-audio" || arg == L"--audio")
        {
            enableAudio = true;
        }
        else if (arg == L"--disable-audio" || arg == L"--no-audio")
        {
            enableAudio = false;
        }
    }

    if (enableAudio != settings_.audioPlaybackEnabled)
    {
        settings_.audioPlaybackEnabled = enableAudio;
        if (settings_.audioPlaybackEnabled && settings_.audioDeviceMoniker.empty())
        {
            settings_.audioDeviceMoniker = kAudioSourceVideoSentinel;
        }
        savePersistentSettings();
    }

    audioEnabled_ = shouldEnableCaptureAudio();
    LocalFree(argv);
}

LRESULT CALLBACK Application::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* self = static_cast<Application*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    auto* self = reinterpret_cast<Application*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!self)
    {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_KEYDOWN && (lParam & (1LL << 30)) == 0)
    {
        if (wParam == VK_F11)
        {
            self->setFullscreen(!self->settings_.videoFullscreen);
            return 0;
        }
        if (wParam == VK_F10)
        {
            self->setShowLatencyOverlay(!self->settings_.showLatencyOverlay);
            return 0;
        }
        if (wParam == 'M')
        {
            self->showSettingsMenu();
            return 0;
        }
    }

    if (self->overlay_.processEvent(hwnd, msg, wParam, lParam))
    {
        return 1;
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        // Direct3D owns the entire client area; GDI erases can cover the HUD
        // after switching between decorated and borderless window styles.
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        self->requestImmediateRender();
        return 0;
    }
    case WM_SIZE:
    {
        const UINT width = LOWORD(lParam);
        const UINT height = HIWORD(lParam);
        if (wParam == SIZE_MINIMIZED || width == 0 || height == 0)
        {
            return 0;
        }
        if (wParam == SIZE_MAXIMIZED)
        {
            self->suppressCaptureDrivenResize_ = true;
        }
        else if (wParam == SIZE_RESTORED)
        {
            if (self->initialMaximizePending_)
            {
                self->initialMaximizeRestoreSeen_ = true;
                self->initialMaximizePending_ = false;
                self->renderer_.onResize(width, height);
                logApp("[App] WM_SIZE -> " + std::to_string(width) + "x" + std::to_string(height));
                return 0;
            }

            const bool wasSuppressed = self->suppressCaptureDrivenResize_;
            self->suppressCaptureDrivenResize_ = false;
            if (wasSuppressed)
            {
                const std::uint32_t srcW = self->currentSourceWidth_.load(std::memory_order_acquire);
                const std::uint32_t srcH = self->currentSourceHeight_.load(std::memory_order_acquire);
                if (srcW > 0 && srcH > 0 && !self->settings_.videoFullscreen)
                {
                    self->resizeWindowToClient(static_cast<int>(srcW), static_cast<int>(srcH));
                    self->updateWindowResizeMode();
                }
            }
        }
        self->renderer_.onResize(width, height);
        logApp("[App] WM_SIZE -> " + std::to_string(width) + "x" + std::to_string(height));
        return 0;
    }
    case WM_MOVE:
        return 0;
    case WM_ENTERSIZEMOVE:
        SetTimer(hwnd, kTimerRenderDuringInteraction, 16, nullptr);
        self->renderFrame(true);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hwnd, kTimerRenderDuringInteraction);
        self->renderFrame(true);
        return 0;
    case WM_TIMER:
        if (wParam == kTimerRenderDuringInteraction)
        {
            self->renderFrame(true);
            return 0;
        }
        return 0;
    case WM_SHOWWINDOW:
        return 0;
    case WM_ACTIVATE:
        return 0;
    case WM_GETMINMAXINFO:
        if (self->applyLockedWindowSize(reinterpret_cast<MINMAXINFO*>(lParam)))
        {
            return 0;
        }
        break;
    case WM_CLOSE:
        self->captureWindowPlacementForPersistence();
        logApp("[App] WM_CLOSE received");
        break;
    case WM_DESTROY:
        logApp("[App] WM_DESTROY received");
        PostQuitMessage(0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && !self->overlay_.isMenuVisible())
        {
            SetCursor(nullptr);
            return TRUE;
		}
    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool Application::createWindow(int width, int height)
{
    logApp("[App] Registering window class");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Application::windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;

    const HICON classIcon = static_cast<HICON>(LoadImageW(GetModuleHandle(nullptr),
                                                          L"IDI_ICON1",
                                                          IMAGE_ICON,
                                                          GetSystemMetrics(SM_CXICON),
                                                          GetSystemMetrics(SM_CYICON),
                                                          LR_DEFAULTCOLOR));
    const HICON classIconSmall = static_cast<HICON>(LoadImageW(GetModuleHandle(nullptr),
                                                               L"IDI_ICON1",
                                                               IMAGE_ICON,
                                                               GetSystemMetrics(SM_CXSMICON),
                                                               GetSystemMetrics(SM_CYSMICON),
                                                               LR_DEFAULTCOLOR));
    wc.hIcon = classIcon;
    wc.hIconSm = classIconSmall;

    if (!RegisterClassExW(&wc))
    {
        logApp("[App] RegisterClassExW failed");
        return false;
    }
    classRegistered_ = true;

    DWORD style = (settings_.videoFullscreen || settings_.videoBorderlessWindowed) ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    const int initialClientWidth = (settings_.hasWindowPlacement && settings_.windowClientWidth > 0)
        ? static_cast<int>(settings_.windowClientWidth)
        : width;
    const int initialClientHeight = (settings_.hasWindowPlacement && settings_.windowClientHeight > 0)
        ? static_cast<int>(settings_.windowClientHeight)
        : height;

    RECT rect{0, 0, initialClientWidth, initialClientHeight};
    AdjustWindowRect(&rect, style, FALSE);

    const int windowWidth = rect.right - rect.left;
    const int windowHeight = rect.bottom - rect.top;

    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;

    if (settings_.hasWindowPlacement)
    {
        windowX = settings_.windowPosX;
        windowY = settings_.windowPosY;
    }
    else
    {
        POINT cursorPos{};
        if (GetCursorPos(&cursorPos))
        {
            const HMONITOR monitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
            {
                const int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
                windowX = monitorInfo.rcMonitor.left + (monitorWidth - windowWidth) / 2;
                windowY = monitorInfo.rcMonitor.top + (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top - windowHeight) / 2;
            }
        }
    }

    if (windowX != CW_USEDEFAULT && windowY != CW_USEDEFAULT)
    {
        const RECT clamped = clampWindowRectToNearestWorkArea(
            RECT{windowX, windowY, windowX + windowWidth, windowY + windowHeight});
        windowX = clamped.left;
        windowY = clamped.top;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        L"CaptureViewer",
        style,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        nullptr,
        nullptr,
        GetModuleHandle(nullptr),
        this);

    if (!hwnd_)
    {
        logApp("[App] CreateWindowExW failed");
        return false;
    }

    if (classIcon)
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(classIcon));
    }
    if (classIconSmall)
    {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(classIconSmall));
    }

    if (!SetWindowTextW(hwnd_, L"CaptureViewer"))
    {
        logApp("[App] SetWindowTextW failed");
    }

    ShowWindow(hwnd_, (settings_.hasWindowPlacement && settings_.windowWasMaximized && !settings_.videoFullscreen) ? SW_SHOWMAXIMIZED : SW_SHOW);
    suppressCaptureDrivenResize_ = settings_.hasWindowPlacement && settings_.windowWasMaximized;
    initialMaximizePending_ = settings_.hasWindowPlacement && settings_.windowWasMaximized;
    initialMaximizeRestoreSeen_ = false;
    UpdateWindow(hwnd_);
    RECT initialClient{};
    if (GetClientRect(hwnd_, &initialClient))
    {
        if (settings_.hasWindowPlacement && settings_.windowWasMaximized && settings_.windowClientWidth > 0 && settings_.windowClientHeight > 0)
        {
            lockedClientWidth_ = static_cast<int>(settings_.windowClientWidth);
            lockedClientHeight_ = static_cast<int>(settings_.windowClientHeight);
        }
        else
        {
            lockedClientWidth_ = initialClient.right - initialClient.left;
            lockedClientHeight_ = initialClient.bottom - initialClient.top;
        }
    }
    if (!initialMaximizePending_)
    {
        updateWindowResizeMode();
    }
    logApp("[App] Window created");
    return true;
}

void Application::destroyWindow()
{
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (classRegistered_)
    {
        UnregisterClassW(kWindowClassName, GetModuleHandle(nullptr));
        classRegistered_ = false;
    }
}

void Application::handleFrame(const DirectShowCapture::Frame& frame)
{
    // The Sample Grabber invokes the handler synchronously. Serialize producers
    // defensively, but never make a capture callback wait for rendering or a GPU.
    std::scoped_lock captureLock(captureCopyMutex_);

    int backIndex = 1 - frontBufferIndex_;
    CpuFrame& dst = frames_[backIndex];

    dst.timestamp100ns = frame.timestamp100ns;
    dst.receivedAt = frame.receivedAt;

    const std::uint32_t frameWidth = frame.width;
    const std::uint32_t frameHeight = frame.height;
    const std::uint32_t defaultStride = (frame.pixelFormat == DirectShowCapture::PixelFormat::NV12) ? frameWidth : frameWidth * 4u;
    const std::uint32_t stride = frame.stride != 0 ? frame.stride : defaultStride;

    dst.width = frameWidth;
    dst.height = frameHeight;
    dst.pixelFormat = frame.pixelFormat;

    const std::uint32_t knownWidth = currentSourceWidth_.load(std::memory_order_acquire);
    const std::uint32_t knownHeight = currentSourceHeight_.load(std::memory_order_acquire);
    const std::uint32_t knownFrameRate100 = currentSourceFrameRate100_.load(std::memory_order_acquire);
    if (frameWidth != knownWidth || frameHeight != knownHeight || frame.nominalFrameRate100 != knownFrameRate100)
    {
        pendingSourceWidth_.store(frameWidth, std::memory_order_release);
        pendingSourceHeight_.store(frameHeight, std::memory_order_release);
        pendingSourceFrameRate100_.store(frame.nominalFrameRate100, std::memory_order_release);
        sourceChangePending_.store(true, std::memory_order_release);
    }

    const std::size_t requiredBytes = frameBufferBytes(frame.pixelFormat, stride, frameHeight);
    if (frame.dataSize < requiredBytes)
    {
        logApp("[App] Warning: frame data shorter than expected (" + std::to_string(frame.dataSize) + " < " + std::to_string(requiredBytes) + ")");
    }

    dst.stride = stride;
    dst.data.resize(requiredBytes);

    const bool bottomUp = frame.bottomUp;

    if (frame.data && frame.dataSize > 0)
    {
        const auto* srcRows = static_cast<const std::uint8_t*>(frame.data);
        if (!bottomUp)
        {
            const std::size_t copyBytes = std::min(requiredBytes, frame.dataSize);
            std::memcpy(dst.data.data(), srcRows, copyBytes);
            if (copyBytes < requiredBytes)
            {
                std::memset(dst.data.data() + copyBytes, 0, requiredBytes - copyBytes);
            }
        }
        else
        {
            for (std::uint32_t y = 0; y < frameHeight; ++y)
            {
                const std::uint32_t srcIndex = frameHeight - 1 - y;
                const std::size_t srcOffset = static_cast<std::size_t>(srcIndex) * stride;
                const std::size_t dstOffset = static_cast<std::size_t>(y) * stride;
                const std::size_t copyBytes = srcOffset < frame.dataSize ? std::min<std::size_t>(stride, frame.dataSize - srcOffset) : 0;
                if (copyBytes > 0)
                {
                    std::memcpy(dst.data.data() + dstOffset, srcRows + srcOffset, copyBytes);
                }
                if (copyBytes < stride)
                {
                    std::memset(dst.data.data() + dstOffset + copyBytes, 0, stride - copyBytes);
                }
            }
        }
    }
    else
    {
        std::memset(dst.data.data(), 0, dst.data.size());
    }

    static std::atomic<bool> loggedPixels{false};
    if (dst.pixelFormat == DirectShowCapture::PixelFormat::BGRA8 && !loggedPixels.exchange(true))
    {
        logApp("[App] Stored frame size=" + std::to_string(dst.data.size()) + " stride=" + std::to_string(dst.stride));
        auto logPixel = [&](const char* label, std::size_t row, std::size_t col) {
            if (row < dst.height && col < dst.width)
            {
                const std::size_t offset = row * dst.stride + col * 4;
                if (offset + 3 < dst.data.size())
                {
                    const auto* px = dst.data.data() + offset;
                    std::ostringstream oss;
                    oss << "[App] Sample pixel " << label << " (row=" << row << ", col=" << col << ") = "
                        << std::hex << std::uppercase
                        << std::setw(2) << std::setfill('0') << static_cast<int>(px[0])
                        << std::setw(2) << static_cast<int>(px[1])
                        << std::setw(2) << static_cast<int>(px[2])
                        << std::setw(2) << static_cast<int>(px[3]);
                    logApp(oss.str());
                }
            }
        };
        logPixel("top-left", 0, 0);
        logPixel("center", dst.height / 2, dst.width / 2);
        logPixel("bottom-right", dst.height - 1, dst.width - 1);
    }

    {
        std::scoped_lock publishLock(frameMutex_);
        dst.sequence = frameCounter_.load(std::memory_order_relaxed) + 1;
        frontBufferIndex_ = backIndex;
        frameCounter_.store(dst.sequence, std::memory_order_release);
    }
    if (frameReadyEvent_)
    {
        SetEvent(frameReadyEvent_);
    }

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
    {
        // dst can be swapped into the render thread after publication.
        logApp("[App] First frame received: " + std::to_string(frameWidth) + "x" + std::to_string(frameHeight) + " stride=" + std::to_string(stride));
    }
}

void Application::renderLoop()
{
    MSG msg = {};

    while (running_)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                logApp("[App] WM_QUIT in render loop");
                running_ = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!running_)
        {
            break;
        }

        processPendingSourceDimensions();

        if (overlay_.isMenuVisible())
        {
            const bool forced = forceRender_.load(std::memory_order_acquire);

            std::uint32_t targetFrameRate100 = currentSourceFrameRate100_.load(std::memory_order_acquire);
            if (targetFrameRate100 == 0)
            {
                targetFrameRate100 = settings_.videoPreferredFrameRate100;
            }
            if (targetFrameRate100 == 0)
            {
                targetFrameRate100 = 6000;
            }

            targetFrameRate100 = std::clamp<std::uint32_t>(targetFrameRate100, 1500, 12000);

            const double frameSeconds = 100.0 / static_cast<double>(targetFrameRate100);
            const auto frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(frameSeconds));
            const auto now = std::chrono::steady_clock::now();

            if (overlayNextFrameDeadline_.time_since_epoch().count() == 0)
            {
                overlayNextFrameDeadline_ = now;
            }

            if (!forced && now < overlayNextFrameDeadline_)
            {
                std::this_thread::sleep_until(overlayNextFrameDeadline_);
            }

            renderFrame(false);

            const auto afterRender = std::chrono::steady_clock::now();
            overlayNextFrameDeadline_ += frameDuration;
            if (overlayNextFrameDeadline_ < afterRender)
            {
                overlayNextFrameDeadline_ = afterRender;
            }
        }
        else
        {
            overlayNextFrameDeadline_ = std::chrono::steady_clock::time_point{};
            const bool didRender = renderFrame(false);
            if (!didRender)
            {
                if (frameReadyEvent_)
                {
                    HANDLE handles[] = {frameReadyEvent_};
                    MsgWaitForMultipleObjectsEx(1,
                                                handles,
                                                (settings_.showLatencyOverlay ||
                                                 frameCounter_.load(std::memory_order_acquire) > lastPresentedFrame_ ||
                                                 forceRender_.load(std::memory_order_acquire)) ? 250 : INFINITE,
                                                QS_ALLINPUT,
                                                MWMO_INPUTAVAILABLE);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    }
}

void Application::loadPersistentSettings()
{
    settings_ = settingsManager_.load();
    if (settings_.audioPlaybackEnabled && settings_.audioDeviceMoniker.empty())
    {
        settings_.audioDeviceMoniker = kAudioSourceVideoSentinel;
    }
    audioEnabled_ = shouldEnableCaptureAudio();
    suppressCaptureDrivenResize_ = settings_.windowWasMaximized;
    initialMaximizePending_ = settings_.windowWasMaximized;
    initialMaximizeRestoreSeen_ = false;
}

void Application::savePersistentSettings()
{
    settingsManager_.save(settings_);
}

void Application::showSettingsMenu()
{
    overlay_.toggleMenu(*this);
}

void Application::applyAudioPlaybackSetting()
{
    const bool useVideoAudio = shouldUseVideoAudio();
    if (settings_.audioPlaybackEnabled && useVideoAudio && settings_.audioDeviceMoniker != kAudioSourceVideoSentinel)
    {
        if (settings_.audioDeviceMoniker.empty() || settings_.audioDeviceMoniker == settings_.videoDeviceMoniker)
        {
            settings_.audioDeviceMoniker = kAudioSourceVideoSentinel;
            savePersistentSettings();
        }
    }

    const bool desiredCaptureAudio = shouldEnableCaptureAudio();

    if (desiredCaptureAudio != audioEnabled_)
    {
        audioEnabled_ = desiredCaptureAudio;
        restartVideoCapture();
    }

    if (!settings_.audioPlaybackEnabled)
    {
        audioPlayback_.stop();
        return;
    }

    if (desiredCaptureAudio)
    {
        audioPlayback_.stop();
        return;
    }

    std::string sourceMoniker;
    if (useVideoAudio)
    {
        sourceMoniker = settings_.videoDeviceMoniker;
    }
    else
    {
        sourceMoniker = settings_.audioDeviceMoniker;
    }

    if (!sourceMoniker.empty())
    {
        audioPlayback_.start(sourceMoniker,
                            settings_.audioOutputDeviceMonikers,
                            settings_.audioOutputUseDefaultOnly);
    }
    else
    {
        audioPlayback_.stop();
    }
}

void Application::setAudioPlaybackEnabled(bool enabled)
{
    if (settings_.audioPlaybackEnabled == enabled)
    {
        return;
    }

    settings_.audioPlaybackEnabled = enabled;
    if (settings_.audioPlaybackEnabled && settings_.audioDeviceMoniker.empty())
    {
        settings_.audioDeviceMoniker = kAudioSourceVideoSentinel;
    }
    savePersistentSettings();
    logApp(std::string("[App] Audio playback toggled -> ") + (settings_.audioPlaybackEnabled ? "enabled" : "disabled"));
    applyAudioPlaybackSetting();
}

void Application::selectVideoDevice(const std::string& moniker)
{
    if (settings_.videoDeviceMoniker == moniker)
    {
        return;
    }

    settings_.videoDeviceMoniker = moniker;
    settings_.videoPreferredWidth = 1920;
    settings_.videoPreferredHeight = 1080;
    settings_.videoPreferredFrameRate100 = 6000;
    savePersistentSettings();
    logApp(std::string("[App] Selected video capture device: ") + settings_.videoDeviceMoniker);
    restartVideoCapture();
    if (settings_.audioDeviceMoniker == kAudioSourceVideoSentinel && settings_.audioPlaybackEnabled)
    {
        applyAudioPlaybackSetting();
    }
    requestImmediateRender();
}

void Application::setVideoCaptureMode(std::uint32_t width, std::uint32_t height, std::uint32_t frameRate100)
{
    if (width == 0 || height == 0 || frameRate100 < 100 || frameRate100 > 100000)
    {
        return;
    }
    if (settings_.videoPreferredWidth == width && settings_.videoPreferredHeight == height &&
        settings_.videoPreferredFrameRate100 == frameRate100 && captureStatus_.empty())
    {
        return;
    }
    settings_.videoPreferredWidth = width;
    settings_.videoPreferredHeight = height;
    settings_.videoPreferredFrameRate100 = frameRate100;
    savePersistentSettings();
    std::ostringstream message;
    message << "[App] Requested capture mode -> " << width << "x" << height << " @ "
            << std::fixed << std::setprecision(2) << (static_cast<double>(frameRate100) / 100.0) << " FPS";
    logApp(message.str());
    restartVideoCapture();
    requestImmediateRender();
}

void Application::setVideoFrameRate100(std::uint32_t frameRate100)
{
    setVideoCaptureMode(settings_.videoPreferredWidth, settings_.videoPreferredHeight, frameRate100);
}

void Application::selectAudioDevice(const std::string& moniker)
{
    std::string newMoniker = moniker.empty() ? kAudioSourceVideoSentinel : moniker;
    if (settings_.audioDeviceMoniker == newMoniker)
    {
        return;
    }

    settings_.audioDeviceMoniker = newMoniker;
    savePersistentSettings();
    const std::string logLabel = (settings_.audioDeviceMoniker == kAudioSourceVideoSentinel) ? std::string("video source audio") : settings_.audioDeviceMoniker;
    logApp(std::string("[App] Selected audio capture device: ") + logLabel);
    applyAudioPlaybackSetting();
    requestImmediateRender();
}

void Application::setAudioOutputUseDefaultOnly(bool enabled)
{
    if (settings_.audioOutputUseDefaultOnly == enabled)
    {
        return;
    }

    settings_.audioOutputUseDefaultOnly = enabled;
    savePersistentSettings();
    logApp(std::string("[App] Audio output mode -> ") + (enabled ? "default output only" : "selected output devices"));
    applyAudioPlaybackSetting();
    requestImmediateRender();
}

void Application::setAudioOutputDeviceSelected(const std::string& moniker, bool selected)
{
    if (moniker.empty())
    {
        return;
    }

    auto& outputs = settings_.audioOutputDeviceMonikers;
    const auto it = std::find(outputs.begin(), outputs.end(), moniker);
    bool changed = false;

    if (selected)
    {
        if (it == outputs.end())
        {
            outputs.push_back(moniker);
            changed = true;
        }
        if (settings_.audioOutputUseDefaultOnly)
        {
            settings_.audioOutputUseDefaultOnly = false;
            changed = true;
        }
    }
    else
    {
        if (it != outputs.end())
        {
            outputs.erase(it);
            changed = true;
        }
    }

    if (!changed)
    {
        return;
    }

    savePersistentSettings();
    applyAudioPlaybackSetting();
    requestImmediateRender();
}

void Application::setVideoAllowResizing(bool enabled)
{
    if (settings_.videoAllowResizing == enabled)
    {
        return;
    }

    settings_.videoAllowResizing = enabled;
    savePersistentSettings();
    logApp(std::string("[App] Video allow resizing -> ") + (settings_.videoAllowResizing ? "enabled" : "disabled"));
    updateWindowResizeMode();

    if (!settings_.videoAllowResizing && !settings_.videoFullscreen)
    {
        const std::uint32_t srcW = currentSourceWidth_.load(std::memory_order_acquire);
        const std::uint32_t srcH = currentSourceHeight_.load(std::memory_order_acquire);
        if (srcW > 0 && srcH > 0)
        {
            lockedClientWidth_ = static_cast<int>(srcW);
            lockedClientHeight_ = static_cast<int>(srcH);
        }
        else if (hwnd_)
        {
            RECT client{};
            if (GetClientRect(hwnd_, &client))
            {
                lockedClientWidth_ = client.right - client.left;
                lockedClientHeight_ = client.bottom - client.top;
            }
        }

        if (lockedClientWidth_ > 0 && lockedClientHeight_ > 0)
        {
            resizeWindowToClient(lockedClientWidth_, lockedClientHeight_);
        }
    }

    requestImmediateRender();
}

void Application::setVideoAspectMode(VideoAspectMode mode)
{
    if (settings_.videoAspectMode == mode)
    {
        return;
    }

    settings_.videoAspectMode = mode;
    savePersistentSettings();
    logApp(std::string("[App] Video aspect mode -> ") + std::to_string(static_cast<unsigned int>(mode)));
    requestImmediateRender();
}

void Application::setVideoScalePercent(unsigned int percent)
{
    percent = std::clamp(percent, 25u, 200u);
    if (settings_.videoScalePercent == percent)
    {
        return;
    }
    settings_.videoScalePercent = percent;
    savePersistentSettings();
    requestImmediateRender();
}

void Application::setShowLatencyOverlay(bool enabled)
{
    settings_.showLatencyOverlay = enabled;
    savePersistentSettings();
    nextStatsRefresh_ = {};
    requestImmediateRender();
}

void Application::setWindowMode(int mode)
{
    if (mode == 2)
    {
        setFullscreen(true);
    }
    else
    {
        setFullscreen(false);
        setBorderlessWindowed(mode == 1);
    }
}

void Application::setVideoFormatPreference(VideoFormatPreference preference)
{
    if (settings_.videoFormatPreference == preference)
    {
        return;
    }

    settings_.videoFormatPreference = preference;
    savePersistentSettings();
    switch (preference)
    {
    case VideoFormatPreference::XRGB:
        logApp("[App] Video format preference -> XRGB");
        break;
    case VideoFormatPreference::NV12:
        logApp("[App] Video format preference -> NV12");
        break;
    default:
        logApp("[App] Video format preference -> Auto");
        break;
    }

    restartVideoCapture();
    requestImmediateRender();
}

void Application::setVSyncEnabled(bool enabled)
{
    if (settings_.vsyncEnabled == enabled)
    {
        return;
    }

    settings_.vsyncEnabled = enabled;
    savePersistentSettings();
    renderer_.setVSyncEnabled(enabled);
    logApp(std::string("[App] VSync -> ") + (settings_.vsyncEnabled ? "enabled" : "disabled"));
    requestImmediateRender();
}

void Application::setBorderlessWindowed(bool enabled)
{
    if (settings_.videoBorderlessWindowed == enabled)
    {
        return;
    }

    settings_.videoBorderlessWindowed = enabled;
    savePersistentSettings();
    logApp(std::string("[App] Borderless windowed -> ") + (settings_.videoBorderlessWindowed ? "enabled" : "disabled"));
    updateWindowResizeMode(true);
    requestImmediateRender();
}

void Application::setFullscreen(bool enabled)
{
    if (settings_.videoFullscreen == enabled)
    {
        return;
    }

    if (enabled)
    {
        captureWindowPlacementForPersistence();
        restoreWindowPlacementAfterFullscreen_ = false;
    }
    else
    {
        restoreWindowPlacementAfterFullscreen_ = true;
    }

    settings_.videoFullscreen = enabled;
    savePersistentSettings();
    logApp(std::string("[App] Fullscreen -> ") + (settings_.videoFullscreen ? "enabled" : "disabled"));
    updateWindowResizeMode();
    requestImmediateRender();
}

void Application::recenterWindow()
{
    if (!hwnd_ || settings_.videoFullscreen)
    {
        return;
    }

    RECT windowRect{};
    if (!GetWindowRect(hwnd_, &windowRect))
    {
        return;
    }

    const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
    {
        return;
    }

    const int windowWidth = windowRect.right - windowRect.left;
	const int windowHeight = windowRect.bottom - windowRect.top;
    const int newX = monitorInfo.rcWork.left + ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - windowWidth) / 2;
	const int newY = monitorInfo.rcWork.top + ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) - windowHeight) / 2;

    if (SetWindowPos(hwnd_, nullptr, newX, newY, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOSENDCHANGING))
    {
        requestImmediateRender();
    }
}

void Application::requestImmediateRender()
{
    forceRender_.store(true, std::memory_order_release);
    if (frameReadyEvent_)
    {
        SetEvent(frameReadyEvent_);
    }
}

bool Application::uploadLatestFrame()
{
    // Resource creation may wait for a previous GPU frame. Prepare it from a
    // descriptor first, then select the latest completed capture *after* waiting.
    // Bound retries if a device keeps changing formats while we prepare it.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
        auto format = DirectShowCapture::PixelFormat::BGRA8;
        {
            std::scoped_lock lock(frameMutex_);
            const bool hasNewer = frameCounter_.load(std::memory_order_acquire) > lastSelectedFrame_;
            const CpuFrame& candidate = hasNewer ? frames_[frontBufferIndex_] : renderCpuFrame_;
            if (candidate.sequence <= lastPresentedFrame_ || candidate.data.empty() ||
                candidate.width == 0 || candidate.height == 0)
            {
                return false;
            }
            width = candidate.width;
            height = candidate.height;
            stride = candidate.stride;
            format = candidate.pixelFormat;
        }

        if (!renderer_.prepareVideoResources(width, height, stride, toRendererFormat(format)))
        {
            return false;
        }

        {
            std::scoped_lock lock(frameMutex_);
            if (frameCounter_.load(std::memory_order_acquire) > lastSelectedFrame_)
            {
                CpuFrame& newest = frames_[frontBufferIndex_];
                if (newest.width != width || newest.height != height || newest.pixelFormat != format)
                {
                    continue;
                }
                std::swap(renderCpuFrame_, newest);
                lastSelectedFrame_ = renderCpuFrame_.sequence;
            }
        }

        // renderCpuFrame_ belongs exclusively to this thread. The callback can
        // keep overwriting the mailbox throughout upload and Present waits.
        const CpuFrame& src = renderCpuFrame_;
        if (!renderer_.uploadFrame(src.data.data(), src.data.size(), src.stride,
                                   src.width, src.height, toRendererFormat(src.pixelFormat)))
        {
            return false;
        }
        uploadedFrameReceivedAt_ = src.receivedAt;
        return true;
    }
    return false;
}

void Application::processPendingSourceDimensions()
{
    if (!sourceChangePending_.load(std::memory_order_acquire))
    {
        return;
    }

    const std::uint32_t newWidth = pendingSourceWidth_.load(std::memory_order_acquire);
    const std::uint32_t newHeight = pendingSourceHeight_.load(std::memory_order_acquire);
    const std::uint32_t newFrameRate100 = pendingSourceFrameRate100_.load(std::memory_order_acquire);
    if (newWidth != 0 && newHeight != 0)
    {
        applySourceDimensions(newWidth, newHeight);
        currentSourceFrameRate100_.store(newFrameRate100, std::memory_order_release);
    }
    sourceChangePending_.store(false, std::memory_order_release);
}

bool Application::renderFrame(bool forcePresent)
{
    if (hwnd_ && IsIconic(hwnd_))
    {
        return false;
    }

    processPendingSourceDimensions();

    frameRates_.update(std::chrono::steady_clock::now(),
                       frameCounter_.load(std::memory_order_acquire), previewFrameCount_);

    const bool menuVisible = overlay_.isMenuVisible();
    renderer_.setBlurEnabled(menuVisible);

    if (menuVisible)
    {
        overlay_.newFrame();
        overlay_.buildUI(*this);
        overlay_.endFrame();
    }

    const bool requested = forceRender_.exchange(false, std::memory_order_acq_rel);
    const bool forced = forcePresent || requested;
    const bool hasUnpresentedFrame = frameCounter_.load(std::memory_order_acquire) > lastPresentedFrame_;
    const bool statsRefresh = settings_.showLatencyOverlay &&
        std::chrono::steady_clock::now() >= nextStatsRefresh_;
    // HUD draw data must not turn the capture-driven path into an unlimited render loop.
    if (!hasUnpresentedFrame && !forced && !menuVisible && !statsRefresh)
    {
        return false;
    }
    if (!menuVisible && settings_.showLatencyOverlay)
    {
        overlay_.newFrame();
        overlay_.buildUI(*this);
        overlay_.endFrame();
    }

    // Reserve presentation/GPU capacity before choosing which captured frame
    // to show. No capture mutex is held during this bounded wait.
    if (!renderer_.prepareFrameForUpload())
    {
        if (forced)
        {
            forceRender_.store(true, std::memory_order_release);
        }
        return false;
    }
    const bool uploaded = uploadLatestFrame();
    const bool uploadFailed = !uploaded && frameCounter_.load(std::memory_order_acquire) > lastPresentedFrame_;
    if (uploadFailed)
    {
        captureStatus_ = "Cannot preview this frame. Try a different capture resolution or pixel format.";
        // Keep settings usable even for unsupported frames. Do not sample a
        // newly allocated texture until it has received a valid upload.
        if (!menuVisible && !forced && !statsRefresh)
        {
            return false;
        }
    }
    else if (uploaded)
    {
        captureStatus_.clear();
    }

    if (hwnd_)
    {
        RECT clientRect{};
        if (GetClientRect(hwnd_, &clientRect))
        {
            bool viewportValid = false;
            const RECT viewport = computeVideoViewport(clientRect, viewportValid);
            if (viewportValid)
            {
                renderer_.setViewportRect(static_cast<float>(viewport.left),
                                          static_cast<float>(viewport.top),
                                          static_cast<float>(viewport.right - viewport.left),
                                          static_cast<float>(viewport.bottom - viewport.top));
            }
            else
            {
                renderer_.setViewportRect(0.0f, 0.0f,
                                          static_cast<float>(clientRect.right - clientRect.left),
                                          static_cast<float>(clientRect.bottom - clientRect.top));
            }
        }
    }

    const bool presented = renderer_.render([&](ID3D12GraphicsCommandList* cmdList) {
        overlay_.render(cmdList);
    }, !uploadFailed);
    if (presented && uploaded)
    {
        // Only successful, unique captured frames contribute to preview FPS and
        // timing. Failed presents retain the CPU frame for retry or replacement.
        skippedFrameCount_ += renderCpuFrame_.sequence - lastPresentedFrame_ - 1;
        lastPresentedFrame_ = renderCpuFrame_.sequence;
        ++previewFrameCount_;
        frameTiming_.record(uploadedFrameReceivedAt_, renderer_.lastPresentReturnTime());
    }
    else if (!presented && forced)
    {
        forceRender_.store(true, std::memory_order_release);
    }
    nextStatsRefresh_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    return presented;
}

std::string Application::toLowerCopy(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

void Application::applySourceDimensions(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    currentSourceWidth_.store(width, std::memory_order_release);
    currentSourceHeight_.store(height, std::memory_order_release);
    static std::uint32_t lastLoggedWidth = 0;
    static std::uint32_t lastLoggedHeight = 0;
    if (width != lastLoggedWidth || height != lastLoggedHeight)
    {
        logApp("[App] Active capture resolution -> " + std::to_string(width) + "x" + std::to_string(height));
        lastLoggedWidth = width;
        lastLoggedHeight = height;
    }

    if (hwnd_ && (suppressCaptureDrivenResize_ || IsZoomed(hwnd_)))
    {
        return;
    }

    lockedClientWidth_ = static_cast<int>(width);
    lockedClientHeight_ = static_cast<int>(height);

    if (hwnd_ && !settings_.videoFullscreen)
    {
        resizeWindowToClient(static_cast<int>(width), static_cast<int>(height));
        updateWindowResizeMode();
    }
}

bool Application::resizeWindowToClient(int width, int height, bool preserveClientPosition, const POINT* clientOrigin)
{
    if (!hwnd_ || width <= 0 || height <= 0)
    {
        return false;
    }

    RECT current{};
    if (!GetClientRect(hwnd_, &current))
    {
        return false;
    }

    const int currentWidth = current.right - current.left;
    const int currentHeight = current.bottom - current.top;
    if (!preserveClientPosition && currentWidth == width && currentHeight == height)
    {
        return false;
    }

    RECT desired{};
    if (preserveClientPosition)
    {
        POINT origin{};
        if (clientOrigin)
        {
            origin = *clientOrigin;
        }
        else
        {
            if (!ClientToScreen(hwnd_, &origin))
            {
                return false;
            }
        }
        desired.left = origin.x;
        desired.top = origin.y;
        desired.right = origin.x + width;
        desired.bottom = origin.y + height;
    }
    else
    {
        desired = RECT{0, 0, width, height};
    }

    DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&desired, style, FALSE, exStyle))
    {
        return false;
    }

    const int windowWidth = desired.right - desired.left;
    const int windowHeight = desired.bottom - desired.top;

    int windowX = desired.left;
    int windowY = desired.top;
    if (!preserveClientPosition)
    {
        RECT currentWindowRect{};
        if (!GetWindowRect(hwnd_, &currentWindowRect))
        {
            return false;
        }

        windowX = currentWindowRect.left;
        windowY = currentWindowRect.top;
    }

    if (!SetWindowPos(hwnd_, nullptr, windowX, windowY, windowWidth, windowHeight,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING))
    {
        return false;
    }

    return true;
}

void Application::updateWindowResizeMode(bool preserveClientPosition)
{
    if (!hwnd_)
    {
        return;
    }

    POINT preservedClientOrigin{};
    const POINT* preservedClientOriginPtr = nullptr;
    if (preserveClientPosition)
    {
        RECT clientRect{};
        if (GetClientRect(hwnd_, &clientRect) && ClientToScreen(hwnd_, &preservedClientOrigin))
        {
            preservedClientOriginPtr = &preservedClientOrigin;
        }
    }

    const bool fullscreen = settings_.videoFullscreen;
    const bool borderlessWindowed = settings_.videoBorderlessWindowed;
    const LONG_PTR desiredStyle = (fullscreen || borderlessWindowed) ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    const LONG_PTR currentStyle = GetWindowLongPtr(hwnd_, GWL_STYLE);
    if (currentStyle != desiredStyle)
    {
        SetWindowLongPtr(hwnd_, GWL_STYLE, desiredStyle);
    }

    if (fullscreen)
    {
        const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
        {
            SetWindowPos(hwnd_, HWND_NOTOPMOST,
                         monitorInfo.rcMonitor.left,
                         monitorInfo.rcMonitor.top,
                         monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                         monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        return;
    }

    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_FRAMECHANGED);

    if (IsZoomed(hwnd_))
    {
        return;
    }

    if (restoreWindowPlacementAfterFullscreen_ && settings_.windowWasMaximized)
    {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
        restoreWindowPlacementAfterFullscreen_ = false;
        return;
    }

    const int desiredWidth = lockedClientWidth_ > 0 ? lockedClientWidth_ : kDefaultWidth;
    const int desiredHeight = lockedClientHeight_ > 0 ? lockedClientHeight_ : kDefaultHeight;
    if (restoreWindowPlacementAfterFullscreen_ && settings_.hasWindowPlacement)
    {
        RECT desired{0, 0, desiredWidth, desiredHeight};
        DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_STYLE));
        DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_EXSTYLE));
        if (AdjustWindowRectEx(&desired, style, FALSE, exStyle))
        {
            const int windowWidth = desired.right - desired.left;
            const int windowHeight = desired.bottom - desired.top;
            RECT target{settings_.windowPosX,
                        settings_.windowPosY,
                        settings_.windowPosX + windowWidth,
                        settings_.windowPosY + windowHeight};
            target = clampWindowRectToNearestWorkArea(target);
            SetWindowPos(hwnd_,
                         HWND_NOTOPMOST,
                         target.left,
                         target.top,
                         windowWidth,
                         windowHeight,
                         SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_FRAMECHANGED);
            restoreWindowPlacementAfterFullscreen_ = false;
            return;
        }
        restoreWindowPlacementAfterFullscreen_ = false;
    }

    resizeWindowToClient(desiredWidth, desiredHeight, preserveClientPosition, preservedClientOriginPtr);
}

bool Application::applyLockedWindowSize(MINMAXINFO* info) const
{
    if (!info || settings_.videoAllowResizing || settings_.videoFullscreen || !hwnd_ || lockedClientWidth_ <= 0 || lockedClientHeight_ <= 0)
    {
        return false;
    }

    RECT rect{0, 0, lockedClientWidth_, lockedClientHeight_};
    DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&rect, style, FALSE, exStyle))
    {
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    info->ptMinTrackSize.x = width;
    info->ptMinTrackSize.y = height;
    info->ptMaxTrackSize.x = width;
    info->ptMaxTrackSize.y = height;
    return true;
}

RECT Application::computeVideoViewport(const RECT& clientRect, bool& valid) const
{
    const auto viewport = computeVideoViewportDimensions(
        clientRect.right - clientRect.left, clientRect.bottom - clientRect.top,
        static_cast<int>(currentSourceWidth_.load(std::memory_order_acquire)),
        static_cast<int>(currentSourceHeight_.load(std::memory_order_acquire)),
        settings_.videoAspectMode, settings_.videoScalePercent);
    valid = viewport.width > 0 && viewport.height > 0;
    return RECT{viewport.x, viewport.y, viewport.x + viewport.width, viewport.y + viewport.height};
}

bool Application::shouldUseVideoAudio() const
{
    if (settings_.audioDeviceMoniker.empty())
    {
        return true;
    }
    if (settings_.audioDeviceMoniker == kAudioSourceVideoSentinel)
    {
        return true;
    }
    if (!settings_.videoDeviceMoniker.empty() && settings_.audioDeviceMoniker == settings_.videoDeviceMoniker)
    {
        return true;
    }
    return false;
}

bool Application::shouldEnableCaptureAudio() const
{
    if (!settings_.audioPlaybackEnabled || !shouldUseVideoAudio())
    {
        return false;
    }

    // Capture-graph monitoring is kept only for default output mode.
    // Any explicit output selection is routed through AudioPlayback.
    if (!settings_.audioOutputUseDefaultOnly)
    {
        return false;
    }

    return settings_.audioOutputDeviceMonikers.empty();
}

void Application::restartVideoCapture()
{
    if (!running_)
    {
        return;
    }

    logApp("[App] Restarting video capture with updated settings");
    directShowCapture_.stop();

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frames_[0] = CpuFrame{};
        frames_[1] = CpuFrame{};
        frontBufferIndex_ = 0;
    }
    renderCpuFrame_ = CpuFrame{};
    frameCounter_.store(0, std::memory_order_release);
    lastSelectedFrame_ = 0;
    lastPresentedFrame_ = 0;
    uploadedFrameReceivedAt_ = {};
    frameTiming_.reset();
    frameRates_.reset();
    previewFrameCount_ = 0;
    skippedFrameCount_ = 0;
    nextStatsRefresh_ = {};
    currentSourceWidth_.store(0, std::memory_order_release);
    currentSourceHeight_.store(0, std::memory_order_release);
    currentSourceFrameRate100_.store(0, std::memory_order_release);
    pendingSourceWidth_.store(0, std::memory_order_release);
    pendingSourceHeight_.store(0, std::memory_order_release);
    pendingSourceFrameRate100_.store(0, std::memory_order_release);
    sourceChangePending_.store(false, std::memory_order_release);
    captureFormatNotice_.clear();

    try
    {
        DirectShowCapture::Options options;
        options.deviceMoniker = settings_.videoDeviceMoniker;
        options.enableAudio = audioEnabled_;
        options.desiredWidth = settings_.videoPreferredWidth;
        options.desiredHeight = settings_.videoPreferredHeight;
        options.desiredFrameRate100 = settings_.videoPreferredFrameRate100;
        options.videoFormatPreference = toCaptureVideoFormat(settings_.videoFormatPreference);
        directShowCapture_.start([this](const DirectShowCapture::Frame& frame) {
            handleFrame(frame);
        }, options);
        captureStatus_.clear();
        logApp("[App] Video capture restarted successfully");
    }
    catch (const std::exception& ex)
    {
        captureStatus_ = ex.what();
        logApp(std::string("[App] Failed to restart capture: ") + ex.what());
    }
    catch (...)
    {
        captureStatus_ = "Could not start video capture.";
        logApp("[App] Failed to restart capture: unknown error");
    }
    syncCaptureDeviceInfo();
}

void Application::syncCaptureDeviceInfo()
{
    const auto info = directShowCapture_.deviceInfo();
    if (!info.moniker.empty() && settings_.videoDeviceMoniker != info.moniker)
    {
        settings_.videoDeviceMoniker = info.moniker;
        savePersistentSettings();
    }
    captureFormatNotice_ = info.warning;
    overlay_.refreshCaptureCapabilities(*this);
}

void Application::captureWindowPlacementForPersistence()
{
    if (!hwnd_ || IsIconic(hwnd_) || settings_.videoFullscreen)
    {
        return;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd_, &placement))
    {
        return;
    }

    RECT clientRect{};
    if (!GetClientRect(hwnd_, &clientRect))
    {
        return;
    }

    const bool maximized = IsZoomed(hwnd_) || placement.showCmd == SW_SHOWMAXIMIZED;
    const RECT normalRect = placement.rcNormalPosition;
    settings_.windowPosX = normalRect.left;
    settings_.windowPosY = normalRect.top;

    if (maximized)
    {
        RECT frameMargins{0, 0, 0, 0};
        DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_STYLE));
        DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd_, GWL_EXSTYLE));
        if (AdjustWindowRectEx(&frameMargins, style, FALSE, exStyle))
        {
            const int windowWidth = normalRect.right - normalRect.left;
            const int windowHeight = normalRect.bottom - normalRect.top;
            const int frameWidth = frameMargins.right - frameMargins.left;
            const int frameHeight = frameMargins.bottom - frameMargins.top;
            const int normalClientWidth = windowWidth - frameWidth;
            const int normalClientHeight = windowHeight - frameHeight;
            settings_.windowClientWidth = static_cast<unsigned int>(std::max(1, normalClientWidth));
            settings_.windowClientHeight = static_cast<unsigned int>(std::max(1, normalClientHeight));
        }
    }
    else
    {
        const int clientWidth = clientRect.right - clientRect.left;
        const int clientHeight = clientRect.bottom - clientRect.top;
        if (clientWidth > 0 && clientHeight > 0)
        {
            settings_.windowClientWidth = static_cast<unsigned int>(clientWidth);
            settings_.windowClientHeight = static_cast<unsigned int>(clientHeight);
        }
    }

    settings_.hasWindowPlacement = true;
    settings_.windowWasMaximized = maximized;
    savePersistentSettings();
}
