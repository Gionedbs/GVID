// Flusso completo: registra 3 clip a catalogo, le ritrova, compone una playlist,
// riapre il catalogo e la riproduce controllando ogni fotogramma in uscita.
// Uso: flow_smoke <cartella> <velocita, 1 = tempo reale>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/ingest.h"
#include "catalog/catalog.h"
#include "hal/simulated_capture.h"
#include "play/mxf_reader.h"
#include "play/playlist_player.h"

using namespace vs;

namespace {
constexpr std::size_t kStride = 1920 * 2;

struct Expected { int64_t code; int64_t frame; };

class SequenceSink final : public IPlayoutSink {
public:
    explicit SequenceSink(std::vector<Expected> expected) : expected_(std::move(expected)) {}
    int64_t good = 0, wrong = 0, audioErrors = 0, repeated = 0, extra = 0;
    void output(const OutputFrame& f) override {
        if (f.repeated) { ++repeated; return; }
        if (position_ >= expected_.size()) { ++extra; return; }
        const Expected e = expected_[position_++];
        const bool video = readFrameCode(f.uyvy, kStride, 24) == e.code && readFrameCode(f.uyvy, kStride, 25) == e.code;
        if (video) ++good; else { ++wrong; if (wrong <= 5) std::printf("  atteso %lld, trovato %lld\n", static_cast<long long>(e.code), static_cast<long long>(readFrameCode(f.uyvy, kStride, 24))); }
        if (f.audio.size() != 4) { ++audioErrors; return; }
        for (std::size_t t = 0; t < 4; ++t) {
            if (f.audio[t].size() != kAudioSamplesPerFrame) { ++audioErrors; continue; }
            const int64_t diff = int64_t{f.audio[t][960]} - simulatedAudioSample(e.frame, 960, t);
            if (diff > 512 || diff < -512) ++audioErrors;
        }
    }
    bool complete() const { return position_ == expected_.size(); }
private:
    std::vector<Expected> expected_;
    std::size_t position_ = 0;
};

int fail(const std::string& text) {
    std::printf("FALLITO: %s\nESITO: FALLITO\n", text.c_str());
    return 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) { std::puts("uso: flow_smoke <cartella> <velocita>"); return 2; }
    const std::string dir = argv[1];
    const double pace = std::atof(argv[2]);
    if (pace <= 0.0) { std::puts("velocita non valida"); return 2; }
    const std::string db = dir + "/flow.db";
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(db + suffix);

    struct Plan { const char* name; int64_t frames; int64_t base; std::vector<std::pair<std::string, std::string>> keys; const char* date; };
    const std::vector<Plan> plans = {
        {"GOL Inter - Milan", 150, 10000, {{"squadra", "INTER"}, {"squadra", "MILAN"}, {"evento", "GOL"}}, "2026-04-19T20:45:00"},
        {"Rigore Juventus", 100, 20000, {{"squadra", "JUVENTUS"}, {"evento", "RIGORE"}}, "2026-04-20T18:00:00"},
        {"Intervista allenatore", 200, 30000, {{"evento", "INTERVISTA"}}, "2026-05-02T12:00:00"},
    };

