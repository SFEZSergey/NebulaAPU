// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Ps5Recomp.Cli.HleGenerator;

/// <summary>
/// Generates a package-specific HLE inventory from recompiler manifests,
/// SharpEmu SysAbiExport metadata, and the public PS5 symbol-name catalog.
/// Missing semantics stay unresolved; registration must never imply success.
/// </summary>
internal static partial class HleRegistryGenerator
{
    private static readonly byte[] NidSuffix =
    [
        0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
        0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30,
    ];

    public static HleRegistryResult Generate(
        string packageDirectory,
        string sharpEmuSourceDirectory,
        string? ps5NamesPath,
        string outputDirectory,
        IReadOnlyList<HleEntry>? knownImplementations = null,
        IReadOnlyList<string>? externalSourcePaths = null)
    {
        var fullOutputDirectory = Path.GetFullPath(outputDirectory);
        Directory.CreateDirectory(fullOutputDirectory);

        var imports = LoadPackageImports(packageDirectory);
        var sharpEmuCatalog = LoadSharpEmuExports(sharpEmuSourceDirectory);
        var nameCatalog = string.IsNullOrWhiteSpace(ps5NamesPath) ||
                          !File.Exists(ps5NamesPath)
            ? new Dictionary<string, string>(StringComparer.Ordinal)
            : LoadNameCatalog(ps5NamesPath);
        var externalCatalog = HleExternalCatalogLoader.Load(
            externalSourcePaths,
            ComputeNid);
        var implementations = (knownImplementations ?? [])
            .GroupBy(entry => entry.Nid, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group.First(),
                StringComparer.Ordinal);

        var entries = imports
            .GroupBy(import => import.Nid, StringComparer.Ordinal)
            .Select(group => CreateCatalogEntry(
                group.Key,
                group.ToArray(),
                sharpEmuCatalog.ByNid,
                nameCatalog,
                implementations,
                externalCatalog.ByNid,
                externalCatalog.AllByNid))
            .OrderBy(entry => entry.Nid, StringComparer.Ordinal)
            .ToArray();

        var headerPath = Path.Combine(
            fullOutputDirectory,
            "hle_registry.generated.h");
        var sourcePath = Path.Combine(
            fullOutputDirectory,
            "hle_registry.generated.cpp");
        var bindingsPath = Path.Combine(
            fullOutputDirectory,
            "hle_bindings.generated.inc");
        var reportPath = Path.Combine(
            fullOutputDirectory,
            "imports-usage.csv");
        var missingPath = Path.Combine(
            fullOutputDirectory,
            "missing_hle.toml");
        var stubsPath = Path.Combine(
            fullOutputDirectory,
            "hle-stubs.generated.cpp");
        var abiReportPath = Path.Combine(
            fullOutputDirectory,
            "hle-abi-report.txt");
        var conflictsPath = Path.Combine(
            fullOutputDirectory,
            "hle-conflicts.csv");
        var workQueuePath = Path.Combine(
            fullOutputDirectory,
            "hle-work-queue.csv");
        var workQueue = CreateWorkQueue(entries, externalCatalog.AllByNid);

        File.WriteAllText(
            headerPath,
            GenerateHeader(entries),
            new UTF8Encoding(false));
        File.WriteAllText(
            sourcePath,
            GenerateSource(entries),
            new UTF8Encoding(false));
        File.WriteAllText(
            bindingsPath,
            GenerateBindings(entries),
            new UTF8Encoding(false));
        File.WriteAllText(
            reportPath,
            GenerateUsageReport(imports, entries),
            new UTF8Encoding(false));
        File.WriteAllText(
            missingPath,
            GenerateMissingToml(entries),
            new UTF8Encoding(false));
        File.WriteAllText(
            stubsPath,
            GenerateStubSkeletons(entries),
            new UTF8Encoding(false));
        File.WriteAllText(
            abiReportPath,
            GenerateAbiReport(
                imports,
                entries,
                sharpEmuCatalog,
                externalCatalog,
                workQueue),
            new UTF8Encoding(false));
        File.WriteAllText(
            conflictsPath,
            GenerateConflicts(entries, externalCatalog.Conflicts),
            new UTF8Encoding(false));
        File.WriteAllText(
            workQueuePath,
            GenerateWorkQueue(workQueue),
            new UTF8Encoding(false));

        var implemented = entries.Count(entry => entry.Status == "implemented");
        var sharpEmu = entries.Count(entry => entry.Status == "sharpemu");
        var stubbed = entries.Count(entry => entry.Status == "stubbed");
        var named = entries.Count(entry => !string.IsNullOrWhiteSpace(entry.Name));
        return new HleRegistryResult(
            headerPath,
            sourcePath,
            bindingsPath,
            reportPath,
            missingPath,
            stubsPath,
            abiReportPath,
            conflictsPath,
            workQueuePath,
            imports.Count,
            entries.Length,
            named,
            implemented,
            sharpEmu,
            stubbed,
            entries.Length - implemented - sharpEmu - stubbed,
            sharpEmuCatalog.ExportCount,
            externalCatalog.CandidateCount,
            entries.Count(entry =>
                externalCatalog.ByNid.ContainsKey(entry.Nid)),
            entries.Count(entry =>
                externalCatalog.Conflicts.ContainsKey(entry.Nid)),
            workQueue.Count,
            workQueue.Count(item => item.HasKyty));
    }

