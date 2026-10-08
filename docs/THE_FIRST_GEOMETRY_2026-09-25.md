# The first geometry in a scene target

Measured 2026-09-25.

## What changed

Seeding the registers the hardware fills before a vertex shader - the
vertex index in v5, the instance index in v8, the wave's shape in s2 and
s3 - puts geometry into a scene target for the first time.

0x520440000, read back from the device at flip 120:

| | distinct values sampled |
| --- | --- |
| before | 1 |
| after | 2, split roughly in half |

The two values are (0,0,0,1) and (0,0,0,0) as half floats - an alpha of
one against an alpha of nothing. Painting that difference shows the shape:
**one triangle**, covering half the surface along the diagonal.

So the vertex shaders now produce positions. They did not before, and no
amount of work on the state around the draw would have changed that.

## What it says about what is left

A triangle across half the screen is what a rect list looks like when its
fourth corner is missing. Half this title's draws are rect lists - three
vertices that describe a rectangle, with the fourth derived as v0 + v2 -
v1 rather than read - and this runtime draws them as a strip with a fourth
vertex the guest never supplied.

That approximation was tested earlier and ruled out, correctly at the
time: with the vertex shaders producing nothing at all, drawing them one
way or the other made no difference. It is back in question now, because
there is geometry for the difference to show in.

KytyPS5 draws a rect list as a patch list and lets tessellation derive the
corner. That is the shape of the fix.

## What has not changed

The frame that reaches the display is still a flat colour. The scene
target now holds a triangle rather than nothing, but what it holds is
black - the pixel shaders write (0,0,0,1) where they run. Whether that is
correct for this pass or another thing to chase is not measured.
