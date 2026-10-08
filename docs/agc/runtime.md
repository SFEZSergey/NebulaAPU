# How the runtime executes AGC

The title's code runs natively. When it submits a command buffer, the HLE
(`ps5rt_hle_impl.cpp`) parses it on the submitting thread and hands the GPU
runtime (`Ps5GpuRuntime.dll`, `ps5gpu_native.cpp`) a stream of shader-state
registrations, draws, dispatches and flips. A single worker thread in the
GPU runtime turns those into Vulkan.

```text
title thread                       GPU worker thread
------------                       -----------------
submit DCB/ACB
  parse packets                    take command from queue
  execute writes, DMA, labels      build/lookup pipeline for the state
  register shader state            read guest buffers and images
  snapshot per-draw constants      record, submit, present
  enqueue draw / dispatch / flip   ...
flip: wait until <= 2 frames ahead
```

## What must happen at submission, not later

The worker runs behind the title. Anything that lives in a per-frame ring
has been rewritten by the time the worker reaches it.

- **Constants.** Every buffer a state's manifest names, up to 64 KB, is
  copied when the draw is submitted and the worker uses the copy. The
  intro's vertex stage found its matrix zeroed otherwise. **measured**
- **Indices.** Same reason: read by the worker frames later they were
  another frame's, and the UI plane the video is drawn onto came out as
  scattered cells of the wrong triangles. **measured**
- **Memory writes the stream makes** (`WRITE_DATA`, `RELEASE_MEM`,
  `DMA_DATA`): executed at submission, because the title polls them.
- **Frames ahead.** The flip waits until the worker is at most two flips
  behind (`PS5GPU_NATIVE_MAX_FRAMES_AHEAD`). Unbounded, the intro's
  twelve-megabyte frame copies had all landed before the worker drew the
  first frame that reads them. **measured**

## Knowing what the guest wrote: write watches

The worker must not re-read gigabytes of unchanged memory, and must not
miss a write. It protects the pages of what it has read (`VirtualProtect`
to read-only) and a vectored exception handler catches the title's first
write, marks the range dirty and restores it.

Rules that each cost a bug:

1. **Arm before reading.** Armed after, a write between the read and the
   arming was never seen and stale bytes were kept - whole green frames of
   the intro video. **measured** (`2921e5e`)
2. **Every writer that is not the title's own store must say so.**
   `ReadFile` into guest memory and `WriteProcessMemory` fail on a protected
   page without faulting into the handler; files came in partly (the noise
   on the SIE plate). DMA packets and file reads now call
   `ps5rt_native_gpu_guest_written` first. **measured** (`2566676`)
3. **Watches overlap.** Two watches can cover a page, one already disarmed
   and one still holding the page read-only. The handler must look for an
   *armed* watch covering the fault, not the nearest one; taking the
   nearest, it declined a legitimate write and the title's main thread died
   writing light constants. **measured** (`f303853`)
4. **"Armed and clean" is not enough; check the arming serial.** A watch
   disarmed by a write and re-armed by someone else looks clean. A verdict
   taken under the first arming - "this image is empty" - then stays true
   forever. That was the PS Studios intro going green on every other frame,
   one run in four. Every cached verdict records the serial it was taken
   under. **measured** (`f303853`)
5. A range can include the title's own read-only pages; only its read-write
   parts are lowered and restored.

## Caches that rest on the watches

| cache | what it keeps | key | invalidated by |
| --- | --- | --- | --- |
| image upload | the uploaded surface | address, size, format | watch dirty |
| empty image | "all zero, don't read again" | address, size | watch dirty or new arming |
| buffer slot | a pipeline's read-only buffer >= 256 KB on the device | slot, address, size | watch dirty or new arming |
| shared buffer | device copy of a read-only range >= 1 MB, copied GPU-to-GPU into any slot | address, size | watch dirty or new arming |

The shared copy exists because the intro's biggest buffers are
double-buffered: a slot keeps one address, alternated every frame and
missed every time, re-reading 16 unchanged megabytes a frame. **measured**
(`46cec95`)

## Guest memory queries

Checking whether a guest range is committed and writable is
`VirtualQuery`, and the title asks about a hundred thousand times a frame.
Both the HLE and the GPU runtime keep per-thread region caches (32 entries).