    public static void RunWorkQueueSelfTest()
    {
        var entries = new[]
        {
            new HleCatalogEntry(
                "AAAAAAAAAAA",
                "sceAgcFixture",
                "libSceAgc",
                string.Empty,
                string.Empty,
                "missing",
                string.Empty,
                "KytyPS5:src/libs/libGraphicsDriver.cpp:1",
                "function",
                2,
                90,
                "kytyps5-lib-func-nid+ida-ps5-symbol-name"),
            new HleCatalogEntry(
                "BBBBBBBBBBB",
                "sceOtherFixture",
                "libSceOther",
                string.Empty,
                string.Empty,
                "missing",
                string.Empty,
                "ida_ps5_elf_plugin:cfg/ps5_symbols.txt:1",
                "function",
                8,
                62,
                "ida-ps5-symbol-name"),
            new HleCatalogEntry(
                "CCCCCCCCCCC",
                "sceImplementedFixture",
                "libSceAgc",
                string.Empty,
                string.Empty,
                "implemented",
                "ps5rt_fixture",
                "KytyPS5:src/libs/libGraphicsDriver.cpp:2",
                "function",
                1,
                100,
                "native-binding+kytyps5-lib-func-nid"),
        };
        var external = new Dictionary<string, IReadOnlyList<ExternalHleCandidate>>(
            StringComparer.Ordinal)
        {
            ["AAAAAAAAAAA"] =
            [
                new ExternalHleCandidate(
                    "AAAAAAAAAAA",
                    "GraphicsFixture",
                    "libSceAgc",
                    "KytyPS5 handler: Gen5::GraphicsFixture",
                    "KytyPS5:src/libs/libGraphicsDriver.cpp:1",
                    "kytyps5-lib-func-nid",
                    78),
            ],
        };
        var queue = CreateWorkQueue(entries, external);
        if (queue.Count != 2 ||
            queue[0].Nid != "AAAAAAAAAAA" ||
            queue[0].KytyHandler != "Gen5::GraphicsFixture" ||
            queue[0].PriorityTier != "P0")
        {
            throw new InvalidOperationException(
                "HLE work queue self-test failed.");
        }
    }

    private static HleCatalogEntry CreateCatalogEntry(
        string nid,
        IReadOnlyList<PackageImport> imports,
        IReadOnlyDictionary<string, SharpEmuExport> sharpEmuExports,
        IReadOnlyDictionary<string, string> nameCatalog,
        IReadOnlyDictionary<string, HleEntry> implementations,
        IReadOnlyDictionary<string, ExternalHleCandidate> externalCandidates,
        IReadOnlyDictionary<string, IReadOnlyList<ExternalHleCandidate>>
            allExternalCandidates)
    {
        sharpEmuExports.TryGetValue(nid, out var sharpEmu);
        nameCatalog.TryGetValue(nid, out var catalogName);
        implementations.TryGetValue(nid, out var implementation);
        externalCandidates.TryGetValue(nid, out var external);
        allExternalCandidates.TryGetValue(nid, out var externalMatches);

        var name = implementation?.Name ??
                   sharpEmu?.ExportName ??
                   catalogName ??
                   external?.Name ??
                   string.Empty;
        var library = implementation?.Library ??
                      sharpEmu?.Library ??
                      external?.Library ??
                      string.Empty;
        var status = implementation?.Status ??
                     (sharpEmu is null ? "missing" : "sharpemu");
        var signature = FirstNonEmpty(
            implementation?.Signature,
            external?.Signature,
            sharpEmu?.Signature);
        var sources = new List<string>();
        var evidence = new List<string>();
        var confidence = 0;
        if (implementation is not null)
        {
            sources.Add("ps5rt:HleKnownImplementations");
            evidence.Add("native-binding");
            confidence = 100;
        }
        if (sharpEmu is not null)
        {
            sources.Add($"{sharpEmu.SourceFile}:{sharpEmu.SourceLine}");
            evidence.Add("sharpemu-export");
            confidence = Math.Max(confidence, 95);
        }
        if (!string.IsNullOrWhiteSpace(catalogName))
        {
            sources.Add("ps5_names.txt");
            evidence.Add("ps5-name-hash");
            confidence = Math.Max(confidence, 90);
        }
        if (external is not null)
        {
            foreach (var candidate in externalMatches ?? [external])
            {
                sources.Add(candidate.Source);
                evidence.Add(candidate.Evidence);
                confidence = Math.Max(confidence, candidate.Confidence);
            }
        }
        var kind = imports.Any(import => import.Kind == "function")
            ? "function"
            : "data";

        return new HleCatalogEntry(
            nid,
            name,
            library,
            signature,
            sharpEmu?.Target ?? string.Empty,
            status,
            implementation?.Handler ??
                (sharpEmu is null ? string.Empty : "ps5rt_sharpemu_import"),
            string.Join("; ", sources.Distinct(StringComparer.Ordinal)),
            kind,
            imports.Count,
            confidence,
            string.Join("+", evidence.Distinct(StringComparer.Ordinal)));
    }

