// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Ps5Recomp.Cli;

/// <summary>
/// Automated build pipeline: compile/export -> HLE inventory -> link -> verify.
/// Handles caching, resume, and failure bundle generation.
/// Inspired by N64Recomp file-comparison dedup and XenonRecomp two-tool architecture.
/// </summary>
internal static class BuildOrchestrator
{
    private const string ManifestFile = "build-manifest.json";
    private const string TraceFile = "build-trace.jsonl";

    public static int Run(BuildOptions options)
    {
        var startTime = Stopwatch.StartNew();
        var workDir = Path.GetFullPath(options.OutputDirectory);
        Directory.CreateDirectory(workDir);

        var manifestPath = Path.Combine(workDir, ManifestFile);
        var manifest = LoadOrCreateManifest(manifestPath);
        var tracePath = Path.Combine(workDir, TraceFile);
        using var traceWriter = new StreamWriter(tracePath, append: false, encoding: Encoding.UTF8);

        try
        {
            // Stage 1: compile/export. The frontend already performs inspection,
            // so running inspect first would repeat the full CFG analysis.
            if (!manifest.HasStage("compile"))
            {
                TraceEvent(traceWriter, "stage_start", "compile");
                var packageDir = Path.Combine(workDir, "package");
                var compileResult = CompilerFrontend.Compile(
                    CreateArgs(options, Command.Compile, packageDir),
                    options.InputPath,
                    packageDir);
                PrintOutput(compileResult);
                manifest.CompleteStage("compile", packageDir);
                SaveManifest(manifestPath, manifest);
                TraceEvent(traceWriter, "stage_complete", "compile");
            }

            // Stage 1b: compile the dependent modules the export listed in
            // modules.json. Each becomes an additional guest image, which is
            // what resolves the bulk of the imports.
            if (!manifest.HasStage("modules"))
            {
                TraceEvent(traceWriter, "stage_start", "modules");
                var packageDir = Path.Combine(workDir, "package");
                var moduleCount = CompileModules(options, packageDir, traceWriter);
                manifest.CompleteStage("modules", moduleCount.ToString());
                SaveManifest(manifestPath, manifest);
                TraceEvent(traceWriter, "stage_complete", "modules");
            }

            // Stage 2: Generate HLE registry
            if (!manifest.HasStage("hle_generate"))
            {
                TraceEvent(traceWriter, "stage_start", "hle_generate");
                var packageDir = Path.Combine(workDir, "package");
                var hleDir = Path.Combine(packageDir, "generated", "hle");
                var knownImpls = HleGenerator.HleKnownImplementations.GetKnown();
                var ps5NamesPath = ResolvePs5Names(options);
                var sharpEmuSourcePath = ResolveSharpEmuSource(options);
                var hleResult = HleGenerator.HleRegistryGenerator.Generate(
                    packageDir,
                    sharpEmuSourcePath,
                    ps5NamesPath,
                    hleDir,
                    knownImpls,
                    options.HleSourcePaths.Count != 0
                        ? options.HleSourcePaths
                        : HleGenerator.HleExternalCatalogLoader
                            .FindDefaultSources());
                Console.WriteLine(
                    $"HLE inventory: {hleResult.TotalReferences} references, " +
                    $"{hleResult.UniqueNids} unique NIDs, " +
                    $"{hleResult.NamedCount} named, " +
                    $"{hleResult.ImplementedCount} implemented, " +
                    $"{hleResult.SharpEmuCount} SharpEmu bridge, " +
                    $"{hleResult.StubbedCount} stubbed, " +
                    $"{hleResult.MissingCount} missing");
                Console.WriteLine(
                    $"SharpEmu exports scanned: {hleResult.SharpEmuExportCount}");
                Console.WriteLine(
                    $"External candidates: {hleResult.ExternalCandidateCount}; " +
                    $"package matches: {hleResult.ExternalMatchCount}; " +
                    $"conflicts: {hleResult.ExternalConflictCount}");
                Console.WriteLine(
                    $"HLE work queue: {hleResult.WorkQueueCount} missing; " +
                    $"{hleResult.WorkQueueKytyCount} Kyty-backed; " +
                    $"{hleResult.WorkQueuePath}");
                manifest.CompleteStage("hle_generate", hleDir);
                SaveManifest(manifestPath, manifest);
                TraceEvent(traceWriter, "stage_complete", "hle_generate");
            }

            // Stage 3: Compile (if full build requested)
            if (options.FullBuild)
            {
                // Stage 3: Link
                if (!manifest.HasStage("link"))
                {
                    TraceEvent(traceWriter, "stage_start", "link");
                    var packageDir = Path.Combine(workDir, "package");
                    var runner = NativeRunnerBuilder.Build(
                        packageDir, options.ExecutableName, options.Cxx);
                    Console.WriteLine($"PE: {runner.ExecutablePath}");
                    manifest.CompleteStage("link", runner.ExecutablePath);
                    SaveManifest(manifestPath, manifest);
                    TraceEvent(traceWriter, "stage_complete", "link");
                }

                // Stage 4: Verify image mapping and import patching without
                // entering the game.
                if (!manifest.HasStage("verify"))
                {
                    TraceEvent(traceWriter, "stage_start", "verify");
                    var packageDir = Path.Combine(workDir, "package");
                    VerifyRunner(
                        Path.Combine(packageDir, options.ExecutableName),
                        packageDir);
                    manifest.CompleteStage("verify", packageDir);
                    SaveManifest(manifestPath, manifest);
                    TraceEvent(traceWriter, "stage_complete", "verify");
                }
            }

            startTime.Stop();
            manifest.ElapsedMs = startTime.ElapsedMilliseconds;
            manifest.Status = "success";
            SaveManifest(manifestPath, manifest);
            TraceEvent(traceWriter, "build_complete", manifest.Status);

            Console.WriteLine($"Build completed in {startTime.Elapsed.TotalSeconds:F1}s");
            return 0;
        }
        catch (Exception ex)
        {
            startTime.Stop();
            manifest.Status = "failed";
            manifest.Error = ex.Message;
            manifest.ElapsedMs = startTime.ElapsedMilliseconds;
            SaveManifest(manifestPath, manifest);
            TraceEvent(traceWriter, "build_failed", ex.Message);

            // Generate failure bundle
            GenerateFailureBundle(workDir, manifest, ex);

            Console.Error.WriteLine($"Build failed: {ex.Message}");
            return 1;
        }
    }

