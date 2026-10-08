// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

using Ps5Recomp.ShaderCompiler;

namespace Ps5Recomp.ShaderCompiler.Vulkan;

public static partial class Gen5SpirvTranslator
{
    private sealed partial class CompilationContext
    {
        /// <summary>
        /// How a guest block leaves: what SPIR-V has to be emitted for it once
        /// everything the terminator needs has been computed.
        /// </summary>
        private enum StructuredExitKind
        {
            Return,
            Branch,
            Conditional,
        }

        private readonly record struct StructuredExit(
            StructuredExitKind Kind,
            uint Condition,
            int Target,
            bool TargetExits);

        /// <summary>
        /// Emits the guest's control flow as SPIR-V control flow, instead of a
        /// switch over a program counter held in a variable.
        /// </summary>
        /// <remarks>
        /// The dispatcher is correct for any graph, and that generality has a
        /// price that is not performance: a workgroup barrier cannot be placed
        /// correctly underneath it. The wave64 bridge needs one on every
        /// wave-wide branch, and inside a switch over a per-invocation counter
        /// a barrier is only reached in step while every invocation happens to
        /// hold the same counter. Loops are where that stops being true.
        ///
        /// One guest block is not one SPIR-V block. Emitting a guest
        /// instruction can open a selection of its own - the wave64 bridge
        /// does, on every wave-wide branch - so the block a terminator is
        /// written into is rarely the block whose label the rest of the code
        /// branches to. Two things follow, and both were learned from a driver
        /// crash rather than from reading the emitter. A loop header has to be
        /// a block of its own that does nothing but carry OpLoopMerge, because
        /// the back edge names it and the merge instruction has to be in the
        /// block that is named. And the branch condition has to be computed
        /// before the merge instruction is written, because the merge has to
        /// be the instruction immediately before the terminator, and computing
        /// a wave-wide condition emits blocks.
        ///
        /// Emission is destructive - there is no going back to the dispatcher
        /// once this has written into the module - so the result is checked
        /// against the structured control flow rules by the caller.
        /// </remarks>
        private bool TryEmitStructured(
            IReadOnlyList<ShaderBlock> blocks,
            Gen5ControlFlowAnalysis cfg,
            out string error)
        {
            error = string.Empty;
            var labels = new uint[blocks.Count];
            for (var index = 0; index < labels.Length; index++)
            {
                labels[index] = _module.AllocateId();
            }

            var headers = new Dictionary<int, Gen5ControlFlowAnalysis.Loop>();
            var latches = new HashSet<int>();
            // A loop whose merge block was taken by a selection enclosing it
            // gets one of its own, sitting between the latch and the block it
            // branches to. Inside such a loop "the merge" means this label,
            // not the block the analysis names, and every break has to say so
            // - a branch straight to the block would leave the loop by
            // something that is not its merge, which is the one thing SPIR-V
            // does not allow.
            var syntheticMerge = new Dictionary<int, uint>();
            var syntheticLatch = new Dictionary<int, int>();
            foreach (var loop in cfg.Loops)
            {
                if (loop.Header == loop.Latches[0])
                {
                    error =
                        $"loop header {loop.Header} is its own latch, which " +
                        "needs a synthesized continue block";
                    return false;
                }

                headers[loop.Header] = loop;
                latches.Add(loop.Latches[0]);
                if (cfg.SyntheticLoopMerges.Contains(loop.Header))
                {
                    syntheticMerge[loop.Header] = _module.AllocateId();
                    syntheticLatch[loop.Latches[0]] = loop.Header;
                }
            }

            // The innermost synthetic loop that both contains the branch and
            // merges where it is going. Anything else keeps the block label.
            uint TargetLabel(int from, int to)
            {
                var best = -1;
                foreach (var (header, _) in syntheticMerge)
                {
                    var loop = headers[header];
                    if (from >= header &&
                        from <= loop.Latches[0] &&
                        cfg.MergeTargets[header] == to &&
                        header > best)
                    {
                        best = header;
                    }
                }

                return best >= 0 ? syntheticMerge[best] : labels[to];
            }

            _module.AddStatement(SpirvOp.Branch, labels[0]);
            for (var index = 0; index < blocks.Count; index++)
            {
                _module.AddLabel(labels[index]);
                if (headers.TryGetValue(index, out var loop))
                {
                    var body = _module.AllocateId();
                    _module.AddStatement(
                        SpirvOp.LoopMerge,
                        syntheticMerge.TryGetValue(index, out var ownMerge)
                            ? ownMerge
                            : labels[cfg.MergeTargets[index]],
                        labels[loop.Latches[0]],
                        0);
                    _module.AddStatement(SpirvOp.Branch, body);
                    _module.AddLabel(body);
                }

                if (!TryEmitBlockInstructions(blocks, index, out error))
                {
                    error = $"block=0x{blocks[index].StartPc:X}: {error}";
                    return false;
                }

                if (!TryResolveStructuredExit(blocks, index, out var exit, out error))
                {
                    error = $"block=0x{blocks[index].StartPc:X}: {error}";
                    return false;
                }

                if (exit.Kind == StructuredExitKind.Conditional &&
                    !latches.Contains(index))
                {
                    var merge = headers.ContainsKey(index)
                        ? cfg.HeaderSelectionTargets[index]
                        : cfg.MergeTargets[index];
                    if (merge < 0 || merge >= blocks.Count)
                    {
                        error =
                            $"block=0x{blocks[index].StartPc:X}: no merge " +
                            "block for a conditional branch";
                        return false;
                    }

                    _module.AddStatement(
                        SpirvOp.SelectionMerge, TargetLabel(index, merge), 0);
                }

                EmitStructuredExit(exit, blocks, index, labels, TargetLabel);

                // Straight after the latch, which is where the block the loop
                // branches to would otherwise begin. The next iteration emits
                // that block, so the two end up adjacent and in the order the
                // construct needs.
                if (syntheticLatch.TryGetValue(index, out var owner))
                {
                    _module.AddLabel(syntheticMerge[owner]);
                    _module.AddStatement(
                        SpirvOp.Branch, labels[cfg.MergeTargets[owner]]);
                }
            }

            // Every path out of the guest's code returns, so whatever the
            // caller appends after this needs a block of its own to sit in.
            _module.AddLabel();
            return true;
        }