    private static List<PackageImport> LoadPackageImports(
        string packageDirectory)
    {
        var fullPackageDirectory = Path.GetFullPath(packageDirectory);
        var manifestPaths = new List<string>
        {
            Path.Combine(fullPackageDirectory, "manifest.json"),
        };
        var modulesDirectory = Path.Combine(fullPackageDirectory, "modules");
        if (Directory.Exists(modulesDirectory))
        {
            manifestPaths.AddRange(Directory.EnumerateFiles(
                modulesDirectory,
                "manifest.json",
                SearchOption.AllDirectories));
        }

        var imports = new List<PackageImport>();
        foreach (var manifestPath in manifestPaths
                     .Distinct(StringComparer.OrdinalIgnoreCase)
                     .OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
        {
            if (!File.Exists(manifestPath))
            {
                throw new FileNotFoundException(
                    "Package manifest was not found.",
                    manifestPath);
            }

            using var document = JsonDocument.Parse(
                File.ReadAllText(manifestPath));
            var root = document.RootElement;
            var image = root.TryGetProperty("sourceFile", out var sourceFile)
                ? sourceFile.GetString() ?? Path.GetFileName(
                    Path.GetDirectoryName(manifestPath)) ?? "unknown"
                : Path.GetFileName(
                    Path.GetDirectoryName(manifestPath)) ?? "unknown";

            if (root.TryGetProperty("imports", out var functionImports))
            {
                foreach (var import in functionImports.EnumerateArray())
                {
                    var nid = import.GetProperty("nid").GetString();
                    if (string.IsNullOrWhiteSpace(nid))
                    {
                        continue;
                    }
                    imports.Add(new PackageImport(
                        image,
                        import.GetProperty("stubAddress").GetUInt64(),
                        nid,
                        "function"));
                }
            }

            if (root.TryGetProperty(
                    "importedRelocations",
                    out var relocations))
            {
                foreach (var relocation in relocations.EnumerateArray())
                {
                    var nid = relocation.GetProperty("nid").GetString();
                    if (string.IsNullOrWhiteSpace(nid))
                    {
                        continue;
                    }
                    var isData = relocation.TryGetProperty(
                        "isData",
                        out var dataProperty) && dataProperty.GetBoolean();
                    if (!isData)
                    {
                        continue;
                    }
                    imports.Add(new PackageImport(
                        image,
                        relocation.GetProperty("targetAddress").GetUInt64(),
                        nid,
                        "data"));
                }
            }
        }
        return imports;
    }

    private static SharpEmuCatalog LoadSharpEmuExports(
        string sharpEmuSourceDirectory)
    {
        var fullSourceDirectory = Path.GetFullPath(sharpEmuSourceDirectory);
        if (!Directory.Exists(fullSourceDirectory))
        {
            throw new DirectoryNotFoundException(
                $"SharpEmu source directory was not found: {fullSourceDirectory}");
        }

        var exports = new List<SharpEmuExport>();
        var malformed = new List<string>();
        foreach (var sourcePath in Directory
                     .EnumerateFiles(
                         fullSourceDirectory,
                         "*.cs",
                         SearchOption.AllDirectories)
                     .OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
        {
            var text = File.ReadAllText(sourcePath);
            foreach (Match attribute in SysAbiExportRegex().Matches(text))
            {
                var arguments = attribute.Groups["arguments"].Value;
                var nid = GetNamedString(arguments, "Nid");
                var exportName = GetNamedString(arguments, "ExportName");
                var library = GetNamedString(arguments, "LibraryName");
                var target = GetNamedExpression(arguments, "Target");

                if (string.IsNullOrWhiteSpace(nid) &&
                    !string.IsNullOrWhiteSpace(exportName))
                {
                    nid = ComputeNid(exportName);
                }
                if (string.IsNullOrWhiteSpace(nid))
                {
                    malformed.Add(
                        $"{RelativePath(fullSourceDirectory, sourcePath)}:" +
                        $"{LineNumber(text, attribute.Index)}");
                    continue;
                }

                var declarationLength = Math.Min(
                    1200,
                    text.Length - attribute.Index - attribute.Length);
                var declarationText = text.Substring(
                    attribute.Index + attribute.Length,
                    declarationLength);
                var method = MethodDeclarationRegex().Match(declarationText);
                var methodName = method.Success
                    ? method.Groups["method"].Value
                    : string.Empty;
                var returnType = method.Success
                    ? method.Groups["return"].Value
                    : "int";
                var parameters = method.Success
                    ? CollapseWhitespace(method.Groups["parameters"].Value)
                    : "CpuContext ctx";
                if (string.IsNullOrWhiteSpace(exportName))
                {
                    exportName = methodName;
                }

                exports.Add(new SharpEmuExport(
                    nid,
                    exportName,
                    string.IsNullOrWhiteSpace(library)
                        ? "libKernel"
                        : library,
                    $"{returnType} {methodName}({parameters})",
                    target,
                    RelativePath(fullSourceDirectory, sourcePath),
                    LineNumber(text, attribute.Index)));
            }
        }

        var duplicates = exports
            .GroupBy(export => export.Nid, StringComparer.Ordinal)
            .Where(group => group.Count() > 1)
            .ToDictionary(
                group => group.Key,
                group => (IReadOnlyList<SharpEmuExport>)group.ToArray(),
                StringComparer.Ordinal);
        var byNid = exports
            .GroupBy(export => export.Nid, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group
                    .OrderBy(export => export.SourceFile, StringComparer.Ordinal)
                    .ThenBy(export => export.SourceLine)
                    .First(),
                StringComparer.Ordinal);
        return new SharpEmuCatalog(
            byNid,
            duplicates,
            malformed,
            exports.Count);
    }

    private static Dictionary<string, string> LoadNameCatalog(
        string ps5NamesPath)
    {
        var names = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var rawLine in File.ReadLines(ps5NamesPath))
        {
            var line = rawLine.Trim();
            string? name = null;
            if (line.StartsWith("/PG", StringComparison.Ordinal) ||
                line.StartsWith("/PL", StringComparison.Ordinal))
            {
                name = line[3..];
            }
            else if (line.StartsWith("/G", StringComparison.Ordinal))
            {
                name = line[2..];
            }
            else if (!line.StartsWith('#'))
            {
                name = line;
            }

            if (string.IsNullOrWhiteSpace(name))
            {
                continue;
            }
            names.TryAdd(ComputeNid(name), name);
        }
        return names;
    }

    public static string ComputeNid(string symbolName)
    {
        var nameBytes = Encoding.UTF8.GetBytes(symbolName);
        var input = new byte[nameBytes.Length + NidSuffix.Length];
        nameBytes.CopyTo(input, 0);
        NidSuffix.CopyTo(input, nameBytes.Length);
        var hash = SHA1.HashData(input);
        Span<byte> reversed = stackalloc byte[8];
        for (var index = 0; index < reversed.Length; index++)
        {
            reversed[index] = hash[7 - index];
        }
        return Convert.ToBase64String(reversed)
            .TrimEnd('=')
            .Replace('/', '-');
    }

    private static string GenerateHeader(
        IReadOnlyList<HleCatalogEntry> entries)
    {
        var text = new StringBuilder(4096);
        text.AppendLine("// Auto-generated by ps5recomp. Do not edit.");
        text.AppendLine("#pragma once");
        text.AppendLine();
        text.AppendLine("#include <cstddef>");
        text.AppendLine("#include <string_view>");
        text.AppendLine();
        text.AppendLine("namespace ps5rt::hle {");
        text.AppendLine();
        text.AppendLine("struct HleSymbol {");
        text.AppendLine("    const char* nid;");
        text.AppendLine("    const char* name;");
        text.AppendLine("    const char* library;");
        text.AppendLine("    const char* signature;");
        text.AppendLine("    const char* status;");
        text.AppendLine("    const char* source;");
        text.AppendLine("    const char* kind;");
        text.AppendLine("    const char* evidence;");
        text.AppendLine("    int confidence;");
        text.AppendLine("    std::size_t reference_count;");
        text.AppendLine("};");
        text.AppendLine();
        text.AppendLine(
            $"inline constexpr std::size_t kHleCatalogSize = {entries.Count}U;");
        text.AppendLine(
            "extern const HleSymbol kHleCatalog[kHleCatalogSize];");
        text.AppendLine(
            "const HleSymbol* HleLookup(std::string_view nid);");
        text.AppendLine();
        text.AppendLine("} // namespace ps5rt::hle");
        return text.ToString();
    }

    private static string GenerateSource(
        IReadOnlyList<HleCatalogEntry> entries)
    {
        var text = new StringBuilder(entries.Count * 180);
        text.AppendLine("// Auto-generated by ps5recomp. Do not edit.");
        text.AppendLine("#include \"hle_registry.generated.h\"");
        text.AppendLine();
        text.AppendLine("namespace ps5rt::hle {");
        text.AppendLine();
        text.AppendLine(
            "const HleSymbol kHleCatalog[kHleCatalogSize] = {");
        foreach (var entry in entries)
        {
            text.Append("    {\"")
                .Append(EscapeCpp(entry.Nid)).Append("\", \"")
                .Append(EscapeCpp(entry.Name)).Append("\", \"")
                .Append(EscapeCpp(entry.Library)).Append("\", \"")
                .Append(EscapeCpp(entry.Signature)).Append("\", \"")
                .Append(EscapeCpp(entry.Status)).Append("\", \"")
                .Append(EscapeCpp(entry.Source)).Append("\", \"")
                .Append(EscapeCpp(entry.Kind)).Append("\", \"")
                .Append(EscapeCpp(entry.Evidence)).Append("\", ")
                .Append(entry.Confidence).Append(", ")
                .Append(entry.ReferenceCount)
                .AppendLine("U},");
        }
        text.AppendLine("};");
        text.AppendLine();
        text.AppendLine("const HleSymbol* HleLookup(std::string_view nid) {");
        text.AppendLine("    std::size_t low = 0;");
        text.AppendLine("    std::size_t high = kHleCatalogSize;");
        text.AppendLine("    while (low < high) {");
        text.AppendLine("        const auto middle = low + (high - low) / 2;");
        text.AppendLine("        const std::string_view candidate =");
        text.AppendLine("            kHleCatalog[middle].nid;");
        text.AppendLine("        if (candidate < nid) {");
        text.AppendLine("            low = middle + 1;");
        text.AppendLine("        } else if (candidate > nid) {");
        text.AppendLine("            high = middle;");
        text.AppendLine("        } else {");
        text.AppendLine("            return &kHleCatalog[middle];");
        text.AppendLine("        }");
        text.AppendLine("    }");
        text.AppendLine("    return nullptr;");
        text.AppendLine("}");
        text.AppendLine();
        text.AppendLine("} // namespace ps5rt::hle");
        return text.ToString();
    }

    private static string GenerateBindings(
        IReadOnlyList<HleCatalogEntry> entries)
    {
        var text = new StringBuilder(4096);
        text.AppendLine("// Auto-generated by ps5recomp. Do not edit.");
        text.AppendLine("// Included inside resolve_import().");
        foreach (var entry in entries.Where(entry =>
                     !string.IsNullOrWhiteSpace(entry.Handler) &&
                     entry.Status != "sharpemu"))
        {
            text.Append("PS5RT_HLE_BIND(\"")
                .Append(EscapeCpp(entry.Nid))
                .Append("\", ")
                .Append(entry.Handler)
                .Append(", \"")
                .Append(EscapeCpp(entry.Name))
                .Append("\", \"")
                .Append(EscapeCpp(entry.Status))
                .AppendLine("\")");
        }
        return text.ToString();
    }

    private static string GenerateUsageReport(
        IReadOnlyList<PackageImport> imports,
        IReadOnlyList<HleCatalogEntry> entries)
    {
        var byNid = entries.ToDictionary(
            entry => entry.Nid,
            StringComparer.Ordinal);
        var text = new StringBuilder(imports.Count * 120);
        text.AppendLine(
            "image,address,kind,nid,name,library,status,handler,signature," +
            "confidence,evidence,source," +
            "called,call_count,first_caller,last_error,blocking");
        foreach (var import in imports
                     .OrderBy(import => import.Image, StringComparer.Ordinal)
                     .ThenBy(import => import.Address))
        {
            var entry = byNid[import.Nid];
            text.Append(CsvField(import.Image)).Append(',')
                .Append($"0x{import.Address:X16}").Append(',')
                .Append(import.Kind).Append(',')
                .Append(CsvField(entry.Nid)).Append(',')
                .Append(CsvField(entry.Name)).Append(',')
                .Append(CsvField(entry.Library)).Append(',')
                .Append(entry.Status).Append(',')
                .Append(CsvField(entry.Handler)).Append(',')
                .Append(CsvField(entry.Signature)).Append(',')
                .Append(entry.Confidence).Append(',')
                .Append(CsvField(entry.Evidence)).Append(',')
                .Append(CsvField(entry.Source))
                .AppendLine(",false,0,,,false");
        }
        return text.ToString();
    }

    private static IReadOnlyList<HleWorkQueueItem> CreateWorkQueue(
        IReadOnlyList<HleCatalogEntry> entries,
        IReadOnlyDictionary<string, IReadOnlyList<ExternalHleCandidate>>
            externalCandidates)
    {
        var items = new List<HleWorkQueueItem>();
        foreach (var entry in entries.Where(entry => entry.Status == "missing"))
        {
            externalCandidates.TryGetValue(entry.Nid, out var candidates);
            var evidence = EvidenceSet(entry.Evidence);
            var kyty = candidates?
                .FirstOrDefault(candidate =>
                    candidate.Evidence.Equals(
                        "kytyps5-lib-func-nid",
                        StringComparison.Ordinal));
            var hasKyty = kyty is not null ||
                          evidence.Contains("kytyps5-lib-func-nid");
            var hasSharpProspero =
                evidence.Contains("sharpprospero-stubcatalog-name");
            var hasPayloadSdk =
                evidence.Contains("ps5-payload-sdk-stub-name");
            var hasIda = evidence.Contains("ida-ps5-symbol-name");
            var hasSignature = !string.IsNullOrWhiteSpace(entry.Signature);
            var sourceCount = entry.Source.Length == 0
                ? 0
                : entry.Source.Split(';', StringSplitOptions.TrimEntries |
                                        StringSplitOptions.RemoveEmptyEntries)
                    .Distinct(StringComparer.Ordinal)
                    .Count();
            var score = ScoreWorkItem(
                entry,
                evidence.Count,
                sourceCount,
                hasKyty,
                hasSharpProspero,
                hasPayloadSdk,
                hasIda,
                hasSignature);
            var tier = PriorityTier(score, hasKyty, entry.Library);
            items.Add(new HleWorkQueueItem(
                entry.Nid,
                entry.Name,
                entry.Library,
                entry.Kind,
                entry.ReferenceCount,
                entry.Confidence,
                evidence.Count,
                sourceCount,
                hasKyty,
                hasSharpProspero,
                hasPayloadSdk,
                hasIda,
                hasSignature,
                score,
                tier,
                KytyHandler(kyty),
                kyty?.Source ?? string.Empty,
                ImplementationHint(entry.Library, hasKyty),
                entry.Evidence,
                entry.Source));
        }

        return items
            .OrderByDescending(item => item.Score)
            .ThenByDescending(item => item.ReferenceCount)
            .ThenBy(item => item.Library, StringComparer.Ordinal)
            .ThenBy(item => item.Name, StringComparer.Ordinal)
            .ThenBy(item => item.Nid, StringComparer.Ordinal)
            .ToArray();
    }

    private static string GenerateWorkQueue(
        IReadOnlyList<HleWorkQueueItem> queue)
    {
        var text = new StringBuilder(queue.Count * 180);
        text.AppendLine(
            "priority_rank,priority_tier,score,nid,name,library,kind," +
            "references,confidence,evidence_count,source_count,has_kyty," +
            "has_sharpprospero,has_payload_sdk,has_ida,has_signature," +
            "kyty_handler,kyty_source,implementation_hint,evidence,source");
        var rank = 1;
        foreach (var item in queue)
        {
            text.Append(rank++).Append(',')
                .Append(item.PriorityTier).Append(',')
                .Append(item.Score).Append(',')
                .Append(CsvField(item.Nid)).Append(',')
                .Append(CsvField(item.Name)).Append(',')
                .Append(CsvField(item.Library)).Append(',')
                .Append(item.Kind).Append(',')
                .Append(item.ReferenceCount).Append(',')
                .Append(item.Confidence).Append(',')
                .Append(item.EvidenceCount).Append(',')
                .Append(item.SourceCount).Append(',')
                .Append(BoolCsv(item.HasKyty)).Append(',')
                .Append(BoolCsv(item.HasSharpProspero)).Append(',')
                .Append(BoolCsv(item.HasPayloadSdk)).Append(',')
                .Append(BoolCsv(item.HasIda)).Append(',')
                .Append(BoolCsv(item.HasSignature)).Append(',')
                .Append(CsvField(item.KytyHandler)).Append(',')
                .Append(CsvField(item.KytySource)).Append(',')
                .Append(CsvField(item.ImplementationHint)).Append(',')
                .Append(CsvField(item.Evidence)).Append(',')
                .AppendLine(CsvField(item.Source));
        }
        return text.ToString();
    }

    private static string GenerateMissingToml(
        IReadOnlyList<HleCatalogEntry> entries)
    {
        var text = new StringBuilder();
        text.AppendLine("# Auto-generated by ps5recomp. Do not edit.");
        text.AppendLine("# Exact HLE semantics must be implemented or ported.");
        foreach (var entry in entries.Where(entry =>
                     entry.Status == "missing"))
        {
            text.AppendLine();
            text.AppendLine("[[hle]]");
            text.Append("nid = \"").Append(EscapeToml(entry.Nid))
                .AppendLine("\"");
            text.Append("name = \"").Append(EscapeToml(entry.Name))
                .AppendLine("\"");
            text.Append("library = \"").Append(EscapeToml(entry.Library))
                .AppendLine("\"");
            text.Append("kind = \"").Append(entry.Kind).AppendLine("\"");
            text.Append("signature = \"")
                .Append(EscapeToml(entry.Signature)).AppendLine("\"");
            text.Append("source = \"").Append(EscapeToml(entry.Source))
                .AppendLine("\"");
            text.Append("confidence = ").Append(entry.Confidence)
                .AppendLine();
            text.Append("evidence = \"").Append(EscapeToml(entry.Evidence))
                .AppendLine("\"");
            text.Append("references = ").Append(entry.ReferenceCount)
                .AppendLine();
            text.AppendLine("called = false");
            text.AppendLine("blocking = false");
            text.AppendLine("profile_override = \"\"");
        }
        return text.ToString();
    }

    private static string GenerateStubSkeletons(
        IReadOnlyList<HleCatalogEntry> entries)
    {
        var text = new StringBuilder();
        text.AppendLine("// Auto-generated HLE implementation skeletons.");
        text.AppendLine("// This file is diagnostic output and is not compiled.");
        text.AppendLine("#include <cstdint>");
        text.AppendLine();
        text.AppendLine("#if defined(__GNUC__) && defined(_WIN32)");
        text.AppendLine("#define PS5_GUEST_ABI __attribute__((sysv_abi))");
        text.AppendLine("#else");
        text.AppendLine("#define PS5_GUEST_ABI");
        text.AppendLine("#endif");
        foreach (var entry in entries.Where(entry =>
                     entry.Status == "missing" &&
                     !string.IsNullOrWhiteSpace(entry.Name)))
        {
            text.AppendLine();
            text.Append("// NID: ").Append(entry.Nid);
            if (!string.IsNullOrWhiteSpace(entry.Library))
            {
                text.Append(" library: ").Append(entry.Library);
            }
            text.AppendLine();
            if (!string.IsNullOrWhiteSpace(entry.Signature))
            {
                text.Append("// SharpEmu: ").AppendLine(entry.Signature);
            }
            text.Append("extern \"C\" PS5_GUEST_ABI std::uint64_t ")
                .Append("ps5rt_missing_")
                .Append(CppIdentifier(entry.Name))
                .AppendLine("(");
            text.AppendLine("    std::uint64_t arg0,");
            text.AppendLine("    std::uint64_t arg1,");
            text.AppendLine("    std::uint64_t arg2,");
            text.AppendLine("    std::uint64_t arg3,");
            text.AppendLine("    std::uint64_t arg4,");
            text.AppendLine("    std::uint64_t arg5) {");
            text.AppendLine("    (void)arg0; (void)arg1; (void)arg2;");
            text.AppendLine("    (void)arg3; (void)arg4; (void)arg5;");
            text.AppendLine("    return 0x80020001ULL;");
            text.AppendLine("}");
        }
        return text.ToString();
    }

    private static string GenerateAbiReport(
        IReadOnlyList<PackageImport> imports,
        IReadOnlyList<HleCatalogEntry> entries,
        SharpEmuCatalog sharpEmuCatalog,
        ExternalHleCatalog externalCatalog,
        IReadOnlyList<HleWorkQueueItem> workQueue)
    {
        var text = new StringBuilder();
        text.AppendLine("ps5recomp HLE ABI report");
        text.AppendLine($"package_references={imports.Count}");
        text.AppendLine($"package_unique_nids={entries.Count}");
        text.AppendLine(
            $"package_named={entries.Count(entry => entry.Name.Length != 0)}");
        text.AppendLine(
            $"package_implemented={entries.Count(entry => entry.Status == "implemented")}");
        text.AppendLine(
            $"package_sharpemu={entries.Count(entry => entry.Status == "sharpemu")}");
        text.AppendLine(
            $"package_stubbed={entries.Count(entry => entry.Status == "stubbed")}");
        text.AppendLine(
            $"package_missing={entries.Count(entry => entry.Status == "missing")}");
        text.AppendLine(
            $"sharpemu_exports={sharpEmuCatalog.ExportCount}");
        text.AppendLine(
            $"sharpemu_unique_nids={sharpEmuCatalog.ByNid.Count}");
        text.AppendLine(
            $"sharpemu_duplicate_nids={sharpEmuCatalog.Duplicates.Count}");
        text.AppendLine(
            $"sharpemu_malformed_exports={sharpEmuCatalog.Malformed.Count}");
        text.AppendLine(
            $"external_sources={externalCatalog.SourceCount}");
        text.AppendLine(
            $"external_candidates={externalCatalog.CandidateCount}");
        text.AppendLine(
            $"external_unique_nids={externalCatalog.ByNid.Count}");
        text.AppendLine(
            $"package_external_matches={entries.Count(entry => externalCatalog.ByNid.ContainsKey(entry.Nid))}");
        text.AppendLine(
            $"package_known_signatures={entries.Count(entry => entry.Signature.Length != 0)}");
        text.AppendLine(
            $"external_conflicts={externalCatalog.Conflicts.Count}");
        text.AppendLine(
            $"package_external_conflicts={entries.Count(entry => externalCatalog.Conflicts.ContainsKey(entry.Nid))}");
        text.AppendLine(
            $"work_queue_items={workQueue.Count}");
        text.AppendLine(
            $"work_queue_kyty_backed={workQueue.Count(item => item.HasKyty)}");
        text.AppendLine(
            $"work_queue_p0={workQueue.Count(item => item.PriorityTier == "P0")}");
        text.AppendLine(
            $"malformed_package_nids={entries.Count(entry => !ValidNidRegex().IsMatch(entry.Nid))}");

        if (sharpEmuCatalog.Duplicates.Count != 0)
        {
            text.AppendLine();
            text.AppendLine("duplicate SharpEmu NIDs:");
            foreach (var duplicate in sharpEmuCatalog.Duplicates
                         .OrderBy(pair => pair.Key, StringComparer.Ordinal))
            {
                text.Append("  ").Append(duplicate.Key).Append(": ");
                text.AppendLine(string.Join(
                    ", ",
                    duplicate.Value.Select(export =>
                        $"{export.Library}:{export.ExportName}")));
            }
        }
        return text.ToString();
    }

    private static string GenerateConflicts(
        IReadOnlyList<HleCatalogEntry> entries,
        IReadOnlyDictionary<string, IReadOnlyList<ExternalHleCandidate>> conflicts)
    {
        var packageNids = entries
            .Select(entry => entry.Nid)
            .ToHashSet(StringComparer.Ordinal);
        var text = new StringBuilder();
        text.AppendLine(
            "nid,name,library,signature,confidence,evidence,source");
        foreach (var conflict in conflicts
                     .Where(pair => packageNids.Contains(pair.Key))
                     .OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            foreach (var candidate in conflict.Value)
            {
                text.Append(CsvField(candidate.Nid)).Append(',')
                    .Append(CsvField(candidate.Name)).Append(',')
                    .Append(CsvField(candidate.Library)).Append(',')
                    .Append(CsvField(candidate.Signature)).Append(',')
                    .Append(candidate.Confidence).Append(',')
                    .Append(CsvField(candidate.Evidence)).Append(',')
                    .AppendLine(CsvField(candidate.Source));
            }
        }
        return text.ToString();
    }

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ??
        string.Empty;

    private static HashSet<string> EvidenceSet(string evidence) =>
        evidence.Split('+', StringSplitOptions.TrimEntries |
                            StringSplitOptions.RemoveEmptyEntries)
            .ToHashSet(StringComparer.Ordinal);

    private static int ScoreWorkItem(
        HleCatalogEntry entry,
        int evidenceCount,
        int sourceCount,
        bool hasKyty,
        bool hasSharpProspero,
        bool hasPayloadSdk,
        bool hasIda,
        bool hasSignature)
    {
        var score = 0;
        if (hasKyty)
        {
            score += 1000;
        }
        if (hasSharpProspero)
        {
            score += 120;
        }
        if (hasPayloadSdk)
        {
            score += 100;
        }
        if (hasIda)
        {
            score += 40;
        }
        if (hasSignature)
        {
            score += 80;
        }
        if (entry.Kind == "function")
        {
            score += 30;
        }
        score += LibraryPriority(entry.Library);
        score += Math.Min(entry.ReferenceCount * 20, 120);
        score += Math.Min(evidenceCount * 20, 160);
        score += Math.Min(sourceCount * 10, 120);
        score += Math.Min(entry.Confidence, 100);
        if (string.IsNullOrWhiteSpace(entry.Library))
        {
            score -= 150;
        }
        return score;
    }

    private static int LibraryPriority(string library) => library switch
    {
        "libSceAgc" => 500,
        "libSceAgcDriver" => 470,
        "libKernel" => 440,
        "libSceLibcInternal" => 380,
        "libc" => 350,
        "libSceFont" => 300,
        "libSceAmpr" => 280,
        "libSceJson" => 260,
        "libSceJson2" => 260,
        "libSceHttp" => 240,
        "libSceHttp2" => 240,
        "libSceNet" => 230,
        "libSceNetCtl" => 220,
        "libSceVideoOut" => 210,
        "libSceAvPlayer" => 200,
        "libSceAudioOut2" => 190,
        "libSceAudioOut" => 180,
        "libSceSaveData" => 170,
        "libScePad" => 160,
        "libSceUserService" => 150,
        _ => 80,
    };

    private static string PriorityTier(
        int score,
        bool hasKyty,
        string library)
    {
        if (hasKyty &&
            (library == "libSceAgc" ||
             library == "libSceAgcDriver" ||
             library == "libKernel" ||
             library == "libSceFont"))
        {
            return "P0";
        }
        if (hasKyty || score >= 1100)
        {
            return "P1";
        }
        return score >= 750 ? "P2" : "P3";
    }

    private static string KytyHandler(ExternalHleCandidate? candidate)
    {
        const string prefix = "KytyPS5 handler: ";
        if (candidate is null ||
            !candidate.Signature.StartsWith(prefix, StringComparison.Ordinal))
        {
            return string.Empty;
        }
        return candidate.Signature[prefix.Length..];
    }

    private static string ImplementationHint(string library, bool hasKyty)
    {
        var prefix = hasKyty
            ? "port or wrap KytyPS5 handler"
            : "derive ABI and implement native shim";
        return library switch
        {
            "libSceAgc" or "libSceAgcDriver" =>
                $"{prefix}; AGC command builder/GPU bridge path",
            "libKernel" or "libc" or "libSceLibcInternal" =>
                $"{prefix}; host syscall/libc/runtime path",
            "libSceFont" =>
                $"{prefix}; font metrics/raster path",
            "libSceAmpr" =>
                $"{prefix}; asset/package resolver path",
            "libSceJson" or "libSceJson2" =>
                $"{prefix}; JSON object/string ABI path",
            "libSceHttp" or "libSceHttp2" or "libSceNet" or "libSceNetCtl" =>
                $"{prefix}; network stub or local-success policy path",
            _ => prefix,
        };
    }

    private static string BoolCsv(bool value) => value ? "true" : "false";

    private static string? GetNamedString(string arguments, string name)
    {
        var match = Regex.Match(
            arguments,
            $@"\b{Regex.Escape(name)}\s*=\s*""(?<value>(?:\\.|[^""])*)""",
            RegexOptions.CultureInvariant);
        return match.Success
            ? Regex.Unescape(match.Groups["value"].Value)
            : null;
    }

    private static string GetNamedExpression(string arguments, string name)
    {
        var match = Regex.Match(
            arguments,
            $@"\b{Regex.Escape(name)}\s*=\s*(?<value>[^,\r\n\)]+)",
            RegexOptions.CultureInvariant);
        return match.Success
            ? CollapseWhitespace(match.Groups["value"].Value)
            : string.Empty;
    }

    private static int LineNumber(string text, int index)
    {
        var line = 1;
        for (var position = 0; position < index; position++)
        {
            if (text[position] == '\n')
            {
                line++;
            }
        }
        return line;
    }

    private static string RelativePath(string root, string path) =>
        Path.GetRelativePath(root, path).Replace('\\', '/');

    private static string CollapseWhitespace(string value) =>
        WhitespaceRegex().Replace(value, " ").Trim();

    private static string EscapeCpp(string value) =>
        value.Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal)
            .Replace("\r", "\\r", StringComparison.Ordinal);

