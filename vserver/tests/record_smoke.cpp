// Prova di registrazione a 4 canali su sorgente simulata.
// Uso: record_smoke <cartella> <secondi> <velocita, 1 = tempo reale>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/machine_config.h"
#include "hal/simulated_capture.h"
#include "record/mxf_writer.h"
#include "record/recorder.h"

using namespace vs;

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) { std::puts("uso: record_smoke <cartella> <secondi> <velocita, 1 = tempo reale> [thread encoder]"); return 2; }
    const int encoderThreads = argc == 5 ? std::atoi(argv[4]) : 0;
    const std::string dir = argv[1];
    const int64_t frames = std::atoll(argv[2]) * 25;
    const double pace = std::atof(argv[3]);
    if (pace <= 0.0) { std::puts("velocita non valida"); return 2; }
    const MachineConfig cfg;

    {   // Un percorso non valido deve produrre un errore, non un file vuoto.
        MxfWriter bad;
        MxfWriter::Options o;
        o.path = dir + "/cartella-inesistente/x.mxf";
        o.audioTracks = cfg.audioTracks;
        if (bad.open(o)) { std::puts("FALLITO: aperto un percorso non valido"); return 1; }
        std::printf("percorso non valido -> %s\n", bad.lastError().c_str());
    }

    struct Channel {
        std::unique_ptr<SimulatedCapture> source;
        std::unique_ptr<MxfWriter> writer;
        std::unique_ptr<ChannelRecorder> recorder;
    };
    std::vector<Channel> channels(static_cast<std::size_t>(cfg.inputs));
    for (int i = 0; i < cfg.inputs; ++i) {
        Channel& c = channels[static_cast<std::size_t>(i)];
        SimulatedCapture::Options s;
        s.channel = i;
        s.frames = frames;
        s.audioTracks = cfg.audioTracks;
        s.paceFactor = pace;
        if (i == 3) s.timecodeBreakAt = frames / 2;
        c.source = std::make_unique<SimulatedCapture>(s);
        c.writer = std::make_unique<MxfWriter>();
        MxfWriter::Options o;
        o.path = dir + "/ch" + std::to_string(i + 1) + ".mxf";
        o.audioTracks = cfg.audioTracks;
        o.startTimecode = s.startTimecode;
        o.encoderThreads = encoderThreads;
        if (!c.writer->open(o)) { std::printf("apertura ch%d: %s\n", i + 1, c.writer->lastError().c_str()); return 1; }
        c.recorder = std::make_unique<ChannelRecorder>(*c.source, *c.writer);
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (auto& c : channels) c.recorder->start();
    int status = 0;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const RecordingStats r = channels[i].recorder->join();
        std::printf("ch%zu: catturati %lld scritti %lld persi %lld ripetuti %lld coda-max %zu salti-tc %lld ok=%d %s\n", i + 1,
                    static_cast<long long>(r.captured), static_cast<long long>(r.written), static_cast<long long>(r.dropped),
                    static_cast<long long>(r.repeated), r.maxQueued, static_cast<long long>(r.timecodeBreaks), r.ok ? 1 : 0, r.error.c_str());
        if (!r.ok || r.captured != frames || r.written != frames || r.repeated != r.dropped) status = 1;
        if (pace <= 1.0 && r.dropped != 0) status = 1;  // in tempo reale non si perde nulla
        if (r.timecodeBreaks != (i == 3 ? 1 : 0)) status = 1;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("durata %.1f s per %lld fotogrammi per canale (%.2fx tempo reale)\n", seconds,
                static_cast<long long>(frames), static_cast<double>(frames) / 25.0 / seconds);
    std::puts(status ? "ESITO: FALLITO" : "ESITO: OK");
    return status;
}
