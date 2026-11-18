#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "play/playback_channel.h"
namespace vs {
struct PlayoutEntry {
    std::string path;
    int64_t inFrame = 0;
    int64_t outFrame = 0;  // escluso
};

// Restituisce nullptr se la clip non si può aprire.
using ProviderFactory = std::function<std::unique_ptr<IFrameProvider>(const std::string& path)>;

// Riproduce una sequenza a taglio netto. Mentre una clip è in onda la successiva viene
// preparata su un secondo lettore; il cambio avviene sul fotogramma esatto. Se la
// successiva non è pronta l'uscita ripete l'ultimo fotogramma e il ritardo viene contato.
// Se una clip non è utilizzabile la sequenza si ferma sull'ultimo fotogramma con un errore.
class PlaylistPlayer {
public:
    PlaylistPlayer(ProviderFactory factory, IPlayoutSink& sink) : factory_(std::move(factory)), sink_(sink) {}

    ~PlaylistPlayer() {
        if (preroll_.joinable()) preroll_.join();
        for (auto& t : retired_) if (t.joinable()) t.join();
    }
    PlaylistPlayer(const PlaylistPlayer&) = delete;
    PlaylistPlayer& operator=(const PlaylistPlayer&) = delete;

    bool start(std::vector<PlayoutEntry> entries) {
        entries_ = std::move(entries);
        if (entries_.empty()) { setError("sequenza vuota"); finished_ = true; return false; }
        current_ = prepare(entries_[0]);
        if (!current_) { finished_ = true; return false; }
        goLive(0);
        return true;
    }

    // Produce un fotogramma d'uscita. Restituisce false a sequenza terminata.
    bool tick() {
        if (finished_) { hold(); return false; }
        if (remaining_ == 0) {
            if (index_ + 1 >= entries_.size()) {
                finished_ = true;
                hold();
                return false;
            }
            if (!prerollDone_.load()) {
                ++lateCuts_;
                hold();
                return true;
            }
            preroll_.join();
            std::unique_ptr<Deck> next = std::move(prepared_);
            if (!next) {
                finished_ = true;
                hold();
                return false;
            }
            retire(std::move(current_));
            current_ = std::move(next);
            ++cuts_;
            goLive(index_ + 1);
        }
        if (current_->channel->tick()) {
            --remaining_;
            ++frames_;
        }
        return true;
    }

    int64_t frames() const { return frames_; }
    int64_t cuts() const { return cuts_; }
    int64_t lateCuts() const { return lateCuts_; }
    int64_t underruns() const { return current_ ? underrunsBefore_ + current_->channel->stats().underruns : underrunsBefore_; }
    std::string error() {
        std::lock_guard<std::mutex> lock(m_);
        return error_;
    }

private:
    struct Deck {
        std::unique_ptr<IFrameProvider> provider;
        std::unique_ptr<PlaybackChannel> channel;
    };

    void setError(const std::string& text) {
        std::lock_guard<std::mutex> lock(m_);
        if (error_.empty()) error_ = text;
    }

    std::unique_ptr<Deck> prepare(const PlayoutEntry& e) {
        auto deck = std::make_unique<Deck>();
        deck->provider = factory_(e.path);
        if (!deck->provider) { setError("clip non apribile: " + e.path); return nullptr; }
        if (e.inFrame < 0 || e.outFrame <= e.inFrame || e.outFrame > deck->provider->frameCount()) {
            setError("punti IN/OUT non validi: " + e.path);
            return nullptr;
        }
        deck->channel = std::make_unique<PlaybackChannel>(*deck->provider, sink_);
        deck->channel->start();
        if (!deck->channel->cue(e.inFrame)) { setError("preparazione fallita: " + e.path); return nullptr; }
        return deck;
    }

    void goLive(std::size_t index) {
        index_ = index;
        remaining_ = entries_[index].outFrame - entries_[index].inFrame;
        current_->channel->setSpeed(kSpeedNormal);
        if (index + 1 < entries_.size()) {
            prerollDone_ = false;
            preroll_ = std::thread([this, next = entries_[index + 1]] {
                prepared_ = prepare(next);
                prerollDone_ = true;
            });
        }
    }

    void hold() {
        if (current_) current_->channel->repeatLast();
    }

    // Fermare un lettore può richiedere decine di millisecondi: non deve pesare sull'uscita.
    void retire(std::unique_ptr<Deck> deck) {
        underrunsBefore_ += deck->channel->stats().underruns;
        retired_.emplace_back([d = std::shared_ptr<Deck>(std::move(deck))]() mutable { d.reset(); });
    }

    ProviderFactory factory_;
    IPlayoutSink& sink_;
    std::vector<PlayoutEntry> entries_;
    std::unique_ptr<Deck> current_, prepared_;
    std::size_t index_ = 0;
    int64_t remaining_ = 0, frames_ = 0, cuts_ = 0, lateCuts_ = 0, underrunsBefore_ = 0;
    bool finished_ = false;
    std::atomic<bool> prerollDone_{true};
    std::thread preroll_;
    std::vector<std::thread> retired_;
    std::mutex m_;
    std::string error_;
};
}  // namespace vs
