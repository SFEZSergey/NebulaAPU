// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Runtime.InteropServices;
using SharpEmu.HLE;

namespace Ps5HleBridge;

internal sealed unsafe class DirectGuestMemory : ICpuMemory
{
    private const uint MemCommit = 0x1000;
    private const uint PageNoAccess = 0x01;
    private const uint PageGuard = 0x100;
    private const uint WritableMask =
        0x04 | 0x08 | 0x40 | 0x80;
    [ThreadStatic]
    private static int _readBatchDepth;
    [ThreadStatic]
    private static ulong _cachedReadRegionStart;
    [ThreadStatic]
    private static ulong _cachedReadRegionEnd;

    public void BeginReadBatch()
    {
        if (_readBatchDepth++ == 0)
        {
            ClearReadRegionCache();
        }
    }

    public void EndReadBatch()
    {
        if (_readBatchDepth <= 0)
        {
            _readBatchDepth = 0;
            ClearReadRegionCache();
            return;
        }

        if (--_readBatchDepth == 0)
        {
            ClearReadRegionCache();
        }
    }

    public bool TryRead(ulong virtualAddress, Span<byte> destination)
    {
        if (destination.IsEmpty)
        {
            return true;
        }
        if (!IsAccessible(
                virtualAddress,
                checked((ulong)destination.Length),
                writeAccess: false))
        {
            return false;
        }

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

    public bool TryWrite(ulong virtualAddress, ReadOnlySpan<byte> source)
    {
        if (source.IsEmpty)
        {
            return true;
        }
        if (!IsAccessible(
                virtualAddress,
                checked((ulong)source.Length),
                writeAccess: true))
        {
            return false;
        }

        fixed (byte* sourcePointer = source)
        {
            Buffer.MemoryCopy(
                sourcePointer,
                (void*)virtualAddress,
                source.Length,
                source.Length);
        }
        return true;
    }

    public bool TryCompare(
        ulong virtualAddress,
        ReadOnlySpan<byte> expected)
    {
        if (expected.IsEmpty)
        {
            return true;
        }
        if (!IsAccessible(
                virtualAddress,
                checked((ulong)expected.Length),
                writeAccess: false))
        {
            return false;
        }
        return new ReadOnlySpan<byte>(
            (void*)virtualAddress,
            expected.Length).SequenceEqual(expected);
    }

    private static bool IsAccessible(
        ulong address,
        ulong length,
        bool writeAccess)
    {
        if (address == 0 || length == 0 ||
            address > ulong.MaxValue - length)
        {
            return false;
        }

        var end = address + length;
        var current = address;
        while (current < end)
        {
            if (!writeAccess &&
                _readBatchDepth > 0 &&
                current >= _cachedReadRegionStart &&
                current < _cachedReadRegionEnd)
            {
                current = Math.Min(end, _cachedReadRegionEnd);
                continue;
            }

            if (VirtualQuery(
                    (void*)current,
                    out var information,
                    (nuint)sizeof(MemoryBasicInformation)) == 0 ||
                information.State != MemCommit ||
                (information.Protect & (PageNoAccess | PageGuard)) != 0 ||
                (writeAccess &&
                 (information.Protect & WritableMask) == 0))
            {
                return false;
            }

            var regionBase = checked((ulong)information.BaseAddress);
            var regionEnd = regionBase + information.RegionSize;
            if (regionEnd <= current)
            {
                return false;
            }
            if (!writeAccess && _readBatchDepth > 0)
            {
                _cachedReadRegionStart = regionBase;
                _cachedReadRegionEnd = regionEnd;
            }
            current = Math.Min(end, regionEnd);
        }
        return true;
    }

    private static void ClearReadRegionCache()
    {
        _cachedReadRegionStart = 0;
        _cachedReadRegionEnd = 0;
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
