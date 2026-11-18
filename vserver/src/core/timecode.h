#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
namespace vs {
inline constexpr int64_t kFps = 25;
inline constexpr int64_t kFramesPerDay = 24 * 3600 * kFps;

// Riporta un conteggio qualsiasi, anche negativo, nell'arco delle 24 ore.
inline int64_t wrapFrames(int64_t frames) {
    const int64_t r = frames % kFramesPerDay;
    return r < 0 ? r + kFramesPerDay : r;
}

inline std::string formatTimecode(int64_t frames) {
    const int64_t t = wrapFrames(frames);
    char out[32];
    std::snprintf(out, sizeof out, "%02d:%02d:%02d:%02d", static_cast<int>(t / (3600 * kFps)),
                  static_cast<int>(t / (60 * kFps) % 60), static_cast<int>(t / kFps % 60),
                  static_cast<int>(t % kFps));
    return out;
}

inline std::optional<int64_t> parseTimecode(const std::string& text) {
    if (text.size() != 11 || text[2] != ':' || text[5] != ':' || text[8] != ':') return std::nullopt;
    int parts[4] = {};
    for (int i = 0; i < 4; ++i) {
        const char tens = text[static_cast<std::size_t>(i) * 3];
        const char units = text[static_cast<std::size_t>(i) * 3 + 1];
        if (tens < '0' || tens > '9' || units < '0' || units > '9') return std::nullopt;
        parts[i] = (tens - '0') * 10 + (units - '0');
    }
    const int h = parts[0], m = parts[1], s = parts[2], f = parts[3];
    if (h > 23 || m > 59 || s > 59 || f >= kFps) return std::nullopt;
    return (h * 3600LL + m * 60LL + s) * kFps + f;
}

enum class TimecodeSource { Internal, ExternalLtc };

// Sorgente commutabile a macchina in funzione. Senza LTC non c'è ripiego
// automatico sul generatore interno: l'assenza di segnale viene segnalata.
class TimecodeClock {
public:
    void setSource(TimecodeSource source) { source_ = source; }
    TimecodeSource source() const { return source_; }
    void presetInternal(int64_t frames) { internal_ = wrapFrames(frames); }
    void tick() { internal_ = wrapFrames(internal_ + 1); }
    void onExternalLtc(std::optional<int64_t> frames) { external_ = frames; }
    std::optional<int64_t> current() const {
        if (source_ == TimecodeSource::Internal) return internal_;
        return external_;
    }
    bool noSignal() const { return source_ == TimecodeSource::ExternalLtc && !external_.has_value(); }

private:
    TimecodeSource source_ = TimecodeSource::Internal;
    int64_t internal_ = 0;
    std::optional<int64_t> external_;
};
}  // namespace vs
