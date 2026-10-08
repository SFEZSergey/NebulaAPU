// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Text.RegularExpressions;

namespace Ps5Recomp.Cli.HleGenerator;

/// <summary>
/// Collects export-name and ABI candidates from public PS4/PS5 projects.
/// Name candidates are re-hashed locally. Sources that publish an explicit
/// NID are retained as implementation evidence without implying HLE support.
/// </summary>
internal static partial class HleExternalCatalogLoader
{
    public static IReadOnlyList<string> FindDefaultSources()
    {
        var candidates = new[]
        {
            LocalPaths.Dev("reference", "PS5SDK"),
            LocalPaths.Dev("reference", "KytyPS5"),
            LocalPaths.Dev("reference", "SharpProspero"),
            LocalPaths.Dev("reference", "ps5-payload-sdk"),
            LocalPaths.Dev("reference", "ida_ps5_elf_plugin"),
            LocalPaths.Dev("reference", "shadPS4"),
            LocalPaths.Dev("reference", "OpenOrbis-PS4-Toolchain"),
            LocalPaths.Dev("reference", "ps4libdoc"),
        };
        return candidates
            .Where(path => Directory.Exists(path) || File.Exists(path))
            .Select(Path.GetFullPath)
            .ToArray();
    }

    public static void RunSelfTest(
        string outputRoot,
        Func<string, string> nidComputer)
    {
        var source = Path.Combine(outputRoot, "hle-catalog-fixtures");
        var kytyLibraries = Path.Combine(source, "src", "libs");
        var sharpProspero = Path.Combine(
            source,
            "tools",
            "SharpProspero.Prx");
        var payloadStubs = Path.Combine(source, "sce_stubs");
        var idaConfig = Path.Combine(source, "cfg");
        Directory.CreateDirectory(kytyLibraries);
        Directory.CreateDirectory(sharpProspero);
        Directory.CreateDirectory(payloadStubs);
        Directory.CreateDirectory(idaConfig);

        File.WriteAllText(
            Path.Combine(kytyLibraries, "libFixture.cpp"),
            """
            LIB_DEFINE(InitFixture_1) {
                LIB_FUNC("AAAAAAAAAAA", Fixture::sceKytyFixture);
            }
            """);
        File.WriteAllText(
            Path.Combine(sharpProspero, "StubCatalog.cs"),
            """
            public static class StubCatalog {
                public static object Core => new Entry("libSceFixture", Fixture);
                private static readonly string[] Fixture = ["sceSharpFixture"];
            }
            """);
        File.WriteAllText(
            Path.Combine(payloadStubs, "libSceFixture.c"),
            """
            asm(".global scePayloadFixture\n"
                ".type scePayloadFixture @function\n"
                "scePayloadFixture:\n");
            """);
        File.WriteAllText(
            Path.Combine(idaConfig, "ps5_symbols.txt"),
            "sceIdaFixture\n");

        var catalog = Load([source], nidComputer);
        AssertSelfTestCandidate(
            catalog,
            "AAAAAAAAAAA",
            "kytyps5-lib-func-nid",
            "libSceFixture");
        AssertSelfTestCandidate(
            catalog,
            nidComputer("sceSharpFixture"),
            "sharpprospero-stubcatalog-name",
            "libSceFixture");
        AssertSelfTestCandidate(
            catalog,
            nidComputer("scePayloadFixture"),
            "ps5-payload-sdk-stub-name",
            "libSceFixture");
        AssertSelfTestCandidate(
            catalog,
            nidComputer("sceIdaFixture"),
            "ida-ps5-symbol-name",
            string.Empty);
    }

