// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Buffers.Binary;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using Ps5Recomp.ShaderCompiler;
using Ps5Recomp.ShaderCompiler.Vulkan;

namespace Ps5GpuBridge;

public enum Ps5GpuShaderStage : uint
{
    Vertex = 0,
    Pixel = 1,
    Compute = 2,
}

public enum Ps5GpuResult : int
{
    Ok = 0,
    InvalidArgument = 1,
    StateDecodeFailed = 2,
    EvaluationFailed = 3,
    SpirvCompilationFailed = 4,
    OutOfMemory = 5,
    InternalError = 6,
}

[StructLayout(LayoutKind.Sequential)]
public struct Ps5GpuRegisterValue
{
    public uint Register;
    public uint Value;
}

[StructLayout(LayoutKind.Sequential)]
public struct Ps5GpuPixelOutput
{
    public uint GuestSlot;
    public uint HostLocation;
    public uint Kind;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct Ps5GpuShaderRequest
{
    public uint StructSize;
    public uint AbiVersion;
    public Ps5GpuShaderStage Stage;
    public uint Flags;
    public ulong ShaderAddress;
    public ulong ShaderHeaderAddress;
    public Ps5GpuRegisterValue* Registers;
    public uint RegisterCount;
    public uint UserDataBaseRegister;
    public uint UserDataScalarRegisterBase;
    public uint LocalSizeX;
    public uint LocalSizeY;
    public uint LocalSizeZ;
    public uint WaveLaneCount;
    public ulong StorageBufferOffsetAlignment;
    public Ps5GpuPixelOutput* PixelOutputs;
    public uint PixelOutputCount;
    public uint PixelInputEnable;
    public uint PixelInputAddress;
    public int GlobalBufferBase;
    public int TotalGlobalBufferCount;
    public int ImageBindingBase;
    public int InitialScalarBufferIndex;
    public int RequiredVertexOutputCount;
    public int ComputeWorkGroupXRegister;
    public int ComputeWorkGroupYRegister;
    public int ComputeWorkGroupZRegister;
    public int ComputeThreadGroupSizeRegister;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct Ps5GpuShaderResult
{
    public uint StructSize;
    public Ps5GpuResult Status;
    public byte* Spirv;
    public uint SpirvSize;
    public uint AttributeCount;
    public uint GlobalMemoryBindingCount;
    public uint ImageBindingCount;
    public uint VertexInputCount;
    public byte* ResourceManifest;
    public uint ResourceManifestSize;
}

internal static unsafe class GpuBridgeExports
{
    private const uint AbiVersion = 0x00010001;
    private const uint ResolveVertexInputs = 1u << 0;
    private const uint MaximumRegisterCount = 0x10000;
    private const uint MaximumPixelOutputCount = 8;
    private const uint ResourceManifestMagic = 0x4D524750;
    private const uint ResourceManifestVersion = 1;
    private const uint ResourceBufferWritable = 1u << 0;
    private const uint ResourceBufferWriteBack = 1u << 1;
    private const uint ResourceImageStorage = 1u << 0;
    private const uint ResourceImageHasSampler = 1u << 1;
    private const uint ResourceImageValidExtent = 1u << 2;
    private const int ResourceManifestHeaderSize = 64;
    private const int ScalarRegisterCount = 256;
    private const int ResourceGlobalSize = 32;
    private const int ResourceImageSize = 80;
    private const int ResourceVertexInputSize = 48;
    private const uint ShaderCacheMagic = 0x43534750;
    private const uint ShaderCacheVersion = 2;
    private const int ShaderCacheHeaderSize = 36;
    // Fifty-six milliseconds per compute translation is the largest single
    // cost on the thread that owes the frame, and the phases behind it want
    // different fixes: decoding the shader state and resolving its
    // descriptors reads guest memory, generating SPIR-V does not, and the
    // manifest carries whatever the buffers held. Time them apart before
    // choosing which one to cache.
    private static readonly long[] StageTicks = new long[6];
    private static readonly long[] StageCalls = new long[6];
    private static readonly string[] StageNames =
        ["decode", "evaluate", "spirv", "manifest", "store", "key"];
    private static long stageReports;

    private static long StageBegin() => Stopwatch.GetTimestamp();

    private static void StageEnd(int stage, long started)
    {
        StageTicks[stage] += Stopwatch.GetTimestamp() - started;
        StageCalls[stage]++;
    }

    private static void ReportStages()
    {
        const long ReportEvery = 64;
        if (++stageReports % ReportEvery != 0)
        {
            return;
        }
        var text = new StringBuilder(256);
        text.Append("ps5gpu.compile_stages calls=").Append(stageReports);
        for (var stage = 0; stage < StageTicks.Length; stage++)
        {
            if (StageCalls[stage] == 0)
            {
                continue;
            }
            text.Append(' ').Append(StageNames[stage]).Append('=')
                .Append(StageTicks[stage] * 1000 / Stopwatch.Frequency)
                .Append("ms/").Append(StageCalls[stage]);
        }
        Console.Error.WriteLine(text.ToString());
    }

    // Whether the disk cache is worth writing to for a given shader.
    //
    // The key includes every shader register and its value, and the
    // user-data registers among them hold the descriptor pointers the guest
    // rebinds on every dispatch, so for this title the key is unique by
    // construction: 1984 entries stored in one run and not one hit. Writing
    // them costs 5.1 seconds of the 15.5 the guest thread spends
    // translating, on the thread that owes the frame.
    //
    // Narrowing the key is the real fix - KytyPS5 keys a compute program on
    // the shader hash plus thirteen integers describing the dispatch, and
    // never on an address - and it needs the descriptor shape, which means
    // reading it out of the translator. Until then, stop paying for a cache
    // that cannot hit: after enough stores against no hits for one shader,
    // give up on that shader. A cache is optional by nature, so declining
    // to fill it changes nothing but the cost.
    private const int StoresBeforeGivingUp = 8;
    private static readonly Dictionary<ulong, int> ComputeCacheStores = new();
    private static readonly HashSet<ulong> ComputeCacheHopeless = new();

    // How many distinct programs one shader actually produces. If the
    // emitter bakes buffer addresses into the SPIR-V then every dispatch
    // yields a different program and nothing can be reused; if it does not,
    // the count is small and a permutation cache is worth building. This
    // only counts and reports.
    private static readonly Dictionary<ulong, HashSet<uint>> SpirvShapes =
        new();


    private static void NoteSpirvShape(
        ulong key, ulong shaderAddress, byte[] spirv)
    {
        NoteEmitterKey(key, shaderAddress, spirv);
        var hash = 2166136261u;
        foreach (var value in spirv)
        {
            hash = (hash ^ value) * 16777619u;
        }
        lock (SpirvShapes)
        {
            if (!SpirvShapes.TryGetValue(shaderAddress, out var shapes))
            {
                shapes = new HashSet<uint>();
                SpirvShapes[shaderAddress] = shapes;
            }
            if (shapes.Add(hash))
            {
                Console.Error.WriteLine(
                    $"ps5gpu.spirv_shape addr=0x{shaderAddress:X16} " +
                    $"distinct={shapes.Count} bytes={spirv.Length} " +
                    $"hash=0x{hash:X8}");
                DumpSpirvShape(shaderAddress, shapes.Count, hash, spirv);
            }
        }
    }

    private const int ProgramCacheLimit = 4096;
    private static readonly Dictionary<ulong, (byte[] Spirv, uint Attributes)>
        ProgramCache = new();
    private static long programCacheHits;
    private static long programCacheMisses;

    private static bool TryTakeProgram(
        ulong key,
        Ps5GpuShaderRequest* request,
        Gen5ShaderEvaluation evaluation,
        out Gen5SpirvShader shader)
    {
        (byte[] Spirv, uint Attributes) entry;
        lock (ProgramCache)
        {
            if (!ProgramCache.TryGetValue(key, out entry))
            {
                programCacheMisses++;
                shader = default!;
                return false;
            }

            programCacheHits++;
        }

        shader = new Gen5SpirvShader(
            entry.Spirv,
            evaluation.GlobalMemoryBindings,
            evaluation.ImageBindings,
            entry.Attributes,
            request->Stage == Ps5GpuShaderStage.Vertex
                ? evaluation.VertexInputs ?? []
                : []);
        return true;
    }

    private static void KeepProgram(ulong key, Gen5SpirvShader shader)
    {
        lock (ProgramCache)
        {
            if (ProgramCache.Count >= ProgramCacheLimit)
            {
                return;
            }

            ProgramCache[key] = (shader.Spirv, shader.AttributeCount);
        }
    }

    private static readonly Dictionary<ulong, uint> EmitterKeys = new();
    private static long emitterKeyCompiles;
    private static long emitterKeyRepeats;
    private static long emitterKeyConflicts;

    private static void HashU32(ref ulong hash, uint value)
    {
        hash ^= value;
        hash *= 1099511628211ul;
    }

    private static void HashU64(ref ulong hash, ulong value)
    {
        HashU32(ref hash, (uint)value);
        HashU32(ref hash, (uint)(value >> 32));
    }

    private static ulong ComputeEmitterKey(
        Ps5GpuShaderRequest* request,
        Gen5ShaderState state,
        Gen5ShaderEvaluation evaluation) =>
        ComputeEmitterKey(request, state, evaluation, coarse: false);

    private static ulong ComputeEmitterKey(
        Ps5GpuShaderRequest* request,
        Gen5ShaderState state,
        Gen5ShaderEvaluation evaluation,
        bool coarse)
    {
        // Only sound where the runtime scalar block is carrying the
        // registers; without it the values below are emitted as constants.
        coarse = coarse && request->InitialScalarBufferIndex >= 0;
        var hash = 14695981039346656037ul;
        HashU32(ref hash, request->UserDataBaseRegister);
        HashU32(ref hash, request->UserDataScalarRegisterBase);
        HashU32(ref hash, (uint)request->ComputeWorkGroupXRegister);
        HashU32(ref hash, (uint)request->ComputeWorkGroupYRegister);
        HashU32(ref hash, (uint)request->ComputeWorkGroupZRegister);
        HashU32(ref hash, (uint)request->ComputeThreadGroupSizeRegister);
        HashU32(ref hash, (uint)state.Program.Instructions.Count);
        foreach (var instruction in state.Program.Instructions)
        {
            HashU32(ref hash, instruction.Pc);
            foreach (var word in instruction.Words)
            {
                HashU32(ref hash, word);
            }
        }

        HashU32(ref hash, (uint)request->Stage);
        HashU32(ref hash, request->Flags);
        HashU64(ref hash, request->ShaderAddress);
        HashU64(ref hash, request->ShaderHeaderAddress);
        HashU32(ref hash, request->LocalSizeX);
        HashU32(ref hash, request->LocalSizeY);
        HashU32(ref hash, request->LocalSizeZ);
        HashU32(ref hash, request->WaveLaneCount);
        HashU64(ref hash, request->StorageBufferOffsetAlignment);
        HashU32(ref hash, (uint)request->GlobalBufferBase);
        HashU32(ref hash, (uint)request->TotalGlobalBufferCount);
        HashU32(ref hash, (uint)request->ImageBindingBase);
        HashU32(ref hash, (uint)request->InitialScalarBufferIndex);
        HashU32(ref hash, (uint)request->RequiredVertexOutputCount);
        HashU32(ref hash, request->PixelInputEnable);
        HashU32(ref hash, request->PixelInputAddress);
        HashU32(ref hash, request->PixelOutputCount);
        for (var index = 0u; index < request->PixelOutputCount; index++)
        {
            HashU32(ref hash, request->PixelOutputs[index].HostLocation);
            HashU32(ref hash, request->PixelOutputs[index].Kind);
        }

        HashU32(ref hash, (uint)evaluation.InitialScalarRegisters.Count);
        if (!coarse)
        {
            foreach (var value in evaluation.InitialScalarRegisters)
            {
                HashU32(ref hash, value);
            }

            foreach (var value in evaluation.ScalarRegisters)
            {
                HashU32(ref hash, value);
            }
        }

        HashU32(ref hash, (uint)evaluation.ImageBindings.Count);
        foreach (var image in evaluation.ImageBindings)
        {
            HashU32(ref hash, image.Pc);
            HashU32(ref hash, image.MipLevel ?? uint.MaxValue);
            foreach (var word in image.ResourceDescriptor)
            {
                HashU32(ref hash, word);
            }

            foreach (var word in image.SamplerDescriptor)
            {
                HashU32(ref hash, word);
            }
        }

        HashU32(ref hash, (uint)evaluation.GlobalMemoryBindings.Count);
        foreach (var buffer in evaluation.GlobalMemoryBindings)
        {
            HashU32(ref hash, buffer.ScalarAddress);
            if (!coarse)
            {
                HashU64(ref hash, buffer.BaseAddress);
                HashU32(ref hash, (uint)buffer.DataLength);
            }

            HashU32(ref hash, buffer.Writable ? 1u : 0u);
            HashU32(ref hash, buffer.WriteBackToGuest ? 1u : 0u);
            foreach (var pc in buffer.InstructionPcs)
            {
                HashU32(ref hash, pc);
            }
        }

        var vertexInputs = evaluation.VertexInputs;
        HashU32(ref hash, (uint)(vertexInputs?.Count ?? 0));
        if (vertexInputs is not null)
        {
            foreach (var input in vertexInputs)
            {
                HashU32(ref hash, input.Pc);
                HashU32(ref hash, input.Location);
                HashU32(ref hash, input.ComponentCount);
                HashU32(ref hash, input.DataFormat);
                HashU32(ref hash, input.NumberFormat);
                HashU64(ref hash, input.BaseAddress);
                HashU32(ref hash, input.Stride);
                HashU32(ref hash, input.OffsetBytes);
                HashU32(ref hash, (uint)input.DataLength);
            }
        }

        return hash;
    }

    // Does that key decide the program? A repeat that yields the same
    // SPIR-V is a hit the cache would have served; a repeat that yields a
    // different one is a wrong answer, and one is too many.
    private static readonly HashSet<ulong> StaticKeysA = new();
    private static readonly HashSet<ulong> StaticKeysB = new();

    private static void NoteStaticKeys(
        Ps5GpuShaderRequest* request,
        Gen5ShaderState state,
        Gen5ShaderEvaluation evaluation)
    {
        var shape = 14695981039346656037ul;
        HashU32(ref shape, (uint)request->Stage);
        HashU64(ref shape, request->ShaderAddress);
        HashU32(ref shape, request->LocalSizeX);
        HashU32(ref shape, request->LocalSizeY);
        HashU32(ref shape, request->LocalSizeZ);
        HashU32(ref shape, request->WaveLaneCount);
        HashU32(ref shape, (uint)state.Program.Instructions.Count);
        foreach (var instruction in state.Program.Instructions)
        {
            foreach (var word in instruction.Words)
            {
                HashU32(ref shape, word);
            }
        }

        // An image binding decides the sampled type and dimensionality, so
        // its descriptor stays in both keys; a buffer binding decides only
        // which slot a load goes to.
        foreach (var image in evaluation.ImageBindings)
        {
            HashU32(ref shape, image.Pc);
            foreach (var word in image.ResourceDescriptor)
            {
                HashU32(ref shape, word);
            }
        }

        foreach (var buffer in evaluation.GlobalMemoryBindings)
        {
            foreach (var pc in buffer.InstructionPcs)
            {
                HashU32(ref shape, pc);
            }
        }

        var withCounts = shape;
        HashU32(ref withCounts, (uint)evaluation.GlobalMemoryBindings.Count);
        HashU32(ref withCounts, (uint)evaluation.ImageBindings.Count);
        HashU32(ref withCounts, (uint)(evaluation.VertexInputs?.Count ?? 0));

        lock (EmitterKeys)
        {
            StaticKeysA.Add(withCounts);
            StaticKeysB.Add(shape);
        }
    }

    // The key separates the initial registers, the buffer addresses and
    // the data lengths, and the emitter reads none of them once the runtime
    // scalar block carries the registers: the values are loaded from the
    // block, the byte bias is loaded from the block, and DataLength appears
    // only in a trace. Dropping them from the key leaves 144 distinct
    // programs a run where the full key leaves 512, and across three runs
    // no coarse key ever mapped to two different modules - the counter that
    // would have said so is still running, reported as coarse=n/conflicts.
    //
    // It works and it is not the default, because it makes the run worse.
    // Compilation falls from 6461ms to 1826ms and the guest, no longer
    // waiting on it, fills the command queue to its 16384 ceiling. At that
    // point the runtime discards the oldest whole frame, those frames'
    // draws never run, their render targets are never marked as written by
    // the device, and every dispatch goes back to reading them out of guest
    // memory: images 3.5s to 8.9s, worker commands 6900 to 3328, flips 53
    // to 63 down to 26. A circle, entered by making one end faster than the
    // other can take.
    //
    // So it waits for the worker. PS5RECOMP_COARSE_PROGRAM_KEY=1 turns it
    // on to measure the next time that changes.
    private static readonly bool FineProgramKey =
        Environment.GetEnvironmentVariable(
            "PS5RECOMP_COARSE_PROGRAM_KEY") != "1";

    private static readonly Dictionary<ulong, uint> CoarseKeys = new();
    private static long coarseKeyConflicts;

    private static void NoteCoarseKey(
        ulong key,
        ulong shaderAddress,
        uint spirvHash)
    {
        lock (CoarseKeys)
        {
            if (CoarseKeys.TryGetValue(key, out var seen))
            {
                if (seen != spirvHash)
                {
                    coarseKeyConflicts++;
                    Console.Error.WriteLine(
                        $"ps5gpu.coarse_key_conflict " +
                        $"addr=0x{shaderAddress:X16} key=0x{key:X16} " +
                        $"was=0x{seen:X8} now=0x{spirvHash:X8}");
                }
            }
            else
            {
                CoarseKeys[key] = spirvHash;
            }
        }
    }

    private static void NoteEmitterKey(
        ulong key,
        ulong shaderAddress,
        byte[] spirv)
    {
        var spirvHash = 2166136261u;
        foreach (var value in spirv)
        {
            spirvHash = (spirvHash ^ value) * 16777619u;
        }

        lock (EmitterKeys)
        {
            emitterKeyCompiles++;
            if (EmitterKeys.TryGetValue(key, out var seen))
            {
                emitterKeyRepeats++;
                if (seen != spirvHash)
                {
                    emitterKeyConflicts++;
                    Console.Error.WriteLine(
                        $"ps5gpu.emitter_key_conflict addr=0x{shaderAddress:X16} " +
                        $"key=0x{key:X16} was=0x{seen:X8} now=0x{spirvHash:X8}");
                }
            }
            else
            {
                EmitterKeys[key] = spirvHash;
            }

            if (emitterKeyCompiles % 128 == 0)
            {
                Console.Error.WriteLine(
                    $"ps5gpu.emitter_key compiles={emitterKeyCompiles} " +
                    $"distinct={EmitterKeys.Count} repeats={emitterKeyRepeats} " +
                    $"conflicts={emitterKeyConflicts} " +
                    $"hits={programCacheHits} misses={programCacheMisses} " +
                    $"coarse={CoarseKeys.Count}/{coarseKeyConflicts} " +
                    $"staticA={StaticKeysA.Count} staticB={StaticKeysB.Count}");
            }
        }
    }

    // Counting says one shader yields up to 73 programs; it does not say
    // what differs between them. Set PS5RECOMP_SPIRV_DUMP to a directory to
    // write every new shape out and diff them.
    private static readonly string? SpirvDumpDirectory =
        Environment.GetEnvironmentVariable("PS5RECOMP_SPIRV_DUMP");

    private static void DumpSpirvShape(
        ulong shaderAddress, int ordinal, uint hash, byte[] spirv)
    {
        if (string.IsNullOrEmpty(SpirvDumpDirectory) || ordinal > 8)
        {
            return;
        }
        try
        {
            Directory.CreateDirectory(SpirvDumpDirectory);
            File.WriteAllBytes(
                Path.Combine(
                    SpirvDumpDirectory,
                    $"{shaderAddress:X16}_{ordinal:D2}_{hash:X8}.spv"),
                spirv);
        }
        catch (IOException)
        {
        }
    }

    private static bool ComputeCacheWorthStoring(ulong shaderAddress)
    {
        lock (ComputeCacheStores)
        {
            if (ComputeCacheHopeless.Contains(shaderAddress))
            {
                return false;
            }
            ComputeCacheStores.TryGetValue(shaderAddress, out var stores);
            if (stores < StoresBeforeGivingUp)
            {
                ComputeCacheStores[shaderAddress] = stores + 1;
                return true;
            }
            ComputeCacheHopeless.Add(shaderAddress);
            Console.Error.WriteLine(
                $"ps5gpu.shader_cache giving_up addr=0x{shaderAddress:X16} " +
                $"stores={stores} hits=0");
            return false;
        }
    }

    private static void ComputeCacheHit(ulong shaderAddress)
    {
        lock (ComputeCacheStores)
        {
            ComputeCacheHopeless.Remove(shaderAddress);
            ComputeCacheStores[shaderAddress] = 0;
        }
    }

    private static readonly DirectGuestShaderMemory GuestMemory = new();
    private static readonly GuestShaderContext Context = new(GuestMemory);

    [UnmanagedCallersOnly(
        EntryPoint = "ps5gpu_get_abi_version",
        CallConvs = [typeof(CallConvCdecl)])]
    public static uint GetAbiVersion() => AbiVersion;

    [UnmanagedCallersOnly(
        EntryPoint = "ps5gpu_compile_spirv",
        CallConvs = [typeof(CallConvCdecl)])]
    public static Ps5GpuResult CompileSpirv(
        Ps5GpuShaderRequest* request,
        Ps5GpuShaderResult* result,
        byte* errorBuffer,
        uint errorBufferSize)
    {
        if (result is not null)
        {
            *result = default;
            result->StructSize = (uint)sizeof(Ps5GpuShaderResult);
        }

        try
        {
            var validation = ValidateRequest(request, result);
            if (validation != Ps5GpuResult.Ok)
            {
                return Fail(result, validation, errorBuffer, errorBufferSize, "invalid shader request");
            }

            if (TryLoadComputeCache(request, out var cachedShader, out var cachedManifest))
            {
                RefreshGlobalSnapshots(cachedManifest);
                var cachedResult = PopulateResult(
                    result,
                    cachedShader.Spirv,
                    cachedShader.AttributeCount,
                    cachedShader.GlobalMemoryBindingCount,
                    cachedShader.ImageBindingCount,
                    cachedShader.VertexInputCount,
                    cachedManifest,
                    errorBuffer,
                    errorBufferSize);
                if (cachedResult == Ps5GpuResult.Ok)
                {
                    ComputeCacheHit(request->ShaderAddress);
                    Console.Error.WriteLine(
                        $"ps5gpu.shader_cache hit stage={request->Stage} " +
                        $"addr=0x{request->ShaderAddress:X16} bytes={cachedShader.Spirv.Length} " +
                        $"manifest={cachedManifest.Length}");
                }
                return cachedResult;
            }

            var registers = new Dictionary<uint, uint>(
                checked((int)request->RegisterCount));
            for (var index = 0u; index < request->RegisterCount; index++)
            {
                registers[request->Registers[index].Register] =
                    request->Registers[index].Value;
            }

            Gen5ComputeSystemRegisters? computeRegisters =
                request->Stage == Ps5GpuShaderStage.Compute
                ? new Gen5ComputeSystemRegisters(
                    OptionalRegister(request->ComputeWorkGroupXRegister),
                    OptionalRegister(request->ComputeWorkGroupYRegister),
                    OptionalRegister(request->ComputeWorkGroupZRegister),
                    OptionalRegister(request->ComputeThreadGroupSizeRegister))
                : null;

            var decodeStarted = StageBegin();
            var decoded = Gen5ShaderTranslator.TryCreateState(
                Context,
                request->ShaderAddress,
                request->ShaderHeaderAddress,
                registers,
                request->UserDataBaseRegister,
                out var state,
                out var error,
                computeRegisters,
                request->UserDataScalarRegisterBase);
            StageEnd(0, decodeStarted);
            if (!decoded)
            {
                return Fail(
                    result,
                    Ps5GpuResult.StateDecodeFailed,
                    errorBuffer,
                    errorBufferSize,
                    error);
            }

            var resolveVertexInputs =
                request->Stage == Ps5GpuShaderStage.Vertex &&
                (request->Flags & ResolveVertexInputs) != 0;
            var evaluateStarted = StageBegin();
            var evaluated = Gen5ShaderScalarEvaluator.TryEvaluate(
                Context,
                state,
                out var evaluation,
                out error,
                resolveVertexInputs);
            StageEnd(1, evaluateStarted);
            if (!evaluated)
            {
                return Fail(
                    result,
                    Ps5GpuResult.EvaluationFailed,
                    errorBuffer,
                    errorBufferSize,
                    error);
            }

            try
            {
                var keyStarted = StageBegin();
                var key = ComputeEmitterKey(
                    request, state, evaluation, coarse: !FineProgramKey);
                var checkKey = ComputeEmitterKey(
                    request, state, evaluation, coarse: FineProgramKey);
                NoteStaticKeys(request, state, evaluation);
                StageEnd(5, keyStarted);
                var spirvStarted = StageBegin();
                Gen5SpirvShader shader;
                if (!TryTakeProgram(key, request, evaluation, out shader))
                {
                    if (!TryCompile(
                            request, state, evaluation, out shader, out error))
                    {
                        return Fail(
                            result,
                            Ps5GpuResult.SpirvCompilationFailed,
                            errorBuffer,
                            errorBufferSize,
                            error);
                    }

                    NoteSpirvShape(key, request->ShaderAddress, shader.Spirv);
                    var coarseHash = 2166136261u;
                    foreach (var value in shader.Spirv)
                    {
                        coarseHash = (coarseHash ^ value) * 16777619u;
                    }

                    NoteCoarseKey(
                        checkKey, request->ShaderAddress, coarseHash);
                    KeepProgram(key, shader);
                }

                StageEnd(2, spirvStarted);
                var manifestStarted = StageBegin();
                var resourceManifest =
                    BuildResourceManifest(request, shader, evaluation);
                StageEnd(3, manifestStarted);
                var storeStarted = StageBegin();
                TryStoreComputeCache(request, shader, resourceManifest);
                StageEnd(4, storeStarted);
                ReportStages();
                return PopulateResult(
                    result,
                    shader.Spirv,
                    shader.AttributeCount,
                    checked((uint)shader.GlobalMemoryBindings.Count),
                    checked((uint)shader.ImageBindings.Count),
                    checked((uint)shader.VertexInputs.Count),
                    resourceManifest,
                    errorBuffer,
                    errorBufferSize);
            }
            finally
            {
                ReturnPooledEvaluationArrays(evaluation);
            }
        }
        catch (Exception exception)
        {
            return Fail(
                result,
                Ps5GpuResult.InternalError,
                errorBuffer,
                errorBufferSize,
                exception.Message);
        }
    }

    [UnmanagedCallersOnly(
        EntryPoint = "ps5gpu_free",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Free(void* allocation) => NativeMemory.Free(allocation);

    private sealed record CachedShader(
        byte[] Spirv,
        uint AttributeCount,
        uint GlobalMemoryBindingCount,
        uint ImageBindingCount,
        uint VertexInputCount);

    private static Ps5GpuResult PopulateResult(
        Ps5GpuShaderResult* result,
        byte[] spirv,
        uint attributeCount,
        uint globalMemoryBindingCount,
        uint imageBindingCount,
        uint vertexInputCount,
        byte[] resourceManifest,
        byte* errorBuffer,
        uint errorBufferSize)
    {
        byte* spirvAllocation = null;
        byte* resourceAllocation = null;
        try
        {
            spirvAllocation = (byte*)NativeMemory.Alloc(
                checked((nuint)spirv.Length));
            if (spirvAllocation is null)
            {
                return Fail(
                    result,
                    Ps5GpuResult.OutOfMemory,
                    errorBuffer,
                    errorBufferSize,
                    "SPIR-V allocation failed");
            }

            if (resourceManifest.Length != 0)
            {
                resourceAllocation = (byte*)NativeMemory.Alloc(
                    checked((nuint)resourceManifest.Length));
                if (resourceAllocation is null)
                {
                    NativeMemory.Free(spirvAllocation);
                    spirvAllocation = null;
                    return Fail(
                        result,
                        Ps5GpuResult.OutOfMemory,
                        errorBuffer,
                        errorBufferSize,
                        "resource manifest allocation failed");
                }
            }

            spirv.CopyTo(new Span<byte>(spirvAllocation, spirv.Length));
            if (resourceManifest.Length != 0)
            {
                resourceManifest.CopyTo(
                    new Span<byte>(
                        resourceAllocation,
                        resourceManifest.Length));
            }
        }
        catch
        {
            NativeMemory.Free(resourceAllocation);
            NativeMemory.Free(spirvAllocation);
            throw;
        }

        result->Spirv = spirvAllocation;
        result->SpirvSize = checked((uint)spirv.Length);
        result->AttributeCount = attributeCount;
        result->GlobalMemoryBindingCount = globalMemoryBindingCount;
        result->ImageBindingCount = imageBindingCount;
        result->VertexInputCount = vertexInputCount;
        result->ResourceManifest = resourceAllocation;
        result->ResourceManifestSize =
            checked((uint)resourceManifest.Length);
        result->Status = Ps5GpuResult.Ok;
        WriteError(errorBuffer, errorBufferSize, string.Empty);
        return Ps5GpuResult.Ok;
    }

    private static bool TryLoadComputeCache(
        Ps5GpuShaderRequest* request,
        out CachedShader shader,
        out byte[] resourceManifest)
    {
        shader = default!;
        resourceManifest = [];
        if (!TryGetComputeCachePath(request, out var path) ||
            !File.Exists(path))
        {
            return false;
        }

        try
        {
            var file = File.ReadAllBytes(path);
            if (file.Length < ShaderCacheHeaderSize)
            {
                return false;
            }
            var span = file.AsSpan();
            if (ReadU32(span, 0) != ShaderCacheMagic ||
                ReadU32(span, 4) != ShaderCacheVersion)
            {
                return false;
            }

            var attributeCount = ReadU32(span, 8);
            var globalCount = ReadU32(span, 12);
            var imageCount = ReadU32(span, 16);
            var vertexCount = ReadU32(span, 20);
            var spirvSize = checked((int)ReadU32(span, 24));
            var manifestSize = checked((int)ReadU32(span, 28));
            var payloadCrc = ReadU32(span, 32);
            if (spirvSize < 20 ||
                manifestSize < ResourceManifestHeaderSize ||
                ShaderCacheHeaderSize + spirvSize + manifestSize !=
                    file.Length)
            {
                return false;
            }

            var spirv = span.Slice(ShaderCacheHeaderSize, spirvSize);
            var manifest = span.Slice(
                ShaderCacheHeaderSize + spirvSize,
                manifestSize);
            if (ReadU32(spirv, 0) != 0x07230203 ||
                ReadU32(manifest, 0) != ResourceManifestMagic ||
                Crc32(file.AsSpan(ShaderCacheHeaderSize)) != payloadCrc)
            {
                return false;
            }

            // The stored manifest stops at its data section; the caller
            // is handed the full-size buffer the header describes, with the
            // data left zeroed for RefreshGlobalSnapshots to fill.
            var totalSize = checked((int)ReadU32(manifest, 12));
            if (totalSize < manifestSize)
            {
                return false;
            }
            shader = new CachedShader(
                spirv.ToArray(),
                attributeCount,
                globalCount,
                imageCount,
                vertexCount);
            resourceManifest = new byte[totalSize];
            manifest.CopyTo(resourceManifest.AsSpan(0, manifestSize));
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static void TryStoreComputeCache(
        Ps5GpuShaderRequest* request,
        Gen5SpirvShader shader,
        byte[] resourceManifest)
    {
        if (!TryGetComputeCachePath(request, out var path) ||
            !ComputeCacheWorthStoring(request->ShaderAddress))
        {
            return;
        }

        try
        {
            var directory = Path.GetDirectoryName(path);
            if (string.IsNullOrEmpty(directory))
            {
                return;
            }
            Directory.CreateDirectory(directory);

            // Only the manifest down to its data section is worth keeping.
            // Everything past that offset is a snapshot of whatever the
            // guest buffers held when this shader was translated, and the
            // load path overwrites all of it from guest memory before the
            // caller ever sees it. Writing it out meant a file averaging
            // 2.7MB and peaking at 20MB, and a CRC over the same, per
            // translation, on the thread that owes the frame - two
            // gigabytes in two minutes for bytes nobody reads.
            //
            // Vertex-input data is in that same section and is not
            // refreshed on load, so the trim only applies where there is
            // none. Compute, which is all this cache stores, has none.
            var storedManifest = TrimmableManifestPrefix(resourceManifest);
            var file = new byte[
                checked(
                    ShaderCacheHeaderSize +
                    shader.Spirv.Length +
                    storedManifest)];
            var span = file.AsSpan();
            WriteU32(span, 0, ShaderCacheMagic);
            WriteU32(span, 4, ShaderCacheVersion);
            WriteU32(span, 8, shader.AttributeCount);
            WriteU32(
                span,
                12,
                checked((uint)shader.GlobalMemoryBindings.Count));
            WriteU32(
                span,
                16,
                checked((uint)shader.ImageBindings.Count));
            WriteU32(
                span,
                20,
                checked((uint)shader.VertexInputs.Count));
            WriteU32(span, 24, checked((uint)shader.Spirv.Length));
            WriteU32(span, 28, checked((uint)storedManifest));
            shader.Spirv.CopyTo(span.Slice(ShaderCacheHeaderSize));
            resourceManifest.AsSpan(0, storedManifest).CopyTo(
                span.Slice(
                    ShaderCacheHeaderSize + shader.Spirv.Length));
            WriteU32(
                span,
                32,
                Crc32(span.Slice(ShaderCacheHeaderSize)));

            var temporary = path + $".{Environment.ProcessId}.tmp";
            File.WriteAllBytes(temporary, file);
            File.Move(temporary, path, true);
            Console.Error.WriteLine(
                $"ps5gpu.shader_cache store stage={request->Stage} " +
                $"addr=0x{request->ShaderAddress:X16} bytes={shader.Spirv.Length} " +
                $"manifest={resourceManifest.Length}");
        }
        catch
        {
            // Cache failures must never change shader compilation behavior.
        }
    }

    private static bool TryGetComputeCachePath(
        Ps5GpuShaderRequest* request,
        out string path)
    {
        path = string.Empty;
        if (request->Stage != Ps5GpuShaderStage.Compute)
        {
            return false;
        }
        var directory =
            Environment.GetEnvironmentVariable("PS5GPU_SHADER_CACHE_DIR");
        if (string.IsNullOrWhiteSpace(directory))
        {
            return false;
        }

        var key = ComputeRequestKey(request);
        path = Path.Combine(
            directory,
            $"compute-{request->ShaderAddress:X16}-{key}.bin");
        return true;
    }

    private static string ComputeRequestKey(Ps5GpuShaderRequest* request)
    {
        var text = new StringBuilder(2048);
        text.Append(ShaderCacheVersion).Append('|')
            .Append((uint)request->Stage).Append('|')
            .Append(request->Flags).Append('|')
            .Append(request->ShaderAddress).Append('|')
            .Append(request->ShaderHeaderAddress).Append('|')
            .Append(request->UserDataBaseRegister).Append('|')
            .Append(request->UserDataScalarRegisterBase).Append('|')
            .Append(request->LocalSizeX).Append('|')
            .Append(request->LocalSizeY).Append('|')
            .Append(request->LocalSizeZ).Append('|')
            .Append(request->WaveLaneCount).Append('|')
            .Append(request->StorageBufferOffsetAlignment).Append('|')
            .Append(request->GlobalBufferBase).Append('|')
            .Append(request->TotalGlobalBufferCount).Append('|')
            .Append(request->ImageBindingBase).Append('|')
            .Append(request->InitialScalarBufferIndex).Append('|')
            .Append(request->ComputeWorkGroupXRegister).Append('|')
            .Append(request->ComputeWorkGroupYRegister).Append('|')
            .Append(request->ComputeWorkGroupZRegister).Append('|')
            .Append(request->ComputeThreadGroupSizeRegister).Append('|')
            .Append(request->RegisterCount).Append('|');
        for (var index = 0u; index < request->RegisterCount; index++)
        {
            text.Append(request->Registers[index].Register)
                .Append('=')
                .Append(request->Registers[index].Value)
                .Append(';');
        }

        var digest = SHA256.HashData(
            Encoding.UTF8.GetBytes(text.ToString()));
        return Convert.ToHexString(digest);
    }

    // How much of a manifest is worth storing: everything up to the data
    // section, unless something in that section will not be rebuilt on
    // load, in which case all of it.
    private static int TrimmableManifestPrefix(byte[] manifest)
    {
        var span = manifest.AsSpan();
        if (span.Length < ResourceManifestHeaderSize ||
            ReadU32(span, 0) != ResourceManifestMagic ||
            ReadU32(span, 4) != ResourceManifestVersion ||
            ReadU32(span, 28) != 0)
        {
            return manifest.Length;
        }
        var dataOffset = ReadU32(span, 44);
        return dataOffset >= ResourceManifestHeaderSize &&
            dataOffset <= (uint)manifest.Length
            ? checked((int)dataOffset)
            : manifest.Length;
    }

    private static void RefreshGlobalSnapshots(byte[] manifest)
    {
        try
        {
            var span = manifest.AsSpan();
            if (span.Length < ResourceManifestHeaderSize ||
                ReadU32(span, 0) != ResourceManifestMagic ||
                ReadU32(span, 4) != ResourceManifestVersion)
            {
                return;
            }
            var globalCount = ReadU32(span, 20);
            var globalOffset = ReadU32(span, 32);
            for (var index = 0u; index < globalCount; index++)
            {
                var record = checked(
                    (int)globalOffset +
                    (int)index * ResourceGlobalSize);
                if (record < 0 ||
                    record + ResourceGlobalSize > span.Length)
                {
                    return;
                }
                var baseAddress = ReadU64(span, record + 8);
                var dataOffset = checked((int)ReadU32(span, record + 16));
                var dataSize = checked((int)ReadU32(span, record + 20));
                if (baseAddress < 0x10000 ||
                    dataOffset < 0 ||
                    dataSize < 0 ||
                    dataOffset + dataSize > span.Length)
                {
                    continue;
                }
                GuestMemory.TryRead(
                    baseAddress,
                    span.Slice(dataOffset, dataSize));
            }
        }
        catch
        {
            // The cached manifest remains a valid fallback snapshot.
        }
    }

    // A byte at a time rather than a bit at a time. The payload this runs
    // over is a translated shader, so the eight-fold difference is milli-
    // seconds on the thread that owes the frame, not a micro-optimisation.
    private static readonly uint[] Crc32Table = BuildCrc32Table();

    private static uint[] BuildCrc32Table()
    {
        var table = new uint[256];
        for (var index = 0u; index < table.Length; index++)
        {
            var value = index;
            for (var bit = 0; bit < 8; bit++)
            {
                value = (value >> 1) ^
                    (0xEDB8_8320u & (uint)-(int)(value & 1));
            }
            table[index] = value;
        }
        return table;
    }

    private static uint Crc32(ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFF_FFFFu;
        foreach (var value in data)
        {
            crc = (crc >> 8) ^ Crc32Table[(byte)(crc ^ value)];
        }
        return ~crc;
    }

    private static Ps5GpuResult ValidateRequest(
        Ps5GpuShaderRequest* request,
        Ps5GpuShaderResult* result)
    {
        if (request is null ||
            result is null ||
            request->StructSize < sizeof(Ps5GpuShaderRequest) ||
            request->AbiVersion != AbiVersion ||
            request->ShaderAddress == 0 ||
            request->Stage > Ps5GpuShaderStage.Compute ||
            request->RegisterCount > MaximumRegisterCount ||
            (request->RegisterCount != 0 && request->Registers is null) ||
            request->PixelOutputCount > MaximumPixelOutputCount ||
            (request->PixelOutputCount != 0 && request->PixelOutputs is null))
        {
            return Ps5GpuResult.InvalidArgument;
        }

        return Ps5GpuResult.Ok;
    }

    private static bool TryCompile(
        Ps5GpuShaderRequest* request,
        Gen5ShaderState state,
        Gen5ShaderEvaluation evaluation,
        out Gen5SpirvShader shader,
        out string error)
    {
        var alignment = Math.Max(request->StorageBufferOffsetAlignment, 1);
        switch (request->Stage)
        {
            case Ps5GpuShaderStage.Vertex:
                return Gen5SpirvTranslator.TryCompileVertexShader(
                    state,
                    evaluation,
                    out shader,
                    out error,
                    request->GlobalBufferBase,
                    request->TotalGlobalBufferCount,
                    request->ImageBindingBase,
                    request->InitialScalarBufferIndex,
                    request->RequiredVertexOutputCount,
                    alignment);

            case Ps5GpuShaderStage.Pixel:
                var outputs = ReadPixelOutputs(request);
                return Gen5SpirvTranslator.TryCompilePixelShader(
                    state,
                    evaluation,
                    outputs,
                    out shader,
                    out error,
                    request->GlobalBufferBase,
                    request->TotalGlobalBufferCount,
                    request->ImageBindingBase,
                    request->InitialScalarBufferIndex,
                    request->PixelInputEnable,
                    request->PixelInputAddress,
                    alignment);

            case Ps5GpuShaderStage.Compute:
                return Gen5SpirvTranslator.TryCompileComputeShader(
                    state,
                    evaluation,
                    request->LocalSizeX,
                    request->LocalSizeY,
                    request->LocalSizeZ,
                    out shader,
                    out error,
                    request->GlobalBufferBase,
                    request->TotalGlobalBufferCount,
                    request->InitialScalarBufferIndex,
                    request->WaveLaneCount == 0 ? 32 : request->WaveLaneCount,
                    alignment);

            default:
                shader = default!;
                error = "unsupported shader stage";
                return false;
        }
    }

    private static IReadOnlyList<Gen5PixelOutputBinding> ReadPixelOutputs(
        Ps5GpuShaderRequest* request)
    {
        if (request->PixelOutputCount == 0)
        {
            return [new Gen5PixelOutputBinding(0, 0, Gen5PixelOutputKind.Float)];
        }

        var outputs = new Gen5PixelOutputBinding[request->PixelOutputCount];
        for (var index = 0u; index < request->PixelOutputCount; index++)
        {
            var output = request->PixelOutputs[index];
            outputs[index] = new Gen5PixelOutputBinding(
                output.GuestSlot,
                output.HostLocation,
                (Gen5PixelOutputKind)output.Kind);
        }
        return outputs;
    }

    private static uint? OptionalRegister(int value) =>
        value < 0 ? null : checked((uint)value);

    private static byte[] BuildResourceManifest(
        Ps5GpuShaderRequest* request,
        Gen5SpirvShader shader,
        Gen5ShaderEvaluation evaluation)
    {
        checked
        {
            var globalOffset = ResourceManifestHeaderSize;
            var imageOffset =
                globalOffset +
                shader.GlobalMemoryBindings.Count * ResourceGlobalSize;
            var vertexOffset =
                imageOffset +
                shader.ImageBindings.Count * ResourceImageSize;
            var scalarOffset = Align8(
                vertexOffset +
                shader.VertexInputs.Count * ResourceVertexInputSize);
            // 256 initial registers, then one byte bias per descriptor,
            // then one write mark per descriptor. The translator computes
            // the same two offsets from the same two numbers.
            var descriptorCount =
                request->GlobalBufferBase +
                shader.GlobalMemoryBindings.Count;
            var scalarDwords = request->InitialScalarBufferIndex >= 0
                ? ScalarRegisterCount + descriptorCount + descriptorCount
                : 0;
            var dataOffset = Align8(scalarOffset + scalarDwords * 4);
            var totalSize = dataOffset;
            foreach (var binding in shader.GlobalMemoryBindings)
            {
                ValidateData(binding.Data, binding.DataLength);
                totalSize = Align8(totalSize + binding.DataLength);
            }
            foreach (var input in shader.VertexInputs)
            {
                ValidateData(input.Data, input.DataLength);
                totalSize = Align8(totalSize + input.DataLength);
            }

            var manifest = new byte[totalSize];
            var span = manifest.AsSpan();
            WriteU32(span, 0, ResourceManifestMagic);
            WriteU32(span, 4, ResourceManifestVersion);
            WriteU32(span, 8, ResourceManifestHeaderSize);
            WriteU32(span, 12, checked((uint)totalSize));
            WriteU32(span, 16, (uint)request->Stage);
            WriteU32(
                span,
                20,
                checked((uint)shader.GlobalMemoryBindings.Count));
            WriteU32(
                span,
                24,
                checked((uint)shader.ImageBindings.Count));
            WriteU32(
                span,
                28,
                checked((uint)shader.VertexInputs.Count));
            WriteU32(span, 32, checked((uint)globalOffset));
            WriteU32(span, 36, checked((uint)imageOffset));
            WriteU32(span, 40, checked((uint)vertexOffset));
            WriteU32(span, 44, checked((uint)dataOffset));
            if (scalarDwords != 0)
            {
                WriteU32(span, 48, checked((uint)scalarOffset));
                WriteU32(span, 52, checked((uint)scalarDwords));
                WriteU32(
                    span,
                    56,
                    checked((uint)request->InitialScalarBufferIndex) + 1);
                var initial = evaluation.InitialScalarRegisters;
                for (var index = 0; index < ScalarRegisterCount; index++)
                {
                    WriteU32(
                        span,
                        scalarOffset + index * 4,
                        index < initial.Count ? initial[index] : 0u);
                }

                // Every word past the registers is a per-binding byte bias,
                // and all of them are zero: the native presenter asks for a
                // storage buffer offset alignment of one, so it binds each
                // buffer at exactly the guest offset and nothing is
                // discarded. The shader reads the slots unconditionally, so
                // the space is still reserved.
            }

            var dataCursor = dataOffset;
            for (var index = 0;
                 index < shader.GlobalMemoryBindings.Count;
                 index++)
            {
                var binding = shader.GlobalMemoryBindings[index];
                var record = globalOffset + index * ResourceGlobalSize;
                var flags =
                    (binding.Writable ? ResourceBufferWritable : 0) |
                    (binding.WriteBackToGuest ? ResourceBufferWriteBack : 0);
                WriteU32(
                    span,
                    record,
                    checked((uint)(request->GlobalBufferBase + index)));
                WriteU32(span, record + 4, binding.ScalarAddress);
                WriteU64(span, record + 8, binding.BaseAddress);
                WriteU32(span, record + 16, checked((uint)dataCursor));
                WriteU32(
                    span,
                    record + 20,
                    checked((uint)binding.DataLength));
                WriteU32(span, record + 24, flags);
                binding.Data.AsSpan(0, binding.DataLength).CopyTo(
                    span.Slice(dataCursor, binding.DataLength));
                dataCursor = Align8(dataCursor + binding.DataLength);
            }

            for (var index = 0;
                 index < shader.ImageBindings.Count;
                 index++)
            {
                var binding = shader.ImageBindings[index];
                var record = imageOffset + index * ResourceImageSize;
                var storage = Gen5ShaderTranslator.RequiresStorageImage(
                    binding,
                    shader.ImageBindings);
                var validExtent = TryDecodeImageExtent(
                    binding.ResourceDescriptor,
                    out var baseAddress,
                    out var width,
                    out var height);
                var flags =
                    (storage ? ResourceImageStorage : 0) |
                    (binding.SamplerDescriptor.Count != 0
                        ? ResourceImageHasSampler
                        : 0) |
                    (validExtent ? ResourceImageValidExtent : 0);
                WriteU32(
                    span,
                    record,
                    checked((uint)(request->ImageBindingBase + index + 1)));
                WriteU32(span, record + 4, binding.Pc);
                WriteU32(span, record + 8, flags);
                WriteU32(
                    span,
                    record + 12,
                    binding.MipLevel ?? uint.MaxValue);
                WriteU64(span, record + 16, baseAddress);
                WriteU32(span, record + 24, width);
                WriteU32(span, record + 28, height);
                WriteDescriptorWords(
                    span,
                    record + 32,
                    binding.ResourceDescriptor,
                    8);
                WriteDescriptorWords(
                    span,
                    record + 64,
                    binding.SamplerDescriptor,
                    4);
            }

            for (var index = 0;
                 index < shader.VertexInputs.Count;
                 index++)
            {
                var input = shader.VertexInputs[index];
                var record =
                    vertexOffset + index * ResourceVertexInputSize;
                WriteU32(span, record, input.Pc);
                WriteU32(span, record + 4, input.Location);
                WriteU32(span, record + 8, input.ComponentCount);
                WriteU32(span, record + 12, input.DataFormat);
                WriteU32(span, record + 16, input.NumberFormat);
                WriteU32(span, record + 20, input.Stride);
                WriteU32(span, record + 24, input.OffsetBytes);
                WriteU32(
                    span,
                    record + 28,
                    checked((uint)input.DataLength));
                WriteU64(span, record + 32, input.BaseAddress);
                WriteU32(span, record + 40, checked((uint)dataCursor));
                WriteU32(span, record + 44, input.DataPooled ? 1u : 0u);
                input.Data.AsSpan(0, input.DataLength).CopyTo(
                    span.Slice(dataCursor, input.DataLength));
                dataCursor = Align8(dataCursor + input.DataLength);
            }
            return manifest;
        }
    }

    private static void ValidateData(byte[] data, int length)
    {
        if (length < 0 || length > data.Length)
        {
            throw new InvalidDataException(
                $"invalid resource data length {length}/{data.Length}");
        }
    }

    private static int Align8(int value) =>
        checked((value + 7) & ~7);

    private static void WriteU32(Span<byte> destination, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(
            destination.Slice(offset, sizeof(uint)),
            value);

    private static void WriteU64(Span<byte> destination, int offset, ulong value) =>
        BinaryPrimitives.WriteUInt64LittleEndian(
            destination.Slice(offset, sizeof(ulong)),
            value);

    private static uint ReadU32(ReadOnlySpan<byte> source, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(
            source.Slice(offset, sizeof(uint)));

    private static ulong ReadU64(ReadOnlySpan<byte> source, int offset) =>
        BinaryPrimitives.ReadUInt64LittleEndian(
            source.Slice(offset, sizeof(ulong)));

    private static void WriteDescriptorWords(
        Span<byte> destination,
        int offset,
        IReadOnlyList<uint> words,
        int maximumCount)
    {
        for (var index = 0;
             index < Math.Min(words.Count, maximumCount);
             index++)
        {
            WriteU32(destination, offset + index * sizeof(uint), words[index]);
        }
    }

    private static bool TryDecodeImageExtent(
        IReadOnlyList<uint> descriptor,
        out ulong baseAddress,
        out uint width,
        out uint height)
    {
        baseAddress = 0;
        width = 0;
        height = 0;
        if (descriptor.Count < 4)
        {
            return false;
        }

        baseAddress =
            (((ulong)descriptor[1] & 0xFFu) << 32 | descriptor[0]) << 8;
        width =
            (((descriptor[1] >> 30) & 0x3u) |
             ((descriptor[2] & 0x3FFFu) << 2)) + 1;
        height = ((descriptor[2] >> 14) & 0xFFFFu) + 1;
        var unifiedFormat = (descriptor[1] >> 20) & 0x1FFu;
        return baseAddress >= 0x10000 &&
            width is > 0 and <= 16384 &&
            height is > 0 and <= 16384 &&
            unifiedFormat != 0;
    }

    private static void ReturnPooledEvaluationArrays(
        Gen5ShaderEvaluation evaluation)
    {
        var returned = new HashSet<byte[]>(
            ReferenceEqualityComparer.Instance);
        foreach (var binding in evaluation.GlobalMemoryBindings)
        {
            if (binding.DataPooled && returned.Add(binding.Data))
            {
                Gen5ShaderScalarEvaluator.GlobalMemoryPool.Return(binding.Data);
            }
        }

        if (evaluation.VertexInputs is not { } vertexInputs)
        {
            return;
        }
        foreach (var binding in vertexInputs)
        {
            if (binding.DataPooled && returned.Add(binding.Data))
            {
                Gen5ShaderScalarEvaluator.GlobalMemoryPool.Return(binding.Data);
            }
        }
    }

    private static Ps5GpuResult Fail(
        Ps5GpuShaderResult* result,
        Ps5GpuResult status,
        byte* errorBuffer,
        uint errorBufferSize,
        string error)
    {
        if (result is not null)
        {
            result->Status = status;
        }
        WriteError(errorBuffer, errorBufferSize, error);
        return status;
    }

    private static void WriteError(
        byte* errorBuffer,
        uint errorBufferSize,
        string error)
    {
        if (errorBuffer is null || errorBufferSize == 0)
        {
            return;
        }

        var destination = new Span<byte>(
            errorBuffer,
            checked((int)Math.Min(errorBufferSize, int.MaxValue)));
        destination.Clear();
        if (destination.Length == 1 || error.Length == 0)
        {
            return;
        }

        var encoded = Encoding.UTF8.GetBytes(error);
        encoded.AsSpan(0, Math.Min(encoded.Length, destination.Length - 1))
            .CopyTo(destination);
    }
}
