// Crea un file di prova per i sistemi di montaggio, usando lo stesso MxfWriter della registrazione.
// Legge video UYVY 1920x1080 grezzo dallo standard input; l'audio è un tono per traccia.
// Uso: ffmpeg ... -pix_fmt uyvy422 -f rawvideo - | make_test_clip <file.mxf>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>

#include "hal/simulated_capture.h"
#include "record/mxf_writer.h"

using namespace vs;

int main(int argc, char** argv) {
    if (argc != 2) { std::puts("uso: make_test_clip <file.mxf>"); return 2; }
    _setmode(_fileno(stdin), _O_BINARY);

    MxfWriter writer;
    MxfWriter::Options options;
    options.path = argv[1];
    options.audioTracks = 4;
    options.startTimecode = parseTimecode("10:00:00:00").value();
    if (!writer.open(options)) { std::printf("errore: %s\n", writer.lastError().c_str()); return 1; }

    CapturedFrame frame;
    frame.uyvy.resize(1920u * 1080u * 2u);
    frame.audio.resize(4);
    for (auto& track : frame.audio) track.resize(kAudioSamplesPerFrame);

    int64_t count = 0;
    while (std::fread(frame.uyvy.data(), 1, frame.uyvy.size(), stdin) == frame.uyvy.size()) {
        for (std::size_t t = 0; t < frame.audio.size(); ++t)
            for (int s = 0; s < kAudioSamplesPerFrame; ++s) frame.audio[t][static_cast<std::size_t>(s)] = simulatedAudioSample(count, s, t);
        frame.sequence = count;
        frame.timecode = options.startTimecode + count;
        if (!writer.write(frame)) { std::printf("errore: %s\n", writer.lastError().c_str()); return 1; }
        ++count;
    }
    if (!writer.close()) { std::printf("errore: %s\n", writer.lastError().c_str()); return 1; }
    std::printf("fotogrammi scritti: %lld\n", static_cast<long long>(count));
    return count > 0 ? 0 : 1;
}
