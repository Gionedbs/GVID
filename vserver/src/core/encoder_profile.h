#pragma once
#include <string>
#include <utility>
#include <vector>
namespace vs {
// XDCAM HD422 50 Mb/s, 1080i50, in MXF OP1a. Il GOP deve restare aperto:
// con +cgop questo encoder produce GOP da 10 fotogrammi invece che da 12.
inline std::vector<std::pair<std::string, std::string>> xdcam50EncoderOptions() {
    return {{"profile", "0"}, {"level", "2"},
            {"b", "50M"}, {"minrate", "50M"}, {"maxrate", "50M"},
            {"bufsize", "17825792"}, {"rc_init_occupancy", "17825792"},
            {"g", "12"}, {"bf", "2"}, {"sc_threshold", "1000000000"},
            {"flags", "+ildct+ilme"}, {"alternate_scan", "1"},
            {"qmin", "1"}, {"qmax", "28"}, {"intra_dc_precision", "2"},
            {"non_linear_quant", "1"}, {"intra_vlc", "1"},
            {"color_primaries", "bt709"}, {"color_trc", "bt709"}, {"colorspace", "bt709"}};
}
}  // namespace vs