    private static string EscapeToml(string value) =>
        value.Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal)
            .Replace("\r", "\\r", StringComparison.Ordinal);

    private static string CsvField(string value) =>
        value.Contains(',') || value.Contains('"') ||
        value.Contains('\r') || value.Contains('\n')
            ? $"\"{value.Replace("\"", "\"\"", StringComparison.Ordinal)}\""
            : value;

    private static string CppIdentifier(string value)
    {
        var text = new StringBuilder(value.Length);
        foreach (var character in value)
        {
            text.Append(char.IsAsciiLetterOrDigit(character)
                ? character
                : '_');
        }
        if (text.Length == 0 || char.IsDigit(text[0]))
        {
            text.Insert(0, '_');
        }
        return text.ToString();
    }

    [GeneratedRegex(
        @"\[\s*SysAbiExport\s*\((?<arguments>.*?)\)\s*\]",
        RegexOptions.Singleline | RegexOptions.CultureInvariant)]
    private static partial Regex SysAbiExportRegex();

    [GeneratedRegex(
        @"(?:(?:public|internal|protected|private|static|unsafe|partial)\s+)+" +
        @"(?<return>[A-Za-z_][A-Za-z0-9_\.<>?]*)\s+" +
        @"(?<method>[A-Za-z_][A-Za-z0-9_]*)\s*" +
        @"\((?<parameters>.*?)\)",
        RegexOptions.Singleline | RegexOptions.CultureInvariant)]
    private static partial Regex MethodDeclarationRegex();

    [GeneratedRegex(
        @"\s+",
        RegexOptions.CultureInvariant)]
    private static partial Regex WhitespaceRegex();

    [GeneratedRegex(
        @"^[A-Za-z0-9+\-]{11}$",
        RegexOptions.CultureInvariant)]
    private static partial Regex ValidNidRegex();
}

