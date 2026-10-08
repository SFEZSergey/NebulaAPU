# Shaders

Gen5 shaders are RDNA-family machine code. The runtime decodes them
(`gen5_decoder_*.h`) and translates them to SPIR-V for Vulkan
(`gen5_translate.h`). What follows is what the title's shaders need from a
translation - most of it found by a frame that came out wrong.

## Finding the code

A stage's address is `PGM_HI:PGM_LO << 8` from its SH registers (see
[registers.md](registers.md)). Code ends at `s_endpgm` (`0xBF810000`).
Waves are 32 lanes wide. **measured**

## User data

User data is the stage's argument block: descriptors, pointers to descriptor
tables and constants, in SGPRs from the moment the shader starts. For a
vertex stage it begins at s8; for pixel and compute stages at s0. Take the
registers the draw set, up to 32, and stop at the first one it did not.
**measured**

## Scalar evaluation

Before emitting anything the translator runs the scalar half of the shader
with the user data it was given (`gen5_scalar_eval.h`). Scalar loads read
guest memory at translation time; what they load is a descriptor, a pointer,
or data.

- A load that fills a descriptor - the registers a buffer or image
  instruction names - is resolved there and then, and the descriptor is
  written into the module and its manifest.
- A load of plain data becomes a real load from a bound buffer, not a
  constant.
- `s_getpc_b64` needs the shader's own guest address; without it, 53
  registers a run went unknown.
- `s_buffer_load` names a V# rather than an address.
- The walk is bounded. An unbounded walk over a real shader - 347 blocks,
  branches everywhere - does not finish, and the title stalled in a
  semaphore loop with no frame at all. A bound costs an unresolved
  descriptor, which is a failure that declines to bind, not one that hangs.

**measured**

## Addresses baked into a translation

A translation carries every address it resolved by reading memory. The state
it belongs to was cached by its registers alone, so a frame that laid a
per-frame ring out differently - same registers, a different address behind
the same pointer - reused a translation that read another draw's data. The
intro's video plane read a matrix instead of its constants: up to a third of
frames black. **measured** (`ae4ca39`)

The translator now reports which guest words it baked in (loads that filled
a descriptor, and loads whose result another load used as an address). At
each draw the HLE re-reads them, hashes them, and adds the hash to the
state's registers, so a new layout is a new state with its own translation.
At most 16 layouts per register set; past that they share the first.
`PS5RT_NO_GRAPHICS_DEPENDENCIES=1` turns it off.

**open**, and the largest cost left: in the intro about nine stage
translations and five new compute states happen every flip, because each
frame's rings are a new layout. The fix is to resolve buffer addresses when
binding rather than when translating, so one translation serves every
layout.

## EXEC and divergence

EXEC is per lane. The translation holds every lane mask - EXEC, VCC, the SGPR
pair a compare writes, EXEC for `v_cmpx` - as this lane's bit spread across
the word (0 or all ones), starts with every lane in, and makes the exec
branches read it: `s_cbranch_execz` skips for a lane that is out,
`s_cbranch_execnz` loops for one still in.

Taking every `s_cbranch_execz` as taken skipped the body of every divergent
`if` in every shader. The intro's fog pass lost all its arithmetic and wrote
the ray it started from: the magenta wedge behind "Sony Interactive
Entertainment". **measured** (`2566676`)

## Values the hardware provides

Before a vertex shader runs the hardware puts the vertex index in v5, the
instance index in v8 and the wave's shape in s2 and s3. Without them the
vertex shaders produced no geometry at all - every draw of the loading
screen, degenerate. **measured** (`THE_FIRST_GEOMETRY_2026-09-25.md`)

Compute gets its local invocation id in v0-v2 and its workgroup id in the
SGPRs after the user data, as `COMPUTE_PGM_RSRC2` bits 7-9 enable them. The
module's `LocalSize` is the dispatch's `COMPUTE_NUM_THREAD_*`, not a fixed
64x1x1. **measured** (`8cc6b71`)

Pixel shaders get their system values per `SPI_PS_INPUT_ENA/ADDR`, and read
attributes through `SPI_PS_INPUT_CNTL` (see registers). **measured**

## Exports

A vertex stage that never exports to targets 12-15 writes no position.
Colour exports go to MRT slots, which the draw's target and shader masks then
filter. **measured**

## Images in shaders

- Type-10 descriptors are declared `Dim3D`. **measured**
- Storage images a compute pass reads before writing must be uploaded from
  guest memory first; a dispatch that read one untouched read zeros - the
  depth chain. **measured** (`2566676`)

## The shape of the generated code

The managed C# translator emits a program-counter dispatcher: a loop around
a switch on the counter, one case per GCN basic block. Values cannot stay in
SSA form across it, so the register file lives in Private arrays and about
40% of a module is loads and stores to it; one compute shader came to
129,517 SPIR-V instructions. (`NATIVE_SHADER_PATH_2026-09-12.md`)

The native C++ translator emits structured SPIR-V instead. `gen5_cfg.h`
builds the graph, `gen5_structure.h` finds dominators, back edges, natural
loops and merge points, and `gen5_emit_plan.h` / `gen5_emit_control.h` place
`OpLoopMerge` and `OpSelectionMerge` from them. Every shader decoded so far has
had a reducible graph. **code**

Getting it right for the scene after the intro took more than the textbook
version: a selection closes when a block its opener does not dominate comes
up; a branch whose arms never rejoin gets an unreachable merge; a second exit
reached from several places is copied to each; back edges go through the
continue target. With those, 70 of the 71 stages dumped from the scene pass
`spirv-val`; the other is a prologue ending in `s_setpc_b64`, which is not
translated. **measured** (`24a21e0`, `05b2a3d`)

A per-invocation loop budget (`PS5RT_LOOP_BUDGET`, 16384 back edges, 0 = off)
makes a loop whose exit was mistranslated leave instead of resetting the GPU.
**code** (`05b2a3d`)

## Checking a translation

`PS5RT_NATIVE_SHADER_DUMP=dir` writes each stage's guest code and user data
beside its module. `tools/gen5-disasm` reads them. The test of a translation
is behavioural: the same dispatch against the same inputs must leave the
same bytes behind, which surface dumps and the buffer traces measure.
