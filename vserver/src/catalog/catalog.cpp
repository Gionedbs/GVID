#include "catalog/catalog.h"

#include <mutex>
#include <variant>

#include "sqlite3.h"

namespace vs {
namespace {
constexpr int kSchemaVersion = 1;

const char* const kSchema = R"sql(
CREATE TABLE IF NOT EXISTS clips(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  path TEXT NOT NULL UNIQUE,
  channel INTEGER NOT NULL,
  start_tc INTEGER NOT NULL,
  frames INTEGER NOT NULL CHECK(frames > 0),
  repeated_frames INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_clips_name ON clips(name);
CREATE INDEX IF NOT EXISTS idx_clips_created ON clips(created_at);
CREATE INDEX IF NOT EXISTS idx_clips_tc ON clips(start_tc);
CREATE TABLE IF NOT EXISTS clip_keys(
  clip_id INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,
  value TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_keys_value ON clip_keys(value);
CREATE INDEX IF NOT EXISTS idx_keys_clip ON clip_keys(clip_id);
CREATE TABLE IF NOT EXISTS playlist_items(
  channel INTEGER NOT NULL,
  position INTEGER NOT NULL,
  clip_id INTEGER NOT NULL REFERENCES clips(id) ON DELETE RESTRICT,
  in_frame INTEGER NOT NULL,
  out_frame INTEGER NOT NULL,
  PRIMARY KEY(channel, position)
);
CREATE INDEX IF NOT EXISTS idx_playlist_clip ON playlist_items(clip_id);
)sql";

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) { rc_ = sqlite3_prepare_v2(db, sql.c_str(), -1, &s_, nullptr); }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    bool ok() const { return rc_ == SQLITE_OK; }
    void bind(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); }
    void bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.c_str(), -1, SQLITE_TRANSIENT); }
    int step() { return sqlite3_step(s_); }
    void reset() { sqlite3_reset(s_); sqlite3_clear_bindings(s_); }
    int64_t i64(int c) { return sqlite3_column_int64(s_, c); }
    std::string text(int c) {
        const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
        return t ? t : "";
    }
private:
    sqlite3_stmt* s_ = nullptr;
    int rc_ = SQLITE_ERROR;
};

// Trasforma il testo cercato in un pattern LIKE, neutralizzando i caratteri speciali.
std::string likeContains(const std::string& text) {
    std::string out = "%";
    for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    return out + "%";
}
}  // namespace

struct Catalog::Impl {
    sqlite3* db = nullptr;
    std::mutex m;
    std::string error;

    bool sqlFail() {
        error = db ? sqlite3_errmsg(db) : "catalogo non aperto";
        return false;
    }
    bool exec(const char* sql) {
        if (!db) return sqlFail();
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK ? true : sqlFail();
    }
    bool abort() {
        const std::string saved = db ? sqlite3_errmsg(db) : "catalogo non aperto";
        if (db) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        error = saved;
        return false;
    }

    std::vector<std::pair<std::string, std::string>> keysOf(int64_t id) {
        std::vector<std::pair<std::string, std::string>> keys;
        Stmt s(db, "SELECT kind, value FROM clip_keys WHERE clip_id=? ORDER BY rowid");
        s.bind(1, id);
        while (s.step() == SQLITE_ROW) keys.emplace_back(s.text(0), s.text(1));
        return keys;
    }

    ClipRecord read(Stmt& s) {
        ClipRecord c;
        c.id = s.i64(0);
        c.name = s.text(1);
        c.path = s.text(2);
        c.channel = static_cast<int>(s.i64(3));
        c.startTimecode = s.i64(4);
        c.frames = s.i64(5);
        c.repeatedFrames = s.i64(6);
        c.createdAt = s.text(7);
        return c;
    }
};

Catalog::Catalog() : impl_(std::make_unique<Impl>()) {}
Catalog::~Catalog() { close(); }
std::string Catalog::lastError() const { return impl_->error; }

void Catalog::close() {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->db) sqlite3_close(impl_->db);
    impl_->db = nullptr;
}

bool Catalog::open(const std::string& path) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (sqlite3_open(path.c_str(), &d.db) != SQLITE_OK) {
        d.sqlFail();
        sqlite3_close(d.db);
        d.db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(d.db, 5000);
    // WAL con sincronizzazione completa: ogni conferma è su disco prima di proseguire.
    if (!d.exec("PRAGMA journal_mode=WAL") || !d.exec("PRAGMA synchronous=FULL") || !d.exec("PRAGMA foreign_keys=ON")) return false;
    if (!d.exec(kSchema)) return false;
    Stmt version(d.db, "PRAGMA user_version");
    int64_t current = 0;
    if (version.step() == SQLITE_ROW) current = version.i64(0);
    if (current > kSchemaVersion) { d.error = "catalogo creato da una versione più recente"; return false; }
    return d.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
}

std::optional<int64_t> Catalog::addClip(const ClipRecord& clip) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.exec("BEGIN IMMEDIATE")) return std::nullopt;
    Stmt s(d.db, "INSERT INTO clips(name,path,channel,start_tc,frames,repeated_frames,created_at) VALUES(?,?,?,?,?,?,?)");
    if (!s.ok()) { d.abort(); return std::nullopt; }
    s.bind(1, clip.name);
    s.bind(2, clip.path);
    s.bind(3, int64_t{clip.channel});
    s.bind(4, clip.startTimecode);
    s.bind(5, clip.frames);
    s.bind(6, clip.repeatedFrames);
    s.bind(7, clip.createdAt);
    if (s.step() != SQLITE_DONE) { d.abort(); return std::nullopt; }
    const int64_t id = sqlite3_last_insert_rowid(d.db);
    Stmt k(d.db, "INSERT INTO clip_keys(clip_id,kind,value) VALUES(?,?,?)");
    if (!k.ok()) { d.abort(); return std::nullopt; }
    for (const auto& [kind, value] : clip.keys) {
        k.reset();
        k.bind(1, id);
        k.bind(2, kind);
        k.bind(3, value);
        if (k.step() != SQLITE_DONE) { d.abort(); return std::nullopt; }
    }
    if (!d.exec("COMMIT")) { d.abort(); return std::nullopt; }
    return id;
}

