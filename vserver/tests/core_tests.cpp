#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "record/recorder.h"
#include "play/playback_channel.h"
#include "play/playlist_player.h"
#include "catalog/catalog.h"
#include <filesystem>
#include <map>
#include "core/encoder_profile.h"
#include "core/machine_config.h"
#include "core/playlist.h"
#include "core/speed.h"
#include "core/timecode.h"
#include "hal/simulated_device.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FALLITO riga %d: %s\n", __LINE__, #c); } } while (0)

using namespace vs;

static void timecode() {
    CHECK(formatTimecode(0) == "00:00:00:00");
    CHECK(formatTimecode(25) == "00:00:01:00");
    CHECK(formatTimecode(kFramesPerDay) == "00:00:00:00");
    CHECK(formatTimecode(-1) == "23:59:59:24");
    CHECK(parseTimecode("10:00:00:12") == (10 * 3600LL) * 25 + 12);
    CHECK(!parseTimecode("24:00:00:00"));
    CHECK(!parseTimecode("00:00:00:25"));
    CHECK(!parseTimecode("00:00:00:00x"));
    for (int64_t f : {int64_t{0}, int64_t{1}, int64_t{999999}, kFramesPerDay - 1}) CHECK(parseTimecode(formatTimecode(f)) == f);
}

static void clockSwitch() {
    TimecodeClock c;
    c.presetInternal(100);
    c.tick();
    CHECK(c.current() == 101);
    CHECK(!c.noSignal());
    c.setSource(TimecodeSource::ExternalLtc);
    CHECK(c.noSignal());
    CHECK(!c.current());
    c.onExternalLtc(500);
    CHECK(c.current() == 500);
    CHECK(!c.noSignal());
    c.setSource(TimecodeSource::Internal);
    CHECK(c.current() == 101);
}

static void speed() {
    FieldCursor normal(0, 1000);
    for (int i = 0; i < 10; ++i) CHECK(normal.advance(kSpeedNormal));
    CHECK(normal.field() == 10);

    FieldCursor quarter(0, 1000);
    for (int i = 0; i < 400; ++i) CHECK(quarter.advance(4));
    CHECK(quarter.field() == 100);

    FieldCursor stop(0, 1000);
    stop.seek(50);
    CHECK(stop.advance(0));
    CHECK(stop.field() == 50);

    FieldCursor reverse(0, 1000);
    reverse.seek(3);
    CHECK(reverse.advance(-kSpeedNormal));
    CHECK(reverse.field() == 2);
    reverse.seek(0);
    CHECK(!reverse.advance(-kSpeedNormal));
    CHECK(reverse.field() == 0);

    FieldCursor end(0, 5);
    end.seek(5);
    CHECK(!end.advance(kSpeedNormal));
    CHECK(end.field() == 5);
}

static void playlist() {
    Playlist p;
    CHECK(p.load(0).error == PlaylistError::Empty);
    CHECK(p.append({1, 10, 20}) == PlaylistError::None);
    CHECK(p.append({2, 0, 50}) == PlaylistError::None);
    CHECK(p.append({3, 5, 5}) == PlaylistError::InvalidItem);

    auto first = p.load(0, 3);
    CHECK(first.error == PlaylistError::None && first.startField == 26 && !first.endOfList);

    auto atEnd = p.load(2);
    CHECK(atEnd.error == PlaylistError::None && atEnd.index == 1 && atEnd.endOfList);
    CHECK(p.load(3).error == PlaylistError::OutOfRange);
    CHECK(p.load(0, 10).error == PlaylistError::OutOfRange);
    CHECK(p.load(0, -1).error == PlaylistError::OutOfRange);

    CHECK(p.move(1, 0) == PlaylistError::None);
    CHECK(p.at(0).clipId == 2);
    CHECK(p.setInOut(0, 7, 3) == PlaylistError::InvalidItem);
    CHECK(p.at(0).inFrame == 0);
    CHECK(p.setInOut(0, 2, 30) == PlaylistError::None);
    CHECK(p.remove(5) == PlaylistError::OutOfRange);
    CHECK(p.remove(0) == PlaylistError::None);
    CHECK(p.size() == 1);

    Playlist full;
    for (std::size_t i = 0; i < kPlaylistCapacity; ++i) CHECK(full.append({i, 0, 1}) == PlaylistError::None);
    CHECK(full.append({9, 0, 1}) == PlaylistError::Full);
}

