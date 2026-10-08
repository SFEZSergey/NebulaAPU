// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using SharpEmu.HLE;
using SharpEmu.Libs.Kernel;

namespace Ps5HleBridge;

[StructLayout(LayoutKind.Sequential)]
public unsafe struct Ps5HleCallFrame
{
    public fixed ulong Gpr[16];
    public ulong Rip;
    public ulong Rflags;
    public ulong FsBase;
    public ulong GsBase;
    public ushort FpuControlWord;
    public ushort Reserved0;
    public uint Mxcsr;
    public fixed ulong Xmm[32];
    public uint ControlFlags;
    public uint Reserved1;
}

internal static unsafe class BridgeExports
{
    private const int ErrorNotImplemented = unchecked((int)0x80020001);
    private const uint ControlContextTransfer = 1u << 0;
    private const uint ControlRestoreFullFpuState = 1u << 1;
    private static readonly DirectGuestMemory GuestMemory = new();
    private static readonly BridgeGuestThreadScheduler GuestScheduler = new();
    private static readonly object InitGate = new();
    private static readonly object PolicyGate = new();
    private static readonly HashSet<string> LoggedAutoStubs =
        new(StringComparer.Ordinal);
    private static IReadOnlyDictionary<string, ExportedFunction>? _exports;
    private static HlePolicy _policy = HlePolicy.Disabled;
    private static string _policySignature = string.Empty;
    private static long _nextPolicyRefresh;

    [ThreadStatic]
    private static bool _guestThreadEntered;

