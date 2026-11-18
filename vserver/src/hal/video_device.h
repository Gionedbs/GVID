#pragma once
#include <cstdint>
#include <optional>
namespace vs {
enum class ChannelDirection { Input, Output };

// Confine verso la scheda: il resto del sistema non dipende dall'SDK del produttore.
class IVideoDevice {
public:
    virtual ~IVideoDevice() = default;
    virtual int channelCount() const = 0;
    virtual bool setDirection(int channel, ChannelDirection direction) = 0;
    virtual bool referenceLocked() const = 0;
    virtual std::optional<int64_t> readLtc() = 0;
};
}  // namespace vs
