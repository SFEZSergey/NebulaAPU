# NebulaAPU

Experimental native Windows compatibility runtime and static relinking
research for **Astro Bot**.

> **Status as of October 1, 2026:** not a playable port. The runtime
> boots Astro Bot through its whole opening - the "Sony Interactive
> Entertainment presents" screen, the PlayStation Studios intro video with
> sound, and the Team Asobi splash - to the title screen, in a native
> Win32/Vulkan window of its own. There is no input yet, so the game goes no
> further than the title screen.

## Current Milestone

The repository contains the current Astro Bot-specific implementation:

- a native C++ Windows runtime and HLE layer;
- PS5 import/NID inventory and generated dispatch support;
- native AGC command and register decoding;
- a Gen5 (GCN/RDNA) shader to SPIR-V translator in C++, ported from
  SharpEmu's C# shader compiler;
- a native Vulkan GPU runtime with its own presentation window
  (borderless full screen by default, Alt+Enter or F11 for a window);
- a native AvPlayer (Media Foundation video decoded on the GPU through
  D3D11, waveOut sound);
- native libSceJson, semaphores and guest heap;
- the PS5 APR/AMPR asset streaming path;
- deterministic probe, capture and replay tools;
- temporary managed bridges used only for reference comparison.

What works today:

- the game's own recompiled shaders draw its frames, compute dispatches run
  with their real workgroup sizes and ids, and their results reach guest
  memory;
- the translator models EXEC per lane, so divergent branches run as they
  should, and every dumped scene stage but one passes `spirv-val` as
  structured SPIR-V;
- linear, tiled (swizzle modes 9, 24, 27 and more), block-compressed
  (BC1-BC7), 1D, 1x1 and 3D (volume) textures upload, including the top
  level of a mip chain;
- fast clears, including DCC fast clears whose colour AGC writes on the
  GPU, are applied;
- the intro video plays within about 50 ms of its sound, with no green or
  black frames in recent runs;
- the Team Asobi splash renders as in the reference capture: the nebula
  background, the logo fading in, centred, in its colours;
- the scene after the intro and the title screen draw, with memory levelling
  off instead of climbing and no GPU resets in 1300-frame runs;
- the native window is the game's display: VideoOut flips go to it, not to a
  second presenter;
- replay can carry a whole scene: captures hold the graphics states'
  SPIR-V and manifests, and BC and volume images whole.

Known problems:

- no input, so nothing past the title screen can be reached;
- the scene after the intro is slow: around 130 ms a frame on the Team Asobi
  splash, with the GPU worker the limit;
- output is at the game's 4K target, scaled to the window;
- one shader (a prologue ending in `s_setpc_b64`) still does not translate,
  and `gen5_translate_test` fails two memory-operation count checks it
  failed before the recent translator work;
- importing guest memory into Vulkan (`PS5GPU_NATIVE_IMPORT_GUEST=1`) is an
  experiment that crashes the NVIDIA driver and stays off.

## Recent Changes

From September 27 to October 1, 2026:

- **Intro video.** Decoding moved to the GPU (D3D11; `PS5RT_AVPLAYER_SOFTWARE=1`
  keeps the software path), the clock follows the sound device, and late
  frames are dropped before they are copied. Write watches are armed before
  a read, which ended the green frames; per-draw baked addresses are part of
  a state's key, which ended the black flicker.
- **Opening rendering.** Per-lane EXEC, compute workgroup ids, 3D images and
  storage images filled from guest memory fixed the magenta wedge and the
  fog behind the SIE screen; fast clears turned its grey back to black.
- **Past the intro.** Native libSceJson kept the loader thread alive; the
  AvPlayer stream info uses the PS5 layout; APR looks for missing prein
  files in every sibling directory, which ended the "Animation" crash after
  the PlayStation Studios video; thread ids stay unique for the process.
- **Team Asobi splash and title screen.** VOP3 `v_mac`, SDWA compare masks,
  1x1 and 1D images, `v_readfirstlane` VOP1, `v_fma_mix_f32`, `v_movrels`,
  buffer loads at run-time offsets and more were added or fixed in the
  translator; structured SPIR-V for every scene shader stopped the GPU
  resets; DCC fast clear colours brought the title screen out of black.
