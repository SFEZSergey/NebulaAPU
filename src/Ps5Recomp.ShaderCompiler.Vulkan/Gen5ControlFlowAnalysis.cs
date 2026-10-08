// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

namespace Ps5Recomp.ShaderCompiler.Vulkan;

/// <summary>
/// Reducibility and loop analysis over a shader's basic-block graph.
/// </summary>
/// <remarks>
/// The translator emits a program-counter dispatch loop - one SPIR-V switch
/// over every block - which is correct for any control flow at all, including
/// irreducible graphs, and costs the whole GCN register file living in memory,
/// because no value can be proven to survive an iteration of the dispatcher.
/// Structured control flow removes that cost, but only exists for graphs of
/// the right shape. This says which shape a graph has, so the decision to
/// structure it is made on the graph rather than on hope.
///
/// Nothing here emits anything. It is deliberately separable from the
/// translator: the input is a successor list, and the output is a description.
/// </remarks>
internal sealed class Gen5ControlFlowAnalysis
{
    private Gen5ControlFlowAnalysis(
        int blockCount,
        int edgeCount,
        bool reducible,
        IReadOnlyList<Loop> loops,
        IReadOnlyList<int> depth,
        IReadOnlyList<int> postDominators,
        IReadOnlyList<int> mergeTargets,
        IReadOnlyList<int> headerSelectionTargets,
        IReadOnlySet<int> syntheticLoopMerges,
        string layoutRejection)
    {
        BlockCount = blockCount;
        EdgeCount = edgeCount;
        Reducible = reducible;
        Loops = loops;
        Depth = depth;
        PostDominators = postDominators;
        MergeTargets = mergeTargets;
        HeaderSelectionTargets = headerSelectionTargets;
        SyntheticLoopMerges = syntheticLoopMerges;
        LayoutRejection = layoutRejection;
    }

    /// <summary>A natural loop, named by the block every path into it passes.</summary>
    internal sealed record Loop(
        int Header,
        IReadOnlyList<int> Latches,
        IReadOnlySet<int> Body,
        IReadOnlySet<int> ExitTargets);

    public int BlockCount { get; }

    public int EdgeCount { get; }

    /// <summary>
    /// False when some loop can be entered other than through its header.
    /// Such a graph has no structured form without duplicating blocks.
    /// </summary>
    public bool Reducible { get; }

    public IReadOnlyList<Loop> Loops { get; }

    /// <summary>Loop nesting depth per block; 0 outside every loop.</summary>
    public IReadOnlyList<int> Depth { get; }

    /// <summary>
    /// Immediate post-dominator per block, or -1 for the virtual exit.
    /// A conditional branch's join is where the two arms come back together,
    /// which is what a structured selection needs to name as its merge.
    /// </summary>
    public IReadOnlyList<int> PostDominators { get; }

    /// <summary>
    /// The block each header names as its merge, or -1 for blocks that are
    /// not headers. SPIR-V allows a block to be the merge of at most one
    /// header, so this cannot simply be the post-dominator: nested selections
    /// routinely share one, and the inner one has to name something else.
    /// </summary>
    public IReadOnlyList<int> MergeTargets { get; }

    /// <summary>
    /// A loop header's own selection merge, or -1 where it has none. The
    /// header names the loop's merge, so a header that also ends in a
    /// conditional branch needs a second one for the selection - the loop
    /// merge is already claimed and a block may be the merge of only one
    /// header.
    /// </summary>
    public IReadOnlyList<int> HeaderSelectionTargets { get; }

    /// <summary>
    /// Loop headers whose merge is a block of its own rather than the block
    /// named in <see cref="MergeTargets"/>, which the loop then branches to.
    ///
    /// A selection that skips a whole loop wants the block after the loop as
    /// its merge, and that is the same block the loop wants. Only one header
    /// may name a block, so one of them has to give it up - and it has to be
    /// the loop, because the selection encloses it and a merge that sits
    /// inside the selection would not close it. The emitter gives the loop a
    /// block between its latch and that merge whose only content is a branch
    /// to it, which is where a break inside the loop goes.
    /// </summary>
    public IReadOnlySet<int> SyntheticLoopMerges { get; }

