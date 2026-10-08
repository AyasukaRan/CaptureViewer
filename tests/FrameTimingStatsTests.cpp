#include "FrameTimingStats.hpp"

#include <chrono>
#include <cmath>
#include <iostream>

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
    using Clock = FrameTimingStats::Clock;
    const auto base = Clock::time_point{} + seconds(10);

    FrameTimingStats stats;
    check(!stats.hasSamples(), "initially has no samples");
    checkNear(stats.latestMs(), 0.0, "empty latest is zero");
    checkNear(stats.averageMs(), 0.0, "empty average is zero");
    checkNear(stats.maximumMs(), 0.0, "empty maximum is zero");
    check(stats.lastReceivedAt() == Clock::time_point{}, "empty timestamp is default");

    check(!stats.record(Clock::time_point{}, base), "rejects default arrival timestamp");
    check(!stats.record(base, base - microseconds(1)), "rejects negative elapsed time");
    check(!stats.hasSamples(), "invalid first samples leave stats empty");

    check(stats.record(base, base + microseconds(1250)), "accepts valid fractional duration");
    check(stats.hasSamples(), "has sample after recording");
    checkNear(stats.latestMs(), 1.25, "duration converted to milliseconds");
    checkNear(stats.averageMs(), 1.25, "one-sample average");
    checkNear(stats.maximumMs(), 1.25, "one-sample maximum");
    check(stats.lastReceivedAt() == base, "remembers arrival timestamp");

    const auto second = base + seconds(1);
    check(stats.record(second, second + microseconds(3750)), "accepts newer sample");
    checkNear(stats.latestMs(), 3.75, "latest follows newest sample");
    checkNear(stats.averageMs(), 2.5, "averages distinct frame samples");
    checkNear(stats.maximumMs(), 3.75, "finds maximum sample");

    check(!stats.record(second, second + seconds(5)), "rejects repeated frame arrival");
    check(!stats.record(base, base + seconds(5)), "rejects older frame arrival");
    check(!stats.record(second + seconds(1), second), "rejects negative later duration");
    check(!stats.record(Clock::time_point{}, second), "rejects default arrival after valid samples");
    checkNear(stats.latestMs(), 3.75, "rejections preserve latest");
    checkNear(stats.averageMs(), 2.5, "rejections preserve average");
    checkNear(stats.maximumMs(), 3.75, "rejections preserve maximum");
    check(stats.lastReceivedAt() == second, "rejections preserve last arrival");

    const auto third = second + seconds(1);
    check(stats.record(third, third), "accepts zero elapsed time");
    checkNear(stats.latestMs(), 0.0, "zero duration recorded");
    checkNear(stats.averageMs(), 5.0 / 3.0, "zero participates in average");

    FrameTimingStats ring;
    for (int i = 1; i <= 120; ++i)
    {
        const auto received = base + seconds(i);
        const auto elapsed = milliseconds(i == 1 ? 1000 : i);
        check(ring.record(received, received + elapsed), "accepts samples filling ring");
    }
    checkNear(ring.averageMs(), 8259.0 / 120.0, "full ring average");
    checkNear(ring.maximumMs(), 1000.0, "full ring includes earliest maximum");

    const auto overflowArrival = base + seconds(121);
    check(ring.record(overflowArrival, overflowArrival + milliseconds(3)), "accepts sample beyond capacity");
    checkNear(ring.latestMs(), 3.0, "ring latest after eviction");
    checkNear(ring.averageMs(), 7262.0 / 120.0, "ring excludes oldest sample from average");
    checkNear(ring.maximumMs(), 120.0, "ring excludes evicted maximum");

    for (int i = 122; i <= 241; ++i)
    {
        const auto received = base + seconds(i);
        check(ring.record(received, received + milliseconds(2)), "accepts repeated ring wrap");
    }
    checkNear(ring.latestMs(), 2.0, "latest after complete replacement");
    checkNear(ring.averageMs(), 2.0, "average after complete replacement");
    checkNear(ring.maximumMs(), 2.0, "maximum after complete replacement");

    ring.reset();
    check(!ring.hasSamples(), "reset removes samples");
    checkNear(ring.latestMs(), 0.0, "reset clears latest");
    checkNear(ring.averageMs(), 0.0, "reset clears average");
    checkNear(ring.maximumMs(), 0.0, "reset clears maximum");
    check(ring.lastReceivedAt() == Clock::time_point{}, "reset clears timestamp");
    check(ring.record(base, base + milliseconds(7)), "reset accepts earlier stream timestamp");
    checkNear(ring.averageMs(), 7.0, "new stream has no previous samples");
    checkNear(ring.maximumMs(), 7.0, "new stream has no previous maximum");

    if (failures != 0)
    {
        std::cerr << failures << " checks failed\n";
        return 1;
    }
    std::cout << "FrameTimingStats checks passed\n";
    return 0;
}
