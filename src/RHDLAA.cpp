// RHDLAA - NVIDIA DLAA in place of gtaRenderHook's TAA. GTA SA 1.0 US.
//
// With a RenderHook that exports RHSetTemporalAAHandler (RenderHookAPI.h) the plugin registers a handler and needs
// no patching; RenderHook passes the frame's textures, jitter and matrices. For the 2021-09-11 release, which has no
// such export, it hooks that binary at fixed offsets as described below.
//
// The DLSS runtime is 64-bit only, so it runs in RHDLAA64.exe (scripts\RHDLAA\), started by this plugin. Each frame:
//  - RenderHook's TAA step (call at RH+0xDBB32 to CTemporalAA::Render, before tonemapping) is replaced: the HDR
//    colour is copied into a texture shared with the helper, and a pass writes camera motion vectors + depth from
//    the G-buffer into two more shared textures;
//  - the game's device signals a shared fence, the helper GPU-waits it, runs DLAA into the shared output and signals
//    back; the game GPU-waits that and copies the result into RenderHook's output raster. If the helper is not
//    ready or fails, RenderHook's own TAA runs instead.
//  - Jitter: RenderHook's CTemporalAA::JitterProjMatrix (call at RH+0xECA6D) never advances its sample index
//    (m_nCurrentJitterSample = m_nCurrentJitterSample++ % SubPixelCount), so its TAA has no subpixel jitter at all.
//    The wrapper advances the index and measures the offset it applied, which DLAA needs exactly.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include "Shared.h"
#include "RenderHookAPI.h"
#include "MotionVectorsVS.h"
#include "MotionVectorsPS.h"

namespace {

FILE* g_log;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ------------------------------------------------------------------------------------------------ game / RenderHook
float* const kViewTransform = reinterpret_cast<float*>(0xC9BC80);   // RwD3D9D3D9ViewTransform
float* const kProjTransform = reinterpret_cast<float*>(0x8E2458);   // RwD3D9D3D9ProjTransform

const uint32_t kTaaCall = 0xDBB32;          // RenderTonemappedOutput: call CTemporalAA::Render
const uint32_t kTaaRender = 0xF3330;        // ecx = input, edx = output, stack: gbuffer1, gbuffer2 (caller cleans)
const uint32_t kJitterCall = 0xECA6D;       // idle hook: call CTemporalAA::JitterProjMatrix
const uint32_t kJitterFn = 0xF30E0;
const uint32_t kJitterIndex = 0x14EB64;     // CTemporalAA::m_nCurrentJitterSample
const uint32_t kEngine = 0x1428DC;          // g_pRwCustomEngine; [+8] renderer, [renderer+0x14] context
const uint32_t kDynamicCast = 0x11E1A2;     // __RTDynamicCast
const uint32_t kRasterSrcType = 0x140D28;   // RTTI descriptors used by the state manager to get a raster's SRV
const uint32_t kRasterDstType = 0x140D04;

uint8_t* g_rh;
void* g_taaRender;
void* g_jitterFn;

typedef void*(__cdecl* DynamicCastFn)(void*, long, void*, void*, int);
typedef ID3D11ShaderResourceView*(__thiscall* GetSrvFn)(void*);

ID3D11ShaderResourceView* RasterSrv(void* raster)
{
    if (!raster) return nullptr;
    void* res = *reinterpret_cast<void**>(static_cast<uint8_t*>(raster) + 0x34);
    if (!res) return nullptr;
    void* tex = reinterpret_cast<DynamicCastFn>(g_rh + kDynamicCast)(res, 0, g_rh + kRasterSrcType, g_rh + kRasterDstType, 0);
    if (!tex) return nullptr;
    return (*reinterpret_cast<GetSrvFn**>(tex))[0](tex);
}

ID3D11Texture2D* RasterTexture(void* raster)
{
    ID3D11ShaderResourceView* srv = RasterSrv(raster);
    if (!srv) return nullptr;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)); res->Release(); }
    return tex;   // caller releases
}

ID3D11DeviceContext* RhContext()
{
    uint8_t* engine = *reinterpret_cast<uint8_t**>(g_rh + kEngine);
    if (!engine) return nullptr;
    uint8_t* renderer = *reinterpret_cast<uint8_t**>(engine + 8);
    return renderer ? *reinterpret_cast<ID3D11DeviceContext**>(renderer + 0x14) : nullptr;
}

