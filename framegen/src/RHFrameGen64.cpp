// RHFrameGen64 - 64-bit D3D12 helper for RenderHook's frame generation bridge (step 1: presentation only).
// Started by RenderHook with the game's process id. Opens the frames RenderHook shares (D3D11 shared NT handles) and
// presents them through its own flip-model swapchain, in a borderless, click-through, topmost window over the game
// window's client area. The game keeps focus and input. F5 shows/hides the window (A/B against the game's own
// output). Logs how long frames take from RenderHook to Present, and exits with the game.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <share.h>
#include <cmath>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include "FrameGenShared.h"

namespace {

FILE* g_log;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

rhfg::Shared* g_shared;
IDXGIFactory4* g_factory;
ID3D12Device* g_device;
ID3D12CommandQueue* g_queue;
ID3D12GraphicsCommandList* g_list;
const UINT kBuffers = 3;
ID3D12CommandAllocator* g_allocators[kBuffers];
UINT64 g_allocatorFence[kBuffers];
ID3D12Fence* g_localFence;
UINT64 g_localValue;
HANDLE g_localEvent, g_readyEvent;
IDXGISwapChain3* g_swapChain;
ID3D12Resource* g_backBuffers[kBuffers];
ID3D12Resource* g_frames[rhfg::kSlots];
ID3D12Resource* g_depth[rhfg::kSlots];
ID3D12Resource* g_motion[rhfg::kSlots];
ID3D12Resource* g_hudless[rhfg::kSlots];
ID3D12Resource* g_ui[rhfg::kSlots];
// Receiving (IngestFrames thread): every frame is copied out of its shared slot at once, on the copy queue, into our
// own textures, and the slot goes back to the game. Presenting reads only our copies, so a Present that Streamline
// holds for frame pacing never keeps the game's slots busy (the game drops frames when all slots are busy).
ID3D12CommandQueue* g_copyQueue;
ID3D12CommandAllocator* g_copyAllocators[rhfg::kSlots];
ID3D12GraphicsCommandList* g_copyList;
ID3D12Fence* g_copyFence;
UINT64 g_copyValue;
HANDLE g_copyEvent, g_ingestedEvent;
ID3D12Resource* g_ownColor[rhfg::kSlots];
ID3D12Resource* g_ownDepth[rhfg::kSlots];
ID3D12Resource* g_ownMotion[rhfg::kSlots];
ID3D12Resource* g_ownHudless[rhfg::kSlots];
ID3D12Resource* g_ownUi[rhfg::kSlots];
struct Received
{
    uint64_t frame;          // 0: nothing
    UINT64 copyValue;        // g_copyFence value once our copy is done
    uint32_t frameIndex;     // the game's frame index (Reflex)
    int64_t published;
    rhfg::Camera camera;     // snapshot: the game reuses the slot as soon as it's free
    bool hasHud;             // our HUD-less and HUD layer copies belong to this frame
    int64_t frameStart;      // QPC at the start of the game's frame
};
Received g_received[rhfg::kSlots];
UINT64 g_ownBusyUntil[rhfg::kSlots];  // g_localFence value after which presenting no longer reads our copy
volatile uint64_t g_latestReceived;
uint64_t g_receiveNext = 1;
bool g_receiving;                      // false while (re)configuring
CRITICAL_SECTION g_slotLock;           // receiving vs presenting vs Configure
uint32_t g_gamePid;
// with RHFrameGen64.timing next to the exe: per presented frame the game's frame start and our Present call (QPC),
// to pair with PresentMon's display times (C:\rhs\tools\presentmon)
FILE* g_timing;
// Streamline / DLSS Frame Generation (F11 switches it on and off; keys in RHFrameGen64.ini)
bool g_sl, g_fgSupported, g_fgWanted = true, g_fgApplied;
const sl::ViewportHandle g_viewport{ 0u };
// slGetNewFrameToken isn't thread safe: the presenting and the marker thread both need tokens
CRITICAL_SECTION g_tokenLock;
HANDLE g_markerEvent, g_sleptEvent, g_pickedEvent;

sl::FrameToken* FrameToken(uint32_t frameIndex)
{
    sl::FrameToken* token = nullptr;
    EnterCriticalSection(&g_tokenLock);
    if (slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk) token = nullptr;
    LeaveCriticalSection(&g_tokenLock);
    return token;
}

// Reflex events from the game's frame loop (rhfg::MarkerEvent queue), passed to Streamline as they come: the markers
// get Streamline's timestamps, so they must not wait. MarkerSleep blocks here (slReflexSleep), not the presenting loop.
DWORD WINAPI ForwardMarkers(void* gameProcess)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE waits[2] = { static_cast<HANDLE>(gameProcess), g_markerEvent };
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        while (g_shared->markerTail != g_shared->markerHead) {
            const rhfg::MarkerEvent e = g_shared->markers[g_shared->markerTail % rhfg::kMarkers];
            _ReadWriteBarrier();
            g_shared->markerTail = g_shared->markerTail + 1;
            sl::FrameToken* token = FrameToken(e.frameIndex);
            if (e.marker == rhfg::MarkerSleep) {
                if (token) slReflexSleep(*token);
                SetEvent(g_sleptEvent);
            } else if (token)
                slPCLSetMarker(static_cast<sl::PCLMarker>(e.marker), *token);
        }
    }
    return 0;
}
ID3D12Fence *g_readyFence, *g_doneFence;
HWND g_window, g_gameWindow;
bool g_tearing, g_layered = true, g_userVisible = true, g_shown;
uint32_t g_configGeneration;
UINT g_width, g_height;
DXGI_FORMAT g_format;
// frames waiting for the timing thread (WatchFrames)
struct Pending { uint64_t frame, localValue; int64_t published; };
const uint32_t kPending = 64;
Pending g_pending[kPending];
volatile LONG g_pendingHead, g_pendingTail;
HANDLE g_pendingEvent;
CRITICAL_SECTION g_statsLock;

