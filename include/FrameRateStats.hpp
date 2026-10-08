#pragma once

#include <chrono>
#include <cstdint>

// Rates of received frames and newly presented frames over actual elapsed time.
// Call on the render thread; repeated HUD redraws must not increase either count.
class FrameRateStats {
public:
    using Clock = std::chrono::steady_clock;

    void update(Clock::time_point now, std::uint64_t receivedCount,
                std::uint64_t presentedCount)
    {
        if (!initialized_ || now < lastUpdate_ ||
            receivedCount < lastReceivedCount_ || presentedCount < lastPresentedCount_)
        {
            rebase(now, receivedCount, presentedCount);
            return;
        }

        lastUpdate_ = now;
        lastReceivedCount_ = receivedCount;
        lastPresentedCount_ = presentedCount;
        const auto elapsed = now - windowStartedAt_;
        if (elapsed < std::chrono::milliseconds(500))
        {
            return;
        }

        const double seconds = std::chrono::duration<double>(elapsed).count();
        captureFps_ = static_cast<double>(receivedCount - windowReceivedCount_) / seconds;
        previewFps_ = static_cast<double>(presentedCount - windowPresentedCount_) / seconds;
        hasSample_ = true;
        windowStartedAt_ = now;
        windowReceivedCount_ = receivedCount;
        windowPresentedCount_ = presentedCount;
    }

    void reset()
    {
        *this = FrameRateStats{};
    }

    bool hasSample() const { return hasSample_; }
    double captureFps() const { return captureFps_; }
    double previewFps() const { return previewFps_; }

private:
    void rebase(Clock::time_point now, std::uint64_t receivedCount,
                std::uint64_t presentedCount)
    {
        initialized_ = true;
        hasSample_ = false;
        captureFps_ = 0.0;
        previewFps_ = 0.0;
        windowStartedAt_ = now;
        lastUpdate_ = now;
        windowReceivedCount_ = lastReceivedCount_ = receivedCount;
        windowPresentedCount_ = lastPresentedCount_ = presentedCount;
    }

    bool initialized_ = false;
    bool hasSample_ = false;
    Clock::time_point windowStartedAt_{};
    Clock::time_point lastUpdate_{};
    std::uint64_t windowReceivedCount_ = 0;
    std::uint64_t windowPresentedCount_ = 0;
    std::uint64_t lastReceivedCount_ = 0;
    std::uint64_t lastPresentedCount_ = 0;
    double captureFps_ = 0.0;
    double previewFps_ = 0.0;
};