    /// <summary>
    /// Empty when the graph's block layout is one a range-recursive emitter
    /// can walk; otherwise the first reason it is not, for the trace.
    /// </summary>
    public string LayoutRejection { get; }

    /// <summary>
    /// Whether structured control flow can be emitted for this graph without
    /// duplicating or reordering blocks: the shape has to be right and the
    /// layout has to match it.
    /// </summary>
    public bool Emittable => Structurable && LayoutRejection.Length == 0;

    public int MaxDepth => Depth.Count == 0 ? 0 : Depth.Max();

    /// <summary>
    /// Whether this graph is one the structured emitter can handle today.
    /// Deliberately narrow: a loop with two latches needs a synthesized
    /// continue block, and one with several exit targets needs a synthesized
    /// merge block plus a variable to remember which exit was taken. Both are
    /// ordinary techniques and both are additions to make later, on evidence
    /// that real shaders need them, rather than up front.
    /// </summary>
    public bool Structurable =>
        Reducible &&
        Loops.All(static loop =>
            loop.Latches.Count == 1 && loop.ExitTargets.Count <= 1);

    public static Gen5ControlFlowAnalysis Analyze(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        IReadOnlyList<bool>? conditionalTerminators = null)
    {
        if (blockCount <= 0)
        {
            return new Gen5ControlFlowAnalysis(
                0, 0, true, [], [], [], [], [], new HashSet<int>(),
                string.Empty);
        }

        var edgeCount = 0;
        for (var block = 0; block < blockCount; block++)
        {
            edgeCount += successors[block].Count;
        }

        var order = ComputeReversePostorder(blockCount, successors);
        var dominators = ComputeDominators(blockCount, successors, order);
        var predecessors = BuildPredecessors(blockCount, successors);

        // A retreating edge that is not dominated by its target is an entry
        // into a loop that bypasses the loop's header, which is exactly what
        // makes a graph irreducible.
        var reducible = true;
        var backEdges = new List<(int Latch, int Header)>();
        var visiting = new bool[blockCount];
        var finished = new bool[blockCount];
        var stack = new Stack<(int Block, int Next)>();
        stack.Push((0, 0));
        visiting[0] = true;
        while (stack.Count > 0)
        {
            var (block, next) = stack.Pop();
            if (next >= successors[block].Count)
            {
                visiting[block] = false;
                finished[block] = true;
                continue;
            }

            stack.Push((block, next + 1));
            var target = successors[block][next];
            if (visiting[target])
            {
                if (Dominates(dominators, target, block))
                {
                    backEdges.Add((block, target));
                }
                else
                {
                    reducible = false;
                }

                continue;
            }

            if (!finished[target])
            {
                visiting[target] = true;
                stack.Push((target, 0));
            }
        }

        var byHeader = new Dictionary<int, List<int>>();
        foreach (var (latch, header) in backEdges)
        {
            if (!byHeader.TryGetValue(header, out var latches))
            {
                latches = [];
                byHeader.Add(header, latches);
            }

            latches.Add(latch);
        }

        var loops = new List<Loop>();
        var depth = new int[blockCount];
        foreach (var (header, latches) in byHeader.OrderBy(static pair => pair.Key))
        {
            var body = CollectLoopBody(header, latches, predecessors);
            var exits = new HashSet<int>();
            foreach (var member in body)
            {
                foreach (var target in successors[member])
                {
                    if (!body.Contains(target))
                    {
                        exits.Add(target);
                    }
                }
            }

            foreach (var member in body)
            {
                depth[member]++;
            }

            loops.Add(new Loop(header, latches, body, exits));
        }

        var postDominators = ComputePostDominators(blockCount, successors);
        var conditional = conditionalTerminators ?? [.. Enumerable.Range(
            0, blockCount).Select(block => successors[block].Count > 1)];
        var mergeTargets = new int[blockCount];
        Array.Fill(mergeTargets, -1);
        var headerSelectionTargets = new int[blockCount];
        Array.Fill(headerSelectionTargets, -1);
        var syntheticLoopMerges = new HashSet<int>();
        var rejection = reducible
            ? DescribeLayoutRejection(
                blockCount, successors, loops, postDominators)
            : "irreducible";
        if (rejection.Length == 0)
        {
            rejection = AssignMergeTargets(
                blockCount,
                successors,
                loops,
                postDominators,
                conditional,
                mergeTargets,
                headerSelectionTargets,
                syntheticLoopMerges);
        }

        if (rejection.Length == 0)
        {
            rejection = DescribeNestingRejection(
                blockCount, successors, loops, mergeTargets);
        }

        return new Gen5ControlFlowAnalysis(
            blockCount,
            edgeCount,
            reducible,
            loops,
            depth,
            postDominators,
            mergeTargets,
            headerSelectionTargets,
            syntheticLoopMerges,
            rejection);
    }

