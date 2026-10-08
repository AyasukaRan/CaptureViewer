#include "DirectShowCapture.hpp"
#include "CaptureCapabilities.hpp"
#include "FrameRateOptions.hpp"

#include <Windows.h>
#include <OleAuto.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <uuids.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kPreferredDeviceName[] = L"AVerMedia HD Capture GC573 1";

    const GUID kCLSID_SampleGrabber = {0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
    const GUID kIID_ISampleGrabber = {0x6B652FFF, 0x11FE, 0x4FCE, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
    const GUID kIID_ISampleGrabberCB = {0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};
    const GUID kCLSID_NullRenderer = {0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
    const GUID kPreferredVideoSubtypeRgb24 = MEDIASUBTYPE_RGB24;
    const GUID kPreferredVideoSubtypeXrgb = MEDIASUBTYPE_RGB32;
    const GUID kPreferredVideoSubtypeNv12 = MEDIASUBTYPE_NV12;

    using Microsoft::WRL::ComPtr;

    void logMessage(const std::string& text)
    {
        std::ofstream("viewer.log", std::ios::app) << text << '\n';
    }

    std::string formatHr(HRESULT hr)
    {
        char buffer[512] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr,
                       hr,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buffer,
                       static_cast<DWORD>(std::size(buffer)),
                       nullptr);
        return buffer;
    }

    void throwIfFailed(HRESULT hr, const char* context)
    {
        if (FAILED(hr))
        {
            std::ostringstream oss;
            oss << context << " (HRESULT 0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(hr) << "): " << formatHr(hr);
            logMessage(std::string("[Capture] ") + oss.str());
            throw std::runtime_error(oss.str());
        }
    }

    std::string narrow(const std::wstring& wstr)
    {
        if (wstr.empty())
        {
            return {};
        }
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(length - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, result.data(), length, nullptr, nullptr);
        return result;
    }

    std::wstring widen(const std::string& str)
    {
        if (str.empty())
        {
            return {};
        }
        const int required = ::MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), required);
        return result;
    }

    void freeMediaType(AM_MEDIA_TYPE& mt)
    {
        if (mt.cbFormat != 0 && mt.pbFormat)
        {
            CoTaskMemFree(mt.pbFormat);
            mt.cbFormat = 0;
            mt.pbFormat = nullptr;
        }
        if (mt.pUnk)
        {
            mt.pUnk->Release();
            mt.pUnk = nullptr;
        }
    }

    bool isSubtype(const GUID& value, const GUID& expected)
    {
        return InlineIsEqualGUID(value, expected) != FALSE;
    }

    bool isXrgb32CompatibleSubtype(const GUID& subtype)
    {
        return isSubtype(subtype, MEDIASUBTYPE_RGB32) ||
               isSubtype(subtype, MEDIASUBTYPE_ARGB32);
    }

    bool isXrgbCompatibleSubtype(const GUID& subtype)
    {
        return isSubtype(subtype, MEDIASUBTYPE_RGB24) ||
               isXrgb32CompatibleSubtype(subtype);
    }

    const char* subtypeName(const GUID& subtype)
    {
        if (isSubtype(subtype, kPreferredVideoSubtypeRgb24))
        {
            return "RGB24";
        }
        if (isXrgb32CompatibleSubtype(subtype))
        {
            return "XRGB";
        }
        if (isSubtype(subtype, kPreferredVideoSubtypeNv12))
        {
            return "NV12";
        }
        return "Other";
    }

}

struct ISampleGrabberCB : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long bufferLen) = 0;
};

struct ISampleGrabber : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

struct DirectShowCaptureImpl;

class SampleGrabberCallback : public ISampleGrabberCB
{
public:
    explicit SampleGrabberCallback(DirectShowCaptureImpl* owner) noexcept
        : owner_(owner)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == kIID_ISampleGrabberCB)
        {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG value = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (value == 0)
        {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long bufferLen) override;

    void resetOwner()
    {
        owner_.store(nullptr, std::memory_order_release);
    }

private:
    std::atomic<ULONG> refCount_{1};
    std::atomic<DirectShowCaptureImpl*> owner_;
};

struct DirectShowCaptureImpl
{
    enum class ActiveSubtype
    {
        Unknown,
        XRGB,
        RGB24,
        NV12,
    };

    DirectShowCapture::FrameHandler handler;
    std::thread worker;
    std::atomic<bool> running{false};

    std::mutex initMutex;
    std::condition_variable initCv;
    bool initCompleted = false;
    std::exception_ptr initError;

    std::mutex errorMutex;
    std::string lastError;
    mutable std::mutex deviceInfoMutex;
    DirectShowCapture::DeviceInfo deviceInfo;

    std::atomic<bool> frameReceived{false};

    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> captureBuilder;
    ComPtr<IMediaControl> control;
    ComPtr<IBaseFilter> captureFilter;
    ComPtr<IBaseFilter> sampleGrabberFilter;
    ComPtr<ISampleGrabber> sampleGrabber;
    ComPtr<IBaseFilter> nullRenderer;
    ComPtr<IMoniker> selectedMoniker;
    SampleGrabberCallback* callback = nullptr;

    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
    std::uint32_t frameStride = 0;
    std::uint32_t contentLeft = 0;
    std::uint32_t contentTop = 0;
    std::uint32_t contentRight = 0;
    std::uint32_t contentBottom = 0;
    bool bottomUp = false;
    std::atomic<bool> loggedSampleSize{false};

