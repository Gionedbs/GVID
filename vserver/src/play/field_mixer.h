#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
namespace vs {
// Scrive un campo sorgente nelle righe di una data parità. Se la parità coincide
// copia le righe; altrimenti le ricava dalla media delle due righe vicine del campo.
inline void composeField(std::vector<uint8_t>& out, const std::vector<uint8_t>& src, int srcParity, int dstParity,
                         std::size_t stride, std::size_t height) {
    for (std::size_t y = static_cast<std::size_t>(dstParity); y < height; y += 2) {
        uint8_t* line = out.data() + y * stride;
        if (srcParity == dstParity) {
            std::memcpy(line, src.data() + y * stride, stride);
            continue;
        }
        std::size_t above = y, below = y;
        if (y > 0) above = y - 1; else above = y + 1;
        if (y + 1 < height) below = y + 1; else below = y - 1;
        const uint8_t* a = src.data() + above * stride;
        const uint8_t* b = src.data() + below * stride;
        for (std::size_t x = 0; x < stride; ++x) line[x] = static_cast<uint8_t>((a[x] + b[x] + 1) / 2);
    }
}
}  // namespace vs
