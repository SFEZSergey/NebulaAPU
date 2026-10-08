// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Buffers.Binary;

namespace Ps5Recomp.Cli;

internal static class MinimalPs5Elf
{
    public const ulong ExpectedReturnValue = 42;
    private const ulong BootstrapHleAddress = 0x00000007FFF00000;

    public static byte[] Create()
    {
        const int elfHeaderSize = 64;
        const int programHeaderSize = 56;
        const int codeOffset = 0x1000;
        byte[] code =
        [
            0x48, 0xB8,                   // mov rax, BootstrapHleAddress
            0x00, 0x00, 0xF0, 0xFF,
            0x07, 0x00, 0x00, 0x00,
            0xFF, 0xD0,                   // call rax
            0x83, 0xC0, 0x23,             // add eax, 35
            0xC3,                         // ret (HLE returns 7, total is 42)
        ];
        if (BinaryPrimitives.ReadUInt64LittleEndian(code.AsSpan(2)) !=
            BootstrapHleAddress)
        {
            throw new InvalidOperationException("Bootstrap HLE fixture address is invalid.");
        }
        var image = new byte[codeOffset + code.Length];
        var header = image.AsSpan(0, elfHeaderSize);
        header[0] = 0x7F;
        header[1] = (byte)'E';
        header[2] = (byte)'L';
        header[3] = (byte)'F';
        header[4] = 2; // ELFCLASS64
        header[5] = 1; // little endian
        header[6] = 1; // current version
        header[8] = 2; // PS5 ABI generation
        BinaryPrimitives.WriteUInt16LittleEndian(header[16..], 3); // ET_DYN
        BinaryPrimitives.WriteUInt16LittleEndian(header[18..], 0x3E); // x86-64
        BinaryPrimitives.WriteUInt32LittleEndian(header[20..], 1);
        BinaryPrimitives.WriteUInt64LittleEndian(header[24..], 0x1000);
        BinaryPrimitives.WriteUInt64LittleEndian(header[32..], elfHeaderSize);
        BinaryPrimitives.WriteUInt16LittleEndian(header[52..], elfHeaderSize);
        BinaryPrimitives.WriteUInt16LittleEndian(header[54..], programHeaderSize);
        BinaryPrimitives.WriteUInt16LittleEndian(header[56..], 1);

        var programHeader = image.AsSpan(elfHeaderSize, programHeaderSize);
        BinaryPrimitives.WriteUInt32LittleEndian(programHeader, 1); // PT_LOAD
        BinaryPrimitives.WriteUInt32LittleEndian(programHeader[4..], 5); // R-X
        BinaryPrimitives.WriteUInt64LittleEndian(programHeader[8..], codeOffset);
        BinaryPrimitives.WriteUInt64LittleEndian(programHeader[16..], 0x1000);
        BinaryPrimitives.WriteUInt64LittleEndian(programHeader[32..], (ulong)code.Length);
        BinaryPrimitives.WriteUInt64LittleEndian(programHeader[40..], (ulong)code.Length);
        BinaryPrimitives.WriteUInt64LittleEndian(programHeader[48..], 0x1000);
        code.CopyTo(image.AsSpan(codeOffset));
        return image;
    }
}
