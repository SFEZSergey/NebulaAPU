// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

namespace Ps5Recomp.ShaderCompiler.Vulkan;

/// <summary>
/// Checks the structured control flow rules against an emitted function body.
/// </summary>
/// <remarks>
/// A module that breaks them is not rejected at pipeline creation with a
/// message - the driver miscompiles it and the process dies somewhere else
/// entirely, which is how the first structured shader was found to be wrong.
/// There is no validation layer and no spirv-val on this machine, so the
/// checks a translator most needs are the ones it can run on its own output.
///
/// This reads the words the builder has already emitted rather than a
/// side-channel the emitter maintains, so it cannot agree with the emitter by
/// construction.
/// </remarks>
public static class SpirvStructuredValidation
{
    private const ushort OpFunction = 54;
    private const ushort OpFunctionEnd = 56;
    private const ushort OpLoopMerge = 246;
    private const ushort OpSelectionMerge = 247;
    private const ushort OpLabel = 248;
    private const ushort OpBranch = 249;
    private const ushort OpBranchConditional = 250;
    private const ushort OpSwitch = 251;
    private const ushort OpKill = 252;
    private const ushort OpReturn = 253;
    private const ushort OpReturnValue = 254;
    private const ushort OpUnreachable = 255;

    private sealed class Block
    {
        public uint Label;
        public int Index;
        public ushort Terminator;
        public bool TerminatorLast = true;
        public bool MergeAdjacent = true;
        public int MergeCount;
        public uint Merge;
        public uint Continue;
        public bool IsHeader;
        public bool IsLoop;
        public List<uint> Successors = [];
    }

    /// <summary>
    /// Returns an empty string when the body is structurally valid, and a
    /// short description of the first few violations when it is not.
    /// </summary>
    public static string Describe(IReadOnlyList<uint> functionWords)
    {
        var blocks = ReadBlocks(functionWords);
        if (blocks.Count == 0)
        {
            return string.Empty;
        }

        var problems = new List<string>();
        var byLabel = new Dictionary<uint, Block>();
        foreach (var block in blocks)
        {
            if (!byLabel.TryAdd(block.Label, block))
            {
                problems.Add($"label {block.Label} defined twice");
            }
        }

        CheckShape(blocks, byLabel, problems);
        var reachable = FindReachable(blocks, byLabel);
        var dominators = ComputeDominators(blocks, byLabel, reachable);
        CheckHeaders(blocks, byLabel, reachable, dominators, problems);
        CheckBackEdges(blocks, byLabel, reachable, dominators, problems);
        return problems.Count == 0
            ? string.Empty
            : $"{problems.Count} violation(s): " +
              string.Join("; ", problems.Take(6));
    }

    private static List<Block> ReadBlocks(IReadOnlyList<uint> words)
    {
        var blocks = new List<Block>();
        Block? current = null;
        var inFunction = false;
        var index = 0;
        var sawTerminator = false;
        var mergeAt = -1;
        var position = 0;
        while (index < words.Count)
        {
            var header = words[index];
            var length = (int)(header >> 16);
            var opcode = (ushort)(header & 0xFFFF);
            if (length <= 0 || index + length > words.Count)
            {
                break;
            }

            var operands = new uint[length - 1];
            for (var i = 0; i < operands.Length; i++)
            {
                operands[i] = words[index + 1 + i];
            }

            index += length;
            switch (opcode)
            {
                case OpFunction:
                    inFunction = true;
                    current = null;
                    continue;
                case OpFunctionEnd:
                    inFunction = false;
                    current = null;
                    continue;
            }

            if (!inFunction)
            {
                continue;
            }

            if (opcode == OpLabel)
            {
                current = new Block { Label = operands[0], Index = blocks.Count };
                blocks.Add(current);
                sawTerminator = false;
                mergeAt = -1;
                position = 0;
                continue;
            }

            if (current == null)
            {
                continue;
            }

            position++;
            if (sawTerminator)
            {
                current.TerminatorLast = false;
            }

            switch (opcode)
            {
                case OpSelectionMerge:
                    current.MergeCount++;
                    current.IsHeader = true;
                    current.Merge = operands[0];
                    mergeAt = position;
                    break;
                case OpLoopMerge:
                    current.MergeCount++;
                    current.IsHeader = true;
                    current.IsLoop = true;
                    current.Merge = operands[0];
                    current.Continue = operands[1];
                    mergeAt = position;
                    break;
                case OpBranch:
                    current.Successors.Add(operands[0]);
                    goto case OpReturn;
                case OpBranchConditional:
                    current.Successors.Add(operands[1]);
                    current.Successors.Add(operands[2]);
                    goto case OpReturn;
                case OpSwitch:
                    current.Successors.Add(operands[1]);
                    for (var i = 3; i < operands.Length; i += 2)
                    {
                        current.Successors.Add(operands[i]);
                    }

                    goto case OpReturn;
                case OpKill:
                case OpReturnValue:
                case OpUnreachable:
                case OpReturn:
                    current.Terminator = opcode;
                    sawTerminator = true;
                    if (current.MergeCount > 0 && mergeAt != position - 1)
                    {
                        current.MergeAdjacent = false;
                    }

                    break;
            }
        }

        return blocks;
    }

