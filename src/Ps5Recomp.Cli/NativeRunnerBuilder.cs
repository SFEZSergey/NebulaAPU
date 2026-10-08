// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Diagnostics;

namespace Ps5Recomp.Cli;

internal sealed record NativeRunnerBuildResult(
    string ExecutablePath,
    string RuntimePath,
    string CompilerOutput);

internal static class NativeRunnerBuilder
{
    private const int BuildTimeoutMilliseconds = 60_000;

    public static NativeRunnerBuildResult Build(
        string packageDirectory,
        string executableName,
        string cxx)
    {
        var fullPackageDirectory = Path.GetFullPath(packageDirectory);
        var generatedDirectory = Path.Combine(fullPackageDirectory, "generated");
        var generatedMetadata =
            RuntimeMetadataGenerator.Generate(fullPackageDirectory);
        var generatedSource = generatedMetadata.SourcePath;
        var generatedHeader = generatedMetadata.HeaderPath;
        var generatedHleDirectory = Path.Combine(
            generatedDirectory,
            "hle");
        var generatedHleSource = Path.Combine(
            generatedHleDirectory,
            "hle_registry.generated.cpp");
        var generatedHleHeader = Path.Combine(
            generatedHleDirectory,
            "hle_registry.generated.h");
        var generatedHleBindings = Path.Combine(
            generatedHleDirectory,
            "hle_bindings.generated.inc");
        var hasGeneratedHle =
            File.Exists(generatedHleSource) &&
            File.Exists(generatedHleHeader) &&
            File.Exists(generatedHleBindings);
        var runtimeSource = Path.Combine(
            AppContext.BaseDirectory,
            "runtime",
            "native",
            "ps5rt_runner.cpp");
        var hleImplSource = Path.Combine(
            AppContext.BaseDirectory,
            "runtime",
            "native",
            "ps5rt_hle_impl.cpp");
        var runtimeLibrarySource = Path.Combine(
            AppContext.BaseDirectory,
            "runtime",
            "native",
            "ps5runtime.cpp");
        var runtimeHeader = Path.Combine(
            AppContext.BaseDirectory,
            "runtime",
            "native",
            "ps5rt_api.h");
        var hleImplObject = Path.Combine(
            generatedDirectory,
            "ps5rt_hle_impl.o");
        var executablePath = Path.Combine(fullPackageDirectory, executableName);
        var runtimePath = Path.Combine(fullPackageDirectory, "Ps5Runtime.dll");
        var runtimeImportLibrary = Path.Combine(
            fullPackageDirectory,
            "libPs5Runtime.dll.a");
        var hleBridgePath = InstallHleBridge(fullPackageDirectory);
        var gpuBridgePath = InstallGpuBridge(fullPackageDirectory);

        RequireFile(generatedSource);
        RequireFile(generatedHeader);
        RequireFile(runtimeSource);
        RequireFile(hleImplSource);
        RequireFile(runtimeLibrarySource);
        RequireFile(runtimeHeader);

        var runtimeArguments = new List<string>
        {
            "-std=c++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-DPS5RT_BUILD_DLL",
            "-DWIN32_LEAN_AND_MEAN",
            "-DNOMINMAX",
            "-shared",
            "-static-libgcc",
            "-static-libstdc++",
            runtimeLibrarySource,
            "-o",
            runtimePath,
            $"-Wl,--out-implib,{runtimeImportLibrary}",
            "-ld3d12",
            "-ldxgi",
            "-lole32",
        };
        var runtimeOutput = RunCompiler(
            cxx,
            fullPackageDirectory,
            runtimeArguments,
            "runtime DLL");

        var hleImplArguments = new List<string>
        {
            "-std=c++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-DWIN32_LEAN_AND_MEAN",
            "-DNOMINMAX",
            "-mfsgsbase",
            "-fno-exceptions",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
            "-c",
            hleImplSource,
            "-o",
            hleImplObject,
        };
        var hleImplOutput = RunCompiler(
            cxx,
            fullPackageDirectory,
            hleImplArguments,
            "HLE implementation");

        var runnerArguments = new List<string>
        {
            "-std=c++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-DWIN32_LEAN_AND_MEAN",
            "-DNOMINMAX",
            "-static-libgcc",
            "-static-libstdc++",
            "-mfsgsbase",
        };
        if (hasGeneratedHle)
        {
            runnerArguments.Add("-DPS5RT_HAS_GENERATED_HLE=1");
        }
        runnerArguments.Add(runtimeSource);
        runnerArguments.Add(hleImplObject);
        runnerArguments.Add(generatedSource);
        if (hasGeneratedHle)
        {
            runnerArguments.Add(generatedHleSource);
        }
        runnerArguments.Add(runtimeImportLibrary);
        runnerArguments.Add("-I");
        runnerArguments.Add(generatedDirectory);
        if (hasGeneratedHle)
        {
            runnerArguments.Add("-I");
            runnerArguments.Add(generatedHleDirectory);
        }
        runnerArguments.Add("-o");
        runnerArguments.Add(executablePath);
        var runnerOutput = RunCompiler(
            cxx,
            fullPackageDirectory,
            runnerArguments,
            "native runner");

        var compilerOutput = string.Join(
            Environment.NewLine,
            new[]
            {
                hleBridgePath is null
                    ? "SharpEmu HLE bridge: not found; bridge NIDs remain unresolved."
                    : $"SharpEmu HLE bridge: {hleBridgePath}",
                gpuBridgePath is null
                    ? "PS5 GPU bridge: not found; guest shader translation is unavailable."
                    : $"PS5 GPU bridge: {gpuBridgePath}",
                runtimeOutput,
                hleImplOutput,
                runnerOutput,
            }
                .Where(text => !string.IsNullOrWhiteSpace(text))
                .Select(text => text.TrimEnd()));

        RequireFile(executablePath);
        RequireFile(runtimePath);
        return new NativeRunnerBuildResult(
            executablePath,
            runtimePath,
            compilerOutput);
    }

