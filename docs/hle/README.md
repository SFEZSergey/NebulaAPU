# The libraries beside AGC

Not graphics, but each of these decided at some point whether a frame
appeared at all. Same conventions as [../agc/README.md](../agc/README.md):
**measured**, **code**, **inferred**, **open**.

## VideoOut and the window

- Astro Bot registers two 3840x2160 display buffers, A2B10G10R10, linear,
  and flips between them. **measured**
- Only one presenter may exist. With the native window on, the managed
  VideoOut had still been consulted on every call and started a second
  presenter - a second window and swapchain on a second device - and with
  the NVIDIA overlay on the driver took the process down on the first
  present. The calls the native VideoOut answers no longer reach the
  managed one. **measured** (`fe84db4`)
- Overlays (NVIDIA, recorders) draw into swapchain images through their own
  Vulkan layer: the images must allow colour-attachment use, and presents
  use the standard acquire and render-finished semaphores. **measured**
  (`c8d126c`)

## AvPlayer

Native, on Media Foundation (`ps5rt_avplayer.h`). The bridge's player could
not create a player in this runtime and the title skipped its intro.

- The intro is an MP4 in the title's video data directory: 3840x2160, 59.94
  fps, 8.5 s, stereo 48 kHz. **measured**
- Decoding is on a D3D11 device; software decoding managed 54 of the 60
  frames a second a 4K stream needs. `PS5RT_AVPLAYER_SOFTWARE=1` keeps it.
- The clock follows the audio device's play position while sound comes out.
  Started from the title's `start`, the sound trailed the picture by the
  device's start-up for the whole video. Frames the clock has passed are
  dropped before their 12 MB copy. **measured** (`8e15937`)
- `sceAvPlayerGetStreamInfo`: **32 bytes** on PS5, not the 40 of the PS4
  layout - the title keeps it in a 0x20 stack slot under its stack guard,
  and a 40-byte write killed the player's event thread on
  `__stack_chk_fail`. The video stream is **type 1**: the title looks for
  `"## Video on Stream %d"` with type 1 and enables that stream. Details at
  +8 (width, height, aspect as float), duration at +24. **measured**
  (`e8a3dff`)
- Events are delivered on a guest thread (it needs the title's TLS),
  through `event_callback(object, event, 0, null)`; the title sees 2, 3, 4,
  3, 1 in that order over the intro. **measured** On event 2 it asks for
  the stream info and enables the video stream, which makes 2 "ready";
  3 as play and 1 as stop are **inferred**.
- Frame buffers: an all-zero middle row of luma is a green frame on screen;
  the pacing trace counts them.

## libSceJson

Native (`ps5rt_json.h`), our own parser. The bridge's `String::c_str()`
returned null because it allocated from a guest allocator this runtime does
not provide; the loader thread that reads the title's JSON died on it after
the intros and the game stayed on a black screen. **measured** (`f303853`)

Layout, from what the title's code does with the objects:

- `sce::Json::Value`, 0x20 bytes. The title reads scalars through the
  references `getBoolean/getInteger/getUInteger/getReal` return; we keep
  the value's node at +0x00, an ownership flag at +0x08, the scalar at
  +0x10 and the type at +0x1C.
- `sce::Json::String`, 8 bytes - the title keeps one in an eight-byte stack
  slot with its stack guard right above. A pointer to a NUL-terminated
  buffer.
- `ValueType`: 0 null, 1 boolean, 2 integer, 3 unsigned integer, 4 real,
  5 string, 6 array, 7 object. The title checks for 4 before `getReal`.
  **code**
- `operator[]` hands out references, so a member's Value must stay at one
  address for as long as its parent lives.

36 functions are imported; `PS5RT_JSON_BRIDGE=1` hands them back to the
bridge, `PS5RT_TRACE_JSON=1` traces parses and missing keys.

## Threads and their ids

- `pthread_getthreadid` returns an id unique for the life of the process.
  Windows reuses a finished thread's id for the next thread within moments,
  which a console does not. Havok keys its per-thread contexts by this id
  (`m_threadIdToContext`) and caches it in the thread's static TLS at
  -0x74 from the thread pointer, initially -1. **measured**; that id reuse
  is what lost a loader thread's context once is **inferred** - the crash
  was seen once and not reproduced. `PS5RT_WINDOWS_THREAD_IDS=1` restores
  the Windows ids.
- The title's engine takes several recursive spin locks keyed by that id
  (resource manager, Havok); an id of 0 means free.
- Static TLS: module 1 (eboot) is 0xA0 bytes, 0x40 of them initialised from
  the template, placed immediately below the thread pointer. **measured**
- `mov %fs:0, reg` in the title's code is patched into a call that returns
  the thread pointer (visible as `call 0x822cc0000` followed by NOPs).

## Synchronisation

- POSIX semaphores and their `scePthreadSem` twins are native; through the
  bridge each semaphore word read was a `VirtualQuery`, and `sem_post` alone
  was a sixth of the main thread. Same ids, same error codes.
  `PS5RT_POSIX_SEM_MANAGED=1` restores the bridge. **measured** (`136b812`)
- Mutex types: 1 error-checking, 2 recursive, 3 normal (treated as
  recursive), 4 adaptive; a static initialiser of 1 means type 4.
- `clock_gettime` fills a real timespec; REALTIME ids 0, 9 and 10 return
  Unix time, the others the performance counter.

## Files

- The title reads through APR (the AMPR path): paths are resolved, then read
  in batches. `PS5RT_TRACE_APR=1` shows both.
- It probes a lot: about 218 distinct paths fail to resolve in a minute,
  most of them ordinary (save-data defaults, fonts it has fallbacks for).
- In this dump 110 animations the title looks for under
  `data/prein/effects/anim` sit in `data/prein/characters/anim` (and a few in
  `common/`). The extraction seems to keep one copy of a file the package
  has twice. Without them the scarf animations never load and the title
  dies instancing an "Animation" component after the intros. A failed path
  under `prein/effects/` is retried under `prein/characters/` and
  `prein/common/`; `PS5RT_APR_SIBLING_DIRS=0` turns that off.
  **measured**, the duplication **inferred**.

## When the title asserts

The title's asserts print `ASSERT: <file>:<line>` and then execute
`int 0x41`, which this runtime ignores (`PS5RT_IGNORE_INT41=1`), so the
code after a failed assert runs anyway. Some fire on every run and are
harmless as far as we know (an assert in the title's result code, 24 times at start). Others are
the first sign of a crash a few instructions later - read the assert before
the fault.

A guest worker thread that faults is aborted and logged as
`guest_worker_abort ... host_id=`; the process continues. The main thread's
faults are fatal and print registers, windows around rbx and r14, and the
guest return addresses on the stack.

## Open, at the time of writing

After the intros the title loads the title screen and, on some runs, dies at
a fixed offset in the main executable instancing an "Animation" component whose template has no
data. The early animator-graph asserts (driver variables of
an invalid type) are the leading suspect.