internal sealed record PackageImport(
    string Image,
    ulong Address,
    string Nid,
    string Kind);

internal sealed record SharpEmuExport(
    string Nid,
    string ExportName,
    string Library,
    string Signature,
    string Target,
    string SourceFile,
    int SourceLine);

internal sealed record SharpEmuCatalog(
    IReadOnlyDictionary<string, SharpEmuExport> ByNid,
    IReadOnlyDictionary<string, IReadOnlyList<SharpEmuExport>> Duplicates,
    IReadOnlyList<string> Malformed,
    int ExportCount);

internal sealed record HleCatalogEntry(
    string Nid,
    string Name,
    string Library,
    string Signature,
    string Generation,
    string Status,
    string Handler,
    string Source,
    string Kind,
    int ReferenceCount,
    int Confidence,
    string Evidence);

internal sealed record HleEntry(
    string Name,
    string Nid,
    string Library,
    string Handler,
    string Status = "implemented",
    string Signature = "");

internal sealed record HleWorkQueueItem(
    string Nid,
    string Name,
    string Library,
    string Kind,
    int ReferenceCount,
    int Confidence,
    int EvidenceCount,
    int SourceCount,
    bool HasKyty,
    bool HasSharpProspero,
    bool HasPayloadSdk,
    bool HasIda,
    bool HasSignature,
    int Score,
    string PriorityTier,
    string KytyHandler,
    string KytySource,
    string ImplementationHint,
    string Evidence,
    string Source);

internal sealed record HleRegistryResult(
    string HeaderPath,
    string SourcePath,
    string BindingsPath,
    string ReportPath,
    string MissingPath,
    string StubsPath,
    string AbiReportPath,
    string ConflictsPath,
    string WorkQueuePath,
    int TotalReferences,
    int UniqueNids,
    int NamedCount,
    int ImplementedCount,
    int SharpEmuCount,
    int StubbedCount,
    int MissingCount,
    int SharpEmuExportCount,
    int ExternalCandidateCount,
    int ExternalMatchCount,
    int ExternalConflictCount,
    int WorkQueueCount,
    int WorkQueueKytyCount);
