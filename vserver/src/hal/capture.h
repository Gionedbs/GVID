#pragma once
#include <cstdint>
#include <optional>
#include <vector>
namespace vs {
// A 1080i50 ogni fotogramma ha esattamente 1920 campioni audio per traccia.
inline constexpr int kAudioSamplesPerFrame = 1920;

struct CapturedFrame {
    int64_t sequence = 0;
    std::optional<int64_t> timecode;     // in fotogrammi, assente se manca il segnale
    std::vector<uint8_t> uyvy;           // 2 byte per pixel, campi intercalati
    std::vector<std::vector<int32_t>> audio;  // per traccia, allineato a sinistra su 32 bit
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    // Riempie il fotogramma e attende il successivo. False a fine flusso.
    virtual bool capture(CapturedFrame& frame) = 0;
};
}  // namespace vs