    public static ExternalHleCatalog Load(
        IReadOnlyList<string>? sourcePaths,
        Func<string, string> nidComputer)
    {
        var candidates = new List<ExternalHleCandidate>();
        var sources = (sourcePaths ?? [])
            .Where(path => !string.IsNullOrWhiteSpace(path))
            .Select(Path.GetFullPath)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        foreach (var source in sources)
        {
            if (File.Exists(source))
            {
                LoadNameFile(source, source, "name-list", 60, nidComputer, candidates);
                continue;
            }
            if (!Directory.Exists(source))
            {
                continue;
            }

            var knownNames = Path.Combine(source, "known_names.txt");
            if (File.Exists(knownNames))
            {
                LoadNameFile(
                    source,
                    knownNames,
                    "ps4libdoc-name",
                    65,
                    nidComputer,
                    candidates);
            }

            var idaPs5Symbols = Path.Combine(source, "cfg", "ps5_symbols.txt");
            if (File.Exists(idaPs5Symbols))
            {
                LoadNameFile(
                    source,
                    idaPs5Symbols,
                    "ida-ps5-symbol-name",
                    62,
                    nidComputer,
                    candidates);
            }

            var ps5Headers = Path.Combine(source, "ps5");
            if (Directory.Exists(ps5Headers))
            {
                LoadPs5SdkHeaders(source, ps5Headers, nidComputer, candidates);
            }

            var openOrbisHeaders = Path.Combine(source, "include", "orbis");
            if (Directory.Exists(openOrbisHeaders))
            {
                LoadOpenOrbisHeaders(
                    source,
                    openOrbisHeaders,
                    nidComputer,
                    candidates);
            }

            var shadLibraries = Path.Combine(source, "src", "core", "libraries");
            if (Directory.Exists(shadLibraries))
            {
                LoadShadPs4Sources(
                    source,
                    shadLibraries,
                    nidComputer,
                    candidates);
            }

            var kytyLibraries = Path.Combine(source, "src", "libs");
            if (Directory.Exists(kytyLibraries))
            {
                LoadKytyPs5Libraries(source, kytyLibraries, candidates);
            }

            var sharpProsperoStubCatalog = Path.Combine(
                source,
                "tools",
                "SharpProspero.Prx",
                "StubCatalog.cs");
            if (File.Exists(sharpProsperoStubCatalog))
            {
                LoadSharpProsperoStubCatalog(
                    source,
                    sharpProsperoStubCatalog,
                    nidComputer,
                    candidates);
            }

            var payloadSdkStubs = Path.Combine(source, "sce_stubs");
            if (Directory.Exists(payloadSdkStubs))
            {
                LoadPayloadSdkStubs(
                    source,
                    payloadSdkStubs,
                    nidComputer,
                    candidates);
            }
        }

        var unique = candidates
            .GroupBy(candidate => new
            {
                candidate.Nid,
                candidate.Name,
                candidate.Library,
                candidate.Signature,
                candidate.Source,
            })
            .Select(group => group
                .OrderByDescending(candidate => candidate.Confidence)
                .First())
            .ToArray();
        var allByNid = unique
            .GroupBy(candidate => candidate.Nid, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => (IReadOnlyList<ExternalHleCandidate>)group
                    .OrderByDescending(candidate => candidate.Confidence)
                    .ThenByDescending(candidate =>
                        !string.IsNullOrWhiteSpace(candidate.Signature))
                    .ThenBy(candidate => candidate.Source, StringComparer.Ordinal)
                    .ToArray(),
                StringComparer.Ordinal);
        var bestByNid = allByNid.ToDictionary(
            pair => pair.Key,
            pair => pair.Value[0],
            StringComparer.Ordinal);
        var conflicts = allByNid
            .Where(pair =>
                pair.Value.Select(candidate => candidate.Name)
                    .Where(name => name.Length != 0)
                    .Distinct(StringComparer.Ordinal)
                    .Skip(1)
                    .Any() ||
                pair.Value.Select(candidate => candidate.Signature)
                    .Where(signature => signature.Length != 0)
                    .Distinct(StringComparer.Ordinal)
                    .Skip(1)
                    .Any())
            .ToDictionary(
                pair => pair.Key,
                pair => pair.Value,
                StringComparer.Ordinal);

        return new ExternalHleCatalog(
            bestByNid,
            allByNid,
            conflicts,
            unique.Length,
            sources.Length);
    }

    private static void LoadNameFile(
        string sourceRoot,
        string path,
        string evidence,
        int confidence,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        var lineNumber = 0;
        foreach (var rawLine in File.ReadLines(path))
        {
            lineNumber++;
            var name = NormalizeName(rawLine);
            if (name.Length == 0)
            {
                continue;
            }
            AddCandidate(
                name,
                string.Empty,
                string.Empty,
                RelativeSource(sourceRoot, path, lineNumber),
                evidence,
                confidence,
                nidComputer,
                candidates);
        }
    }

