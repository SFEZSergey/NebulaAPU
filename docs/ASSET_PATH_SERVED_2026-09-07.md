# Serving the asset path, 2026-09-07

[ASSET_DELIVERY_2026-09-06.md](ASSET_DELIVERY_2026-09-06.md) ended with the
barrier named: the title streams its assets through APR/AMPR, none of which
was implemented, and it only reaches that path at all when it is started
with the command line its package ships. This is what it took to serve it.

## The path itself

Nineteen entry points across libkernel's APR half and libSceAmpr, ported
from KytyPS5's `src/libs/libAmpr.cpp` (GPL-2.0): resolve paths to file ids
and sizes, build a command buffer of ReadFile records, submit, wait, reset.
The title agrees with that command buffer layout without being asked - its
`sceAmprAprCommandBufferConstructor` arrives with `reserved_state0` at
`command_buffer+0x18` and `reserved_state1` at `+0x20`, which are exactly
`APR_COMMAND_BUFFER_MAP_OFFSET` and `APR_COMMAND_BUFFER_SG_OFFSET`.

As in Kyty, the guest-visible record carries an opcode and the header
counters the title reads back, while the parameters live in a side table
keyed by record offset. Nothing but this code executes these buffers, so
reproducing the hardware encoding would buy nothing.

`sceKernelAprGetFileStat` is deliberately left with the bridge: the title
does not call it, and guessing `SceKernelStat`'s layout to serve a call
that never comes would be worse than not serving it.

The first run with it: 72 paths resolved, zero failures, 67 reads, 63.9 MB
of real `.gnfp` where there had been zeroes.

## Three crashes, each a different thing

Serving the reads moved the run from nineteen seconds to a minute and a
half, through three separate faults. They are recorded here because each
one was diagnosed rather than guessed, and because the first two took
better instruments before they said anything.

**A call through an uninitialised worker callback.** `call [r14+0x128]`
with `rdi=[r14+0x130]`, from a Havok-shaped thread pool. Finding it needed
two fixes to the reporting. The guest image is mapped by hand so no host
unwinder walks out of it - the handler now scans the stack for return
addresses `is_guest_code` accepts, which gave the chain
`0x800410CD6 0x800410B30 0x8003CE2B4`. And everything the handler printed
after `fatal_exception=` was being lost, because astro-cycle.ps1 stops the
process the moment it sees that line; the diagnostics print before it now.

`trace_guest_thread_start` was reading `+0x08` and `+0x40` of the thread
descriptor, which is the middle of an inline C++ type name. The trampoline
at `0x8003CE260` does `mov rdi,[rbx+0x20]` then `call [rbx+0x18]`, and for
this pool that entry is the thunk at `0x8004119A0`, which dereferences
twice more before a virtual call. Following the real chain showed the
faulting field was already garbage when the thread started, so the pool
had not been set up yet - the setter that writes `0x800411260` into
`+0x128` had not run.

This one stopped happening and **why is not established**. The semaphore
handle truncation committed alongside it demonstrably never fired: every
handle that reached `sceKernelWaitSema` was already 32-bit clean. It is
recorded as a race this build perturbs, not as a fix.

**The host running out of commit.** `std::bad_alloc`, at 58 to 85 seconds.
`worker_phases` now carries `commit_free` and `phys_free`, which is what
made it visible: 967 MB of commit free at the first flip and 6 MB by the
sixty-first. The title maps 10.2 GB of direct memory in six calls and
touches a fraction of it, and every byte was committed up front. Maps at or
above 64 MB are reserved now and committed as they are touched.

The granule turned out to matter more than the idea. At 64 KB the
reservation is cut into thousands of regions, and every later VirtualQuery
walk over guest memory steps through all of them: a 40 second run fell from
about sixty flips to three. Four megabytes keeps a gigabyte of working set
at a few hundred regions, and the same run reaches 302.

**A memcpy inside ucrtbase writing a read-only guest page.** Two causes,
which `fatal_where` separated by printing the faulting page's protection.
With `protect=4` it was two threads faulting on the same reserved block -
the first commits it, the second arrives to find it usable, and the
recovery declined that case and called it fatal; it retries now, at most
twice for one address so a genuine fault cannot loop. With `protect=2` it
was the real thing: the HLE side caches range writability and the GPU
module arms texture pages read-only from its own threads with no way to say
so. Dropping the cache for writes was correct and cost twenty times the
frame rate. The GPU module now bumps a generation counter the executable
exports, and stale entries are ignored. It reports
`native_gpu.host_protect_generation resolved=1`.

## Where this leaves the picture

With `PS5RT_PACKAGE_ARGS=1` over 100 seconds: no crash, 360 flips, 864 MB
of assets streamed, and **227 of 32339 images read as all zero**. On
2026-09-05 that number was 634 of 704. The default path over 40 seconds is
not just unregressed but better: 302 flips against about sixty before, and
259 of 24914 zero.

The textures have content. The picture does not: the sampled pixel is
`0xC0000000` on all 217 sampled flips, unchanged. 360 identical frames is
consistent with the title sitting on a black loading screen while it
streams, and equally consistent with the frame it draws never reaching the
display buffer. Nothing here distinguishes those, and that is the next
question.

## Not established

Why the worker-pool crash stopped. See above - the change that plausibly
addressed it never fired.

Whether the title is loading toward something or looping. 864 MB in 100
seconds with no visible change could be either.

Whether `commit_free` at 1268 MB by the end of a 100 second run is a slow
leak or the working set levelling off. Only longer runs answer that, and
the probe was 150 seconds.
