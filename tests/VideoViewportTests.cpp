#include "VideoViewport.hpp"

#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    int failures = 0;

    void expectViewport(std::string_view name, const VideoViewportDimensions& actual,
                        const VideoViewportDimensions& expected)
    {
        if (actual.x != expected.x || actual.y != expected.y ||
            actual.width != expected.width || actual.height != expected.height)
        {
            std::cerr << name << ": expected (" << expected.x << ", " << expected.y
                      << ", " << expected.width << ", " << expected.height << "), got ("
                      << actual.x << ", " << actual.y << ", " << actual.width << ", "
                      << actual.height << ")\n";
            ++failures;
        }
    }
}

int main()
{
    using Mode = VideoAspectMode;
    expectViewport("fit wide source", computeVideoViewportDimensions(800, 600, 1920, 1080, Mode::Maintain),
                   {0, 75, 800, 450});
    expectViewport("fit tall source", computeVideoViewportDimensions(800, 600, 600, 1200, Mode::Maintain),
                   {250, 0, 300, 600});
    expectViewport("fit upscales", computeVideoViewportDimensions(1920, 1080, 640, 480, Mode::Maintain),
                   {240, 0, 1440, 1080});
    expectViewport("stretch", computeVideoViewportDimensions(800, 600, 1920, 1080, Mode::Stretch),
                   {0, 0, 800, 600});
    expectViewport("fill horizontal crop", computeVideoViewportDimensions(800, 600, 1920, 1080, Mode::Fill),
                   {-133, 0, 1067, 600});
    expectViewport("fill vertical crop", computeVideoViewportDimensions(800, 600, 600, 1200, Mode::Fill),
                   {0, -500, 800, 1600});
    expectViewport("native no upscale", computeVideoViewportDimensions(1920, 1080, 640, 480, Mode::Capture),
                   {640, 300, 640, 480});
    expectViewport("native downscales to fit", computeVideoViewportDimensions(800, 600, 1920, 1080, Mode::Capture),
                   {0, 75, 800, 450});
    expectViewport("custom 50 percent", computeVideoViewportDimensions(800, 600, 640, 480, Mode::Custom, 50),
                   {240, 180, 320, 240});
    expectViewport("custom 100 percent", computeVideoViewportDimensions(800, 600, 640, 480, Mode::Custom, 100),
                   {80, 60, 640, 480});
    expectViewport("custom 200 percent crops", computeVideoViewportDimensions(800, 600, 640, 480, Mode::Custom, 200),
                   {-240, -180, 1280, 960});
    expectViewport("odd source rounding", computeVideoViewportDimensions(800, 600, 641, 481, Mode::Custom, 50),
                   {239, 179, 321, 241});
    expectViewport("odd fit centering", computeVideoViewportDimensions(801, 601, 1920, 1080, Mode::Maintain),
                   {0, 75, 801, 451});
    expectViewport("minimum one pixel", computeVideoViewportDimensions(1, 1, 1920, 1080, Mode::Maintain),
                   {0, 0, 1, 1});
    expectViewport("clamp custom minimum", computeVideoViewportDimensions(800, 600, 640, 480, Mode::Custom, 0),
                   {320, 240, 160, 120});
    expectViewport("clamp custom maximum", computeVideoViewportDimensions(800, 600, 640, 480, Mode::Custom,
                   std::numeric_limits<unsigned int>::max()), {-240, -180, 1280, 960});
    expectViewport("zero client width", computeVideoViewportDimensions(0, 600, 640, 480, Mode::Stretch), {});
    expectViewport("zero client height", computeVideoViewportDimensions(800, 0, 640, 480, Mode::Maintain), {});
    expectViewport("zero source width", computeVideoViewportDimensions(800, 600, 0, 480, Mode::Fill), {});
    expectViewport("zero source height", computeVideoViewportDimensions(800, 600, 640, 0, Mode::Custom), {});
    expectViewport("negative source dimension", computeVideoViewportDimensions(800, 600, -640, 480, Mode::Capture), {});
    expectViewport("unsupported client dimension", computeVideoViewportDimensions(20000, 600, 640, 480, Mode::Stretch), {});
    expectViewport("unsupported source dimension", computeVideoViewportDimensions(800, 600,
                   std::numeric_limits<int>::max(), 480, Mode::Fill), {});
    expectViewport("invalid aspect mode", computeVideoViewportDimensions(800, 600, 640, 480,
                   static_cast<Mode>(999)), {});
    expectViewport("pathological fill is bounded", computeVideoViewportDimensions(16384, 1, 1, 16384, Mode::Fill),
                   {8191, -16383, 2, 32767});
    expectViewport("max custom is bounded", computeVideoViewportDimensions(16384, 16384, 16384, 16384,
                   Mode::Custom, 200), {-8191, -8191, 32767, 32767});

    if (failures != 0)
    {
        std::cerr << failures << " viewport check(s) failed\n";
        return 1;
    }
    std::cout << "All viewport checks passed\n";
    return 0;
}