    std::wstring requestedMoniker;
    std::wstring selectedFriendlyName;
    std::wstring selectedMonikerDisplayName;
    bool audioEnabled = false;
    std::uint32_t requestedWidth = 0;
    std::uint32_t requestedHeight = 0;
    std::uint32_t requestedFrameRate100 = 0;
    DirectShowCapture::VideoFormatPreference requestedFormatPreference = DirectShowCapture::VideoFormatPreference::XRGB;

    ActiveSubtype activeSubtype = ActiveSubtype::Unknown;
    std::uint32_t activeFrameRate100 = 0;
    std::vector<std::uint8_t> convertedBuffer;

    DirectShowCaptureImpl() = default;

    ~DirectShowCaptureImpl()
    {
        stop();
    }

    void start(DirectShowCapture::FrameHandler cb, const DirectShowCapture::Options& options)
    {
        if (!cb)
        {
            throw std::invalid_argument("Frame handler must not be empty");
        }

        if (running.load(std::memory_order_acquire))
        {
            throw std::runtime_error("Capture already running");
        }
        // An initialization/runtime failure can leave a completed, joinable worker.
        if (worker.joinable())
        {
            worker.join();
        }
        {
            std::lock_guard<std::mutex> lock(deviceInfoMutex);
            deviceInfo = {};
            deviceInfo.moniker = options.deviceMoniker;
        }

        handler = std::move(cb);
        requestedMoniker = widen(options.deviceMoniker);
        selectedFriendlyName.clear();
        selectedMonikerDisplayName.clear();
        audioEnabled = options.enableAudio;
        requestedWidth = options.desiredWidth;
        requestedHeight = options.desiredHeight;
        requestedFrameRate100 = options.desiredFrameRate100;
        requestedFormatPreference = options.videoFormatPreference;
        activeSubtype = ActiveSubtype::Unknown;
        convertedBuffer.clear();
        if (running.exchange(true))
        {
            throw std::runtime_error("Capture already running");
        }

        {
            std::lock_guard<std::mutex> lock(initMutex);
            initCompleted = false;
            initError = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError.clear();
        }
        frameReceived.store(false, std::memory_order_release);

        worker = std::thread([this]() {
            runCaptureThread();
        });

        std::unique_lock<std::mutex> lock(initMutex);
        initCv.wait(lock, [this]() { return initCompleted; });
        if (initError)
        {
            auto err = initError;
            initError = nullptr;
            lock.unlock();
            running.store(false, std::memory_order_release);
            if (worker.joinable())
            {
                worker.join();
            }
            std::rethrow_exception(err);
        }

        logMessage("[Capture] Initialization completed successfully");
    }

    void stop()
    {
        running.store(false, std::memory_order_release);

        if (worker.joinable())
        {
            worker.join();
        }

        releaseGraph();
    }

    std::string currentFriendlyName() const
    {
        std::lock_guard<std::mutex> lock(deviceInfoMutex);
        return deviceInfo.friendlyName.empty() ? deviceInfo.moniker : deviceInfo.friendlyName;
    }

    void runCaptureThread()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto finalizeInit = [this](std::exception_ptr err = nullptr) {
            std::lock_guard<std::mutex> lock(initMutex);
            if (initCompleted)
            {
                return;
            }
            initError = err;
            initCompleted = true;
            initCv.notify_all();
        };

        try
        {
            selectCaptureDevice();
            buildGraph();
            logMessage("[Capture] Graph constructed");

            if (control)
            {
                throwIfFailed(control->Run(), "Failed to start graph");
                logMessage("[Capture] Graph running");
            }
            finalizeInit();

            while (running.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            if (control)
            {
                control->StopWhenReady();
                logMessage("[Capture] Graph stop requested");
            }

            if (!frameReceived.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (lastError.empty())
                {
                    const std::string deviceLabel = selectedFriendlyName.empty()
                        ? "the selected capture device"
                        : narrow(selectedFriendlyName);
                    lastError = "No video frames received from '" + deviceLabel + "'. Verify the signal and that no other application is using the device.";
                }
                logMessage("[Capture] No frames were received from the device");
            }
        }
        catch (...)
        {
            auto err = std::current_exception();
            finalizeInit(err);
            storeRuntimeError(err);
            running.store(false, std::memory_order_release);
            logMessage("[Capture] Exception thrown inside capture thread");
        }

        releaseGraph();
        CoUninitialize();
        logMessage("[Capture] Capture thread exited");
    }

    void selectCaptureDevice()
    {
        ComPtr<ICreateDevEnum> devEnum;
        throwIfFailed(CoCreateInstance(CLSID_SystemDeviceEnum,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&devEnum)),
                      "Failed to create device enumerator");

