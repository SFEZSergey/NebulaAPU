// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Diagnostics;
using Ps5Recomp.Cli;

return ProgramEntry.Run(args);

internal static class ProgramEntry
{
    public static int Run(string[] args)
    {
        if (!Arguments.TryParse(args, out var options))
        {
            Arguments.PrintUsage();
            return 1;
        }

        try
        {
            return options.Command switch
            {
                Command.SelfTest => RunSelfTest(options),
                Command.Inspect => RunInspect(options),
                Command.Link => RunLink(options),
                Command.Build => RunBuild(options),
                Command.HleAudit => RunHleAudit(options),
                _ => RunCompile(options),
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ps5recomp failed: {ex.Message}");
            return 2;
        }
    }

    private static int RunCompile(Arguments options)
    {
        var frontend = CompilerFrontend.Compile(
            options,
            options.InputPath!,
            options.OutputDirectory);
        var runner = NativeRunnerBuilder.Build(
            options.OutputDirectory,
            options.ExecutableName,
            options.Cxx);

        PrintFrontendOutput(frontend);
        PrintPackage(options.OutputDirectory);
        Console.WriteLine($"Windows PE:     {runner.ExecutablePath}");
        Console.WriteLine($"Runtime DLL:    {runner.RuntimePath}");
        if (!string.IsNullOrWhiteSpace(runner.CompilerOutput))
        {
            Console.WriteLine(runner.CompilerOutput);
        }

        return 0;
    }

    private static int RunInspect(Arguments options)
    {
        var frontend = CompilerFrontend.Inspect(options);
        PrintFrontendOutput(frontend);
        return 0;
    }

    private static int RunLink(Arguments options)
    {
        var packageDirectory = Path.GetFullPath(options.InputPath!);
        var runner = NativeRunnerBuilder.Build(
            packageDirectory,
            options.ExecutableName,
            options.Cxx);
        PrintPackage(packageDirectory);
        Console.WriteLine($"Windows PE:     {runner.ExecutablePath}");
        Console.WriteLine($"Runtime DLL:    {runner.RuntimePath}");
        if (!string.IsNullOrWhiteSpace(runner.CompilerOutput))
        {
            Console.WriteLine(runner.CompilerOutput);
        }
        return 0;
    }

    private static int RunBuild(Arguments options)
    {
        var buildOptions = new BuildOptions
        {
            InputPath = options.InputPath!,
            OutputDirectory = options.OutputDirectory,
            ExecutableName = options.ExecutableName,
            Cxx = options.Cxx,
            MaxInstructions = options.MaximumInstructions,
            FullBuild = true,
            Ps5NamesPath = options.Ps5NamesPath,
            SharpEmuSourcePath = options.SharpEmuSourcePath,
            HleSourcePaths = options.HleSourcePaths.ToList(),
            ProfilePath = options.ProfilePath,
            App0Directory = options.App0Directory,
        };
        return BuildOrchestrator.Run(buildOptions);
    }

    private static int RunHleAudit(Arguments options)
    {
        var packageDirectory = Path.GetFullPath(options.InputPath!);
        var outputDirectory = Path.GetFullPath(options.OutputDirectory);
        var sharpEmuSource = !string.IsNullOrWhiteSpace(
            options.SharpEmuSourcePath)
            ? Path.GetFullPath(options.SharpEmuSourcePath)
            : LocalPaths.Dev("sharpemu", "src");
        var ps5Names = !string.IsNullOrWhiteSpace(options.Ps5NamesPath)
            ? Path.GetFullPath(options.Ps5NamesPath)
            : LocalPaths.Dev("sharpemu", "scripts", "ps5_names.txt");
        var result = Ps5Recomp.Cli.HleGenerator.HleRegistryGenerator.Generate(
            packageDirectory,
            sharpEmuSource,
            File.Exists(ps5Names) ? ps5Names : null,
            outputDirectory,
            Ps5Recomp.Cli.HleGenerator.HleKnownImplementations.GetKnown(),
            options.HleSourcePaths.Count != 0
                ? options.HleSourcePaths
                : Ps5Recomp.Cli.HleGenerator.HleExternalCatalogLoader
                    .FindDefaultSources());
        Console.WriteLine(
            $"HLE inventory: {result.TotalReferences} references, " +
            $"{result.UniqueNids} unique NIDs");
        Console.WriteLine(
            $"Named: {result.NamedCount}; implemented: " +
            $"{result.ImplementedCount}; SharpEmu bridge: " +
            $"{result.SharpEmuCount}; stubbed: {result.StubbedCount}; " +
            $"missing: {result.MissingCount}");
        Console.WriteLine(
            $"SharpEmu exports scanned: {result.SharpEmuExportCount}");
        Console.WriteLine(
            $"External candidates: {result.ExternalCandidateCount}; " +
            $"package matches: {result.ExternalMatchCount}; " +
            $"conflicts: {result.ExternalConflictCount}");
        Console.WriteLine($"Registry:        {result.SourcePath}");
        Console.WriteLine($"Missing HLE:     {result.MissingPath}");
        Console.WriteLine($"Import usage:    {result.ReportPath}");
        Console.WriteLine($"ABI report:      {result.AbiReportPath}");
        Console.WriteLine($"Conflicts:       {result.ConflictsPath}");
        Console.WriteLine(
            $"Work queue:      {result.WorkQueuePath} " +
            $"({result.WorkQueueCount} missing, " +
            $"{result.WorkQueueKytyCount} Kyty-backed)");
        return 0;
    }

    private static int RunSelfTest(Arguments options)
    {
        var root = Path.GetFullPath(options.OutputDirectory);
        Directory.CreateDirectory(root);
        Ps5Recomp.Cli.HleGenerator.HleExternalCatalogLoader.RunSelfTest(
            root,
            Ps5Recomp.Cli.HleGenerator.HleRegistryGenerator.ComputeNid);
        Console.WriteLine("HLE catalog self-test: PASS");
        Ps5Recomp.Cli.HleGenerator.HleRegistryGenerator
            .RunWorkQueueSelfTest();
        Console.WriteLine("HLE work queue self-test: PASS");
        var inputPath = Path.Combine(root, "minimal-ps5.elf");
        var packageDirectory = Path.Combine(root, "package");
        File.WriteAllBytes(inputPath, MinimalPs5Elf.Create());

        var frontend = CompilerFrontend.Compile(options, inputPath, packageDirectory);
        var runner = NativeRunnerBuilder.Build(
            packageDirectory,
            options.ExecutableName,
            options.Cxx);
        PrintFrontendOutput(frontend);
        PrintPackage(packageDirectory);
        Console.WriteLine($"Windows PE:     {runner.ExecutablePath}");
        Console.WriteLine($"Runtime DLL:    {runner.RuntimePath}");

        var startInfo = new ProcessStartInfo
        {
            FileName = runner.ExecutablePath,
            WorkingDirectory = packageDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        startInfo.ArgumentList.Add("--package");
        startInfo.ArgumentList.Add(packageDirectory);
        startInfo.ArgumentList.Add("--expect-return");
        startInfo.ArgumentList.Add(MinimalPs5Elf.ExpectedReturnValue.ToString());
        startInfo.ArgumentList.Add("--headless");

        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException("Could not start the generated native runner.");
        var standardOutput = process.StandardOutput.ReadToEnd();
        var standardError = process.StandardError.ReadToEnd();
        if (!process.WaitForExit(10_000))
        {
            process.Kill(entireProcessTree: true);
            throw new TimeoutException("Generated native runner did not exit within 10 seconds.");
        }

        Console.Write(standardOutput);
        Console.Error.Write(standardError);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Generated native runner exited with code {process.ExitCode}.");
        }

        Console.WriteLine("Self-test:      PASS");
        return 0;
    }

    private static void PrintPackage(string packageDirectory)
    {
        var fullDirectory = Path.GetFullPath(packageDirectory);
        var manifestPath = Path.Combine(fullDirectory, "manifest.json");
        var imagePath = Path.Combine(fullDirectory, "image.bin");
        if (!File.Exists(manifestPath) || !File.Exists(imagePath))
        {
            throw new InvalidDataException(
                "Compiler frontend did not produce manifest.json and image.bin.");
        }

        Console.WriteLine($"Static package: {fullDirectory}");
        Console.WriteLine($"Manifest:       {manifestPath}");
        Console.WriteLine($"Memory image:   {imagePath}");
    }

    private static void PrintFrontendOutput(CompilerFrontendResult result)
    {
        if (!string.IsNullOrWhiteSpace(result.StandardOutput))
        {
            Console.Write(result.StandardOutput);
        }
        if (!string.IsNullOrWhiteSpace(result.StandardError))
        {
            Console.Error.Write(result.StandardError);
        }
    }
}

internal enum Command
{
    Compile,
    Inspect,
    Link,
    Build,
    HleAudit,
    SelfTest,
}

internal sealed class Arguments
{
    public Command Command { get; internal set; } = Command.Compile;