    [UnmanagedCallersOnly(
        EntryPoint = "ps5hle_initialize",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize(byte* app0Path)
    {
        try
        {
            EnsureInitialized();
            var app0 = ReadUtf8(app0Path);
            if (!string.IsNullOrWhiteSpace(app0) && Directory.Exists(app0))
            {
                KernelMemoryCompatExports.RegisterGuestPathMount("/app0", app0);
                KernelMemoryCompatExports.RegisterGuestPathMount("/hostapp", app0);
            }
            return _exports!.Count;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[PS5HLE] initialization failed: {ex}");
            return -1;
        }
    }

    [UnmanagedCallersOnly(
        EntryPoint = "ps5hle_has_nid",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int HasNid(byte* nid)
    {
        try
        {
            EnsureInitialized();
            var key = ReadUtf8(nid);
            if (key.Length == 0)
            {
                return 0;
            }
            if (_exports!.ContainsKey(key))
            {
                return 1;
            }

            var policy = GetPolicy();
            if (policy.Rules.ContainsKey(key))
            {
                return 2;
            }
            return policy.Permissive ? 3 : 0;
        }
        catch
        {
            return 0;
        }
    }

    [UnmanagedCallersOnly(
        EntryPoint = "ps5hle_dispatch",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int Dispatch(byte* nid, Ps5HleCallFrame* frame)
    {
        if (frame is null)
        {
            return -1;
        }

        long traceSequence = 0;
        try
        {
            EnsureInitialized();
            frame->ControlFlags = 0;
            _ = GuestThreadExecution.TryConsumeCurrentContextTransfer(out _);
            var key = ReadUtf8(nid);
            if (key.Length == 0)
            {
                frame->Gpr[(int)CpuRegister.Rax] =
                    unchecked((ulong)ErrorNotImplemented);
                return 0;
            }
            if (!_exports!.TryGetValue(key, out var export))
            {
                var policy = GetPolicy();
                if (policy.Rules.TryGetValue(key, out var configuredReturn))
                {
                    traceSequence = HleCallTrace.Begin(
                        key,
                        "<policy-stub>",
                        "ps5recomp",
                        frame->Rip);
                    ApplyAutoStub(frame, key, configuredReturn, "rule");
                    HleCallTrace.Complete(
                        traceSequence,
                        frame->Gpr[(int)CpuRegister.Rax]);
                    traceSequence = 0;
                    return 1;
                }
                if (policy.Permissive)
                {
                    traceSequence = HleCallTrace.Begin(
                        key,
                        "<permissive-stub>",
                        "ps5recomp",
                        frame->Rip);
                    ApplyAutoStub(
                        frame,
                        key,
                        policy.DefaultReturn,
                        "permissive");
                    HleCallTrace.Complete(
                        traceSequence,
                        frame->Gpr[(int)CpuRegister.Rax]);
                    traceSequence = 0;
                    return 1;
                }

                frame->Gpr[(int)CpuRegister.Rax] =
                    unchecked((ulong)ErrorNotImplemented);
                traceSequence = HleCallTrace.Begin(
                    key,
                    "<unresolved>",
                    "ps5recomp",
                    frame->Rip);
                HleCallTrace.Complete(
                    traceSequence,
                    frame->Gpr[(int)CpuRegister.Rax],
                    dispatchSucceeded: false);
                traceSequence = 0;
                return 0;
            }

            traceSequence = HleCallTrace.Begin(
                export.Nid,
                export.Name,
                export.LibraryName,
                frame->Rip);
            var context = new CpuContext(GuestMemory, Generation.Gen5)
            {
                Rip = frame->Rip,
                Rflags = frame->Rflags,
                FsBase = frame->FsBase,
                GsBase = frame->GsBase,
                FpuControlWord = frame->FpuControlWord,
                Mxcsr = frame->Mxcsr,
            };
            for (var index = 0; index < 16; index++)
            {
                context[(CpuRegister)index] = frame->Gpr[index];
                context.SetXmmRegister(
                    index,
                    frame->Xmm[index * 2],
                    frame->Xmm[(index * 2) + 1]);
            }

            if (!_guestThreadEntered)
            {
                _ = GuestThreadExecution.EnterGuestThread(
                    frame->FsBase != 0
                        ? frame->FsBase
                        : unchecked((ulong)Environment.CurrentManagedThreadId));
                _guestThreadEntered = true;
            }

            var returnSlotAddress = frame->Gpr[(int)CpuRegister.Rsp];
            var previousScheduler = GuestThreadExecution.Scheduler;
            var previousImportFrame = GuestThreadExecution.EnterImportCallFrame(
                frame->Rip,
                returnSlotAddress <= ulong.MaxValue - sizeof(ulong)
                    ? returnSlotAddress + sizeof(ulong)
                    : 0,
                returnSlotAddress);
            try
            {
                GuestThreadExecution.Scheduler = GuestScheduler;
                context.ClearRaxWriteFlag();
                var result = export.Function(context);
                if (!context.WasRaxWritten)
                {
                    context[CpuRegister.Rax] = unchecked((ulong)result);
                }
            }
            finally
            {
                GuestThreadExecution.RestoreImportCallFrame(previousImportFrame);
                GuestThreadExecution.Scheduler = previousScheduler;
            }

            for (var index = 0; index < 16; index++)
            {
                frame->Gpr[index] = context[(CpuRegister)index];
                context.GetXmmRegister(
                    index,
                    out frame->Xmm[index * 2],
                    out frame->Xmm[(index * 2) + 1]);
            }
            frame->Rip = context.Rip;
            frame->Rflags = context.Rflags;
            frame->FsBase = context.FsBase;
            frame->GsBase = context.GsBase;
            frame->FpuControlWord = context.FpuControlWord;
            frame->Mxcsr = context.Mxcsr;

            if (GuestThreadExecution.TryConsumeCurrentContextTransfer(
                    out var transferTarget))
            {
                ApplyContextTransfer(frame, transferTarget);
            }
            HleCallTrace.Complete(
                traceSequence,
                frame->Gpr[(int)CpuRegister.Rax]);
            traceSequence = 0;
            return 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[PS5HLE] dispatch failed: {ex}");
            frame->Gpr[(int)CpuRegister.Rax] =
                unchecked((ulong)ErrorNotImplemented);
            HleCallTrace.Complete(
                traceSequence,
                frame->Gpr[(int)CpuRegister.Rax],
                dispatchSucceeded: false,
                fault: ex.GetType().Name);
            return -1;
        }
    }

    private static void ApplyContextTransfer(
        Ps5HleCallFrame* frame,
        GuestCpuContinuation target)
    {
        frame->Gpr[(int)CpuRegister.Rax] = target.Rax;
        frame->Gpr[(int)CpuRegister.Rcx] = target.Rcx;
        frame->Gpr[(int)CpuRegister.Rdx] = target.Rdx;
        frame->Gpr[(int)CpuRegister.Rbx] = target.Rbx;
        frame->Gpr[(int)CpuRegister.Rsp] = target.Rsp;
        frame->Gpr[(int)CpuRegister.Rbp] = target.Rbp;
        frame->Gpr[(int)CpuRegister.Rsi] = target.Rsi;
        frame->Gpr[(int)CpuRegister.Rdi] = target.Rdi;
        frame->Gpr[(int)CpuRegister.R8] = target.R8;
        frame->Gpr[(int)CpuRegister.R9] = target.R9;
        frame->Gpr[(int)CpuRegister.R10] = target.R10;
        frame->Gpr[(int)CpuRegister.R11] = target.R11;
        frame->Gpr[(int)CpuRegister.R12] = target.R12;
        frame->Gpr[(int)CpuRegister.R13] = target.R13;
        frame->Gpr[(int)CpuRegister.R14] = target.R14;
        frame->Gpr[(int)CpuRegister.R15] = target.R15;
        frame->Rip = target.Rip;
        frame->Rflags = target.Rflags;
        frame->FsBase = target.FsBase;
        frame->GsBase = target.GsBase;
        frame->FpuControlWord = target.FpuControlWord;
        frame->Mxcsr = target.Mxcsr;
        frame->ControlFlags =
            ControlContextTransfer |
            (target.RestoreFullFpuState
                ? ControlRestoreFullFpuState
                : 0);
    }

    private static void EnsureInitialized()
    {
        if (_exports is not null)
        {
            return;
        }

        lock (InitGate)
        {
            if (_exports is not null)
            {
                return;
            }

            EnsureThreadPoolHeadroom();
            StartPoolHeartbeat();
            StartGpuWaitProbe();

            _exports = SharpEmu.Generated.SysAbiExportRegistry
                .CreateExports(Generation.Gen5)
                .GroupBy(export => export.Nid, StringComparer.Ordinal)
                .ToDictionary(
                    group => group.Key,
                    group => group.First(),
                    StringComparer.Ordinal);
            RefreshPolicy(force: true);
            Console.Error.WriteLine(
                $"[PS5HLE] registered {_exports.Count} SharpEmu Gen5 exports");
        }
    }

    // SharpEmu's GPU wait monitor is a while(true) loop queued onto the
    // thread pool by EnsureGpuWaitMonitor, and it is the only thing that
    // drains DCBs whose waits did not resolve at submit time. A long-running
    // item on a pool worker starves when the guest's own blocking calls hold
    // the rest, and the pool then injects replacements only slowly - which is
    // why merely enabling HleCallTrace, whose two timers keep the pool warm,
    // made the first frame arrive four to eight times sooner. Giving the pool
    // headroom up front removes the dependency on that accident.
    //
    // PS5RECOMP_POOL_MIN_THREADS=0 restores the runtime default for A/B runs.
    private static void EnsureThreadPoolHeadroom()
    {
        const int DefaultMinimum = 64;
        var configured = Environment.GetEnvironmentVariable(
            "PS5RECOMP_POOL_MIN_THREADS");
        var minimum = int.TryParse(configured, out var parsed) && parsed >= 0
            ? parsed
            : DefaultMinimum;
        if (minimum == 0)
        {
            return;
        }

        ThreadPool.GetMinThreads(out var workers, out var completionPorts);
        ThreadPool.GetMaxThreads(out var maxWorkers, out var maxPorts);
        Console.Error.WriteLine(
            $"[PS5HLE] pool min={workers}/{completionPorts} " +
            $"max={maxWorkers}/{maxPorts} requested={minimum}");
        if (workers >= minimum)
        {
            return;
        }

        // SetMinThreads refuses anything above the current maximum, so raise
        // the ceiling first rather than reporting a failure we caused.
        if (maxWorkers < minimum &&
            !ThreadPool.SetMaxThreads(minimum, maxPorts))
        {
            Console.Error.WriteLine(
                $"[PS5HLE] could not raise pool maximum to {minimum}");
            return;
        }

        if (!ThreadPool.SetMinThreads(minimum, completionPorts))
        {
            Console.Error.WriteLine(
                $"[PS5HLE] could not raise pool minimum to {minimum}");
            return;
        }

        Console.Error.WriteLine(
            $"[PS5HLE] pool minimum workers {workers} -> {minimum}");
    }

    // Second probe for the same question. If what unstuck the GPU wait
    // monitor was not pool headroom but simply having a timer callback run
    // on the pool - HleCallTrace has two - then an empty timer reproduces
    // the effect on its own. Off unless PS5RECOMP_POOL_HEARTBEAT_MS is set.
    private static Timer? _poolHeartbeat;

    private static void StartPoolHeartbeat()
    {
        var configured = Environment.GetEnvironmentVariable(
            "PS5RECOMP_POOL_HEARTBEAT_MS");
        if (!int.TryParse(configured, out var period) || period <= 0)
        {
            return;
        }

        _poolHeartbeat = new Timer(static _ => { }, null, period, period);
        Console.Error.WriteLine($"[PS5HLE] pool heartbeat every {period} ms");
    }

    // The one fact that separates "the wait monitor never got a thread"
    // from "it ran and the wait was genuinely unsatisfied". SharpEmu's
    // MonitorGpuWaits keeps a heartbeat for exactly this, but nothing in
    // this build ever read it - LoadProgressDiagnostics only arms on a GTA
    // thread name. Run J established that a timer on its own does not move
    // the run, so sampling from one does not disturb what it measures.
    //
    // PS5RECOMP_GPU_WAIT_PROBE_MS=0 turns it off.
    private static Timer? _gpuWaitProbe;

    private static void StartGpuWaitProbe()
    {
        const int DefaultPeriod = 5_000;
        var configured = Environment.GetEnvironmentVariable(
            "PS5RECOMP_GPU_WAIT_PROBE_MS");
        var period = int.TryParse(configured, out var parsed) && parsed >= 0
            ? parsed
            : DefaultPeriod;
        if (period == 0)
        {
            return;
        }

        _gpuWaitProbe = new Timer(
            static _ =>
            {
                try
                {
                    var monitor = SharpEmu.Libs.Agc.AgcExports
                        .GpuWaitMonitorHeartbeat();
                    var submits = SharpEmu.Libs.Agc.AgcExports
                        .DcbSubmitHeartbeat();
                    Console.Error.WriteLine(
                        $"[PS5HLE] gpu_wait polls={monitor.Count} " +
                        $"since_poll_s={monitor.SecondsSinceLastIteration:0.###} " +
                        $"submits={submits.Count} " +
                        $"since_submit_s={submits.SecondsSinceLastSubmit:0.###}");
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"[PS5HLE] gpu_wait probe: {ex.Message}");
                }
            },
            null,
            period,
            period);
    }

    private static HlePolicy GetPolicy()
    {
        RefreshPolicy(force: false);
        return _policy;
    }

    private static void RefreshPolicy(bool force)
    {
        var now = Environment.TickCount64;
        if (!force && now < Volatile.Read(ref _nextPolicyRefresh))
        {
            return;
        }

        lock (PolicyGate)
        {
            now = Environment.TickCount64;
            if (!force && now < _nextPolicyRefresh)
            {
                return;
            }
            _nextPolicyRefresh = now + 500;

            var configuredPath = Environment.GetEnvironmentVariable(
                "PS5RECOMP_HLE_POLICY");
            var path = string.IsNullOrWhiteSpace(configuredPath)
                ? Path.Combine(
                    AppContext.BaseDirectory,
                    "ps5recomp-hle-policy.json")
                : Path.GetFullPath(configuredPath);
            var permissiveOverride = TryReadBooleanEnvironment(
                "PS5RECOMP_HLE_PERMISSIVE");
            var defaultOverride = TryReadReturnEnvironment(
                "PS5RECOMP_HLE_DEFAULT_RETURN");

            var signature = path;
            if (File.Exists(path))
            {
                var info = new FileInfo(path);
                signature +=
                    $"|{info.LastWriteTimeUtc.Ticks}|{info.Length}";
            }
            signature += $"|{permissiveOverride}|{defaultOverride}";
            if (!force &&
                string.Equals(
                    signature,
                    _policySignature,
                    StringComparison.Ordinal))
            {
                return;
            }

            try
            {
                var permissive = false;
                ulong defaultReturn = 0;
                var rules = new Dictionary<string, ulong>(
                    StringComparer.Ordinal);
                if (File.Exists(path))
                {
                    using var document = JsonDocument.Parse(
                        File.ReadAllText(path));
                    var root = document.RootElement;
                    if (root.ValueKind != JsonValueKind.Object)
                    {
                        throw new InvalidDataException(
                            "HLE policy root must be a JSON object");
                    }

                    if (root.TryGetProperty(
                            "permissive",
                            out var permissiveElement) &&
                        permissiveElement.ValueKind is
                            JsonValueKind.True or JsonValueKind.False)
                    {
                        permissive = permissiveElement.GetBoolean();
                    }
                    if (root.TryGetProperty(
                            "defaultReturn",
                            out var defaultElement) &&
                        TryReadReturn(defaultElement, out var parsedDefault))
                    {
                        defaultReturn = parsedDefault;
                    }
                    if (root.TryGetProperty(
                            "rules",
                            out var rulesElement) &&
                        rulesElement.ValueKind == JsonValueKind.Object)
                    {
                        ReadRules(rulesElement, rules);
                    }

                    foreach (var property in root.EnumerateObject())
                    {
                        if (property.NameEquals("permissive") ||
                            property.NameEquals("defaultReturn") ||
                            property.NameEquals("rules"))
                        {
                            continue;
                        }
                        if (TryReadReturn(
                                property.Value,
                                out var flatReturn))
                        {
                            rules[property.Name] = flatReturn;
                        }
                    }
                }

                if (permissiveOverride.HasValue)
                {
                    permissive = permissiveOverride.Value;
                }
                if (defaultOverride.HasValue)
                {
                    defaultReturn = defaultOverride.Value;
                }

                _policy = new HlePolicy(
                    permissive,
                    defaultReturn,
                    rules);
                _policySignature = signature;
                Console.Error.WriteLine(
                    $"[PS5HLE] policy path={path} " +
                    $"permissive={permissive} " +
                    $"default=0x{defaultReturn:X16} rules={rules.Count}");
            }
            catch (Exception ex)
            {
                _policySignature = signature;
                Console.Error.WriteLine(
                    $"[PS5HLE] policy load failed path={path}: {ex.Message}");
            }
        }
    }

    private static void ReadRules(
        JsonElement rulesElement,
        Dictionary<string, ulong> rules)
    {
        foreach (var property in rulesElement.EnumerateObject())
        {
            if (TryReadReturn(property.Value, out var returnValue))
            {
                rules[property.Name] = returnValue;
            }
        }
    }

    private static bool TryReadReturn(
        JsonElement element,
        out ulong returnValue)
    {
        if (element.ValueKind == JsonValueKind.Object &&
            element.TryGetProperty("return", out var nested))
        {
            return TryReadReturn(nested, out returnValue);
        }
        if (element.ValueKind == JsonValueKind.Number)
        {
            if (element.TryGetUInt64(out returnValue))
            {
                return true;
            }
            if (element.TryGetInt64(out var signed))
            {
                returnValue = unchecked((ulong)signed);
                return true;
            }
        }
        if (element.ValueKind == JsonValueKind.String)
        {
            return TryParseReturn(element.GetString(), out returnValue);
        }

        returnValue = 0;
        return false;
    }

    private static bool TryParseReturn(
        string? text,
        out ulong returnValue)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            returnValue = 0;
            return false;
        }

        text = text.Trim();
        if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            return ulong.TryParse(
                text.AsSpan(2),
                System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture,
                out returnValue);
        }
        if (long.TryParse(
                text,
                System.Globalization.NumberStyles.Integer,
                System.Globalization.CultureInfo.InvariantCulture,
                out var signed))
        {
            returnValue = unchecked((ulong)signed);
            return true;
        }
        return ulong.TryParse(
            text,
            System.Globalization.NumberStyles.Integer,
            System.Globalization.CultureInfo.InvariantCulture,
            out returnValue);
    }

    private static bool? TryReadBooleanEnvironment(string name)
    {
        var value = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(value))
        {
            return null;
        }
        return value.Trim().ToLowerInvariant() switch
        {
            "1" or "true" or "yes" or "on" => true,
            "0" or "false" or "no" or "off" => false,
            _ => null,
        };
    }

    private static ulong? TryReadReturnEnvironment(string name)
    {
        return TryParseReturn(
            Environment.GetEnvironmentVariable(name),
            out var value)
            ? value
            : null;
    }

    private static void ApplyAutoStub(
        Ps5HleCallFrame* frame,
        string nid,
        ulong returnValue,
        string source)
    {
        frame->Gpr[(int)CpuRegister.Rax] = returnValue;
        lock (LoggedAutoStubs)
        {
            if (LoggedAutoStubs.Add(nid))
            {
                Console.Error.WriteLine(
                    $"[PS5HLE] auto_stub nid={nid} source={source} " +
                    $"return=0x{returnValue:X16}");
            }
        }
    }

    private sealed class HlePolicy(
        bool permissive,
        ulong defaultReturn,
        IReadOnlyDictionary<string, ulong> rules)
    {
        public static HlePolicy Disabled { get; } = new(
            false,
            0,
            new Dictionary<string, ulong>(StringComparer.Ordinal));

        public bool Permissive { get; } = permissive;
        public ulong DefaultReturn { get; } = defaultReturn;
        public IReadOnlyDictionary<string, ulong> Rules { get; } = rules;
    }

    private sealed class BridgeGuestThreadScheduler : IGuestThreadScheduler
    {
        public bool SupportsGuestContextTransfer => true;

        public void RegisterGuestThreadContext(
            ulong threadHandle,
            CpuContext context)
        {
        }

        public bool TryStartThread(
            CpuContext creatorContext,
            GuestThreadStartRequest request,
            out string? error)
        {
            error = "Native ps5recomp bridge does not schedule guest threads";
            return false;
        }

        public bool TryJoinThread(
            CpuContext callerContext,
            ulong threadHandle,
            out ulong returnValue,
            out string? error)
        {
            returnValue = 0;
            error = "Native ps5recomp bridge does not schedule guest threads";
            return false;
        }

        public void Pump(CpuContext callerContext, string reason)
        {
        }

        public int WakeBlockedThreads(
            string wakeKey,
            int maxCount = int.MaxValue) => 0;

        public bool TrySetGuestThreadPriority(
            ulong guestThreadHandle,
            int guestPriority) => false;

        public bool TrySetGuestThreadAffinity(
            ulong guestThreadHandle,
            ulong affinityMask) => false;

        public IReadOnlyList<GuestThreadSnapshot> SnapshotThreads() =>
            Array.Empty<GuestThreadSnapshot>();

        public bool TryCallGuestFunction(
            CpuContext callerContext,
            ulong entryPoint,
            ulong arg0,
            ulong arg1,
            ulong stackAddress,
            ulong stackSize,
            string reason,
            out string? error)
        {
            error = "Native ps5recomp bridge cannot call guest functions";
            return false;
        }

        public bool TryCallGuestFunction(
            CpuContext callerContext,
            ulong entryPoint,
            ulong arg0,
            ulong arg1,
            ulong arg2,
            ulong stackAddress,
            ulong stackSize,
            string reason,
            out ulong returnValue,
            out string? error)
        {
            returnValue = 0;
            error = "Native ps5recomp bridge cannot call guest functions";
            return false;
        }

        public bool TryCallGuestFunction(
            CpuContext callerContext,
            ulong entryPoint,
            ulong arg0,
            ulong arg1,
            ulong arg2,
            ulong arg3,
            ulong stackAddress,
            ulong stackSize,
            string reason,
            out ulong returnValue,
            out string? error)
        {
            returnValue = 0;
            error = "Native ps5recomp bridge cannot call guest functions";
            return false;
        }

        // The recompiled title runs its own code rather than being stepped
        // through an interpreter, so there is no import safe point for an
        // exception to wait at - it has already been delivered or it never
        // was.
        public bool HasPendingGuestExceptionForCurrentThread() => false;

        public bool TryCallGuestContinuation(
            CpuContext callerContext,
            GuestCpuContinuation continuation,
            string reason,
            out string? error)
        {
            error = "Native ps5recomp bridge cannot call guest continuations";
            return false;
        }

        public bool TryRaiseGuestException(
            CpuContext callerContext,
            ulong threadHandle,
            ulong handler,
            int exceptionType,
            out string? error)
        {
            error = "Native ps5recomp bridge cannot raise guest exceptions";
            return false;
        }
    }

    private static string ReadUtf8(byte* value)
    {
        if (value is null)
        {
            return string.Empty;
        }

        const int MaximumLength = 32 * 1024;
        var length = 0;
        while (length < MaximumLength && value[length] != 0)
        {
            length++;
        }
        return length == 0
            ? string.Empty
            : Encoding.UTF8.GetString(new ReadOnlySpan<byte>(value, length));
    }
}