// ------------------------------------------------------------------------------------------------ settings
bool g_enabled = true;
int g_preset = 0;
int g_toggleKey = VK_F9;
float g_jitterSignX = 1.0f, g_jitterSignY = 1.0f;
float g_resetDistance = 30.0f;

// ------------------------------------------------------------------------------------------------ matrices
struct Mat { float m[16]; };

Mat Mul(const Mat& a, const Mat& b)
{
    Mat r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j] + a.m[i * 4 + 3] * b.m[12 + j];
    return r;
}

bool Invert(const Mat& in, Mat& out)
{
    const float* m = in.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-12f) return false;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] / det;
    return true;
}

// Per frame, captured when RenderHook jitters the main camera.
struct FrameCamera {
    Mat view, proj, jitteredProj;
    bool valid;
};
FrameCamera g_cam, g_prevCam;
bool g_jitterActive;   // DLAA ran last frame, so keep advancing the jitter

void __cdecl JitterHook()
{
    if (g_jitterActive) ++*reinterpret_cast<int*>(g_rh + kJitterIndex);
    memcpy(g_cam.proj.m, kProjTransform, sizeof(Mat));
    reinterpret_cast<void(__cdecl*)()>(g_jitterFn)();
    memcpy(g_cam.jitteredProj.m, kProjTransform, sizeof(Mat));
    memcpy(g_cam.view.m, kViewTransform, sizeof(Mat));
    // the unjittered projection is the game's one; RenderHook only adds to _31/_32
    g_cam.valid = true;
}

// ------------------------------------------------------------------------------------------------ D3D11 side
struct Reprojection {
    Mat viewInv, viewProj, prevViewProj;
    float proj[4];
    float screen[2];
    float objectMotion; // 1: RenderHook's object motion vectors are bound (t1)
    float pad;
};

ID3D11Device5* g_device;
ID3D11DeviceContext4* g_context;
ID3DDeviceContextState* g_state;
ID3D11VertexShader* g_vs;
ID3D11PixelShader* g_ps;
ID3D11Buffer* g_cb;

ID3D11Texture2D *g_color, *g_depth, *g_motion, *g_output;
ID3D11RenderTargetView *g_depthRtv, *g_motionRtv;
ID3D11Fence* g_fence;
uint64_t g_fenceValue;
UINT g_width, g_height;
DXGI_FORMAT g_format;
bool g_created;

rhdlaa::Shared* g_shared;
HANDLE g_request, g_done, g_helper, g_job;
bool g_helperStarted, g_broken, g_active = true, g_keyDown, g_reset = true;
LARGE_INTEGER g_lastFrame;

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

void ReleaseTextures()
{
    Release(g_depthRtv); Release(g_motionRtv);
    Release(g_color); Release(g_depth); Release(g_motion); Release(g_output);
    Release(g_fence);
    g_created = false;
}

void Break(const char* why)
{
    Log("DLAA disabled for this session: %s", why);
    g_broken = true;
    g_jitterActive = false;
}

bool InitDevice(ID3D11DeviceContext* rhContext)
{
    ID3D11Device* device = nullptr;
    rhContext->GetDevice(&device);
    device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_device));
    rhContext->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_context));
    device->Release();
    if (!g_device || !g_context) { Break("D3D11.4 (Windows 10+) interfaces not available"); return false; }

    D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
    if (FAILED(g_device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state)) ||
        FAILED(g_device->CreateVertexShader(g_MotionVectorsVS, sizeof g_MotionVectorsVS, nullptr, &g_vs)) ||
        FAILED(g_device->CreatePixelShader(g_MotionVectorsPS, sizeof g_MotionVectorsPS, nullptr, &g_ps))) {
        Break("could not create shaders/state");
        return false;
    }
    D3D11_BUFFER_DESC cbd = { sizeof(Reprojection), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(g_device->CreateBuffer(&cbd, nullptr, &g_cb))) { Break("could not create constant buffer"); return false; }
    return true;
}

