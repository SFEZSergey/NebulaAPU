# Roadmap Status

> The live status and the roadmap are in [README.md](README.md). This file keeps
> the implementation record from July-August 2026; its sections are historical
> unless they say otherwise. Later findings are in the dated notes under
> [docs/](docs/).

## Repository focus

- Project: NebulaAPU.
- Immediate product goal: an Astro Bot-specific native Windows compatibility
  runtime and port.
- A reusable general PS5 recompiler repository is deferred until the Astro Bot
  runtime reaches stable game-produced frames.
- Existing `Ps5Recomp` executable, namespace, and file names are transitional
  internal identifiers and do not change the current product focus.

## Document authority

- Planning documents and session logs are kept outside the repository and are
  not part of it.
- This file is an implementation record, not a roadmap; the roadmap is in
  [README.md](README.md).

## Language decision

- The production compatibility runtime, GPU runtime, generated game code,
  executable loader, and game-specific patches are implemented in C or C++.
- The boundary between generated game code and the runtime is a versioned plain
  C ABI. C++ ABI, STL objects, and exceptions do not cross DLL boundaries.
- SharpEmu's C# code is reference material for behavior, formats, algorithms,
  and differential traces. It is not the production implementation.
- `SharpEmu.HleBridge` and direct use of `SharpEmu.Libs` are temporary
  diagnostic scaffolding. Reaching a milestone through that bridge does not
  complete the corresponding native-runtime milestone until the required
  behavior is implemented in C or C++.
- New production fixes must be made in this repository's native C/C++
  components.
  A C# experiment is acceptable only when it produces a trace, test vector, or
  algorithm that is then ported to the native runtime.
- A local SharpEmu checkout is used as a read-only reference. The older
  suggestion to maintain runtime changes in a SharpEmu fork is superseded:
  all product changes live in this repository.
- The detailed component and migration contract is documented in
  `docs/NATIVE_CPP_ARCHITECTURE.md`; the source-to-native responsibility map is
  documented in `docs/SHARPEMU_PORTING_MATRIX.md`.

## Phase 1: Parser and analysis

- `ExecutableImage`: available through the local SharpEmu parser frontend.
- SELF/ELF loading and relocations: available.
- Import, export, TLS and module inventory: available.
- Static CFG and bounded jump-table discovery: available.

## Phase 2: Minimal executable

- Deterministic static package: available.
- Generated native metadata: available.
- Standalone Windows PE loader: implemented in this repository.
- Exact-address segment mapping: implemented.
- Direct entry-point dispatch: implemented for import-free x86-64 fixtures.
- Guest-to-runtime ABI thunk: implemented and covered by the end-to-end fixture.
- Sparse exact-address mapping for distant code/import regions: implemented.
- Generated 16-byte import patching and NID registry: implemented.
- Versioned `Ps5Runtime.dll` C ABI: implemented.
- D3D12 device bootstrap: implemented as the default Windows backend.
- Common PS5 GPU IR and production HLE NID set: next milestones.

## Phase 3: Build automation (Roadmap section 29)

- `ps5recomp build` command: automated pipeline inspect -> HLE generate -> compile -> link -> verify.
- `BuildOrchestrator`: handles caching, resume via `build-manifest.json`, failure bundle generation.
- `HleRegistryGenerator`: reads SharpEmu's `ps5_names.txt` (154k names), computes NID via SHA-1, generates C++ `hle_registry.h/.cpp` with lookup tables. Replaces manual if-chain in `ps5rt_runner.cpp`.
- `HleExternalCatalogLoader`: now scans KytyPS5 `LIB_FUNC` known-NID registrations, SharpProspero `StubCatalog`, PS5 payload SDK generated stubs, and the IDA PS5 symbol catalog in addition to the existing PS5SDK/shadPS4/OpenOrbis/ps4libdoc sources.
- External HLE evidence is aggregated across all matching repositories while the highest-confidence candidate still supplies the preferred name/library/signature.
- Astro Bot expanded-source audit: 2062/2071 package NIDs have external evidence; KytyPS5 overlaps 773 NIDs (256 still missing implementations), SharpProspero 512, payload SDK 612, and IDA symbols 2049.
- `self-test` includes synthetic fixtures for the KytyPS5, SharpProspero, payload SDK, and IDA catalog parsers.
- `HleKnownImplementations`: maps the native runtime handlers and aliases; the current Astro Bot audit reports 95 implemented NIDs.
- `TraceSchema`: unified JSONL trace events compatible with SharpEmu reference traces.
- `FailureBundle`: auto-generates `failure-bundle/summary.txt` + `manifest.json` on build failure.
- `astrobot.toml` game profile: title metadata, analysis limits, build config, module list, patches.
- Self-test: PASS, including the external HLE parser fixtures. Astro Bot static build remains available; the expanded HLE audit reports 2071 package NIDs, 95 implemented, 499 SharpEmu bridge, 2 stubbed, and 1475 missing.

