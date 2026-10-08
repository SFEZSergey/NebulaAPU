// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

using System.Buffers.Binary;

namespace Ps5Recomp.ShaderCompiler;

public interface IGuestShaderMemory
{
    bool TryRead(ulong virtualAddress, Span<byte> destination);
}

public sealed class GuestShaderContext(IGuestShaderMemory memory)
{
    public IGuestShaderMemory Memory { get; } =
        memory ?? throw new ArgumentNullException(nameof(memory));

    public bool TryReadUInt32(ulong address, out uint value)
    {
        Span<byte> bytes = stackalloc byte[sizeof(uint)];
        if (!Memory.TryRead(address, bytes))
        {
            value = 0;
            return false;
        }

        value = BinaryPrimitives.ReadUInt32LittleEndian(bytes);
        return true;
    }
}
