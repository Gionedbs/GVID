#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "core/playlist.h"
namespace vs {
struct ClipRecord {
    int64_t id = 0;
    std::string name;
    std::string path;
    int channel = 0;
    int64_t startTimecode = 0;
    int64_t frames = 0;
    int64_t repeatedFrames = 0;  // fotogrammi coperti da ripetizioni durante la registrazione
    std::string createdAt;       // ISO 8601
    std::vector<std::pair<std::string, std::string>> keys;  // tipo (squadra, evento...) e valore
};

struct ClipQuery {
    std::string nameContains;
    std::string keyContains;
    std::string keyKind;
    std::optional<int> channel;
    std::string createdFrom;  // ISO 8601, incluso
    std::string createdTo;    // ISO 8601, incluso
    std::optional<int64_t> containsTimecode;
};

// Catalogo su SQLite. Ogni operazione è una transazione: dopo un'interruzione
// resta lo stato dell'ultima conferma. Utilizzabile da più fili.
class Catalog {
public:
    Catalog();
    ~Catalog();
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    bool open(const std::string& path);
    void close();

    std::optional<int64_t> addClip(const ClipRecord& clip);
    std::optional<ClipRecord> getClip(int64_t id);
    std::vector<ClipRecord> search(const ClipQuery& query);
    // Rifiutata se la clip è usata da una playlist.
    bool removeClip(int64_t id);

    // beforeCommit serve alle prove di interruzione.
    bool savePlaylist(int channel, const Playlist& playlist, const std::function<void()>& beforeCommit = {});
    bool loadPlaylist(int channel, Playlist& out);

    std::string integrityCheck();
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace vs