Usage:

    ps5recomp build "<path to your eboot.bin>" `
      -o artifacts\astrobot-build `
      --ps5-names "<SharpEmu checkout>\scripts\ps5_names.txt"

## GPU policy

- D3D12 is the default Windows backend.
- Vulkan is the secondary portability backend.
- Both consume one backend-neutral PS5 GPU command and shader IR.
- FSR and DLSS are post-processing plugins above the backend boundary; they
  stay disabled until motion vectors, depth, exposure, and frame timing are
  available from the game pipeline.

## Active implementation slice (historical snapshot, 2026-08-03)

Superseded: the native replay, descriptor-backed draws and native VideoOut
ownership described here have since been completed; see
[README.md](README.md) for the current state.

- Current roadmap position: Phase 8, native GPU migration.
- Status snapshot: 2026-08-03. The detailed replay evidence is in
  `docs/GPU_REPLAY_STATUS_2026-08-03.md`.
- The managed SharpEmu GPU path remains reference-authoritative while the
  native runtime observes the same VideoOut and DCB inputs.
- Gate A is complete for the observed path
  `VideoOut registration -> DCB -> per-draw state -> flip`.
- The native C++ parser now handles direct registers, both legacy and native
  indirect-register packets, SDK sentinel color-target blocks, viewport and
  scissor recovery, draw counts/indexing, primitive type, ES/PS addresses,
  render-target metadata, and the final display-buffer flip.
- Verified probe:
  `artifacts\probes\astro-cycle-20260730-202647`.
  It reached `first_frame` in 77.8 seconds with `fatal=0`, 71 native draw
  milestones, and valid RT/viewport/scissor state for all 71 draws.
- The final scanout state matches the SharpEmu reference trace:
  RT `0x0000000507410000`, `3840x2160`,
  viewport `0,2160,3840x-2160`, full `3840x2160` scissor,
  ES `0x000000050070BD00`, PS `0x000000050070CA00`.
- The native replay now reaches Vulkan submission with
  `submit_failures=0`. The early scene states 11, 13, 14, 15 and 16 are
  still skipped because their SPIR-V uses storage-buffer and/or sampled-image
  descriptors. State 12 is the only resource-free early guest pipeline that
  currently executes.
- The immediate blocker is generic descriptor-backed guest draw support, not
  Vulkan initialization or queue submission. The target
  `0x0000000514080000` receives the clear pass but does not yet receive all
  early scene draws.
- Next gate: create per-state descriptor layouts and pipelines, resolve
  `guestBuffers` and sampled-image resources from captured shader user data,
  bind decoded vertex buffers, and replay the same capture. The expected
  milestone is a `guest_descriptor_draw` for states 11, 13, 14, 15 and 16.
- Native VideoOut state ownership remains the next architectural ownership
  gate after the current descriptor-backed draw blocker is cleared.
- A native first-frame milestone requires the same path with the CLR and both
  managed bridge DLLs absent.

## Separation rule

SharpEmu remains a parser/runtime reference while code is extracted behind
stable interfaces. The generated PE is not an emulator frontend and does not
start SharpEmu.