void Fail(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    vsnprintf(g_shared->message, sizeof g_shared->message, fmt, a);
    va_end(a);
    Log("%s", g_shared->message);
    g_shared->state = rhfg::StateFailed;
}

// GPU scheduling priority of the whole process (D3DKMTSetProcessSchedulingPriorityClass, gdi32): HIGH puts our
// work ahead of normal processes like the game; REALTIME would need a privilege.
void RaisePriority()
{
    typedef LONG(APIENTRY * SetPriorityClassFn)(HANDLE, int);
    const int kHigh = 4; // D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH
    auto setGpuPriority = reinterpret_cast<SetPriorityClassFn>(
        GetProcAddress(GetModuleHandleA("gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
    const LONG status = setGpuPriority ? setGpuPriority(GetCurrentProcess(), kHigh) : -1;
    Log("GPU scheduling priority class high: %s (status 0x%08lX)", status == 0 ? "ok" : "refused", status);
    // CPU: wake up right away when a frame arrives
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

void SlLog(sl::LogType type, const char* msg)
{
    if (type != sl::LogType::eInfo) Log("streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", msg);
}

// Before any DXGI/D3D12 call: the interposer (sl.interposer.lib) then hands out Streamline's device and swapchain,
// whose Present DLSS Frame Generation takes over. Plugins come from this folder only; no over-the-air updates.
void InitStreamline()
{
    static wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    static const wchar_t* paths[] = { dir };
    static const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    sl::Preferences pref{};
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = dir;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.logMessageCallback = SlLog;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = _countof(features);
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "6c2a1d9e-4f3b-4a87-9d51-3e8b7f20c4a1";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    const sl::Result r = slInit(pref);
    g_sl = r == sl::Result::eOk;
    Log("Streamline init: %s (%d)", g_sl ? "ok" : "failed, presenting without frame generation", static_cast<int>(r));
}

// on the presenting thread, so the next Present picks it up
void ApplyFrameGen()
{
    if (!g_fgSupported) return;
    sl::DLSSGOptions options{};
    options.mode = g_fgWanted ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    options.numBackBuffers = kBuffers;
    options.colorWidth = options.mvecDepthWidth = g_width;
    options.colorHeight = options.mvecDepthHeight = g_height;
    options.colorBufferFormat = g_format;
    options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
    options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    options.hudLessBufferFormat = g_format;
    options.uiBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    const sl::Result r = slDLSSGSetOptions(g_viewport, options);
    g_fgApplied = g_fgWanted;
    Log("frame generation %s (%d)", g_fgWanted ? "on" : "off", static_cast<int>(r));
}

bool CreateDevice()
{
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory)))) return false;
    LUID luid;
    luid.LowPart = g_shared->adapterLuidLow;
    luid.HighPart = g_shared->adapterLuidHigh;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(g_factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) { Log("adapter of the game not found"); return false; }
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    Log("adapter: %ls", desc.Description);
    if (g_sl) {
        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        info.deviceLUIDSizeInBytes = sizeof(LUID);
        const sl::Result r = slIsFeatureSupported(sl::kFeatureDLSS_G, info);
        g_fgSupported = r == sl::Result::eOk;
        Log("DLSS Frame Generation on this GPU: %s (%d)", g_fgSupported ? "supported" : "not supported", static_cast<int>(r));
    }
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    adapter->Release();
    if (FAILED(hr)) { Log("D3D12CreateDevice failed %08X", hr); return false; }
    if (g_sl) {
        slSetD3DDevice(g_device);
        // frame generation needs Reflex running
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        Log("Reflex low latency: %d", static_cast<int>(slReflexSetOptions(reflex)));
    }

    // our GPU work is small but waits behind the game's queued frames: ask for the highest queue priority we get
    // (global realtime needs the "increase scheduling priority" privilege, usually admin). Writing the swapchain's
    // buffers stays on the direct queue (from a copy queue it removed the device); the copy queue only takes frames
    // out of the shared slots into our own textures (IngestFrames).
    D3D12_COMMAND_QUEUE_DESC q = {};
    for (const D3D12_COMMAND_LIST_TYPE listType : { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COPY }) {
        ID3D12CommandQueue*& queue = listType == D3D12_COMMAND_LIST_TYPE_DIRECT ? g_queue : g_copyQueue;
        q.Type = listType;
        q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
        if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)))) {
            q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)))) return false;
        }
    }
    Log("command queue priority: %s", q.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME ? "global realtime" : "high");
    for (auto& allocator : g_allocators)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr, IID_PPV_ARGS(&g_list)))) return false;
    g_list->Close();
    for (auto& allocator : g_copyAllocators)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_copyAllocators[0], nullptr, IID_PPV_ARGS(&g_copyList)))) return false;
    g_copyList->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence)))) return false;
    g_copyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_localFence)))) return false;
    g_localEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_readyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    IDXGIFactory5* factory5 = nullptr;
    BOOL tearing = FALSE;
    if (SUCCEEDED(g_factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
        factory5->Release();
    }
    g_tearing = tearing != FALSE;
    Log("tearing (VSync off) supported: %d", g_tearing);
    return true;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

RECT GameClientRect()
{
    RECT r = {};
    GetClientRect(g_gameWindow, &r);
    POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
    ClientToScreen(g_gameWindow, &tl);
    ClientToScreen(g_gameWindow, &br);
    return { tl.x, tl.y, br.x, br.y };
}

bool CreateOverlayWindow(bool layered)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RHFrameGen64";
    wc.hCursor = nullptr;
    RegisterClassA(&wc);
    // click-through: input keeps going to the game window underneath
    DWORD ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    if (layered) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    const RECT r = GameClientRect();
    g_window = CreateWindowExA(ex, wc.lpszClassName, "RHFrameGen64", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_window) return false;
    if (layered) SetLayeredWindowAttributes(g_window, 0, 255, LWA_ALPHA);
    g_layered = layered;
    return true;
}

