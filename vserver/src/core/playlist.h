#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace vs {
inline constexpr std::size_t kPlaylistCapacity = 1000;

struct PlaylistItem {
    uint64_t clipId = 0;
    int64_t inFrame = 0;
    int64_t outFrame = 0;  // escluso
    bool valid() const { return inFrame >= 0 && inFrame < outFrame; }
};

enum class PlaylistError { None, Empty, Full, OutOfRange, InvalidItem };

struct LoadResult {
    PlaylistError error = PlaylistError::None;
    std::size_t index = 0;
    bool endOfList = false;
    int64_t startField = 0;
};

class Playlist {
public:
    std::size_t size() const { return items_.size(); }
    const PlaylistItem& at(std::size_t i) const { return items_.at(i); }

    PlaylistError insert(std::size_t position, const PlaylistItem& item) {
        if (!item.valid()) return PlaylistError::InvalidItem;
        if (items_.size() >= kPlaylistCapacity) return PlaylistError::Full;
        if (position > items_.size()) return PlaylistError::OutOfRange;
        items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(position), item);
        return PlaylistError::None;
    }

    PlaylistError append(const PlaylistItem& item) { return insert(items_.size(), item); }

    PlaylistError remove(std::size_t position) {
        if (items_.empty()) return PlaylistError::Empty;
        if (position >= items_.size()) return PlaylistError::OutOfRange;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(position));
        return PlaylistError::None;
    }

    PlaylistError move(std::size_t from, std::size_t to) {
        if (items_.empty()) return PlaylistError::Empty;
        if (from >= items_.size() || to >= items_.size()) return PlaylistError::OutOfRange;
        const PlaylistItem item = items_[from];
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(from));
        items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(to), item);
        return PlaylistError::None;
    }

    PlaylistError setInOut(std::size_t position, int64_t inFrame, int64_t outFrame) {
        if (position >= items_.size()) return PlaylistError::OutOfRange;
        PlaylistItem changed = items_[position];
        changed.inFrame = inFrame;
        changed.outFrame = outFrame;
        if (!changed.valid()) return PlaylistError::InvalidItem;
        items_[position] = changed;
        return PlaylistError::None;
    }

    // Un indice pari alla lunghezza porta sull'ultimo elemento e segnala la fine.
    LoadResult load(std::size_t index, int64_t offsetFrames = 0) const {
        LoadResult r;
        if (items_.empty()) { r.error = PlaylistError::Empty; return r; }
        if (index > items_.size()) { r.error = PlaylistError::OutOfRange; return r; }
        r.endOfList = index == items_.size();
        r.index = r.endOfList ? index - 1 : index;
        const PlaylistItem& item = items_[r.index];
        if (offsetFrames < 0 || offsetFrames >= item.outFrame - item.inFrame) {
            r.error = PlaylistError::OutOfRange;
            return r;
        }
        r.startField = (item.inFrame + offsetFrames) * 2;
        return r;
    }

private:
    std::vector<PlaylistItem> items_;
};
}  // namespace vs
