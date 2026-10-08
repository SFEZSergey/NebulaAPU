// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Collections.Concurrent;
using System.Diagnostics;

namespace Ps5HleBridge;

internal static class HleCallTrace
{
    private const int DefaultStallMilliseconds = 500;
    private const int ReportIntervalMilliseconds = 15_000;
    private const int ReportedExportCount = 20;
    private static readonly bool Enabled = string.Equals(
        Environment.GetEnvironmentVariable("PS5RECOMP_TRACE_HLE"),
        "1",
        StringComparison.Ordinal);
    private static readonly int StallMilliseconds = ReadBoundedInteger(
        "PS5RECOMP_HLE_STALL_MS",
        DefaultStallMilliseconds,
        50,
        60_000);
    private static readonly ConcurrentDictionary<long, ActiveCall> Active = new();
    private static readonly ConcurrentDictionary<(string Nid, ulong Result), int>
        ErrorCounts = new();
    private static readonly ConcurrentDictionary<string, ExportTotals> Totals = new();
    private static readonly long TraceStartTimestamp = Stopwatch.GetTimestamp();
    private static readonly Timer? Watchdog = Enabled
        ? new Timer(
            ScanForStalledCalls,
            null,
            StallMilliseconds,
            Math.Max(100, StallMilliseconds / 2))
        : null;
    private static readonly Timer? Reporter = Enabled
        ? new Timer(
            ReportTotals,
            null,
            ReportIntervalMilliseconds,
            ReportIntervalMilliseconds)
        : null;
    private static long _nextSequence;

    public static long Begin(
        string nid,
        string name,
        string library,
        ulong rip)
    {
        if (!Enabled)
        {
            return 0;
        }

        var sequence = Interlocked.Increment(ref _nextSequence);
        Active[sequence] = new ActiveCall(
            sequence,
            Environment.CurrentManagedThreadId,
            nid,
            name,
            library,
            rip,
            Stopwatch.GetTimestamp());
        return sequence;
    }

    public static void Complete(
        long sequence,
        ulong result,
        bool dispatchSucceeded = true,
        string? fault = null)
    {
        if (sequence == 0 || !Active.TryRemove(sequence, out var active))
        {
            return;
        }

        var elapsed = Stopwatch.GetElapsedTime(active.StartTimestamp);
        Totals
            .GetOrAdd(active.Name, static _ => new ExportTotals())
            .Add(elapsed.Ticks);
        if (elapsed.TotalMilliseconds >= StallMilliseconds)
        {
            Console.Error.WriteLine(
                $"[HLESLOW] seq={sequence} thread={active.ThreadId} " +
                $"name={active.Name} nid={active.Nid} library={active.Library} " +
                $"elapsed_ms={elapsed.TotalMilliseconds:0.###} result=0x{result:X16}");
        }

        if (dispatchSucceeded && !IsErrorResult(result))
        {
            return;
        }

        var count = ErrorCounts.AddOrUpdate(
            (active.Nid, result),
            1,
            static (_, current) => current + 1);
        if (count <= 8 || IsPowerOfTwo(count))
        {
            Console.Error.WriteLine(
                $"[HLEERR] count={count} seq={sequence} thread={active.ThreadId} " +
                $"name={active.Name} nid={active.Nid} library={active.Library} " +
                $"elapsed_ms={elapsed.TotalMilliseconds:0.###} result=0x{result:X16}" +
                (string.IsNullOrEmpty(fault) ? string.Empty : $" fault={fault}"));
        }
    }

