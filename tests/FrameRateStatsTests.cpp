#include "FrameRateStats.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
    int failures = 0;

    void check(bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    }

    void checkNear(double actual, double expected, const char* description)
    {
        check(std::abs(actual - expected) < 1e-9, description);
    }
}

int main()
{
    using namespace std::chrono;
    using Clock = FrameRateStats::Clock;
    const auto base = Clock::time_point{} + seconds(10);

    FrameRateStats stats;
    check(!stats.hasSample(), "new stats have no rate sample");
    checkNear(stats.captureFps(), 0.0, "initial capture rate is zero");
    checkNear(stats.previewFps(), 0.0, "initial preview rate is zero");

    stats.update(base, 1000, 800);
    check(!stats.hasSample(), "first update only establishes count baseline");
    stats.update(base + milliseconds(250), 1060, 830);
    check(!stats.hasSample(), "waits until 500 ms window completes");
    stats.update(base + milliseconds(500), 1120, 860);
    check(stats.hasSample(), "completed window produces rate sample");
    checkNear(stats.captureFps(), 240.0, "measures 240 FPS capture");
    checkNear(stats.previewFps(), 120.0, "measures 120 FPS preview independently");

    stats.update(base + milliseconds(1250), 1300, 950);
    checkNear(stats.captureFps(), 240.0, "uses actual 750 ms elapsed interval");
    checkNear(stats.previewFps(), 120.0, "preview uses actual interval");
    for (int i = 1; i <= 4; ++i)
    {
        stats.update(base + milliseconds(1250 + i * 100), 1300, 950);
        checkNear(stats.captureFps(), 240.0, "partial idle interval retains last sample");
        checkNear(stats.previewFps(), 120.0, "HUD redraw does not inflate preview rate");
    }
    stats.update(base + milliseconds(1750), 1300, 950);
    check(stats.hasSample(), "idle interval still produces valid sample");
    checkNear(stats.captureFps(), 0.0, "stopped signal produces zero capture FPS");
    checkNear(stats.previewFps(), 0.0, "repeated stale-frame redraws produce zero preview FPS");
    stats.update(base + milliseconds(2250), 1420, 1070);
    checkNear(stats.captureFps(), 240.0, "capture rate recovers when signal resumes");
    checkNear(stats.previewFps(), 240.0, "measures 240 FPS preview");

    stats.update(base + milliseconds(2350), 0, 1070);
    check(!stats.hasSample(), "capture counter reset invalidates prior rate");
    checkNear(stats.captureFps(), 0.0, "capture reset clears rate");
    checkNear(stats.previewFps(), 0.0, "capture reset rebases both streams");
    stats.update(base + milliseconds(2850), 60, 1100);
    checkNear(stats.captureFps(), 120.0, "capture reset starts new interval");
    checkNear(stats.previewFps(), 60.0, "preview baseline follows capture reset");
    stats.update(base + milliseconds(2950), 72, 0);
    check(!stats.hasSample(), "preview counter reset rebases both streams");
    stats.update(base + milliseconds(3450), 132, 60);
    checkNear(stats.captureFps(), 120.0, "capture rate after preview reset");
    checkNear(stats.previewFps(), 120.0, "preview rate after preview reset");

    stats.reset();
    check(!stats.hasSample(), "explicit reset clears sample flag");
    checkNear(stats.captureFps(), 0.0, "explicit reset clears capture rate");
    checkNear(stats.previewFps(), 0.0, "explicit reset clears preview rate");
    stats.update(Clock::time_point{}, 0, 0);
    stats.update(Clock::time_point{} + milliseconds(500), 120, 60);
    checkNear(stats.captureFps(), 240.0, "epoch timestamp is valid after reset");
    checkNear(stats.previewFps(), 120.0, "reset accepts earlier clock baseline");

    FrameRateStats clockStats;
    clockStats.update(base, 0, 0);
    clockStats.update(base, 0, 0);
    check(!clockStats.hasSample(), "equal clock ticks do not divide by zero");
    clockStats.update(base, 1, 1);
    clockStats.update(base + milliseconds(500), 120, 60);
    checkNear(clockStats.captureFps(), 240.0, "equal clock ticks preserve window counts");
    checkNear(clockStats.previewFps(), 120.0, "equal clock ticks preserve preview counts");
    clockStats.update(base + milliseconds(750), 180, 90);
    clockStats.update(base + milliseconds(600), 181, 91);
    check(!clockStats.hasSample(), "clock moving backward within window rebases");
    checkNear(clockStats.captureFps(), 0.0, "clock rollback clears stale capture FPS");
    checkNear(clockStats.previewFps(), 0.0, "clock rollback clears stale preview FPS");
    clockStats.update(base + milliseconds(1100), 301, 151);
    checkNear(clockStats.captureFps(), 240.0, "clock rollback recovers with new interval");
    checkNear(clockStats.previewFps(), 120.0, "preview recovers after clock rollback");

    FrameRateStats wrapStats;
    constexpr auto largest = std::numeric_limits<std::uint64_t>::max();
    wrapStats.update(base, largest - 120, largest - 60);
    wrapStats.update(base + milliseconds(500), largest, largest);
    checkNear(wrapStats.captureFps(), 240.0, "safe delta near maximum counter");
    checkNear(wrapStats.previewFps(), 120.0, "safe preview delta near maximum counter");
    wrapStats.update(base + milliseconds(600), 10, 10);
    check(!wrapStats.hasSample(), "counter wrap rebases without unsigned underflow");
    checkNear(wrapStats.captureFps(), 0.0, "counter wrap clears capture rate");
    checkNear(wrapStats.previewFps(), 0.0, "counter wrap clears preview rate");
    wrapStats.update(base + milliseconds(1100), 130, 70);
    checkNear(wrapStats.captureFps(), 240.0, "rate recovers after counter wrap");
    checkNear(wrapStats.previewFps(), 120.0, "preview recovers after counter wrap");

    FrameRateStats rollbackStats;
    rollbackStats.update(base, 100, 100);
    rollbackStats.update(base + milliseconds(100), 200, 200);
    rollbackStats.update(base + milliseconds(200), 150, 150);
    check(!rollbackStats.hasSample(), "counter rollback above initial baseline rebases");
    rollbackStats.update(base + milliseconds(700), 270, 210);
    checkNear(rollbackStats.captureFps(), 240.0, "compares counts with last observation");
    checkNear(rollbackStats.previewFps(), 120.0, "preview rollback uses latest baseline");

    if (failures != 0)
    {
        std::cerr << failures << " checks failed\n";
        return 1;
    }
    std::cout << "FrameRateStats checks passed\n";
    return 0;
}