- **Speed.** Translation is several times faster (code front end, emission
  and scalar-walk results reused between draws); graphics pipelines,
  layouts and SPIR-V are shared by content; read-only buffers go through a
  per-frame arena; region caches, the guest heap and traces stopped eating
  the main thread. A scene frame went from 3.3 s to about 0.13 s.
- **Bounds.** Registered states, pending shaders, compute pipelines and
  idle graphics pipelines are capped, so a long run no longer exhausts
  memory; a per-invocation loop budget (`PS5RT_LOOP_BUDGET`) keeps a
  runaway shader loop from resetting the GPU.
- **Documentation.** [docs/agc/](docs/agc/README.md) and
  [docs/hle/](docs/hle/README.md) collect what was learned about AGC, the
  Gen5 GPU and the libraries around it.

See [ROADMAP_STATUS.md](ROADMAP_STATUS.md) and the dated notes under
[docs/](docs/) for the measured history.

What we have learned about AGC and the Gen5 GPU - packets, registers,
descriptors, shaders and how the runtime executes them - is collected in
[docs/agc/](docs/agc/README.md), and the libraries beside it (VideoOut,
AvPlayer, Json, threads, files) in [docs/hle/](docs/hle/README.md).

## Running

The launcher is `AstroBot.cycle.exe` in the locally built package. Started
with no arguments it reads `AstroBot.cycle.launch.ini` beside it (written by
`tools/astro-cycle.ps1`), which names the game files and the runtime
settings. Game files, the built package and the ini are never committed. Copy
`profiles/astrobot.example.toml` to `profiles/astrobot.toml` and fill in the
paths to your own dump; the local copy is ignored by Git.

The scripts under `tools/` and the CLI look for local tools and reference
checkouts (the .NET SDK, SharpEmu, the Vulkan headers) under one root,
`NEBULA_DEV_ROOT`, which defaults to `C:\dev`. `tools/astro-cycle.ps1` takes
the folder of your extracted dump from `-App0` or `NEBULA_APP0`.

## Tests

The native tests are single-file C++ programs under `tests/native/`.
`tests/native/run.ps1` builds and runs each with MinGW g++ on Windows. They
also build with g++ on Linux, except `gen5_manifest_test`, which includes a
Windows-only header. `gen5_translate_test` fails two memory-operation count
checks, a known gap. There is no CI yet.

## Project Direction

The immediate goal is an Astro Bot native compatibility runtime, not a general
purpose PS5 emulator.

The intended process is:

```text
legally dumped eboot.bin and modules
        |
        v
static analysis and native x86-64 relinking
        |
        v
Astro Bot-specific Windows compatibility runtime
        |
        v
native HLE + native GPU command runtime
        |
        v
Vulkan correctness backend
        |
        v
later D3D12 backend and native Windows presentation
```

A reusable general PS5 recompiler is deferred until the Astro Bot runtime can
reach stable game-produced frames. Existing `Ps5Recomp` file, namespace, and
tool names are transitional internal names retained to avoid interrupting the
current debugging path.

## Native Runtime Rule

The final game process is intended to use native C or C++ code. It must not
depend on the SharpEmu GUI or emulator loop.

The C# projects under `bridge/` and parts of `src/` are temporary diagnostic
and compiler scaffolding. Production runtime changes belong under
`runtime/native/`. Managed reference code is not counted as a completed native
milestone until the behavior is implemented in C or C++.

