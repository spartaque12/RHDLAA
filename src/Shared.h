// Protocol between RHDLAA.asi (inside the 32-bit game) and RHDLAA64.exe (64-bit helper that hosts NVIDIA NGX).
// NVIDIA ships the DLSS runtime for x64 only, so DLAA runs in the helper on its own D3D11 device. The textures and
// the fence are D3D11 shared NT handles; the game duplicates them into the helper and passes the values here.
#pragma once
#include <cstdint>

namespace rhdlaa {

const uint32_t kVersion = 1;

enum : uint32_t { CmdNone, CmdCreate, CmdEvaluate };
enum : uint32_t { StateStarting, StateReady, StateFailed };

struct Shared {
    uint32_t version;
    volatile uint32_t state;        // helper: StateReady once NGX is up and DLSS is available
    char message[512];              // helper: what went wrong, or the DLSS version on success
    uint32_t adapterLuidLow;        // game: adapter of its D3D11 device
    int32_t adapterLuidHigh;
    int32_t preset;                 // game: NVSDK_NGX_DLSS_Hint_Render_Preset (0 = NVIDIA's default)

    uint32_t cmd;                   // game: what the next request is
    uint32_t result;                // helper: 1 = ok

    // CmdCreate: handles valid in the helper process (it closes them after opening)
    uint64_t hColor, hDepth, hMotion, hOutput, hFence;
    uint32_t width, height;

    // CmdEvaluate: helper GPU-waits fenceWait, runs DLAA, signals fenceSignal
    uint64_t fenceWait, fenceSignal;
    float jitterX, jitterY;         // pixels
    int32_t reset;
    float frameTimeMs;
};

inline void Names(uint32_t pid, char* mapping, char* request, char* done, size_t size)
{
    snprintf(mapping, size, "Local\\RHDLAA_%u_map", pid);
    snprintf(request, size, "Local\\RHDLAA_%u_req", pid);
    snprintf(done, size, "Local\\RHDLAA_%u_done", pid);
}

}
