# RHDLAA: NVIDIA DLAA for GTA SA RenderHook

An ASI plugin that replaces [gtaRenderHook](https://github.com/petrgeorgievsky/gtaRenderHook)'s TAA with **NVIDIA DLAA** (DLSS at native resolution) in GTA San Andreas. The result is a sharper, much more stable image, with far less shimmer and grain on foliage, fences, wires and distant detail.

## Requirements

- An NVIDIA **RTX** graphics card (20 series or newer) and a recent driver.
- Windows 10 or 11.
- GTA San Andreas 1.0 US exe.
- One of these `_gtaRenderHook.asi` builds:
  - a build with RenderHook's plugin API (`RHSetTemporalAAHandler`, [gtaRenderHook PR #35](https://github.com/petrgeorgievsky/gtaRenderHook/pull/35)). RHDLAA registers itself through it and patches nothing, so it keeps working with later builds;
  - the 2021-09-11 release, which has no API. RHDLAA patches fixed offsets in that binary.

  Any other build is detected and left untouched.
- An ASI loader.

## Install

1. Download `RHDLAA-x.y.zip` from [Releases](../../releases). Copy its `scripts` folder into the game folder:
   ```
   scripts\RHDLAA.asi
   scripts\RHDLAA.ini
   scripts\RHDLAA\RHDLAA64.exe
   ```
2. Get NVIDIA's DLSS runtime, `nvngx_dlss.dll`, from NVIDIA's official DLSS SDK repository: [NVIDIA/DLSS → lib/Windows_x86_64/rel/nvngx_dlss.dll](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel). Use the file's *Download raw file* button. Put it next to the helper:
   ```
   scripts\RHDLAA\nvngx_dlss.dll
   ```
   Tested with DLSS 310.9.16. The DLL is NVIDIA's and is not included here.
3. In RenderHook's `settings.xml`, in the `<TAA ... />` line:
   - keep `EnableTAA="true"`. RHDLAA runs in place of TAA, so this must stay on.
   - set `SubPixelScale="1"`. This gives DLAA a full pixel of jitter; the default 0.7 works, but less well.
4. Start the game. For the first second or so the old TAA is shown while DLSS loads, then DLAA takes over. Press **F9** to switch between DLAA and RenderHook's TAA to compare.

`scripts\RHDLAA.log` and `scripts\RHDLAA\RHDLAA64.log` show what happened. The log's second line says whether RHDLAA registered through RenderHook's API or hooked the 2021 release. A line `DLAA on: <width>x<height>` means it's working. If anything fails (no RTX card, DLL missing, old driver), RenderHook's TAA keeps running and the logs say why.

**To uninstall:** delete `scripts\RHDLAA.asi`, `scripts\RHDLAA.ini` and the `scripts\RHDLAA` folder.

## Settings (`RHDLAA.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 1 | 0 = RenderHook's TAA as before. |
| `Preset` | 0 | DLSS preset: 0 = NVIDIA's default (K, transformer model), 10 = J (a bit less ghosting, a bit more flicker), 11 = K. |
| `ToggleKey` | 0x78 | Key that switches DLAA / TAA in game, as a Windows virtual-key code (0x78 = F9, 0 = none). |
| `JitterSignX`, `JitterSignY` | 1 | Set to -1 if the image looks soft or wobbles slightly when standing still. 1 is correct on the tested setup. |
| `ResetDistance` | 30 | A camera jump farther than this (metres in one frame), such as a cutscene cut, starts DLAA's history fresh. |

## Frame generation (RTX 40 series and newer only)

`framegen\` holds an experimental companion to RHDLAA: **NVIDIA DLSS Frame Generation** (one generated frame per rendered frame) for RenderHook, built on the same motion vectors and depth.

> **Works only on GeForce RTX 40 series and newer.** NVIDIA allows DLSS Frame Generation on RTX 40/50 only. On RTX 20/30 and other cards Streamline reports it unsupported, and the helper just presents the frames without generating any (a little added latency, nothing gained): leave it off there.

It needs a RenderHook build with the frame generation bridge, from [spartaque12/gtaRenderHook, branch `feature/framegen-bridge`](https://github.com/spartaque12/gtaRenderHook/tree/feature/framegen-bridge) (not upstream). That build also adds a HUD layer (the HUD is kept out of the interpolation), NVIDIA Reflex, object motion vectors and the shader cache.

**How it works.** Frame generation needs D3D12 and a 64-bit process; RenderHook is D3D11 and 32-bit. So RenderHook hands every finished frame, its depth, motion vectors, camera, the frame without HUD and the HUD layer to `RHFrameGen64.exe` through shared textures and fences. The helper presents it with [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) (DLSS-G, Reflex) in a borderless, click-through window over the game's client area. The game keeps focus and input. The frame is handed over from inside DXGI's `Present`, so overlays that draw there (CheatMenu, ...) are included.

**Install.** Download `RHFrameGen-1.0.zip` from [Releases](../../releases) (or build it, below) and copy its `RHFrameGen` folder next to `_gtaRenderHook.asi`. Add NVIDIA's Streamline DLLs, which are not included: from the [Streamline 2.x release](https://github.com/NVIDIA-RTX/Streamline/releases) (tested with 2.14.16), copy `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `nvngx_dlssg.dll` from `bin\x64` into `RHFrameGen\`. In `settings.xml`, in the `<Debug ... />` line: `FrameGenBridge="true"`, and for the HUD to stay sharp `HudLayer="true"`; `NvReflex="true"` adds Reflex on the game's side.

**In game.** F11 switches frame generation on and off, F5 hides the helper's window to compare with the game's own output. Both keys are set in `RHFrameGen\RHFrameGen64.ini` (F1..F24, a virtual-key code, or `none`). Logs: `RHFrameGen\RHFrameGen64.log`, `RHFrameGen\sl.log`, `RHFrameGen\RHFrameGenBridge.log`. If the helper fails, the game carries on with RenderHook's own output.

**Measured** (RTX 4070 Ti SUPER, 1440p, CPU-bound at ~68 fps): ~130–145 fps on screen. Game frame start to screen: 22.9 ms without the bridge, 23.3 ms with the bridge and frame generation off, ~36 ms (median) with frame generation on, about one frame more, as with any frame interpolation.

## Known limitations

- Object motion needs a RenderHook with object motion vectors ([gtaRenderHook PR #53](https://github.com/petrgeorgievsky/gtaRenderHook/pull/53)). There cars, peds (limbs included) and other moving objects get their own motion, and RHDLAA 1.2+ uses it. With other builds, motion vectors come from the camera only, so fast cars can leave a faint trail. Swaying trees have no motion of their own either way.
- Costs roughly 1–1.5 ms per frame at 1440p on an RTX 4070 Ti SUPER, plus about 0.2 ms for the handoff to the helper.

## How it works

GTA SA is a 32-bit game, and NVIDIA ships the DLSS runtime (NGX) for 64-bit only, so it can't be loaded into the game. Instead, `RHDLAA.asi` starts a small 64-bit helper, `RHDLAA64.exe`, that hosts DLSS on its own D3D11 device on the same GPU, and they share work every frame:

1. **Hook.** RHDLAA takes over RenderHook's TAA step, which works on the HDR image before tonemapping, where DLAA belongs:
   - With RenderHook's plugin API, it registers a handler with `RHSetTemporalAAHandler`. RenderHook calls it each frame with the context, the input and output textures, G-buffer 1, the jitter, and the view and unjittered projection matrices. If it returns false (F9, or DLAA not ready), RenderHook runs its own TAA.
   - On the 2021-09-11 release, the call to `CTemporalAA::Render` in `RenderTonemappedOutput` (at `+0xDBB32`) is redirected, and the matrices are read around `CTemporalAA::JitterProjMatrix`.
2. **Inputs.** The HDR colour is copied into a texture shared with the helper (D3D11 shared NT handle). A fullscreen pass writes motion vectors and hardware-style depth into two more shared textures, reconstructed from the view depth in RenderHook's G-buffer and the current and previous camera matrices. Where RenderHook passes object motion (`RHTemporalAAFrame::objectMotion`, PR #53), pixels marked as moving use that instead of camera motion.
3. **Sync.** The game signals a shared `ID3D11Fence` and wakes the helper. The helper makes its GPU wait for that fence, runs DLAA into a shared output texture and signals the fence again. The game's GPU waits for that and copies the result into RenderHook's output. The GPUs synchronise directly; the CPU waits only for the helper to *submit* its work.
4. **Jitter.** In the 2021-09-11 release, `CTemporalAA::JitterProjMatrix` never advances its sample index (`x = (x++) % n` assigns the old value back), so its TAA never actually jittered. There RHDLAA advances the index before each call and measures the offset RenderHook applied. The bug is fixed upstream in [gtaRenderHook PR #31](https://github.com/petrgeorgievsky/gtaRenderHook/pull/31), and with the plugin API RenderHook passes the jitter directly.

Other plugins can ask whether DLAA ran in the last frame through the export `int RHDLAAIsActive()` (nonzero = DLAA replaced RenderHook's TAA). [RHWind](https://github.com/spartaque12/RHWind) 1.6 uses it to draw foliage alpha-tested only while DLAA runs.

The helper exits with the game: it watches the game process and sits in a kill-on-close job object. If it fails or stops answering, the plugin falls back to RenderHook's TAA for the rest of the session.

## Building

Visual Studio 2019/2022 or Build Tools with the C++ x86 and x64 compilers, and the Windows SDK, which provides `fxc`.

1. From NVIDIA's [DLSS SDK](https://github.com/NVIDIA/DLSS), copy into `third_party\ngx\`:
   - `include\*`
   - `lib\Windows_x86_64\x64\nvsdk_ngx_s.lib` → `third_party\ngx\lib\`
   - `lib\Windows_x86_64\rel\nvngx_dlss.dll` → `third_party\ngx\lib\`
2. Run `build.bat`. The output goes to `bin\`.

`test\HelperTest.cpp` is a 32-bit test that shares textures the way the plugin does, runs a few DLAA frames through the helper and reads the result back.

Frame generation helper: from [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) 2.x (tested with 2.14.16), copy `include\`, `lib\x64\sl.interposer.lib` and from `bin\x64\` `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` into `framegen\third_party\streamline\` (same layout), then run `framegen\build.bat`. The output goes to `framegen\bin\RHFrameGen\`. `framegen\src\FrameGenShared.h` is the protocol shared with RenderHook and must match the copy in the RenderHook build.

The DLSS SDK, Streamline and `nvngx_dlss.dll` / `nvngx_dlssg.dll` are © NVIDIA, under the NVIDIA RTX SDKs license.
