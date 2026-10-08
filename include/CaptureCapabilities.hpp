#pragma once

#include "Settings.hpp"

#include <Windows.h>
#include <dshow.h>
#include <cstring>
#include <cstddef>
#include <cstdint>

namespace CaptureCapabilities
{
    inline bool supportedSubtype(const GUID& subtype)
    {
        return InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB24) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_RGB32) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_ARGB32) ||
               InlineIsEqualGUID(subtype, MEDIASUBTYPE_NV12);
    }

    inline VideoFormatPreference formatPreference(const GUID& subtype)
    {
        return InlineIsEqualGUID(subtype, MEDIASUBTYPE_NV12)
            ? VideoFormatPreference::NV12 : VideoFormatPreference::XRGB;
    }

    inline VIDEOINFOHEADER* videoInfo(AM_MEDIA_TYPE* type)
    {
        // The legacy Sample Grabber cannot connect FORMAT_VideoInfo2.
        if (!type || type->majortype != MEDIATYPE_Video ||
            type->formattype != FORMAT_VideoInfo || !type->pbFormat ||
            type->cbFormat < sizeof(VIDEOINFOHEADER) || !supportedSubtype(type->subtype))
        {
            return nullptr;
        }
        auto* info = reinterpret_cast<VIDEOINFOHEADER*>(type->pbFormat);
        return info->bmiHeader.biWidth > 0 && info->bmiHeader.biHeight > 0 ? info : nullptr;
    }

    inline VIDEO_STREAM_CONFIG_CAPS streamCaps(const std::uint8_t* data, std::size_t size)
    {
        VIDEO_STREAM_CONFIG_CAPS caps{};
        if (data && size >= sizeof(caps))
        {
            std::memcpy(&caps, data, sizeof(caps));
        }
        return caps;
    }
}
