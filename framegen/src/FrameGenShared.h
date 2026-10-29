// Protocol between RenderHook (32-bit game, D3D11) and RHFrameGen64.exe (64-bit helper, D3D12) that presents the
// frames in its own window over the game: the base for frame generation, which needs D3D12 and 64-bit (Streamline).
// Keep in sync with the helper's copy; kVersion guards against a mismatch.
#pragma once
#include <cstdint>
#include <cstdio>

namespace rhfg {

const uint32_t kVersion = 6;
const uint32_t kSlots   = 3; // shared frame textures, used round-robin

enum : uint32_t { StateStarting, StateReady, StateFailed };

// Reflex events from the game's frame loop, forwarded by the helper to Streamline right away (sl::PCLMarker values,
// plus MarkerSleep: slReflexSleep, after which the helper sets the "slept" event the game waits for)
const uint32_t kMarkers = 64;
enum : uint32_t
{
    MarkerSimulationStart    = 0,
    MarkerSimulationEnd      = 1,
    MarkerRenderSubmitStart  = 2,
    MarkerRenderSubmitEnd    = 3,
    MarkerSleep              = 100,
};
struct MarkerEvent
{
    uint32_t frameIndex; // the game's frame, the same number the frame is presented under
    uint32_t marker;
};

// Camera of the frame in a slot, for frame generation. D3D convention (row vectors, row-major, v * M), without the
// TAA jitter; prev* is the game's previous frame, which the motion vectors point to.
struct Camera
{
    float view[16], proj[16];
    float prevView[16], prevProj[16];
    uint32_t reset; // 1: no usable previous frame (first frame, camera cut/teleport)
    float pad[3];
};

struct Shared
{
    uint32_t version;
    volatile uint32_t state;   // helper: StateReady once its D3D12 device and window are up
    char message[256];         // helper: what went wrong
    uint32_t adapterLuidLow;   // game: adapter of its D3D11 device
    int32_t  adapterLuidHigh;
    uint64_t gameWindow;       // game: HWND the helper's window covers
    int32_t  vsync;            // game: sync interval for the helper's Present (0 = tearing allowed if supported)

    // game: set before configGeneration is raised; handles are valid in the helper process
    uint32_t width, height, format; // DXGI_FORMAT of the frames
    uint64_t hFrame[kSlots];        // final frame, HUD included (what the game would present)
    uint64_t hDepth[kSlots];        // R32_FLOAT hardware-style depth (0 near .. 1 far/sky), from the camera without jitter
    uint64_t hMotion[kSlots];       // R16G16_FLOAT motion in pixels, current -> previous frame, camera and objects
    uint64_t hHudless[kSlots];      // the frame before the HUD (CHudLayer), format of hFrame
    uint64_t hUi[kSlots];           // the HUD layer, R8G8B8A8_UNORM premultiplied: frame = ui.rgb + (1 - ui.a) * hudless
    uint32_t uiFormat;
    uint64_t hReadyFence;           // game signals frame n when slot n % kSlots holds it
    uint64_t hDoneFence;            // helper signals frame n once it's copied out of its slot
    volatile uint32_t configGeneration;

    // game, per frame: frame n (1, 2, ...) is in slot n % kSlots, ready on the GPU at hReadyFence >= n
    volatile uint64_t frame;
    volatile int64_t  publishQpc[kSlots];  // QueryPerformanceCounter when the frame was published
    Camera camera[kSlots];
    volatile uint32_t droppedFrames;       // game: frames skipped because the helper still held every slot

    // Reflex: the game's frame index of the frame in each slot, and the marker queue (game writes head, helper tail)
    volatile uint32_t slotFrameIndex[kSlots];
    volatile uint32_t slotHasHud[kSlots]; // 1: hHudless/hUi of this slot hold the frame's HUD-less image and HUD layer
    volatile int64_t  frameStartQpc[kSlots]; // QPC at the start of the game's frame (after Reflex's sleep, before input)
    // back-pressure: the frame the helper's presenting thread took last ("picked" event); the game starts its next
    // frame only once its previous one is taken, so it never renders frames that frame generation would skip
    volatile uint64_t pickedFrame;
    MarkerEvent markers[kMarkers];
    volatile uint32_t markerHead, markerTail;
    volatile uint32_t reflexReady;   // helper: its marker thread and Streamline's Reflex are up
    volatile uint32_t gameMarkers;   // game: sends its own markers (otherwise the helper marks frames on arrival)
};

inline void Names( uint32_t pid, char *mapping, char *frameEvent, char *configEvent, size_t size )
{
    snprintf( mapping, size, "Local\\RHFG_%u_map", pid );
    snprintf( frameEvent, size, "Local\\RHFG_%u_frame", pid );
    snprintf( configEvent, size, "Local\\RHFG_%u_config", pid );
}

inline void ReflexNames( uint32_t pid, char *markerEvent, char *sleptEvent, size_t size )
{
    snprintf( markerEvent, size, "Local\\RHFG_%u_marker", pid );
    snprintf( sleptEvent, size, "Local\\RHFG_%u_slept", pid );
}

inline void PickedName( uint32_t pid, char *pickedEvent, size_t size )
{
    snprintf( pickedEvent, size, "Local\\RHFG_%u_picked", pid );
}

} // namespace rhfg
