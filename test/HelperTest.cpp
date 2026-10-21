// x86 test for RHDLAA64.exe: shares textures like RHDLAA.asi does, runs a few DLAA frames, reads the output back.
#include <windows.h>
#include <d3d11_4.h>
#include <cstdio>
#include <cmath>
#include "../src/Shared.h"

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED %s: %08lX\n", #x, hr_); return 1; } } while (0)

static HANDLE Share(ID3D11Texture2D* t, HANDLE proc)
{
    IDXGIResource1* r; HANDLE h = nullptr, d = nullptr;
    t->QueryInterface(__uuidof(IDXGIResource1), (void**)&r);
    r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    r->Release();
    DuplicateHandle(GetCurrentProcess(), h, proc, &d, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(h);
    return d;
}

int main(int argc, char** argv)
{
    ID3D11Device* dev0; ID3D11DeviceContext* ctx0;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev0, nullptr, &ctx0));
    ID3D11Device5* dev; ID3D11DeviceContext4* ctx;
    CHECK(dev0->QueryInterface(__uuidof(ID3D11Device5), (void**)&dev));
    CHECK(ctx0->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&ctx));

    DWORD pid = GetCurrentProcessId();
    char m[64], q[64], d[64];
    rhdlaa::Names(pid, m, q, d, sizeof m);
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(rhdlaa::Shared), m);
    auto* s = (rhdlaa::Shared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rhdlaa::Shared));
    HANDLE req = CreateEventA(nullptr, FALSE, FALSE, q), done = CreateEventA(nullptr, FALSE, FALSE, d);
    memset(s, 0, sizeof *s);
    s->version = rhdlaa::kVersion;
    IDXGIDevice* dx; IDXGIAdapter* ad; DXGI_ADAPTER_DESC desc;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx); dx->GetAdapter(&ad); ad->GetDesc(&desc);
    s->adapterLuidLow = desc.AdapterLuid.LowPart; s->adapterLuidHigh = desc.AdapterLuid.HighPart;
    printf("adapter %ls\n", desc.Description);

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" %lu", argv[1], pid);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(argv[1], cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { printf("start failed %lu\n", GetLastError()); return 1; }
    DWORD t0 = GetTickCount();
    if (WaitForSingleObject(done, 20000) != WAIT_OBJECT_0) { printf("no answer\n"); return 1; }
    printf("helper state %u (%lu ms): %s\n", s->state, GetTickCount() - t0, s->message);
    if (s->state != rhdlaa::StateReady) return 1;

    const UINT W = 1920, H = 1080;
    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    // colour: a ramp so the output is recognisable
    float* half = nullptr; (void)half;
    ID3D11Texture2D *color, *out, *depth, *mv;
    CHECK(dev->CreateTexture2D(&td, nullptr, &color));
    td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    CHECK(dev->CreateTexture2D(&td, nullptr, &out));
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.Format = DXGI_FORMAT_R32_FLOAT;
    CHECK(dev->CreateTexture2D(&td, nullptr, &depth));
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    CHECK(dev->CreateTexture2D(&td, nullptr, &mv));
    ID3D11RenderTargetView* rtv; ID3D11Texture2D* colorRt;
    D3D11_TEXTURE2D_DESC cd = { W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    CHECK(dev->CreateTexture2D(&cd, nullptr, &colorRt));
    CHECK(dev->CreateRenderTargetView(colorRt, nullptr, &rtv));
    float c[4] = { 0.25f, 0.5f, 0.75f, 1 };
    ctx->ClearRenderTargetView(rtv, c);
    ctx->CopyResource(color, colorRt);

    ID3D11Fence* fence;
    CHECK(dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&fence));
    HANDLE fh, fd;
    CHECK(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh));
    DuplicateHandle(GetCurrentProcess(), fh, pi.hProcess, &fd, 0, FALSE, DUPLICATE_SAME_ACCESS);
    s->hColor = (uint64_t)Share(color, pi.hProcess); s->hDepth = (uint64_t)Share(depth, pi.hProcess);
    s->hMotion = (uint64_t)Share(mv, pi.hProcess); s->hOutput = (uint64_t)Share(out, pi.hProcess); s->hFence = (uint64_t)fd;
    s->width = W; s->height = H;
    s->cmd = rhdlaa::CmdCreate; SetEvent(req);
    t0 = GetTickCount();
    WaitForSingleObject(done, 20000);
    printf("create result %u (%lu ms)\n", s->result, GetTickCount() - t0);
    if (!s->result) return 1;

    uint64_t v = 0;
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
    for (int i = 0; i < 20; ++i) {
        QueryPerformanceCounter(&a);
        s->jitterX = 0.1f * (i % 4); s->jitterY = -0.1f * (i % 3); s->reset = i == 0; s->frameTimeMs = 16;
        s->fenceWait = ++v; s->fenceSignal = ++v;
        ctx->Signal(fence, s->fenceWait); ctx->Flush();
        s->cmd = rhdlaa::CmdEvaluate; SetEvent(req);
        if (WaitForSingleObject(done, 2000) != WAIT_OBJECT_0 || !s->result) { printf("evaluate %d failed\n", i); return 1; }
        ctx->Wait(fence, s->fenceSignal);
        QueryPerformanceCounter(&b);
        if (i % 5 == 0) printf("frame %d round trip %.3f ms (CPU)\n", i, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
    }
    D3D11_TEXTURE2D_DESC sd = { W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ID3D11Texture2D* st; CHECK(dev->CreateTexture2D(&sd, nullptr, &st));
    ctx->CopyResource(st, out);
    D3D11_MAPPED_SUBRESOURCE ms; CHECK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms));
    const uint16_t* px = (const uint16_t*)((const uint8_t*)ms.pData + ms.RowPitch * (H / 2)) + (W / 2) * 4;
    auto h2f = [](uint16_t h) { uint32_t e = (h >> 10) & 31, m2 = h & 1023; float r = e ? ldexpf(1.0f + m2 / 1024.0f, e - 15) : ldexpf(m2 / 1024.0f, -14); return (h & 0x8000) ? -r : r; };
    printf("output centre pixel: %.3f %.3f %.3f (input 0.25 0.5 0.75)\n", h2f(px[0]), h2f(px[1]), h2f(px[2]));
    ctx->Unmap(st, 0);
    TerminateProcess(pi.hProcess, 0);
    return 0;
}