void ReleaseFrames()
{
    for (auto& frame : g_frames) Release(frame);
    for (auto& depth : g_depth) Release(depth);
    for (auto& motion : g_motion) Release(motion);
    for (auto& hudless : g_hudless) Release(hudless);
    for (auto& ui : g_ui) Release(ui);
    for (auto& own : g_ownColor) Release(own);
    for (auto& own : g_ownDepth) Release(own);
    for (auto& own : g_ownMotion) Release(own);
    for (auto& own : g_ownHudless) Release(own);
    for (auto& own : g_ownUi) Release(own);
    Release(g_readyFence);
    Release(g_doneFence);
}

void WaitIdle()
{
    if (!g_queue) return;
    g_queue->Signal(g_localFence, ++g_localValue);
    g_localFence->SetEventOnCompletion(g_localValue, g_localEvent);
    WaitForSingleObject(g_localEvent, 2000);
    if (g_copyQueue) {
        g_copyQueue->Signal(g_copyFence, ++g_copyValue);
        g_copyFence->SetEventOnCompletion(g_copyValue, g_copyEvent);
        WaitForSingleObject(g_copyEvent, 2000);
    }
}

// a texture of our own like a shared one (same size and format, so CopyResource works between them)
ID3D12Resource* CreateLike(ID3D12Resource* shared)
{
    D3D12_RESOURCE_DESC desc = shared->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                         nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) Log("CreateCommittedResource failed %08X", hr);
    return resource;
}

template <class T> T* OpenShared(uint64_t handle)
{
    T* object = nullptr;
    HANDLE h = reinterpret_cast<HANDLE>(handle);
    HRESULT hr = g_device->OpenSharedHandle(h, IID_PPV_ARGS(&object));
    CloseHandle(h);
    if (FAILED(hr)) Log("OpenSharedHandle failed %08X", hr);
    return object;
}

bool CreateSwapChain()
{
    for (auto& buffer : g_backBuffers) Release(buffer);
    const UINT flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr;
    if (g_swapChain) {
        hr = g_swapChain->ResizeBuffers(kBuffers, g_width, g_height, g_format, flags);
    } else {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = g_width;
        desc.Height = g_height;
        desc.Format = g_format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBuffers;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.Flags = flags;
        IDXGISwapChain1* swapChain = nullptr;
        hr = g_factory->CreateSwapChainForHwnd(g_queue, g_window, &desc, nullptr, nullptr, &swapChain);
        if (FAILED(hr) && g_layered) {
            // flip model on a layered window may be refused: then without click-through
            Log("swapchain on the click-through window failed %08X, retrying without click-through", hr);
            DestroyWindow(g_window);
            if (!CreateOverlayWindow(false)) return false;
            hr = g_factory->CreateSwapChainForHwnd(g_queue, g_window, &desc, nullptr, nullptr, &swapChain);
        }
        if (SUCCEEDED(hr)) {
            swapChain->QueryInterface(IID_PPV_ARGS(&g_swapChain));
            swapChain->Release();
            g_factory->MakeWindowAssociation(g_window, DXGI_MWA_NO_ALT_ENTER);
        }
    }
    if (FAILED(hr) || !g_swapChain) { Log("swapchain %ux%u format %d failed %08X", g_width, g_height, g_format, hr); return false; }
    for (UINT i = 0; i < kBuffers; i++) g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
    return true;
}

void Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER* out);