    std::vector<int64_t> ids(plans.size());
    {
        Catalog catalog;
        if (!catalog.open(db)) return fail(catalog.lastError());
        std::vector<IngestResult> results(plans.size());
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < plans.size(); ++i) {
            threads.emplace_back([&, i] {
                SimulatedCapture::Options s;
                s.channel = static_cast<int>(i);
                s.frames = plans[i].frames;
                s.paceFactor = 1.0;
                s.burnFrameCode = true;
                s.frameCodeBase = plans[i].base;
                SimulatedCapture source(s);
                ClipRecord clip;
                clip.name = plans[i].name;
                clip.path = dir + "/flow" + std::to_string(i + 1) + ".mxf";
                clip.channel = static_cast<int>(i);
                clip.startTimecode = s.startTimecode;
                clip.createdAt = plans[i].date;
                clip.keys = plans[i].keys;
                results[i] = ingestClip(source, catalog, clip, 4);
            });
        }
        for (auto& t : threads) t.join();
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (!results[i].ok) return fail("registrazione: " + results[i].error);
            if (results[i].stats.dropped != 0) return fail("fotogrammi persi in registrazione");
            ids[i] = results[i].clipId;
        }
        std::puts("1. registrazione: 3 clip a catalogo");

        const auto only = [&](const ClipQuery& q, int64_t id) { const auto r = catalog.search(q); return r.size() == 1 && r[0].id == id; };
        ClipQuery q;
        q.keyContains = "juve";
        if (!only(q, ids[1])) return fail("ricerca per chiave");
        q = {}; q.nameContains = "intervista";
        if (!only(q, ids[2])) return fail("ricerca per nome");
        q = {}; q.createdFrom = "2026-04-19T00:00:00"; q.createdTo = "2026-04-19T23:59:59";
        if (!only(q, ids[0])) return fail("ricerca per data");
        q = {}; q.keyContains = "GOL"; q.keyKind = "evento"; q.containsTimecode = 900000 + 149;
        if (!only(q, ids[0])) return fail("ricerca per timecode");
        std::puts("2. ricerca: nome, chiave, data, timecode");

        Playlist playlist;
        const auto add = [&](std::size_t clip, int64_t in, int64_t out) { return playlist.append({static_cast<uint64_t>(ids[clip]), in, out}) == PlaylistError::None; };
        // Include un elemento di mezzo secondo, GOP non allineati e la stessa clip due volte.
        if (!add(2, 7, 60) || !add(0, 100, 150) || !add(1, 0, 13) || !add(2, 150, 200) || !add(0, 3, 40)) return fail("composizione playlist");
        if (!catalog.savePlaylist(0, playlist)) return fail(catalog.lastError());
        std::puts("3. playlist salvata");
    }

    Catalog reopened;
    if (!reopened.open(db)) return fail(reopened.lastError());
    if (reopened.integrityCheck() != "ok") return fail("integrita catalogo");
    Playlist playlist;
    if (!reopened.loadPlaylist(0, playlist)) return fail(reopened.lastError());
    if (playlist.size() != 5) return fail("playlist ricaricata incompleta");

    std::vector<PlayoutEntry> entries;
    std::vector<Expected> expected;
    for (std::size_t i = 0; i < playlist.size(); ++i) {
        const PlaylistItem& item = playlist.at(i);
        const auto clip = reopened.getClip(static_cast<int64_t>(item.clipId));
        if (!clip) return fail("clip della playlist non trovata");
        entries.push_back({clip->path, item.inFrame, item.outFrame});
        const int64_t base = plans[static_cast<std::size_t>(clip->channel)].base;
        for (int64_t f = item.inFrame; f < item.outFrame; ++f) expected.push_back({base + f, f});
    }
    std::puts("4. catalogo riaperto, playlist ricaricata");

    SequenceSink sink(expected);
    ProviderFactory factory = [](const std::string& path) -> std::unique_ptr<IFrameProvider> {
        auto reader = std::make_unique<MxfReader>();
        if (!reader->open(path)) return nullptr;
        return reader;
    };
    PlaylistPlayer player(factory, sink);
    if (!player.start(entries)) return fail("avvio riproduzione: " + player.error());
    const auto interval = std::chrono::duration<double, std::milli>(40.0 / pace);
    const auto t0 = std::chrono::steady_clock::now();
    int64_t ticks = 0;
    for (;;) {
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval * static_cast<double>(ticks)));
        ++ticks;
        if (!player.tick()) break;
    }
    std::printf("5. riproduzione: attesi %zu, corretti %lld, errati %lld, in piu %lld, audio errati %lld\n", expected.size(),
                static_cast<long long>(sink.good), static_cast<long long>(sink.wrong), static_cast<long long>(sink.extra), static_cast<long long>(sink.audioErrors));
    std::printf("   tagli %lld, tagli in ritardo %lld, ripetizioni %lld, dati in ritardo %lld %s\n", static_cast<long long>(player.cuts()), static_cast<long long>(player.lateCuts()),
                static_cast<long long>(sink.repeated), static_cast<long long>(player.underruns()), player.error().c_str());
    const bool ok = sink.complete() && sink.wrong == 0 && sink.extra == 0 && sink.audioErrors == 0 && player.cuts() == 4 && player.lateCuts() == 0 && player.underruns() == 0 && player.error().empty();
    std::puts(ok ? "ESITO: OK" : "ESITO: FALLITO");
    return ok ? 0 : 1;
}
