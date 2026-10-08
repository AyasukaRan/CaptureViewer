#include "FrameRateOptions.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

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

    bool contains(const std::vector<std::uint32_t>& rates, std::uint32_t rate)
    {
        return std::find(rates.begin(), rates.end(), rate) != rates.end();
    }
}

int main()
{
    using namespace FrameRateOptions;
    constexpr std::int64_t fps30 = 333333;
    constexpr std::int64_t fps60 = 166667;
    constexpr std::int64_t fps120 = 83333;
    constexpr std::int64_t fps240 = 41667;

    check(intervalForRate(6000) == fps60, "rounds 60 FPS interval");
    check(intervalForRate(12000) == fps120, "rounds 120 FPS interval");
    check(intervalForRate(24000) == fps240, "rounds 240 FPS interval");
    check(intervalForRate(5994) == 166834, "converts fractional 59.94 FPS");
    check(rateForInterval(166833) == 5994, "recovers fractional 59.94 FPS");
    check(rateForInterval(fps60) == 6000, "recovers rounded 60 FPS interval");
    check(rateForInterval(fps120) == 12000, "recovers rounded 120 FPS interval");
    check(rateForInterval(fps240) == 24000, "recovers rounded 240 FPS interval");

    const auto variable = enumerate(fps60, fps240, fps30);
    for (const std::uint32_t rate : {3000u, 5994u, 6000u, 11988u, 12000u, 14400u, 23976u, 24000u})
    {
        check(contains(variable, rate), "variable capability includes common high/fractional rate");
    }
    check(!contains(variable, 2997), "30 FPS lower bound excludes 29.97 FPS");
    check(!contains(variable, 36000), "240 FPS upper bound excludes 360 FPS");
    check(std::is_sorted(variable.begin(), variable.end()), "options are sorted");
    check(std::adjacent_find(variable.begin(), variable.end()) == variable.end(), "options contain no duplicates");
    check(allowsRate(fps60, fps240, fps30, 13725), "range allows intermediate manual rate");
    check(!contains(variable, 13725), "finite menu need not list every manual rate");
    check(!allowsRate(fps60, fps240, fps30, 2999), "rejects just below minimum FPS");
    check(!allowsRate(fps60, fps240, fps30, 24001), "rejects above maximum beyond tick tolerance");

    const auto fixed = enumerate(fps60, fps60, fps60);
    check(fixed == std::vector<std::uint32_t>{6000}, "fixed 60 FPS capability remains 60 only");
    check(!allowsRate(fps60, fps60, fps60, 12000), "fixed 60 FPS rejects 120 FPS");
    check(enumerate(166833, 166833, 166833) == std::vector<std::uint32_t>{5994},
          "fixed fractional capability remains 59.94 FPS only");
    check(allowsRate(417083, 0, 0, 2398), "default identity survives 23.976 FPS rounding");

    for (const auto& bounds : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {0, 0}, {0, fps30}, {fps240, 0}, {-1, fps30}, {fps30, fps240},
             {1, 9999}, {10000001, std::numeric_limits<std::int64_t>::max()},
             {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()},
             {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::max()}})
    {
        check(enumerate(fps60, bounds.first, bounds.second) == std::vector<std::uint32_t>{6000},
              "invalid bounds fall back to default only");
        check(allowsRate(fps60, bounds.first, bounds.second, 6000), "invalid bounds still permit default");
        check(!allowsRate(fps60, bounds.first, bounds.second, 12000), "invalid bounds reject unsupported rate");
    }

    constexpr std::int64_t fps90 = 111111;
    check(allowsRate(fps90, fps120 + 1, fps60 - 1, 12000), "minimum interval allows one-tick rounding");
    check(allowsRate(fps90, fps120 + 1, fps60 - 1, 6000), "maximum interval allows one-tick rounding");
    check(!allowsRate(fps90, fps120 + 2, fps60, 12000), "minimum interval rejects two-tick mismatch");
    check(!allowsRate(fps90, fps120, fps60 - 2, 6000), "maximum interval rejects two-tick mismatch");

    const auto nonstandardEndpoints = enumerate(fps60, intervalForRate(13725), intervalForRate(4250));
    check(contains(nonstandardEndpoints, 13725), "includes nonstandard maximum endpoint");
    check(contains(nonstandardEndpoints, 4250), "includes nonstandard minimum endpoint");
    check(contains(enumerate(0, fps240, fps30), 12000), "valid bounds work without default interval");

    const auto unboundedDuration = enumerate(fps60, fps240, std::numeric_limits<std::int64_t>::max());
    check(contains(unboundedDuration, 100), "MAXLONGLONG duration supports 1 FPS lower limit");
    check(contains(unboundedDuration, 12000), "MAXLONGLONG duration retains 120 FPS");
    check(contains(unboundedDuration, 24000), "MAXLONGLONG duration retains 240 FPS");
    check(!contains(unboundedDuration, 36000), "unbounded duration still respects fastest interval");
    check(allowsRate(fps60, fps240, std::numeric_limits<std::int64_t>::max(), 100),
          "unbounded duration allows manual 1 FPS without overflow");
    check(allowsRate(fps60, fps240, std::numeric_limits<std::int64_t>::max(), 13725),
          "unbounded duration allows intermediate manual rate");
    const auto veryFastMinimum = enumerate(fps60, 1, fps30);
    check(contains(veryFastMinimum, 100000), "too-fast minimum interval clamps to 1000 FPS");
    check(contains(veryFastMinimum, 3000), "clamping preserves slower endpoint");
    check(!contains(veryFastMinimum, 2997), "clamping still enforces maximum interval");
    check(contains(enumerate(fps60, fps240, 10000001), 100),
          "duration beyond one second clips to supported 1 FPS");
    const auto widestRawRange = enumerate(fps60, 1, std::numeric_limits<std::int64_t>::max());
    check(contains(widestRawRange, 100) && contains(widestRawRange, 100000),
          "widest positive raw range intersects both supported limits");
    check(widestRawRange.size() <= 36, "unbounded capability enumeration stays finite");

    check(intervalForRate(100) == 10000000, "supports minimum 1 FPS");
    check(intervalForRate(100000) == 10000, "supports maximum 1000 FPS");
    check(rateForInterval(10000000) == 100, "minimum FPS round trip");
    check(rateForInterval(10000) == 100000, "maximum FPS round trip");
    for (const std::uint32_t rate : {0u, 1u, 99u, 100001u, std::numeric_limits<std::uint32_t>::max()})
    {
        check(intervalForRate(rate) == 0, "invalid extreme rate returns zero");
        check(!allowsRate(fps60, 10000, 10000000, rate), "invalid extreme request is rejected");
    }
    for (const std::int64_t interval : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
             std::int64_t{0}, std::int64_t{9999}, std::int64_t{10000001},
             std::numeric_limits<std::int64_t>::max()})
    {
        check(rateForInterval(interval) == 0, "invalid extreme interval returns zero");
        check(enumerate(interval, 0, 0).empty(), "invalid default and bounds produce no rates");
    }
    const auto fullRange = enumerate(fps60, 10000, 10000000);
    check(contains(fullRange, 100) && contains(fullRange, 100000), "full range includes meaningful limits");
    check(fullRange.size() <= 36, "enumeration stays finite even for widest supported range");

    if (failures != 0)
    {
        std::cerr << failures << " checks failed\n";
        return 1;
    }
    std::cout << "FrameRateOptions checks passed\n";
    return 0;
}