bool Configure()
{
    EnterCriticalSection(&g_slotLock);   // receiving waits until the new slots are open
    g_receiving = false;
    const bool ok = [] {
    WaitIdle();
    for (int i = 0; i < 1000 && g_pendingTail != g_pendingHead; i++) Sleep(1); // watcher done with the old fences
    ReleaseFrames();
    g_configGeneration = g_shared->configGeneration;
    g_width = g_shared->width;
    g_height = g_shared->height;
    g_format = static_cast<DXGI_FORMAT>(g_shared->format);
    if (g_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) g_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (g_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) g_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t i = 0; i < rhfg::kSlots; i++) {
        g_frames[i] = OpenShared<ID3D12Resource>(g_shared->hFrame[i]);
        g_depth[i] = OpenShared<ID3D12Resource>(g_shared->hDepth[i]);
        g_motion[i] = OpenShared<ID3D12Resource>(g_shared->hMotion[i]);
        g_hudless[i] = OpenShared<ID3D12Resource>(g_shared->hHudless[i]);
        g_ui[i] = OpenShared<ID3D12Resource>(g_shared->hUi[i]);
        if (!g_hudless[i] || !g_ui[i]) return false;
        if (!g_frames[i] || !g_depth[i] || !g_motion[i]) return false;
        g_ownColor[i] = CreateLike(g_frames[i]);
        g_ownDepth[i] = CreateLike(g_depth[i]);
        g_ownMotion[i] = CreateLike(g_motion[i]);
        g_ownHudless[i] = CreateLike(g_hudless[i]);
        g_ownUi[i] = CreateLike(g_ui[i]);
        if (!g_ownHudless[i] || !g_ownUi[i]) return false;
        if (!g_ownColor[i] || !g_ownDepth[i] || !g_ownMotion[i]) return false;
    }
    g_readyFence = OpenShared<ID3D12Fence>(g_shared->hReadyFence);
    g_doneFence = OpenShared<ID3D12Fence>(g_shared->hDoneFence);
    if (!g_readyFence || !g_doneFence || !CreateSwapChain()) return false;
    ApplyFrameGen();
    Log("frames %ux%u, format %d, VSync %d, click-through %d", g_width, g_height, g_format, g_shared->vsync, g_layered);
    return true;
    }();
    memset(g_received, 0, sizeof g_received);
    memset(g_ownBusyUntil, 0, sizeof g_ownBusyUntil);
    g_receiveNext = 1;
    g_latestReceived = 0;
    g_receiving = ok;
    LeaveCriticalSection(&g_slotLock);
    return ok;
}

