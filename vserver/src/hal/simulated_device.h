#pragma once
#include <cstddef>
#include <vector>
#include "hal/video_device.h"
namespace vs {
// Scheda simulata, per sviluppare e provare senza hardware.
class SimulatedDevice final : public IVideoDevice {
public:
    explicit SimulatedDevice(int channels = 8) : directions_(static_cast<std::size_t>(channels), ChannelDirection::Input) {}
    int channelCount() const override { return static_cast<int>(directions_.size()); }
    bool setDirection(int channel, ChannelDirection direction) override {
        if (channel < 0 || channel >= channelCount()) return false;
        directions_[static_cast<std::size_t>(channel)] = direction;
        return true;
    }
    ChannelDirection direction(int channel) const { return directions_.at(static_cast<std::size_t>(channel)); }
    bool referenceLocked() const override { return locked_; }
    std::optional<int64_t> readLtc() override { return ltc_; }
    void simulateReference(bool locked) { locked_ = locked; }
    void simulateLtc(std::optional<int64_t> frames) { ltc_ = frames; }

private:
    std::vector<ChannelDirection> directions_;
    bool locked_ = false;
    std::optional<int64_t> ltc_;
};
}  // namespace vs