    private static void GenerateFailureBundle(
        string workDir, BuildManifest manifest, Exception ex)
    {
        var bundleDir = Path.Combine(workDir, "failure-bundle");
        Directory.CreateDirectory(bundleDir);

        var summary = new StringBuilder();
        summary.AppendLine($"Build failed: {ex.Message}");
        summary.AppendLine($"Stage: {manifest.LastStage ?? "unknown"}");
        summary.AppendLine($"Elapsed: {manifest.ElapsedMs}ms");
        summary.AppendLine($"Timestamp: {DateTime.UtcNow:O}");
        summary.AppendLine();
        summary.AppendLine("Completed stages:");
        foreach (var stage in manifest.CompletedStages)
        {
            summary.AppendLine($"  - {stage}");
        }
        summary.AppendLine();
        summary.AppendLine($"Stack trace:{Environment.NewLine}{ex.StackTrace}");

        File.WriteAllText(
            Path.Combine(bundleDir, "summary.txt"),
            summary.ToString());

        var manifestCopy = Path.Combine(bundleDir, "manifest.json");
        SaveManifest(manifestCopy, manifest);

        Console.WriteLine($"Failure bundle: {bundleDir}");
    }

    private static void TraceEvent(StreamWriter writer, string eventType, string detail)
    {
        var entry = new Dictionary<string, string>
        {
            ["timestamp"] = DateTime.UtcNow.ToString("O"),
            ["event"] = eventType,
            ["detail"] = detail,
        };
        writer.WriteLine(JsonSerializer.Serialize(entry));
    }

    /// <summary>
    /// Compiles every module the export recorded in modules.json into
    /// package/modules/&lt;name&gt;/. A module that fails to compile is reported and
    /// skipped: a partial module set still resolves more imports than none.
    /// </summary>
    private static int CompileModules(
        BuildOptions options,
        string packageDirectory,
        StreamWriter traceWriter)
    {
        var modulesManifest = Path.Combine(packageDirectory, "modules.json");
        ModuleInventory? inventory = null;
        if (File.Exists(modulesManifest))
        {
            try
            {
                using var stream = File.OpenRead(modulesManifest);
                inventory = JsonSerializer.Deserialize<ModuleInventory>(
                    stream,
                    new JsonSerializerOptions
                    {
                        PropertyNameCaseInsensitive = true,
                    });
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"Modules: cannot read modules.json: {ex.Message}");
            }
        }

