# The vertex shaders produce no geometry

Measured 2026-09-24, continuing from THE_SCENE_TARGETS_ARE_UNTOUCHED.

## The three runs that settle it

The runtime can be told to replace the guest's shaders with a full-screen
triangle and a solid green fragment, either or both
(`PS5GPU_NATIVE_FORCE_FIXED_SHADERS` = `1`, `vertex`, `fragment`). The
main scene target, 0x520440000, read back from the device at flip 120:

| shaders | target holds |
| --- | --- |
| both replaced | `0000003c 0000003c` - as half floats (0, 1, 0, 1), green |
| fragment replaced only | `00000000 00000000` - nothing |
| the guest's own | one value, nothing |

Replacing both fills the target. So the pipeline state around the draw
accepts pixels: the render pass, the write masks, the depth test, the
viewport and scissor, the target binding - none of them is what empties
the frame. That was the other half of the question and it is now answered.

Replacing only the fragment shader fills nothing. A fragment shader that
writes green unconditionally cannot produce an empty target if it runs at
all, so it does not run: the rasteriser is given nothing to shade.

**The guest's vertex shaders produce no geometry.**

## What this rules out

- Pipeline state. Proven above.
- The command queue. Raising it from 16384 to 262144 stops every discard
  and changes nothing.
- The rect list approximation. Half this title's draws are rect lists,
  drawn here as a strip with a derived fourth vertex; drawing them as a
  plain three-vertex triangle instead changes nothing either.
- The shader translator. The frame is equally empty with the bridge
  producing the modules and with the native path producing them.

That last one is worth stating plainly: whatever is wrong is wrong in both
translators, or upstream of both.

## Where to look next

A vertex shader produces nothing when its positions are degenerate, when
it never writes the position built-in at all, or when the attributes it
reads are not bound. The first two are visible in the module; the third is
visible in what the manifest declares against what the draw supplies.

The native translator now resolves every descriptor it meets, so the
module it produces can be read directly and compared against what the
guest's registers say - which is the tool the bridge never offered.
