# Third-Party Notices

## Project license

NebulaAPU is distributed under the **GNU General Public License, version 2
only** (SPDX: `GPL-2.0-only`). See [LICENSE](LICENSE).

Why "only": the repository contains code adapted from KytyPS5, which is
GPL-2.0-only and cannot be offered under later versions. Code received under
GPL-2.0-or-later (SharpEmu) may be used under version 2, so the combination is
distributed under version 2 only. Files that carry an upstream header keep it
unchanged. A file with no header of its own is covered by [LICENSE](LICENSE)
unless it says otherwise.

## SharpEmu

- Source: https://github.com/sharpemu/sharpemu
- License: GNU General Public License v2.0 or later (the headers of the
  files below say `GPL-2.0-or-later`; used here under version 2).

The Gen5 shader decoder, IR, translator and SPIR-V writer under
`runtime/native/` are C++ ports of SharpEmu's C# shader compiler. The headers
of the ported files record the origin:

| C++ file | ported from |
| --- | --- |
| `gen5_shader_ir.h` | `Gen5ShaderIr.cs` |
| `gen5_shader_metadata.h` | `Gen5ShaderMetadataReader.cs` |
| `gen5_decoder_sop.h`, `gen5_decoder_vop.h`, `gen5_decoder_memory.h` | `Gen5ShaderTranslator.cs` |
| `spirv_builder.h` | `SpirvModuleBuilder.cs` |

The C# shader compiler projects `src/Ps5Recomp.ShaderCompiler` and
`src/Ps5Recomp.ShaderCompiler.Vulkan` carry SharpEmu's header too (16 of
their 18 files). The C# bridges under `bridge/` carry this project's own
copyright; they are temporary scaffolding used to compare against SharpEmu.
SharpEmu's `ps5_names.txt` is read by the HLE registry generator as a
catalogue of function names.

Every file whose header names the SharpEmu copyright (they keep it):

```text
runtime/native/
    gen5_buffer_format.h, gen5_cfg.h, gen5_decoder_memory.h, gen5_decoder_sop.h, gen5_decoder_vop.h, gen5_emit_alu.h, gen5_emit_control.h, gen5_emit_image.h, gen5_emit_memory.h, gen5_emit_plan.h, gen5_emit_stage.h, gen5_emit_typed.h, gen5_emit_valu.h, gen5_manifest.h, gen5_registers.h, gen5_scalar_eval.h, gen5_shader_ir.h, gen5_shader_metadata.h, gen5_structure.h, gen5_translate.h, spirv_builder.h
src/Ps5Recomp.ShaderCompiler/
    Gen5InlineConstants.cs, Gen5ShaderIr.cs, Gen5ShaderMetadataReader.cs, Gen5ShaderScalarEvaluator.cs, Gen5ShaderTranslator.cs, Gfx10UnifiedFormat.cs, GuestDrawKind.cs, GuestShaderContext.cs
src/Ps5Recomp.ShaderCompiler.Vulkan/
    Gen5ControlFlowAnalysis.cs, Gen5SpirvShader.cs, Gen5SpirvTranslator.Alu.cs, Gen5SpirvTranslator.Structured.cs, Gen5SpirvTranslator.cs, SpirvFixedShaders.cs, SpirvModuleBuilder.cs, SpirvStructuredValidation.cs
```

## KytyPS5

- Source: https://github.com/KytyPS5/KytyPS5
- License: GNU General Public License v2.0 only.

Adapted from KytyPS5:

| here | upstream | what |
| --- | --- | --- |
| `runtime/native/ps5gpu_native.cpp`, the `ExactDetilePattern` tables `depth_x_1`, `depth_x_2`, `render_x_4`, `render_x_2`, `render_x_16` and `standard_4k_16` (search for "converted from KytyPS5" / "converted from Kyty") | `src/graphics/guest_gpu/tile.cpp`: `Depth64KB8/16XOffsetBytes`, `Depth64KB8/16YOffsetBytes`, `Gen5RenderTargetOffsetInBlock`, `Gen5Standard4KBOffsetInBlock` | tile-mode 24, 27 and 5 address swizzles, converted from per-bit offset functions to XOR mask tables |

The upstream commit these were taken from was not recorded when they were
ported (they arrived with the first commit of this repository, 2026-09-27);
the functions are present in KytyPS5 `719e025` (2026-10-04). That file carries
no header of its own and is covered by KytyPS5's `LICENSE` (GPL-2.0).
KytyPS5 also ships `LICENSES/Kyty-MIT.txt` for code inherited from the
original Kyty; nothing here is known to come from that part.

Elsewhere KytyPS5 is a reference only: comments in
`Gen5ControlFlowAnalysis.cs`, `Gen5SpirvTranslator.cs`,
`GpuBridgeExports.cs` and `docs/NATIVE_CPP_ARCHITECTURE.md` describe how it
solves the same problem, without its code. The HLE registry generator scans
its `LIB_FUNC` registrations as one source of known function names.

## shadPS4

- Source: https://github.com/shadps4-emu/shadPS4
- License: GNU General Public License v2.0.

Used as a technical reference - for example the NV12 frame layout reproduced
in `runtime/native/ps5rt_avplayer.h` - and scanned by the HLE registry
generator for function names and signatures. Source adapted from it stays
under its original license.

## Other sources scanned for function names and signatures

`HleExternalCatalogLoader` can read local checkouts of the projects below
(by default under `NEBULA_DEV_ROOT\reference`). Only function names, NIDs
and library names are taken from them - facts that the generator re-hashes
and checks locally. None of their source is distributed in this repository,
and no code from them is compiled into it.

| project | license (as found) |
| --- | --- |
| [PS5Dev/PS5SDK](https://github.com/PS5Dev/PS5SDK) - a community homebrew SDK, not Sony's SDK | not checked |
| [SvenGDK/SharpProspero](https://github.com/SvenGDK/SharpProspero) | GPL-3.0 |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | GPL-3.0 |
| [flatz/ida_ps5_elf_plugin](https://github.com/flatz/ida_ps5_elf_plugin) | no license file |
| [OpenOrbis-PS4-Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) | not checked |
| [ps4libdoc](https://github.com/idc/ps4libdoc) | not checked |

Before code from any of them is used, its license has to be checked: GPL-3.0
code cannot be combined with `GPL-2.0-only`, and code without a
license cannot be copied at all, so these stay name sources only. RPCS3 is
used only as a technical reference and for differential traces.

## Contribution requirements

When third-party source code or a substantial algorithm is committed:

1. Record the upstream repository, commit, and original file path.
2. Preserve copyright and SPDX/license headers.
3. Clearly mark substantial local modifications.
4. Keep applicable upstream license and notice files.
5. Check that the upstream license can be combined with `GPL-2.0-only`: code
   under `GPL-2.0-or-later` and `GPL-2.0-only` can; code under a license
   without a version-2 option cannot.
6. Provide corresponding source code for distributed GPL-covered binaries.

Each third-party component retains the copyright of its original authors and
contributors.
