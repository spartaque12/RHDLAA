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

## Known limitations

- Motion vectors come from the camera only. Cars, peds and swaying trees have no motion of their own, the same as with RenderHook's TAA. DLAA handles that better than the TAA did, but fast cars can leave a faint trail.
- Costs roughly 1–1.5 ms per frame at 1440p on an RTX 4070 Ti SUPER, plus about 0.2 ms for the handoff to the helper.

## How it works

GTA SA is a 32-bit game, and NVIDIA ships the DLSS runtime (NGX) for 64-bit only, so it can't be loaded into the game. Instead, `RHDLAA.asi` starts a small 64-bit helper, `RHDLAA64.exe`, that hosts DLSS on its own D3D11 device on the same GPU, and they share work every frame:

1. **Hook.** RHDLAA takes over RenderHook's TAA step, which works on the HDR image before tonemapping, where DLAA belongs:
   - With RenderHook's plugin API, it registers a handler with `RHSetTemporalAAHandler`. RenderHook calls it each frame with the context, the input and output textures, G-buffer 1, the jitter, and the view and unjittered projection matrices. If it returns false (F9, or DLAA not ready), RenderHook runs its own TAA.
   - On the 2021-09-11 release, the call to `CTemporalAA::Render` in `RenderTonemappedOutput` (at `+0xDBB32`) is redirected, and the matrices are read around `CTemporalAA::JitterProjMatrix`.
2. **Inputs.** The HDR colour is copied into a texture shared with the helper (D3D11 shared NT handle). A fullscreen pass writes camera motion vectors and hardware-style depth into two more shared textures, reconstructed from the view depth in RenderHook's G-buffer and the current and previous camera matrices.
3. **Sync.** The game signals a shared `ID3D11Fence` and wakes the helper. The helper makes its GPU wait for that fence, runs DLAA into a shared output texture and signals the fence again. The game's GPU waits for that and copies the result into RenderHook's output. The GPUs synchronise directly; the CPU waits only for the helper to *submit* its work.
4. **Jitter.** In the 2021-09-11 release, `CTemporalAA::JitterProjMatrix` never advances its sample index (`x = (x++) % n` assigns the old value back), so its TAA never actually jittered. There RHDLAA advances the index before each call and measures the offset RenderHook applied. The bug is fixed upstream in [gtaRenderHook PR #31](https://github.com/petrgeorgievsky/gtaRenderHook/pull/31), and with the plugin API RenderHook passes the jitter directly.

The helper exits with the game: it watches the game process and sits in a kill-on-close job object. If it fails or stops answering, the plugin falls back to RenderHook's TAA for the rest of the session.

## Building

Visual Studio 2019/2022 or Build Tools with the C++ x86 and x64 compilers, and the Windows SDK, which provides `fxc`.

1. From NVIDIA's [DLSS SDK](https://github.com/NVIDIA/DLSS), copy into `third_party\ngx\`:
   - `include\*`
   - `lib\Windows_x86_64\x64\nvsdk_ngx_s.lib` → `third_party\ngx\lib\`
   - `lib\Windows_x86_64\rel\nvngx_dlss.dll` → `third_party\ngx\lib\`
2. Run `build.bat`. The output goes to `bin\`.

`test\HelperTest.cpp` is a 32-bit test that shares textures the way the plugin does, runs a few DLAA frames through the helper and reads the result back.

The DLSS SDK and `nvngx_dlss.dll` are © NVIDIA, under the NVIDIA RTX SDKs license.