Parts of the runtime are ported or adapted from SharpEmu and KytyPS5 under
their licenses (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
SharpEmu, shadPS4, KytyPS5, and RPCS3 are also used as technical references
and for differential traces. None of them is bundled as a game frontend.

## Repository Layout

```text
runtime/native/   native HLE, execution, GPU and replay runtime
src/              transitional compiler and shader-analysis components
bridge/           temporary SharpEmu/compiler comparison bridges
tools/            automated probes, capture and replay scripts
profiles/         Astro Bot-specific build profile
config/           HLE policy
docs/             architecture, research and current status
```

Build products, captures, game files, generated executables, shaders extracted
from the game, and local logs are excluded from Git.

## Native GPU Build

The current runtime is built directly with MinGW C++20. CMake and Ninja are
not required for this path.

Example:

```powershell
& $env:MINGW_GXX `
  -std=c++20 -O2 -Wall -Wextra `
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DPS5GPU_NATIVE_BUILD=1 `
  -I $env:VULKAN_INCLUDE `
  -shared -pthread -static-libgcc -static-libstdc++ `
  runtime\native\ps5gpu_native.cpp `
  -o artifacts\package\Ps5GpuRuntime.dll
```

The replay tool consumes a locally generated capture. Captures and extracted
game shaders are intentionally not published.

## Roadmap

1. ~~Upload linear guest textures into native Vulkan images.~~ Done.
2. ~~Port the required PS5 detile modes, starting with tile mode 24.~~ Done
   for what the opening frames ask for: tile modes 24, 27, 5 and linear all
   upload, with no unsupported-mode refusals in a run.
3. ~~Execute or replace compute image writers used by the opening frame.~~
   Done. Dispatches run and their write-backs land.
4. ~~Find where the textures the shaders sample are supposed to come
   from.~~ Answered on 2026-09-06: the title does not read its assets
   through the file API at all - 36 opens and one `sceKernelRead` in a run
   - it uses the PS5 APR/AMPR streaming path, and of the twenty-two APR
   and AMPR imports it has, only three constructors are ever called. See
   [docs/ASSET_DELIVERY_2026-09-06.md](docs/ASSET_DELIVERY_2026-09-06.md).
5. ~~Get the asset loader past constructing its two AMPR command buffers,
   then serve the ReadFile commands it builds.~~ Done on 2026-09-07: the
   APR/AMPR path is implemented natively and the title streams 864 MB in a
   100 second run, with 227 of 32339 sampled images reading as all zero
   against 634 of 704 two days earlier. See
   [docs/ASSET_PATH_SERVED_2026-09-07.md](docs/ASSET_PATH_SERVED_2026-09-07.md).
6. ~~Find out why the loaded frame still does not reach the display
   buffer.~~ Answered on 2026-09-11: it does reach it, and it is a loading
   screen. No draw in a run carries geometry - 2335 of them are full-screen
   triangles - and the title is still working through its assets, 6613
   distinct files in thirteen minutes. See
   [docs/WHY_THE_FRAME_IS_BLACK_2026-09-11.md](docs/WHY_THE_FRAME_IS_BLACK_2026-09-11.md).
7. ~~Find where the worker's time goes.~~ Largely done on 2026-09-27:
   repeated reads of images nothing had written, byte-at-a-time copies and
   hashes, and trace-only passes are gone; image work during the intro fell
   from 3.5 s to 0.6 s per 50 frames.
8. ~~Produce the first complete game-rendered frame.~~ Done.
9. ~~Reach the Sony intro.~~ Done: the SIE screen and the PlayStation Studios
   video, with sound.
10. ~~Move VideoOut ownership and presentation into native C++.~~ Done: a
    native Win32/Vulkan window.
11. ~~Finish the intro.~~ Done: the haze and flicker are fixed, the video
    is decoded on the GPU and stays on its sound, and the Team Asobi splash
    renders.
12. ~~Reach the title screen.~~ Done on 2026-10-01.
13. Make the scene after the intro fast: the worker, frames in flight
    instead of a wait per frame.
14. Add native input, save data and filesystem coverage.
15. Reach the menu and first playable level.
16. Extract reusable runtime/compiler components into a separate recompiler
    project after Astro Bot is stable.

## Licensing

The project is distributed under the GNU General Public License, version 2
only (`GPL-2.0-only`). Parts of it are adapted from KytyPS5, which is
GPL-2.0-only, and from SharpEmu, whose files are GPL-2.0-or-later and are
used here under version 2. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The GPL license applies to this project's source code. It does not grant any
rights to Astro Bot, PlayStation software, firmware, keys, or other
third-party copyrighted material.

## Legal Notice

This project is not affiliated with Sony Interactive Entertainment or Team
Asobi.

The repository does not distribute:

- Astro Bot or other game files;
- game assets, modules, or decompiled game code;
- PS5 firmware dumps;
- encryption keys;
- generated executables containing game-derived code or data;
- instructions for obtaining unauthorized copies.

Users must supply files from their own legally obtained copy. All
game-specific extraction and processing is intended to happen locally.

Astro Bot, PlayStation, and related trademarks and copyrighted materials
belong to their respective owners.