        var app0 = options.App0Directory ?? inventory?.App0Directory;
        if (string.IsNullOrWhiteSpace(app0) || !Directory.Exists(app0))
        {
            Console.WriteLine("Modules: no app0 directory, skipping");
            return 0;
        }

        var modules = inventory?.Modules ?? [];
        if (modules.Count == 0)
        {
            // compile does not emit modules.json (only inspect does), so fall
            // back to the dump's fakelib directory, which is where the stub
            // modules live.
            modules = EnumerateFakelibModules(app0);
        }

        if (modules.Count == 0)
        {
            Console.WriteLine("Modules: nothing to compile");
            return 0;
        }

        var modulesRoot = Path.Combine(packageDirectory, "modules");
        Directory.CreateDirectory(modulesRoot);

        var compiled = 0;
        var failed = 0;
        foreach (var module in modules)
        {
            if (string.IsNullOrWhiteSpace(module.RelativePath))
            {
                continue;
            }

            var modulePath = Path.Combine(app0, module.RelativePath);
            if (!File.Exists(modulePath))
            {
                Console.Error.WriteLine($"Modules: missing {module.RelativePath}");
                failed++;
                continue;
            }

            var moduleName = Path.GetFileNameWithoutExtension(module.RelativePath);
            var moduleOutput = Path.Combine(modulesRoot, moduleName);
            if (File.Exists(Path.Combine(moduleOutput, "manifest.json")))
            {
                compiled++;
                continue;
            }

            try
            {
                var arguments = CreateArgs(options, Command.Compile, moduleOutput);
                arguments.App0Directory = app0;
                var result = CompilerFrontend.CompileModule(
                    arguments,
                    modulePath,
                    packageDirectory,
                    moduleOutput);
                PrintOutput(result);
                compiled++;
                TraceEvent(traceWriter, "module_compiled", module.RelativePath);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"Modules: {module.RelativePath} failed: {ex.Message}");
                failed++;
                TraceEvent(traceWriter, "module_failed", module.RelativePath);
            }
        }

        Console.WriteLine($"Modules: {compiled} compiled, {failed} failed");
        return compiled;
    }

    private static List<ModuleEntry> EnumerateFakelibModules(string app0Directory)
    {
        var fakelib = Path.Combine(app0Directory, "fakelib");
        if (!Directory.Exists(fakelib))
        {
            return [];
        }

        return Directory
            .EnumerateFiles(fakelib)
            .Where(path =>
                path.EndsWith(".sprx", StringComparison.OrdinalIgnoreCase) ||
                path.EndsWith(".prx", StringComparison.OrdinalIgnoreCase))
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .Select(path => new ModuleEntry
            {
                RelativePath = Path.Combine("fakelib", Path.GetFileName(path)),
                Kind = Path.GetExtension(path).TrimStart('.'),
            })
            .ToList();
    }

    private sealed class ModuleInventory
    {
        [JsonPropertyName("app0Directory")]
        public string? App0Directory { get; init; }

        [JsonPropertyName("modules")]
        public List<ModuleEntry> Modules { get; init; } = [];
    }

    private sealed class ModuleEntry
    {
        [JsonPropertyName("relativePath")]
        public string RelativePath { get; init; } = string.Empty;

        [JsonPropertyName("kind")]
        public string Kind { get; init; } = string.Empty;
    }

    private static Arguments CreateArgs(BuildOptions options, Command command, string outputDir) => new()
    {
        Command = command,
        InputPath = options.InputPath,
        OutputDirectory = outputDir,
        ExecutableName = options.ExecutableName,
        Cxx = options.Cxx,
        MaximumInstructions = options.MaxInstructions,
        AllowPartialControlFlow = command == Command.Compile,
        App0Directory = options.App0Directory,
    };

    private static string ResolvePs5Names(BuildOptions options)
    {
        if (!string.IsNullOrWhiteSpace(options.Ps5NamesPath) &&
            File.Exists(options.Ps5NamesPath))
        {
            return options.Ps5NamesPath;
        }

        // Try relative to SharpEmu source
        var sharpemuPath = Path.GetFullPath(
            Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..",
                "sharpemu", "scripts", "ps5_names.txt"));
        if (File.Exists(sharpemuPath))
        {
            return sharpemuPath;
        }

        throw new FileNotFoundException(
            "ps5_names.txt not found. Specify --ps5-names <path>.");
    }

    private static string ResolveSharpEmuSource(BuildOptions options)
    {
        if (!string.IsNullOrWhiteSpace(options.SharpEmuSourcePath) &&
            Directory.Exists(options.SharpEmuSourcePath))
        {
            return Path.GetFullPath(options.SharpEmuSourcePath);
        }

        var preferred = LocalPaths.Dev("sharpemu", "src");
        if (Directory.Exists(preferred))
        {
            return preferred;
        }

        var relative = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "..",
            "..",
            "..",
            "..",
            "sharpemu",
            "src"));
        if (Directory.Exists(relative))
        {
            return relative;
        }

        throw new DirectoryNotFoundException(
            "SharpEmu source was not found. Specify " +
            "--sharpemu-source <directory>.");
    }

    private static void VerifyRunner(
        string executablePath,
        string packageDirectory)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = executablePath,
            WorkingDirectory = packageDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        startInfo.ArgumentList.Add("--package");
        startInfo.ArgumentList.Add(packageDirectory);
        startInfo.ArgumentList.Add("--validate-only");
        startInfo.ArgumentList.Add("--headless");

        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException(
                "Could not start generated runner validation.");
        var standardOutput = process.StandardOutput.ReadToEndAsync();
        var standardError = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(30_000))
        {
            process.Kill(entireProcessTree: true);
            throw new TimeoutException(
                "Generated runner validation exceeded 30 seconds.");
        }
        Task.WaitAll(standardOutput, standardError);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Generated runner validation exited with code " +
                $"{process.ExitCode}.{Environment.NewLine}" +
                standardError.Result.TrimEnd());
        }
        Console.Write(standardOutput.Result);
        if (!string.IsNullOrWhiteSpace(standardError.Result))
        {
            Console.Error.Write(standardError.Result);
        }
    }

    private static void PrintOutput(CompilerFrontendResult result)
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

    private static BuildManifest LoadOrCreateManifest(string path)
    {
        if (!File.Exists(path))
        {
            return new BuildManifest { Status = "pending" };
        }

        try
        {
            return JsonSerializer.Deserialize<BuildManifest>(
                File.ReadAllText(path)) ?? new BuildManifest { Status = "pending" };
        }
        catch
        {
            return new BuildManifest { Status = "pending" };
        }
    }

    private static void SaveManifest(string path, BuildManifest manifest)
    {
        File.WriteAllText(
            path,
            JsonSerializer.Serialize(manifest, new JsonSerializerOptions
            {
                WriteIndented = true,
            }) + Environment.NewLine);
    }
}

internal sealed class BuildOptions
{
    public string InputPath { get; set; } = string.Empty;
    public string OutputDirectory { get; set; } = string.Empty;
    public string ExecutableName { get; set; } = "Game.exe";
    public string Cxx { get; set; } = "g++";
    public int MaxInstructions { get; set; } = 2_000_000;
    public bool FullBuild { get; set; }
    public string? Ps5NamesPath { get; set; }
    public string? SharpEmuSourcePath { get; set; }
    public List<string> HleSourcePaths { get; set; } = [];
    public string? ProfilePath { get; set; }
    public string? App0Directory { get; set; }
}

internal sealed class BuildManifest
{
    public string Status { get; set; } = "pending";
    public string? Error { get; set; }
    public long ElapsedMs { get; set; }
    public string? LastStage { get; set; }
    public List<string> CompletedStages { get; set; } = [];
    public Dictionary<string, string> StageOutputs { get; set; } = [];

    public bool HasStage(string stage) =>
        CompletedStages.Contains(stage);

    public void CompleteStage(string stage, string output)
    {
        CompletedStages.Add(stage);
        StageOutputs[stage] = output;
        LastStage = stage;
    }
}
