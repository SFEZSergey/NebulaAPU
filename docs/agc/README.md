# AGC as this runtime understands it

What two months of running Astro Bot through a native runtime taught us about
AGC - the PS5's graphics command layer - and about the Gen5 GPU underneath
it. Written from our own observations: command streams read out of a running
title, traces, dumps of render targets, and the title's own code read with a
disassembler.

This is not Sony's documentation and does not reproduce it. Nothing here is
copied from an SDK; where another project's reading was compared against
ours, that is said, and only the fact is kept. The code these pages describe
is not all ours: the shader translator is a C++ port of SharpEmu's C# shader
compiler, and parts of the runtime are adapted from KytyPS5 - see
[../../THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md). The observations
are our own.

## Pages

| page | what it covers |
| --- | --- |
| [command-stream.md](command-stream.md) | PM4 packets in DCB/ACB buffers: which opcodes appear, their layouts, what each does to state |
| [registers.md](registers.md) | The registers the runtime reads, what they mean, the selector trap |
| [resources.md](resources.md) | V#, T#, S#, formats, tile modes, mip layout, volumes, compression |
| [shaders.md](shaders.md) | Gen5 shaders as the translator sees them: user data, scalar evaluation, EXEC, compute workgroups, fixed-function inputs |
| [runtime.md](runtime.md) | How the native runtime turns all of that into Vulkan, and the traps it fell into |
| [../hle/README.md](../hle/README.md) | The libraries beside AGC that decided whether a frame appeared: VideoOut, AvPlayer, Json, threads, files |

## How to read a claim

Every non-obvious statement carries one of these:

- **measured** - seen on this title, with the commit or the trace that shows
  it. Reproducible with the tools named.
- **code** - read from the title's own code or the command stream, not yet
  confirmed by a run that depends on it.
- **inferred** - the explanation that fits what was measured, not proved.
- **open** - not known. Written down so nobody takes a guess for a fact.

When a page is wrong, fix it and say what changed and why - the dated notes
one level up (`docs/*_2026-*.md`) show why that matters: several of them were
corrected the same day they were written.

## Where things live in the code

| topic | file |
| --- | --- |
| Packet decoding, register state, draws, fast clears | `runtime/native/ps5rt_hle_impl.cpp` (`parse_agc_dcb`, `apply_*`) |
| GPU runtime: resources, uploads, watches, Vulkan | `runtime/native/ps5gpu_native.cpp` |
| Descriptor decoding, resource manifests | `runtime/native/gen5_manifest.h` |
| Shader decoding | `runtime/native/gen5_decoder_*.h`, `gen5_shader_ir.h` |
| Scalar evaluation (descriptors, loads) | `runtime/native/gen5_scalar_eval.h` |
| Translation to SPIR-V | `runtime/native/gen5_translate.h`, `gen5_emit_*.h`, `spirv_builder.h` |
| Offline disassembler | `tools/gen5-disasm.cpp` |

## Diagnostics worth knowing first

Most findings below came from a handful of switches. Their full list is in
`runtime.md`; these answer the common questions:

- `PS5GPU_NATIVE_PROBE_SURFACES=addr,...` - the centre pixel of up to eight
  surfaces at the end of every frame, without the cost of a dump.
- `PS5GPU_NATIVE_FRAME_DUMP_*` - a whole surface, after a chosen draw.
- `PS5GPU_NATIVE_TRACE_TARGET_BUFFERS=addr` - every buffer every draw into a
  target read, as floats.
- `PS5RT_NATIVE_SHADER_DUMP=dir` - the guest code and user data behind each
  translated stage, for `tools/gen5-disasm`.
- `PS5RT_FATAL_PEEK=addr:bytes,...` - guest memory at the moment of a crash.
