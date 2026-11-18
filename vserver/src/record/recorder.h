#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "core/timecode.h"
#include "hal/capture.h"
namespace vs {
class IFrameSink {
public:
    virtual ~IFrameSink() = default;
    virtual bool write(const CapturedFrame& frame) = 0;
    virtual bool close() = 0;
    virtual std::string lastError() const = 0;
};

struct RecordingStats {
    int64_t captured = 0;
    int64_t written = 0;   // fotogrammi nel file, ripetizioni comprese
    int64_t dropped = 0;   // fotogrammi persi per buffer pieno
    int64_t repeated = 0;  // ripetizioni scritte per compensare i buchi
    int64_t timecodeBreaks = 0;
    int64_t timecodeMissing = 0;
    std::size_t maxQueued = 0;
    bool ok = true;
    std::string error;
};

// Un filo cattura, uno scrive. La cattura non si blocca mai: se il buffer è pieno
// il fotogramma viene perso e conteggiato, così un disco lento non ferma l'ingresso.
// Nel file ogni buco è coperto ripetendo l'immagine precedente con audio muto, così
// durata e timecode restano corretti. Le ripetizioni passano comunque dall'encoder:
// con un sovraccarico prolungato la chiusura del file arriva in ritardo.
class ChannelRecorder {
public:
    ChannelRecorder(ICaptureSource& source, IFrameSink& sink, std::size_t bufferFrames = 25)
        : source_(source), sink_(sink) {
        // Un buffer in più resta sempre occupato dall'ultimo fotogramma scritto.
        for (std::size_t i = 0; i < bufferFrames + 1; ++i) free_.push_back(std::make_unique<CapturedFrame>());
    }
    ~ChannelRecorder() { requestStop(); join(); }
    ChannelRecorder(const ChannelRecorder&) = delete;
    ChannelRecorder& operator=(const ChannelRecorder&) = delete;

    void start() {
        writer_ = std::thread([this] { writeLoop(); });
        capturer_ = std::thread([this] { captureLoop(); });
    }
    void requestStop() { stop_ = true; }

    RecordingStats join() {
        if (capturer_.joinable()) capturer_.join();
        if (writer_.joinable()) writer_.join();
        return stats_;
    }

private:
    void captureLoop() {
        CapturedFrame scratch;
        int64_t captured = 0, dropped = 0, lastSequence = -1;
        while (!stop_) {
            std::unique_ptr<CapturedFrame> frame;
            {
                std::lock_guard<std::mutex> lock(m_);
                if (!free_.empty()) { frame = std::move(free_.back()); free_.pop_back(); }
            }
            if (!frame) {
                if (!source_.capture(scratch)) break;
                ++captured;
                ++dropped;
                lastSequence = scratch.sequence;
                continue;
            }
            if (!source_.capture(*frame)) break;
            ++captured;
            lastSequence = frame->sequence;
            {
                std::lock_guard<std::mutex> lock(m_);
                queue_.push_back(std::move(frame));
            }
            cv_.notify_one();
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            stats_.captured = captured;
            stats_.dropped = dropped;
            lastCapturedSequence_ = lastSequence;
            done_ = true;
        }
        cv_.notify_one();
    }

    void writeLoop() {
        std::unique_ptr<CapturedFrame> last;  // base per le ripetizioni
        bool failed = false;

        const auto writeOne = [&](const CapturedFrame& f) {
            if (failed) return false;
            if (sink_.write(f)) {
                ++stats_.written;
                return true;
            }
            failed = true;
            stats_.ok = false;
            stats_.error = sink_.lastError();
            stop_ = true;
            return false;
        };

        // Scrive ripetizioni fino al fotogramma precedente a "sequence".
        const auto fillBefore = [&](int64_t sequence) {
            if (!last) return;
            while (!failed && last->sequence + 1 < sequence) {
                ++last->sequence;
                if (last->timecode) last->timecode = wrapFrames(*last->timecode + 1);
                for (auto& track : last->audio) std::fill(track.begin(), track.end(), 0);
                if (writeOne(*last)) ++stats_.repeated;
            }
        };

        for (;;) {
            std::unique_ptr<CapturedFrame> frame;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return !queue_.empty() || done_; });
                if (queue_.empty()) break;
                if (queue_.size() > stats_.maxQueued) stats_.maxQueued = queue_.size();
                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!frame->timecode) {
                ++stats_.timecodeMissing;
            } else if (last && last->timecode) {
                // Un buco dovuto a fotogrammi persi non è un salto del timecode in ingresso.
                const int64_t expected = wrapFrames(*last->timecode + (frame->sequence - last->sequence));
                if (expected != wrapFrames(*frame->timecode)) ++stats_.timecodeBreaks;
            }
            fillBefore(frame->sequence);
            writeOne(*frame);
            std::unique_ptr<CapturedFrame> previous = std::move(last);
            last = std::move(frame);
            if (previous) {
                std::lock_guard<std::mutex> lock(m_);
                free_.push_back(std::move(previous));
            }
        }
        // I fotogrammi persi in coda alla registrazione vanno coperti anch'essi.
        fillBefore(lastCapturedSequence_ + 1);
        if (!sink_.close() && stats_.ok) {
            stats_.ok = false;
            stats_.error = sink_.lastError();
        }
    }

    ICaptureSource& source_;
    IFrameSink& sink_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<CapturedFrame>> queue_;
    std::vector<std::unique_ptr<CapturedFrame>> free_;
    std::atomic<bool> stop_{false};
    bool done_ = false;
    int64_t lastCapturedSequence_ = -1;
    RecordingStats stats_;
    std::thread capturer_, writer_;
};
}  // namespace vs