    private static void LoadPs5SdkHeaders(
        string sourceRoot,
        string headersRoot,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        foreach (var path in Directory.EnumerateFiles(
                     headersRoot,
                     "*.h",
                     SearchOption.TopDirectoryOnly))
        {
            var text = File.ReadAllText(path);
            foreach (Match match in Ps5SdkFunctionRegex().Matches(text))
            {
                var name = match.Groups["name"].Value;
                var returnType = CollapseWhitespace(
                    match.Groups["return"].Value);
                var parameters = CollapseWhitespace(
                    match.Groups["parameters"].Value);
                AddCandidate(
                    name,
                    LibraryFromHeader(path, ps5: true),
                    $"{returnType} {name}({parameters})",
                    RelativeSource(
                        sourceRoot,
                        path,
                        LineNumber(text, match.Index)),
                    "ps5sdk-header",
                    90,
                    nidComputer,
                    candidates);
            }
        }
    }

    private static void LoadOpenOrbisHeaders(
        string sourceRoot,
        string headersRoot,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        foreach (var path in Directory.EnumerateFiles(
                     headersRoot,
                     "*.h",
                     SearchOption.AllDirectories))
        {
            var text = StripComments(File.ReadAllText(path));
            foreach (Match match in CFunctionPrototypeRegex().Matches(text))
            {
                var name = match.Groups["name"].Value;
                var returnType = CollapseWhitespace(
                    match.Groups["return"].Value);
                var parameters = CollapseWhitespace(
                    match.Groups["parameters"].Value);
                AddCandidate(
                    name,
                    LibraryFromHeader(path, ps5: false),
                    $"{returnType} {name}({parameters})",
                    RelativeSource(
                        sourceRoot,
                        path,
                        LineNumber(text, match.Index)),
                    "openorbis-header",
                    75,
                    nidComputer,
                    candidates);
            }
        }
    }

