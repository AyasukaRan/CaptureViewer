#pragma once

#include "Settings.hpp"

#include <algorithm>
#include <cmath>

struct VideoViewportDimensions
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Dimensions and offsets are physical pixels. Custom scaling is relative to the
// source resolution, so 100% maps one source pixel to one output pixel.
inline VideoViewportDimensions computeVideoViewportDimensions(
    int clientWidth, int clientHeight, int sourceWidth, int sourceHeight,
    VideoAspectMode mode, unsigned int scalePercent = 100) noexcept
{
    // D3D12 2D textures are limited to 16,384 pixels per dimension. Reject
    // unsupported dimensions rather than producing overflowing viewports.
    constexpr int maxTextureDimension = 16384;
    if (clientWidth <= 0 || clientHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0 ||
        clientWidth > maxTextureDimension || clientHeight > maxTextureDimension ||
        sourceWidth > maxTextureDimension || sourceHeight > maxTextureDimension)
    {
        return {};
    }

    if (mode == VideoAspectMode::Stretch)
    {
        return {0, 0, clientWidth, clientHeight};
    }

    const double widthScale = static_cast<double>(clientWidth) / sourceWidth;
    const double heightScale = static_cast<double>(clientHeight) / sourceHeight;
    double scale = 1.0;
    bool keepInsideClient = false;
    switch (mode)
    {
    case VideoAspectMode::Maintain:
        scale = std::min(widthScale, heightScale);
        keepInsideClient = true;
        break;
    case VideoAspectMode::Capture:
        scale = std::min({1.0, widthScale, heightScale});
        keepInsideClient = true;
        break;
    case VideoAspectMode::Fill:
        scale = std::max(widthScale, heightScale);
        break;
    case VideoAspectMode::Custom:
        scale = static_cast<double>(std::clamp(scalePercent, 25u, 200u)) / 100.0;
        break;
    default:
        return {};
    }

    // An extremely narrow source in Fill mode could otherwise create a huge
    // viewport. Reduce both axes together to preserve aspect ratio and keep
    // centered viewport coordinates inside D3D12's [-32768, 32767] bounds.
    // This safety cap can prevent full coverage for pathological aspect ratios.
    constexpr int maxViewportDimension = 32767;
    scale = std::min(scale, static_cast<double>(maxViewportDimension) /
        std::max(sourceWidth, sourceHeight));
    int width = std::clamp(static_cast<int>(std::lround(sourceWidth * scale)), 1, maxViewportDimension);
    int height = std::clamp(static_cast<int>(std::lround(sourceHeight * scale)), 1, maxViewportDimension);
    if (keepInsideClient)
    {
        width = std::min(width, clientWidth);
        height = std::min(height, clientHeight);
    }

    return {(clientWidth - width) / 2, (clientHeight - height) / 2, width, height};
}
