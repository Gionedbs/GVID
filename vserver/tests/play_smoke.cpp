// Registra una clip con il numero di fotogramma impresso nell'immagine, poi la riproduce
// su 2 uscite e controlla che ogni campo in uscita sia esattamente quello atteso.
// Uso: play_smoke <cartella> <velocita, 1 = tempo reale>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "hal/simulated_capture.h"
#include "play/mxf_reader.h"
#include "play/playback_channel.h"
#include "record/mxf_writer.h"
#include "record/recorder.h"

using namespace vs;

namespace {
constexpr int64_t kFrames = 750;
constexpr std::size_t kStride = 1920 * 2;

class CheckingSink final : public IPlayoutSink {
public:
    int64_t frames = 0, mismatches = 0, repeated = 0, audioFrames = 0, audioErrors = 0;
    void reset() { frames = mismatches = repeated = audioFrames = audioErrors = 0; }
    void output(const OutputFrame& f) override {
        if (f.repeated) { ++repeated; return; }
        ++frames;
        // Righe 24 e 25: una per campo, all'interno dei blocchi del codice.
        if (readFrameCode(f.uyvy, kStride, 24) != f.fieldA / 2 || readFrameCode(f.uyvy, kStride, 25) != f.fieldB / 2) ++mismatches;
        if (f.audio.empty()) return;
        ++audioFrames;
        if (f.audio.size() != 4) { ++audioErrors; return; }
        const int64_t frame = f.fieldA / 2;
        for (std::size_t t = 0; t < 4; ++t) {
            if (f.audio[t].size() != kAudioSamplesPerFrame) { ++audioErrors; continue; }
            for (int s : {0, 960, 1919}) {
                const int64_t diff = int64_t{f.audio[t][static_cast<std::size_t>(s)]} - simulatedAudioSample(frame, s, t);
                if (diff > 512 || diff < -512) { ++audioErrors; break; }
            }
        }
    }
};

struct Scenario { const char* name; int64_t start; int speed; int ticks; };

const std::vector<Scenario> kScenarios = {
    {"1x", 100, 16, 100},      {"0,5x", 200, 8, 100},     {"0,25x", 300, 4, 100},
    {"2x", 40, 32, 100},       {"-1x", 500, -16, 100},    {"-0,5x", 450, -8, 100},
    {"fermo", 400, 0, 25},     {"salto-a", 700, 16, 10},  {"salto-b", 50, 16, 10},
    {"salto-c", 600, 16, 10},  {"salto-d", 120, -16, 10}, {"fine clip", 740, 16, 40},
    {"inizio clip", 10, -16, 40},
};

int runOutput(int id, const std::string& path, double pace, std::vector<Scenario> scenarios) {
    MxfReader reader;
    if (!reader.open(path)) { std::printf("uscita %d: %s\n", id, reader.lastError().c_str()); return 1; }
    if (reader.frameCount() != kFrames) { std::printf("uscita %d: durata %lld\n", id, static_cast<long long>(reader.frameCount())); return 1; }
    CheckingSink sink;
    PlaybackChannel channel(reader, sink);
    channel.start();
    int status = 0;
    const auto interval = std::chrono::duration<double, std::milli>(40.0 / pace);
    for (const Scenario& sc : scenarios) {
        sink.reset();
        const auto before = channel.stats().underruns;
        const auto t0 = std::chrono::steady_clock::now();
        if (!channel.cue(sc.start, sc.speed < 0 ? -1 : 1)) { std::printf("uscita %d %s: cue fallito (%s)\n", id, sc.name, reader.lastError().c_str()); return 1; }
        const auto cueMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        channel.setSpeed(sc.speed);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < sc.ticks; ++i) {
            std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval * i));
            channel.tick();
        }
        const auto s = channel.stats();
        const int64_t under = s.underruns - before;
        const bool audioExpected = sc.speed == 16;
        const bool audioOk = sink.audioErrors == 0 && (!audioExpected || sink.audioFrames > 0);
        const bool ok = sink.mismatches == 0 && under == 0 && audioOk && s.decodeErrors == 0;
        std::printf("uscita %d %-12s cue %5.1f ms | fotogrammi %3lld errati %lld | ripetuti %lld | audio %3lld errori %lld | fine=%d %s\n", id, sc.name, cueMs,
                    static_cast<long long>(sink.frames), static_cast<long long>(sink.mismatches), static_cast<long long>(under),
                    static_cast<long long>(sink.audioFrames), static_cast<long long>(sink.audioErrors), s.ended ? 1 : 0, ok ? "" : "<-- FALLITO");
        if (!ok) status = 1;
    }
    if (reader.lastError().size()) std::printf("uscita %d ultimo errore: %s\n", id, reader.lastError().c_str());
    return status;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) { std::puts("uso: play_smoke <cartella> <velocita>"); return 2; }
    const std::string path = std::string(argv[1]) + "/playtest.mxf";
    const double pace = std::atof(argv[2]);
    if (pace <= 0.0) { std::puts("velocita non valida"); return 2; }

    {
        SimulatedCapture::Options s;
        s.frames = kFrames;
        s.paceFactor = 1.0;
        s.burnFrameCode = true;
        SimulatedCapture source(s);
        MxfWriter writer;
        MxfWriter::Options o;
        o.path = path;
        o.startTimecode = s.startTimecode;
        if (!writer.open(o)) { std::printf("scrittura: %s\n", writer.lastError().c_str()); return 1; }
        ChannelRecorder recorder(source, writer, 100);
        recorder.start();
        const auto r = recorder.join();
        std::printf("clip registrata: scritti %lld persi %lld ok=%d\n", static_cast<long long>(r.written), static_cast<long long>(r.dropped), r.ok ? 1 : 0);
        if (!r.ok || r.written != kFrames || r.dropped != 0) return 1;
    }

    int a = 1, b = 1;
    std::vector<Scenario> reversed(kScenarios.rbegin(), kScenarios.rend());
    std::thread first([&] { a = runOutput(1, path, pace, kScenarios); });
    std::thread second([&] { b = runOutput(2, path, pace, reversed); });
    first.join();
    second.join();
    std::puts((a || b) ? "ESITO: FALLITO" : "ESITO: OK");
    return (a || b) ? 1 : 0;
}