    public string? InputPath { get; internal set; }

    public string OutputDirectory { get; internal set; } =
        Path.Combine(Environment.CurrentDirectory, "artifacts", "output");

    public string? App0Directory { get; internal set; }

    public string ExecutableName { get; internal set; } = "Game.exe";

    public string Cxx { get; internal set; } =
        Environment.GetEnvironmentVariable("CXX") ?? "g++";

    public string DotnetPath { get; internal set; } =
        Environment.GetEnvironmentVariable("PS5RECOMP_DOTNET") ??
        DefaultFile(
            LocalPaths.Dev("dotnet-sdk-10.0.103", "dotnet.exe"),
            "dotnet");

    public string FrontendPath { get; internal set; } =
        Environment.GetEnvironmentVariable("PS5RECOMP_FRONTEND") ??
        LocalPaths.Dev("sharpemu", "artifacts", "bin", "Debug", "net10.0", "SharpEmu.Tools.Recompiler.dll");

    public string? Ps5NamesPath { get; internal set; }

    public string? SharpEmuSourcePath { get; internal set; }

    public List<string> HleSourcePaths { get; } = [];

    public string? ProfilePath { get; internal set; }

    public bool AllowPartialControlFlow { get; internal set; }

    public int MaximumInstructions { get; internal set; } = 2_000_000;