    // SPIR-V lets a block be the merge of at most one header, and nested
    // selections share a post-dominator all the time: an if inside an if that
    // both fall through to the same place. Where the post-dominator is taken,
    // the inner selection merges at its own fall-through instead, and its
    // taken arm branches out to the enclosing merge - which is what a break
    // out of a selection is, and is allowed. Where neither block is free
    // there is no assignment without inserting a block, and this refuses
    // rather than emitting a module that is quietly invalid.
    private static string AssignMergeTargets(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        IReadOnlyList<Loop> loops,
        IReadOnlyList<int> postDominators,
        IReadOnlyList<bool> conditional,
        int[] mergeTargets,
        int[] headerSelectionTargets,
        HashSet<int> syntheticLoopMerges)
    {
        var claimed = new bool[blockCount + 1];
        var claimedByLoop = new int[blockCount + 1];
        Array.Fill(claimedByLoop, -1);
        var isHeader = new bool[blockCount];
        var headerLatch = new int[blockCount];
        Array.Fill(headerLatch, -1);
        var latches = new HashSet<int>();
        foreach (var loop in loops)
        {
            isHeader[loop.Header] = true;
            headerLatch[loop.Header] = loop.Latches[0];
            latches.Add(loop.Latches[0]);
            var merge = loop.Latches[0] + 1;
            if (merge >= blockCount)
            {
                return $"loop{loop.Header}:merge-past-end";
            }

            if (claimed[merge])
            {
                return $"loop{loop.Header}:merge{merge}-taken";
            }

            claimed[merge] = true;
            claimedByLoop[merge] = loop.Header;
            mergeTargets[loop.Header] = merge;
        }

        for (var block = 0; block < blockCount; block++)
        {
            if (latches.Contains(block) || !conditional[block])
            {
                continue;
            }

            // Three candidates, in order of how much of the construct they
            // keep. The post-dominator encloses both arms. The taken target
            // encloses the fall-through arm alone and turns an if-then-else
            // into an if-then whose second arm leaves by an enclosing merge,
            // which is a break and is allowed. The fall-through encloses
            // nothing and only works when the taken arm leaves outward too.
            var taken = successors[block]
                .FirstOrDefault(target => target != block + 1, -1);
            var candidates = new[] { postDominators[block], taken, block + 1 };

            // A loop header's selection has to merge inside its own loop; the
            // post-dominator of a header is normally past the loop's end,
            // where a merge would enclose the loop rather than sit in it.
            var limit = isHeader[block] ? headerLatch[block] : blockCount - 1;
            var candidate = -1;
            foreach (var option in candidates)
            {
                if (option <= block || option > limit || isHeader[option])
                {
                    continue;
                }

                if (!claimed[option])
                {
                    candidate = option;
                    break;
                }

                // Taken by a loop this block's selection would enclose. That
                // is the one case where the claim can be moved rather than
                // refused: the loop is inside the selection, so its merge has
                // to be inside it too, and a block of the loop's own between
                // the latch and this one is exactly that. Both shaders in the
                // title that cannot be structured fail here - one selection
                // skipping one loop, in each of them.
                var owner = claimedByLoop[option];
                if (owner > block && owner < option &&
                    !syntheticLoopMerges.Contains(owner))
                {
                    syntheticLoopMerges.Add(owner);
                    claimedByLoop[option] = -1;
                    candidate = option;
                    break;
                }
            }

            if (candidate < 0)
            {
                return $"block{block}:no-free-merge";
            }

            claimed[candidate] = true;
            if (isHeader[block])
            {
                headerSelectionTargets[block] = candidate;
            }
            else
            {
                mergeTargets[block] = candidate;
            }
        }

        return string.Empty;
    }

