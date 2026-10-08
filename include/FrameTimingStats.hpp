#pragma once

#include <array>
#include <chrono>
#include <cstddef>

// Callback arrival to successful Present return, measured on one monotonic clock.
// Owned by the render thread; this does not measure hardware or display latency.
class FrameTimingStats {
public:
    using Clock = std::chrono::steady_clock;

    bool record(Clock::time_point receivedAt, Clock::time_point presentReturnedAt)
    {
        if (receivedAt == Clock::time_point{} || presentReturnedAt < receivedAt ||
            (hasSamples() && receivedAt <= lastReceivedAt_))
        {
            return false;
        }

        const double milliseconds =
            std::chrono::duration<double, std::milli>(presentReturnedAt - receivedAt).count();
        if (sampleCount_ == samples_.size())
        {
            totalMs_ -= samples_[nextSample_];
        }
        else
        {
            ++sampleCount_;
        }
        samples_[nextSample_] = milliseconds;
        nextSample_ = (nextSample_ + 1) % samples_.size();
        totalMs_ += milliseconds;
        latestMs_ = milliseconds;
        lastReceivedAt_ = receivedAt;
        return true;
    }

    void reset()
    {
        samples_.fill(0.0);
        nextSample_ = 0;
        sampleCount_ = 0;
        totalMs_ = 0.0;
        latestMs_ = 0.0;
        lastReceivedAt_ = Clock::time_point{};
    }

    bool hasSamples() const { return sampleCount_ != 0; }
    double latestMs() const { return latestMs_; }
    double averageMs() const { return hasSamples() ? totalMs_ / sampleCount_ : 0.0; }

    double maximumMs() const
    {
        double maximum = 0.0;
        for (std::size_t i = 0; i < sampleCount_; ++i)
        {
            if (samples_[i] > maximum)
            {
                maximum = samples_[i];
            }
        }
        return maximum;
    }

    Clock::time_point lastReceivedAt() const { return lastReceivedAt_; }

private:
    std::array<double, 120> samples_{};
    std::size_t nextSample_ = 0;
    std::size_t sampleCount_ = 0;
    double totalMs_ = 0.0;
    double latestMs_ = 0.0;
    Clock::time_point lastReceivedAt_{};
};
