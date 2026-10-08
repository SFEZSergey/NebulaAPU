# SharpEmu Porting Matrix

> Status note: this matrix records the plan as of 2026-08-03. Since then the
> Gen5 shader decode/translate and SPIR-V writer rows have been ported to C++
> (`runtime/native/gen5_*.h`, `spirv_builder.h`), and VideoOut and the Vulkan
> runtime run natively - see [README.md](../README.md). Ported files keep
> SharpEmu's copyright header; see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

A local SharpEmu checkout is used read-only. The source paths below identify
reference implementations; all destination product code belongs to this
repository.

| Capability | SharpEmu reference | Native destination | Current gate |
| --- | --- | --- | --- |
| SELF/ELF metadata | `SharpEmu.Core/Loader` | compiler frontend / `analysis.db` | available through frontend |
| NID catalog | source generator and `ps5_names.txt` | generated C++ HLE registry | native authority |
| Guest memory/TLS | native execution backend | `runtime/native/core` | partial native authority |
| Threads/synchronization | `SharpEmu.Libs/Kernel` | `runtime/native/hle` | partial native authority |
| Filesystem/saves | kernel and SaveData libraries | `runtime/native/hle` | partial native authority |
| VideoOut state | `SharpEmu.Libs/VideoOut` | `runtime/native/videoout` | native observation; reference authority |
| DCB/ACB parsing | `SharpEmu.Libs/Agc/AgcExports.cs` | `runtime/native/gpu/agc` | per-draw native observation |
| GPU command model | `SharpEmu.Libs/Gpu/IGuestGpuBackend.cs` | `runtime/native/gpu/ir` | next active gate |
| Gen5 shader decode | `SharpEmu.ShaderCompiler` | `runtime/native/gpu/shader` | managed reference |
| Vulkan renderer | `SharpEmu.Libs/Gpu/Vulkan` | `runtime/native/gpu/vulkan` | managed reference |
| D3D12 renderer | no complete reference backend | `runtime/native/gpu/d3d12` | bootstrap only |

## Active Slice

The active Phase 8 slice is native GPU replay and descriptor-backed draw
migration:

- observe successful managed `sceVideoOutOpen`;
- decode the registered PS5 display-buffer attributes in C++;
- record display addresses, format, dimensions and pitch;
- parse the same successful `sceAgcDriverSubmitDcb` command in C++;
- decode direct and indirect register packets, including SDK sentinel state;
- report per-draw count/indexing, primitive, ES/PS, RT, viewport and scissor;
- correlate `RFlip` with a registered display buffer;
- leave the managed result authoritative until comparison is stable.

The verified `astro-cycle-20260730-202647` probe produced 71 complete native
draw milestones and matched the saved SharpEmu trace for the final 4K scanout.
The newer capture
`artifacts/probes/astro-cycle-20260802-235047/native-gpu-frame.bin` reaches the
native Vulkan submit path with `submit_failures=0`.

The current renderer executes the resource-free guest state 12 and the
specialized composition/post-processing states. Early scene states 11, 13, 14,
15 and 16 are now known to require:

- `guestBuffers` storage-buffer descriptors at binding 0;
- sampled images at bindings 1 and 2 for states 11, 13 and 14;
- decoded ES vertex resources for states 11, 14 and 15.

The immediate native destination is a generic descriptor-backed pipeline path.
It must resolve shader user-data resources, create per-state descriptor
layouts/pipelines, bind decoded vertex buffers, and preserve the existing
specialized paths. The detailed evidence and replay commands are recorded in
`docs/GPU_REPLAY_STATUS_2026-08-03.md`.

This slice does not yet claim a native guest-rendered frame. It does establish
the golden capture, native submit path, and exact next blocker needed for
native VideoOut ownership and final display rendering.