    // A range-recursive emitter walks blocks in layout order and needs the
    // layout to agree with the structure: a loop has to occupy a contiguous
    // run of blocks ending at its latch, and a conditional branch has to be a
    // forward branch whose target is where its arms rejoin. Neither is
    // guaranteed by reducibility, and both are cheap to check. Reordering
    // blocks to make them true is possible and is not attempted here - the
    // point is to know whether it is needed.
    private static string DescribeLayoutRejection(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        IReadOnlyList<Loop> loops,
        IReadOnlyList<int> postDominators)
    {
        foreach (var loop in loops)
        {
            var latch = loop.Latches[0];
            if (loop.Body.Any(block => block < loop.Header || block > latch))
            {
                return $"loop{loop.Header}:body-outside-range";
            }

            if (loop.Body.Count != latch - loop.Header + 1)
            {
                return $"loop{loop.Header}:body-not-contiguous";
            }

            // The emitter names the loop's merge block by label, and the
            // label it has is the block after the latch. A loop whose only
            // exit is somewhere else would leave that block unreachable,
            // which cannot happen in a layout a compiler produced, but is
            // worth failing on rather than assuming.
            if (loop.ExitTargets.Count == 0)
            {
                return $"loop{loop.Header}:no-exit";
            }

            foreach (var exit in loop.ExitTargets)
            {
                if (exit != latch + 1)
                {
                    return $"loop{loop.Header}:exit{exit}!=latch{latch}+1";
                }
            }

            // A latch carries the back edge, so it needs no merge instruction
            // of its own only while its two targets are the header it returns
            // to and the merge it leaves by. Anything else is a selection
            // that would need one.
            if (successors[latch].Count > 1 &&
                (!successors[latch].Contains(loop.Header) ||
                 !successors[latch].Contains(latch + 1)))
            {
                return $"loop{loop.Header}:latch-targets";
            }
        }

        for (var block = 0; block < blockCount; block++)
        {
            if (successors[block].Count < 2)
            {
                continue;
            }

            if (successors[block].Count > 2)
            {
                return $"block{block}:multiway";
            }

            var isLatch = loops.Any(loop => loop.Latches[0] == block);
            if (isLatch)
            {
                continue;
            }

            var fallthrough = block + 1;
            if (!successors[block].Contains(fallthrough))
            {
                return $"block{block}:no-fallthrough";
            }

            // A conditional branch whose target is the block it would fall
            // into has one edge and two arms. BuildSuccessors keeps the edge
            // set a set, the way KytyPS5 does with AddUnique in ShaderCFG.cpp,
            // and Analyze is told which blocks end conditionally separately -
            // so a count of arms is never read off a count of edges and this
            // is unreachable. It stays as a guard, because when it was not a
            // guard it was LINQ's "Sequence contains no matching element" and
            // it cost twelve dispatches of 0x555F4F500 in every run.
            var takenTargets = successors[block]
                .Where(target => target != fallthrough)
                .ToList();
            if (takenTargets.Count == 0)
            {
                throw new InvalidOperationException(
                    $"block{block} branches to {fallthrough} either way");
            }

            var taken = takenTargets[0];
            if (taken <= block)
            {
                return $"block{block}:backward-branch";
            }

            // Two layouts are both fine and both common. An if-then falls
            // through into the arm and branches over it, so the taken target
            // is the join. An if-then-else falls through into the first arm
            // and branches to the second, so the taken target opens the else
            // and the join comes after it. A join before the taken target
            // means the arms do not nest, and a join of -1 means both arms
            // end the program, which needs no merge block to be reached.
            var join = postDominators[block];
            if (join >= 0 && join < taken)
            {
                return $"block{block}:join{join}<target{taken}";
            }
        }

        return string.Empty;
    }

