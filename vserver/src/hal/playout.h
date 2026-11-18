#pragma once
#include <cstdint>
#include <vector>
namespace vs {
struct OutputFrame {
    int64_t fieldA = 0;       // campo sorgente nelle righe pari
    int64_t fieldB = 0;       // campo sorgente nelle righe dispari
    bool repeated = false;    // ripetizione per dati non pronti
    std::vector<uint8_t> uyvy;
    std::vector<std::vector<int32_t>> audio;  // vuoto = muto
};

class IPlayoutSink {
public:
    virtual ~IPlayoutSink() = default;
    virtual void output(const OutputFrame& frame) = 0;
};
}  // namespace vs