bool StartHelper()
{
    g_helperStarted = true;
    const DWORD pid = GetCurrentProcessId();
    char mapName[64], reqName[64], doneName[64];
    rhdlaa::Names(pid, mapName, reqName, doneName, sizeof mapName);
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(rhdlaa::Shared), mapName);
    g_shared = mapping ? static_cast<rhdlaa::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rhdlaa::Shared))) : nullptr;
    g_request = CreateEventA(nullptr, FALSE, FALSE, reqName);
    g_done = CreateEventA(nullptr, FALSE, FALSE, doneName);
    if (!g_shared || !g_request || !g_done) { Break("could not create shared memory/events"); return false; }
    memset(g_shared, 0, sizeof *g_shared);
    g_shared->version = rhdlaa::kVersion;
    g_shared->preset = g_preset;

    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&desc);
    Release(adapter); Release(dxgi);
    g_shared->adapterLuidLow = desc.AdapterLuid.LowPart;
    g_shared->adapterLuidHigh = desc.AdapterLuid.HighPart;

    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&StartHelper), &self);
    GetModuleFileNameA(self, exe, MAX_PATH);
    if (char* slash = strrchr(exe, '\\')) strcpy_s(slash + 1, exe + MAX_PATH - slash - 1, "RHDLAA\\RHDLAA64.exe");
    snprintf(cmd, sizeof cmd, "\"%s\" %lu", exe, pid);

    // the helper dies with the game
    g_job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Log("cannot start %s (error %lu)", exe, GetLastError());
        Break("helper missing");
        return false;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    Log("helper started: %s", exe);
    return true;
}

HANDLE ShareTexture(ID3D11Texture2D* tex)
{
    IDXGIResource1* res = nullptr;
    HANDLE h = nullptr, dup = nullptr;
    if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res)))) {
        res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
        res->Release();
    }
    if (h) {
        DuplicateHandle(GetCurrentProcess(), h, g_helper, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CloseHandle(h);
    }
    return dup;
}

DXGI_FORMAT Typed(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

bool Request(uint32_t cmd, DWORD timeout)
{
    g_shared->cmd = cmd;
    g_shared->result = 0;
    ResetEvent(g_done);   // the helper also signals it once at start-up
    SetEvent(g_request);
    HANDLE waits[2] = { g_done, g_helper };
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeout);
    if (w == WAIT_OBJECT_0 + 1) { Break("helper exited (see scripts\\RHDLAA\\RHDLAA64.log)"); return false; }
    if (w != WAIT_OBJECT_0) { Break("helper did not answer in time"); return false; }
    return g_shared->result != 0;
}

bool CreateTextures(const D3D11_TEXTURE2D_DESC& src)
{
    ReleaseTextures();
    g_width = src.Width;
    g_height = src.Height;
    g_format = src.Format;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = src.Width; d.Height = src.Height; d.MipLevels = 1; d.ArraySize = 1;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    d.Format = Typed(src.Format); d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &g_color);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (SUCCEEDED(hr)) hr = g_device->CreateTexture2D(&d, nullptr, &g_output);
    d.Format = DXGI_FORMAT_R32_FLOAT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = g_device->CreateTexture2D(&d, nullptr, &g_depth);
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (SUCCEEDED(hr)) hr = g_device->CreateTexture2D(&d, nullptr, &g_motion);
    if (SUCCEEDED(hr)) hr = g_device->CreateRenderTargetView(g_depth, nullptr, &g_depthRtv);
    if (SUCCEEDED(hr)) hr = g_device->CreateRenderTargetView(g_motion, nullptr, &g_motionRtv);
    if (SUCCEEDED(hr)) hr = g_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_fence));
    if (FAILED(hr)) { Log("creating shared textures failed %08X (format %d)", hr, src.Format); Break("shared textures"); return false; }
    g_fenceValue = 0;

    HANDLE fence = nullptr, fenceDup = nullptr;
    g_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence);
    if (fence) { DuplicateHandle(GetCurrentProcess(), fence, g_helper, &fenceDup, 0, FALSE, DUPLICATE_SAME_ACCESS); CloseHandle(fence); }
    g_shared->hColor = reinterpret_cast<uint64_t>(ShareTexture(g_color));
    g_shared->hDepth = reinterpret_cast<uint64_t>(ShareTexture(g_depth));
    g_shared->hMotion = reinterpret_cast<uint64_t>(ShareTexture(g_motion));
    g_shared->hOutput = reinterpret_cast<uint64_t>(ShareTexture(g_output));
    g_shared->hFence = reinterpret_cast<uint64_t>(fenceDup);
    g_shared->width = g_width;
    g_shared->height = g_height;
    if (!g_shared->hColor || !g_shared->hDepth || !g_shared->hMotion || !g_shared->hOutput || !fenceDup) {
        Break("could not share textures with the helper");
        return false;
    }
    if (!Request(rhdlaa::CmdCreate, 10000)) {
        if (!g_broken) Break("DLAA feature creation failed (see scripts\\RHDLAA\\RHDLAA64.log)");
        return false;
    }
    Log("DLAA on: %ux%u, colour format %d", g_width, g_height, src.Format);
    g_created = true;
    g_reset = true;
    return true;
}