// Receiving thread: for every frame the game publishes, at once and in order, a copy-queue copy out of the shared
// slot (colour, depth, motion vectors) into our own textures, then the slot is signalled free (done fence). GPU-side
// waits only: for the game's frame (ready fence) and for presenting to be done reading our copy from 3 frames ago.
DWORD WINAPI IngestFrames(void* gameProcess)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    char frameName[64], mapName[64], configName[64];
    rhfg::Names(g_gamePid, mapName, frameName, configName, sizeof frameName);
    HANDLE frameEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, frameName);
    HANDLE waits[2] = { static_cast<HANDLE>(gameProcess), frameEvent };
    for (;;) {
        if (WaitForMultipleObjects(2, waits, FALSE, 50) == WAIT_OBJECT_0) break;
        EnterCriticalSection(&g_slotLock);
        const uint64_t latest = g_receiving ? g_shared->frame : 0;
        // only the newest kSlots frames are in the slots (the game drops a frame rather than overwrite one)
        if (latest >= rhfg::kSlots && g_receiveNext < latest - rhfg::kSlots + 1) g_receiveNext = latest - rhfg::kSlots + 1;
        for (; g_receiving && g_receiveNext <= latest; g_receiveNext++) {
            const uint64_t n = g_receiveNext;
            const uint32_t slot = static_cast<uint32_t>(n % rhfg::kSlots);
            Received& r = g_received[slot];
            if (g_copyFence->GetCompletedValue() < r.copyValue) {   // this slot's allocator still in use
                g_copyFence->SetEventOnCompletion(r.copyValue, g_copyEvent);
                WaitForSingleObject(g_copyEvent, 1000);
            }
            g_copyAllocators[slot]->Reset();
            g_copyList->Reset(g_copyAllocators[slot], nullptr);
            // HUD-less frame and HUD layer only when the game drew this frame's HUD into its layer
            const bool hasHud = g_shared->slotHasHud[slot] != 0;
            const int count = hasHud ? 5 : 3;
            ID3D12Resource* from[5] = { g_frames[slot], g_depth[slot], g_motion[slot], g_hudless[slot], g_ui[slot] };
            ID3D12Resource* to[5] = { g_ownColor[slot], g_ownDepth[slot], g_ownMotion[slot], g_ownHudless[slot], g_ownUi[slot] };
            D3D12_RESOURCE_BARRIER b[10];
            for (int i = 0; i < count; i++) {
                Barrier(from[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, &b[i * 2]);
                Barrier(to[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST, &b[i * 2 + 1]);
            }
            g_copyList->ResourceBarrier(count * 2, b);
            for (int i = 0; i < count; i++) g_copyList->CopyResource(to[i], from[i]);
            for (int i = 0; i < count; i++) {
                Barrier(from[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON, &b[i * 2]);
                Barrier(to[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON, &b[i * 2 + 1]);
            }
            g_copyList->ResourceBarrier(count * 2, b);
            g_copyList->Close();
            g_copyQueue->Wait(g_localFence, g_ownBusyUntil[slot]);  // presenting is done with our copy in this slot
            g_copyQueue->Wait(g_readyFence, n);                     // the game's frame is in the slot
            ID3D12CommandList* lists[] = { g_copyList };
            g_copyQueue->ExecuteCommandLists(1, lists);
            g_copyQueue->Signal(g_doneFence, n);                    // the slot is free for the game again
            g_copyQueue->Signal(g_copyFence, ++g_copyValue);
            r.frame = n;
            r.copyValue = g_copyValue;
            r.frameIndex = g_shared->slotFrameIndex[slot];
            r.published = g_shared->publishQpc[slot];
            r.camera = g_shared->camera[slot];
            r.hasHud = hasHud;
            r.frameStart = g_shared->frameStartQpc[slot];
            g_latestReceived = n;
        }
        LeaveCriticalSection(&g_slotLock);
        if (latest) SetEvent(g_ingestedEvent);
    }
    return 0;
}

void Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, D3D12_RESOURCE_BARRIER* out)
{
    *out = {};
    out->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out->Transition.pResource = resource;
    out->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out->Transition.StateBefore = from;
    out->Transition.StateAfter = to;
}

// Shown only while the game window is in front and not minimised, over its client area
void UpdateWindow()
{
    const bool gameInFront = GetForegroundWindow() == g_gameWindow && !IsIconic(g_gameWindow);
    const bool show = g_userVisible && gameInFront && g_swapChain;
    if (show) {
        const RECT r = GameClientRect();
        RECT now;
        GetWindowRect(g_window, &now);
        if (!g_shown || !EqualRect(&r, &now))
            SetWindowPos(g_window, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (g_shown)
        ShowWindow(g_window, SW_HIDE);
    g_shown = show;
}

struct Stats
{
    LARGE_INTEGER freq, since;
    uint64_t presented, firstFrame;
    uint32_t firstDropped;
    double cpuSum, cpuMax, gpuSum, gpuMax, bridgeSum, bridgeMax;
    uint32_t gpuSamples;
    double screenSum, screenMax;
    uint32_t screenSamples;
    uint32_t hudFrames;   // presented with the HUD-less frame and HUD layer tagged
} g_stats;

// publish time of each Present (by present count), to match DXGI's frame statistics: when it reached the screen
const uint32_t kPresentHistory = 256;
int64_t g_presentPublished[kPresentHistory];
UINT g_lastStatsPresent;
bool g_statsUnavailableLogged;

void CheckFrameStatistics()
{
    DXGI_FRAME_STATISTICS st = {};
    const HRESULT hr = g_swapChain->GetFrameStatistics(&st);
    if (FAILED(hr)) {
        if (!g_statsUnavailableLogged && hr != DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
            Log("frame statistics unavailable (%08X): no 'on screen' timing", hr);
            g_statsUnavailableLogged = true;
        }
        return;
    }
    if (st.PresentCount == g_lastStatsPresent || st.PresentCount == 0) return;
    g_lastStatsPresent = st.PresentCount;
    const int64_t published = g_presentPublished[st.PresentCount % kPresentHistory];
    if (!published) return;
    const double ms = (st.SyncQPCTime.QuadPart - published) * 1000.0 / g_stats.freq.QuadPart;
    if (ms < 0 || ms > 500) return;
    EnterCriticalSection(&g_statsLock);
    g_stats.screenSum += ms;
    if (ms > g_stats.screenMax) g_stats.screenMax = ms;
    g_stats.screenSamples++;
    LeaveCriticalSection(&g_statsLock);
}

double MsSince(int64_t qpc)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - qpc) * 1000.0 / g_stats.freq.QuadPart;
}

// GPU timing on its own thread, so it doesn't depend on where the presenting loop blocks: for every frame, when the
// game's GPU finished it (ready fence) and when our copy finished (local fence). Fences complete in order, so waiting
// for them one after the other only adds the thread's wake-up time.
DWORD WINAPI WatchFrames(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE readyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr), doneEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
        if (g_pendingTail == g_pendingHead) { WaitForSingleObject(g_pendingEvent, 100); continue; }
        const Pending p = g_pending[g_pendingTail % kPending];
        if (!g_readyFence) { InterlockedIncrement(&g_pendingTail); continue; }
        g_readyFence->SetEventOnCompletion(p.frame, readyEvent);
        WaitForSingleObject(readyEvent, 1000);
        const double ready = MsSince(p.published);
        g_localFence->SetEventOnCompletion(p.localValue, doneEvent);
        WaitForSingleObject(doneEvent, 1000);
        const double bridge = MsSince(p.published) - ready;
        InterlockedIncrement(&g_pendingTail);
        EnterCriticalSection(&g_statsLock);
        g_stats.gpuSum += ready;
        if (ready > g_stats.gpuMax) g_stats.gpuMax = ready;
        g_stats.bridgeSum += bridge;
        if (bridge > g_stats.bridgeMax) g_stats.bridgeMax = bridge;
        g_stats.gpuSamples++;
        LeaveCriticalSection(&g_statsLock);
    }
}

// 4x4 in double: GTA's world reaches several thousand units, where float clip-space round trips lose precision
struct MatD { double m[16]; };

MatD ToD(const float* f) { MatD r; for (int i = 0; i < 16; i++) r.m[i] = f[i]; return r; }

