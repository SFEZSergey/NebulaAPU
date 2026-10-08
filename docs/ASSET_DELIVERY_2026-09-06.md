# Where the textures were supposed to come from, 2026-09-06

[GPU_PICTURE_STATE_2026-09-05.md](GPU_PICTURE_STATE_2026-09-05.md) ended on
a measurement rather than an answer: of 105 addresses a shader samples in a
run, 21 are neither render targets nor ever uploaded with content, and the
guest never writes those pages at all. That put the blocker upstream of the
GPU layer without saying where. This is where.

## The title does not read its assets through the file API

Every import that reaches SharpEmu is counted by NID, and the ranking was
printed twenty-four deep, which is enough to see the audio loop and nothing
else. Printed in full - 162 distinct NIDs - the loading end of a run reads:

```text
36  sceKernelOpen         1G3lF1Gg1k8
10  sceKernelStat         eV9wAD2riIA
 1  sceKernelFstat        kBwCPsYX-m4
 1  sceKernelClose        UK2Tl2DWUns
 1  sceKernelRead         Cg4srZ6TKbU
 0  sceKernelPread        +r3rMFwItV4
 0  sceKernelLseek        oib76F-12fk
```

Thirty-six files opened, one read, in thirty seconds. Both numbers
reproduce exactly across two runs. There is no fios2 in the import table,
no mmap of a package, and `ignored_guest_int41` is zero for a whole run, so
there is no `int 0x41` syscall path carrying reads behind the imports
either.

## It reads them through APR/AMPR, and that never starts

The title imports the whole PS5 asset-streaming API and nothing else that
could deliver bulk data:

```text
sceKernelAprResolveFilepathsToIds              WT-5NKy42fw
sceKernelAprResolveFilepathsToIdsAndFileSizes  gEpBkcwxUjw
sceKernelAprGetFileSize                        WvEu7yl3Ivg
sceKernelAprGetFileStat                        ApkYaHb8Sek
sceAmprCommandBufferConstructor                8aI7R7WaOlc
sceAmprCommandBufferSetBuffer                  N-FSPA4S3nI
sceAmprAprCommandBufferConstructor             a8uLzYY--tM
sceAmprAprCommandBufferReadFile                mQ16-QdKv7k
sceAmprMeasureCommandSizeReadFile              vWU-odnS+fU
sceAmprCommandBufferWriteKernelEventQueue_04_00  H896Pt-yB4I
sceKernelAprSubmitCommandBuffer                eE4Szl8sil8
sceKernelAprSubmitCommandBufferAndGetId        qvMUCyyaCSI
sceKernelAprSubmitCommandBufferAndGetResult    ASoW5WE-UPo
sceKernelAprWaitCommandBuffer                  rqwFKI4PAiM
```

That is the shape of it: resolve paths to file ids, ask their sizes, build
a command buffer of ReadFile commands, submit it, wait for the queue event.
The package carries the index this addressing needs -
`ampr_emu.index`, 24 MB, magic `AMPRIDX3`.

Of those twenty-two APR and AMPR imports, exactly three are ever called,
twice each: `sceAmprCommandBufferConstructor`,
`sceAmprAprCommandBufferConstructor`, `sceAmprCommandBufferSetBuffer`. Also
reproduced across two runs. The loader builds its two command buffers and
then stops: no path is ever resolved to an id, no ReadFile command is ever
written, nothing is ever submitted, nothing is ever waited on. The next
section is why, and it is not the API - the title is being started wrong.

So the twenty-one empty textures are not a GPU-side failure to see bytes
that arrived. No bytes were requested.

## Why it stops: it thinks it is a development build

Tracing every file and APR call with its arguments and its result
(`PS5RT_TRACE_ASSET_CALLS=1`) shows what the thirty-six opens are for.
One succeeds - `/app0/param.sfx`, opened, fstat'd, 0x4A7 bytes read,
closed. Every other one looks like this:

```text
/host/%ASOBI_ROOT%/target/data/system/gfx/sys_decal_water.gnfp
/host/%ASOBI_ROOT%/target/data/common/physics_config.xml
/host/%ASOBI_ROOT%/target/data/prein/save_data/levels.xml
```

All of them return 0x80020002, ENOENT. That is a devkit host path with an
unexpanded build-root variable in it, and no package can satisfy it -
SharpEmu's path resolver has `/app0`, `/hostapp`, `/temp0`, `/download0`
and a mount table, and default-denies anything else. Every one of those
files does exist in the package, at exactly the tail of the path:
`data/system/gfx/sys_decal_water.gnfp` and the rest.

The title picks that prefix because it was started with no arguments. The
package ships the command line it expects, in `args.txt`:

```text
-package -sequence product -mode product -odxBinary -perfDisplay false
-debugDisplay false -debugInput false
```

The runner was passing `argc=1` and the process name. Passing the shipped
line instead - `PS5RT_PACKAGE_ARGS=1` - changes the run completely. Not one
`/host/` path is requested. Instead the APR path that had never started
runs at full rate: in nineteen seconds, 133 calls to
`sceKernelAprResolveFilepathsToIdsAndFileSizes`, 121 to
`sceAmprAprCommandBufferReadFile`, 100 submits, 100 waits, in a steady
resolve / read / submit / wait / reset loop.

