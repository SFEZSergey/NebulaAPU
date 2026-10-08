// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

namespace Ps5Recomp.Cli;

/// <summary>
/// Default locations of local tools and reference checkouts. They sit under
/// one root, NEBULA_DEV_ROOT (C:\dev when unset); every default can still be
/// overridden by its own option or environment variable.
/// </summary>
internal static class LocalPaths
{
    public static string DevRoot { get; } =
        Environment.GetEnvironmentVariable("NEBULA_DEV_ROOT") is { Length: > 0 } root
            ? root
            : @"C:\dev";

    public static string Dev(params string[] parts) =>
        Path.Combine(new[] { DevRoot }.Concat(parts).ToArray());
}