static void machine() {
    MachineConfig cfg;
    SimulatedDevice dev;
    CHECK(cfg.inputs + cfg.outputs <= dev.channelCount());
    CHECK(cfg.audioTracks == 4);
    for (int ch = 0; ch < cfg.outputs; ++ch) CHECK(dev.setDirection(cfg.inputs + ch, ChannelDirection::Output));
    CHECK(dev.direction(0) == ChannelDirection::Input);
    CHECK(dev.direction(5) == ChannelDirection::Output);
    CHECK(!dev.setDirection(99, ChannelDirection::Input));
    CHECK(!dev.referenceLocked());
    dev.simulateReference(true);
    CHECK(dev.referenceLocked());
    dev.simulateLtc(1234);
    CHECK(dev.readLtc() == 1234);
}

static void encoder() {
    bool closedGop = false, gop12 = false;
    for (const auto& [key, value] : xdcam50EncoderOptions()) {
        if (value.find("cgop") != std::string::npos) closedGop = true;
        if (key == "g" && value == "12") gop12 = true;
    }
    CHECK(!closedGop);
    CHECK(gop12);
}

namespace {
class ScriptedSource final : public ICaptureSource {
public:
    explicit ScriptedSource(int frames, int64_t tcJumpAt = -1, bool noTc = false) : frames_(frames), jump_(tcJumpAt), noTc_(noTc) {}
    bool capture(CapturedFrame& f) override {
        if (next_ >= frames_) return false;
        f.sequence = next_;
        f.timecode = noTc_ ? std::nullopt : std::optional<int64_t>(next_ + ((jump_ >= 0 && next_ >= jump_) ? 50 : 0));
        f.audio = {{7, 7}};
        ++next_;
        return true;
    }
private:
    int frames_;
    int64_t jump_;
    bool noTc_;
    int64_t next_ = 0;
};

// Consegna sequenze prestabilite, per provare i buchi in modo deterministico.
class GappySource final : public ICaptureSource {
public:
    struct Step { int64_t sequence; int64_t timecode; };
    explicit GappySource(std::vector<Step> steps) : steps_(std::move(steps)) {}
    bool capture(CapturedFrame& f) override {
        if (next_ >= steps_.size()) return false;
        f.sequence = steps_[next_].sequence;
        f.timecode = steps_[next_].timecode;
        f.uyvy = {static_cast<uint8_t>(f.sequence)};
        f.audio = {{7, 7}};
        ++next_;
        return true;
    }
private:
    std::vector<Step> steps_;
    std::size_t next_ = 0;
};

class TestSink final : public IFrameSink {
public:
    int delayMs = 0;
    int failAfter = -1;
    std::vector<int64_t> seen;
    std::vector<int> pixels;
    int64_t silent = 0;
    int closes = 0;
    bool write(const CapturedFrame& f) override {
        if (delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        if (failAfter >= 0 && static_cast<int>(seen.size()) >= failAfter) return false;
        seen.push_back(f.sequence);
        pixels.push_back(f.uyvy.empty() ? -1 : f.uyvy[0]);
        if (!f.audio.empty() && !f.audio[0].empty() && f.audio[0][0] == 0) ++silent;
        return true;
    }
    bool close() override { ++closes; return true; }
    std::string lastError() const override { return "errore simulato"; }
};
}  // namespace

static void recorder() {
    {   // Tutti i fotogrammi arrivano in ordine.
        ScriptedSource src(200);
        TestSink sink;
        ChannelRecorder rec(src, sink, 250);
        rec.start();
        const auto s = rec.join();
        CHECK(s.ok && s.captured == 200 && s.written == 200 && s.dropped == 0 && s.timecodeBreaks == 0);
        CHECK(sink.seen.size() == 200);
        bool ordered = true;
        for (std::size_t i = 0; i < sink.seen.size(); ++i) if (sink.seen[i] != static_cast<int64_t>(i)) ordered = false;
        CHECK(ordered);
        CHECK(sink.closes == 1);
    }
    {   // Con scrittura lenta il file resta completo: ogni fotogramma perso è ripetuto.
        ScriptedSource src(300);
        TestSink sink;
        sink.delayMs = 2;
        ChannelRecorder rec(src, sink, 4);
        rec.start();
        const auto s = rec.join();
        CHECK(s.ok && s.captured == 300);
        CHECK(s.dropped > 0);
        CHECK(s.written == 300);
        CHECK(s.repeated == s.dropped);
        CHECK(sink.silent == s.repeated);
        CHECK(s.timecodeBreaks == 0);
        CHECK(sink.seen.size() == 300);
        bool ordered = true;
        for (std::size_t i = 0; i < sink.seen.size(); ++i) if (sink.seen[i] != static_cast<int64_t>(i)) ordered = false;
        CHECK(ordered);
    }
    {   // Un buco nel mezzo: si ripete l'immagine precedente con audio muto.
        GappySource src({{0, 100}, {1, 101}, {5, 105}, {6, 106}});
        TestSink sink;
        ChannelRecorder rec(src, sink, 10);
        rec.start();
        const auto s = rec.join();
        CHECK(s.written == 7 && s.repeated == 3 && s.timecodeBreaks == 0);
        CHECK(sink.silent == 3);
        CHECK((sink.seen == std::vector<int64_t>{0, 1, 2, 3, 4, 5, 6}));
        CHECK((sink.pixels == std::vector<int>{0, 1, 1, 1, 1, 5, 6}));
    }
    {   // Un buco con un vero salto di timecode viene comunque segnalato.
        GappySource src({{0, 0}, {3, 50}});
        TestSink sink;
        ChannelRecorder rec(src, sink, 10);
        rec.start();
        const auto s = rec.join();
        CHECK(s.timecodeBreaks == 1 && s.written == 4 && s.repeated == 2);
    }
    {   // Un salto di timecode viene segnalato una sola volta.
        ScriptedSource src(50, 20);
        TestSink sink;
        ChannelRecorder rec(src, sink, 60);
        rec.start();
        const auto s = rec.join();
        CHECK(s.timecodeBreaks == 1);
    }
    {   // Timecode assente: nessun salto, ma conteggiato.
        ScriptedSource src(10, -1, true);
        TestSink sink;
        ChannelRecorder rec(src, sink, 20);
        rec.start();
        const auto s = rec.join();
        CHECK(s.timecodeMissing == 10 && s.timecodeBreaks == 0);
    }
    {   // Errore di scrittura: la registrazione si ferma e riporta il motivo.
        ScriptedSource src(100000);
        TestSink sink;
        sink.failAfter = 5;
        ChannelRecorder rec(src, sink, 8);
        rec.start();
        const auto s = rec.join();
        CHECK(!s.ok && s.error == "errore simulato" && s.written == 5);
        CHECK(sink.closes == 1);
    }
}

namespace {
class FakeProvider final : public IFrameProvider {
public:
    explicit FakeProvider(int64_t frames, int delayMs = 0) : frames_(frames), delayMs_(delayMs) {}
    int64_t frameCount() const override { return frames_; }
    int gopLength() const override { return 12; }
    int width() const override { return 4; }
    int height() const override { return 4; }
    bool decodeGop(int64_t gopStart, const std::function<void(std::shared_ptr<DecodedFrame>)>& deliver) override {
        if (delayMs_) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        for (int64_t f = gopStart; f < gopStart + 12 && f < frames_; ++f) {
            auto d = std::make_shared<DecodedFrame>();
            d->index = f;
            d->uyvy.assign(32, static_cast<uint8_t>(f));
            d->audio = {{1}};
            deliver(std::move(d));
        }
        return true;
    }
private:
    int64_t frames_;
    int delayMs_;
};

class RecordingSink final : public IPlayoutSink {
public:
    struct Entry { int64_t a, b; bool audio; };
    std::vector<Entry> good;
    int repeated = 0;
    void output(const OutputFrame& f) override {
        if (f.repeated) { ++repeated; return; }
        good.push_back({f.fieldA, f.fieldB, !f.audio.empty()});
    }
};

void runUntil(PlaybackChannel& ch, RecordingSink& sink, std::size_t n) {
    for (int i = 0; i < 20000 && sink.good.size() < n; ++i) {
        ch.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}  // namespace

static void playback() {
    {   // Campi della stessa parità copiati, parità opposta interpolata.
        const std::vector<uint8_t> src{10, 10, 20, 20, 30, 30, 40, 40};
        std::vector<uint8_t> out(8, 0);
        composeField(out, src, 0, 1, 2, 4);
        CHECK(out[2] == 20 && out[6] == 30 && out[0] == 0);
        composeField(out, src, 1, 0, 2, 4);
        CHECK(out[0] == 20 && out[4] == 30);
        composeField(out, src, 1, 1, 2, 4);
        CHECK(out[2] == 20 && out[6] == 40);
    }
    {   // 1x: fotogrammi interi e consecutivi, con audio.
        FakeProvider p(120);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(10));
        ch.setSpeed(kSpeedNormal);
        runUntil(ch, s, 30);
        CHECK(s.good.size() == 30);
        bool ok = true;
        for (std::size_t i = 0; i < s.good.size(); ++i)
            if (s.good[i].a != 20 + static_cast<int64_t>(i) * 2 || s.good[i].b != s.good[i].a + 1 || !s.good[i].audio) ok = false;
        CHECK(ok);
    }
    {   // 0,5x: ogni campo sorgente occupa un intero fotogramma d'uscita, senza audio.
        FakeProvider p(120);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(50));
        ch.setSpeed(8);
        runUntil(ch, s, 6);
        CHECK(s.good[0].a == 100 && s.good[0].b == 100 && !s.good[0].audio);
        CHECK(s.good[1].a == 101 && s.good[1].b == 101);
        CHECK(s.good[5].a == 105);
    }
    {   // Indietro a 1x: si parte dal campo dispari dello stesso fotogramma.
        FakeProvider p(120);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(10));
        ch.setSpeed(-kSpeedNormal);
        runUntil(ch, s, 3);
        CHECK(s.good[0].a == 21 && s.good[0].b == 20 && !s.good[0].audio);
        CHECK(s.good[1].a == 19 && s.good[1].b == 18);
    }
    {   // Fermo immagine.
        FakeProvider p(120);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(30));
        ch.setSpeed(0);
        runUntil(ch, s, 5);
        CHECK(s.good[4].a == 60 && s.good[4].b == 60);
    }
    {   // Fine clip: si ferma sull'ultimo campo e lo segnala.
        FakeProvider p(24);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(23));
        ch.setSpeed(kSpeedNormal);
        runUntil(ch, s, 3);
        CHECK(s.good[0].a == 46 && s.good[0].b == 47);
        CHECK(s.good[2].a == 47);
        CHECK(ch.stats().ended);
    }
    {   // Inizio clip all'indietro.
        FakeProvider p(24);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(0));
        ch.setSpeed(-kSpeedNormal);
        runUntil(ch, s, 1);
        CHECK(s.good[0].a == 1 && s.good[0].b == 0);
        CHECK(ch.stats().ended);
    }
    {   // Decodifica lenta: l'uscita ripete, ma non salta nessun fotogramma.
        FakeProvider p(240, 40);
        RecordingSink s;
        PlaybackChannel ch(p, s);
        ch.start();
        CHECK(ch.cue(0));
        ch.setSpeed(kSpeedNormal);
        runUntil(ch, s, 96);
        CHECK(s.good.size() == 96);
        CHECK(ch.stats().underruns > 0);
        CHECK(s.repeated > 0);
        CHECK(ch.stats().decodeErrors == 0);
        bool consecutive = true;
        for (std::size_t i = 0; i < s.good.size(); ++i) if (s.good[i].a != static_cast<int64_t>(i) * 2) consecutive = false;
        CHECK(consecutive);
    }
}

