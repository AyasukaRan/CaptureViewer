#include "DeviceEnumeration.hpp"

#include <Windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    }

    struct Capability
    {
        GUID subtype;
        GUID formatType;
        REFERENCE_TIME defaultInterval;
        REFERENCE_TIME minimumInterval;
        REFERENCE_TIME maximumInterval;
        bool fails = false;
    };

    class FakeStreamConfig final : public IAMStreamConfig
    {
    public:
        explicit FakeStreamConfig(std::vector<Capability> capabilities, bool queryFails = false)
            : capabilities_(std::move(capabilities)), queryFails_(queryFails) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
        {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (InlineIsEqualGUID(iid, IID_IUnknown) || InlineIsEqualGUID(iid, IID_IAMStreamConfig))
            {
                *object = static_cast<IAMStreamConfig*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG remaining = --references_;
            if (remaining == 0) delete this;
            return remaining;
        }

        HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE** type) override
        {
            if (!type) return E_POINTER;
            *type = nullptr;
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int* count, int* size) override
        {
            if (!count || !size) return E_POINTER;
            *count = 0;
            *size = 0;
            if (queryFails_) return E_FAIL;
            *count = static_cast<int>(capabilities_.size());
            *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetStreamCaps(int index, AM_MEDIA_TYPE** type, BYTE* data) override
        {
            if (!type || !data) return E_POINTER;
            *type = nullptr;
            if (index < 0 || static_cast<std::size_t>(index) >= capabilities_.size()) return E_INVALIDARG;
            const auto& capability = capabilities_[static_cast<std::size_t>(index)];
            if (capability.fails) return E_FAIL;

            auto* mediaType = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!mediaType) return E_OUTOFMEMORY;
            std::memset(mediaType, 0, sizeof(*mediaType));
            mediaType->majortype = MEDIATYPE_Video;
            mediaType->subtype = capability.subtype;
            mediaType->formattype = capability.formatType;
            mediaType->bFixedSizeSamples = TRUE;
            const bool videoInfo2 = InlineIsEqualGUID(capability.formatType, FORMAT_VideoInfo2);
            mediaType->cbFormat = videoInfo2 ? sizeof(VIDEOINFOHEADER2) : sizeof(VIDEOINFOHEADER);
            mediaType->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mediaType->cbFormat));
            if (!mediaType->pbFormat)
            {
                CoTaskMemFree(mediaType);
                return E_OUTOFMEMORY;
            }
            std::memset(mediaType->pbFormat, 0, mediaType->cbFormat);
            BITMAPINFOHEADER* bitmap;
            if (videoInfo2)
            {
                auto* info = reinterpret_cast<VIDEOINFOHEADER2*>(mediaType->pbFormat);
                info->AvgTimePerFrame = capability.defaultInterval;
                bitmap = &info->bmiHeader;
            }
            else
            {
                auto* info = reinterpret_cast<VIDEOINFOHEADER*>(mediaType->pbFormat);
                info->AvgTimePerFrame = capability.defaultInterval;
                bitmap = &info->bmiHeader;
            }
            bitmap->biSize = sizeof(BITMAPINFOHEADER);
            bitmap->biWidth = 1920;
            bitmap->biHeight = 1080;
            bitmap->biPlanes = 1;
            bitmap->biBitCount = InlineIsEqualGUID(capability.subtype, MEDIASUBTYPE_NV12) ? 12 : 32;
            VIDEO_STREAM_CONFIG_CAPS caps{};
            caps.guid = capability.formatType;
            caps.MinFrameInterval = capability.minimumInterval;
            caps.MaxFrameInterval = capability.maximumInterval;
            std::memcpy(data, &caps, sizeof(caps));
            // IAMStreamConfig transfers both COM-task allocations to its caller.
            *type = mediaType;
            return S_OK;
        }

    private:
        std::atomic<ULONG> references_{1};
        std::vector<Capability> capabilities_;
        bool queryFails_;
    };

    std::vector<VideoModeInfo> enumerateFake(std::vector<Capability> caps, bool queryFails = false)
    {
        Microsoft::WRL::ComPtr<IAMStreamConfig> config;
        config.Attach(new FakeStreamConfig(std::move(caps), queryFails));
        return enumerateVideoModes(config.Get());
    }

    bool contains(const std::vector<VideoModeInfo>& modes, double fps, VideoFormatPreference format)
    {
        return std::any_of(modes.begin(), modes.end(), [&](const VideoModeInfo& mode) {
            return mode.width == 1920 && mode.height == 1080 &&
                std::abs(mode.frameRate - fps) < 0.005 && mode.format == format;
        });
    }
}

int main()
{
    using Format = VideoFormatPreference;
    const Capability range{MEDIASUBTYPE_NV12, FORMAT_VideoInfo, 166667, 41667, 333333};
    const auto ranged = enumerateFake({range});
    check(contains(ranged, 30.0, Format::NV12), "includes the slow end of the driver's frame interval range");
    check(contains(ranged, 60.0, Format::NV12), "preserves the driver's 60 FPS default");
    check(contains(ranged, 120.0, Format::NV12), "finds 120 FPS when the capability default is only 60 FPS");
    check(contains(ranged, 240.0, Format::NV12), "finds 240 FPS from the driver's minimum frame interval");
    check(!contains(ranged, 360.0, Format::NV12), "does not invent rates outside the advertised range");

    const Capability fixedNv12{MEDIASUBTYPE_NV12, FORMAT_VideoInfo, 166667, 166667, 166667};
    const Capability fixedRgb32{MEDIASUBTYPE_RGB32, FORMAT_VideoInfo, 166667, 166667, 166667};
    const auto mixed = enumerateFake({fixedNv12, fixedNv12, fixedRgb32, fixedRgb32});
    check(mixed.size() == 2, "deduplicates identical modes while retaining both pixel format families");
    check(contains(mixed, 60.0, Format::NV12), "retains NV12 at the shared resolution and FPS");
    check(contains(mixed, 60.0, Format::XRGB), "retains XRGB at the shared resolution and FPS");

    const auto fixed = enumerateFake({fixedNv12});
    check(fixed.size() == 1 && contains(fixed, 60.0, Format::NV12), "fixed 60 FPS capability remains 60-only");
    const auto unsupported = enumerateFake({
        {MEDIASUBTYPE_MJPG, FORMAT_VideoInfo, 166667, 41667, 333333},
        {MEDIASUBTYPE_NV12, FORMAT_VideoInfo2, 166667, 41667, 333333}
    });
    check(unsupported.empty(), "excludes unsupported MJPG and VideoInfo2 instead of advertising unusable rates");

    Capability failed = range;
    failed.fails = true;
    const auto partial = enumerateFake({failed, fixedRgb32});
    check(partial.size() == 1 && contains(partial, 60.0, Format::XRGB), "skips GetStreamCaps failures and continues with later capabilities");
    check(enumerateFake({failed}).empty(), "all failed capability reads produce an empty mode list");
    check(enumerateFake({fixedNv12}, true).empty(), "capability count query failure produces an empty mode list");
    check(enumerateVideoModes(static_cast<IAMStreamConfig*>(nullptr)).empty(), "null configuration produces an empty mode list");

    if (failures)
    {
        std::cerr << failures << " device enumeration checks failed\n";
        return 1;
    }
    std::cout << "Device enumeration checks passed\n";
    return 0;
}
