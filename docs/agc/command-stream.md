# The command stream

AGC hands the GPU command buffers of PM4 packets: DCBs for graphics, ACBs for
compute. `parse_agc_dcb` in `ps5rt_hle_impl.cpp` walks them on the submitting
thread and turns them into register state, draws, dispatches, memory writes
and flips for the GPU runtime.

## Packet header

A type-3 header (`header >> 30 == 3`) carries the opcode in bits 8-15 and the
length in its count field. Type 2 is a one-dword filler and is skipped.
Anything else ends the walk. **measured**

Bits 2-7 of the header matter for AGC's NOP packets: AGC reuses `NOP` (0x10)
with a sub-code in those bits for its own operations. Treating every NOP as
padding loses resets, register uploads, DMA copies and flips. **measured**

## Opcodes seen and what we do with them

| opcode | name | handling |
| --- | --- | --- |
| 0x10 | NOP | Dispatched on the sub-code, below |
| 0x11 | SET_BASE | Selector 1: the base address for indirect draw and dispatch arguments |
| 0x15 | DISPATCH_DIRECT | Compute dispatch with the current SH state |
| 0x16 | DISPATCH_INDIRECT | Same, arguments read from the SET_BASE address |
| 0x24 | DRAW_INDIRECT | Draw, arguments from memory |
| 0x25 | DRAW_INDEX_INDIRECT | Indexed draw, arguments from memory |
| 0x26 | INDEX_BASE | 64-bit address of the index buffer, held until the next one |
| 0x27 | DRAW_INDEX_2 | Indexed draw with its own base |
| 0x2A | INDEX_TYPE | Bit 0-1: 0 is 16-bit indices, otherwise 32-bit |
| 0x2D | DRAW_INDEX_AUTO | Non-indexed draw |
| 0x30 | DRAW_INDEX_MULTI_AUTO | Non-indexed, several instances |
| 0x35 | DRAW_INDEX_OFFSET_2 | Indexed draw with an offset and no base - uses INDEX_BASE |
| 0x37 | WRITE_DATA | Control word, 64-bit address, payload - executed as a guest write |
| 0x3C | WAIT_REG_MEM | 32-bit wait on memory; seven dwords |
| 0x47 | EVENT_WRITE_EOP | Never seen on this title; logged if it ever arrives |
| 0x49 | RELEASE_MEM | End-of-pipe: event control, address, 64-bit value - written |
| 0x63 | SET_SH_REG_INDIRECT | SH registers from a table in memory |
| 0x64 | SET_UCONFIG_REG_INDIRECT | UCONFIG registers from memory |
| 0x69 | SET_CONTEXT_REG | Context registers, inline |
| 0x76 | SET_SH_REG | SH registers, inline |
| 0x79 | SET_UCONFIG_REG | UCONFIG registers, inline |
| 0x93 | WAIT_REG_MEM (64) | The 64-bit wait has its own opcode - nine dwords, not a width flag |
| 0x9F | SET_CONTEXT_REG_INDIRECT | Context registers from memory |

**measured**, except where the handling says a packet was never seen.

Every geometry draw Astro Bot issues is `DRAW_INDEX_OFFSET_2`: the offset in
the packet, the base from the last `INDEX_BASE`. Reading the base from the
draw packet itself gave address zero and a count of 923415 for draws whose
real index buffer held 24192 indices. **measured**
(`THE_SCENE_TARGETS_ARE_UNTOUCHED_2026-09-14.md`)

## AGC's NOP sub-codes

| sub-code | meaning |
| --- | --- |
| 0x04 | DRAW_INDEX_AUTO in AGC's own form |
| 0x05 | Draw reset: clear SH, context and UCONFIG state and reseed defaults |
| 0x09 | ACB reset, the same for a compute stream |
| 0x0A | 32-bit memory wait |
| 0x11 | SH registers from memory |
| 0x12 | Context registers from memory |
| 0x13 | UCONFIG registers from memory |
| 0x15 | WRITE_DATA |
| 0x16 | 64-bit memory wait |
| 0x17 | Flip |
| 0x18 | RELEASE_MEM |
| 0x19 | DMA_DATA - a copy between two guest ranges, executed |

**measured**

## Default state

