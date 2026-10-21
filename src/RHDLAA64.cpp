// RHDLAA64 - 64-bit helper for RHDLAA.asi. Started by the game with its process id; runs NVIDIA DLAA on textures the
// game shares with it, and exits when the game does.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <share.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "Shared.h"

namespace {

FILE* g_log;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

rhdlaa::Shared* g_shared;
ID3D11Device5* g_device;
ID3D11DeviceContext4* g_context;
NVSDK_NGX_Parameter* g_params;
NVSDK_NGX_Handle* g_feature;
ID3D11Texture2D *g_color, *g_depth, *g_motion, *g_output;
ID3D11Fence* g_fence;

void Fail(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    vsnprintf(g_shared->message, sizeof g_shared->message, fmt, a);
    va_end(a);
    Log("%s", g_shared->message);
    g_shared->state = rhdlaa::StateFailed;
}

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool CreateDevice()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == g_shared->adapterLuidLow && desc.AdapterLuid.HighPart == g_shared->adapterLuidHigh) {
            Log("adapter: %ls", desc.Description);
            break;
        }
        Release(adapter);
    }
    factory->Release();
    if (!adapter) { Log("adapter of the game not found"); return false; }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &device, nullptr, &context);
    adapter->Release();
    if (FAILED(hr)) { Log("D3D11CreateDevice failed %08X", hr); return false; }
    device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_device));
    context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_context));
    device->Release();
    context->Release();
    return g_device && g_context;
}

bool InitNgx()
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("6c2a1d9e-4f3b-4a87-9d51-3e8b7f20c4a1", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                             "1.0", dir, g_device);
    if (NVSDK_NGX_FAILED(r)) { Fail("NGX init failed (0x%08X). Is the NVIDIA driver up to date?", r); return false; }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r)) { Fail("NGX capability parameters failed (0x%08X)", r); return false; }
    int available = 0, needsDriver = 0;
    unsigned int major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    if (needsDriver) { Fail("DLSS needs NVIDIA driver %u.%u or newer", major, minor); return false; }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
        int code = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &code);
        Fail("DLSS not available on this GPU/driver (0x%08X). Needs an RTX card and nvngx_dlss.dll next to RHDLAA64.exe", code);
        return false;
    }
    snprintf(g_shared->message, sizeof g_shared->message, "DLSS ready");
    return true;
}

void ReleaseFeature()
{
    if (g_feature) { NVSDK_NGX_D3D11_ReleaseFeature(g_feature); g_feature = nullptr; }
    Release(g_color); Release(g_depth); Release(g_motion); Release(g_output); Release(g_fence);
}

ID3D11Texture2D* OpenTexture(uint64_t handle)
{
    ID3D11Texture2D* tex = nullptr;
    HANDLE h = reinterpret_cast<HANDLE>(handle);
    HRESULT hr = g_device->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    CloseHandle(h);
    if (FAILED(hr)) Log("OpenSharedResource1 failed %08X", hr);
    return tex;
}

bool Create()
{
    ReleaseFeature();
    g_color = OpenTexture(g_shared->hColor);
    g_depth = OpenTexture(g_shared->hDepth);
    g_motion = OpenTexture(g_shared->hMotion);
    g_output = OpenTexture(g_shared->hOutput);
    HANDLE hf = reinterpret_cast<HANDLE>(g_shared->hFence);
    HRESULT hr = g_device->OpenSharedFence(hf, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_fence));
    CloseHandle(hf);
    if (FAILED(hr)) Log("OpenSharedFence failed %08X", hr);
    if (!g_color || !g_depth || !g_motion || !g_output || !g_fence) return false;

    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, g_shared->preset);
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = create.Feature.InTargetWidth = g_shared->width;
    create.Feature.InHeight = create.Feature.InTargetHeight = g_shared->height;
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(g_context, &g_feature, g_params, &create);
    if (NVSDK_NGX_FAILED(r)) { Log("DLSS feature creation failed 0x%08X", r); g_feature = nullptr; return false; }
    Log("DLAA feature %ux%u, preset %d", g_shared->width, g_shared->height, g_shared->preset);
    return true;
}

bool Evaluate()
{
    if (!g_feature) return false;
    g_context->Wait(g_fence, g_shared->fenceWait);
    NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = g_color;
    eval.Feature.pInOutput = g_output;
    eval.pInDepth = g_depth;
    eval.pInMotionVectors = g_motion;
    eval.InJitterOffsetX = g_shared->jitterX;
    eval.InJitterOffsetY = g_shared->jitterY;
    eval.InRenderSubrectDimensions = { g_shared->width, g_shared->height };
    eval.InReset = g_shared->reset;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    eval.InFrameTimeDeltaInMsec = g_shared->frameTimeMs;
    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(g_context, g_feature, g_params, &eval);
    g_context->Signal(g_fence, g_shared->fenceSignal);
    g_context->Flush();
    if (NVSDK_NGX_FAILED(r)) {
        static int reported;
        if (reported++ < 5) Log("DLSS evaluate failed 0x%08X", r);
        return false;
    }
    return true;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
    const uint32_t pid = wcstoul(cmdLine, nullptr, 10);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* dot = strrchr(path, '.')) strcpy_s(dot, path + MAX_PATH - dot, ".log");
    g_log = _fsopen(path, "w", _SH_DENYNO);
    Log("RHDLAA64 1.0, game pid %u", pid);

    char mapName[64], reqName[64], doneName[64];
    rhdlaa::Names(pid, mapName, reqName, doneName, sizeof mapName);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapName);
    HANDLE request = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, reqName);
    HANDLE done = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, doneName);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!mapping || !request || !done || !game) { Log("cannot open the game's shared objects"); return 1; }
    g_shared = static_cast<rhdlaa::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rhdlaa::Shared)));
    if (!g_shared || g_shared->version != rhdlaa::kVersion) { Log("protocol version mismatch"); return 1; }

    if (!CreateDevice()) Fail("could not create a D3D11 device on the game's GPU");
    else if (InitNgx()) g_shared->state = rhdlaa::StateReady;
    Log("%s", g_shared->message);
    SetEvent(done);
    if (g_shared->state != rhdlaa::StateReady) return 1;

    HANDLE waits[2] = { request, game };
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        bool ok = false;
        switch (g_shared->cmd) {
        case rhdlaa::CmdCreate: ok = Create(); break;
        case rhdlaa::CmdEvaluate: ok = Evaluate(); break;
        }
        g_shared->result = ok ? 1 : 0;
        SetEvent(done);
    }
    Log("game exited");
    ReleaseFeature();
    if (g_params) NVSDK_NGX_D3D11_DestroyParameters(g_params);
    NVSDK_NGX_D3D11_Shutdown1(g_device);
    return 0;
}
