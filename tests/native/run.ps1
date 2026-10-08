# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

# Builds and runs every test of the native shader path.
#
# The runtime links nothing and is compiled as single translation units, so
# these are too: each test is one file against the headers it exercises.
# They are fast enough to run on every change, which is the point - the
# port is being judged by them until it is complete enough to be judged by
# a frame.
[CmdletBinding()]
param(
    [string]$Compiler = "",
    [switch]$KeepBinaries
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$native = Join-Path (Split-Path -Parent $here) "..\runtime\native"
$native = [IO.Path]::GetFullPath($native)

if ($Compiler -eq "") {
    $candidate = Get-Command g++ -ErrorAction SilentlyContinue
    if ($null -ne $candidate) {
        $Compiler = $candidate.Source
    } else {
        $winlibs = Join-Path $env:LOCALAPPDATA ("Microsoft\WinGet\Packages\" +
            "BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\" +
            "mingw64\bin\g++.exe")
        if (Test-Path -LiteralPath $winlibs) {
            $Compiler = $winlibs
        } else {
            throw "No g++ found; pass -Compiler with a path to one."
        }
    }
}

$failed = @()
foreach ($source in Get-ChildItem -Path $here -Filter "*_test.cpp" | Sort-Object Name) {
    $exe = Join-Path $env:TEMP ($source.BaseName + ".exe")
    & $Compiler -std=c++20 -O2 -Wall -Wextra -Werror -I $native $source.FullName -o $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("{0}: did not compile" -f $source.BaseName)
        $failed += $source.BaseName
        continue
    }
    & $exe | Out-Null
    if ($LASTEXITCODE -ne 0) {
        & $exe
        $failed += $source.BaseName
    } else {
        Write-Host ("{0}: ok" -f $source.BaseName)
    }
    if (-not $KeepBinaries) {
        Remove-Item -LiteralPath $exe -ErrorAction SilentlyContinue
    }
}

if ($failed.Count -ne 0) {
    throw ("failed: {0}" -f ($failed -join ", "))
}
Write-Host "all native shader path tests passed"
