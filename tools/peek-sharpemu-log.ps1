# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [int]$GuiProcessId,
    [ValidateRange(50, 2000)]
    [int]$HoldMilliseconds = 350,
    [string]$OutputPath =
        "$(if ($env:NEBULA_DEV_ROOT) { $env:NEBULA_DEV_ROOT } else { 'C:\dev' })\ps5recomp\artifacts\reference\sharpemu-beta5-live-peek.log",
    [switch]$Capture
)

$ErrorActionPreference = "Stop"

if (-not ("SharpEmuPipePeek" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class SharpEmuPipePeek
{
    private const int ProcessHandleInformation = 51;
    private const uint ProcessDuplicateHandle = 0x0040;
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessSuspendResume = 0x0800;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint DuplicateSameAccess = 0x00000002;
    private const uint FileTypePipe = 3;

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessHandleEntry
    {
        public UIntPtr HandleValue;
        public UIntPtr HandleCount;
        public UIntPtr PointerCount;
        public uint GrantedAccess;
        public uint ObjectTypeIndex;
        public uint HandleAttributes;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoStatusBlock
    {
        public IntPtr Status;
        public UIntPtr Information;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FilePipeLocalInformation
    {
        public uint NamedPipeType;
        public uint NamedPipeConfiguration;
        public uint MaximumInstances;
        public uint CurrentInstances;
        public uint InboundQuota;
        public uint ReadDataAvailable;
        public uint OutboundQuota;
        public uint WriteQuotaAvailable;
        public uint NamedPipeState;
        public uint NamedPipeEnd;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DuplicateHandle(
        IntPtr sourceProcess,
        IntPtr sourceHandle,
        IntPtr targetProcess,
        out IntPtr targetHandle,
        uint access,
        bool inherit,
        uint options);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetFileType(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool PeekNamedPipe(
        IntPtr pipe,
        byte[] buffer,
        uint size,
        out uint bytesRead,
        out uint totalAvailable,
        out uint bytesLeft);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationProcess(
        IntPtr process,
        int informationClass,
        IntPtr information,
        int length,
        out int returnLength);

    [DllImport("ntdll.dll")]
    private static extern int NtSuspendProcess(IntPtr process);

    [DllImport("ntdll.dll")]
    private static extern int NtResumeProcess(IntPtr process);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationFile(
        IntPtr file,
        out IoStatusBlock ioStatus,
        out FilePipeLocalInformation information,
        uint length,
        int informationClass);

    public static void Resume(int processId)
    {
        IntPtr process = OpenProcess(
            ProcessSuspendResume | ProcessQueryLimitedInformation,
            false,
            processId);
        if (process == IntPtr.Zero)
        {
            throw new Win32Exception(
                Marshal.GetLastWin32Error(),
                "OpenProcess failed");
        }
        try
        {
            NtResumeProcess(process);
        }
        finally
        {
            CloseHandle(process);
        }
    }

    public static string Capture(int processId, int holdMilliseconds)
    {
        IntPtr process = OpenProcess(
            ProcessDuplicateHandle |
            ProcessQueryInformation |
            ProcessSuspendResume |
            ProcessQueryLimitedInformation,
            false,
            processId);
        if (process == IntPtr.Zero)
        {
            throw new Win32Exception(
                Marshal.GetLastWin32Error(),
                "OpenProcess failed");
        }

        var pipeHandles = new List<IntPtr>();
        var pipeValues = new List<ulong>();
        bool suspended = false;
        try
        {
            // Recover from an interrupted previous probe before querying handles.
            NtResumeProcess(process);

            foreach (ProcessHandleEntry entry in EnumerateHandles(process))
            {
                if ((entry.GrantedAccess & 1) == 0)
                {
                    continue;
                }

                ulong handleValue = entry.HandleValue.ToUInt64();
                if (!DuplicateHandle(
                        process,
                        new IntPtr(unchecked((long)handleValue)),
                        GetCurrentProcess(),
                        out IntPtr duplicate,
                        0,
                        false,
                        DuplicateSameAccess))
                {
                    continue;
                }

                if (GetFileType(duplicate) == FileTypePipe)
                {
                    pipeHandles.Add(duplicate);
                    pipeValues.Add(handleValue);
                }
                else
                {
                    CloseHandle(duplicate);
                }
            }

            int status = NtSuspendProcess(process);
            if (status != 0)
            {
                throw new InvalidOperationException(
                    $"NtSuspendProcess failed: 0x{status:X8}");
            }
            suspended = true;
            var watchdog = new Thread(() =>
            {
                Thread.Sleep(3000);
                NtResumeProcess(process);
            })
            {
                IsBackground = true,
            };
            watchdog.Start();
            Thread.Sleep(Math.Max(50, holdMilliseconds));

            var output = new StringBuilder();
            for (int index = 0; index < pipeHandles.Count; index++)
            {
                IntPtr pipe = pipeHandles[index];
                int queryStatus = NtQueryInformationFile(
                        pipe,
                        out _,
                        out FilePipeLocalInformation pipeInfo,
                        (uint)Marshal.SizeOf<FilePipeLocalInformation>(),
                        24);
                if (queryStatus != 0 || pipeInfo.ReadDataAvailable == 0)
                {
                    continue;
                }

                byte[] data = new byte[
                    Math.Min(pipeInfo.ReadDataAvailable, 65536)];
                if (!PeekNamedPipe(
                        pipe,
                        data,
                        (uint)data.Length,
                        out uint bytesRead,
                        out uint available,
                        out _) ||
                    bytesRead == 0)
                {
                    continue;
                }

                output.AppendLine(
                    $"--- pipe handle=0x{pipeValues[index]:X} " +
                    $"bytes={bytesRead} available={available} ---");
                output.AppendLine(
                    Encoding.UTF8.GetString(data, 0, (int)bytesRead));
            }

            return output.ToString();
        }
        finally
        {
            if (suspended)
            {
                NtResumeProcess(process);
            }
            foreach (IntPtr pipe in pipeHandles)
            {
                CloseHandle(pipe);
            }
            CloseHandle(process);
        }
    }

    private static IEnumerable<ProcessHandleEntry> EnumerateHandles(
        IntPtr process)
    {
        int size = 65536;
        IntPtr buffer = IntPtr.Zero;
        try
        {
            while (true)
            {
                buffer = Marshal.AllocHGlobal(size);
                int status = NtQueryInformationProcess(
                    process,
                    ProcessHandleInformation,
                    buffer,
                    size,
                    out int needed);
                if (status == 0)
                {
                    break;
                }

                Marshal.FreeHGlobal(buffer);
                buffer = IntPtr.Zero;
                if (needed <= size)
                {
                    throw new InvalidOperationException(
                        $"NtQueryInformationProcess failed: 0x{status:X8}");
                }
                size = Math.Max(size * 2, needed + 65536);
            }

            long count = Marshal.ReadInt64(buffer);
            int entrySize = Marshal.SizeOf<ProcessHandleEntry>();
            IntPtr entryAddress = IntPtr.Add(buffer, IntPtr.Size * 2);
            for (long index = 0; index < count; index++)
            {
                var entry =
                    Marshal.PtrToStructure<ProcessHandleEntry>(entryAddress);
                yield return entry;
                entryAddress = IntPtr.Add(entryAddress, entrySize);
            }
        }
        finally
        {
            if (buffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
    }
}
'@
}

if ($GuiProcessId -le 0) {
    $processes = @(
        Get-Process -Name SharpEmu -ErrorAction Stop |
            Sort-Object StartTime)
    if ($processes.Count -lt 2) {
        throw "A SharpEmu GUI and child process were not both found."
    }
    $GuiProcessId = $processes[0].Id
}

if (-not $Capture) {
    [SharpEmuPipePeek]::Resume($GuiProcessId)
    Write-Output "gui_pid=$GuiProcessId resumed=true"
    exit 0
}

$captured = [SharpEmuPipePeek]::Capture(
    $GuiProcessId,
    $HoldMilliseconds)
if ([string]::IsNullOrWhiteSpace($captured)) {
    Write-Output "gui_pid=$GuiProcessId pipe_data=none"
    exit 2
}

$directory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $directory -Force | Out-Null
[IO.File]::WriteAllText(
    $OutputPath,
    $captured,
    [Text.UTF8Encoding]::new($false))

Write-Output "gui_pid=$GuiProcessId bytes=$((Get-Item -LiteralPath $OutputPath).Length)"
Write-Output "output=$OutputPath"
Get-Content -LiteralPath $OutputPath