Then it crashes: `0xC0000005` with `rip` equal to the fault address,
0xA16425B25, which is in no image - a call through a pointer read out of a
buffer that no read ever filled. That is the expected consequence of
driving the path while every APR and AMPR entry point is still a
permissive stub returning zero, and it is why the argument passing is
opt-in rather than on by default. The default run is unchanged: 30
seconds, no fatal lines.

So the barrier has moved one step and is now precise: implement APR/AMPR.
KytyPS5's `src/libs/libAmpr.cpp` implements the whole surface -
`ResolveFilepathsToIdsAndFileSizes` at 585, `AprCommandBufferReadFile` at
1764, `SubmitCommandBufferAndGetResult` at 670, `WaitCommandBuffer` at 741
- under GPL-2.0.

## What this rules out

Three plausible readings of the same symptom were checked and are not it.

**GPU DMA is not filling textures.** `sceAgcDcbDmaData` and
`sceAgcAcbDmaData` record a packet the command-buffer walker never
executes, which looked like a hole big enough to lose texture uploads
through. Counted rather than assumed: 3174 calls in a run, and every single
one is four bytes - `max=4`, 12,696 bytes total. These are label and GDS
writes, not payload. The walker still does not execute them and that is
still worth fixing, but it is not this.

**Direct memory is not aliased.** `sceKernelMapDirectMemory` ignores the
physical offset it is given and hands back fresh zeroed pages, so two maps
of one offset would silently become two unrelated allocations - the guest
writing to one while the GPU reads the other. A run makes six maps, all of
distinct offsets, so no aliasing occurs. Two are relocated because the
address the title asked for was already taken, both without `MAP_FIXED`.
The textures in question all sit inside the third map, 0x87C00000 bytes
placed exactly where it was requested at 0x500000000.

**The guest issues no syscalls.** `PS5RT_IGNORE_INT41=1` skips guest
`int 0x41` instructions, which would have hidden any I/O done by statically
linked kernel code. The counter is zero for a whole run.

## Reference material for the path itself

KytyPS5's `src/libs/libAmpr.cpp` is GPL-2.0, which
[CLAUDE.md](../CLAUDE.md) permits porting, and covers the whole surface:
`Apr::ResolveFilepathsToIdsAndFileSizes` at 585,
`Apr::SubmitCommandBufferAndGetResult` at 670, `Apr::WaitCommandBuffer` at
741, `Ampr::CommandBufferConstructor` at 1634,
`Ampr::CommandBufferSetBuffer` at 1681, `Ampr::AprCommandBufferReadFile` at
1764, `Ampr::MeasureCommandSizeReadFile` at 1790,
`Ampr::CommandBufferGetNumCommands` at 2010, with the NID registrations at
758-771 and 2648-2703. SharpEmu has its own
`SharpEmu.Libs/Ampr/AmprExports.cs` and
`SharpEmu.Libs/Kernel/KernelAprCompatExports.cs`, which is what these calls
currently reach.

## What happened next

The APR/AMPR path is implemented and the title streams gigabytes through
it. See
[ASSET_PATH_SERVED_2026-09-07.md](ASSET_PATH_SERVED_2026-09-07.md), which
also settles the crash below - it had three separate causes, none of them
the missing bytes.

## Not established

Whether the crash under `PS5RT_PACKAGE_ARGS=1` has one cause or several is
not known. One unfilled buffer is enough to explain it, and no attempt was
made to find a second.

What the 41 `sceAmprCommandBufferGetCurrentOffset` calls and the 152
`GetNumCommands` calls are checking against is also unknown; the command
buffer layout has not been read yet.

## Instruments added for this

- `sharpemu_hle_mix` now prints the whole ranking in `part=n/m` chunks
  instead of the top twenty-four, which is what made any of this visible.
- `memory.allocate_direct`, `memory.map_direct`, `memory.map_direct_result`
  and `memory.map_flexible` are always on. They were behind
  `PS5RECOMP_TRACE_HLE`, which also turns on every other call the title
  makes and is too expensive to leave enabled, so the one class of event
  worth always having was the one never recorded. They fire at allocation
  time, not per frame.
- `PS5RT_TRACE_DMA_DATA=1` prints every DMA_DATA with a running total; the
  counters behind it are always kept. The previous trace was capped at the
  first four calls by `agc_trace_first`, which is why the four four-byte
  lines in every old log said nothing about the other 3170.
- `PS5RT_TRACE_ASSET_CALLS=1` prints every file, APR and AMPR call with
  its arguments, its return address, its result, and - for the calls whose
  first argument is a path - the path itself. This is what turned "the
  loader stops" into "the loader is asking for files under a devkit
  prefix".
- `PS5RT_PACKAGE_ARGS=1` starts the title with the command line in
  `args.txt` rather than with no arguments. Off by default, see above.
- `PS5RT_TRACE_IO=1` prints file reads. `io.open` and `io.open_failed` are
  always on. Both went to `trace_stderr`, which is a stub in
  `ps5rt_hle_impl.cpp`, so a failed open of a game asset was silent.
  `ps5rt_kernel_read_real` also threw away `ReadFile`'s result, reporting
  success with zero bytes on a failed read; it now records the error.
  Astro Bot reaches none of this - the NIDs wired to those two
  implementations, `wuCroIGjt2g` and `AqBioC2vF3I`, are not in its import
  table - so the fix is for whatever does.