    private static void CheckShape(
        List<Block> blocks,
        Dictionary<uint, Block> byLabel,
        List<string> problems)
    {
        foreach (var block in blocks)
        {
            if (block.Terminator == 0)
            {
                problems.Add($"block#{block.Index} has no terminator");
            }

            if (!block.TerminatorLast)
            {
                problems.Add($"block#{block.Index} continues past its terminator");
            }

            if (block.MergeCount > 1)
            {
                problems.Add(
                    $"block#{block.Index} has {block.MergeCount} merge instructions");
            }

            if (!block.MergeAdjacent)
            {
                problems.Add(
                    $"block#{block.Index} merge instruction is not next to its terminator");
            }

            foreach (var target in block.Successors)
            {
                if (!byLabel.ContainsKey(target))
                {
                    problems.Add($"block#{block.Index} branches to undefined {target}");
                }
            }

            if (!block.IsHeader)
            {
                continue;
            }

            if (!byLabel.ContainsKey(block.Merge))
            {
                problems.Add($"block#{block.Index} merges to undefined {block.Merge}");
            }
            else if (block.Merge == block.Label)
            {
                problems.Add($"block#{block.Index} merges to itself");
            }

            if (block.IsLoop && !byLabel.ContainsKey(block.Continue))
            {
                problems.Add(
                    $"block#{block.Index} continues to undefined {block.Continue}");
            }
        }

        var mergeOwner = new Dictionary<uint, int>();
        var continueOwner = new Dictionary<uint, int>();
        foreach (var block in blocks.Where(static candidate => candidate.IsHeader))
        {
            if (!mergeOwner.TryAdd(block.Merge, block.Index))
            {
                problems.Add(
                    $"merge of block#{block.Index} is already the merge of " +
                    $"block#{mergeOwner[block.Merge]}");
            }

            if (block.IsLoop && !continueOwner.TryAdd(block.Continue, block.Index))
            {
                problems.Add(
                    $"continue of block#{block.Index} is already the continue of " +
                    $"block#{continueOwner[block.Continue]}");
            }
        }
    }

    private static HashSet<uint> FindReachable(
        List<Block> blocks,
        Dictionary<uint, Block> byLabel)
    {
        var reachable = new HashSet<uint>();
        var stack = new Stack<uint>();
        stack.Push(blocks[0].Label);
        while (stack.Count > 0)
        {
            var label = stack.Pop();
            if (!reachable.Add(label) || !byLabel.TryGetValue(label, out var block))
            {
                continue;
            }

            foreach (var target in block.Successors)
            {
                stack.Push(target);
            }

            if (block.IsHeader)
            {
                stack.Push(block.Merge);
                if (block.IsLoop)
                {
                    stack.Push(block.Continue);
                }
            }
        }

        return reachable;
    }

