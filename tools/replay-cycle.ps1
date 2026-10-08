# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Capture,
    [string]$OutputRoot = (
        "$(if ($env:NEBULA_DEV_ROOT) { $env:NEBULA_DEV_ROOT } else { 'C:\dev' })\ps5recomp\artifacts\gpu-replay\matrix-" +
        (Get-Date -Format "yyyyMMdd-HHmmss")
    ),
    [ValidateRange(3, 300)]
    [int]$TimeoutSeconds = 30,
    [switch]$Matrix,
    [switch]$SkipBuild
)

# Root of the local tools and reference checkouts (C:\dev unless
# NEBULA_DEV_ROOT says otherwise).
$DevRoot = if ($env:NEBULA_DEV_ROOT) { $env:NEBULA_DEV_ROOT } else { 'C:\dev' }

$ErrorActionPreference = "Stop"
$dotnet = "$DevRoot\dotnet-sdk-10.0.103\dotnet.exe"
$sharpEmuRoot = "$DevRoot\SharpEmu-main"
$cacheRoot = "$DevRoot\ps5recomp\artifacts\cache"
$project = Join-Path $sharpEmuRoot `
    "src\SharpEmu.RenderReplay\SharpEmu.RenderReplay.csproj"

$env:DOTNET_ROOT = Split-Path -Parent $dotnet
$env:DOTNET_HOST_PATH = $dotnet
$env:DOTNET_CLI_HOME = "$DevRoot\dotnet-cli-home"
$env:LOCALAPPDATA = "$DevRoot\sharpemu\artifacts\localappdata"
$env:PATH = "$env:DOTNET_ROOT;$env:PATH"
$env:MSBUILDDISABLENODEREUSE = "1"
$env:MSBuildEnableWorkloadResolver = "false"
$env:SHARPEMU_VK_PIPELINE_CACHE_PATH = Join-Path `
    $cacheRoot "render-replay-vulkan-pipeline-cache.bin"

$Capture = [IO.Path]::GetFullPath($Capture)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath (Join-Path $Capture "manifest.json"))) {
    throw "Capture manifest is missing: $Capture"
}
$manifest = Get-Content -LiteralPath (Join-Path $Capture "manifest.json") -Raw |
    ConvertFrom-Json
$firstTextureAddress = if ($manifest.textures.Count -gt 0) {
    "0x{0:X16}" -f [uint64]$manifest.textures[0].address
} else {
    "*"
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null

if (-not $SkipBuild) {
    Write-Host "[build] SharpEmu.RenderReplay"
    & $dotnet build $project `
        -c Debug `
        -m:1 `
        --no-restore `
        -nodeReuse:false `
        -p:UseSharedCompilation=false `
        -p:NuGetAudit=false
    if ($LASTEXITCODE -ne 0) {
        throw "RenderReplay build failed with exit code $LASTEXITCODE"
    }
}

$variants = if ($Matrix) {
    @(
        [pscustomobject]@{ Name = "baseline"; Arguments = @() }
        [pscustomobject]@{
            Name = "solid-fragment"
            Arguments = @("--force-solid-fragment")
        }
        [pscustomobject]@{
            Name = "fullscreen-vertex"
            Arguments = @("--force-fullscreen-vertex")
        }
        [pscustomobject]@{
            Name = "cpu-detile"
            Arguments = @("--gpu-detile", "cpu")
        }
        [pscustomobject]@{
            Name = "copy-fragment"
            Arguments = @("--force-copy-fragment")
        }
        [pscustomobject]@{
            Name = "copy-fragment-cpu"
            Arguments = @("--force-copy-fragment", "--gpu-detile", "cpu")
        }
        [pscustomobject]@{
            Name = "white-texture"
            Arguments = @("--force-white-textures", $firstTextureAddress)
        }
        [pscustomobject]@{
            Name = "white-texture-cpu"
            Arguments = @(
                "--gpu-detile", "cpu",
                "--force-white-textures", $firstTextureAddress
            )
        }
        [pscustomobject]@{
            Name = "copy-white-cpu"
            Arguments = @(
                "--force-copy-fragment",
                "--gpu-detile", "cpu",
                "--force-white-textures", $firstTextureAddress
            )
        }
        [pscustomobject]@{
            Name = "attribute-fragment-0"
            Arguments = @("--force-attribute-fragment", "0")
        }
        [pscustomobject]@{
            Name = "fullscreen-pipeline"
            Arguments = @("--force-fullscreen-pipeline")
        }
    )
} else {
    @([pscustomobject]@{ Name = "baseline"; Arguments = @() })
}

$results = [Collections.Generic.List[object]]::new()
foreach ($variant in $variants) {
    $output = Join-Path $OutputRoot $variant.Name
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $arguments = @(
        "run",
        "--project", $project,
        "-c", "Debug",
        "--no-build",
        "--no-restore",
        "--",
        "--capture", $Capture,
        "--headless",
        "--output", $output,
        "--timeout-seconds", $TimeoutSeconds
    ) + $variant.Arguments

    Write-Host "[replay] $($variant.Name)"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $dotnet @arguments
    $exitCode = $LASTEXITCODE
    $watch.Stop()

    $summaryPath = Join-Path $output "summary.txt"
    $summary = @{}
    if (Test-Path -LiteralPath $summaryPath) {
        foreach ($line in Get-Content -LiteralPath $summaryPath) {
            $separator = $line.IndexOf("=")
            if ($separator -gt 0) {
                $summary[$line.Substring(0, $separator)] =
                    $line.Substring($separator + 1)
            }
        }
    }
    $results.Add([pscustomobject]@{
        Variant = $variant.Name
        ExitCode = $exitCode
        Seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 2)
        Success = $summary["success"]
        Width = $summary["width"]
        Height = $summary["height"]
        NonblackPixels = $summary["nonblack_pixels"]
        Sha256 = $summary["sha256"]
        Output = $output
    })
}

$results | Format-Table `
    Variant,ExitCode,Seconds,Success,Width,Height,NonblackPixels,Sha256 `
    -AutoSize
$results | Export-Csv `
    -LiteralPath (Join-Path $OutputRoot "matrix.csv") `
    -NoTypeInformation

[pscustomobject]@{
    OutputRoot = $OutputRoot
    Results = $results
}