- Protection changes are recorded with their range in a 256-entry ring,
  and a cached region survives the changes that do not overlap it. Dropping
  everything on each change put `VirtualQuery` at a fifth of the main
  thread. **measured** (`136b812`, `145634b`)
- MinGW has no native TLS: every `thread_local` access calls
  `__emutls_get_address`. The hot cache is reached through `TlsGetValue`
  instead; that alone was 13% of the main thread. **measured** (`5db7c80`)

## Compute states

This title registers a new compute state for nearly every dispatch. States
are retired once 2048 dispatches have passed without them. At 512 - a few
frames at seventy dispatches a frame - 4.6 states a frame were rebuilt;
at 2048, 0.7. Their buffers go back to a pool. **measured** (`46cec95`)

Graphics states do not take buffers from that pool: doing so made the intro
flash green in three runs of seven, for reasons not found. **measured**
(`e673596`)

## Ordering on the GPU

Compute runs through a ring of 16 command buffers. Every command buffer
starts with a full memory barrier, so each one sees everything submitted
before it; and a pipeline waits for its previous dispatch before its
buffers are refilled. With 4 slots, 91% of dispatches waited for a slot.
**measured** (`136b812`)

## The intro, in numbers

Per frame during the PS Studios video, after the work above:

| | before | after |
| --- | --- | --- |
| worker compute | 27.8 ms | 12.2 ms |
| buffer reads | 11.9 ms | 3.9 ms |
| flip latency, median / p90 | 60 / 147 ms | 19 / 23 ms |
| title main thread | 40.6 ms | 35.7 ms |

The flip latency was the audio drift: the player keeps the picture on the
sound, but a frame reached the screen up to 150 ms after the title handed
it over. The main thread is now the limit, mostly re-translating shaders
(see [shaders.md](shaders.md#addresses-baked-into-a-translation)).

## Traps in the traces

- `native_gpu.draw_processed` samples every eighth draw plus two addresses
  unconditionally; histograms over it over-count those two. Use
  `PS5GPU_NATIVE_TRACE_ALL_DRAWS=1` first.
- A watch that refuses to arm marks itself dirty, so `written_since=1` on an
  unarmed range says nothing about the title.
- Flip counts across a day vary by a factor of two on unchanged code.
  Compare within one sitting, or not at all.
- Many guest threads parked on semaphores and event flags is a job system
  at rest, not a deadlock.

## Switches

The ones used most, by purpose. The full list is in the source
(`grep -o '"PS5\(GPU_NATIVE\|RT\)_[A-Z0-9_]*"'`).

| purpose | switch |
| --- | --- |
| look at a surface | `PS5GPU_NATIVE_PROBE_SURFACES`, `PS5GPU_NATIVE_FRAME_DUMP_{ADDRESS,WIDTH,HEIGHT,FLIP,PATH,STATE,VERTICES,SLICE}` |
| look at buffers | `PS5GPU_NATIVE_TRACE_TARGET_BUFFERS`, `..._BUFFER_ADDRESS`, `..._BUFFER_FLOATS`, `..._BUFFER_TOP` |
| look at images | `PS5GPU_NATIVE_TRACE_IMAGE_ADDRESS`, `..._IMAGE_PIXELS`, `..._IMAGE_TOP`, `..._EMPTY_WATCH` |
| shaders | `PS5RT_NATIVE_SHADER_DUMP`, `PS5RT_COMPUTE_OLD_IDS`, `PS5GPU_NATIVE_FORCE_FIXED_SHADERS` |
| packets | `PS5RT_TRACE_DRAW_PACKETS`, `PS5RT_TRACE_REGISTER_WRITES`, `PS5RT_TRACE_DMA_DATA` |
| turn a mechanism off | `PS5GPU_NATIVE_BUFFER_CACHE=0`, `PS5GPU_NATIVE_NO_SHARED_BUFFERS=1`, `PS5GPU_NATIVE_NO_DIRECT_UPLOAD=1`, `PS5RT_NO_FAST_CLEAR=1`, `PS5RT_NO_GRAPHICS_DEPENDENCIES=1`, `PS5GPU_NATIVE_REGION_CACHE_STRICT=1` |
| crashes | `PS5RT_FATAL_PEEK` |