    public static bool TryParse(string[] args, out Arguments options)
    {
        options = new Arguments();
        if (args.Length == 0)
        {
            return false;
        }

        var index = 0;
        options.Command = args[index].ToLowerInvariant() switch
        {
            "compile" => Command.Compile,
            "inspect" => Command.Inspect,
            "link" => Command.Link,
            "build" => Command.Build,
            "hle-audit" => Command.HleAudit,
            "self-test" => Command.SelfTest,
            _ => (Command)(-1),
        };
        if ((int)options.Command < 0)
        {
            return false;
        }

        index++;
        if (options.Command is Command.Compile or Command.Inspect or
            Command.Link or Command.Build or Command.HleAudit)
        {
            if (index >= args.Length || args[index].StartsWith("-", StringComparison.Ordinal))
            {
                return false;
            }

            options.InputPath = args[index++];
        }

        while (index < args.Length)
        {
            var argument = args[index++];
            switch (argument)
            {
                case "-o":
                case "--output":
                    if (!TakeValue(args, ref index, out var output))
                    {
                        return false;
                    }
                    options.OutputDirectory = output;
                    break;

                case "--app0":
                    if (!TakeValue(args, ref index, out var app0))
                    {
                        return false;
                    }
                    options.App0Directory = app0;
                    break;

                case "--exe-name":
                    if (!TakeValue(args, ref index, out var executableName) ||
                        Path.GetFileName(executableName) != executableName ||
                        !executableName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                    {
                        return false;
                    }
                    options.ExecutableName = executableName;
                    break;

                case "--cxx":
                    if (!TakeValue(args, ref index, out var cxx))
                    {
                        return false;
                    }
                    options.Cxx = cxx;
                    break;

                case "--dotnet":
                    if (!TakeValue(args, ref index, out var dotnet))
                    {
                        return false;
                    }
                    options.DotnetPath = dotnet;
                    break;

                case "--frontend":
                    if (!TakeValue(args, ref index, out var frontend))
                    {
                        return false;
                    }
                    options.FrontendPath = frontend;
                    break;

                case "--max-instructions":
                    if (!TakeValue(args, ref index, out var maximumText) ||
                        !int.TryParse(maximumText, out var maximum) ||
                        maximum <= 0)
                    {
                        return false;
                    }
                    options.MaximumInstructions = maximum;
                    break;

                case "--allow-partial-cfg":
                    options.AllowPartialControlFlow = true;
                    break;

                case "--ps5-names":
                    if (!TakeValue(args, ref index, out var ps5Names))
                    {
                        return false;
                    }
                    options.Ps5NamesPath = ps5Names;
                    break;

                case "--sharpemu-source":
                    if (!TakeValue(args, ref index, out var sharpEmuSource))
                    {
                        return false;
                    }
                    options.SharpEmuSourcePath = sharpEmuSource;
                    break;

                case "--hle-source":
                    if (!TakeValue(args, ref index, out var hleSource))
                    {
                        return false;
                    }
                    options.HleSourcePaths.Add(hleSource);
                    break;

                case "--profile":
                    if (!TakeValue(args, ref index, out var profile))
                    {
                        return false;
                    }
                    options.ProfilePath = profile;
                    break;

                default:
                    return false;
            }
        }

        return options.Command switch
        {
            Command.Compile => options.App0Directory is null,
            Command.Inspect => !options.AllowPartialControlFlow,
            Command.Link => options.App0Directory is null &&
                            !options.AllowPartialControlFlow,
            Command.Build => true,
            Command.HleAudit => options.App0Directory is null &&
                                !options.AllowPartialControlFlow,
            Command.SelfTest => options.App0Directory is null &&
                                !options.AllowPartialControlFlow,
            _ => false,
        };
    }

    public static void PrintUsage()
    {
        Console.Error.WriteLine(
            "Usage: ps5recomp compile <eboot.bin|elf> -o <directory> " +
            "[--exe-name Game.exe] [--cxx g++] [--frontend <dll>] " +
            "[--max-instructions N] " +
            "[--allow-partial-cfg]");
        Console.Error.WriteLine(
            "       ps5recomp inspect <eboot.bin|elf> -o <directory> " +
            "[--app0 <directory>] [--max-instructions N]");
        Console.Error.WriteLine(
            "       ps5recomp link <package-directory> " +
            "[--exe-name Game.exe] [--cxx g++]");
        Console.Error.WriteLine(
            "       ps5recomp build <eboot.bin|elf> -o <directory> " +
            "[--exe-name Game.exe] [--cxx g++] [--ps5-names <path>] " +
            "[--sharpemu-source <directory>] [--profile <path>] " +
            "[--hle-source <directory>]... " +
            "[--max-instructions N]");
        Console.Error.WriteLine(
            "       ps5recomp hle-audit <package-directory> -o <directory> " +
            "[--ps5-names <path>] [--sharpemu-source <directory>] " +
            "[--hle-source <directory>]...");
        Console.Error.WriteLine(
            "       ps5recomp self-test [-o <directory>] " +
            "[--exe-name MinimalGame.exe] [--cxx g++]");
    }

    private static bool TakeValue(
        IReadOnlyList<string> args,
        ref int index,
        out string value)
    {
        value = string.Empty;
        if (index >= args.Count || string.IsNullOrWhiteSpace(args[index]))
        {
            return false;
        }

        value = args[index++];
        return true;
    }

    private static string DefaultFile(string preferred, string fallback)
    {
        return File.Exists(preferred) ? preferred : fallback;
    }
}