    private static Dictionary<uint, HashSet<uint>> ComputeDominators(
        List<Block> blocks,
        Dictionary<uint, Block> byLabel,
        HashSet<uint> reachable)
    {
        var live = blocks.Where(block => reachable.Contains(block.Label)).ToList();
        var all = live.Select(static block => block.Label).ToHashSet();
        var predecessors = live.ToDictionary(
            static block => block.Label, static _ => new List<uint>());
        foreach (var block in live)
        {
            foreach (var target in block.Successors)
            {
                if (predecessors.TryGetValue(target, out var list))
                {
                    list.Add(block.Label);
                }
            }
        }

        var dominators = live.ToDictionary(
            static block => block.Label, _ => new HashSet<uint>(all));
        var entry = blocks[0].Label;
        dominators[entry] = [entry];
        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var block in live)
            {
                if (block.Label == entry)
                {
                    continue;
                }

                HashSet<uint> next;
                if (predecessors[block.Label].Count == 0)
                {
                    next = [block.Label];
                }
                else
                {
                    next = new HashSet<uint>(all);
                    foreach (var predecessor in predecessors[block.Label])
                    {
                        next.IntersectWith(dominators[predecessor]);
                    }

                    next.Add(block.Label);
                }

                if (next.SetEquals(dominators[block.Label]))
                {
                    continue;
                }

                dominators[block.Label] = next;
                changed = true;
            }
        }

        return dominators;
    }

    private static void CheckHeaders(
        List<Block> blocks,
        Dictionary<uint, Block> byLabel,
        HashSet<uint> reachable,
        Dictionary<uint, HashSet<uint>> dominators,
        List<string> problems)
    {
        var backEdgeBlocks = new HashSet<uint>();
        foreach (var block in blocks)
        {
            if (block.IsLoop && byLabel.ContainsKey(block.Continue))
            {
                backEdgeBlocks.Add(block.Continue);
            }
        }

        foreach (var block in blocks)
        {
            if (!reachable.Contains(block.Label))
            {
                continue;
            }

            var conditional =
                block.Terminator is OpBranchConditional or OpSwitch;
            if (conditional && !block.IsHeader && !backEdgeBlocks.Contains(block.Label))
            {
                problems.Add(
                    $"block#{block.Index} branches conditionally with no merge");
            }

            if (!block.IsHeader)
            {
                continue;
            }

            if (reachable.Contains(block.Merge) &&
                dominators.TryGetValue(block.Merge, out var mergeDominators) &&
                !mergeDominators.Contains(block.Label))
            {
                problems.Add(
                    $"block#{block.Index} does not dominate its merge " +
                    $"block#{byLabel[block.Merge].Index}");
            }

            if (block.IsLoop &&
                reachable.Contains(block.Continue) &&
                dominators.TryGetValue(block.Continue, out var continueDominators) &&
                !continueDominators.Contains(block.Label))
            {
                problems.Add(
                    $"block#{block.Index} does not dominate its continue " +
                    $"block#{byLabel[block.Continue].Index}");
            }
        }
    }

    private static void CheckBackEdges(
        List<Block> blocks,
        Dictionary<uint, Block> byLabel,
        HashSet<uint> reachable,
        Dictionary<uint, HashSet<uint>> dominators,
        List<string> problems)
    {
        var continueOf = new Dictionary<uint, uint>();
        foreach (var block in blocks.Where(static candidate => candidate.IsLoop))
        {
            continueOf[block.Label] = block.Continue;
        }

        foreach (var block in blocks)
        {
            if (!reachable.Contains(block.Label))
            {
                continue;
            }

            foreach (var target in block.Successors)
            {
                if (target == block.Label ||
                    !dominators.TryGetValue(block.Label, out var blockDominators) ||
                    !blockDominators.Contains(target))
                {
                    continue;
                }

                if (!continueOf.TryGetValue(target, out var continueTarget))
                {
                    problems.Add(
                        $"block#{block.Index} branches back to " +
                        $"block#{byLabel[target].Index}, which is not a loop header");
                    continue;
                }

                // The back edge has to sit in the continue construct, which is
                // the blocks the continue target dominates.
                if (!blockDominators.Contains(continueTarget))
                {
                    problems.Add(
                        $"block#{block.Index} branches back to " +
                        $"block#{byLabel[target].Index} from outside its " +
                        "continue construct");
                }
            }
        }

        foreach (var (header, continueTarget) in continueOf)
        {
            if (!reachable.Contains(header) ||
                !byLabel.TryGetValue(continueTarget, out var block))
            {
                continue;
            }

            var reaches = false;
            var seen = new HashSet<uint>();
            var stack = new Stack<Block>();
            stack.Push(block);
            while (stack.Count > 0 && !reaches)
            {
                var current = stack.Pop();
                if (!seen.Add(current.Label))
                {
                    continue;
                }

                foreach (var target in current.Successors)
                {
                    if (target == header)
                    {
                        reaches = true;
                        break;
                    }

                    if (byLabel.TryGetValue(target, out var next) &&
                        dominators.TryGetValue(target, out var targetDominators) &&
                        targetDominators.Contains(continueTarget))
                    {
                        stack.Push(next);
                    }
                }
            }

            if (!reaches)
            {
                problems.Add(
                    $"continue block#{byLabel[continueTarget].Index} never " +
                    $"branches back to header block#{byLabel[header].Index}");
            }
        }
    }
}
