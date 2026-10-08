#include "DeviceEnumeration.hpp"
#include "CaptureCapabilities.hpp"
#include "FrameRateOptions.hpp"

#include <Windows.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <dshow.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <winreg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Setupapi.lib")

namespace
{
    using Microsoft::WRL::ComPtr;

    void logFormatEnum(const std::string& message)
    {
        std::ofstream("viewer.log", std::ios::app) << "[FormatEnum] " << message << '\n';
    }

    std::string guidToString(const GUID& guid)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(8) << static_cast<unsigned long>(guid.Data1) << "-"
            << std::setw(4) << static_cast<unsigned int>(guid.Data2) << "-"
            << std::setw(4) << static_cast<unsigned int>(guid.Data3) << "-"
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[0])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[1]) << "-"
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[2])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[3])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[4])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[5])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[6])
            << std::setw(2) << static_cast<unsigned int>(guid.Data4[7]);
        return oss.str();
    }

    const char* mediaSubtypeName(const GUID& subtype)
    {
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB24)) return "RGB24";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB32)) return "RGB32";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_ARGB32)) return "ARGB32";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_NV12)) return "NV12";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_YUY2)) return "YUY2";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_P010)) return "P010";
        if (InlineIsEqualGUID(subtype, MEDIASUBTYPE_MJPG)) return "MJPG";
        return "UNKNOWN";
    }

    bool isXrgbCompatibleSubtype(const GUID& subtype)
    {
        return InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB24) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB32) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_ARGB32);
    }

    class ScopedCoInit
    {
    public:
        explicit ScopedCoInit(DWORD flags)
        {
            const HRESULT hr = CoInitializeEx(nullptr, flags);
            if (SUCCEEDED(hr))
            {
                shouldUninit_ = true;
            }
            else if (hr == RPC_E_CHANGED_MODE)
            {
                shouldUninit_ = false;
            }
        }

        ~ScopedCoInit()
        {
            if (shouldUninit_)
            {
                CoUninitialize();
            }
        }

    private:
        bool shouldUninit_ = false;
    };

    std::string wideToUtf8(const std::wstring& input)
    {
        if (input.empty())
        {
            return {};
        }
        const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (sizeNeeded <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(sizeNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), result.data(), sizeNeeded, nullptr, nullptr);
        return result;
    }

    std::string bstrToUtf8(BSTR value)
    {
        if (!value)
        {
            return {};
        }
        return wideToUtf8(std::wstring(value, SysStringLen(value)));
    }

    std::wstring utf8ToWide(const std::string& input)
    {
        if (input.empty())
        {
            return {};
        }
        const int required = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), result.data(), required);
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

    template <typename DeviceInfo>
    std::vector<DeviceInfo> enumerateCategory(REFCLSID category)
    {
        ScopedCoInit coInit(COINIT_APARTMENTTHREADED);

        std::vector<DeviceInfo> devices;

        ComPtr<ICreateDevEnum> devEnum;
        if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))))
        {
            return devices;
        }

        ComPtr<IEnumMoniker> enumMoniker;
        if (devEnum->CreateClassEnumerator(category, enumMoniker.GetAddressOf(), 0) != S_OK || !enumMoniker)
        {
            return devices;
        }

        ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        while (enumMoniker->Next(1, moniker.GetAddressOf(), &fetched) == S_OK)
        {
            DeviceInfo info;

            LPOLESTR displayName = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(nullptr, nullptr, &displayName)) && displayName)
            {
                info.monikerDisplayName = wideToUtf8(displayName);
                CoTaskMemFree(displayName);
            }

            ComPtr<IPropertyBag> props;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&props))) && props)
            {
                VARIANT friendly;
                VariantInit(&friendly);
                if (SUCCEEDED(props->Read(L"FriendlyName", &friendly, nullptr)) && friendly.vt == VT_BSTR)
                {
                    info.friendlyName = bstrToUtf8(friendly.bstrVal);
                }
                VariantClear(&friendly);
            }

            devices.push_back(std::move(info));
            moniker.Reset();
        }

        return devices;
    }
}

std::vector<VideoDeviceInfo> enumerateVideoCaptureDevices()
{
    return enumerateCategory<VideoDeviceInfo>(CLSID_VideoInputDeviceCategory);
}

std::vector<AudioCaptureDeviceInfo> enumerateAudioCaptureDevices()
{
    return enumerateCategory<AudioCaptureDeviceInfo>(CLSID_AudioInputDeviceCategory);
}

std::vector<AudioRenderDeviceInfo> enumerateAudioRenderDevices()
{
    return enumerateCategory<AudioRenderDeviceInfo>(CLSID_AudioRendererCategory);
}

