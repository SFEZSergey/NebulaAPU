# Astro Bot Native GPU Replay Status

Date: 2026-08-04

## Scope

This document records the current native C++ Vulkan replay milestone for the
Astro Bot package. It supplements `ROADMAP_STATUS.md`; it does not replace
`C:\dev\sharpemu_ps5_recomp_dialog.txt` or
`C:\dev\sharpemu_ps5_recomp_roadmap_2026-07-22.txt`.

The production direction remains:

- native C/C++ runtime;
- plain C ABI at DLL boundaries;
- SharpEmu used as a read-only behavioral reference;
- no SharpEmu GUI, CLR, or emulator loop in the final native process.
- Astro Bot is the current product target; extraction of a reusable general
  recompiler is deferred until the game runtime is stable.

## Verified Artifacts

Capture:

```text
C:\dev\ps5recomp\artifacts\probes\astro-cycle-20260804-185724\native-gpu-frame.bin
```

Replay tool:

```text
C:\dev\ps5recomp\artifacts\tools\Ps5GpuReplay.exe
```

Native runtime:

```text
C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\Ps5GpuRuntime.dll
```

GPU bridge and shader package:

```text
C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\Ps5GpuBridge.dll
C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\shaders
```

The native runtime was rebuilt with MinGW C++20 in approximately 13 seconds.
The build completed without warnings:

```powershell
& 'C:\Users\<user>\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe' `
  -std=c++20 -O2 -Wall -Wextra `
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DPS5GPU_NATIVE_BUILD=1 `
  -I 'C:\dev\Kyty-source\source\3rdparty\vulkan\include' `
  -shared -pthread -static-libgcc -static-libstdc++ `
  runtime\native\ps5gpu_native.cpp `
  -o artifacts\astrobot-r26-build\package\Ps5GpuRuntime.dll
```

## Current Draw Chain

The captured early scene contains these shader states:

```text
draw 3: state 11 -> target 0x514080000
draw 4: state 12 -> target 0x514080000
draw 5: state 13 -> target 0x514080000
draw 6: state 14 -> target 0x53AD00000
draw 7: state 15 -> target 0x514080000
draw 8: state 16 -> target 0x514080000
draw 9: state 17 -> target 0x53B9F0000
```

The following transitions then occur:

```text
state 17 -> 18 -> 19
state 20 -> 27
state 28
state 29
state 30 -> final display 0x507410000
```

The current native renderer now reaches the Vulkan submission path with:

```text
guest_pipeline=10
guest_descriptor=15
state28=1
state29=1
post=8
dropped_flips=0
```

Binary resource manifests are generated for both shader stages. The native
runtime loads 26 descriptor-backed states with:

```text
loaded=26
missing_manifest=0
invalid_manifest=0
failed=0
```

States 11 and 13-16 now execute with their translated SPIR-V, storage buffers,
sampled-image bindings, and per-state descriptor sets. State 12 continues
through the resource-free guest-pipeline path.

The runtime also handles PS5 primitive types 6 (`TriStrip`), 7 (`RectList`) and
17 (`RectListLegacy`). Non-indexed RectList draws use a triangle-strip pipeline
and expand one-, three-, or four-vertex submissions to four host vertices.

## SPIR-V Layout Evidence

The state-specific compile and layout traces are stored in:

```text
artifacts\reference\spirv-layout-state11.log
artifacts\reference\spirv-layout-state13.log
artifacts\reference\spirv-layout-state14.log
artifacts\reference\spirv-layout-state15.log
artifacts\reference\spirv-layout-state16.log
```

The bindings reported by the current bridge are:

| State | Stage | Set | Binding | Kind |
| --- | --- | ---: | ---: | --- |
| 11 | ES | 0 | 0 | storage buffer `guestBuffers` |
| 11 | PS | 0 | 0 | storage buffer `guestBuffers` |
| 11 | PS | 0 | 1 | sampled image `tex0` |
| 11 | PS | 0 | 2 | sampled image `tex1` |
| 13 | ES | - | - | no descriptor |
| 13 | PS | 0 | 0 | storage buffer `guestBuffers` |
| 13 | PS | 0 | 1 | sampled image `tex0` |
| 13 | PS | 0 | 2 | sampled image `tex1` |
| 14 | ES | 0 | 0 | storage buffer `guestBuffers` |
| 14 | PS | 0 | 0 | storage buffer `guestBuffers` |
| 14 | PS | 0 | 1 | sampled image `tex0` |
| 14 | PS | 0 | 2 | sampled image `tex1` |
| 15 | ES | 0 | 0 | storage buffer `guestBuffers` |
| 15 | PS | 0 | 0 | storage buffer `guestBuffers` |
| 16 | ES | - | - | no descriptor |
| 16 | PS | 0 | 0 | storage buffer `guestBuffers` |

