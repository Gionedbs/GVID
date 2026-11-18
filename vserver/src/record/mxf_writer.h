#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "record/recorder.h"
namespace vs {
// Scrive un file XDCAM HD422 50 Mb/s in MXF OP1a con una traccia mono per canale audio.
class MxfWriter final : public IFrameSink {
public:
    struct Options {
        std::string path;
        int width = 1920;
        int height = 1080;
        int audioTracks = 4;
        int64_t startTimecode = 0;
        int encoderThreads = 0;  // 0 = automatico
    };

    MxfWriter();
    ~MxfWriter() override;
    MxfWriter(const MxfWriter&) = delete;
    MxfWriter& operator=(const MxfWriter&) = delete;

    bool open(const Options& options);
    bool write(const CapturedFrame& frame) override;
    bool close() override;
    std::string lastError() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace vs