static ClipRecord makeClip(const std::string& name, const std::string& path, int channel, int64_t tc, int64_t frames, const std::string& date,
                           std::vector<std::pair<std::string, std::string>> keys = {}) {
    ClipRecord c;
    c.name = name; c.path = path; c.channel = channel; c.startTimecode = tc; c.frames = frames; c.createdAt = date; c.keys = std::move(keys);
    return c;
}

static void catalog() {
    const std::string file = "core_tests_catalog.db";
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(file + suffix);
    int64_t a = 0, b = 0, c = 0;
    {
        Catalog cat;
        CHECK(cat.open(file));
        a = cat.addClip(makeClip("GOL INTER - MILAN", "a.mxf", 0, 1000, 100, "2026-04-19T20:00:00", {{"squadra", "INTER"}, {"squadra", "MILAN"}, {"evento", "GOL"}})).value_or(0);
        b = cat.addClip(makeClip("Rigore JUVENTUS 100%", "b.mxf", 1, 5000, 50, "2026-04-20T21:00:00", {{"squadra", "JUVENTUS"}, {"evento", "RIGORE"}})).value_or(0);
        c = cat.addClip(makeClip("INTERVISTA", "c.mxf", 2, 9000, 25, "2026-05-01T10:00:00")).value_or(0);
        CHECK(a > 0 && b > 0 && c > 0);

        CHECK(!cat.addClip(makeClip("doppia", "a.mxf", 0, 0, 10, "2026-01-01T00:00:00")));
        CHECK(!cat.lastError().empty());

        const auto ids = [](const std::vector<ClipRecord>& v) { std::vector<int64_t> o; for (const auto& r : v) o.push_back(r.id); return o; };
        using V = std::vector<int64_t>;
        ClipQuery q;
        CHECK(ids(cat.search(q)) == (V{a, b, c}));
        q = {}; q.nameContains = "inter";
        CHECK(ids(cat.search(q)) == (V{a, c}));
        q = {}; q.nameContains = "100%";
        CHECK(ids(cat.search(q)) == (V{b}));
        q = {}; q.nameContains = "%";
        CHECK(ids(cat.search(q)) == (V{b}));
        q = {}; q.nameContains = "_";
        CHECK(ids(cat.search(q)).empty());
        q = {}; q.keyContains = "MILAN"; q.keyKind = "squadra";
        CHECK(ids(cat.search(q)) == (V{a}));
        q = {}; q.keyContains = "GOL"; q.keyKind = "squadra";
        CHECK(ids(cat.search(q)).empty());
        q = {}; q.channel = 1;
        CHECK(ids(cat.search(q)) == (V{b}));
        q = {}; q.createdFrom = "2026-04-20T00:00:00"; q.createdTo = "2026-04-30T23:59:59";
        CHECK(ids(cat.search(q)) == (V{b}));
        q = {}; q.containsTimecode = 1099;
        CHECK(ids(cat.search(q)) == (V{a}));
        q = {}; q.containsTimecode = 1100;
        CHECK(ids(cat.search(q)).empty());
        q = {}; q.nameContains = "x' OR 1=1 --";
        CHECK(ids(cat.search(q)).empty());

        const auto loaded = cat.getClip(a);
        CHECK(loaded && loaded->keys.size() == 3 && loaded->frames == 100);
        CHECK(!cat.getClip(9999));

        Playlist pl;
        CHECK(pl.append({static_cast<uint64_t>(a), 10, 20}) == PlaylistError::None);
        CHECK(pl.append({static_cast<uint64_t>(b), 0, 50}) == PlaylistError::None);
        CHECK(cat.savePlaylist(1, pl));

        // Una clip in playlist non si cancella; una libera sì.
        CHECK(!cat.removeClip(a));
        CHECK(cat.getClip(a).has_value());
        CHECK(cat.removeClip(c));
        CHECK(!cat.getClip(c));
        CHECK(!cat.removeClip(c));

        // Una playlist che punta a una clip inesistente viene rifiutata e non tocca la precedente.
        Playlist bad;
        CHECK(bad.append({9999, 0, 1}) == PlaylistError::None);
        CHECK(!cat.savePlaylist(1, bad));
        Playlist same;
        CHECK(cat.loadPlaylist(1, same) && same.size() == 2);
    }
    {   // I dati restano dopo la chiusura.
        Catalog cat;
        CHECK(cat.open(file));
        CHECK(cat.integrityCheck() == "ok");
        Playlist pl;
        CHECK(cat.loadPlaylist(1, pl));
        CHECK(pl.size() == 2 && pl.at(0).clipId == static_cast<uint64_t>(a) && pl.at(0).inFrame == 10 && pl.at(1).outFrame == 50);
        Playlist empty;
        CHECK(cat.loadPlaylist(0, empty) && empty.size() == 0);
        // Aggiornare una playlist la sostituisce per intero.
        Playlist shorter;
        CHECK(shorter.append({static_cast<uint64_t>(b), 1, 2}) == PlaylistError::None);
        CHECK(cat.savePlaylist(1, shorter));
        CHECK(cat.loadPlaylist(1, pl) && pl.size() == 1 && pl.at(0).inFrame == 1);
    }
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(file + suffix);
}

