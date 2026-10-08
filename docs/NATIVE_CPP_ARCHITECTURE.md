# Native C++ Architecture

## Authority

The project follows these documents in this order:

1. `C:\dev\sharpemu_ps5_recomp_dialog.txt` defines the product and native-port
   goals.
2. `C:\dev\sharpemu_ps5_recomp_roadmap_2026-07-22.txt` supplements the primary
   roadmap with build automation, diagnostics, and reference repositories.
3. `ROADMAP_STATUS.md` records implementation progress only.

The current project decision overrides the old suggestion to maintain a
modified SharpEmu fork. A local SharpEmu checkout is read-only reference input.
This repository is the canonical source. The older local workspace remains a
temporary experiment/build workspace during the repository migration.

## Production Invariant

The shipped game process is native C or C++ code:

```text
AstroBot.exe / AstroBotHost.exe
              |
              v
         gamecode.dll
              |
       versioned plain C ABI
              |
              v
         ps5runtime.dll
```

The final process must not:

- load the CLR;
- load `SharpEmu.Libs.dll`;
- load `Ps5HleBridge.dll`;
- load `Ps5GpuBridge.dll`;
- start SharpEmu or its GUI;
- execute an emulator CPU loop for normal x86-64 game code.

C# bridges may only be used as temporary reference runners. A result obtained
through a managed bridge is a reference milestone, not a native milestone.
No new production behavior is implemented in those bridges.

## Component Layout

```text
runtime/native/
  core/                  memory, TLS, ABI, threads, callbacks
  hle/                   kernel, filesystem, saves, input, audio
  gpu/
    agc/                 DCB/ACB and Gen5 packet decoding
    ir/                  backend-neutral command/resource IR
    shader/              Gen5 decode, shader IR, reflection
    vulkan/              first correctness backend
    d3d12/               later default Windows backend
  videoout/              display buffers, vblank, flip and present
  platform/windows/      window, timing and platform services
  profiles/astrobot/     versioned game-specific patches
```

Existing large native source files are migrated into these ownership
boundaries incrementally. File movement is not allowed to delay functional
parity.

## ABI Rules

- DLL boundaries use a versioned plain C ABI.
- Every public structure starts with `struct_size` and `abi_version`.
- ABI structures use fixed-width integer types.
- Ownership of every pointer and allocation is explicit.
- STL containers, C++ exceptions, RTTI-dependent objects, and CRT-owned
  allocations do not cross DLL boundaries.
- Guest entry points and HLE functions use the PS5 SysV x86-64 ABI.
- Host-only implementation code may use C++ internally.

## SharpEmu Use

SharpEmu supplies:

- parser and loader behavior;
- HLE semantics and constants;
- AGC/PM4 packet behavior;
- Gen5 shader decoding behavior;
- VideoOut formats and state transitions;
- reference traces and golden test vectors.

Transfer is incremental:

1. capture a deterministic SharpEmu trace or fixture;
2. port the required algorithm into native C++;
3. run both implementations on the same input;
4. compare state, output and failure classification;
5. switch one capability to native;
6. remove the bridge path only after parity.

SharpEmu-specific GUI, session control, managed memory abstractions and
emulator loop are not transferred.

## GPU Boundary

The command path is:

```text
sceAgc / sceAgcDriver
  -> DCB/ACB parser
  -> register and resource state
  -> backend-neutral GPU IR
  -> Vulkan
  -> D3D12 after Vulkan correctness
  -> VideoOut flip/present
```

AGC packet decoding must not call Vulkan or D3D12 directly. Shader decoding
must not depend on either graphics API. Game-specific fixes are selected by
module hash and validated preimage, never by unversioned absolute-address
hacks in common runtime code.

## Migration Gates

### Gate A: Native observation

The C++ runtime parses the same command or HLE call as the reference bridge
without changing the reference result. It emits deterministic native
milestones.

### Gate B: Native authority

The C++ implementation owns the result while the reference path is optional
comparison-only code.

### Gate C: Managed removal

The process reaches the milestone with no CLR or managed bridge loaded.

For the GPU path, managed removal requires:

- native VideoOut buffer registration;
- native DCB/ACB submission;
- native register/resource tracking;
- native shader compilation;
- native pipeline and draw execution;
- native flip/present;
- a confirmed guest-rendered frame.

## Current Critical Path

The immediate sequence is:

1. native shadow observation of VideoOut registration and DCB submissions
   [complete];
2. deterministic per-draw register milestones and comparison with SharpEmu
   [complete];
3. native VideoOut state ownership [active];
4. backend-neutral draw IR;
5. native Gen5 shader compiler parity;
6. Vulkan draw and present;
7. disable managed GPU routing;
8. repeat the Astro Bot first-frame probe with no CLR loaded.

## Knowing When A Guest Texture Changed

Measured on a 120 second run, the worker prepares 6308 guest images and
reads 57GB doing it, because every dispatch re-reads every texture it
binds. Preparing one costs 12.6ms; uploading them costs 28s of the 104s
the worker spends in compute.

The reason it re-reads is that `image.upload_hash`, which decides whether
anything needs uploading, lives on the per-state image record, and the
title registers a compute state per dispatch. The record is therefore
always empty and the answer is always "upload it".