        ComPtr<IEnumMoniker> enumMoniker;
        HRESULT hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, enumMoniker.GetAddressOf(), 0);
        if (hr != S_OK || !enumMoniker)
        {
            throw std::runtime_error("No video capture devices were found");
        }

        const std::wstring requested = requestedMoniker;

        ComPtr<IMoniker> matched;
        std::wstring matchedFriendly;
        std::wstring matchedDisplay;

        ComPtr<IMoniker> preferred;
        std::wstring preferredFriendly;
        std::wstring preferredDisplay;

        ComPtr<IMoniker> fallback;
        std::wstring fallbackFriendly;
        std::wstring fallbackDisplay;

        ComPtr<IMoniker> current;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, current.GetAddressOf(), &fetched) == S_OK)
        {
            std::wstring displayName;
            {
                LPOLESTR temp = nullptr;
                if (SUCCEEDED(current->GetDisplayName(nullptr, nullptr, &temp)) && temp)
                {
                    displayName.assign(temp);
                    CoTaskMemFree(temp);
                }
            }

            std::wstring friendly;
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(current->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))) && bag)
            {
                VARIANT name;
                VariantInit(&name);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &name, nullptr)) && name.vt == VT_BSTR)
                {
                    friendly.assign(name.bstrVal, SysStringLen(name.bstrVal));
                }
                VariantClear(&name);
            }

            const std::string friendlyLog = friendly.empty() ? narrow(displayName) : narrow(friendly);
            logMessage("[Capture] Found device: " + (friendlyLog.empty() ? std::string("<unnamed>") : friendlyLog));

            const bool matchesRequested = !requested.empty() &&
                ((!displayName.empty() && displayName == requested) || (!friendly.empty() && friendly == requested));

            if (matchesRequested)
            {
                matched = current;
                matchedFriendly = friendly;
                matchedDisplay = displayName;
                logMessage("[Capture] Selected requested device");
                break;
            }

            if (!preferred && !friendly.empty() && friendly == kPreferredDeviceName)
            {
                preferred = current;
                preferredFriendly = friendly;
                preferredDisplay = displayName;
                logMessage("[Capture] Remembering preferred device by friendly name");
            }

            if (!fallback)
            {
                fallback = current;
                fallbackFriendly = friendly;
                fallbackDisplay = displayName;
            }

            current.Reset();
        }

        if (!matched)
        {
            if (preferred)
            {
                matched = preferred;
                matchedFriendly = preferredFriendly;
                matchedDisplay = preferredDisplay;
                logMessage("[Capture] Using preferred device fallback");
            }
            else if (fallback)
            {
                matched = fallback;
                matchedFriendly = fallbackFriendly;
                matchedDisplay = fallbackDisplay;
                logMessage("[Capture] Falling back to first enumerated device");
            }
        }

        if (!matched)
        {
            throw std::runtime_error("Failed to select a video capture device");
        }

        selectedMoniker = matched;
        selectedFriendlyName = matchedFriendly;
        selectedMonikerDisplayName = matchedDisplay;
        if (selectedFriendlyName.empty() && !selectedMonikerDisplayName.empty())
        {
            selectedFriendlyName = selectedMonikerDisplayName;
        }

        logMessage("[Capture] Using device: " + narrow(selectedFriendlyName.empty() ? selectedMonikerDisplayName : selectedFriendlyName));
        {
            std::lock_guard<std::mutex> lock(deviceInfoMutex);
            deviceInfo.moniker = narrow(selectedMonikerDisplayName);
            deviceInfo.friendlyName = narrow(selectedFriendlyName);
        }
    }

    void buildGraph()
    {
        throwIfFailed(CoCreateInstance(CLSID_FilterGraph,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&graph)),
                      "Failed to create FilterGraph");

        throwIfFailed(CoCreateInstance(CLSID_CaptureGraphBuilder2,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&captureBuilder)),
                      "Failed to create CaptureGraphBuilder2");

        throwIfFailed(captureBuilder->SetFiltergraph(graph.Get()), "Failed to associate filter graph");

        const std::wstring filterName = !selectedFriendlyName.empty() ? selectedFriendlyName :
            (!selectedMonikerDisplayName.empty() ? selectedMonikerDisplayName : std::wstring(L"Video Capture Source"));

        throwIfFailed(addSourceFilterForMoniker(selectedMoniker.Get(), filterName.c_str(), captureFilter.GetAddressOf()),
                      "Failed to add capture filter");

        throwIfFailed(CoCreateInstance(kCLSID_SampleGrabber,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&sampleGrabberFilter)),
                      "Failed to create Sample Grabber filter");

        throwIfFailed(sampleGrabberFilter->QueryInterface(kIID_ISampleGrabber, reinterpret_cast<void**>(sampleGrabber.GetAddressOf())),
                      "Failed to query ISampleGrabber");

        throwIfFailed(graph->AddFilter(sampleGrabberFilter.Get(), L"Sample Grabber"),
                      "Failed to add Sample Grabber to graph");

        throwIfFailed(CoCreateInstance(kCLSID_NullRenderer,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&nullRenderer)),
                      "Failed to create Null Renderer");
        throwIfFailed(graph->AddFilter(nullRenderer.Get(), L"Null Renderer"),
                      "Failed to add Null Renderer to graph");

        callback = new SampleGrabberCallback(this);
        throwIfFailed(sampleGrabber->SetOneShot(FALSE), "Failed to configure Sample Grabber");
        throwIfFailed(sampleGrabber->SetBufferSamples(FALSE), "Failed to disable Sample Grabber buffering");
        throwIfFailed(sampleGrabber->SetCallback(callback, 1), "Failed to set Sample Grabber callback");

        ComPtr<IAMStreamConfig> streamConfig;
        HRESULT hrConfig = captureBuilder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                                         &MEDIATYPE_Video,
                                                         captureFilter.Get(),
                                                         IID_PPV_ARGS(streamConfig.GetAddressOf()));
        if (FAILED(hrConfig) || !streamConfig)
        {
            hrConfig = captureBuilder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                                     &MEDIATYPE_Video,
                                                     captureFilter.Get(),
                                                     IID_PPV_ARGS(streamConfig.GetAddressOf()));
        }

        if (streamConfig)
        {
            auto modes = enumerateVideoModes(streamConfig.Get());
            std::vector<::VideoFormatPreference> formats;
            for (const auto& mode : modes)
            {
                if (std::find(formats.begin(), formats.end(), mode.format) == formats.end())
                {
                    formats.push_back(mode.format);
                }
            }
            std::lock_guard<std::mutex> lock(deviceInfoMutex);
            deviceInfo.modes = std::move(modes);
            deviceInfo.formats = std::move(formats);
        }
        else if (requestedWidth || requestedHeight || requestedFrameRate100)
        {
            throw std::runtime_error("The capture device does not expose configurable video modes. Check its driver or try another capture device.");
        }

        GUID requestedSubtype = kPreferredVideoSubtypeXrgb;
        switch (requestedFormatPreference)
        {
        case DirectShowCapture::VideoFormatPreference::NV12:
            requestedSubtype = kPreferredVideoSubtypeNv12;
            break;
        case DirectShowCapture::VideoFormatPreference::Auto:
        case DirectShowCapture::VideoFormatPreference::XRGB:
        default:
            requestedSubtype = kPreferredVideoSubtypeXrgb;
            break;
        }

        GUID negotiatedSubtype = requestedSubtype;
        if (streamConfig)
        {
            negotiatedSubtype = applyRequestedFormat(streamConfig.Get(), requestedSubtype);
        }

        AM_MEDIA_TYPE mediaType{};
        mediaType.majortype = MEDIATYPE_Video;
        mediaType.formattype = FORMAT_VideoInfo;
        mediaType.subtype = negotiatedSubtype;
        throwIfFailed(sampleGrabber->SetMediaType(&mediaType), "Failed to set Sample Grabber media type");
        logMessage(std::string("[Capture] Sample Grabber requested ") + subtypeName(mediaType.subtype) + " media subtype");

        HRESULT hr = captureBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, captureFilter.Get(), sampleGrabberFilter.Get(), nullRenderer.Get());
        if (FAILED(hr))
        {
            hr = captureBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, captureFilter.Get(), sampleGrabberFilter.Get(), nullRenderer.Get());
        }
        throwIfFailed(hr, "Failed to build capture graph");

        logCurrentFormat("Negotiated capture format (post RenderStream)");

        if (audioEnabled)
        {
            HRESULT audioHr = captureBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, captureFilter.Get(), nullptr, nullptr);
            if (FAILED(audioHr))
            {
                audioHr = captureBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Audio, captureFilter.Get(), nullptr, nullptr);
            }
            if (SUCCEEDED(audioHr))
            {
                logMessage("[Capture] Audio playback path connected");
            }
            else
            {
                logMessage("[Capture] Failed to connect audio playback path; continuing without audio");
                audioEnabled = false;
            }
        }

        logSampleGrabberFormat();
        // RenderStream may insert filters and renegotiate the source. Validate the format
        // delivered to the callback, rather than assuming SetFormat survived connection.
        if ((requestedWidth && requestedWidth != frameWidth) ||
            (requestedHeight && requestedHeight != frameHeight))
        {
            throw std::runtime_error("The connected capture stream changed the requested resolution.");
        }
        if (requestedFrameRate100 && requestedFrameRate100 != activeFrameRate100)
        {
            std::ostringstream message;
            message << "Requested " << std::fixed << std::setprecision(2)
                    << static_cast<double>(requestedFrameRate100) / 100.0
                    << " FPS, but the connected stream selected "
                    << static_cast<double>(activeFrameRate100) / 100.0 << " FPS.";
            const auto difference = activeFrameRate100 > requestedFrameRate100
                ? activeFrameRate100 - requestedFrameRate100 : requestedFrameRate100 - activeFrameRate100;
            if (!activeFrameRate100 || difference > std::max<std::uint32_t>(1, requestedFrameRate100 / 100))
            {
                throw std::runtime_error(message.str());
            }
            std::lock_guard<std::mutex> lock(deviceInfoMutex);
            deviceInfo.warning = message.str();
        }

        ComPtr<IMediaFilter> mediaFilter;
        if (SUCCEEDED(graph.As(&mediaFilter)) && mediaFilter)
        {
            mediaFilter->SetSyncSource(nullptr);
        }

        throwIfFailed(graph->QueryInterface(IID_PPV_ARGS(&control)), "Failed to query IMediaControl");
    }

    GUID applyRequestedFormat(IAMStreamConfig* streamConfig, const GUID& defaultSubtype)
    {
        if (!streamConfig)
        {
            return defaultSubtype;
        }

        int capabilityCount = 0;
        int capabilitySize = 0;
        if (FAILED(streamConfig->GetNumberOfCapabilities(&capabilityCount, &capabilitySize)) || capabilityCount <= 0 || capabilitySize <= 0)
        {
            throw std::runtime_error("Unable to read capture modes from the device.");
        }

        const bool hasRequestedResolution = requestedWidth != 0 && requestedHeight != 0;
        const bool hasRequestedFrameRate = requestedFrameRate100 != 0;
        const auto formatRate = [](std::uint32_t rate) {
            std::ostringstream text;
            text << std::fixed << std::setprecision(2) << static_cast<double>(rate) / 100.0;
            return text.str();
        };
        auto mediaTypeDeleter = [](AM_MEDIA_TYPE* type) {
            if (type)
            {
                freeMediaType(*type);
                CoTaskMemFree(type);
            }
        };
        using MediaTypePtr = std::unique_ptr<AM_MEDIA_TYPE, decltype(mediaTypeDeleter)>;
        struct Candidate
        {
            MediaTypePtr type;
            int score;
        };
        std::vector<Candidate> candidates;
        std::vector<std::uint8_t> capabilityBuffer(static_cast<std::size_t>(capabilitySize));
        for (int i = 0; i < capabilityCount; ++i)
        {
            std::fill(capabilityBuffer.begin(), capabilityBuffer.end(), 0);
            AM_MEDIA_TYPE* raw = nullptr;
            const HRESULT hr = streamConfig->GetStreamCaps(i, &raw, capabilityBuffer.data());
            MediaTypePtr type(raw, mediaTypeDeleter);
            if (FAILED(hr) || !type)
            {
                continue;
            }
            auto* info = CaptureCapabilities::videoInfo(type.get());
            if (!info)
            {
                continue;
            }
            const bool nv12 = isSubtype(type->subtype, kPreferredVideoSubtypeNv12);
            if ((requestedFormatPreference == DirectShowCapture::VideoFormatPreference::NV12 && !nv12) ||
                (requestedFormatPreference == DirectShowCapture::VideoFormatPreference::XRGB && nv12))
            {
                continue;
            }
            if (hasRequestedResolution &&
                (static_cast<std::uint32_t>(info->bmiHeader.biWidth) != requestedWidth ||
                 static_cast<std::uint32_t>(info->bmiHeader.biHeight) != requestedHeight))
            {
                continue;
            }

            const auto caps = CaptureCapabilities::streamCaps(capabilityBuffer.data(), capabilityBuffer.size());
            const auto defaultRate = FrameRateOptions::rateForInterval(info->AvgTimePerFrame);
            int score = isXrgb32CompatibleSubtype(type->subtype) ? 450 : (nv12 ? 400 : 300);
            if (hasRequestedFrameRate)
            {
                if (!FrameRateOptions::allowsRate(info->AvgTimePerFrame, caps.MinFrameInterval,
                                                  caps.MaxFrameInterval, requestedFrameRate100))
                {
                    continue;
                }
                if (defaultRate == requestedFrameRate100)
                {
                    score += 10000;
                }
                // Request the interval explicitly for range-based modes. Preserve a matching
                // native interval exactly: rounding FPS to two decimals and back can shift
                // one tick, which strict discrete-mode drivers may reject.
                if (defaultRate != requestedFrameRate100)
                {
                    info->AvgTimePerFrame = FrameRateOptions::intervalForRate(requestedFrameRate100);
                }
            }
            candidates.push_back({std::move(type), score});
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });
        if (candidates.empty())
        {
            std::ostringstream message;
            message << "The device does not advertise a compatible ";
            if (hasRequestedResolution) message << requestedWidth << "x" << requestedHeight << " ";
            if (hasRequestedFrameRate) message << formatRate(requestedFrameRate100) << " FPS ";
            message << "capture mode. Select an available mode and try Automatic color format.";
            throw std::runtime_error(message.str());
        }

        // Drivers can round requested rates. Keep a close alternative while looking for an
        // exact match in another supported subtype, instead of silently choosing the first one.
        MediaTypePtr roundedFallback(nullptr, mediaTypeDeleter);
        std::uint32_t smallestDifference = std::numeric_limits<std::uint32_t>::max();
        std::string lastFailure = "The device rejected the requested capture mode.";
        const auto acceptActual = [&](AM_MEDIA_TYPE* actual, bool allowRounded) -> bool {
            const auto* info = CaptureCapabilities::videoInfo(actual);
            if (!info)
            {
                lastFailure = "The device returned an unsupported capture format.";
                return false;
            }
            const bool actualNv12 = isSubtype(actual->subtype, kPreferredVideoSubtypeNv12);
            if ((requestedFormatPreference == DirectShowCapture::VideoFormatPreference::NV12 && !actualNv12) ||
                (requestedFormatPreference == DirectShowCapture::VideoFormatPreference::XRGB && actualNv12))
            {
                lastFailure = "The device changed the requested color format. Try Automatic color format.";
                return false;
            }
            if (hasRequestedResolution &&
                (static_cast<std::uint32_t>(info->bmiHeader.biWidth) != requestedWidth ||
                 static_cast<std::uint32_t>(info->bmiHeader.biHeight) != requestedHeight))
            {
                lastFailure = "The device changed the requested capture resolution.";
                return false;
            }
            const auto rate = FrameRateOptions::rateForInterval(info->AvgTimePerFrame);
            if (hasRequestedFrameRate && rate != requestedFrameRate100)
            {
                lastFailure = "Requested " + formatRate(requestedFrameRate100) +
                              " FPS, but the device selected " + formatRate(rate) + " FPS.";
                if (!allowRounded)
                {
                    return false;
                }
            }
            {
                std::lock_guard<std::mutex> lock(deviceInfoMutex);
                deviceInfo.width = static_cast<std::uint32_t>(info->bmiHeader.biWidth);
                deviceInfo.height = static_cast<std::uint32_t>(info->bmiHeader.biHeight);
                deviceInfo.frameRate100 = rate;
                deviceInfo.warning = hasRequestedFrameRate && rate != requestedFrameRate100 ? lastFailure : std::string{};
            }
            logMessage("[Capture] Applied " + std::to_string(info->bmiHeader.biWidth) + "x" +
                       std::to_string(info->bmiHeader.biHeight) + " @" + formatRate(rate) +
                       " FPS subtype=" + subtypeName(actual->subtype));
            return true;
        };

        for (auto& candidate : candidates)
        {
            const HRESULT setResult = streamConfig->SetFormat(candidate.type.get());
            if (FAILED(setResult))
            {
                lastFailure = "The device rejected the requested capture mode: " + formatHr(setResult);
                logMessage("[Capture] " + lastFailure);
                continue;
            }
            AM_MEDIA_TYPE* rawActual = nullptr;
            const HRESULT getResult = streamConfig->GetFormat(&rawActual);
            MediaTypePtr actual(rawActual, mediaTypeDeleter);
            if (FAILED(getResult) || !actual)
            {
                lastFailure = "The device accepted the capture setting but its actual format could not be read back.";
                continue;
            }
            if (acceptActual(actual.get(), false))
            {
                return actual->subtype;
            }
            logMessage("[Capture] " + lastFailure);
            if (const auto* info = CaptureCapabilities::videoInfo(actual.get()))
            {
                const auto actualRate = FrameRateOptions::rateForInterval(info->AvgTimePerFrame);
                const auto difference = actualRate > requestedFrameRate100
                    ? actualRate - requestedFrameRate100 : requestedFrameRate100 - actualRate;
                const bool dimensionsMatch = !hasRequestedResolution ||
                    (static_cast<std::uint32_t>(info->bmiHeader.biWidth) == requestedWidth &&
                     static_cast<std::uint32_t>(info->bmiHeader.biHeight) == requestedHeight);
                const bool actualNv12 = isSubtype(actual->subtype, kPreferredVideoSubtypeNv12);
                const bool formatMatches = requestedFormatPreference == DirectShowCapture::VideoFormatPreference::Auto ||
                    (requestedFormatPreference == DirectShowCapture::VideoFormatPreference::NV12 && actualNv12) ||
                    (requestedFormatPreference == DirectShowCapture::VideoFormatPreference::XRGB && !actualNv12);
                // Permit ordinary fractional-rate rounding (e.g. 60 -> 59.94), never a
                // silent 240 -> 60 FPS fallback. The UI receives requested-vs-actual text.
                if (hasRequestedFrameRate && dimensionsMatch && formatMatches && actualRate != 0 &&
                    difference <= std::max<std::uint32_t>(1, requestedFrameRate100 / 100) &&
                    difference < smallestDifference)
                {
                    smallestDifference = difference;
                    roundedFallback = std::move(actual);
                }
            }
        }
        if (roundedFallback && SUCCEEDED(streamConfig->SetFormat(roundedFallback.get())))
        {
            AM_MEDIA_TYPE* rawActual = nullptr;
            const HRESULT getResult = streamConfig->GetFormat(&rawActual);
            MediaTypePtr actual(rawActual, mediaTypeDeleter);
            const auto* info = actual ? CaptureCapabilities::videoInfo(actual.get()) : nullptr;
            const auto actualRate = info ? FrameRateOptions::rateForInterval(info->AvgTimePerFrame) : 0;
            const auto difference = actualRate > requestedFrameRate100
                ? actualRate - requestedFrameRate100 : requestedFrameRate100 - actualRate;
            if (SUCCEEDED(getResult) && actualRate != 0 &&
                difference <= std::max<std::uint32_t>(1, requestedFrameRate100 / 100) &&
                acceptActual(actual.get(), true))
            {
                return actual->subtype;
            }
        }
        throw std::runtime_error(lastFailure + " Select another listed rate or color format.");
    }

    void logCurrentFormat(const std::string& context)
    {
        if (!captureBuilder || !captureFilter)
        {
            logMessage("[Capture] " + context + ": capture builder unavailable");
            return;
        }

        ComPtr<IAMStreamConfig> streamConfig;
        HRESULT hr = captureBuilder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                                    &MEDIATYPE_Video,
                                                    captureFilter.Get(),
                                                    IID_PPV_ARGS(streamConfig.GetAddressOf()));
        if (FAILED(hr) || !streamConfig)
        {
            hr = captureBuilder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                               &MEDIATYPE_Video,
                                               captureFilter.Get(),
                                               IID_PPV_ARGS(streamConfig.GetAddressOf()));
        }

        if (FAILED(hr) || !streamConfig)
        {
            logMessage("[Capture] " + context + ": IAMStreamConfig not available");
            return;
        }

        AM_MEDIA_TYPE* currentType = nullptr;
        hr = streamConfig->GetFormat(&currentType);
        if (FAILED(hr) || !currentType)
        {
            logMessage("[Capture] " + context + ": IAMStreamConfig::GetFormat failed");
            return;
        }

        const bool hasVideoInfo = currentType->formattype == FORMAT_VideoInfo &&
            currentType->cbFormat >= sizeof(VIDEOINFOHEADER) && currentType->pbFormat;
        if (!hasVideoInfo)
        {
            logMessage("[Capture] " + context + ": unexpected media type");
        }
        else
        {
            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(currentType->pbFormat);
            describeVideoInfo(*vih, currentType->subtype, context, false);
        }

        if (currentType)
        {
            if (currentType->cbFormat != 0 && currentType->pbFormat)
            {
                CoTaskMemFree(currentType->pbFormat);
                currentType->pbFormat = nullptr;
                currentType->cbFormat = 0;
            }
            if (currentType->pUnk)
            {
                currentType->pUnk->Release();
                currentType->pUnk = nullptr;
            }
            CoTaskMemFree(currentType);
        }
    }

    void logSampleGrabberFormat()
    {
        if (!sampleGrabber)
        {
            return;
        }

        AM_MEDIA_TYPE connected{};
        HRESULT hr = sampleGrabber->GetConnectedMediaType(&connected);
        if (FAILED(hr))
        {
            logMessage("[Capture] SampleGrabber::GetConnectedMediaType failed");
            return;
        }

        const auto* vih = CaptureCapabilities::videoInfo(&connected);
        if (!vih)
        {
            freeMediaType(connected);
            throw std::runtime_error("Sample Grabber did not provide a supported RGB or NV12 capture format.");
        }

        describeVideoInfo(*vih, connected.subtype, "SampleGrabber format", true);
        freeMediaType(connected);
    }

    void describeVideoInfo(const VIDEOINFOHEADER& vih, const GUID& subtype, const std::string& context, bool updateState)
    {
        const LONG biWidth = vih.bmiHeader.biWidth;
        const LONG biHeight = vih.bmiHeader.biHeight;
        const std::uint32_t width = static_cast<std::uint32_t>(std::abs(biWidth));
        const std::uint32_t height = static_cast<std::uint32_t>(std::abs(biHeight));
        const DWORD bits = vih.bmiHeader.biBitCount ? vih.bmiHeader.biBitCount : 32;
        std::uint32_t nominalFrameRate100 = 0;
        if (vih.AvgTimePerFrame > 0)
        {
            const double frameRate = 10'000'000.0 / static_cast<double>(vih.AvgTimePerFrame);
            nominalFrameRate100 = static_cast<std::uint32_t>(std::llround(frameRate * 100.0));
        }

        RECT active = vih.rcSource;
        if (active.right <= active.left || active.bottom <= active.top)
        {
            active.left = 0;
            active.top = 0;
            active.right = static_cast<LONG>(width);
            active.bottom = static_cast<LONG>(height);
        }

        const auto clampRect = [](LONG value, LONG minValue, LONG maxValue) {
            return std::clamp(value, minValue, maxValue);
        };

        active.left = clampRect(active.left, 0, static_cast<LONG>(width));
        active.top = clampRect(active.top, 0, static_cast<LONG>(height));
        active.right = clampRect(active.right, active.left + 1, static_cast<LONG>(width));
        active.bottom = clampRect(active.bottom, active.top + 1, static_cast<LONG>(height));

        std::uint32_t stride = width;
        bool isBottomUp = biHeight > 0;
        ActiveSubtype subtypeState = ActiveSubtype::Unknown;
        const DWORD imageSize = vih.bmiHeader.biSizeImage;

        if (isXrgb32CompatibleSubtype(subtype))
        {
            stride = width * 4;
            subtypeState = ActiveSubtype::XRGB;
        }
        else if (isSubtype(subtype, kPreferredVideoSubtypeRgb24))
        {
            stride = (width * 3u + 3u) & ~3u;
            subtypeState = ActiveSubtype::RGB24;
        }
        else if (isSubtype(subtype, kPreferredVideoSubtypeNv12))
        {
            stride = width;
            const std::uint32_t totalRows = height + ((height + 1u) / 2u);
            if (imageSize > 0 && totalRows > 0)
            {
                const std::uint32_t inferredStride = imageSize / totalRows;
                if (inferredStride >= width)
                {
                    stride = inferredStride;
                }
            }
            isBottomUp = false;
            subtypeState = ActiveSubtype::NV12;
        }
        else
        {
            std::uint32_t bytesPerPixel = bits != 0 ? static_cast<std::uint32_t>((bits + 7u) / 8u) : 4u;
            if (bytesPerPixel == 0)
            {
                bytesPerPixel = 4;
            }
            stride = width * bytesPerPixel;
        }

        std::ostringstream oss;
        oss << "[Capture] " << context
            << ": frame=" << width << "x" << height
            << " @" << std::fixed << std::setprecision(2) << (static_cast<double>(nominalFrameRate100) / 100.0)
             << " stride=" << stride
             << " subtype=" << subtypeName(subtype)
             << " bottomUp=" << (isBottomUp ? "true" : "false")
             << " rcSource={" << active.left << ", " << active.top << ", " << active.right << ", " << active.bottom << "}";
        logMessage(oss.str());

        if (updateState)
        {
            frameWidth = width;
            frameHeight = height;
            frameStride = stride;
            bottomUp = isBottomUp;
            activeSubtype = subtypeState;
            contentLeft = static_cast<std::uint32_t>(active.left);
            contentTop = static_cast<std::uint32_t>(active.top);
            contentRight = static_cast<std::uint32_t>(active.right);
            contentBottom = static_cast<std::uint32_t>(active.bottom);
            activeFrameRate100 = nominalFrameRate100;
            std::lock_guard<std::mutex> lock(deviceInfoMutex);
            deviceInfo.width = width;
            deviceInfo.height = height;
            deviceInfo.frameRate100 = nominalFrameRate100;
        }
    }

    void convertRgb24ToBgra(const BYTE* source, std::size_t sourceSize)
    {
        const std::size_t rowBytes = static_cast<std::size_t>(frameWidth) * 3;
        const std::size_t requiredBytes = static_cast<std::size_t>(frameStride) * frameHeight;
        if (sourceSize < requiredBytes)
        {
            convertedBuffer.clear();
            return;
        }

        const std::size_t outStride = static_cast<std::size_t>(frameWidth) * 4;
        convertedBuffer.resize(outStride * frameHeight);

        for (std::uint32_t y = 0; y < frameHeight; ++y)
        {
            const std::uint32_t srcY = bottomUp ? (frameHeight - 1 - y) : y;
            const BYTE* src = source + static_cast<std::size_t>(srcY) * frameStride;
            std::uint8_t* dst = convertedBuffer.data() + static_cast<std::size_t>(y) * outStride;

            for (std::uint32_t x = 0; x < frameWidth; ++x)
            {
                const std::size_t srcOffset = static_cast<std::size_t>(x) * 3;
                if (srcOffset + 2 >= rowBytes)
                {
                    break;
                }

                const std::size_t dstOffset = static_cast<std::size_t>(x) * 4;
                dst[dstOffset + 0] = src[srcOffset + 0];
                dst[dstOffset + 1] = src[srcOffset + 1];
                dst[dstOffset + 2] = src[srcOffset + 2];
                dst[dstOffset + 3] = 255;
            }
        }
    }

    HRESULT processBuffer(double sampleTime, const BYTE* buffer, long bufferLen)
    {
        const auto receivedAt = std::chrono::steady_clock::now();
        if (!running.load(std::memory_order_acquire) || !handler)
        {
            return S_OK;
        }

        if (bufferLen <= 0 || frameWidth == 0 || frameHeight == 0)
        {
            return S_OK;
        }

        DirectShowCapture::Frame frame{};
        frame.receivedAt = receivedAt;
        frame.sampleWidth = frameWidth;
        frame.sampleHeight = frameHeight;
        frame.contentLeft = contentLeft;
        frame.contentTop = contentTop;
        frame.contentRight = contentRight != 0 ? (contentRight) : frameWidth;
        frame.contentBottom = contentBottom != 0 ? (contentBottom) : frameHeight;
        frame.nominalFrameRate100 = activeFrameRate100;

        frame.width = frameWidth;
        frame.height = frameHeight;
        frame.timestamp100ns = sampleTime >= 0.0 ? static_cast<std::uint64_t>(sampleTime * 10'000'000.0) : 0;
        frame.pixelFormat = DirectShowCapture::PixelFormat::BGRA8;

        if (activeSubtype == ActiveSubtype::NV12)
        {
            frame.data = buffer;
            frame.dataSize = static_cast<std::size_t>(bufferLen);
            frame.stride = frameStride != 0 ? frameStride : frameWidth;
            frame.pixelFormat = DirectShowCapture::PixelFormat::NV12;
            frame.bottomUp = false;
        }
        else if (activeSubtype == ActiveSubtype::RGB24)
        {
            convertRgb24ToBgra(buffer, static_cast<std::size_t>(bufferLen));
            if (convertedBuffer.empty())
            {
                return S_OK;
            }
            frame.data = convertedBuffer.data();
            frame.dataSize = convertedBuffer.size();
            frame.stride = frameWidth * 4;
            frame.bottomUp = false;
        }
        else
        {
            frame.data = buffer;
            frame.dataSize = static_cast<std::size_t>(bufferLen);
            frame.stride = frameStride != 0 ? frameStride : frameWidth * 4;
            frame.bottomUp = bottomUp;
        }

        try
        {
            if (!loggedSampleSize.exchange(true, std::memory_order_acq_rel))
            {
                logMessage("[Capture] First sample size=" + std::to_string(frame.dataSize));
            }
            handler(frame);
            frameReceived.store(true, std::memory_order_release);
        }
        catch (...)
        {
            storeRuntimeError(std::current_exception());
            return E_FAIL;
        }

        return S_OK;
    }

    void releaseGraph()
    {
        if (sampleGrabber)
        {
            sampleGrabber->SetCallback(nullptr, 0);
        }
        if (callback)
        {
            callback->resetOwner();
            callback->Release();
            callback = nullptr;
        }

        if (control)
        {
            control->Stop();
        }

        nullRenderer.Reset();
        sampleGrabber.Reset();
        sampleGrabberFilter.Reset();
        captureFilter.Reset();
        control.Reset();
        captureBuilder.Reset();
        graph.Reset();
        selectedMoniker.Reset();
        frameWidth = frameHeight = frameStride = 0;
        contentLeft = contentTop = 0;
        contentRight = contentBottom = 0;
        bottomUp = false;
        activeSubtype = ActiveSubtype::Unknown;
        activeFrameRate100 = 0;
        convertedBuffer.clear();
        audioEnabled = false;
    }

    HRESULT addSourceFilterForMoniker(IMoniker* moniker, const wchar_t* name, IBaseFilter** outFilter)
    {
        if (!moniker || !graph)
        {
            return E_POINTER;
        }

        ComPtr<IBaseFilter> filter;
        HRESULT hr = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = graph->AddFilter(filter.Get(), name);
        if (FAILED(hr))
        {
            return hr;
        }

        if (outFilter)
        {
            *outFilter = filter.Detach();
        }

        return S_OK;
    }

    void storeRuntimeError(const std::exception_ptr& err)
    {
        if (!err)
        {
            return;
        }
        try
        {
            std::rethrow_exception(err);
        }
        catch (const std::exception& ex)
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = ex.what();
            {
                std::lock_guard<std::mutex> infoLock(deviceInfoMutex);
                deviceInfo.warning = lastError;
            }
            logMessage(std::string("[Capture] Runtime exception: ") + lastError);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastError = "Unknown capture error";
            {
                std::lock_guard<std::mutex> infoLock(deviceInfoMutex);
                deviceInfo.warning = lastError;
            }
            logMessage("[Capture] Runtime exception: unknown");
        }
    }
};

HRESULT SampleGrabberCallback::BufferCB(double sampleTime, BYTE* buffer, long bufferLen)
{
    auto* owner = owner_.load(std::memory_order_acquire);
    if (!owner)
    {
        return S_OK;
    }
    return owner->processBuffer(sampleTime, buffer, bufferLen);
}

DirectShowCapture::DirectShowCapture()
    : impl_(std::make_unique<DirectShowCaptureImpl>())
{
}

DirectShowCapture::~DirectShowCapture() = default;

void DirectShowCapture::start(FrameHandler handler, const Options& options)
{
    impl_->start(std::move(handler), options);
}

void DirectShowCapture::stop()
{
    impl_->stop();
}

std::string DirectShowCapture::consumeLastError()
{
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    std::string result = impl_->lastError;
    impl_->lastError.clear();
    return result;
}

std::string DirectShowCapture::currentDeviceFriendlyName() const
{
    return impl_->currentFriendlyName();
}

DirectShowCapture::DeviceInfo DirectShowCapture::deviceInfo() const
{
    std::lock_guard<std::mutex> lock(impl_->deviceInfoMutex);
    return impl_->deviceInfo;
}
