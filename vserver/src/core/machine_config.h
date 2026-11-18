#pragma once
namespace vs {
// Configurazione della macchina: 1080i50, 4 ingressi, 2 uscite, 4 tracce audio.
struct MachineConfig {
    int inputs = 4;
    int outputs = 2;
    int audioTracks = 4;
    int width = 1920;
    int height = 1080;
    int framesPerSecond = 25;
};
}  // namespace vs
