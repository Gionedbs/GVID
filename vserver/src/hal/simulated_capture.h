#pragma once
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <thread>
#include "hal/capture.h"
namespace vs {
// Codice binario a blocchi da 64x64 in alto a sinistra: sopravvive alla compressione.
inline constexpr int kFrameCodeBits = 16;

inline void burnFrameCode(std::vector<uint8_t>& uyvy, std::size_t stride, int64_t code) {
    for (std::size_t y = 0; y < 64; ++y) {
        uint8_t* line = uyvy.data() + y * stride;
        for (int bit = 0; bit < kFrameCodeBits; ++bit) {
            const uint8_t luma = ((code >> bit) & 1) ? 235 : 16;
            uint8_t* p = line + static_cast<std::size_t>(bit) * 128;
            for (int x = 0; x < 32; ++x) {
                p[0] = 128; p[1] = luma; p[2] = 128; p[3] = luma;
                p += 4;
            }
        }
    }
}

inline int64_t readFrameCode(const std::vector<uint8_t>& uyvy, std::size_t stride, std::size_t line) {
    int64_t code = 0;
    const uint8_t* p = uyvy.data() + line * stride;
    for (int bit = 0; bit < kFrameCodeBits; ++bit)
        if (p[static_cast<std::size_t>(bit) * 128 + 65] > 128) code |= int64_t{1} << bit;
    return code;
}

inline int32_t simulatedAudioSample(int64_t frame, int sample, std::size_t track) {
    const double step = 2.0 * 3.14159265358979 * 220.0 * static_cast<double>(track + 1) / 48000.0;
    const double n = static_cast<double>(frame * kAudioSamplesPerFrame + sample);
    return static_cast<int32_t>(std::sin(n * step) * 0.25 * 2147483392.0);
}

// Sorgente di prova: immagine con dettaglio e movimento, audio sinusoidale.
class SimulatedCapture final : public ICaptureSource {
public:
    struct Options {
        int channel = 0;
        int64_t frames = 250;
        int width = 1920;
        int height = 1080;
        int audioTracks = 4;
        double paceFactor = 0.0;  // 1 = tempo reale, 2 = doppio; 0 = nessuna attesa
        int64_t startTimecode = 900000;  // 10:00:00:00
        int64_t timecodeBreakAt = -1;    // simula un salto di timecode
        bool timecodeMissing = false;
        bool burnFrameCode = false;      // imprime il numero di fotogramma nell'immagine
        int64_t frameCodeBase = 0;       // si somma al numero impresso, per distinguere le clip
    };

    explicit SimulatedCapture(Options options) : o_(options) {
        const auto w = static_cast<std::size_t>(o_.width);
        pattern_.resize(w * 4);
        uint32_t noise = 0x12345u + static_cast<uint32_t>(o_.channel) * 977u;
        for (std::size_t i = 0; i < pattern_.size(); i += 2) {
            noise = noise * 1664525u + 1013904223u;
            const auto luma = static_cast<int>(16 + (i * 219) / pattern_.size()) + static_cast<int>(noise >> 28) - 8;
            pattern_[i] = static_cast<uint8_t>(128 + (i % 64) - 32);
            pattern_[i + 1] = static_cast<uint8_t>(luma < 16 ? 16 : luma > 235 ? 235 : luma);
        }
    }

    bool capture(CapturedFrame& f) override {
        if (next_ >= o_.frames) return false;
        if (o_.paceFactor > 0.0) {
            if (next_ == 0) start_ = std::chrono::steady_clock::now();
            const auto wait = std::chrono::duration<double, std::milli>(40.0 * static_cast<double>(next_) / o_.paceFactor);
            std::this_thread::sleep_until(start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(wait));
        }
        const auto w = static_cast<std::size_t>(o_.width), h = static_cast<std::size_t>(o_.height);
        const std::size_t stride = w * 2;
        f.uyvy.resize(stride * h);
        for (std::size_t y = 0; y < h; ++y) {
            // Il secondo campo è spostato di mezzo fotogramma, come nel video interlacciato.
            const auto motion = static_cast<std::size_t>(next_) * 16 + (y & 1) * 8;
            const std::size_t offset = ((motion + y * 6) % w) * 2;
            std::memcpy(f.uyvy.data() + y * stride, pattern_.data() + (offset & ~std::size_t{3}), stride);
        }
        if (o_.burnFrameCode) burnFrameCode(f.uyvy, stride, o_.frameCodeBase + next_);
        f.audio.resize(static_cast<std::size_t>(o_.audioTracks));
        for (std::size_t t = 0; t < f.audio.size(); ++t) {
            auto& track = f.audio[t];
            track.resize(kAudioSamplesPerFrame);
            for (int s = 0; s < kAudioSamplesPerFrame; ++s) track[static_cast<std::size_t>(s)] = simulatedAudioSample(next_, s, t);
        }
        f.sequence = next_;
        const int64_t extra = (o_.timecodeBreakAt >= 0 && next_ >= o_.timecodeBreakAt) ? 100 : 0;
        f.timecode = o_.timecodeMissing ? std::nullopt : std::optional<int64_t>(o_.startTimecode + next_ + extra);
        ++next_;
        return true;
    }

private:
    Options o_;
    int64_t next_ = 0;
    std::vector<uint8_t> pattern_;
    std::chrono::steady_clock::time_point start_;
};
}  // namespace vs