The current descriptor convention is compatible with the existing Kyty and
SharpEmu shader convention: storage buffers use binding 0 and sampled images
use bindings 1 and 2 for these states.

## Verified State 11 Execution

The current capture contains 2,246 memory pages and includes state 11 buffer
and image resources before the large image allocations consume the capture
budget. The important storage-buffer observations are:

```text
descriptor 0: 72 bytes,   25 nonzero
descriptor 1: 32 bytes,   18 nonzero
descriptor 2: 4096 bytes, 542 nonzero
descriptor 3: 128 bytes,  40 nonzero
```

Descriptor 2 previously had a zero-sized manifest snapshot. The native runtime
now allocates a 4-KB fallback range, reads live guest memory even when the
snapshot is empty, and includes that memory in the capture.

Targeted readback immediately after state 11 established:

```text
original ES + solid green PS:
hash=0x0BD6CFB364C32383 nonblack=2073600/2073600

original ES + original PS + diagnostic depth/color inputs:
hash=0x2DFA252A4460A383 nonblack=2073600/2073600

dropped_flips=0
```

The second test uses depth `0.5` and a solid magenta color input. It proves that
the translated original ES and PS, storage-buffer ABI, sampled descriptors,
viewport, topology, rasterization, and color output all execute correctly.
The remaining black output with real inputs is not the first graphics-pipeline
or shader-translation failure.

State 11 currently references, among other resources:

```text
0x513560000 1920x1080 unified-format=22 tile=24
0x525B10000  240x135  unified-format=71 tile=0
```

Both captured CPU-memory ranges are entirely zero. The second image is produced
by a guest compute dispatch in the reference trace, and the first is also
GPU-owned at the point of use. Static guest-memory capture therefore cannot
reconstruct their live Vulkan contents.

## Next Native Change

Implement GPU-produced image replay:

1. add compute dispatch, clear, copy and barrier records to the native capture
   ABI;
2. record the operations that write `0x513560000` and `0x525B10000`;
3. translate or reuse the required compute SPIR-V and bind its storage images
   and buffers;
4. execute those records in submission order before state 11;
5. alias later sampled descriptors to the resulting native Vulkan images;
6. retain the white/pattern overrides only as explicit diagnostic flags.

The next success criterion is a nonblack replay with
`PS5GPU_NATIVE_GUEST_TEXTURE_WHITE` disabled and identifiable game pixels in
the final display buffer.

## Replay Commands

Compile and inspect one captured state without running the game:

```powershell
$env:PS5GPU_REPLAY_COMPILE_STATE='11'
$env:PS5GPU_REPLAY_COMPILE_ONLY='1'
$env:PS5GPU_REPLAY_BRIDGE='C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\Ps5GpuBridge.dll'
$env:PS5GPU_REPLAY_SHADER_OUTPUT_DIR='C:\dev\ps5recomp\artifacts\reference\spirv-layout'
& 'C:\dev\ps5recomp\artifacts\tools\Ps5GpuReplay.exe' `
  'C:\dev\ps5recomp\artifacts\probes\astro-cycle-20260804-185724\native-gpu-frame.bin' `
  'C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\Ps5GpuRuntime.dll'
```

Change `PS5GPU_REPLAY_COMPILE_STATE` to `13`, `14`, `15` or `16` for the other
states.

Replay the full capture after rebuilding the runtime:

```powershell
Remove-Item Env:PS5GPU_REPLAY_COMPILE_STATE -ErrorAction SilentlyContinue
Remove-Item Env:PS5GPU_REPLAY_COMPILE_ONLY -ErrorAction SilentlyContinue
$env:PS5GPU_NATIVE_SHADER_DIR='C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\shaders'
& 'C:\dev\ps5recomp\artifacts\tools\Ps5GpuReplay.exe' `
  'C:\dev\ps5recomp\artifacts\probes\astro-cycle-20260804-185724\native-gpu-frame.bin' `
  'C:\dev\ps5recomp\artifacts\astrobot-r26-build\package\Ps5GpuRuntime.dll'
```

`PS5GPU_NATIVE_FORCE_EARLY_FRAGMENT=1`,
`PS5GPU_NATIVE_FORCE_EARLY_VERTEX=1`,
`PS5GPU_NATIVE_FORCE_EARLY_TEXTURE_WHITE=1`, and
`PS5GPU_NATIVE_FORCE_EARLY_TEXTURE_PATTERN=1` are diagnostic-only switches.
`PS5GPU_NATIVE_FRAME_DUMP_STATE=11` captures the target immediately after the
selected draw instead of after later states overwrite it.

## Repository State

On 2026-08-03, the implementation and current status were migrated into this
repository. The paths under
`C:\dev\ps5recomp\artifacts` describe local verification inputs only; captures,
extracted shaders, binaries, and game files are not part of the repository.
