// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Runtime.InteropServices;
using Ps5Recomp.ShaderCompiler;

namespace Ps5GpuBridge;

internal sealed unsafe class DirectGuestShaderMemory : IGuestShaderMemory
{
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint PageNoAccess = 0x01;
    private const uint PageGuard = 0x100;

    // VirtualQuery is a system call, and most of the reads it guards are four
    // bytes: TryReadUInt32 walks descriptors and constant tables one dword at
    // a time. Sampling the opening frame put a whole bridge thread at forty
    // percent inside NtQueryVirtualMemory for that reason alone.
    //
    // The regions being asked about are the guest's own allocations, made once
    // by the runner and not moved afterwards, so remembering the ones that
    // answered yes removes almost every call. A region decommitted later would
    // be answered from the cache and read anyway - but the copy already
    // happens after the query rather than under it, so that race is there
    // without the cache and the cache only widens it. Invalidate exists for a
    // caller that knows the map has changed.
    private const int CachedRegionCount = 64;
    private readonly (ulong Start, ulong End)[] _readable =
        new (ulong Start, ulong End)[CachedRegionCount];
    private int _cached;
    private int _next;

    public bool TryRead(ulong virtualAddress, Span<byte> destination)
    {
        if (destination.IsEmpty)
        {
            return true;
        }
        if (IsReadable(virtualAddress, checked((ulong)destination.Length)))
        {
            fixed (byte* target = destination)
            {
                Buffer.MemoryCopy(
                    (void*)virtualAddress,
                    target,
                    destination.Length,
                    destination.Length);
            }
            return true;
        }

        return TryReadAcrossReservedPages(virtualAddress, destination);
    }

    // Guest direct memory is reserved and committed as the title touches it,
    // so a range can be part committed and part reserved - the display buffer
    // is, because nothing has written its far half yet. A page that was never
    // committed has never been written either, so it reads as zero; refusing
    // the whole read instead loses the committed part as well. That mattered:
    // the compute shader that writes the frame had its buffer read refused,
    // which left it a zero buffer and disabled its write-back, and the
    // display buffer stayed black.
    //
    // No caching here. The reserved half of a range is exactly the part that
    // becomes committed later, and remembering the answer would keep
    // returning zeros for memory that has since been written.
    private bool TryReadAcrossReservedPages(
        ulong virtualAddress,
        Span<byte> destination)
    {
        var length = checked((ulong)destination.Length);
        if (virtualAddress == 0 || virtualAddress > ulong.MaxValue - length)
        {
            return false;
        }

        var end = virtualAddress + length;
        var current = virtualAddress;
        var covered = false;
        while (current < end)
        {
            if (VirtualQuery(
                    (void*)current,
                    out var information,
                    (nuint)sizeof(MemoryBasicInformation)) == 0)
            {
                return false;
            }

            var regionBase = checked((ulong)information.BaseAddress);
            var regionEnd = regionBase + information.RegionSize;
            if (regionEnd <= current)
            {
                return false;
            }

            var sliceEnd = Math.Min(end, regionEnd);
            var offset = checked((int)(current - virtualAddress));
            var count = checked((int)(sliceEnd - current));
            var slice = destination.Slice(offset, count);
            if (information.State == MemCommit &&
                (information.Protect & (PageNoAccess | PageGuard)) == 0)
            {
                fixed (byte* target = slice)
                {
                    Buffer.MemoryCopy(
                        (void*)current,
                        target,
                        count,
                        count);
                }
                covered = true;
            }
            else if (information.State == MemReserve)
            {
                slice.Clear();
            }
            else
            {
                return false;
            }

            current = sliceEnd;
        }

        // A range that is entirely reserved still belongs to a mapping the
        // runner made - VirtualQuery would have said MEM_FREE otherwise, and
        // that is refused above. Nothing has been written to it, so zeroes
        // are its contents, not a guess. Refusing it instead was still
        // costing the second display buffer its write-back.
        _ = covered;
        return true;
    }

    /// <summary>Forgets every remembered region.</summary>
    public void Invalidate()
    {
        lock (_readable)
        {
            _cached = 0;
            _next = 0;
        }
    }

    private bool IsReadable(ulong address, ulong length)
    {
        if (address == 0 || length == 0 || address > ulong.MaxValue - length)
        {
            return false;
        }

        var end = address + length;
        if (IsRemembered(address, end))
        {
            return true;
        }

        var current = address;
        while (current < end)
        {
            if (VirtualQuery(
                    (void*)current,
                    out var information,
                    (nuint)sizeof(MemoryBasicInformation)) == 0 ||
                information.State != MemCommit ||
                (information.Protect & (PageNoAccess | PageGuard)) != 0)
            {
                return false;
            }

            var regionBase = checked((ulong)information.BaseAddress);
            var regionEnd = regionBase + information.RegionSize;
            if (regionEnd <= current)
            {
                return false;
            }

            Remember(regionBase, regionEnd);
            current = Math.Min(end, regionEnd);
        }
        return true;
    }

    private bool IsRemembered(ulong start, ulong end)
    {
        lock (_readable)
        {
            for (var index = 0; index < _cached; index++)
            {
                var region = _readable[index];
                if (start >= region.Start && end <= region.End)
                {
                    return true;
                }
            }
        }

        return false;
    }

    private void Remember(ulong start, ulong end)
    {
        if (end <= start)
        {
            return;
        }

        lock (_readable)
        {
            for (var index = 0; index < _cached; index++)
            {
                if (_readable[index].Start == start &&
                    _readable[index].End == end)
                {
                    return;
                }
            }

            if (_cached < CachedRegionCount)
            {
                _readable[_cached++] = (start, end);
                return;
            }

            // Round-robin rather than least-recently-used: the working set is
            // a handful of large allocations, and a wrong eviction costs one
            // system call, not a wrong answer.
            _readable[_next] = (start, end);
            _next = (_next + 1) % CachedRegionCount;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryBasicInformation
    {
        public nuint BaseAddress;
        public nuint AllocationBase;
        public uint AllocationProtect;
        public ushort PartitionId;
        public nuint RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

    [DllImport("kernel32.dll")]
    private static extern nuint VirtualQuery(
        void* address,
        out MemoryBasicInformation information,
        nuint length);
}
