// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only

using System.Text;
using System.Text.Json;

namespace Ps5Recomp.Cli;

/// <summary>
/// Unified JSONL trace schema for build and runtime diagnostics.
/// Compatible with SharpEmu reference traces for differential comparison.
/// Events: process_start, module_load, relocation, tls_registration,
///         thread_create, hle_enter, hle_exit, allocation, file_open,
///         basic_block, indirect_branch, shader_seen, pipeline_create,
///         draw, dispatch, barrier, present, exception, process_exit,
///         build_stage, build_error.
/// </summary>
internal static class TraceSchema
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = false,
    };

    public static void WriteEvent(
        StreamWriter writer,
        string eventType,
        string? detail = null,
        Dictionary<string, object>? fields = null)
    {
        var entry = new Dictionary<string, object>
        {
            ["ts"] = DateTime.UtcNow.ToString("O"),
            ["event"] = eventType,
        };

        if (detail is not null)
        {
            entry["detail"] = detail;
        }

        if (fields is not null)
        {
            foreach (var (key, value) in fields)
            {
                entry[key] = value;
            }
        }

        writer.WriteLine(JsonSerializer.Serialize(entry, JsonOptions));
    }

    public static void WriteBuildEvent(
        StreamWriter writer,
        string stage,
        string status,
        long elapsedMs = 0,
        string? error = null)
    {
        var fields = new Dictionary<string, object>
        {
            ["stage"] = stage,
            ["status"] = status,
        };

        if (elapsedMs > 0)
        {
            fields["elapsed_ms"] = elapsedMs;
        }

        if (error is not null)
        {
            fields["error"] = error;
        }

        WriteEvent(writer, "build_stage", fields: fields);
    }
}
