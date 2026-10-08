#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace FrameRateOptions {
    // DirectShow frame intervals are 100 ns; rates here are FPS multiplied by 100.
    inline constexpr std::uint32_t minimumRate100 = 100;
    inline constexpr std::uint32_t maximumRate100 = 100000;
    inline constexpr std::int64_t rateIntervalProduct = 1000000000;
    inline constexpr std::int64_t minimumInterval = 10000;
    inline constexpr std::int64_t maximumInterval = 10000000;

    inline std::int64_t intervalForRate(std::uint32_t rate100)
    {
        if (rate100 < minimumRate100 || rate100 > maximumRate100)
        {
            return 0;
        }
        return (rateIntervalProduct + rate100 / 2) / rate100;
    }

    inline std::uint32_t rateForInterval(std::int64_t interval)
    {
        if (interval < minimumInterval || interval > maximumInterval)
        {
            return 0;
        }
        return static_cast<std::uint32_t>((rateIntervalProduct + interval / 2) / interval);
    }

    inline bool validRange(std::int64_t minInterval, std::int64_t maxInterval)
    {
        // DirectShow allows MAXLONGLONG as an unbounded maximum duration.
        // Require ordered positive bounds and a nonempty supported intersection.
        return minInterval > 0 && minInterval <= maxInterval &&
               minInterval <= maximumInterval && maxInterval >= minimumInterval;
    }

    inline bool allowsRate(std::int64_t defaultInterval, std::int64_t minInterval,
                           std::int64_t maxInterval, std::uint32_t rate100)
    {
        const std::int64_t requestedInterval = intervalForRate(rate100);
        if (requestedInterval == 0)
        {
            return false;
        }
        // The advertised default is usable even when a driver omits its bounds.
        // Preserve its identity despite precision lost by rounding to FPS * 100.
        if (rateForInterval(defaultInterval) == rate100)
        {
            return true;
        }
        if (!validRange(minInterval, maxInterval))
        {
            return false;
        }
        // Clamp before adding/subtracting tolerance, including MAXLONGLONG caps.
        const auto supportedMin = std::max(minInterval, minimumInterval);
        const auto supportedMax = std::min(maxInterval, maximumInterval);
        return requestedInterval >= supportedMin - 1 && requestedInterval <= supportedMax + 1;
    }

    inline std::vector<std::uint32_t> enumerate(std::int64_t defaultInterval,
                                               std::int64_t minInterval,
                                               std::int64_t maxInterval)
    {
        std::vector<std::uint32_t> rates;
        const std::uint32_t defaultRate = rateForInterval(defaultInterval);
        if (defaultRate != 0)
        {
            rates.push_back(defaultRate);
        }
        if (!validRange(minInterval, maxInterval))
        {
            return rates;
        }

        // A finite menu of common rates; allowsRate also accepts manual values.
        constexpr std::array<std::uint32_t, 33> commonRates{
            100, 500, 1000, 1200, 1500, 2000, 2398, 2400, 2500,
            2997, 3000, 4800, 5000, 5994, 6000, 7500, 9000, 10000,
            11988, 12000, 14400, 16500, 20000, 23976, 24000, 30000,
            36000, 48000, 50000, 60000, 72000, 96000, 100000
        };
        const auto addIfAllowed = [&](std::uint32_t rate) {
            if (allowsRate(defaultInterval, minInterval, maxInterval, rate))
            {
                rates.push_back(rate);
            }
        };
        for (const std::uint32_t rate : commonRates)
        {
            addIfAllowed(rate);
        }
        addIfAllowed(rateForInterval(std::max(minInterval, minimumInterval)));
        addIfAllowed(rateForInterval(std::min(maxInterval, maximumInterval)));
        std::sort(rates.begin(), rates.end());
        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
        return rates;
    }
}