Moving that hash onto the `RenderSurface`, which is per guest address and
outlives the states that bind it, was tried three times and measured worse
every time - 25, 30 and 30 flips presented against 48 and 68 without it.
Ruling out the obvious explanation (draws rejected for image layout)
showed zero such rejections, so the mechanism is still unknown. Re-reading
the guest to compute the hash also leaves the 57GB in place, which is most
of what wants removing.

shadPS4 does not ask this question by reading. `VideoCore::PageManager`
keeps a per-page count of write watchers, lowers the page permissions
accordingly, and learns that the guest wrote a texture from the access
violation that follows. `UpdatePageWatchers` arms and disarms the range;
`src/video_core/page_manager.cpp` holds the Windows path. shadPS4 is
GPL-2.0 and portable under the licensing rules in CLAUDE.md.

This runtime already has the half that is hard to add: a vectored
exception handler is installed in `ps5rt_runner.cpp` and already
dispatches guest access violations through
`try_recover_guest_aperture_access`. What is missing is the page state and
the arming.

Until that exists, the texture path cannot be made cheap by asking whether
the bytes changed, because asking costs a read of every byte.

## What A Compute Program Actually Depends On

This title registers a compute state per dispatch - 1778 of them across
twenty-one distinct shaders in one run - and each one is translated afresh
and given its own Vulkan pipeline. The disk cache for translations stores
779 entries and hits none of them.

The cause is the cache key. `ComputeRequestKey` in the GPU bridge hashes
every shader register and its value, and the user-data registers among
them hold the descriptor pointers the guest rebinds on every dispatch, so
the key is unique by construction.

KytyPS5 keys the same thing differently (`c167375`, "shader: centralize
program compilation and permutation caching"). Programs are held as
`unordered_map<{stage, shader_hash}, vector<Permutation>>`, and a
permutation is matched on a static key built from the input info alone.
For compute that key is:

    workgroup_register, wave_size, thread_ids_num, lds_size_dwords,
    scratch_size_dwords, needs_lds_barriers, dispatch_thread_dimensions,
    threads_num[3], group_id[3], tg_size_en

Thirteen small integers and no addresses. For the vertex stage it is the
register layout, stride, swizzle and format of each resource - the shape
of the bindings, never where they point.

So the program is a function of the shader and of the descriptor shape.
The addresses and the buffer contents belong to the manifest, which is
built per dispatch and should stay that way. Narrowing our key to the same
set is what turns 1778 translations and 1778 pipelines back into
twenty-one of each.

KytyPS5 is GPL-2.0 and portable under the licensing rules in CLAUDE.md.

## The Windowed Probe Cannot Currently Run

Every windowed probe since 22:09 on 2026-08-27 exits after two to three
seconds with `0xC000013A`, STATUS_CONTROL_C_EXIT, immediately after
`gpu_bridge=ready`. Windows logs no application error, which fits: the
process is being terminated, not crashing.

It is not ours. The committed build that had just run for 233 seconds
fails identically, and so does every earlier good configuration. Ruled out
by testing: leftover processes, stray probe scripts, both Vulkan pipeline
caches, the game data on H:, the save directory (untouched since July),
anything written under app0 today, and the SharpEmu update (rolled back to
f4f36b5 and it still failed).

`astro-cycle.ps1 -Headless` runs to completion in the same conditions.
`-Headless` changes only `-WindowStyle`, so what fails is the window, and
something on the desktop is closing it. RTSS and RTSSHooksLoader64 have
been resident since 17:05 and hook graphics; they are the first thing to
try stopping. A reboot is the other.

Until then, measure with `-Headless`.

## The HLE Bridge Is Built From The Reference Clone

`astro-cycle.ps1` picks its SharpEmu source tree from a list whose first
entry is `C:\dev\SharpEmu-upstream-latest`, so updating that clone rebuilds
`Ps5HleBridge.dll` and changes what the runtime is. Pulling upstream is not
a read-only act. Checked at b226dca against f4f36b5 over forty seconds
headless - 40 flips presented against 46, 225 submitted against 207, 15617
draws against 14614, fatal=0 either way - so the update is clear, but the
check is the point.

## The Image Loop, And Where It Is Not

After the page watches went in, the worker's compute time is 21.7s of a
forty second run, of which updating a dispatch's resources is 14.4s and
images are 11.4s. Two attempts at that, both reverted, and what they
established:

**Arming a watch on the all-zero path is where the remaining reads are.**
Most preparations end by finding the source entirely zero and returning,
and without a watch each one re-reads nine megabytes to rediscover the
same nothing. Arming there as well:

    img_n            1936  ->   527        img_MB  17612 -> 7145
    img_read       4637ms  ->  1968ms      skips     207 ->  793

**A refused arm has to be remembered.** A range that is not one plain
committed read-write region cannot be protected, and retrying costs a
VirtualQuery and a failed VirtualProtect on every preparation - 464
refusals in one run, and 16 flips presented against 28 to 35 without them.
Remembering the refusal brings it to 14 refusals and 29 flips.

**Neither showed an end-to-end gain**, because the cost is somewhere else
in that loop and it is still unmeasured. An attempt to time the two other
things the loop does per image - finding the surface and probing whether
the address reads - produced counters that contradict each other: 36 loop
iterations against 355 preparations made from inside that same loop on the
same thread. `prepare_guest_image_upload` has exactly one caller, so the
counter did not land where it was meant to. The eleven seconds are not
small; they are unlocated.

The next attempt here starts by placing that counter correctly and
proving it against `img_n` before believing anything else it says.
