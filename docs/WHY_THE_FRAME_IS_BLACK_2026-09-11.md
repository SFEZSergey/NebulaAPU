# Why the frame is black, 2026-09-11

The answer is not the renderer. The title is still loading, and it is still
loading because this runtime is about three orders of magnitude slower than
the hardware at the one thing the loader does most.

## The frame contains no scene

Counting the vertex count of every draw in a run:

```text
2335  vertices=3
  78  vertices=0
```

Not one draw carries geometry. 2335 are full-screen triangles - post
passes, fades, UI - and the 78 with a count of zero are indexed draws whose
count comes from the index buffer, so those are the only ones that could be
anything else. A frame is 31 draws, of which 24 execute and 7 are dropped
because CB_TARGET_MASK and CB_SHADER_MASK share no channel.

The surface the composition reads from is not empty either, it is *blue*:

```text
frame_dump address=0x514080000 size=1920x1080 R16G16B16A16_SFLOAT
nonzero=2073600 nonblack=2073600
```

All 2,073,600 pixels, peak channel exactly 1.0, and converting it gives a
uniform pure blue - a clear colour with nothing drawn over it.

So the picture is a loading screen, faithfully rendered.

## The loader is not stuck either

Over a thirteen minute run the APR path resolves 7133 paths, of which
**6613 are distinct**. Almost nothing repeats. The title is working steadily
through its asset set, not retrying a failure.

(An earlier reading of this data said two paths repeated 502 and 412 times.
That was a grep stopping at the space inside a filename, not a measurement.)

## What it is waiting on

`hle.frame_budget` says the guest thread is not blocked on us: a frame is
30 to 200 ms and `elsewhere` is essentially all of it, with `waited` in the
hundreds of microseconds. It is computing.

`worker_phases` says where the GPU worker's time goes, over 50 flips:

```text
draw=15ms/1558   compute=9957ms/2121   pipeline=2936ms/2121
resources=3681ms  buffers=2362ms  images=1464ms
```

Draws are free. **Compute is 4.7 ms a dispatch**, 42 dispatches a flip,
which is the 200 ms frame. On the hardware those dispatches are
microseconds.

And the 4.7 ms is not pipeline creation - only 22 Vulkan pipelines are ever
built in a run (`programs=22 pl_created=22`, `pl_create=44ms` in total).
It is the resources:

```text
pl_buf=2460ms/6739
buf_n=17922 buf_MB=9101 buf_repeat=8074/5221MB
buf_read=851ms/10858/7029MB
```

17922 buffers prepared, 9.1 GB of them, and **8074 of those - 5.2 GB - are
repeats of an address already seen**. The title registers a compute state
per dispatch, so every dispatch re-reads and re-uploads the buffers it was
given, whether or not anything wrote to them in between.

## What that means for the next step

The lever is not the bridge. Native strchr, strrchr and strstr took the
managed crossings from 470,000 to 140,000 in a 40 second run and the first
flip from 15.7 s to 8.7 s, which was worth doing, but what remains is
88,672 `sceAudioOut2PortSetAttributes` calls and a tail; the guest is not
waiting on the bridge.

The lever is the repeated buffer preparation. 2026-09-05 measured the same
thing from the other side - of repeat reads, 2661 of 2704 were byte
identical and the 43 that differed were all read-only descriptors - and
concluded that a blind frame-scoped cache would be wrong 1.6 per cent of
the time, which is unsound. The write watch this runtime already keeps for
textures answers it properly: a buffer whose guest pages have not been
written since the last upload does not need reading or uploading again.
`watch_faults` is 0 for textures over a whole run, so the machinery works;
it has simply never been pointed at storage buffers.

## Not established

How long the load actually is. 6613 files in thirteen minutes is about 8.5
a second, and nothing here says how many the first playable frame needs.

Whether the null dereference at a fixed offset in the main executable, which still ends some runs,
is on the loading path or beside it. The method it faults in searches a
linked list for a node whose first 32 bytes match a global key and then
dereferences that node's +0x10, which is null. A byte search finds no store
to the caller's stack slot that feeds it, so the field is meant to be
filled by another node in the same list.

## Correction, same day: which half is the cost