void DrawMotionVectors(ID3D11ShaderResourceView* gbuffer1, ID3D11ShaderResourceView* objectMotion, ID3D11Buffer* rhState)
{
    Reprojection r = {};
    r.objectMotion = objectMotion ? 1.0f : 0.0f;
    Invert(g_cam.view, r.viewInv);
    r.viewProj = Mul(g_cam.view, g_cam.proj);
    r.prevViewProj = g_prevCam.valid ? Mul(g_prevCam.view, g_prevCam.proj) : r.viewProj;
    r.proj[0] = g_cam.jitteredProj.m[0];
    r.proj[1] = g_cam.jitteredProj.m[5];
    r.proj[2] = g_cam.jitteredProj.m[8];
    r.proj[3] = g_cam.jitteredProj.m[9];
    r.screen[0] = static_cast<float>(g_width);
    r.screen[1] = static_cast<float>(g_height);
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g_context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &r, sizeof r);
        g_context->Unmap(g_cb, 0);
    }

    ID3DDeviceContextState* rhStateObj = nullptr;
    g_context->SwapDeviceContextState(g_state, &rhStateObj);   // saves all of RenderHook's pipeline state
    ID3D11RenderTargetView* rtvs[2] = { g_motionRtv, g_depthRtv };
    g_context->OMSetRenderTargets(2, rtvs, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, r.screen[0], r.screen[1], 0, 1 };
    g_context->RSSetViewports(1, &vp);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->PSSetShader(g_ps, nullptr, 0);
    ID3D11Buffer* cbs[2] = { rhState, g_cb };
    g_context->PSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* srvs[2] = { gbuffer1, objectMotion };
    g_context->PSSetShaderResources(0, 2, srvs);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrvs[2] = {};
    g_context->PSSetShaderResources(0, 2, nullSrvs);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_context->SwapDeviceContextState(rhStateObj, nullptr);
    Release(rhStateObj);
}

bool CameraJumped()
{
    if (!g_prevCam.valid) return true;
    Mat a, b;
    if (!Invert(g_cam.view, a) || !Invert(g_prevCam.view, b)) return true;
    float dx = a.m[12] - b.m[12], dy = a.m[13] - b.m[13], dz = a.m[14] - b.m[14];
    return dx * dx + dy * dy + dz * dz > g_resetDistance * g_resetDistance;
}