    private static void LoadShadPs4Sources(
        string sourceRoot,
        string librariesRoot,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        foreach (var path in Directory.EnumerateFiles(
                     librariesRoot,
                     "*.*",
                     SearchOption.AllDirectories)
                 .Where(path =>
                     path.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) ||
                     path.EndsWith(".h", StringComparison.OrdinalIgnoreCase)))
        {
            var text = StripComments(File.ReadAllText(path));
            var signatures = ShadFunctionSignatureRegex()
                .Matches(text)
                .Cast<Match>()
                .GroupBy(match => match.Groups["name"].Value, StringComparer.Ordinal)
                .ToDictionary(
                    group => group.Key,
                    group =>
                    {
                        var match = group.First();
                        var name = match.Groups["name"].Value;
                        return $"{CollapseWhitespace(match.Groups["return"].Value)} " +
                               $"{name}({CollapseWhitespace(match.Groups["parameters"].Value)})";
                    },
                    StringComparer.Ordinal);

            foreach (Match match in ShadRegistrationRegex().Matches(text))
            {
                var handler = match.Groups["handler"].Value;
                var name = handler.Contains("::", StringComparison.Ordinal)
                    ? handler[(handler.LastIndexOf("::", StringComparison.Ordinal) + 2)..]
                    : handler;
                signatures.TryGetValue(name, out var signature);
                AddCandidate(
                    name,
                    match.Groups["library"].Value,
                    signature ?? string.Empty,
                    RelativeSource(
                        sourceRoot,
                        path,
                        LineNumber(text, match.Index)),
                    "shadps4-registration",
                    70,
                    nidComputer,
                    candidates);

                if (name.StartsWith("posix_", StringComparison.Ordinal))
                {
                    var exportName = name["posix_".Length..];
                    AddCandidate(
                        exportName,
                        match.Groups["library"].Value,
                        string.IsNullOrWhiteSpace(signature)
                            ? string.Empty
                            : signature.Replace(
                                name + "(",
                                exportName + "(",
                                StringComparison.Ordinal),
                        RelativeSource(
                            sourceRoot,
                            path,
                            LineNumber(text, match.Index)),
                        "shadps4-posix-alias",
                        60,
                        nidComputer,
                        candidates);
                }
            }
        }
    }

    private static void LoadKytyPs5Libraries(
        string sourceRoot,
        string librariesRoot,
        ICollection<ExternalHleCandidate> candidates)
    {
        foreach (var path in Directory.EnumerateFiles(
                     librariesRoot,
                     "*.cpp",
                     SearchOption.TopDirectoryOnly))
        {
            var currentInitializer = string.Empty;
            var lineNumber = 0;
            foreach (var line in File.ReadLines(path))
            {
                lineNumber++;
                var define = KytyLibDefineRegex().Match(line);
                if (define.Success && line.Contains('{'))
                {
                    currentInitializer = define.Groups["initializer"].Value;
                    continue;
                }

                var function = KytyLibFunctionRegex().Match(line);
                if (!function.Success)
                {
                    continue;
                }

                var nid = function.Groups["nid"].Value;
                if (!ValidExternalNid(nid))
                {
                    continue;
                }

                var handler = CollapseWhitespace(
                    function.Groups["handler"].Value);
                var library = InferKytyLibrary(
                    currentInitializer,
                    Path.GetFileName(path),
                    handler);
                var name = HandlerLeafName(handler);
                AddKnownNidCandidate(
                    nid,
                    name,
                    library,
                    $"KytyPS5 handler: {handler}",
                    RelativeSource(sourceRoot, path, lineNumber),
                    "kytyps5-lib-func-nid",
                    78,
                    candidates);
            }
        }
    }

    private static void LoadSharpProsperoStubCatalog(
        string sourceRoot,
        string path,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        var text = File.ReadAllText(path);
        var libraryByArray = SharpProsperoEntryRegex()
            .Matches(text)
            .Cast<Match>()
            .GroupBy(
                match => match.Groups["array"].Value,
                StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group.First().Groups["library"].Value,
                StringComparer.Ordinal);

        foreach (Match array in SharpProsperoArrayRegex().Matches(text))
        {
            var arrayName = array.Groups["array"].Value;
            if (!libraryByArray.TryGetValue(arrayName, out var library))
            {
                continue;
            }

            foreach (Match nameMatch in QuotedStringRegex()
                         .Matches(array.Groups["body"].Value))
            {
                var name = Regex.Unescape(nameMatch.Groups["value"].Value);
                AddCandidate(
                    name,
                    NormalizeSharpProsperoLibrary(library),
                    string.Empty,
                    RelativeSource(
                        sourceRoot,
                        path,
                        LineNumber(text, array.Index + nameMatch.Index)),
                    "sharpprospero-stubcatalog-name",
                    84,
                    nidComputer,
                    candidates);
            }
        }
    }

    private static void LoadPayloadSdkStubs(
        string sourceRoot,
        string stubsRoot,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        foreach (var path in Directory.EnumerateFiles(
                     stubsRoot,
                     "*.c",
                     SearchOption.TopDirectoryOnly))
        {
            var text = File.ReadAllText(path);
            var library = LibraryFromPayloadSdkStub(path);
            foreach (Match match in PayloadSdkGlobalRegex().Matches(text))
            {
                var name = match.Groups["name"].Value;
                AddCandidate(
                    name,
                    library,
                    string.Empty,
                    RelativeSource(
                        sourceRoot,
                        path,
                        LineNumber(text, match.Index)),
                    "ps5-payload-sdk-stub-name",
                    80,
                    nidComputer,
                    candidates);
            }
        }
    }

    private static void AddCandidate(
        string name,
        string library,
        string signature,
        string source,
        string evidence,
        int confidence,
        Func<string, string> nidComputer,
        ICollection<ExternalHleCandidate> candidates)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }
        candidates.Add(new ExternalHleCandidate(
            nidComputer(name),
            name,
            library,
            CollapseWhitespace(signature),
            source,
            evidence,
            confidence));
    }

    private static void AddKnownNidCandidate(
        string nid,
        string name,
        string library,
        string signature,
        string source,
        string evidence,
        int confidence,
        ICollection<ExternalHleCandidate> candidates)
    {
        if (!ValidExternalNid(nid))
        {
            return;
        }
        candidates.Add(new ExternalHleCandidate(
            nid,
            name,
            library,
            CollapseWhitespace(signature),
            source,
            evidence,
            confidence));
    }

    private static string NormalizeName(string rawLine)
    {
        var line = rawLine.Trim();
        if (line.Length == 0 || line.StartsWith('#'))
        {
            return string.Empty;
        }
        if (line.StartsWith("/PG", StringComparison.Ordinal) ||
            line.StartsWith("/PL", StringComparison.Ordinal))
        {
            return line[3..];
        }
        if (line.StartsWith("/G", StringComparison.Ordinal))
        {
            return line[2..];
        }
        return line;
    }

    private static string LibraryFromHeader(string path, bool ps5)
    {
        var stem = Path.GetFileNameWithoutExtension(path);
        if (stem.Equals("libkernel", StringComparison.OrdinalIgnoreCase))
        {
            return "libKernel";
        }
        if (stem.Equals("libc", StringComparison.OrdinalIgnoreCase))
        {
            return ps5 ? "libSceLibcInternal" : "libc";
        }
        if (stem.StartsWith("libSce", StringComparison.Ordinal))
        {
            return stem;
        }
        return "libSce" + stem;
    }

    private static string LibraryFromPayloadSdkStub(string path)
    {
        var stem = Path.GetFileNameWithoutExtension(path);
        if (stem.Equals("libkernel", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("libkernel_sys", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("libkernel_web", StringComparison.OrdinalIgnoreCase))
        {
            return "libKernel";
        }
        return stem;
    }

    private static string NormalizeSharpProsperoLibrary(string library) =>
        library.Equals("libkernel", StringComparison.OrdinalIgnoreCase)
            ? "libKernel"
            : library.Replace("_native", ".native", StringComparison.Ordinal);

    private static string InferKytyLibrary(
        string initializer,
        string fileName,
        string handler)
    {
        if (fileName.Equals("libGraphicsDriver.cpp", StringComparison.OrdinalIgnoreCase))
        {
            return handler.Contains("Gen5Driver::", StringComparison.Ordinal)
                ? "libSceAgcDriver"
                : "libSceAgc";
        }

        var suffix = string.Empty;
        var match = KytyInitializerRegex().Match(initializer);
        if (match.Success)
        {
            suffix = match.Groups["suffix"].Value;
            if (!string.IsNullOrWhiteSpace(suffix))
            {
                return "libSce" + NormalizeKytyLibrarySuffix(suffix);
            }

            var root = match.Groups["root"].Value;
            if (root.Equals("LibKernel", StringComparison.Ordinal))
            {
                return "libKernel";
            }
            if (root.Equals("LibC", StringComparison.Ordinal))
            {
                return "libc";
            }
            if (root.Equals("LibcInternal", StringComparison.Ordinal))
            {
                return "libSceLibcInternal";
            }
            if (root.StartsWith("Lib", StringComparison.Ordinal))
            {
                root = root[3..];
            }
            return "libSce" + NormalizeKytyLibrarySuffix(root);
        }

        var stem = Path.GetFileNameWithoutExtension(fileName);
        if (stem.StartsWith("lib", StringComparison.OrdinalIgnoreCase))
        {
            stem = stem[3..];
        }
        return stem.Equals("Kernel", StringComparison.OrdinalIgnoreCase)
            ? "libKernel"
            : "libSce" + NormalizeKytyLibrarySuffix(stem);
    }

    private static string NormalizeKytyLibrarySuffix(string value) =>
        value.EndsWith("Native", StringComparison.Ordinal)
            ? value[..^"Native".Length] + ".native"
            : value;

    private static string HandlerLeafName(string handler)
    {
        var index = handler.LastIndexOf("::", StringComparison.Ordinal);
        return index < 0 ? handler : handler[(index + 2)..];
    }

    private static bool ValidExternalNid(string nid) =>
        ExternalNidRegex().IsMatch(nid);

    private static void AssertSelfTestCandidate(
        ExternalHleCatalog catalog,
        string nid,
        string evidence,
        string library)
    {
        if (!catalog.AllByNid.TryGetValue(nid, out var candidates) ||
            !candidates.Any(candidate =>
                candidate.Evidence.Equals(evidence, StringComparison.Ordinal) &&
                candidate.Library.Equals(library, StringComparison.Ordinal)))
        {
            throw new InvalidOperationException(
                $"HLE external catalog self-test failed for {evidence}:{nid}.");
        }
    }

    private static string RelativeSource(
        string root,
        string path,
        int lineNumber) =>
        $"{Path.GetFileName(root)}:" +
        $"{Path.GetRelativePath(root, path).Replace('\\', '/')}:" +
        $"{lineNumber}";

    private static string StripComments(string text) =>
        CommentRegex().Replace(text, " ");

    private static string CollapseWhitespace(string value) =>
        WhitespaceRegex().Replace(value, " ").Trim();

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

    [GeneratedRegex(
        @"_Fn_\s*\(\s*(?<return>[^,\r\n]+?)\s*,\s*" +
        @"(?<name>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*" +
        @"(?<parameters>.*?)\)\s*;",
        RegexOptions.Singleline | RegexOptions.CultureInvariant)]
    private static partial Regex Ps5SdkFunctionRegex();

    [GeneratedRegex(
        @"^[ \t]*(?<return>[A-Za-z_][A-Za-z0-9_ \t\*&]*?[ \t\*&])" +
        @"(?<name>(?:sce|_sce|pthread_|__)[A-Za-z_][A-Za-z0-9_]*)[ \t]*" +
        @"\((?<parameters>[^;{}]*)\)[ \t]*;",
        RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex CFunctionPrototypeRegex();

    [GeneratedRegex(
        @"LIB_FUNCTION\s*\(\s*""[^""]+""\s*,\s*" +
        @"""(?<library>[^""]+)""\s*,\s*\d+\s*,\s*" +
        @"""[^""]+""\s*,\s*(?<handler>[A-Za-z_][A-Za-z0-9_:]*)\s*\)",
        RegexOptions.CultureInvariant)]
    private static partial Regex ShadRegistrationRegex();

    [GeneratedRegex(
        @"(?<return>[A-Za-z_][A-Za-z0-9_:<>, \t\*&]*?)\s+" +
        @"PS4_SYSV_ABI\s+(?<name>[A-Za-z_][A-Za-z0-9_]*)\s*" +
        @"\((?<parameters>[^;{}]*)\)\s*(?:;|\{)",
        RegexOptions.CultureInvariant)]
    private static partial Regex ShadFunctionSignatureRegex();

    [GeneratedRegex(
        @"LIB_DEFINE\s*\(\s*(?<initializer>[A-Za-z_][A-Za-z0-9_]*)\s*\)",
        RegexOptions.CultureInvariant)]
    private static partial Regex KytyLibDefineRegex();

    [GeneratedRegex(
        @"LIB_FUNC\s*\(\s*""(?<nid>[A-Za-z0-9+\-]{11})""\s*,\s*" +
        @"(?<handler>[A-Za-z_][A-Za-z0-9_:]*)\s*\)",
        RegexOptions.CultureInvariant)]
    private static partial Regex KytyLibFunctionRegex();

    [GeneratedRegex(
        @"^Init(?<root>[A-Za-z0-9]+)_1(?:_(?<suffix>[A-Za-z0-9]+))?$",
        RegexOptions.CultureInvariant)]
    private static partial Regex KytyInitializerRegex();

    [GeneratedRegex(
        @"new\s+Entry\s*\(\s*""(?<library>[^""]+)""\s*,\s*" +
        @"(?<array>[A-Za-z_][A-Za-z0-9_]*)",
        RegexOptions.CultureInvariant)]
    private static partial Regex SharpProsperoEntryRegex();

    [GeneratedRegex(
        @"private\s+static\s+readonly\s+string\[\]\s+" +
        @"(?<array>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*\[(?<body>.*?)\];",
        RegexOptions.Singleline | RegexOptions.CultureInvariant)]
    private static partial Regex SharpProsperoArrayRegex();

    [GeneratedRegex(
        @"""(?<value>(?:\\.|[^""])*)""",
        RegexOptions.CultureInvariant)]
    private static partial Regex QuotedStringRegex();

    [GeneratedRegex(
        @"\.global\s+(?<name>[A-Za-z_][A-Za-z0-9_]*)\\n",
        RegexOptions.CultureInvariant)]
    private static partial Regex PayloadSdkGlobalRegex();

    [GeneratedRegex(
        @"/\*.*?\*/|//[^\r\n]*",
        RegexOptions.Singleline | RegexOptions.CultureInvariant)]
    private static partial Regex CommentRegex();

    [GeneratedRegex(@"\s+", RegexOptions.CultureInvariant)]
    private static partial Regex WhitespaceRegex();

    [GeneratedRegex(
        @"^[A-Za-z0-9+\-]{11}$",
        RegexOptions.CultureInvariant)]
    private static partial Regex ExternalNidRegex();
}

internal sealed record ExternalHleCandidate(
    string Nid,
    string Name,
    string Library,
    string Signature,
    string Source,
    string Evidence,
    int Confidence);

internal sealed record ExternalHleCatalog(
    IReadOnlyDictionary<string, ExternalHleCandidate> ByNid,
    IReadOnlyDictionary<string, IReadOnlyList<ExternalHleCandidate>> AllByNid,
    IReadOnlyDictionary<string, IReadOnlyList<ExternalHleCandidate>> Conflicts,
    int CandidateCount,
    int SourceCount);
