# Video server 4 IN / 2 OUT

- `SPEC-funzionale.md`: specifica funzionale e stato del prototipo.
- `vserver/`: sorgenti C++ (registrazione XDCAM HD422 in MXF, riproduzione, catalogo SQLite, playlist), con scheda video simulata.
- `vserver/kit/`: script per la macchina di prova (inventario e prove).
- `prototipo/`: dati raccolti sul Dell PowerEdge R530.

## Kit di prova

A ogni push su `main` GitHub Actions compila su Windows e produce `kit-R530`:
Actions → Kit R530 → ultima esecuzione → Artifacts → `kit-R530`.
Si scarica come zip; istruzioni in `LEGGIMI.txt` dentro il kit.

## Compilazione locale (Windows)

Servono Visual Studio 2022 o successivo, CMake 3.25+, una build LGPL shared di FFmpeg 9.0 e
l'amalgamation di SQLite 3.54.1 (`sqlite3.c`, `sqlite3.h`) in `vserver/third_party/sqlite/`.

    cmake -S vserver -B build -A x64 -DFFMPEG_ROOT=C:/percorso/ffmpeg
    cmake --build build --config Release