// true = output written. in/out/gb1/objectMotion belong to the caller; objectMotion may be nullptr.
bool RunDlaa(ID3D11DeviceContext* rhContext, ID3D11Texture2D* in, ID3D11Texture2D* out, ID3D11ShaderResourceView* gb1,
             ID3D11ShaderResourceView* objectMotion)
{
    if (!rhContext || !g_cam.valid) return false;
    if (!g_device && !InitDevice(rhContext)) return false;
    if (!g_helperStarted && !StartHelper()) return false;
    if (g_shared->state == rhdlaa::StateStarting) {
        if (WaitForSingleObject(g_helper, 0) == WAIT_OBJECT_0) Break("helper exited during start (see scripts\\RHDLAA\\RHDLAA64.log)");
        return false;   // RenderHook's TAA until the helper is up
    }
    if (g_shared->state == rhdlaa::StateFailed) {
        Log("helper: %s", g_shared->message);
        Break("helper could not start DLSS");
        return false;
    }

    bool ok = false;
    if (in && out && gb1) {
        D3D11_TEXTURE2D_DESC inDesc, outDesc;
        in->GetDesc(&inDesc);
        out->GetDesc(&outDesc);
        if (inDesc.Format != outDesc.Format || inDesc.Width != outDesc.Width || inDesc.Height != outDesc.Height) {
            Log("input/output rasters differ (format %d/%d)", inDesc.Format, outDesc.Format);
            Break("unexpected raster formats");
        } else if ((g_created && inDesc.Width == g_width && inDesc.Height == g_height && inDesc.Format == g_format) || CreateTextures(inDesc)) {
            ID3D11Buffer* rhState = nullptr;
            g_context->PSGetConstantBuffers(0, 1, &rhState);
            g_context->CopyResource(g_color, in);
            DrawMotionVectors(gb1, objectMotion, rhState);
            Release(rhState);

            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            g_shared->frameTimeMs = g_lastFrame.QuadPart ? static_cast<float>((now.QuadPart - g_lastFrame.QuadPart) * 1000.0 / freq.QuadPart) : 16.7f;
            g_lastFrame = now;
            // jitter RenderHook added to _31/_32 (NDC) -> pixels, y down
            g_shared->jitterX = g_jitterSignX * (g_cam.jitteredProj.m[8] - g_cam.proj.m[8]) * 0.5f * g_width;
            g_shared->jitterY = g_jitterSignY * -(g_cam.jitteredProj.m[9] - g_cam.proj.m[9]) * 0.5f * g_height;
            g_shared->reset = (g_reset || CameraJumped()) ? 1 : 0;
            g_shared->fenceWait = ++g_fenceValue;
            g_shared->fenceSignal = ++g_fenceValue;
            g_context->Signal(g_fence, g_shared->fenceWait);
            g_context->Flush();
            if (Request(rhdlaa::CmdEvaluate, 500)) {
                g_context->Wait(g_fence, g_shared->fenceSignal);
                g_context->CopyResource(out, g_output);
                g_reset = false;
                ok = true;
            } else if (!g_broken) {
                static int failures;
                if (++failures > 30) Break("DLAA evaluation keeps failing (see scripts\\RHDLAA\\RHDLAA64.log)");
            }
        }
    }
    return ok;
}

// 2021 release: RenderHook's rasters
bool RunDlaaOnRasters(void* input, void* output, void* gbuffer1)
{
    ID3D11Texture2D* in = RasterTexture(input);
    ID3D11Texture2D* out = RasterTexture(output);
    bool ok = in && out && RunDlaa(RhContext(), in, out, RasterSrv(gbuffer1), nullptr);
    Release(in);
    Release(out);
    return ok;
}

void CallTaa(void* input, void* output, void* gbuffer1, void* gbuffer2)
{
    void* fn = g_taaRender;
    __asm {
        mov ecx, input
        mov edx, output
        push gbuffer2
        push gbuffer1
        call fn
        add esp, 8
    }
}

void CheckToggleKey()
{
    if (g_toggleKey) {
        bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
        if (down && !g_keyDown) {
            g_active = !g_active;
            g_reset = true;
            Log("toggle: %s%s", g_active ? "DLAA" : "RenderHook TAA", g_broken ? " (DLAA unavailable, see above)" : "");
        }
        g_keyDown = down;
    }
}

// DLAA replaced RenderHook's TAA in the last frame (RHDLAAIsActive)
bool g_dlaaLastFrame = false;

// For other plugins (GetProcAddress): nonzero when DLAA replaced RenderHook's TAA in the last frame. RHWind uses it to
// keep foliage out of the blended pass, whose mix with deferred-lit leaves DLAA shows as speckle.
extern "C" __declspec(dllexport) int __cdecl RHDLAAIsActive()
{
    return g_dlaaLastFrame ? 1 : 0;
}

void __cdecl TaaHook(void* input, void* output, void* gbuffer1, void* gbuffer2)
{
    CheckToggleKey();
    bool done = g_enabled && g_active && !g_broken && RunDlaaOnRasters(input, output, gbuffer1);
    g_dlaaLastFrame = done;
    g_jitterActive = done;
    if (!done) CallTaa(input, output, gbuffer1, gbuffer2);
    g_prevCam = g_cam;
    g_cam.valid = false;
}

// RenderHook with RHSetTemporalAAHandler: called in place of its TAA; false = RenderHook runs its own TAA
bool __cdecl TemporalAAHandler(const RHTemporalAAFrame* frame)
{
    // builds before object motion vectors pass a shorter frame
    if (!frame || frame->size < offsetof(RHTemporalAAFrame, objectMotion)) return false;
    ID3D11ShaderResourceView* objectMotion = frame->size >= sizeof(RHTemporalAAFrame) ? frame->objectMotion : nullptr;
    CheckToggleKey();
    memcpy(g_cam.view.m, frame->view, sizeof(Mat));
    memcpy(g_cam.proj.m, frame->projection, sizeof(Mat));
    g_cam.jitteredProj = g_cam.proj;
    g_cam.jitteredProj.m[8] += frame->jitterX;   // RenderHook adds the jitter to _31/_32
    g_cam.jitteredProj.m[9] += frame->jitterY;
    g_cam.valid = true;
    bool done = g_enabled && g_active && !g_broken && RunDlaa(frame->context, frame->input, frame->output, frame->gbuffer1, objectMotion);
    g_dlaaLastFrame = done;
    g_prevCam = g_cam;
    g_cam.valid = false;
    return done;
}