    // The last thing that can be wrong once merges are assigned is nesting.
    // SPIR-V constructs have to nest, and a branch may leave one only by its
    // merge, by an enclosing merge, or back to an enclosing loop's header.
    // A driver handed a module that breaks this does not have to say so
    // politely - the one here crashed - so it is checked before emission
    // rather than discovered afterwards.
    private static string DescribeNestingRejection(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        IReadOnlyList<Loop> loops,
        IReadOnlyList<int> mergeTargets)
    {
        var headers = new List<(int Start, int End, bool IsLoop)>();
        var loopHeaders = new HashSet<int>(
            loops.Select(static loop => loop.Header));
        for (var block = 0; block < blockCount; block++)
        {
            if (mergeTargets[block] >= 0)
            {
                headers.Add(
                    (block, mergeTargets[block], loopHeaders.Contains(block)));
            }
        }

        foreach (var outer in headers)
        {
            foreach (var inner in headers)
            {
                if (outer.Start >= inner.Start)
                {
                    continue;
                }

                var crosses =
                    inner.Start < outer.End && inner.End > outer.End;
                if (crosses)
                {
                    return
                        $"construct{inner.Start}-{inner.End}:crosses" +
                        $"{outer.Start}-{outer.End}";
                }
            }
        }

        for (var block = 0; block < blockCount; block++)
        {
            var enclosing = headers
                .Where(header =>
                    block >= header.Start && block < header.End)
                .ToList();
            foreach (var target in successors[block])
            {
                if (enclosing.Count == 0)
                {
                    continue;
                }

                var innermost = enclosing
                    .OrderByDescending(static header => header.Start)
                    .First();
                var inside =
                    target > innermost.Start && target < innermost.End;
                var leavesByMerge = enclosing.Any(
                    header => header.End == target);
                var continues = enclosing.Any(
                    header => header.IsLoop && header.Start == target);
                if (!inside && !leavesByMerge && !continues)
                {
                    // Which constructs the block sits in, and where each of
                    // them lets a branch out, is the whole of what has to be
                    // known to say why the target is not one of those exits.
                    // Without it the reason names two blocks and nothing
                    // that could be done about them.
                    var chain = string.Join(
                        "/",
                        enclosing
                            .OrderBy(static header => header.Start)
                            .Select(static header =>
                                $"{(header.IsLoop ? "L" : "S")}" +
                                $"{header.Start}-{header.End}"));
                    var targetHeader = headers.FirstOrDefault(
                        header => header.Start == target);
                    var targetMergeOf = headers
                        .Where(header => header.End == target)
                        .Select(static header => header.Start)
                        .ToList();
                    return $"block{block}:escapes-to-{target}" +
                        $":in={chain}" +
                        $":targetheader=" +
                        $"{(targetHeader.End == 0 ? "-" : $"{targetHeader.Start}-{targetHeader.End}")}" +
                        $":targetmergeof=" +
                        $"{(targetMergeOf.Count == 0 ? "-" : string.Join(",", targetMergeOf))}";
                }
            }
        }

        return string.Empty;
    }

    // Post-dominators are the same fixed point run on the reversed graph, with
    // one virtual exit added so that a shader with several SEndpgm blocks
    // still has a single root to root the tree at.
    private static int[] ComputePostDominators(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors)
    {
        var exit = blockCount;
        var reversed = new List<int>[blockCount + 1];
        for (var block = 0; block <= blockCount; block++)
        {
            reversed[block] = [];
        }

        for (var block = 0; block < blockCount; block++)
        {
            if (successors[block].Count == 0)
            {
                reversed[exit].Add(block);
                continue;
            }

            foreach (var target in successors[block])
            {
                reversed[target].Add(block);
            }
        }

        var order = ComputeReversePostorderFrom(
            blockCount + 1, reversed, exit);
        var immediate = ComputeDominatorsFrom(
            blockCount + 1, reversed, order, exit);
        var result = new int[blockCount];
        for (var block = 0; block < blockCount; block++)
        {
            var candidate = immediate[block];
            result[block] = candidate == exit ? -1 : candidate;
        }

        return result;
    }

    public string Describe()
    {
        var loops = string.Join(
            ",",
            Loops.Select(static loop =>
                $"{loop.Header}:latch{loop.Latches.Count}" +
                $":exit{loop.ExitTargets.Count}"));
        return
            $"blocks={BlockCount} edges={EdgeCount} loops={Loops.Count} " +
            $"depth={MaxDepth} reducible={(Reducible ? 1 : 0)} " +
            $"structurable={(Structurable ? 1 : 0)} " +
            $"emittable={(Emittable ? 1 : 0)}" +
            (LayoutRejection.Length == 0
                ? string.Empty
                : $" rejected={LayoutRejection}") +
            (loops.Length == 0 ? string.Empty : $" shape={loops}");
    }