MatD MulD(const MatD& a, const MatD& b)
{
    MatD r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

// Gauss-Jordan with partial pivoting
bool InvD(const MatD& in, MatD& out)
{
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? in.m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out.m[i * 4 + j] = a[i][j + 4];
    return true;
}

sl::float4x4 ToSl(const MatD& m)
{
    sl::float4x4 r;
    for (int i = 0; i < 4; i++)
        r.row[i] = sl::float4(static_cast<float>(m.m[i * 4]), static_cast<float>(m.m[i * 4 + 1]), static_cast<float>(m.m[i * 4 + 2]),
                              static_cast<float>(m.m[i * 4 + 3]));
    return r;
}

// Streamline's constants for the frame in this slot: row vectors like D3D (clipToPrevClip = clipToView * viewToViewPrev *
// viewToClipPrev), no jitter (the frame is already resolved), motion vectors in pixels current -> previous.
void SetFrameConstants(const sl::FrameToken& token, const rhfg::Camera& cam)
{
    const MatD view = ToD(cam.view), proj = ToD(cam.proj);
    const MatD viewProj = MulD(view, proj), prevViewProj = MulD(ToD(cam.prevView), ToD(cam.prevProj));
    MatD clipToView, viewInv, clipToWorld, clipToPrevClip, prevClipToClip;
    if (!InvD(proj, clipToView) || !InvD(view, viewInv) || !InvD(viewProj, clipToWorld)) return;
    clipToPrevClip = MulD(clipToWorld, prevViewProj);
    if (!InvD(clipToPrevClip, prevClipToClip)) return;

    sl::Constants c{};
    c.cameraViewToClip = ToSl(proj);
    c.clipToCameraView = ToSl(clipToView);
    MatD identity = {};
    identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0;
    c.clipToLensClip = ToSl(identity);
    c.clipToPrevClip = ToSl(clipToPrevClip);
    c.prevClipToClip = ToSl(prevClipToClip);
    c.jitterOffset = sl::float2(0.0f, 0.0f);
    c.mvecScale = sl::float2(1.0f / g_width, 1.0f / g_height);
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    const double* v = viewInv.m;
    c.cameraRight = sl::float3(float(v[0]), float(v[1]), float(v[2]));
    c.cameraUp = sl::float3(float(v[4]), float(v[5]), float(v[6]));
    c.cameraFwd = sl::float3(float(v[8]), float(v[9]), float(v[10]));
    c.cameraPos = sl::float3(float(v[12]), float(v[13]), float(v[14]));
    // D3D perspective: _33 = f / (f - n), _43 = -n f / (f - n)
    const double p33 = proj.m[10], p43 = proj.m[14];
    c.cameraNear = static_cast<float>(-p43 / p33);
    c.cameraFar = static_cast<float>(p43 / (1.0 - p33));
    c.cameraFOV = static_cast<float>(2.0 * atan(1.0 / proj.m[5]));
    c.cameraAspectRatio = static_cast<float>(proj.m[5] / proj.m[0]);
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = cam.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    slSetConstants(c, token, g_viewport);
}

// false when the device is lost
bool PresentFrame(uint64_t frame)
{
    const uint32_t slot = static_cast<uint32_t>(frame % rhfg::kSlots);
    // our copy of the frame (IngestFrames); held while recording, so receiving can't reuse it meanwhile
    EnterCriticalSection(&g_slotLock);
    const Received r = g_received[slot];
    if (r.frame != frame || !g_receiving) { LeaveCriticalSection(&g_slotLock); return true; }
    // back-pressure: the game may start its next frame now
    g_shared->pickedFrame = frame;
    if (g_pickedEvent) SetEvent(g_pickedEvent);
    const int64_t published = r.published;
    // Streamline's frame: the game's frame index. With markers from the game (RenderHook with Reflex) it already marked
    // the simulation and render submission at the real moments; otherwise they are marked here on arrival, which keeps
    // frame generation working but gives Reflex nothing to pace.
    const bool gameMarkers = g_shared->gameMarkers != 0;
    const uint32_t frameIndex = gameMarkers ? r.frameIndex : static_cast<uint32_t>(frame);
    sl::FrameToken* token = g_sl ? FrameToken(frameIndex) : nullptr;
    if (token) {
        if (g_fgApplied != g_fgWanted) ApplyFrameGen();
        if (!gameMarkers) {
            slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
            slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
            slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
        }
        SetFrameConstants(*token, r.camera);
    }
    const UINT index = g_swapChain->GetCurrentBackBufferIndex();
    if (g_localFence->GetCompletedValue() < g_allocatorFence[index]) {
        g_localFence->SetEventOnCompletion(g_allocatorFence[index], g_localEvent);
        WaitForSingleObject(g_localEvent, 1000);
    }
    g_allocators[index]->Reset();
    g_list->Reset(g_allocators[index], nullptr);
    D3D12_RESOURCE_BARRIER b[2];
    Barrier(g_ownColor[slot], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, &b[0]);
    Barrier(g_backBuffers[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, &b[1]);
    g_list->ResourceBarrier(2, b);
    g_list->CopyResource(g_backBuffers[index], g_ownColor[slot]);
    Barrier(g_ownColor[slot], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON, &b[0]);
    Barrier(g_backBuffers[index], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT, &b[1]);
    g_list->ResourceBarrier(2, b);
    if (token) {
        // depth, motion vectors and (when the game drew its HUD into the HUD layer) the frame without HUD plus the HUD
        // layer, premultiplied: frame = ui.rgb + (1 - ui.a) * hudless. From our copies; Streamline copies them on this
        // list. Frame generation then interpolates only the world and puts the HUD on top.
        sl::Resource depth(sl::ResourceType::eTex2d, g_ownDepth[slot], D3D12_RESOURCE_STATE_COMMON);
        sl::Resource motion(sl::ResourceType::eTex2d, g_ownMotion[slot], D3D12_RESOURCE_STATE_COMMON);
        sl::Resource hudless(sl::ResourceType::eTex2d, g_ownHudless[slot], D3D12_RESOURCE_STATE_COMMON);
        sl::Resource ui(sl::ResourceType::eTex2d, g_ownUi[slot], D3D12_RESOURCE_STATE_COMMON);
        sl::Extent extent{};
        extent.width = g_width;
        extent.height = g_height;
        const sl::ResourceTag tags[] = {
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent),
            sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &extent),
            sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eOnlyValidNow, &extent),
        };
        static int tagErrors;
        const uint32_t tagCount = r.hasHud ? _countof(tags) : 2;   // without a HUD layer: depth and motion only
        if (r.hasHud) g_stats.hudFrames++;
        const sl::Result tagResult = slSetTagForFrame(*token, g_viewport, tags, tagCount, g_list);
        if (tagResult != sl::Result::eOk && tagErrors++ < 5) Log("slSetTagForFrame failed (%d)", static_cast<int>(tagResult));
    }
    g_list->Close();

    g_queue->Wait(g_copyFence, r.copyValue);   // our copy of the frame is done
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_localFence, ++g_localValue);
    g_allocatorFence[index] = g_localValue;
    g_ownBusyUntil[slot] = g_localValue;       // receiving may overwrite our copy after this
    LeaveCriticalSection(&g_slotLock);
    if (token && !gameMarkers) slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);

    // timed on the watcher thread (skipped if it's 64 frames behind)
    if (g_pendingHead - g_pendingTail < static_cast<LONG>(kPending)) {
        g_pending[g_pendingHead % kPending] = { frame, g_localValue, published };
        InterlockedIncrement(&g_pendingHead);
        SetEvent(g_pendingEvent);
    }
    g_stats.presented++;
    // how long the Present call itself takes (it may wait for the copy queue)
    LARGE_INTEGER before;
    QueryPerformanceCounter(&before);
    const bool vsync = g_shared->vsync != 0;
    if (token) slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
    LARGE_INTEGER presentCall;
    QueryPerformanceCounter(&presentCall);
    const HRESULT presentResult = g_swapChain->Present(vsync ? 1 : 0, !vsync && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (token) slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
    if (g_timing) {
        LARGE_INTEGER presentReturn;
        QueryPerformanceCounter(&presentReturn);
        fprintf(g_timing, "%llu,%lld,%lld,%lld,%lld,%d\n", static_cast<unsigned long long>(frame), r.frameStart, published,
                presentCall.QuadPart, presentReturn.QuadPart, g_fgApplied ? 1 : 0);
    }
    if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
        // everything on a lost device completes at once and shows nothing: stop, RenderHook falls back to its own output
        Fail("GPU device lost (Present %08X, reason %08X)", presentResult, g_device->GetDeviceRemovedReason());
        return false;
    }
    static int presentErrors;
    if (presentResult != S_OK && presentErrors++ < 5)
        Log("Present returned %08X (frame %llu)", presentResult, static_cast<unsigned long long>(frame));
    UINT presentCount = 0;
    if (SUCCEEDED(g_swapChain->GetLastPresentCount(&presentCount)))
        g_presentPublished[presentCount % kPresentHistory] = published;
    CheckFrameStatistics();
    const double cpu = MsSince(before.QuadPart);
    g_stats.cpuSum += cpu;
    if (cpu > g_stats.cpuMax) g_stats.cpuMax = cpu;
    return true;
}

