#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "core/speed.h"
#include "hal/playout.h"
#include "play/field_mixer.h"
namespace vs {
struct DecodedFrame {
    int64_t index = 0;
    std::vector<uint8_t> uyvy;
    std::vector<std::vector<int32_t>> audio;  // vuoto se non disponibile
};

class IFrameProvider {
public:
    virtual ~IFrameProvider() = default;
    virtual int64_t frameCount() const = 0;
    virtual int gopLength() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    // Decodifica tutti i fotogrammi del GOP che parte da gopStart, in ordine di visualizzazione.
    virtual bool decodeGop(int64_t gopStart, const std::function<void(std::shared_ptr<DecodedFrame>)>& deliver) = 0;
};

struct PlaybackStats {
    int64_t ticks = 0;
    int64_t underruns = 0;
    int64_t decodeErrors = 0;
    bool ended = false;
};

// Un filo decodifica in anticipo, tick() produce un fotogramma d'uscita senza mai aspettare.
// Se i dati non sono pronti ripete l'uscita precedente e tiene ferma la posizione.
class PlaybackChannel {
public:
    PlaybackChannel(IFrameProvider& provider, IPlayoutSink& sink)
        : provider_(provider), sink_(sink), cursor_(0, provider.frameCount() * 2 - 1) {}
    ~PlaybackChannel() { stop(); }
    PlaybackChannel(const PlaybackChannel&) = delete;
    PlaybackChannel& operator=(const PlaybackChannel&) = delete;

    void start() { worker_ = std::thread([this] { workerLoop(); }); }
    void stop() {
        quit_ = true;
        if (worker_.joinable()) worker_.join();
    }

    // Si posiziona e attende che siano pronti il fotogramma e il GOP adiacente nella
    // direzione di partenza. Va chiamata dallo stesso filo di tick().
    bool cue(int64_t frame, int direction = 1, int timeoutMs = 5000) {
        cursor_.seek(frame * 2);
        speed_ = 0;
        requested_ = 0;
        direction_ = direction < 0 ? -1 : 1;
        stats_.ended = false;
        publish();
        const int64_t length = provider_.gopLength();
        const int64_t neighbour = frame / length * length + direction_.load() * length;
        const bool hasNeighbour = neighbour >= 0 && neighbour < provider_.frameCount();
        // All'indietro il primo fotogramma usato del GOP adiacente è l'ultimo.
        const int64_t probe = direction_.load() > 0 ? neighbour : neighbour + length - 1;
        const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!find(frame) || (hasNeighbour && !find(probe))) {
            if (std::chrono::steady_clock::now() > limit) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    void setSpeed(int speed) {
        if (speed != 0) direction_ = speed < 0 ? -1 : 1;
        requested_ = speed;
    }

    // Ripete l'ultima uscita, senza muovere la posizione.
    void repeatLast() {
        if (!haveOutput_) return;
        output_.repeated = true;
        output_.audio.clear();
        sink_.output(output_);
    }

    // Restituisce true se è uscito un fotogramma nuovo.
    bool tick() {
        ++stats_.ticks;
        const int wanted = requested_.load();
        if (wanted != speed_) {
            speed_ = wanted;
            // A 1x ogni fotogramma d'uscita deve coincidere con un fotogramma sorgente.
            if (speed_ == kSpeedNormal) cursor_.seek(cursor_.field() & ~int64_t{1});
            else if (speed_ == -kSpeedNormal) cursor_.seek(cursor_.field() | 1);
        }
        FieldCursor next = cursor_;
        const int64_t fieldA = next.field();
        const bool okA = next.advance(speed_);
        const int64_t fieldB = next.field();
        const bool okB = next.advance(speed_);
        const auto a = find(fieldA / 2), b = find(fieldB / 2);
        if (!a || !b) {
            ++stats_.underruns;
            repeatLast();
            publish();
            return false;
        }
        const auto stride = static_cast<std::size_t>(provider_.width()) * 2;
        const auto height = static_cast<std::size_t>(provider_.height());
        output_.uyvy.resize(stride * height);
        composeField(output_.uyvy, a->uyvy, static_cast<int>(fieldA & 1), 0, stride, height);
        composeField(output_.uyvy, b->uyvy, static_cast<int>(fieldB & 1), 1, stride, height);
        output_.fieldA = fieldA;
        output_.fieldB = fieldB;
        output_.repeated = false;
        const bool wholeFrame = speed_ == kSpeedNormal && (fieldA & 1) == 0 && fieldB == fieldA + 1;
        if (wholeFrame) output_.audio = a->audio; else output_.audio.clear();
        haveOutput_ = true;
        sink_.output(output_);
        cursor_ = next;
        if (!okA || !okB) stats_.ended = true;
        publish();
        return true;
    }

    PlaybackStats stats() const {
        PlaybackStats s = stats_;
        s.decodeErrors = decodeErrors_.load();
        return s;
    }

private:
    using FramePtr = std::shared_ptr<DecodedFrame>;

    void publish() { target_ = cursor_.field(); }

    FramePtr find(int64_t frame) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = cache_.find(frame);
        return it == cache_.end() ? nullptr : it->second;
    }

    void workerLoop() {
        const int64_t length = provider_.gopLength();
        const int64_t lastGop = (provider_.frameCount() - 1) / length * length;
        while (!quit_) {
            const int64_t frame = target_.load() / 2;
            const int64_t direction = direction_.load();
            const int64_t current = frame / length * length;
            std::vector<int64_t> wanted{current};
            for (int64_t n = 1; n <= kLookaheadGops; ++n) wanted.push_back(current + direction * n * length);
            wanted.push_back(current - direction * length);  // il GOP appena passato serve al jog
            std::erase_if(wanted, [lastGop](int64_t g) { return g < 0 || g > lastGop; });

            evictExcept(wanted, length);
            bool decoded = false;
            for (const int64_t gop : wanted) {
                if (quit_ || loaded_.count(gop) || failed_.count(gop)) continue;
                const bool ok = provider_.decodeGop(gop, [this](FramePtr f) {
                    std::lock_guard<std::mutex> lock(m_);
                    cache_[f->index] = std::move(f);
                });
                if (ok) { loaded_.insert(gop); } else { failed_.insert(gop); ++decodeErrors_; }
                decoded = true;
                break;
            }
            if (!decoded) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void evictExcept(const std::vector<int64_t>& keep, int64_t length) {
        for (auto it = loaded_.begin(); it != loaded_.end();) {
            const int64_t gop = *it;
            if (std::find(keep.begin(), keep.end(), gop) != keep.end()) { ++it; continue; }
            {
                std::lock_guard<std::mutex> lock(m_);
                for (int64_t f = gop; f < gop + length; ++f) cache_.erase(f);
            }
            it = loaded_.erase(it);
        }
    }

    static constexpr int64_t kLookaheadGops = 2;
    IFrameProvider& provider_;
    IPlayoutSink& sink_;
    FieldCursor cursor_;
    int speed_ = 0;
    std::atomic<int> requested_{0};
    std::atomic<int> direction_{1};
    std::atomic<int64_t> target_{0};
    std::atomic<bool> quit_{false};
    std::atomic<int64_t> decodeErrors_{0};
    std::thread worker_;
    std::mutex m_;
    std::map<int64_t, FramePtr> cache_;
    std::set<int64_t> loaded_, failed_;  // usati solo dal filo di decodifica
    OutputFrame output_;
    bool haveOutput_ = false;
    PlaybackStats stats_;
};
}  // namespace vs
