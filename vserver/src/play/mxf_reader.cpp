#include "play/mxf_reader.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <vector>

#include "core/timecode.h"
#include "hal/capture.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace vs {
namespace {
const AVRational kFrameBase{1, static_cast<int>(kFps)};
constexpr int kGopLength = 12;

std::string describe(const char* step, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return std::string(step) + ": " + text;
}
}  // namespace

struct MxfReader::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int videoIndex = -1;
    std::vector<int> audioIndex;
    int64_t frames = 0;
    int width = 0, height = 0;
    std::string error;

    bool fail(const char* step, int code) { error = describe(step, code); return false; }

    ~Impl() {
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (decoder) avcodec_free_context(&decoder);
        if (format) avformat_close_input(&format);
    }

    std::shared_ptr<DecodedFrame> convert(int64_t index) const {
        auto out = std::make_shared<DecodedFrame>();
        out->index = index;
        const auto w = static_cast<std::size_t>(width), h = static_cast<std::size_t>(height);
        out->uyvy.resize(w * h * 2);
        for (std::size_t y = 0; y < h; ++y) {
            const int line = static_cast<int>(y);
            const uint8_t* py = frame->data[0] + line * frame->linesize[0];
            const uint8_t* pu = frame->data[1] + line * frame->linesize[1];
            const uint8_t* pv = frame->data[2] + line * frame->linesize[2];
            uint8_t* o = out->uyvy.data() + y * w * 2;
            for (std::size_t x = 0; x < w / 2; ++x) {
                o[0] = pu[x];
                o[1] = py[2 * x];
                o[2] = pv[x];
                o[3] = py[2 * x + 1];
                o += 4;
            }
        }
        return out;
    }
};

MxfReader::MxfReader() : impl_(std::make_unique<Impl>()) {}
MxfReader::~MxfReader() = default;
std::string MxfReader::lastError() const { return impl_->error; }
int64_t MxfReader::frameCount() const { return impl_->frames; }
int MxfReader::gopLength() const { return kGopLength; }
int MxfReader::width() const { return impl_->width; }
int MxfReader::height() const { return impl_->height; }

bool MxfReader::open(const std::string& path) {
    Impl& d = *impl_;
    int r = avformat_open_input(&d.format, path.c_str(), nullptr, nullptr);
    if (r < 0) return d.fail("apertura MXF", r);
    r = avformat_find_stream_info(d.format, nullptr);
    if (r < 0) return d.fail("lettura flussi", r);
    for (unsigned i = 0; i < d.format->nb_streams; ++i) {
        const AVCodecParameters* p = d.format->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && d.videoIndex < 0) d.videoIndex = static_cast<int>(i);
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO) d.audioIndex.push_back(static_cast<int>(i));
    }
    if (d.videoIndex < 0) { d.error = "nessun flusso video"; return false; }
    AVStream* video = d.format->streams[d.videoIndex];
    const AVCodec* codec = avcodec_find_decoder(video->codecpar->codec_id);
    if (!codec) { d.error = "decoder video non disponibile"; return false; }
    d.decoder = avcodec_alloc_context3(codec);
    d.packet = av_packet_alloc();
    d.frame = av_frame_alloc();
    if (!d.decoder || !d.packet || !d.frame) return d.fail("allocazione", AVERROR(ENOMEM));
    r = avcodec_parameters_to_context(d.decoder, video->codecpar);
    if (r < 0) return d.fail("parametri decoder", r);
    r = avcodec_open2(d.decoder, codec, nullptr);
    if (r < 0) return d.fail("apertura decoder", r);
    d.width = video->codecpar->width;
    d.height = video->codecpar->height;
    d.frames = av_rescale_q(video->duration, video->time_base, kFrameBase);
    if (d.frames <= 0) { d.error = "durata non valida"; return false; }
    return true;
}

bool MxfReader::decodeGop(int64_t gopStart, const std::function<void(std::shared_ptr<DecodedFrame>)>& deliver) {
    Impl& d = *impl_;
    AVStream* video = d.format->streams[d.videoIndex];
    int r = av_seek_frame(d.format, d.videoIndex, av_rescale_q(gopStart, kFrameBase, video->time_base), AVSEEK_FLAG_BACKWARD);
    if (r < 0) return d.fail("posizionamento", r);
    avcodec_flush_buffers(d.decoder);

    const int64_t end = std::min<int64_t>(gopStart + kGopLength, d.frames);
    std::map<int64_t, std::map<int, std::vector<int32_t>>> audio;
    int64_t next = gopStart;

    // Con GOP aperto le B iniziali precedono l'I e dipendono dal GOP prima: si scartano
    // quelle con indice inferiore, mentre le ultime due richiedono l'I del GOP dopo.
    const auto receive = [&]() -> bool {
        for (;;) {
            int rc = avcodec_receive_frame(d.decoder, d.frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) return d.fail("decodifica", rc);
            const int64_t index = av_rescale_q(d.frame->best_effort_timestamp, video->time_base, kFrameBase);
            if (index >= next && index < end) {
                auto out = d.convert(index);
                const auto found = audio.find(index);
                if (found != audio.end() && found->second.size() == d.audioIndex.size()) {
                    for (const int stream : d.audioIndex) out->audio.push_back(found->second[stream]);
                }
                next = index + 1;
                deliver(std::move(out));
            }
            av_frame_unref(d.frame);
        }
    };

    bool eof = false;
    while (next < end && !eof) {
        r = av_read_frame(d.format, d.packet);
        if (r == AVERROR_EOF) { eof = true; break; }
        if (r < 0) return d.fail("lettura pacchetto", r);
        const int stream = d.packet->stream_index;
        bool ok = true;
        if (stream == d.videoIndex) {
            r = avcodec_send_packet(d.decoder, d.packet);
            ok = r < 0 ? d.fail("invio pacchetto", r) : receive();
        } else if (std::find(d.audioIndex.begin(), d.audioIndex.end(), stream) != d.audioIndex.end()) {
            AVStream* s = d.format->streams[stream];
            const int64_t index = av_rescale_q(d.packet->pts, s->time_base, kFrameBase);
            auto& samples = audio[index][stream];
            const int count = d.packet->size / 3;
            samples.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const uint8_t* b = d.packet->data + i * 3;
                samples[static_cast<std::size_t>(i)] =
                    static_cast<int32_t>((static_cast<uint32_t>(b[0]) << 8) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 24));
            }
        }
        av_packet_unref(d.packet);
        if (!ok) return false;
    }
    if (eof) {
        r = avcodec_send_packet(d.decoder, nullptr);
        if (r < 0 && r != AVERROR_EOF) return d.fail("svuotamento decoder", r);
        if (!receive()) return false;
    }
    if (next < end) { d.error = "GOP incompleto"; return false; }
    return true;
}
}  // namespace vs
