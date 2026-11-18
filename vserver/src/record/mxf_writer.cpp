#include "record/mxf_writer.h"

#include <cstring>
#include <vector>

#include "core/encoder_profile.h"
#include "core/timecode.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace vs {
namespace {
std::string describe(const char* step, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return std::string(step) + ": " + text;
}
}  // namespace

struct MxfWriter::Impl {
    struct Stream {
        AVCodecContext* encoder = nullptr;
        AVStream* stream = nullptr;
        AVFrame* frame = nullptr;
        int64_t nextPts = 0;
    };

    AVFormatContext* format = nullptr;
    AVPacket* packet = nullptr;
    Stream video;
    std::vector<Stream> audio;
    Options options;
    std::string error;
    bool headerWritten = false;
    bool open = false;

    bool fail(const char* step, int code) { error = describe(step, code); return false; }

    bool drain(Stream& s) {
        for (;;) {
            int r = avcodec_receive_packet(s.encoder, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return fail("ricezione pacchetto", r);
            av_packet_rescale_ts(packet, s.encoder->time_base, s.stream->time_base);
            packet->stream_index = s.stream->index;
            r = av_interleaved_write_frame(format, packet);
            if (r < 0) return fail("scrittura MXF", r);
        }
    }

    bool send(Stream& s, AVFrame* frame) {
        const int r = avcodec_send_frame(s.encoder, frame);
        if (r < 0) return fail("codifica", r);
        return drain(s);
    }

    bool addVideo() {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
        if (!codec) { error = "encoder MPEG-2 non disponibile"; return false; }
        video.encoder = avcodec_alloc_context3(codec);
        video.stream = avformat_new_stream(format, nullptr);
        video.frame = av_frame_alloc();
        if (!video.encoder || !video.stream || !video.frame) return fail("allocazione video", AVERROR(ENOMEM));
        AVCodecContext* c = video.encoder;
        c->width = options.width;
        c->height = options.height;
        c->pix_fmt = AV_PIX_FMT_YUV422P;
        c->time_base = AVRational{1, static_cast<int>(kFps)};
        c->framerate = AVRational{static_cast<int>(kFps), 1};
        c->sample_aspect_ratio = AVRational{1, 1};
        c->field_order = AV_FIELD_TT;
        c->thread_count = options.encoderThreads;
        AVDictionary* opts = nullptr;
        for (const auto& [key, value] : xdcam50EncoderOptions()) av_dict_set(&opts, key.c_str(), value.c_str(), 0);
        int r = avcodec_open2(c, codec, &opts);
        const bool leftover = av_dict_count(opts) != 0;
        std::string unknown = leftover ? av_dict_iterate(opts, nullptr)->key : "";
        av_dict_free(&opts);
        if (r < 0) return fail("apertura encoder video", r);
        if (leftover) { error = "opzione encoder non riconosciuta: " + unknown; return false; }
        r = avcodec_parameters_from_context(video.stream->codecpar, c);
        if (r < 0) return fail("parametri video", r);
        video.stream->time_base = c->time_base;
        video.stream->avg_frame_rate = c->framerate;
        video.frame->format = c->pix_fmt;
        video.frame->width = c->width;
        video.frame->height = c->height;
        r = av_frame_get_buffer(video.frame, 0);
        return r < 0 ? fail("buffer video", r) : true;
    }

    bool addAudio(Stream& s) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
        if (!codec) { error = "encoder PCM non disponibile"; return false; }
        s.encoder = avcodec_alloc_context3(codec);
        s.stream = avformat_new_stream(format, nullptr);
        s.frame = av_frame_alloc();
        if (!s.encoder || !s.stream || !s.frame) return fail("allocazione audio", AVERROR(ENOMEM));
        AVCodecContext* c = s.encoder;
        c->sample_fmt = AV_SAMPLE_FMT_S32;
        c->sample_rate = 48000;
        c->time_base = AVRational{1, 48000};
        const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        int r = av_channel_layout_copy(&c->ch_layout, &mono);
        if (r < 0) return fail("layout audio", r);
        r = avcodec_open2(c, codec, nullptr);
        if (r < 0) return fail("apertura encoder audio", r);
        r = avcodec_parameters_from_context(s.stream->codecpar, c);
        if (r < 0) return fail("parametri audio", r);
        s.stream->time_base = c->time_base;
        s.frame->format = c->sample_fmt;
        s.frame->sample_rate = c->sample_rate;
        s.frame->nb_samples = kAudioSamplesPerFrame;
        r = av_channel_layout_copy(&s.frame->ch_layout, &c->ch_layout);
        if (r < 0) return fail("layout frame audio", r);
        r = av_frame_get_buffer(s.frame, 0);
        return r < 0 ? fail("buffer audio", r) : true;
    }

