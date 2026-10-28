// RHDLAA: motion vectors and hardware-style depth for DLAA, from RenderHook's G-buffer (view depth in gbuffer 1
// .zw, see GBuffer.hlsl EncodeFloatRG). Camera motion from depth everywhere; where RenderHook wrote object motion
// vectors (vehicles, peds; RenderHook builds with RHTemporalAAFrame::objectMotion) those are used instead.

// RenderHook's RenderStateBuffer, only up to fFarClip
cbuffer RenderStateBuffer : register(b0)
{
    uint bHasTexture; float fScreenWidth; float fScreenHeight; uint uiAlphaTestType;
    float fAlphaTestRef; uint bFogEnable; uint uiFogType; uint cFogColor;
    float4 vCameraPos, vSunLightDir, vSkyLightCol, vHorizonCol, vSunColor, vWaterColor, vGradingColor0, vGradingColor1;
    uint uiLightCount; float fFogStart; float fFogRange; float fFarClip;
}

cbuffer Reprojection : register(b1)
{
    row_major float4x4 mViewInv;        // view -> world, this frame
    row_major float4x4 mViewProj;       // world -> clip, this frame, without jitter
    row_major float4x4 mPrevViewProj;   // world -> clip, previous frame, without jitter
    float4 vProj;                       // jittered projection: _11, _22, _31, _32
    float2 vScreen;
    float fObjectMotion;                // 1: txObjectMotion is bound
}

Texture2D txNormalDepth : register(t0);
// RenderHook's object motion: xy pixels current -> previous (no jitter), z = 1 where an object wrote it
Texture2D txObjectMotion : register(t1);

void VS(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 ClipToPixel(float4 clip)
{
    return (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * vScreen;
}

struct Output
{
    float2 motion : SV_Target0;     // pixels, current -> previous
    float depth : SV_Target1;
};

Output PS(float4 pos : SV_Position)
{
    float2 enc = txNormalDepth.Load(int3(pos.xy, 0)).zw;
    float viewZ = dot(enc, float2(1.0, 1.0 / 255.0)) * fFarClip;
    float2 ndc = pos.xy / vScreen * float2(2, -2) + float2(-1, 1);
    // ray through this pixel as the jittered camera saw it, view space (z forward)
    float3 ray = float3((ndc.x - vProj.z) / vProj.x, (ndc.y - vProj.w) / vProj.y, 1.0);

    Output o;
    float4 cur, prev;
    if (viewZ <= 0.0) {
        // sky: infinitely far, only the camera rotation matters
        float4 dir = mul(float4(ray, 0), mViewInv);
        cur = mul(dir, mViewProj);
        prev = mul(dir, mPrevViewProj);
        o.depth = 1.0;
    } else {
        float4 world = mul(float4(ray * viewZ, 1), mViewInv);
        cur = mul(world, mViewProj);
        prev = mul(world, mPrevViewProj);
        o.depth = saturate(cur.z / cur.w);
    }
    o.motion = (prev.w > 0.0 && cur.w > 0.0) ? ClipToPixel(prev) - ClipToPixel(cur) : 0.0;
    if (fObjectMotion > 0.5) {
        float4 own = txObjectMotion.Load(int3(pos.xy, 0));
        if (own.z > 0.5)
            o.motion = own.xy;
    }
    return o;
}