    private static HashSet<int> CollectLoopBody(
        int header,
        IReadOnlyList<int> latches,
        IReadOnlyList<List<int>> predecessors)
    {
        // Walking predecessors from each latch, stopping at the header,
        // is the standard construction and needs no dominator queries.
        var body = new HashSet<int> { header };
        var work = new Stack<int>();
        foreach (var latch in latches)
        {
            if (body.Add(latch))
            {
                work.Push(latch);
            }
        }

        while (work.Count > 0)
        {
            var block = work.Pop();
            foreach (var predecessor in predecessors[block])
            {
                if (body.Add(predecessor))
                {
                    work.Push(predecessor);
                }
            }
        }

        return body;
    }

    private static List<List<int>> BuildPredecessors(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors)
    {
        var predecessors = new List<List<int>>(blockCount);
        for (var block = 0; block < blockCount; block++)
        {
            predecessors.Add([]);
        }

        for (var block = 0; block < blockCount; block++)
        {
            foreach (var target in successors[block])
            {
                if (target >= 0 && target < blockCount)
                {
                    predecessors[target].Add(block);
                }
            }
        }

        return predecessors;
    }

    private static int[] ComputeReversePostorder(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors) =>
        ComputeReversePostorderFrom(blockCount, successors, 0);

    private static int[] ComputeReversePostorderFrom(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        int root)
    {
        var postorder = new List<int>(blockCount);
        var visited = new bool[blockCount];
        var stack = new Stack<(int Block, int Next)>();
        stack.Push((root, 0));
        visited[root] = true;
        while (stack.Count > 0)
        {
            var (block, next) = stack.Pop();
            if (next >= successors[block].Count)
            {
                postorder.Add(block);
                continue;
            }

            stack.Push((block, next + 1));
            var target = successors[block][next];
            if (target >= 0 && target < blockCount && !visited[target])
            {
                visited[target] = true;
                stack.Push((target, 0));
            }
        }

        postorder.Reverse();
        return [.. postorder];
    }

    private static int[] ComputeDominators(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        IReadOnlyList<int> reversePostorder) =>
        ComputeDominatorsFrom(blockCount, successors, reversePostorder, 0);

    private static int[] ComputeDominatorsFrom(
        int blockCount,
        IReadOnlyList<IReadOnlyList<int>> successors,
        IReadOnlyList<int> reversePostorder,
        int root)
    {
        // Cooper, Harvey and Kennedy: iterate to a fixed point in reverse
        // postorder, keeping only the immediate dominator per block. Cheap to
        // write and fast enough for graphs this size.
        var index = new int[blockCount];
        Array.Fill(index, -1);
        for (var position = 0; position < reversePostorder.Count; position++)
        {
            index[reversePostorder[position]] = position;
        }

        var predecessors = BuildPredecessors(blockCount, successors);
        var immediate = new int[blockCount];
        Array.Fill(immediate, -1);
        immediate[root] = root;

        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var block in reversePostorder)
            {
                if (block == root)
                {
                    continue;
                }

                var candidate = -1;
                foreach (var predecessor in predecessors[block])
                {
                    if (index[predecessor] < 0 || immediate[predecessor] < 0)
                    {
                        continue;
                    }

                    candidate = candidate < 0
                        ? predecessor
                        : Intersect(immediate, index, predecessor, candidate);
                }

                if (candidate >= 0 && immediate[block] != candidate)
                {
                    immediate[block] = candidate;
                    changed = true;
                }
            }
        }

        return immediate;
    }

    private static int Intersect(
        IReadOnlyList<int> immediate,
        IReadOnlyList<int> index,
        int left,
        int right)
    {
        while (left != right)
        {
            while (index[left] > index[right])
            {
                left = immediate[left];
            }

            while (index[right] > index[left])
            {
                right = immediate[right];
            }
        }

        return left;
    }

    private static bool Dominates(
        IReadOnlyList<int> immediate, int dominator, int block)
    {
        var current = block;
        while (current >= 0)
        {
            if (current == dominator)
            {
                return true;
            }

            var next = immediate[current];
            if (next == current)
            {
                return false;
            }

            current = next;
        }

        return false;
    }
}
