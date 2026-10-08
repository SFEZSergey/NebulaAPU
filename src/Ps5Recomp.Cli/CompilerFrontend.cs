// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Diagnostics;

namespace Ps5Recomp.Cli;

internal sealed record CompilerFrontendResult(
    string StandardOutput,
    string StandardError);

internal static class CompilerFrontend
{
    private const int DefaultTimeoutMilliseconds = 15 * 60 * 1000;

    public static CompilerFrontendResult Compile(
        Arguments options,
        string inputPath,
        string outputDirectory)
    {
        var arguments = new List<string>
        {
            "compile",
            Path.GetFullPath(inputPath),
            "-o",
            Path.GetFullPath(outputDirectory),
            "--max-instructions",
            options.MaximumInstructions.ToString(),
        };
        if (options.AllowPartialControlFlow)
        {
            arguments.Add("--allow-partial-cfg");
        }
        return Run(options, arguments);
    }

    /// <summary>
    /// Compiles one dependent module against an already built base package.
    /// The frontend writes manifest.json and image.bin into
    /// <paramref name="outputDirectory"/>, which the runtime metadata generator
    /// picks up as an additional guest image.
    /// </summary>
    public static CompilerFrontendResult CompileModule(
        Arguments options,
        string modulePath,
        string basePackageDirectory,
        string outputDirectory)
    {
        var arguments = new List<string>
        {
            "compile-module",
            Path.GetFullPath(modulePath),
            "-o",
            Path.GetFullPath(outputDirectory),
            "--base-package",
            Path.GetFullPath(basePackageDirectory),
        };

        return Run(options, arguments);
    }

    public static CompilerFrontendResult Inspect(Arguments options)
    {
        var arguments = new List<string>
        {
            "inspect",
            Path.GetFullPath(options.InputPath!),
            "-o",
            Path.GetFullPath(options.OutputDirectory),
            "--max-instructions",
            options.MaximumInstructions.ToString(),
        };
        if (!string.IsNullOrWhiteSpace(options.App0Directory))
        {
            arguments.Add("--app0");
            arguments.Add(Path.GetFullPath(options.App0Directory));
        }

        return Run(options, arguments);
    }

    private static CompilerFrontendResult Run(
        Arguments options,
        IReadOnlyList<string> arguments)
    {
        RequireFile(options.DotnetPath, "dotnet host");
        RequireFile(options.FrontendPath, "compiler frontend");

        var startInfo = new ProcessStartInfo
        {
            FileName = options.DotnetPath,
            WorkingDirectory = Path.GetDirectoryName(options.FrontendPath)!,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        startInfo.ArgumentList.Add(options.FrontendPath);
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException("Could not start compiler frontend.");
        var standardOutput = process.StandardOutput.ReadToEndAsync();
        var standardError = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(DefaultTimeoutMilliseconds))
        {
            process.Kill(entireProcessTree: true);
            throw new TimeoutException(
                $"Compiler frontend exceeded {DefaultTimeoutMilliseconds / 60_000} minutes.");
        }

        Task.WaitAll(standardOutput, standardError);
        var result = new CompilerFrontendResult(
            standardOutput.Result,
            standardError.Result);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Compiler frontend exited with code {process.ExitCode}." +
                Environment.NewLine +
                result.StandardError.TrimEnd());
        }

        return result;
    }

    private static void RequireFile(string path, string description)
    {
        if (!File.Exists(path))
        {
            throw new FileNotFoundException(
                $"Configured {description} was not found.",
                path);
        }
    }
}