    private static void ReportTotals(object? state)
    {
        _ = state;
        var snapshot = Totals.ToArray();
        if (snapshot.Length == 0)
        {
            return;
        }

        var uptime = Stopwatch.GetElapsedTime(TraceStartTimestamp).TotalSeconds;
        var ranked = snapshot
            .Select(static entry => (entry.Key, Sample: entry.Value.Snapshot()))
            .OrderByDescending(static entry => entry.Sample.TotalTicks)
            .Take(ReportedExportCount);
        var index = 0;
        foreach (var (name, sample) in ranked)
        {
            var total = TimeSpan.FromTicks(sample.TotalTicks).TotalMilliseconds;
            var longest = TimeSpan.FromTicks(sample.LongestTicks).TotalMilliseconds;
            Console.Error.WriteLine(
                $"[HLETOTAL] uptime_s={uptime:0.###} rank={++index} name={name} " +
                $"calls={sample.Calls} total_ms={total:0.###} " +
                $"mean_ms={total / Math.Max(1, sample.Calls):0.###} " +
                $"longest_ms={longest:0.###}");
        }

        // Ranked by count as well as by time. An export that is being
        // spin-polled is cheap per call and never reaches the time
        // ranking, yet it is exactly what would let per-call overhead
        // change how the run behaves.
        var busiest = snapshot
            .Select(static entry => (entry.Key, Sample: entry.Value.Snapshot()))
            .OrderByDescending(static entry => entry.Sample.Calls)
            .Take(ReportedExportCount);
        long totalCalls = 0;
        foreach (var entry in snapshot)
        {
            totalCalls += entry.Value.Snapshot().Calls;
        }

        index = 0;
        foreach (var (name, sample) in busiest)
        {
            Console.Error.WriteLine(
                $"[HLECALLS] uptime_s={uptime:0.###} rank={++index} " +
                $"name={name} calls={sample.Calls} " +
                $"per_s={sample.Calls / Math.Max(0.001, uptime):0.#}");
        }

        Console.Error.WriteLine(
            $"[HLECALLS] uptime_s={uptime:0.###} total_calls={totalCalls} " +
            $"exports={snapshot.Length} " +
            $"per_s={totalCalls / Math.Max(0.001, uptime):0.#}");
    }

    private sealed class ExportTotals
    {
        private long _calls;
        private long _totalTicks;
        private long _longestTicks;

        public void Add(long ticks)
        {
            Interlocked.Increment(ref _calls);
            Interlocked.Add(ref _totalTicks, ticks);
            var longest = Interlocked.Read(ref _longestTicks);
            while (ticks > longest)
            {
                var seen = Interlocked.CompareExchange(
                    ref _longestTicks,
                    ticks,
                    longest);
                if (seen == longest)
                {
                    break;
                }

                longest = seen;
            }
        }

        public (long Calls, long TotalTicks, long LongestTicks) Snapshot() =>
            (Interlocked.Read(ref _calls),
                Interlocked.Read(ref _totalTicks),
                Interlocked.Read(ref _longestTicks));
    }

    private static void ScanForStalledCalls(object? state)
    {
        _ = state;
        var now = Stopwatch.GetTimestamp();
        foreach (var call in Active.Values)
        {
            var elapsed = Stopwatch.GetElapsedTime(call.StartTimestamp, now);
            if (elapsed.TotalMilliseconds < StallMilliseconds ||
                Interlocked.Exchange(ref call.StallReported, 1) != 0)
            {
                continue;
            }

            Console.Error.WriteLine(
                $"[HLESTALL] seq={call.Sequence} thread={call.ThreadId} " +
                $"name={call.Name} nid={call.Nid} library={call.Library} " +
                $"rip=0x{call.Rip:X16} elapsed_ms={elapsed.TotalMilliseconds:0.###}");
        }
    }

    private static bool IsErrorResult(ulong result)
    {
        var low = unchecked((uint)result);
        var high = result >> 32;
        return (low & 0xF000_0000u) == 0x8000_0000u &&
            (high == 0 || high == uint.MaxValue);
    }

    private static bool IsPowerOfTwo(int value) =>
        value > 0 && (value & (value - 1)) == 0;

    private static int ReadBoundedInteger(
        string variable,
        int fallback,
        int minimum,
        int maximum)
    {
        return int.TryParse(
                Environment.GetEnvironmentVariable(variable),
                out var value)
            ? Math.Clamp(value, minimum, maximum)
            : fallback;
    }

    private sealed class ActiveCall(
        long sequence,
        int threadId,
        string nid,
        string name,
        string library,
        ulong rip,
        long startTimestamp)
    {
        public long Sequence { get; } = sequence;
        public int ThreadId { get; } = threadId;
        public string Nid { get; } = nid;
        public string Name { get; } = name;
        public string Library { get; } = library;
        public ulong Rip { get; } = rip;
        public long StartTimestamp { get; } = startTimestamp;
        public int StallReported;
    }
}