std::optional<ClipRecord> Catalog::getClip(int64_t id) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.db) { d.sqlFail(); return std::nullopt; }
    Stmt s(d.db, "SELECT id,name,path,channel,start_tc,frames,repeated_frames,created_at FROM clips WHERE id=?");
    s.bind(1, id);
    if (s.step() != SQLITE_ROW) { d.error = "clip non trovata"; return std::nullopt; }
    ClipRecord c = d.read(s);
    c.keys = d.keysOf(c.id);
    return c;
}

std::vector<ClipRecord> Catalog::search(const ClipQuery& q) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    std::vector<ClipRecord> out;
    if (!d.db) { d.sqlFail(); return out; }
    std::string sql = "SELECT id,name,path,channel,start_tc,frames,repeated_frames,created_at FROM clips WHERE 1=1";
    std::vector<std::variant<int64_t, std::string>> args;
    if (!q.nameContains.empty()) {
        sql += " AND name LIKE ? ESCAPE '\\'";
        args.emplace_back(likeContains(q.nameContains));
    }
    if (!q.keyContains.empty() || !q.keyKind.empty()) {
        sql += " AND id IN (SELECT clip_id FROM clip_keys WHERE 1=1";
        if (!q.keyContains.empty()) { sql += " AND value LIKE ? ESCAPE '\\'"; args.emplace_back(likeContains(q.keyContains)); }
        if (!q.keyKind.empty()) { sql += " AND kind=?"; args.emplace_back(q.keyKind); }
        sql += ")";
    }
    if (q.channel) { sql += " AND channel=?"; args.emplace_back(int64_t{*q.channel}); }
    if (!q.createdFrom.empty()) { sql += " AND created_at>=?"; args.emplace_back(q.createdFrom); }
    if (!q.createdTo.empty()) { sql += " AND created_at<=?"; args.emplace_back(q.createdTo); }
    if (q.containsTimecode) {
        sql += " AND start_tc<=? AND ?<start_tc+frames";
        args.emplace_back(*q.containsTimecode);
        args.emplace_back(*q.containsTimecode);
    }
    sql += " ORDER BY id";
    Stmt s(d.db, sql);
    if (!s.ok()) { d.sqlFail(); return out; }
    int index = 1;
    for (const auto& a : args) {
        std::visit([&](const auto& v) { s.bind(index, v); }, a);
        ++index;
    }
    while (s.step() == SQLITE_ROW) {
        ClipRecord c = d.read(s);
        c.keys = d.keysOf(c.id);
        out.push_back(std::move(c));
    }
    return out;
}

bool Catalog::removeClip(int64_t id) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.db) return d.sqlFail();
    Stmt s(d.db, "DELETE FROM clips WHERE id=?");
    s.bind(1, id);
    if (s.step() != SQLITE_DONE) return d.sqlFail();
    if (sqlite3_changes(d.db) == 0) { d.error = "clip non trovata"; return false; }
    return true;
}

bool Catalog::savePlaylist(int channel, const Playlist& playlist, const std::function<void()>& beforeCommit) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.exec("BEGIN IMMEDIATE")) return false;
    {
        Stmt del(d.db, "DELETE FROM playlist_items WHERE channel=?");
        del.bind(1, int64_t{channel});
        if (del.step() != SQLITE_DONE) return d.abort();
        Stmt ins(d.db, "INSERT INTO playlist_items(channel,position,clip_id,in_frame,out_frame) VALUES(?,?,?,?,?)");
        if (!ins.ok()) return d.abort();
        for (std::size_t i = 0; i < playlist.size(); ++i) {
            const PlaylistItem& it = playlist.at(i);
            ins.reset();
            ins.bind(1, int64_t{channel});
            ins.bind(2, static_cast<int64_t>(i));
            ins.bind(3, static_cast<int64_t>(it.clipId));
            ins.bind(4, it.inFrame);
            ins.bind(5, it.outFrame);
            if (ins.step() != SQLITE_DONE) return d.abort();
        }
    }
    if (beforeCommit) beforeCommit();
    return d.exec("COMMIT") ? true : d.abort();
}

bool Catalog::loadPlaylist(int channel, Playlist& out) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.db) return d.sqlFail();
    Stmt s(d.db, "SELECT clip_id,in_frame,out_frame FROM playlist_items WHERE channel=? ORDER BY position");
    if (!s.ok()) return d.sqlFail();
    s.bind(1, int64_t{channel});
    Playlist loaded;
    int rc;
    while ((rc = s.step()) == SQLITE_ROW) {
        if (loaded.append({static_cast<uint64_t>(s.i64(0)), s.i64(1), s.i64(2)}) != PlaylistError::None) {
            d.error = "elemento di playlist non valido";
            return false;
        }
    }
    if (rc != SQLITE_DONE) return d.sqlFail();
    out = std::move(loaded);
    return true;
}

std::string Catalog::integrityCheck() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.db) return "catalogo non aperto";
    Stmt s(d.db, "PRAGMA integrity_check");
    std::string result;
    while (s.step() == SQLITE_ROW) result += s.text(0);
    Stmt fk(d.db, "PRAGMA foreign_key_check");
    if (fk.step() == SQLITE_ROW) result += " | riferimenti non validi";
    return result;
}
}  // namespace vs
