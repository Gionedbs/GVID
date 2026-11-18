#pragma once
#include <memory>
#include <string>
#include "play/playback_channel.h"
namespace vs {
// Legge i file XDCAM prodotti da MxfWriter (GOP 12). Un'istanza per canale di uscita.
class MxfReader final : public IFrameProvider {
public:
    MxfReader();
    ~MxfReader() override;
    MxfReader(const MxfReader&) = delete;
    MxfReader& operator=(const MxfReader&) = delete;

    bool open(const std::string& path);
    std::string lastError() const;

    int64_t frameCount() const override;
    int gopLength() const override;
    int width() const override;
    int height() const override;
    bool decodeGop(int64_t gopStart, const std::function<void(std::shared_ptr<DecodedFrame>)>& deliver) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace vs
