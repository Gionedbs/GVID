// Prova di interruzione brusca del catalogo.
//   catalog_crash write <db> <seme>   scrive di continuo e termina il processo a caso
//   catalog_crash verify <db> <ultimo id confermato>
// La scrittura stampa ogni id solo dopo la conferma della transazione.
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>

#include "catalog/catalog.h"

using namespace vs;

namespace {
[[noreturn]] void killNow() {
    TerminateProcess(GetCurrentProcess(), 9);
    std::abort();
}

ClipRecord clip(int64_t n) {
    ClipRecord c;
    c.name = "clip " + std::to_string(n);
    c.path = "file-" + std::to_string(n) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(GetCurrentProcessId()) + ".mxf";
    c.frames = 25;
    c.createdAt = "2026-11-09T12:00:00";
    c.keys = {{"prova", std::to_string(n)}, {"prova", "doppia"}};
    return c;
}

int write(const std::string& db, unsigned seed) {
    Catalog cat;
    if (!cat.open(db)) { std::printf("ERRORE %s\n", cat.lastError().c_str()); return 3; }
    std::mt19937 rng(seed);
    const int rounds = 5 + static_cast<int>(rng() % 40);
    const int killKind = static_cast<int>(rng() % 3);  // 0 dentro la transazione, 1 dopo la conferma, 2 tra le operazioni
    for (int i = 0; i < rounds + 1; ++i) {
        const auto id = cat.addClip(clip(i));
        if (!id) { std::printf("ERRORE %s\n", cat.lastError().c_str()); return 3; }
        std::printf("%lld\n", static_cast<long long>(*id));
        std::fflush(stdout);

        // Tutti gli elementi di una generazione hanno lo stesso in_frame.
        Playlist pl;
        const int count = 2 + static_cast<int>(rng() % 30);
        const int64_t generation = i + static_cast<int64_t>(seed) * 1000;
        for (int k = 0; k < count; ++k) pl.append({static_cast<uint64_t>(*id), generation, generation + 1});
        const bool last = i == rounds;
        const bool ok = cat.savePlaylist(0, pl, [&] { if (last && killKind == 0) killNow(); });
        if (!ok) { std::printf("ERRORE %s\n", cat.lastError().c_str()); return 3; }
        if (last && killKind == 1) killNow();
    }
    killNow();
}

int verify(const std::string& db, int64_t lastCommitted) {
    Catalog cat;
    if (!cat.open(db)) { std::printf("ERRORE apertura: %s\n", cat.lastError().c_str()); return 1; }
    const std::string integrity = cat.integrityCheck();
    if (integrity != "ok") { std::printf("ERRORE integrita: %s\n", integrity.c_str()); return 1; }

    std::set<int64_t> ids;
    for (const auto& c : cat.search({})) {
        ids.insert(c.id);
        if (c.keys.size() != 2) { std::printf("ERRORE clip %lld incompleta: %zu chiavi\n", static_cast<long long>(c.id), c.keys.size()); return 1; }
    }
    for (int64_t i = 1; i <= lastCommitted; ++i) {
        if (!ids.count(i)) { std::printf("ERRORE clip confermata %lld persa\n", static_cast<long long>(i)); return 1; }
    }
    // Può esistere al massimo una clip confermata ma non ancora stampata.
    if (!ids.empty() && *ids.rbegin() > lastCommitted + 1) { std::puts("ERRORE clip inattese"); return 1; }

    Playlist pl;
    if (!cat.loadPlaylist(0, pl)) { std::printf("ERRORE playlist: %s\n", cat.lastError().c_str()); return 1; }
    for (std::size_t i = 1; i < pl.size(); ++i) {
        if (pl.at(i).inFrame != pl.at(0).inFrame) { std::puts("ERRORE playlist parziale"); return 1; }
    }
    std::printf("OK clip %zu ultima confermata %lld playlist %zu\n", ids.size(), static_cast<long long>(lastCommitted), pl.size());
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "write") return write(argv[2], static_cast<unsigned>(std::atoi(argv[3])));
    if (argc == 4 && std::string(argv[1]) == "verify") return verify(argv[2], std::atoll(argv[3]));
    std::puts("uso: catalog_crash write <db> <seme> | verify <db> <ultimo id>");
    return 2;
}