__declspec(naked) void TaaThunk()
{
    __asm {
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        push edx
        push ecx
        call TaaHook
        add esp, 16
        ret
    }
}

// ------------------------------------------------------------------------------------------------ install
bool RedirectCall(uint8_t* at, void* expected, void* to)
{
    if (at[0] != 0xE8 || at + 5 + *reinterpret_cast<int32_t*>(at + 1) != expected) return false;
    DWORD prot;
    VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &prot);
    *reinterpret_cast<int32_t*>(at + 1) = static_cast<int32_t>(static_cast<uint8_t*>(to) - (at + 5));
    VirtualProtect(at + 1, 4, prot, &prot);
    return true;
}

void Install()
{
    g_rh = reinterpret_cast<uint8_t*>(GetModuleHandleA("_gtaRenderHook.asi"));
    if (!g_rh) { Log("_gtaRenderHook.asi not loaded"); return; }
    typedef void(__cdecl* SetHandlerFn)(RHTemporalAAHandler);
    if (auto setHandler = reinterpret_cast<SetHandlerFn>(GetProcAddress(reinterpret_cast<HMODULE>(g_rh), "RHSetTemporalAAHandler"))) {
        setHandler(&TemporalAAHandler);
        Log("registered with RenderHook's temporal AA handler API; DLAA %s, preset %d, toggle key 0x%X",
            g_enabled ? "enabled" : "disabled", g_preset, g_toggleKey);
        return;
    }
    g_taaRender = g_rh + kTaaRender;
    g_jitterFn = g_rh + kJitterFn;
    // check both sites first, so an unknown build is left untouched
    uint8_t* taa = g_rh + kTaaCall;
    uint8_t* jit = g_rh + kJitterCall;
    if (taa[0] != 0xE8 || taa + 5 + *reinterpret_cast<int32_t*>(taa + 1) != g_taaRender ||
        jit[0] != 0xE8 || jit + 5 + *reinterpret_cast<int32_t*>(jit + 1) != g_jitterFn) {
        Log("unsupported _gtaRenderHook.asi build (expected the 2021-09-11 one), nothing patched");
        return;
    }
    RedirectCall(taa, g_taaRender, reinterpret_cast<void*>(&TaaThunk));
    RedirectCall(jit, g_jitterFn, reinterpret_cast<void*>(&JitterHook));
    Log("hooks installed; DLAA %s, preset %d, toggle key 0x%X", g_enabled ? "enabled" : "disabled", g_preset, g_toggleKey);
}

float IniFloat(const char* section, const char* key, const char* def, const char* path)
{
    char buf[32];
    GetPrivateProfileStringA(section, key, def, buf, sizeof buf, path);
    return static_cast<float>(atof(buf));
}

}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        char path[MAX_PATH];
        GetModuleFileNameA(mod, path, MAX_PATH);
        if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".log");
        g_log = _fsopen(path, "w", _SH_DENYNO);
        Log("RHDLAA 1.2");
        if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".ini");
        g_enabled = GetPrivateProfileIntA("DLAA", "Enabled", 1, path) != 0;
        g_preset = GetPrivateProfileIntA("DLAA", "Preset", 0, path);
        char key[32];
        GetPrivateProfileStringA("DLAA", "ToggleKey", "0x78", key, sizeof key, path);
        g_toggleKey = static_cast<int>(strtol(key, nullptr, 0));
        g_jitterSignX = IniFloat("DLAA", "JitterSignX", "1", path) < 0 ? -1.0f : 1.0f;
        g_jitterSignY = IniFloat("DLAA", "JitterSignY", "1", path) < 0 ? -1.0f : 1.0f;
        g_resetDistance = IniFloat("DLAA", "ResetDistance", "30", path);
        Install();
    }
    return TRUE;
}