static void player() {
    const ProviderFactory factory = [](const std::string& path) -> std::unique_ptr<IFrameProvider> {
        if (path == "manca") return nullptr;
        return std::make_unique<FakeProvider>(path == "corta" ? 24 : 120);
    };
    const auto run = [](PlaylistPlayer& p) {
        int idle = 0;
        while (p.tick() && idle < 20000) { ++idle; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    };
    {   // Sequenza con tagli esatti, senza fotogrammi in più o in meno.
        RecordingSink s;
        PlaylistPlayer p(factory, s);
        CHECK(p.start({{"a", 10, 14}, {"b", 50, 53}, {"c", 0, 2}}));
        run(p);
        CHECK(p.error().empty() && p.frames() == 9 && p.cuts() == 2);
        std::vector<int64_t> frames;
        for (const auto& e : s.good) { frames.push_back(e.a / 2); CHECK(e.b == e.a + 1 && e.audio); }
        CHECK((frames == std::vector<int64_t>{10, 11, 12, 13, 50, 51, 52, 0, 1}));
    }
    {   // Elementi di un solo fotogramma.
        RecordingSink s;
        PlaylistPlayer p(factory, s);
        CHECK(p.start({{"a", 5, 6}, {"b", 7, 8}, {"c", 9, 10}}));
        run(p);
        CHECK(p.frames() == 3 && s.good.size() == 3);
        CHECK(s.good[0].a == 10 && s.good[1].a == 14 && s.good[2].a == 18);
    }
    {   // Clip mancante: la sequenza si ferma sull'ultimo fotogramma e segnala il motivo.
        RecordingSink s;
        PlaylistPlayer p(factory, s);
        CHECK(p.start({{"a", 0, 3}, {"manca", 0, 3}, {"c", 0, 3}}));
        run(p);
        CHECK(p.frames() == 3);
        CHECK(p.error().find("manca") != std::string::npos);
        CHECK(!p.tick());
        CHECK(s.repeated > 0);
    }
    {   // Punti oltre la durata e prima clip non valida vengono rifiutati.
        RecordingSink s;
        PlaylistPlayer p(factory, s);
        CHECK(!p.start({{"corta", 0, 25}}));
        CHECK(!p.error().empty());
        CHECK(!p.tick());
    }
    {
        RecordingSink s;
        PlaylistPlayer p(factory, s);
        CHECK(!p.start({}));
    }
}

int main() {
    timecode(); clockSwitch(); speed(); playlist(); machine(); encoder(); recorder(); playback(); catalog(); player();
    if (failures) { std::printf("%d controlli falliti\n", failures); return 1; }
    std::printf("tutti i controlli superati\n");
    return 0;
}
