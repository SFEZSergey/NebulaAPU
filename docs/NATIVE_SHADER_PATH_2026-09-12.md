# The native shader path, and why it cannot be a faithful port

Written 12 September 2026, after the decoding was ported and the SPIR-V
translator was read.

## Where the frame's time goes

Everything around the shaders has been measured and answered: the guest
heap, the lazy commit, the texture watches, the packet trace, the command
buffer ring. What is left is the shaders themselves. Of the 104 seconds a
run spends on compute, 62 are the GPU executing them, and the counters say
that is genuine work rather than a synchronisation artefact - 23178
dispatches take a ring slot and only 768 wait on a fence.

One compute shader measures 2362480 bytes and 129517 SPIR-V instructions.
Its profile:

    OpAccessChain   24551   21% of words
    OpLoad          21109   14%
    OpStore          8530    4%

Forty percent of the module is traffic to and from an emulated register
file held in Private arrays.

## Why the register file is in memory

It is not a careless choice. The translator emits a program counter
dispatcher: one loop, one switch on the counter, one case per GCN basic
block.

    loop {
      switch (pc) {
        case block0: ...; pc = ...; break;
        case block1: ...; pc = ...; break;
      }
    }

A value cannot stay in SSA form across that. Control leaves a case, goes
round the back edge and enters a different case, so every register a block
computes has to be stored and every register it reads has to be loaded.
The access chains are the direct consequence of the dispatcher, and the
dispatcher is the direct consequence of GCN control flow being
unstructured where SPIR-V requires structure.

The comment beside the iteration guard says what the shape costs in
robustness too: a mistranslated exit condition spins the dispatcher
forever and wedges the queue, so there is a step limit whose only purpose
is to make a wrong shader terminate.

## What this means for the port

A faithful port reproduces the 129517 instructions in C++. The reason for
moving the translation natively is to be able to work on exactly this, so
the port has to change it rather than carry it across.

The alternative is a structurizer: turn the guest's arbitrary control flow
graph into structured SPIR-V control flow, so blocks become real blocks
joined by real branches and registers become SSA values the driver can
keep in registers. The problem is known and solved elsewhere - LLVM's
StructurizeCFG, Emscripten's relooper - which makes it work rather than
research.

## What this means for the oracle

The plan until now was that the native path must produce the same words as
the C# one, byte for byte, with the title's own thousands of shaders as
test cases. That test holds only for a faithful port. A translator that
deliberately emits different and smaller code cannot be judged by it.

The test becomes behavioural instead: the same dispatch against the same
inputs must leave the same bytes behind. The frame dumps already do this -
a hash over a surface, compared between builds - and the same comparison
can be made per dispatch on the buffers a shader writes, which is a
narrower and sharper test than a whole frame.

Both are worth having. The decoding, which is ported faithfully, can still
be compared instruction for instruction against the C# decoder. Only the
translation changes shape.

## Where the port stands

Ported and tested:

- `spirv_builder.h` - the SPIR-V module writer.
- `gen5_shader_ir.h` - the decoded form of a program.
- `gen5_decoder_sop.h` - the five scalar families.
- `gen5_decoder_vop.h` - VOP1, VOP2, VOPC.
- `gen5_decoder_memory.h` - VOP3, VOP3P, DS, MTBUF, MUBUF, FLAT, SMRD,
  SMEM, MIMG.

523 opcodes across fourteen families, the tables emitted from the C# by
generators kept in `tools/`, the lengths written by hand with tests
because a wrong length desynchronises a whole program rather than spoiling
one instruction.

Not yet ported: the translation itself, and the CFG structurizer it now
needs first.
