# The draws leave no mark on their render targets

Measured 2026-09-14, with the native shader path driving every compute
shader and most graphics stages.

## What was measured

A run submits about fifteen thousand draws. They name four render targets,
all 3840x2160:

| target | draws |
| --- | --- |
| 0x520440000 | 4924 |
| 0x513BF0000 | 3058 |
| 0x511C10000 | 777 |
| 0x542AB0000 | 659 |

Two of them were read back from the GPU at flip 300 and sampled. Each
holds exactly **one distinct value across every pixel sampled**:

- 0x520440000, 129600 pixels sampled, one value.
- 0x513BF0000, 32400 pixels sampled, one value - `00 00 00 00 00 00 00 3C`,
  which as half floats is black with an alpha of one.

Both are eight bytes a pixel, so sixteen bits a channel.

A surface with one value in it has been cleared and not drawn into. Eight
thousand draws between them left no mark.

This is the rendered surface read back from the device, not the title's
own memory, so it is not a question of whether anything is copied back.

## Why this is the thing to look at

The frame that reaches the display is a flat colour, and has been for as
long as the project has been measuring it. That was previously narrowed to
"the scene target itself is zeros" rather than "the composition clobbers a
good frame". This is the same conclusion arrived at again from the other
end, and with the surface in hand rather than inferred: the draws are the
step that produces nothing.

It is also not the shader translator. The frame is equally flat with the
bridge producing the modules and with the native path producing them, and
the mean colour of a composed frame is identical to the unit between the
two. Whatever rejects these draws rejects them before or after a shader
runs, not inside one.

## The chain the frame goes through

Worth writing down, because it is longer than "draw then show" and each
link is a place the picture can be lost:

1. The scene is drawn into the targets above.
2. A compute shader at 0x50075D200 reads a 2432x1368 image at 0x53E020000
   and writes one at 0x53B9F0000.
3. The composition reads 0x53B9F0000 and writes 0x507410000.
4. The flip displays 0x507410000.

Every link exists and runs: the compute shader is dispatched sixteen times
a run, the composition draws 484 times, the flip happens hundreds of
times. The first link is the one with nothing in it.

## What this does not say

It does not say why. A draw that leaves no mark can be one whose geometry
never reaches the rasteriser - degenerate positions, a viewport or scissor
that excludes it - or one the pipeline state rejects: a depth test that
fails, a colour write mask of zero, a blend that discards. Each of those is
a different fix and they are distinguishable, but none of them has been
measured yet.

One thing already noticed while getting here, worth keeping: the index
buffer that reaches the runtime has an address of zero and a count of
923415, while the packets carry indexed draws with real addresses
(0x100CB2E560 with 24192 indices). Whatever is handing the runtime that
index buffer is not handing it the one the packet named.