A real context starts with registers already set. The graphics stream never
asks for a reset, so decoding it against only what it wrote left every draw
without `CB_COLOR_CONTROL`, scissors and the rest: 100 context registers set
by the stream against 147 by the compute stream. The runtime seeds AGC's
defaults on first use of a stream and again on every reset. **measured**

Supported default-table versions: 7, 8, 10 and 13.

## Register uploads from memory

Two layouts. **measured**

- Native packets (0x63, 0x64, 0x9F): a 64-bit table address at dword 1 (low
  two bits masked), and the entry count in the low 14 bits of dword 4.
- NOP forms (0x11-0x13): the count at dword 1, the address at dwords 2-3.

Each table entry is an (offset, value) pair of 32-bit words.

**open**: AMD's naming calls the count field `num_dwords`, which would make
the count half of what we read. We and the other readings we compared treat
it as a pair count and the frames come out right; `PS5RT_AGC_COUNT_IS_DWORDS=1`
halves it to test the other reading.

## Register offsets carry a selector

The top nibble of a register offset (`0x70000000`) is not part of the
offset. Stored with it still attached, a write is filed under a key nobody
reads - which is why `CB_TARGET_MASK` looked unset while the stream set it
every draw. Strip it. **measured**

One selector value, `0x10000000`, means something: it addresses the bank of
32 `SPI_PS_INPUT_CNTL` registers (0x191-0x1B0), and the offset is an index
into that bank. AGC's interpolant mapping is written through it. Dropped
without the remap, the mapping lands on the first 32 context registers.
**measured**

## Memory writes the stream makes

`WRITE_DATA`, `RELEASE_MEM` and `DMA_DATA` write guest memory. The runtime
executes them on the submitting thread, at submission, because the title
polls those locations (labels, fences) and waits for them.

Two consequences, both learned the hard way:

- A write the runtime makes behind the GPU runtime's write watches must tell
  it first (`ps5rt_native_gpu_guest_written`), or a texture the DMA just
  filled keeps its old upload. **measured**
- Executing at submission means the title's CPU side can run frames ahead of
  what the GPU runtime has drawn. The flip path waits until the worker is at
  most two frames behind (`PS5GPU_NATIVE_MAX_FRAMES_AHEAD`), or copies the
  title makes for later frames land before the frames that read the old
  contents are drawn. **measured**

## Metadata passes - draws that are not draws

After drawing into a target and before sampling it, AGC runs a rectangle
through a two-instruction pixel shader with `CB_COLOR_CONTROL` (0x202) bits
4-6 in a metadata mode:

- mode 2 - fast clear elimination,
- mode 6 - DCC decompression.

The shader is always the same four words:
`7E000280 F8001803 00000000 BF810000` - `v_mov_b32 v0, 0`, an export of red
and green to MRT0, `s_endpgm`. The colour block does not write what it
exports; it rewrites the target's compression state. **measured**

Drawn as an ordinary draw it wrote zeros into red and green of the UI plane
the intro video had just been drawn onto: black frames. It is recognised by
mode and shader words and not drawn. **measured**

### Fast clears

A fast clear writes no pixels: it marks the target's compression state as
holding a clear value, and the mode-6 pass at the end of the frame fills the
pixels nothing drew with `CB_COLOR0_CLEAR_WORD0/1` (context 0x323, 0x324).
A renderer without compression state has to do that itself. We record the
target and clear words at the mode-6 pass, and the next draw into that
target carries `PS5GPU_NATIVE_DRAW_CLEAR_FIRST`: the runtime clears it in its
own format before drawing. Without this the "Sony Interactive Entertainment"
screen came out light grey instead of black - the loading screen's
see-through UI plane showing the white sky behind it. **measured**
(`22563a8`)

Only mode 6. Mode-2 passes run on targets whose "clear value" changes every
frame, and applying it there breaks them. **measured** (`ae4ca39`)

`PS5RT_NO_FAST_CLEAR=1` turns the emulation off.

## Flips

A flip arrives as NOP sub-code 0x17 in the DCB, alongside the VideoOut call
that names the buffer. Astro Bot registers two 3840x2160 display buffers,
format `0x8100000000000000` (A2B10G10R10), tile mode 0, and alternates
between them. **measured**