    private static string? InstallHleBridge(string packageDirectory)
    {
        var configured = Environment.GetEnvironmentVariable(
            "PS5RECOMP_HLE_BRIDGE");
        var candidates = new List<string>();
        if (!string.IsNullOrWhiteSpace(configured))
        {
            candidates.Add(Path.GetFullPath(configured));
        }
        candidates.Add(Path.Combine(
            AppContext.BaseDirectory,
            "Ps5HleBridge.dll"));

        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            candidates.Add(Path.Combine(
                directory.FullName,
                "artifacts",
                "hle-bridge",
                "win-x64",
                "Ps5HleBridge.dll"));
            if (File.Exists(Path.Combine(
                    directory.FullName,
                    "bridge",
                    "SharpEmu.HleBridge",
                    "SharpEmu.HleBridge.csproj")))
            {
                break;
            }
            directory = directory.Parent;
        }

        var source = candidates
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault(File.Exists);
        if (source is null)
        {
            return null;
        }

        var destination = Path.Combine(
            packageDirectory,
            "Ps5HleBridge.dll");
        if (!string.Equals(
                Path.GetFullPath(source),
                Path.GetFullPath(destination),
                StringComparison.OrdinalIgnoreCase))
        {
            File.Copy(source, destination, overwrite: true);
        }
        return destination;
    }

    private static string? InstallGpuBridge(string packageDirectory)
    {
        var configured = Environment.GetEnvironmentVariable(
            "PS5RECOMP_GPU_BRIDGE");
        var candidates = new List<string>();
        if (!string.IsNullOrWhiteSpace(configured))
        {
            candidates.Add(Path.GetFullPath(configured));
        }
        candidates.Add(Path.Combine(
            AppContext.BaseDirectory,
            "Ps5GpuBridge.dll"));

        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            candidates.Add(Path.Combine(
                directory.FullName,
                "artifacts",
                "gpu-bridge",
                "win-x64",
                "Ps5GpuBridge.dll"));
            if (File.Exists(Path.Combine(
                    directory.FullName,
                    "bridge",
                    "Ps5Recomp.GpuBridge",
                    "Ps5Recomp.GpuBridge.csproj")))
            {
                break;
            }
            directory = directory.Parent;
        }

        var source = candidates
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault(File.Exists);
        if (source is null)
        {
            return null;
        }

        var destination = Path.Combine(
            packageDirectory,
            "Ps5GpuBridge.dll");
        if (!string.Equals(
                Path.GetFullPath(source),
                Path.GetFullPath(destination),
                StringComparison.OrdinalIgnoreCase))
        {
            File.Copy(source, destination, overwrite: true);
        }
        return destination;
    }

    private static string RunCompiler(
        string cxx,
        string workingDirectory,
        IReadOnlyList<string> arguments,
        string description)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = cxx,
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException($"Could not start C++ compiler '{cxx}'.");
        var standardOutput = process.StandardOutput.ReadToEndAsync();
        var standardError = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(BuildTimeoutMilliseconds))
        {
            process.Kill(entireProcessTree: true);
            throw new TimeoutException(
                $"{description} compilation exceeded " +
                $"{BuildTimeoutMilliseconds / 1000} seconds.");
        }

        Task.WaitAll(standardOutput, standardError);
        var compilerOutput = string.Join(
            Environment.NewLine,
            new[] { standardOutput.Result, standardError.Result }
                .Where(text => !string.IsNullOrWhiteSpace(text))
                .Select(text => text.TrimEnd()));
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"{description} compiler exited with code {process.ExitCode}." +
                Environment.NewLine +
                compilerOutput);
        }
        return compilerOutput;
    }

    private static void RequireFile(string path)
    {
        if (!File.Exists(path))
        {
            throw new FileNotFoundException("Required native runner input was not found.", path);
        }
    }
}