std::vector<VideoModeInfo> enumerateVideoModes(const std::string& monikerDisplayName)
{
    std::vector<VideoModeInfo> modes;
    if (monikerDisplayName.empty())
    {
        return modes;
    }

    ScopedCoInit coInit(COINIT_MULTITHREADED);

    ComPtr<IGraphBuilder> graph;
    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))))
    {
        return modes;
    }

    ComPtr<ICaptureGraphBuilder2> builder;
    if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))))
    {
        return modes;
    }

    if (FAILED(builder->SetFiltergraph(graph.Get())))
    {
        return modes;
    }

    const std::wstring monikerWide = utf8ToWide(monikerDisplayName);
    if (monikerWide.empty())
    {
        return modes;
    }

    ComPtr<IBindCtx> bindCtx;
    if (FAILED(CreateBindCtx(0, &bindCtx)))
    {
        return modes;
    }

    ULONG eaten = 0;
    ComPtr<IMoniker> moniker;
    if (FAILED(MkParseDisplayName(bindCtx.Get(), monikerWide.c_str(), &eaten, moniker.GetAddressOf())) || !moniker)
    {
        return modes;
    }

    ComPtr<IBaseFilter> captureFilter;
    if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&captureFilter))) || !captureFilter)
    {
        return modes;
    }

    if (FAILED(graph->AddFilter(captureFilter.Get(), L"Source")))
    {
        return modes;
    }

    ComPtr<IAMStreamConfig> streamConfig;
    HRESULT hr = builder->FindInterface(&PIN_CATEGORY_CAPTURE,
                                        &MEDIATYPE_Video,
                                        captureFilter.Get(),
                                        IID_PPV_ARGS(streamConfig.GetAddressOf()));
    if (FAILED(hr) || !streamConfig)
    {
        hr = builder->FindInterface(&PIN_CATEGORY_PREVIEW,
                                     &MEDIATYPE_Video,
                                     captureFilter.Get(),
                                     IID_PPV_ARGS(streamConfig.GetAddressOf()));
    }

    if (FAILED(hr) || !streamConfig)
    {
        return modes;
    }

    return enumerateVideoModes(streamConfig.Get());
}

std::vector<VideoModeInfo> enumerateVideoModes(IAMStreamConfig* streamConfig)
{
    std::vector<VideoModeInfo> modes;
    if (!streamConfig)
    {
        return modes;
    }

    int capabilityCount = 0;
    int capabilitySize = 0;
    if (FAILED(streamConfig->GetNumberOfCapabilities(&capabilityCount, &capabilitySize)) || capabilityCount <= 0 || capabilitySize <= 0)
    {
        logFormatEnum("Unable to read capture capabilities");
        return modes;
    }

    std::vector<std::uint8_t> capabilityBuffer(static_cast<std::size_t>(capabilitySize));
    for (int i = 0; i < capabilityCount; ++i)
    {
        std::fill(capabilityBuffer.begin(), capabilityBuffer.end(), 0);
        AM_MEDIA_TYPE* mediaType = nullptr;
        const HRESULT hr = streamConfig->GetStreamCaps(i, &mediaType, capabilityBuffer.data());
        if (FAILED(hr) || !mediaType)
        {
            if (mediaType)
            {
                freeMediaType(*mediaType);
                CoTaskMemFree(mediaType);
            }
            continue;
        }

        if (const auto* info = CaptureCapabilities::videoInfo(mediaType))
        {
            const auto caps = CaptureCapabilities::streamCaps(capabilityBuffer.data(), capabilityBuffer.size());
            const auto rates = FrameRateOptions::enumerate(info->AvgTimePerFrame, caps.MinFrameInterval, caps.MaxFrameInterval);
            std::ostringstream details;
            details << info->bmiHeader.biWidth << "x" << info->bmiHeader.biHeight
                    << " " << mediaSubtypeName(mediaType->subtype)
                    << " default=" << std::fixed << std::setprecision(2)
                    << FrameRateOptions::rateForInterval(info->AvgTimePerFrame) / 100.0 << " FPS"
                    << " interval100ns=[" << caps.MinFrameInterval << "," << caps.MaxFrameInterval
                    << "] choices=" << rates.size();
            logFormatEnum(details.str());
            for (const auto rate : rates)
            {
                const VideoModeInfo mode{
                    static_cast<std::uint32_t>(info->bmiHeader.biWidth),
                    static_cast<std::uint32_t>(info->bmiHeader.biHeight),
                    static_cast<double>(rate) / 100.0,
                    CaptureCapabilities::formatPreference(mediaType->subtype)
                };
                const bool duplicate = std::any_of(modes.begin(), modes.end(), [&](const VideoModeInfo& existing) {
                    return existing.width == mode.width && existing.height == mode.height &&
                           existing.format == mode.format && std::abs(existing.frameRate - mode.frameRate) < 0.005;
                });
                if (!duplicate)
                {
                    modes.push_back(mode);
                }
            }
        }
        else
        {
            logFormatEnum("Skipping unsupported capture format " + guidToString(mediaType->formattype) +
                          " subtype=" + mediaSubtypeName(mediaType->subtype));
        }
        freeMediaType(*mediaType);
        CoTaskMemFree(mediaType);
    }

    std::sort(modes.begin(), modes.end(), [](const VideoModeInfo& a, const VideoModeInfo& b) {
        if (a.width != b.width) return a.width > b.width;
        if (a.height != b.height) return a.height > b.height;
        if (a.frameRate != b.frameRate) return a.frameRate < b.frameRate;
        return a.format < b.format;
    });
    logFormatEnum("Found " + std::to_string(modes.size()) + " compatible capture mode/rate choices");
    return modes;
}

std::vector<VideoFormatPreference> enumerateVideoFormats(const std::string& monikerDisplayName)
{
    std::vector<VideoFormatPreference> formats;
    for (const auto& mode : enumerateVideoModes(monikerDisplayName))
    {
        if (std::find(formats.begin(), formats.end(), mode.format) == formats.end())
        {
            formats.push_back(mode.format);
        }
    }
    return formats;
}