    void release() {
        if (video.encoder) avcodec_free_context(&video.encoder);
        if (video.frame) av_frame_free(&video.frame);
        for (auto& s : audio) {
            if (s.encoder) avcodec_free_context(&s.encoder);
            if (s.frame) av_frame_free(&s.frame);
        }
        audio.clear();
        if (packet) av_packet_free(&packet);
        if (format) {
            if (format->pb) avio_closep(&format->pb);
            avformat_free_context(format);
            format = nullptr;
        }
        open = false;
        headerWritten = false;
    }

    void convertUyvy(const std::vector<uint8_t>& uyvy) {
        const int w = options.width, h = options.height;
        for (int y = 0; y < h; ++y) {
            const uint8_t* in = uyvy.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 2;
            uint8_t* py = video.frame->data[0] + y * video.frame->linesize[0];
            uint8_t* pu = video.frame->data[1] + y * video.frame->linesize[1];
            uint8_t* pv = video.frame->data[2] + y * video.frame->linesize[2];
            for (int x = 0; x < w / 2; ++x) {
                pu[x] = in[0];
                py[2 * x] = in[1];
                pv[x] = in[2];
                py[2 * x + 1] = in[3];
                in += 4;
            }
        }
    }
};

MxfWriter::MxfWriter() : impl_(std::make_unique<Impl>()) {}
MxfWriter::~MxfWriter() { close(); impl_->release(); }
std::string MxfWriter::lastError() const { return impl_->error; }

bool MxfWriter::open(const Options& options) {
    Impl& d = *impl_;
    d.options = options;
    const auto abort = [&d] { d.release(); return false; };
    int r = avformat_alloc_output_context2(&d.format, nullptr, "mxf", options.path.c_str());
    if (r < 0) { d.fail("creazione MXF", r); return abort(); }
    d.packet = av_packet_alloc();
    if (!d.packet) { d.fail("allocazione pacchetto", AVERROR(ENOMEM)); return abort(); }
    if (!d.addVideo()) return abort();
    d.audio.resize(static_cast<std::size_t>(options.audioTracks));
    for (auto& s : d.audio) if (!d.addAudio(s)) return abort();
    av_dict_set(&d.format->metadata, "timecode", formatTimecode(options.startTimecode).c_str(), 0);
    r = avio_open(&d.format->pb, options.path.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) { d.fail("apertura file", r); return abort(); }
    r = avformat_write_header(d.format, nullptr);
    if (r < 0) { d.fail("intestazione MXF", r); return abort(); }
    d.headerWritten = true;
    d.open = true;
    return true;
}

bool MxfWriter::write(const CapturedFrame& f) {
    Impl& d = *impl_;
    if (!d.open) { d.error = "file non aperto"; return false; }
    const auto expected = static_cast<std::size_t>(d.options.width) * static_cast<std::size_t>(d.options.height) * 2;
    if (f.uyvy.size() != expected) { d.error = "dimensione video inattesa"; return false; }
    if (f.audio.size() != d.audio.size()) { d.error = "numero tracce audio inatteso"; return false; }

    int r = av_frame_make_writable(d.video.frame);
    if (r < 0) return d.fail("frame video", r);
    d.convertUyvy(f.uyvy);
    d.video.frame->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    d.video.frame->pts = d.video.nextPts++;
    if (!d.send(d.video, d.video.frame)) return false;

    for (std::size_t t = 0; t < d.audio.size(); ++t) {
        Impl::Stream& s = d.audio[t];
        if (f.audio[t].size() != kAudioSamplesPerFrame) { d.error = "campioni audio inattesi"; return false; }
        r = av_frame_make_writable(s.frame);
        if (r < 0) return d.fail("frame audio", r);
        std::memcpy(s.frame->data[0], f.audio[t].data(), kAudioSamplesPerFrame * sizeof(int32_t));
        s.frame->pts = s.nextPts;
        s.nextPts += kAudioSamplesPerFrame;
        if (!d.send(s, s.frame)) return false;
    }
    return true;
}

bool MxfWriter::close() {
    Impl& d = *impl_;
    if (!d.open) return d.error.empty();
    d.open = false;
    bool ok = d.send(d.video, nullptr);
    for (auto& s : d.audio) ok = d.send(s, nullptr) && ok;
    if (d.headerWritten) {
        const int r = av_write_trailer(d.format);
        if (r < 0) { d.fail("chiusura MXF", r); ok = false; }
    }
    d.release();
    return ok;
}
}  // namespace vs