void LogStats(uint64_t frame)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double seconds = (now.QuadPart - g_stats.since.QuadPart) / static_cast<double>(g_stats.freq.QuadPart);
    if (seconds < 5.0) return;
    const uint64_t presented = g_stats.presented;
    EnterCriticalSection(&g_statsLock);
    if (presented)
        Log("%.0f s: game %.1f fps, presented %.1f fps, dropped by the game %u, window %s | Present call avg %.2f ms max %.2f | "
            "game GPU backlog avg %.2f ms max %.2f | bridge on GPU (game frame done -> our copy done) avg %.2f ms max %.2f | "
            "publish -> on screen avg %.2f ms max %.2f (%u samples)",
            seconds, (frame - g_stats.firstFrame) / seconds, presented / seconds, g_shared->droppedFrames - g_stats.firstDropped,
            g_shown ? "shown" : "hidden", g_stats.cpuSum / presented, g_stats.cpuMax,
            g_stats.gpuSamples ? g_stats.gpuSum / g_stats.gpuSamples : 0.0, g_stats.gpuMax,
            g_stats.gpuSamples ? g_stats.bridgeSum / g_stats.gpuSamples : 0.0, g_stats.bridgeMax,
            g_stats.screenSamples ? g_stats.screenSum / g_stats.screenSamples : 0.0, g_stats.screenMax, g_stats.screenSamples);
    const uint32_t hudFrames = g_stats.hudFrames;
    g_stats = { g_stats.freq, now, 0, frame, g_shared->droppedFrames, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    LeaveCriticalSection(&g_statsLock);
    if (g_fgSupported) {
        // frames shown by the last Present (real + generated) and DLSS-G's own error flags (DLSSGStatus, 0 = ok)
        sl::DLSSGState state{};
        if (slDLSSGGetState(g_viewport, state, nullptr) == sl::Result::eOk)
            Log("    frame generation %s: %u frames per Present, status 0x%X, HUD-less + HUD layer on %u of %llu frames",
                g_fgApplied ? "on" : "off", state.numFramesActuallyPresented, static_cast<unsigned>(state.status), hudFrames,
                static_cast<unsigned long long>(presented));
    }
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const uint32_t pid = wcstoul(cmdLine, nullptr, 10);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".log");
    g_log = _fsopen(path, "w", _SH_DENYNO);
    // keys, RHFrameGen64.ini [Keys]: F1..F24, a virtual-key code (0x74 or 116) or none
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* dot = strrchr(ini, '.')) strcpy_s(dot, ini + MAX_PATH - dot, ".ini");
    auto readKey = [&](const char* name, const char* def) {
        char value[32];
        GetPrivateProfileStringA("Keys", name, def, value, sizeof value, ini);
        if ((value[0] == 'F' || value[0] == 'f') && value[1] >= '1' && value[1] <= '9') {
            const int n = atoi(value + 1);
            return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
        }
        return static_cast<int>(strtol(value, nullptr, 0));   // "none" -> 0: no key
    };
    const int windowKey = readKey("ToggleWindow", "F5"), frameGenKey = readKey("ToggleFrameGeneration", "F11");
    Log("RHFrameGen64, game pid %u. Keys (RHFrameGen64.ini): window 0x%X, frame generation 0x%X", pid, windowKey, frameGenKey);

    char mapName[64], frameName[64], configName[64];
    rhfg::Names(pid, mapName, frameName, configName, sizeof mapName);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapName);
    HANDLE frameEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, frameName);
    HANDLE configEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, configName);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!mapping || !frameEvent || !configEvent || !game) { Log("cannot open the game's shared objects"); return 1; }
    g_shared = static_cast<rhfg::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rhfg::Shared)));
    if (!g_shared || g_shared->version != rhfg::kVersion) {
        Log("protocol version mismatch: _gtaRenderHook.asi speaks version %u, this RHFrameGen64.exe %u - install both from the same "
            "package", g_shared ? g_shared->version : 0, rhfg::kVersion);
        return 1;
    }
    g_gameWindow = reinterpret_cast<HWND>(g_shared->gameWindow);

    RaisePriority();
    InitStreamline();
    // RHFrameGen64.fgoff next to the exe: start with frame generation off (F11 still switches it)
    char fgOff[MAX_PATH];
    GetModuleFileNameA(nullptr, fgOff, MAX_PATH);
    if (char* dot = strrchr(fgOff, '.')) strcpy_s(dot, fgOff + MAX_PATH - dot, ".fgoff");
    if (GetFileAttributesA(fgOff) != INVALID_FILE_ATTRIBUTES) g_fgWanted = false;
    char timing[MAX_PATH];
    GetModuleFileNameA(nullptr, timing, MAX_PATH);
    if (char* dot = strrchr(timing, '.')) strcpy_s(dot, timing + MAX_PATH - dot, ".timing");
    if (GetFileAttributesA(timing) != INVALID_FILE_ATTRIBUTES) {
        strcpy_s(strrchr(timing, '.'), 13, "_timing.csv");
        g_timing = _fsopen(timing, "w", _SH_DENYNO);
        if (g_timing) fprintf(g_timing, "frame,frameStartQpc,publishQpc,presentCallQpc,presentReturnQpc,frameGeneration\n");
    }
    if (!CreateDevice()) { Fail("could not create a D3D12 device on the game's GPU"); return 1; }
    if (!CreateOverlayWindow(true)) { Fail("could not create the window"); return 1; }
    InitializeCriticalSection(&g_tokenLock);
    char markerName[64], sleptName[64];
    rhfg::ReflexNames(pid, markerName, sleptName, sizeof markerName);
    g_markerEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, markerName);
    g_sleptEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, sleptName);
    char pickedName[64];
    rhfg::PickedName(pid, pickedName, sizeof pickedName);
    g_pickedEvent = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, pickedName);
    if (g_sl && g_markerEvent && g_sleptEvent) {
        CloseHandle(CreateThread(nullptr, 0, ForwardMarkers, game, 0, nullptr));
        g_shared->reflexReady = 1;
        Log("Reflex: forwarding the game's markers and sleep");
    }
    g_shared->state = rhfg::StateReady;
    QueryPerformanceFrequency(&g_stats.freq);
    QueryPerformanceCounter(&g_stats.since);
    InitializeCriticalSection(&g_statsLock);
    g_pendingEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, WatchFrames, nullptr, 0, nullptr));
    // receiving: the game's frame event belongs to IngestFrames from here on; we wait for its "received"
    InitializeCriticalSection(&g_slotLock);
    g_ingestedEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_gamePid = pid;
    CloseHandle(CreateThread(nullptr, 0, IngestFrames, game, 0, nullptr));

    uint64_t presented = 0;
    bool configured = false, keyDown = false, fgKeyDown = false;
    HANDLE waits[3] = { game, configEvent, g_ingestedEvent };
    for (;;) {
        const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 50);
        if (w == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (g_shared->configGeneration != g_configGeneration) {
            configured = Configure();
            if (!configured) { Fail("could not open the shared frames or create the swapchain"); break; }
            presented = 0;
        }
        const bool down = windowKey && (GetAsyncKeyState(windowKey) & 0x8000) != 0;
        if (down && !keyDown) {
            g_userVisible = !g_userVisible;
            Log("window %s (key)", g_userVisible ? "on" : "off");
        }
        keyDown = down;
        const bool fgDown = frameGenKey && (GetAsyncKeyState(frameGenKey) & 0x8000) != 0;
        if (fgDown && !fgKeyDown && g_fgSupported) {
            g_fgWanted = !g_fgWanted;   // applied on the next frame, before its Present
            Log("frame generation %s (key)", g_fgWanted ? "on" : "off");
        }
        fgKeyDown = fgDown;
        UpdateWindow();
        const uint64_t frame = g_latestReceived;
        if (configured && frame > presented) {
            if (!PresentFrame(frame)) break;   // only the newest received; frames in between are skipped
            presented = frame;
        }
        LogStats(g_shared->frame);
    }
    Log("game exited");
    WaitIdle();
    if (g_sl) slShutdown();
    return 0;
}