The paragraph above named the repeated reads as the lever. Reading the
phase timers apart says that is only half of it, and not the larger half.
`compute` is the whole of process_compute; `pipeline`, `resources` and
`record` are sub-phases of it, and `buffers` plus `images` make up
`resources`. So over 2121 dispatches:

```text
compute   9957ms      the whole dispatch
pipeline  2936ms      of which pl_buf=2460ms/6739 is creating buffers
resources 3681ms      of which buffers=2362ms, images=1464ms
record     690ms
```

Creating the per-state Vulkan buffers costs about as much as filling them,
2460 ms against 2362 ms, and both exist for the same reason: the title
registers a compute state per dispatch and each state owns its buffers.
Skipping only the refills would leave the 6739 creations in place and buy
around a tenth of the dispatch. Sharing the buffers by guest address
addresses both, and the write watch then decides the refills.

## Second correction: the buffer work was already measured, twice

Before writing any of it, the two ideas above turn out to be recorded in
ps5gpu_native.cpp with numbers, by whoever tried them:

Sharing buffer payloads across a guest submission - the read half - "saves
almost nothing: 879 hits and 271MB of 8208, reads 8933MB to 8208MB, the
buffer phase 5837ms to 5424ms", and one run of three ended in an access
violation. So of the 8074 repeats the counters report, only about 879 are
across dispatches at all; the rest are within one dispatch, which the
payload map already shares.

Pooling buffers by shape - the creation half - "works and is worth having:
6198 created falls to 502-691, the phase from 2742ms to 879-1014ms", but it
cannot live beside the deferred write-back, because the upload for the next
dispatch runs while the previous one is still reading. Measured that way,
throughput *falls*: "21, 46 and 56 flips against 59 to 70". Settling both
together needs the flush moved back in front of the upload, which returns
1.4 s of the 1.8 s saved.

So roadmap item seven as written twice today is wrong twice, and the file
said so before either version. The remaining question is the 2.6 s of the
9957 that none of pipeline, resources or record accounts for.

## The scene target is empty, and the composition is not why

Measured after geometry appeared, 11 September, late.

The title now issues real geometry: 2076 draws of 24192 vertices in one
run, indexed, triangle lists, into `0x520440000` at 3840x2160. The draw
state is not obviously wrong - `cb_target=0x3F` and `cb_shader=0xFF`, so
both MRT slots are written, and the surface is created as a framebuffer
in `R16G16B16A16_SFLOAT`.

Three dumps of the display buffer, at flips 25, 150 and 300, came back
with the same hash to the byte:

    hash=0xF3B2ACCA063CADC2 nonzero=8294400 nonblack=6641668

Flip 150 is before the first geometry draw (flip 211 in that run) and
flip 300 is after it. Nothing the game drew changed a single bit of what
reached the screen.

That first reads as the composition clobbering the frame, which is what
the note of 5 September said it does. It is not. Dumping the scene target
itself says so:

    stage=surface address=0x520440000 format=R16G16B16A16_SFLOAT
    bytes=66355200 hash=0x83B75B8167930383 nonzero=0 nonblack=0

Sixty-six megabytes of zero. The composition is faithfully presenting an
empty scene. Every downstream surface being flat is a consequence, not a
cause, and the earlier plan to go looking at `0x514080000` and
`0x53B9F0000` in turn would have found flat buffers and learned nothing.

The write-back counters agree that nothing arrives:

    wb_n=14185 wb_clean=14025 wb_missed=0 wb_unmarked=2264/8735MB

Ninety-nine percent of write-backs compare equal, so the device-side
memory after the draws holds what it held before them. `wb_missed=0`
rules out write-backs that were owed and skipped.

The draws are not being dropped: `guest_pipeline` runs 8 to 10 draws a
flip and `guest_descriptor` 3 to 17, and the two counters the draw loop
keeps for dropped draws stay at zero, as the comment beside them has said
since they were added.

So the question is no longer where the picture is lost between surfaces.
It is whether the geometry draws rasterize anything at all. The next
measurement dumps the Vulkan image straight after the geometry draw
rather than the guest memory behind it, which separates "the shaders
produced nothing" from "the result never came back". `PS5GPU_NATIVE_TRACE_ALL_DRAWS`
names the shader state to select for that dump.
