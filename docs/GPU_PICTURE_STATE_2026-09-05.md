# Where the picture stands, 2026-09-05

What a 40 second headless probe produces today: a black frame carrying one
flat grey wedge and a few dark UI panels. This is what was measured about
why, and - as importantly - which of the obvious explanations were checked
and are not it.

## The draws are not being dropped

86 render target addresses are asked for by the guest; the worker reaches
75. The missing 11 are backlog - the worker gets through 5141 of 12045
draws in a run - not a filter. Inside the frame loop nothing is dropped at
all: `no_surface=0 no_pipeline=0` over a run, every draw that reaches the
loop finds its surface and its pipeline.

A frame is 69 draws, of which 62 execute and 7 are masked by
CB_TARGET_MASK. 60 of the 62 run recompiled guest shaders. That is the
whole frame; the game really does draw this little at this point.

**Trap:** `native_gpu.draw_processed` traces every eighth draw *plus* the
display address and the composition source unconditionally. Any histogram
built from it over-counts exactly those two addresses. Set
`PS5GPU_NATIVE_TRACE_ALL_DRAWS=1` before drawing conclusions from it. This
cost a wrong reading on 2026-09-05.

Also note `draws=` in `native_gpu.vulkan_frame` counted the whole run
rather than the frame until 97d6ac9; older logs read as though 98 per cent
of a frame was being thrown away.

## The composition is compositing an empty surface

`kAstroCompositionSourceAddress` (0x53B9F0000) is stretched over the whole
display buffer, on top of what the frame already drew there. Its contents
are a uniform 0.05-peak grey with vertical striping, at flip 40 and
immediately after the draw that writes it - the same hash
(0xDE708B699B3F94D9) either way, so it is born like that rather than
overwritten.

Two passes write that address and neither produces anything:

- state 18, at 1920x1080 R8G8_UNORM: entirely zero.
- state 30, at 2432x1368 R16G16B16A16_SFLOAT: samples *itself* through a
  read copy plus two 1x1 textures, so with a cleared target in and nothing
  else, flat grey out.

Turning the composition off leaves the display buffer with the geometry the
frame drew - black, one grey wedge, 11.6 per cent non-black against 80 per
cent with it. So the composition strictly costs the frame today.

Two guards were tried on it and both are dead conditions, measured, not
committed:

- `device_written` - the pass that *clears* a surface sets it too, so an
  untouched surface still reports written.
- `draw_target`, set when a draw names the surface - true here, because
  draws do target 0x53B9F0000.

`kAstroCopyPasses` is stale: the copy pass that used to fill the source,
the shader at 0x500795200, does not appear once in this capture.

## What is actually missing: textures nothing ever fills

A shader samples 105 distinct addresses in a run. 48 read all zero.

- 18 are render targets, expected and handled.
- 21 are neither render targets on the guest side nor uploaded with content
  even once, sitting in the committed PAGE_READWRITE region at
  0x500000000.

Walking outward from each empty address a page at a time
(`PS5GPU_NATIVE_TRACE_EMPTY_NEIGHBOURHOOD=1`), 30 addresses have no
non-zero byte within eight megabytes in either direction. Nothing was
loaded anywhere near them. 8 have data nearby, but only 4 at exactly one
page before, and one of those four is the display buffer itself - so this
is a heap with neighbours, not a systematic offset in the descriptor base.

The guest never writes them. Arming the write watch on each empty range
(`PS5GPU_NATIVE_TRACE_EMPTY_WATCH=1`) and asking on every later sighting,
fourteen addresses report armed and clean on all sixty-odd frames that
sample them - the bytes do not arrive late, they do not arrive at all.
Two, 0x50FC30000 and 0x513070000, still refuse to arm and stay unknown
rather than clean. Not for the reason first assumed: after 54443bb a range
spanning several sub-regions arms fine, and what blocks these two is that
part of their range is already PAGE_READONLY under a different watch of
ours. Watches are per texture and overlap, and nothing merges them.

Note when reading the trace that a refused arm marks itself dirty, so
`written_since=1` on an address with `armed_now=0` says nothing about the
guest. That is the right behaviour for the upload path - a range we cannot
watch must be assumed changed - and a trap for anyone reading it as a
finding.

The two addresses that are not 64KB aligned, 0x555F59000 and 0x55C009000,
looked like a descriptor base off by a page and are not. Counting how many
of each texture's own pages carry anything starting from the nearest hit,
0x555F59000 wants 2048 and finds one; the best of the whole set,
0x513DD0000, finds 48 of 640 and those start two and a half megabytes away.
They are empty textures that happen to sit beside something.

## The watch system, once its counters were visible

`worker_phases` had outgrown the trace buffer and was cut off mid-word, so
watch_arms, watch_faults and watch_refused had not been printed in any run
for a while. With the buffer at 4096:

- `watch_faults=0` over a whole run. Not one guest write faults through an
  armed page, which is the empty-texture finding arrived at from the other
  direction.
- `watch_refused` was 21 against 19 arms - more refused than armed - and is
  6 after 54443bb. The six left are genuine overlaps.
- Textures skipped on the watch went from 317 a run to 388-445.

Flip counts across the day ran 60 to 112 on unchanged code, so nothing in
this section is a throughput claim. Measure within one sitting or not at
all.

## Not established

42 of 47 guest threads are blocked, 13 of them continuously for the whole
run, on semaphores (0x10009, 0x1000D-F), event flags (0x10051, 0x10053)
and condition variables. This is *not* evidence of a fault: a job system
parks its worker pool exactly like this when idle. It is written down
because it will look alarming in a log, not because it is a finding.

The guest is not blocked on us either: `hle.frame_budget` reports a 55ms
frame of which about 1.2ms is spent waiting on the GPU and the rest is the
guest's own code.

16 distinct pixel shaders appear in the first quarter of a run and no new
one appears in the last quarter, so the title reaches a steady screen early
and repeats it. Whether that is the intro looping correctly or the title
stuck has not been established.