        /// <summary>
        /// Works out how a block leaves, emitting whatever the decision needs.
        /// </summary>
        /// <remarks>
        /// Separate from emitting the branch because computing a wave-wide
        /// condition opens and closes blocks of its own, and a merge
        /// instruction has to be adjacent to the terminator it belongs to.
        /// </remarks>
        private bool TryResolveStructuredExit(
            IReadOnlyList<ShaderBlock> blocks,
            int index,
            out StructuredExit exit,
            out string error)
        {
            error = string.Empty;
            exit = default;
            var block = blocks[index];
            var terminator = _state.Program.Instructions[block.EndIndex - 1];
            if (terminator.Opcode == "SEndpgm")
            {
                exit = new StructuredExit(StructuredExitKind.Return, 0, -1, true);
                return true;
            }

            if (terminator.Opcode == "SBranch")
            {
                if (!TryGetBranchTargetPc(terminator, out var targetPc))
                {
                    error = "invalid scalar branch target";
                    return false;
                }

                if (IsExitBranchTarget(_state.Program.Instructions, targetPc))
                {
                    exit = new StructuredExit(
                        StructuredExitKind.Return, 0, -1, true);
                    return true;
                }

                if (!TryFindBlock(blocks, targetPc, out var targetBlock))
                {
                    error = $"unknown scalar branch target 0x{targetPc:X}";
                    return false;
                }

                exit = new StructuredExit(
                    StructuredExitKind.Branch, 0, targetBlock, false);
                return true;
            }

            if (terminator.Opcode.StartsWith("SCbranch", StringComparison.Ordinal))
            {
                if (!TryGetBranchTargetPc(terminator, out var targetPc))
                {
                    error = $"invalid conditional branch {terminator.Opcode}";
                    return false;
                }

                var takenExits = IsExitBranchTarget(
                    _state.Program.Instructions, targetPc);
                var takenBlock = -1;
                if (!takenExits && !TryFindBlock(blocks, targetPc, out takenBlock))
                {
                    error = $"unknown branch target 0x{targetPc:X}";
                    return false;
                }

                // Last, because a wave-wide condition emits a ballot exchange
                // with a barrier and a selection around it, and everything
                // after this call has to stay in one block.
                if (!TryGetBranchCondition(terminator.Opcode, out var condition))
                {
                    error = $"invalid conditional branch {terminator.Opcode}";
                    return false;
                }

                exit = new StructuredExit(
                    StructuredExitKind.Conditional,
                    condition,
                    takenBlock,
                    takenExits);
                return true;
            }

            exit = index + 1 < blocks.Count
                ? new StructuredExit(StructuredExitKind.Branch, 0, index + 1, false)
                : new StructuredExit(StructuredExitKind.Return, 0, -1, true);
            return true;
        }

        private void EmitStructuredExit(
            StructuredExit exit,
            IReadOnlyList<ShaderBlock> blocks,
            int index,
            uint[] labels,
            Func<int, int, uint> targetLabel)
        {
            switch (exit.Kind)
            {
                case StructuredExitKind.Return:
                    _module.AddStatement(SpirvOp.Return);
                    return;
                case StructuredExitKind.Branch:
                    _module.AddStatement(
                        SpirvOp.Branch, targetLabel(index, exit.Target));
                    return;
            }

            // An arm that leaves the function has no block of the guest's own
            // to branch to, so it gets one that does nothing but return. The
            // same for a conditional in the last block, whose fall-through is
            // the end of the program.
            var takenLabel = exit.TargetExits
                ? _module.AllocateId()
                : targetLabel(index, exit.Target);
            var fallthroughExits = index + 1 >= blocks.Count;
            var fallthroughLabel = fallthroughExits
                ? _module.AllocateId()
                : targetLabel(index, index + 1);
            _module.AddStatement(
                SpirvOp.BranchConditional,
                exit.Condition,
                takenLabel,
                fallthroughLabel);
            if (exit.TargetExits)
            {
                _module.AddLabel(takenLabel);
                _module.AddStatement(SpirvOp.Return);
            }

            if (fallthroughExits)
            {
                _module.AddLabel(fallthroughLabel);
                _module.AddStatement(SpirvOp.Return);
            }
        }
    }
}
