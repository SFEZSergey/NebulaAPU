# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

[CmdletBinding()]
param(
    [string]$Package = "$(if ($env:NEBULA_DEV_ROOT) { $env:NEBULA_DEV_ROOT } else { 'C:\dev' })\ps5recomp\artifacts\astrobot-r26-build\package",
    [string]$App0 = $env:NEBULA_APP0,
    [ValidateRange(5, 1800)]
    [int]$ProbeSeconds = 45,
    [ValidateSet("auto", "d3d12", "vulkan", "none")]
    [string]$GpuBackend = "auto",
    [string]$ExecutableName = "AstroBot.cycle.exe",
    [string]$SharpEmuSourceRoot = "",
    [string]$SharpEmuPackagesRoot = "",
    [switch]$Headless,
    # Kept for old command lines: the native window is the only window now.
    [switch]$NativeWindow,
    [switch]$TraceGpu,
    [switch]$TraceDraws,
    [switch]$TraceFramePackets,
    [switch]$TraceHle,
    [switch]$LogFiber,
    [switch]$TraceFsRecovery,
    [int]$HleStallMs = 500,
    [switch]$NativeGpuShadow,
    [switch]$NativeVideoOut,
    [switch]$NativeGpuRuntime,
    [switch]$NativeGpuCapture,
    [ValidateRange(0, 100000)]
    [int]$NativeGpuCaptureFlip = 0,
    [string]$NativeFrameDumpAddress = "",
    [ValidateRange(0, 16384)]
    [int]$NativeFrameDumpWidth = 0,
    [ValidateRange(0, 16384)]
    [int]$NativeFrameDumpHeight = 0,
    [ValidateRange(0, 4096)]
    [int]$NativeFrameDumpState = 0,
    [ValidateRange(0, 100000)]
    [int]$NativeFrameDumpFlip = 0,
    [string]$NativeVideoOutNids = "",
    [switch]$NativeRealComposition,
    [ValidateSet(
        "real",
        "real-es-green",
        "fullscreen-real-ps",
        "fullscreen-copy-source")]
    [string]$NativeRealCompositionMode = "real",
    [ValidateSet(
        "real",
        "real-es-green",
        "fullscreen-real-ps",
        "fullscreen-copy-source")]
    [string]$NativeState29Mode = "real",
    [ValidateRange(-1, 3)]
    [int]$NativeState28CopyImage = -1,
    [ValidateSet(-1, 20, 21, 22, 23, 24, 25, 26, 27)]
    [int]$NativePostCopyState = -1,
    [ValidateRange(-1, 4096)]
    [int]$NativeSampleCopyState = -1,
    [ValidateRange(1, 4096)]
    [int]$NativeLiveGraphicsMinState = 35,
    [switch]$NativeCompileGraphics,
    [switch]$ForceSolidFragment,
    [switch]$CaptureFrame,
    [switch]$ForceGpuWaits,
    [switch]$StopOnFirstFrame,
    [ValidateRange(0, 100000)]
    [int]$StopAfterNativeFlip = 0,
    [ValidateRange(0, 300)]
    [int]$StopAfterFlipStallSeconds = 0,
    [switch]$StopOnComputeReadback,
    [switch]$ForceBuild,
    # The title only reaches its asset path when it is started with the
    # command line from args.txt, so this is on by default: a run without it
    # loads nothing and every later measurement is of the loading screen.
    [bool]$PackageArgs = $true,
    # Wiping the compute shader cache belongs to runs that measure compile
    # cost. It used to be done by hand in every probe wrapper, which made
    # each one recompile everything before it could measure anything else.
    [switch]$ClearShaderCache,
    [bool]$BufferPool = $true,
    [switch]$TraceTime,
    [switch]$TraceAllDraws,
    [switch]$TraceDrawInputs,
    [switch]$TraceSubmits,
    [switch]$TraceApr,
    [switch]$TraceAssetCalls,
    [switch]$TraceIo,
    [switch]$TraceAlloc,
    [switch]$TraceHleCalls,
    [switch]$TraceDmaData,
    [switch]$TraceDrawPackets,
    [switch]$TraceRegisterWrites,
    [ValidateRange(0, 1000000)]
    [int]$NativeFrameDumpDraw = 0,
    [ValidateRange(0, 32)]
    [int]$NativeFrameDumpBpp = 0,
    [switch]$NativeFrameDumpReadCopy,
    [ValidateRange(-1, 1)]
    [int]$RetireCompute = -1,
    [ValidateRange(0, 65536)]
    [int]$RetireKeep = 0,
    [ValidateRange(0, 65536)]
    [int]$ComputeStateCap = 0,
    [ValidateRange(-1, 1)]
    [int]$WriteMask = -1,
    [switch]$EagerCommit,
    [switch]$AgcCountIsDwords,
    [switch]$AgcNoDefaultState,
    [switch]$ComputeScalarBlock,
    [switch]$ForceBlend,
    [switch]$NoMaskSkip,
    [string]$ForceFragmentStates = "",
    [switch]$LiveBufferRebase,
    [string]$RealEsBufferMode = "",
    [ValidateRange(0, 1000000)]
    [int]$CaptureDrawThreshold = 0,
    [ValidateRange(0, 100000)]
    [int]$CaptureFlips = 0,
    [ValidateRange(0, 131072)]
    [int]$CaptureMemoryMb = 0
)

# Root of the local tools and reference checkouts (C:\dev unless
# NEBULA_DEV_ROOT says otherwise).
$DevRoot = if ($env:NEBULA_DEV_ROOT) { $env:NEBULA_DEV_ROOT } else { 'C:\dev' }

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$nativeDir = Join-Path $projectRoot "runtime\native"
$generatedDir = Join-Path $Package "generated"
$generatedHleDir = Join-Path $generatedDir "hle"
$packageLeaf = Split-Path -Leaf $Package
$packageTag = if ($packageLeaf -ieq "package") {
    Split-Path -Leaf (Split-Path -Parent $Package)
} else {
    $packageLeaf
}
$packageTag = $packageTag -replace "[^A-Za-z0-9._-]", "_"
$buildDir = Join-Path $projectRoot "artifacts\build\cycle-$packageTag"
$probeRoot = Join-Path $projectRoot "artifacts\probes"
$cacheRoot = Join-Path $projectRoot "artifacts\cache"
$liveShaderCacheDir = Join-Path $cacheRoot "$packageTag-live-shaders"
$exePath = Join-Path $Package $ExecutableName
$nativeShaderDir = Join-Path $Package "shaders"
$runtimeImportLibrary = Join-Path $Package "libPs5Runtime.dll.a"
$bridgeProject = Join-Path $projectRoot "bridge\SharpEmu.HleBridge\SharpEmu.HleBridge.csproj"
$bridgeSource = Join-Path $projectRoot "bridge\SharpEmu.HleBridge\BridgeExports.cs"
$bridgeMemorySource = Join-Path $projectRoot "bridge\SharpEmu.HleBridge\DirectGuestMemory.cs"
$bridgePublishDir = Join-Path $projectRoot "artifacts\hle-bridge-main\win-x64"
$bridgeAssets = Join-Path $projectRoot `
    "bridge\SharpEmu.HleBridge\obj\project.assets.json"
$gpuBridgeProject = Join-Path $projectRoot `
    "bridge\Ps5Recomp.GpuBridge\Ps5Recomp.GpuBridge.csproj"
$gpuBridgeSource = Join-Path $projectRoot `
    "bridge\Ps5Recomp.GpuBridge\GpuBridgeExports.cs"
$gpuBridgeMemorySource = Join-Path $projectRoot `
    "bridge\Ps5Recomp.GpuBridge\DirectGuestShaderMemory.cs"
$gpuBridgePublishDir = Join-Path $projectRoot `
    "artifacts\gpu-bridge-main\win-x64"
$packageGpuBridge = Join-Path $Package "Ps5GpuBridge.dll"
$policyTemplate = Join-Path $projectRoot "config\astrobot-hle-policy.json"
$policyPath = Join-Path $Package "ps5recomp-hle-policy.json"
$dotnet = "$DevRoot\dotnet-sdk-10.0.103\dotnet.exe"
$sharpEmuRoot = $SharpEmuSourceRoot
if ([string]::IsNullOrWhiteSpace($sharpEmuRoot)) {
    $sharpEmuRoot = @(
        "$DevRoot\SharpEmu-upstream-latest"
        "$DevRoot\SharpEmu-main"
        "$DevRoot\sharpemu"
    ) | Where-Object {
        Test-Path -LiteralPath (
            Join-Path $_ "src\SharpEmu.Libs\SharpEmu.Libs.csproj")
    } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($sharpEmuRoot)) {
    throw "No usable SharpEmu source tree was found."
}
$sharpEmuPackages = $SharpEmuPackagesRoot
if ([string]::IsNullOrWhiteSpace($sharpEmuPackages)) {
    $sharpEmuPackages = @(
        (Join-Path $sharpEmuRoot ".packages")
        "$DevRoot\SharpEmu-main\.packages"
        "$DevRoot\sharpemu\.packages"
    ) | Where-Object {
        Test-Path -LiteralPath $_
    } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($sharpEmuPackages)) {
    $sharpEmuPackages = Join-Path $sharpEmuRoot ".packages"
}
$sharpEmuProject = Join-Path $sharpEmuRoot "src\SharpEmu.Libs\SharpEmu.Libs.csproj"
$sharpEmuBin = Join-Path $sharpEmuRoot "artifacts\bin\Debug\net10.0"
$sharpEmuLib = Join-Path $sharpEmuBin "SharpEmu.Libs.dll"
$sharpEmuAssets = Join-Path $sharpEmuRoot `
    "artifacts\obj\SharpEmu.Libs\project.assets.json"
$packageHleBridge = Join-Path $Package "Ps5HleBridge.dll"
$sdlNativeSource = Join-Path $sharpEmuPackages `
    "ppy.sdl3-cs\2026.629.0\runtimes\win-x64\native\SDL3.dll"
$packageSdlNative = Join-Path $Package "SDL3.dll"

$env:DOTNET_ROOT = Split-Path -Parent $dotnet
$env:DOTNET_HOST_PATH = $dotnet
$env:DOTNET_CLI_HOME = "$DevRoot\dotnet-cli-home"
$env:LOCALAPPDATA = "$DevRoot\sharpemu\artifacts\localappdata"
$env:PATH = "$env:DOTNET_ROOT;$env:PATH"
$env:MSBUILDDISABLENODEREUSE = "1"
$env:MSBuildEnableWorkloadResolver = "false"

if ([string]::IsNullOrWhiteSpace($App0)) {
    throw "Pass -App0 or set NEBULA_APP0 to the folder of your own extracted dump."
}
$requiredFiles = @(
    $Package,
    $App0,
    (Join-Path $Package "Ps5Runtime.dll"),
    $packageHleBridge,
    $packageGpuBridge,
    $runtimeImportLibrary,
    $bridgeProject,
    $bridgeSource,
    $bridgeMemorySource,
    $gpuBridgeProject,
    $gpuBridgeSource,
    $gpuBridgeMemorySource,
    $policyTemplate,
    $dotnet,
    $sharpEmuProject,
    $sdlNativeSource,
    (Join-Path $generatedDir "runtime_guest_image.cpp"),
    (Join-Path $generatedDir "runtime_guest_image.h"),
    (Join-Path $generatedHleDir "hle_registry.generated.cpp"),
    (Join-Path $generatedHleDir "hle_registry.generated.h"),
    (Join-Path $generatedHleDir "hle_bindings.generated.inc"),
    (Join-Path $nativeDir "ps5rt_runner.cpp"),
    (Join-Path $nativeDir "ps5rt_hle_impl.cpp"),
    (Join-Path $nativeDir "ps5gpu_native_api.h"),
    (Join-Path $nativeDir "ps5gpu_native.cpp"),
    (Join-Path $nativeDir "ps5gpu_capture.h"),
    (Join-Path $nativeDir "ps5gpu_fixed_spirv.h"),
    (Join-Path $nativeDir "ps5gpu_replay.cpp")
)
foreach ($path in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required path is missing: $path"
    }
}

# The ILCompiler link step shells out to vswhere.exe to locate the MSVC
# toolchain. It is not on PATH by default, and its absence surfaces as an
# opaque MSB3073 exit code 123 from Microsoft.NETCore.Native.targets.
if (-not (Get-Command vswhere.exe -ErrorAction SilentlyContinue)) {
    $vsInstaller = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer"
    if (Test-Path -LiteralPath (Join-Path $vsInstaller "vswhere.exe")) {
        $env:PATH = "$vsInstaller;$env:PATH"
    } else {
        throw "vswhere.exe is missing: $vsInstaller"
    }
}

$compiler = (Get-Command g++.exe -ErrorAction Stop).Source
$vulkanIncludeDir = "$DevRoot\Kyty-source\source\3rdparty\vulkan\include"
if (-not (Test-Path -LiteralPath $vulkanIncludeDir)) {
    throw "Vulkan headers are missing: $vulkanIncludeDir"
}
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
if ($ClearShaderCache) {
    # Every cache, not just the compute one. The live shader directory
    # holds whole modules and survives a change of translator settings, so
    # a run made with an experiment in place leaves its modules behind and
    # the next run loads them - which cost two rounds of diagnosing a
    # "broken environment" that was a stale module crashing the driver on
    # load. The pipeline cache is keyed by the driver rather than by us and
    # goes for the same reason.
    foreach ($stale in @(
        (Join-Path $cacheRoot "$packageTag-compute-shader-cache-v1"),
        (Join-Path $cacheRoot "$packageTag-live-shaders"),
        (Join-Path $cacheRoot "$packageTag-vulkan-pipeline-cache.bin"),
        (Join-Path $cacheRoot "$packageTag-native-vulkan-pipeline-cache.bin")
    )) {
        if (Test-Path -LiteralPath $stale) {
            Remove-Item -Recurse -Force -LiteralPath $stale
            Write-Host "cleared: $stale"
        }
    }
}
New-Item -ItemType Directory -Path $liveShaderCacheDir -Force | Out-Null
Copy-Item -LiteralPath $sdlNativeSource -Destination $packageSdlNative -Force

function Invoke-NativeTool {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [Parameter(Mandatory)]
        [string]$WorkingDirectory,
        [Parameter(Mandatory)]
        [string]$LogPath,
        [int]$TimeoutSeconds = 120
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not start: $FilePath"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw "Command timed out after $TimeoutSeconds seconds: $FilePath"
    }

    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $commandLine = $FilePath + " " + ($Arguments -join " ")
    $log = @(
        "command=$commandLine"
        "exit_code=$($process.ExitCode)"
        ""
        $stdout.TrimEnd()
        $stderr.TrimEnd()
    ) -join [Environment]::NewLine
    [IO.File]::WriteAllText(
        $LogPath,
        $log,
        [Text.UTF8Encoding]::new($false))

    if ($process.ExitCode -ne 0) {
        $tail = ($log -split "\r?\n" | Select-Object -Last 40) -join "`n"
        throw "Native tool failed with exit code $($process.ExitCode):`n$tail"
    }
}

function Test-NeedsBuild {
    param(
        [Parameter(Mandatory)]
        [string]$Output,
        [Parameter(Mandatory)]
        [string[]]$Dependencies
    )

    if ($ForceBuild -or -not (Test-Path -LiteralPath $Output)) {
        return $true
    }
    $outputTime = (Get-Item -LiteralPath $Output).LastWriteTimeUtc
    foreach ($dependency in $Dependencies) {
        if ((Get-Item -LiteralPath $dependency).LastWriteTimeUtc -gt $outputTime) {
            return $true
        }
    }
    return $false
}

$sharpEmuDependencies = @(
    (Join-Path $sharpEmuRoot "Directory.Build.props"),
    (Join-Path $sharpEmuRoot "Directory.Packages.props"),
    $sharpEmuProject
) + @(
    Get-ChildItem -LiteralPath (Join-Path $sharpEmuRoot "src") -Recurse -File |
        Where-Object { $_.Extension -in ".cs", ".csproj", ".props", ".targets" } |
        ForEach-Object FullName
)
if ($ForceBuild -or -not (Test-Path -LiteralPath $sharpEmuAssets)) {
    Write-Host "[restore] SharpEmu.Libs"
    Invoke-NativeTool -FilePath $dotnet -WorkingDirectory $sharpEmuRoot `
        -LogPath (Join-Path $buildDir "SharpEmu.restore.log") `
        -TimeoutSeconds 600 `
        -Arguments @(
            "restore", $sharpEmuProject,
            "--packages", $sharpEmuPackages,
            "-m:1",
            "-nodeReuse:false",
            "-p:BuildInParallel=false",
            "-p:RestorePackagesPath=$sharpEmuPackages",
            "-p:RestoreIgnoreFailedSources=true"
        )
}
if (Test-NeedsBuild -Output $sharpEmuLib -Dependencies $sharpEmuDependencies) {
    Write-Host "[build] SharpEmu.Libs.dll"
    Invoke-NativeTool -FilePath $dotnet -WorkingDirectory $sharpEmuRoot `
        -LogPath (Join-Path $buildDir "SharpEmu.Libs.log") `
        -TimeoutSeconds 600 `
        -Arguments @(
            "build", $sharpEmuProject,
            "-c", "Debug",
            "--no-restore",
            "-m:1",
            "-nodeReuse:false",
            "-p:RestorePackagesPath=$sharpEmuPackages",
            "-p:UseSharedCompilation=false"
        )
}
foreach ($path in @(
    (Join-Path $sharpEmuBin "SharpEmu.HLE.dll"),
    $sharpEmuLib
)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "SharpEmu build output is missing: $path"
    }
}

$sharpEmuRuntimeAssemblies = @(
    Get-ChildItem -LiteralPath $sharpEmuBin -Filter "*.dll" -File |
        ForEach-Object FullName
)
$bridgeSourceFiles = @(
    Get-ChildItem -LiteralPath (Split-Path -Parent $bridgeProject) `
        -Filter "*.cs" -File |
        ForEach-Object FullName
)
$bridgeDependencies = @($bridgeProject) +
    $bridgeSourceFiles +
    $sharpEmuRuntimeAssemblies
if (Test-NeedsBuild -Output $packageHleBridge -Dependencies $bridgeDependencies) {
    if ($ForceBuild -or (Test-NeedsBuild `
            -Output $bridgeAssets `
            -Dependencies @($bridgeProject))) {
        Write-Host "[restore] Ps5HleBridge.dll"
        Invoke-NativeTool -FilePath $dotnet -WorkingDirectory $projectRoot `
            -LogPath (Join-Path $buildDir "Ps5HleBridge.restore.log") `
            -TimeoutSeconds 600 `
            -Arguments @(
                "restore", $bridgeProject,
                "-r", "win-x64",
                "--packages", $sharpEmuPackages,
                "-m:1",
                "-nodeReuse:false",
                "-p:BuildInParallel=false",
                "-p:RestorePackagesPath=$sharpEmuPackages",
                "-p:RestoreIgnoreFailedSources=true",
                "-p:SharpEmuRoot=$sharpEmuRoot",
                "-p:SharpEmuBin=$sharpEmuBin"
            )
    }
    Write-Host "[build] Ps5HleBridge.dll"
    New-Item -ItemType Directory -Path $bridgePublishDir -Force | Out-Null
    Invoke-NativeTool -FilePath $dotnet -WorkingDirectory $projectRoot `
        -LogPath (Join-Path $buildDir "Ps5HleBridge.log") `
        -TimeoutSeconds 600 `
        -Arguments @(
            "publish", $bridgeProject,
            "-c", "Release",
            "-r", "win-x64",
            "--self-contained", "true",
            "--no-restore",
            "-m:1",
            "-nodeReuse:false",
            "-o", $bridgePublishDir,
            "-p:UseSharedCompilation=false",
            "-p:SharpEmuRoot=$sharpEmuRoot",
            "-p:SharpEmuBin=$sharpEmuBin"
        )
    Copy-Item -LiteralPath (Join-Path $bridgePublishDir "Ps5HleBridge.dll") `
        -Destination $packageHleBridge -Force
}

$gpuBridgeDependencies = @(
    $gpuBridgeProject,
    $gpuBridgeSource,
    $gpuBridgeMemorySource
) + @(
    Get-ChildItem -LiteralPath `
        (Join-Path $projectRoot "src\Ps5Recomp.ShaderCompiler") `
        -Recurse -File |
        Where-Object { $_.Extension -in ".cs", ".csproj" } |
        ForEach-Object FullName
) + @(
    Get-ChildItem -LiteralPath `
        (Join-Path $projectRoot "src\Ps5Recomp.ShaderCompiler.Vulkan") `
        -Recurse -File |
        Where-Object { $_.Extension -in ".cs", ".csproj" } |
        ForEach-Object FullName
)
if (Test-NeedsBuild `
        -Output $packageGpuBridge `
        -Dependencies $gpuBridgeDependencies) {
    Write-Host "[build] Ps5GpuBridge.dll"
    New-Item -ItemType Directory -Path $gpuBridgePublishDir -Force |
        Out-Null
    Invoke-NativeTool -FilePath $dotnet -WorkingDirectory $projectRoot `
        -LogPath (Join-Path $buildDir "Ps5GpuBridge.log") `
        -TimeoutSeconds 600 `
        -Arguments @(
            "publish", $gpuBridgeProject,
            "-c", "Release",
            "-r", "win-x64",
            "--self-contained", "true",
            "--no-restore",
            "-m:1",
            "-nodeReuse:false",
            "-o", $gpuBridgePublishDir,
            "-p:UseSharedCompilation=false"
        )
    Copy-Item -LiteralPath `
        (Join-Path $gpuBridgePublishDir "Ps5GpuBridge.dll") `
        -Destination $packageGpuBridge -Force
}

$commonHeaders = @(
    (Join-Path $nativeDir "ps5gpu_bridge_api.h"),
    (Join-Path $nativeDir "ps5gpu_native_api.h"),
    (Join-Path $nativeDir "ps5rt_api.h"),
    (Join-Path $nativeDir "ps5rt_hle_impl.h"),
    (Join-Path $nativeDir "ps5rt_avplayer.h"),
    (Join-Path $nativeDir "ps5rt_json.h"),
    (Join-Path $nativeDir "ps5rt_shared.h")
) + @(
    # The shader translator is header-only and compiled into the HLE; a
    # change to it alone used to leave the old translator in the build.
    Get-ChildItem -LiteralPath $nativeDir -File |
        Where-Object { $_.Name -like "gen5_*.h" -or $_.Name -eq "spirv_builder.h" } |
        ForEach-Object FullName
)
$hleSource = Join-Path $nativeDir "ps5rt_hle_impl.cpp"
$runnerSource = Join-Path $nativeDir "ps5rt_runner.cpp"
$nativeGpuSource = Join-Path $nativeDir "ps5gpu_native.cpp"
$nativeGpuHeader = Join-Path $nativeDir "ps5gpu_native_api.h"
$nativeGpuCaptureHeader = Join-Path $nativeDir "ps5gpu_capture.h"
$nativeGpuFixedSpirvHeader = Join-Path $nativeDir "ps5gpu_fixed_spirv.h"
$nativeGpuGen5PreflightHeader = Join-Path $nativeDir "ps5gpu_gen5_preflight.h"
$nativeGpuReplaySource = Join-Path $nativeDir "ps5gpu_replay.cpp"
$nativeGpuDll = Join-Path $Package "Ps5GpuRuntime.dll"
$nativeGpuReplayDir = Join-Path $projectRoot "artifacts\tools"
$nativeGpuReplayExe = Join-Path $nativeGpuReplayDir "Ps5GpuReplay.exe"
New-Item -ItemType Directory -Path $nativeGpuReplayDir -Force | Out-Null
$runtimeGuestSource = Join-Path $generatedDir "runtime_guest_image.cpp"
$runtimeGuestHeader = Join-Path $generatedDir "runtime_guest_image.h"
$registrySource = Join-Path $generatedHleDir "hle_registry.generated.cpp"
$registryHeader = Join-Path $generatedHleDir "hle_registry.generated.h"
$bindingsSource = Join-Path $generatedHleDir "hle_bindings.generated.inc"

$hleObject = Join-Path $buildDir "ps5rt_hle_impl.o"
$runnerObject = Join-Path $buildDir "ps5rt_runner.o"
$runtimeGuestObject = Join-Path $buildDir "runtime_guest_image.o"
$registryObject = Join-Path $buildDir "hle_registry.generated.o"

$compileBase = @(
    "-std=c++20",
    "-O2",
    "-Wall",
    "-Wextra",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    "-mfsgsbase"
)

if (Test-NeedsBuild -Output $nativeGpuDll `
        -Dependencies @(
            $nativeGpuSource,
            (Join-Path $nativeDir "ps5gpu_bridge_api.h"),
            $nativeGpuHeader,
            $nativeGpuCaptureHeader,
            $nativeGpuFixedSpirvHeader,
            $nativeGpuGen5PreflightHeader,
            (Join-Path $nativeDir "ps5gpu_window.h")
        )) {
    Write-Host "[build] Ps5GpuRuntime.dll"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $Package `
        -LogPath (Join-Path $buildDir "Ps5GpuRuntime.log") `
        -Arguments @(
            "-std=c++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-DWIN32_LEAN_AND_MEAN",
            "-DNOMINMAX",
            "-DPS5GPU_NATIVE_BUILD=1",
            "-I", $vulkanIncludeDir,
            "-shared",
            "-pthread",
            "-static-libgcc",
            "-static-libstdc++",
            $nativeGpuSource,
            "-o", $nativeGpuDll
        )
} else {
    Write-Host "[reuse] $nativeGpuDll"
}

if (Test-NeedsBuild -Output $nativeGpuReplayExe `
        -Dependencies @(
            $nativeGpuReplaySource,
            (Join-Path $nativeDir "ps5gpu_bridge_api.h"),
            $nativeGpuHeader,
            $nativeGpuCaptureHeader
        )) {
    Write-Host "[build] Ps5GpuReplay.exe"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $projectRoot `
        -LogPath (Join-Path $buildDir "Ps5GpuReplay.log") `
        -Arguments @(
            "-std=c++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-DWIN32_LEAN_AND_MEAN",
            "-DNOMINMAX",
            "-static-libgcc",
            "-static-libstdc++",
            $nativeGpuReplaySource,
            "-o", $nativeGpuReplayExe
        )
} else {
    Write-Host "[reuse] $nativeGpuReplayExe"
}

# The AGC default register state is recovered from the game's own copy of
# Sony's library. The header is generated into the package rather than checked
# in, so no data derived from that library enters the repository.
$agcLibrary = Join-Path $App0 "fakelib\libSceAgc.sprx"
$agcDefaultsHeader = Join-Path $generatedDir "agc_default_state.h"
$agcDefaultsTool = Join-Path $projectRoot "tools\agc_extract_defaults.py"
if (-not (Test-Path -LiteralPath $agcLibrary)) {
    throw "libSceAgc.sprx is missing: $agcLibrary"
}
if (Test-NeedsBuild -Output $agcDefaultsHeader `
        -Dependencies @($agcLibrary, $agcDefaultsTool)) {
    Write-Host "[generate] agc_default_state.h"
    $python = (Get-Command python -ErrorAction Stop).Source
    Invoke-NativeTool -FilePath $python -WorkingDirectory $projectRoot `
        -LogPath (Join-Path $buildDir "agc_default_state.log") `
        -Arguments @($agcDefaultsTool, $agcLibrary, $agcDefaultsHeader)
}

if (Test-NeedsBuild -Output $hleObject -Dependencies (@($hleSource, $agcDefaultsHeader) + $commonHeaders)) {
    Write-Host "[build] ps5rt_hle_impl.o"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $Package `
        -LogPath (Join-Path $buildDir "ps5rt_hle_impl.log") `
        -Arguments ($compileBase + @(
            "-fno-exceptions",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
            "-I", $generatedDir,
            "-c", $hleSource,
            "-o", $hleObject
        ))
}

if (Test-NeedsBuild -Output $runtimeGuestObject `
        -Dependencies @($runtimeGuestSource, $runtimeGuestHeader)) {
    Write-Host "[build] runtime_guest_image.o"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $Package `
        -LogPath (Join-Path $buildDir "runtime_guest_image.log") `
        -Arguments @(
            "-std=c++20", "-O2", "-Wall", "-Wextra",
            "-c", $runtimeGuestSource,
            "-o", $runtimeGuestObject
        )
}

if (Test-NeedsBuild -Output $registryObject `
        -Dependencies @($registrySource, $registryHeader)) {
    Write-Host "[build] hle_registry.generated.o"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $Package `
        -LogPath (Join-Path $buildDir "hle_registry.generated.log") `
        -Arguments @(
            "-std=c++20", "-O2", "-Wall", "-Wextra",
            "-c", $registrySource,
            "-o", $registryObject
        )
}

$runnerDependencies = @(
    $runnerSource,
    $runtimeGuestHeader,
    $registryHeader,
    $bindingsSource
) + $commonHeaders
if (Test-NeedsBuild -Output $runnerObject -Dependencies $runnerDependencies) {
    Write-Host "[build] ps5rt_runner.o"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $Package `
        -LogPath (Join-Path $buildDir "ps5rt_runner.log") `
        -Arguments ($compileBase + @(
            "-DPS5RT_HAS_GENERATED_HLE=1",
            "-c", $runnerSource,
            "-I", $generatedDir,
            "-I", $generatedHleDir,
            "-o", $runnerObject
        ))
}

$linkDependencies = @(
    $runtimeGuestObject,
    $registryObject,
    $runnerObject,
    $hleObject,
    $runtimeImportLibrary
)
if (Test-NeedsBuild -Output $exePath -Dependencies $linkDependencies) {
    Write-Host "[link] $exePath"
    Invoke-NativeTool -FilePath $compiler -WorkingDirectory $Package `
        -LogPath (Join-Path $buildDir "link.log") `
        -Arguments @(
            "-std=c++20",
            "-O2",
            "-static-libgcc",
            "-static-libstdc++",
            "-mfsgsbase",
            $runtimeGuestObject,
            $registryObject,
            $runnerObject,
            $hleObject,
            $runtimeImportLibrary,
            "-luser32",
            # Media Foundation, for the intro video.
            "-lmfplat",
            "-lmfreadwrite",
            "-lmfuuid",
            "-lole32",
            "-lshlwapi",
            "-lwinmm",
            "-o", $exePath
        )
    # The sampler prints sites as module+0xOFFSET, and those offsets only
    # mean anything against the exact binary that produced them. Every
    # relink moves them. Dumping the symbol table beside the exe keeps a
    # run readable afterwards: nm survives the link even without -g, so
    # this costs a second and needs no debug build.
    #   python tools/sample_symbols.py --binary <exe> 0x28BD7
    $nm = Get-Command nm.exe -ErrorAction SilentlyContinue
    if ($nm) {
        $symbolPath = [System.IO.Path]::ChangeExtension($exePath, ".nm")
        & $nm.Source --numeric-sort $exePath |
            Set-Content -LiteralPath $symbolPath -Encoding ascii
    }
} else {
    Write-Host "[reuse] $exePath"
}

if (-not (Test-Path -LiteralPath $policyPath)) {
    Copy-Item -LiteralPath $policyTemplate -Destination $policyPath
}

$probeName = "astro-cycle-" + (Get-Date -Format "yyyyMMdd-HHmmss")
$probeDir = Join-Path $probeRoot $probeName
New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
$stdoutPath = Join-Path $probeDir "stdout.log"
$stderrPath = Join-Path $probeDir "stderr.log"
$summaryPath = Join-Path $probeDir "summary.txt"

$env:PS5RECOMP_HLE_BRIDGE = Join-Path $Package "Ps5HleBridge.dll"
$env:PS5RECOMP_HLE_POLICY = $policyPath
$env:PS5RECOMP_HLE_PERMISSIVE = "1"
$env:PS5RECOMP_GPU_BRIDGE = Join-Path $Package "Ps5GpuBridge.dll"
$env:PS5GPU_SHADER_CACHE_DIR = Join-Path $cacheRoot `
    "$packageTag-compute-shader-cache-v1"
$env:SHARPEMU_SAVEDATA_DIR = "$DevRoot\sharpemu\artifacts\saves"
$env:SHARPEMU_IGNORE_INT41 = "1"
$env:PS5RT_IGNORE_INT41 = "1"
$env:SHARPEMU_LOG_VIDEOOUT = "1"
$env:SHARPEMU_LOG_AGC = if ($TraceGpu) { "1" } else { "0" }
$env:SHARPEMU_LOG_AGC_SHADER = if ($TraceGpu) { "1" } else { "0" }
$env:PS5GPU_NATIVE_COMPUTE_READBACK = if ($TraceGpu) { "1" } else { "0" }
$env:SHARPEMU_TRACE_DRAWS = if ($TraceDraws) { "1" } else { "0" }
$env:SHARPEMU_TRACE_VK_DRAW_STATE = if ($TraceDraws) { "1" } else { "0" }
$env:SHARPEMU_TRACE_FRAME_PACKETS = if ($TraceFramePackets) { "1" } else { "0" }
$env:PS5RECOMP_TRACE_HLE = if ($TraceHle) { "1" } else { "0" }
$env:PS5RT_NATIVE_GPU_SHADOW = if ($NativeGpuShadow) { "1" } else { "0" }
$env:PS5RT_NATIVE_VIDEOOUT = if ($NativeVideoOut) { "1" } else { "0" }
$env:PS5RT_NATIVE_GPU_RUNTIME = if ($NativeGpuRuntime) { "1" } else { "0" }
# The native runtime's window is the title's window. The managed presenter
# shows only what the managed renderer drew, so it always runs headless;
# -Headless now means no window at all, for probes.
$env:PS5GPU_NATIVE_WINDOW = if ($Headless) { "0" } else { "1" }
$env:PS5RT_NATIVE_VIDEOOUT_NIDS = $NativeVideoOutNids
$env:PS5GPU_NATIVE_REAL_COMPOSITION = if ($NativeRealComposition) {
    "1"
} else {
    "0"
}
$env:PS5GPU_NATIVE_REAL_COMPOSITION_MODE = $NativeRealCompositionMode
$env:PS5GPU_NATIVE_ASTRO_STATE29_MODE = $NativeState29Mode
$env:PS5GPU_NATIVE_ASTRO_STATE28_COPY_IMAGE = [string]$NativeState28CopyImage
$env:PS5GPU_NATIVE_ASTRO_POST_COPY_STATE = [string]$NativePostCopyState
$env:PS5GPU_NATIVE_SAMPLE_COPY_STATE = [string]$NativeSampleCopyState
$env:PS5GPU_NATIVE_LIVE_GRAPHICS_MIN_STATE = [string]$NativeLiveGraphicsMinState
$env:PS5GPU_NATIVE_LIVE_GRAPHICS = if ($NativeCompileGraphics) {
    "1"
} else {
    "0"
}
$env:PS5GPU_NATIVE_LIVE_SHADER_CACHE_DIR = $liveShaderCacheDir
$env:PS5GPU_NATIVE_SHADER_DIR = if (
    (Test-Path -LiteralPath (Join-Path $nativeShaderDir "state-30-es.spv")) -and
    (Test-Path -LiteralPath (Join-Path $nativeShaderDir "state-30-ps.spv"))
) {
    $nativeShaderDir
} else {
    ""
}
$env:PS5GPU_NATIVE_CAPTURE_PATH = if ($NativeGpuCapture) {
    Join-Path $probeDir "native-gpu-frame.bin"
} else {
    ""
}
$env:PS5GPU_NATIVE_CAPTURE_FLIP = [string]$NativeGpuCaptureFlip
$env:PS5GPU_NATIVE_FRAME_DUMP_PATH = if (
    $CaptureFrame -or
    $NativeFrameDumpAddress -ne "" -or
    $NativeFrameDumpState -ne 0
) {
    Join-Path $probeDir "native-frame-dump.rgba"
} else {
    ""
}
$dumpRequested = (
    $CaptureFrame -or
    $NativeFrameDumpAddress -ne "" -or
    $NativeFrameDumpState -ne 0 -or
    $NativeFrameDumpFlip -ne 0 -or
    $NativeFrameDumpDraw -ne 0
)
if ($dumpRequested -and
    ($NativeFrameDumpWidth -eq 0 -or $NativeFrameDumpHeight -eq 0)) {
    $NativeFrameDumpWidth = 3840
    $NativeFrameDumpHeight = 2160
    Write-Host (
        "frame dump size defaulted to " +
        "${NativeFrameDumpWidth}x${NativeFrameDumpHeight}")
}
$env:PS5GPU_NATIVE_FRAME_DUMP_ADDRESS = $NativeFrameDumpAddress
$env:PS5GPU_NATIVE_FRAME_DUMP_WIDTH = [string]$NativeFrameDumpWidth
$env:PS5GPU_NATIVE_FRAME_DUMP_HEIGHT = [string]$NativeFrameDumpHeight
$env:PS5GPU_NATIVE_FRAME_DUMP_STATE = [string]$NativeFrameDumpState
$env:PS5GPU_NATIVE_FRAME_DUMP_FLIP = [string]$NativeFrameDumpFlip
$env:PS5GPU_NATIVE_FRAME_DUMP_DRAW = [string]$NativeFrameDumpDraw
$env:PS5GPU_NATIVE_FRAME_DUMP_BPP = [string]$NativeFrameDumpBpp
$env:PS5GPU_NATIVE_FRAME_DUMP_READ_COPY = if ($NativeFrameDumpReadCopy) {
    "1"
} else {
    ""
}

# Every one of these used to be set by a wrapper script in a scratchpad
# directory, behind this script's back. A wrapper cannot be given a default,
# cannot be validated, and is not read by anyone reviewing what a run
# actually measured - which is how one of them came to launch the title
# without its arguments and another to wipe the shader cache before every
# capture. They are parameters now.
$env:PS5RT_PACKAGE_ARGS = if ($PackageArgs) { "1" } else { "" }
$env:PS5GPU_NATIVE_BUFFER_POOL = if ($BufferPool) { "1" } else { "" }
$env:PS5RT_TRACE_TIME = if ($TraceTime) { "1" } else { "" }
$env:PS5GPU_NATIVE_TRACE_TIME = if ($TraceTime) { "1" } else { "" }
$env:PS5GPU_NATIVE_TRACE_ALL_DRAWS = if ($TraceAllDraws) { "1" } else { "" }
$env:PS5GPU_NATIVE_TRACE_DRAW_INPUTS = if ($TraceDrawInputs) { "1" } else { "" }
$env:PS5GPU_NATIVE_TRACE_SUBMITS = if ($TraceSubmits) { "1" } else { "" }
$env:PS5RT_TRACE_APR = if ($TraceApr) { "1" } else { "" }
$env:PS5RT_TRACE_ASSET_CALLS = if ($TraceAssetCalls) { "1" } else { "" }
$env:PS5RT_TRACE_IO = if ($TraceIo) { "1" } else { "" }
$env:PS5RT_TRACE_ALLOC = if ($TraceAlloc) { "1" } else { "" }
$env:PS5RT_TRACE_HLE_CALLS = if ($TraceHleCalls) { "1" } else { "" }
$env:PS5RT_TRACE_DMA_DATA = if ($TraceDmaData) { "1" } else { "" }
$env:PS5RT_TRACE_DRAW_PACKETS = if ($TraceDrawPackets) { "1" } else { "" }
$env:PS5RT_TRACE_REGISTER_WRITES = if ($TraceRegisterWrites) {
    "1"
} else {
    ""
}
$env:PS5RT_EAGER_COMMIT = if ($EagerCommit) { "1" } else { "" }
$env:PS5RT_AGC_COUNT_IS_DWORDS = if ($AgcCountIsDwords) { "1" } else { "" }
$env:PS5RT_AGC_NO_DEFAULT_STATE = if ($AgcNoDefaultState) { "1" } else { "" }
$env:PS5RT_COMPUTE_SCALAR_BLOCK = if ($ComputeScalarBlock) { "1" } else { "" }
$env:PS5GPU_NATIVE_FORCE_BLEND = if ($ForceBlend) { "1" } else { "" }
$env:PS5GPU_NATIVE_NO_MASK_SKIP = if ($NoMaskSkip) { "1" } else { "" }
$env:PS5GPU_NATIVE_LIVE_BUFFER_REBASE = if ($LiveBufferRebase) {
    "1"
} else {
    ""
}
$env:PS5GPU_NATIVE_FORCE_FRAGMENT_STATES = $ForceFragmentStates
$env:PS5GPU_NATIVE_REAL_ES_BUFFER_MODE = $RealEsBufferMode
$env:PS5GPU_NATIVE_RETIRE_COMPUTE = if ($RetireCompute -ge 0) {
    [string]$RetireCompute
} else {
    ""
}
$env:PS5GPU_NATIVE_WRITE_MASK = if ($WriteMask -ge 0) {
    [string]$WriteMask
} else {
    ""
}
$env:PS5GPU_NATIVE_RETIRE_KEEP = if ($RetireKeep -gt 0) {
    [string]$RetireKeep
} else {
    ""
}
$env:PS5GPU_NATIVE_COMPUTE_STATE_CAP = if ($ComputeStateCap -gt 0) {
    [string]$ComputeStateCap
} else {
    ""
}
$env:PS5GPU_NATIVE_CAPTURE_DRAW_THRESHOLD = if ($CaptureDrawThreshold -gt 0) {
    [string]$CaptureDrawThreshold
} else {
    ""
}
$env:PS5GPU_NATIVE_CAPTURE_FLIPS = if ($CaptureFlips -gt 0) {
    [string]$CaptureFlips
} else {
    ""
}
$env:PS5GPU_NATIVE_CAPTURE_MEMORY_MB = if ($CaptureMemoryMb -gt 0) {
    [string]$CaptureMemoryMb
} else {
    ""
}
$env:SHARPEMU_GPU_WAIT_MODE = if ($ForceGpuWaits) {
    "force"
} else {
    ""
}
$env:SHARPEMU_FORCE_SUBMIT_ORPHAN_PREAMBLES = if ($ForceGpuWaits) {
    "1"
} else {
    "0"
}
$env:PS5RECOMP_HLE_TRACE_CAPACITY = "4096"
$env:PS5RECOMP_HLE_STALL_MS = "$HleStallMs"
$env:PS5RT_TRACE_FS_RECOVERY = if ($TraceFsRecovery) { "1" } else { "0" }
$env:SHARPEMU_VK_PIPELINE_CACHE_PATH = Join-Path `
    $cacheRoot "$packageTag-vulkan-pipeline-cache.bin"
$env:PS5GPU_NATIVE_PIPELINE_CACHE_PATH = Join-Path `
    $cacheRoot "$packageTag-native-vulkan-pipeline-cache.bin"
$env:SHARPEMU_FORCE_SOLID_FRAGMENT = if ($ForceSolidFragment) { "1" } else { "0" }
$env:SHARPEMU_LOG_FIBER = if ($LogFiber) { "1" } else { "0" }
$env:SHARPEMU_TRACE_GUEST_IMAGES = if ($CaptureFrame) { "present" } else { "" }
$env:SHARPEMU_GUEST_IMAGE_DUMP_DIR = if ($CaptureFrame) {
    Join-Path $probeDir "frames"
} else {
    ""
}
$env:SHARPEMU_GPU_CAPTURE_MODE = if ($CaptureFrame) {
    "first-translated-present"
} else {
    ""
}
$env:SHARPEMU_GPU_CAPTURE_DIR = if ($CaptureFrame) {
    Join-Path $probeDir "gpu-captures"
} else {
    ""
}

$runArguments = @(
    "--package", ('"{0}"' -f $Package),
    "--app0", ('"{0}"' -f $App0),
    "--allow-unresolved-imports",
    "--gpu-backend", $GpuBackend
)
$runArguments += "--headless"

Write-Host "[probe] $ProbeSeconds seconds, backend=$GpuBackend"
$probeWindowStyle = if ($Headless) { "Hidden" } else { "Normal" }
# The cleanup at the end of this script kills the instance it started, and
# only if it reaches the end. Interrupt the script - a timeout, a Ctrl-C -
# and the title keeps running with its several gigabytes of commit, and the
# next invocation starts a second one beside it. Two of them racing over the
# same package and the same shader cache measures nothing, and the machine
# has 24GB. Clear the field first.
$leftovers = @(
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ExecutableName)) `
        -ErrorAction SilentlyContinue)
if ($leftovers.Count -gt 0) {
    Write-Host ("stopping {0} leftover instance(s)" -f $leftovers.Count)
    foreach ($stale in $leftovers) {
        Stop-Process -Id $stale.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 700
}

# What the executable needs to start on its own - double-clicked, with no
# arguments. It reads this beside itself: the environment above, where the
# game's files are, and the backend. Traces, dumps and captures are left
# out; they are for runs this script measures, not for playing.
$launchIni = [IO.Path]::ChangeExtension($exePath, ".launch.ini")
$launchLines = @(
    "# Written by astro-cycle.ps1 on $(Get-Date -Format s). The executable",
    "# reads this when started with no arguments.",
    "app0=$App0",
    "gpu_backend=$GpuBackend"
)
# A double-click launch is always the player's configuration, whatever this
# probe ran with: a probe without its window or with the legacy composition
# used to leave the next double-click with a black screen or no window.
$launchLines += "headless=1"
$playerSettings = [ordered]@{
    PS5GPU_NATIVE_WINDOW = "1"
    PS5GPU_NATIVE_REAL_COMPOSITION = "0"
    PS5GPU_NATIVE_LIVE_GRAPHICS = "1"
    PS5RT_NATIVE_GPU_RUNTIME = "1"
    PS5RT_NATIVE_GPU_SHADOW = "1"
    PS5RT_NATIVE_VIDEOOUT = "1"
    PS5RT_NATIVE_SHADER_PRODUCER = "1,graphics"
}
foreach ($variable in (Get-ChildItem Env: | Sort-Object Name)) {
    $name = $variable.Name
    if ($name -notmatch '^(PS5RECOMP_|PS5RT_|PS5GPU_|SHARPEMU_|DOTNET_)' -and
        $name -ne 'LOCALAPPDATA') {
        continue
    }
    if ($name -match '(TRACE|FRAME_DUMP|CAPTURE|_DUMP)' -or
        $playerSettings.Contains($name) -or
        [string]::IsNullOrEmpty($variable.Value)) {
        continue
    }
    $launchLines += "$name=$($variable.Value)"
}
foreach ($setting in $playerSettings.GetEnumerator()) {
    $launchLines += "$($setting.Key)=$($setting.Value)"
}
Set-Content -LiteralPath $launchIni -Value $launchLines -Encoding ascii

$game = Start-Process -FilePath $exePath `
    -ArgumentList $runArguments `
    -WorkingDirectory $Package `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -WindowStyle $probeWindowStyle `
    -PassThru

$stopReason = "timeout"
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$stderrOffset = [int64]0
$stderrRemainder = ""
$firstNativeFlipSeconds = -1.0
$lastProgressSeconds = 0.0
$lastProgressKind = "process_start"
while ($stopwatch.Elapsed.TotalSeconds -lt $ProbeSeconds) {
    Start-Sleep -Milliseconds 250
    $game.Refresh()
    if ($game.HasExited) {
        $stopReason = "process_exit"
        break
    }
    if (Test-Path -LiteralPath $stderrPath) {
        $newLogText = ""
        $stream = [IO.File]::Open(
            $stderrPath,
            [IO.FileMode]::Open,
            [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite)
        try {
            if ($stream.Length -lt $stderrOffset) {
                $stderrOffset = 0
                $stderrRemainder = ""
            }
            if ($stream.Length -gt $stderrOffset) {
                [void]$stream.Seek(
                    $stderrOffset,
                    [IO.SeekOrigin]::Begin)
                $remaining = [int64]($stream.Length - $stderrOffset)
                $buffer = [byte[]]::new([int]$remaining)
                $read = 0
                while ($read -lt $buffer.Length) {
                    $count = $stream.Read(
                        $buffer,
                        $read,
                        $buffer.Length - $read)
                    if ($count -le 0) {
                        break
                    }
                    $read += $count
                }
                $stderrOffset += $read
                if ($read -gt 0) {
                    $chunk = [Text.Encoding]::UTF8.GetString(
                        $buffer,
                        0,
                        $read)
                    $combined = $stderrRemainder + $chunk
                    $lastNewline = $combined.LastIndexOf("`n")
                    if ($lastNewline -ge 0) {
                        $newLogText = $combined.Substring(
                            0,
                            $lastNewline + 1)
                        $stderrRemainder = $combined.Substring(
                            $lastNewline + 1)
                    } else {
                        $stderrRemainder = $combined
                    }
                }
            }
        } finally {
            $stream.Dispose()
        }

        if ($newLogText -ne "") {
            if ($newLogText.Contains("fatal_exception=")) {
                $stopReason = "fatal"
                Start-Sleep -Milliseconds 500
                break
            }

            $progressMatches = [regex]::Matches(
                $newLogText,
                "native_shadow\.agc\.submit_(?:dcb|ccb)|" +
                "native_gpu\.(?:draw_processed|compute_processed|" +
                "flip_processed|vulkan_frame)|" +
                "Vulkan VideoOut presented|guest_puts=|thread_start")
            if ($progressMatches.Count -gt 0) {
                $lastProgressSeconds =
                    $stopwatch.Elapsed.TotalSeconds
                $lastProgressKind =
                    $progressMatches[
                        $progressMatches.Count - 1].Value
            }

            $flipMatches = [regex]::Matches(
                $newLogText,
                "native_gpu\.(?:vulkan_frame flip|" +
                "flip_processed count)=(\d+)")
            foreach ($flipMatch in $flipMatches) {
                if ([int]$flipMatch.Groups[1].Value -ge 1 -and
                    $firstNativeFlipSeconds -lt 0) {
                    $firstNativeFlipSeconds =
                        $stopwatch.Elapsed.TotalSeconds
                }
            }

            if ($StopOnFirstFrame -and
                ($newLogText.Contains(
                    "Hosted first frame presented") -or
                 $newLogText.Contains(
                    "Vulkan VideoOut presented first frame") -or
                 $newLogText.Contains(
                    "native_gpu.vulkan_frame ") -or
                 $newLogText.Contains(
                    "native_gpu.flip_processed "))) {
                $stopReason = "first_frame"
                Start-Sleep -Milliseconds 500
                break
            }

            # Stop on flip_processed, not vulkan_frame. vulkan_frame is traced
            # when the frame is submitted, which is before write_gpu_capture
            # runs; killing there truncated every capture once they grew past
            # a couple of hundred megabytes. flip_processed comes after the
            # file is closed and renamed.
            if ($StopAfterNativeFlip -gt 0 -and
                $newLogText.Contains(
                    "native_gpu.flip_processed count=$StopAfterNativeFlip ")) {
                $stopReason =
                    "native_flip_$StopAfterNativeFlip"
                Start-Sleep -Milliseconds 500
                break
            }

            if ($StopOnComputeReadback -and
                $newLogText.Contains(
                    "native_gpu.compute_readback state=10 ")) {
                $stopReason = "compute_readback"
                Start-Sleep -Milliseconds 500
                break
            }
        }

        if ($StopAfterFlipStallSeconds -gt 0 -and
            $firstNativeFlipSeconds -ge 0 -and
            $stopwatch.Elapsed.TotalSeconds -
                $lastProgressSeconds -ge
                $StopAfterFlipStallSeconds) {
            $stopReason = "post_flip_stall"
            break
        }
    }
}

$game.Refresh()
if (-not $game.HasExited) {
    Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
    $game.WaitForExit(5000) | Out-Null
}
$game.Refresh()
$exitCode = if ($game.HasExited) { $game.ExitCode } else { $null }

$stdout = if (Test-Path -LiteralPath $stdoutPath) {
    @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
} else {
    @()
}
$stderr = if (Test-Path -LiteralPath $stderrPath) {
    @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
} else {
    @()
}
$allLines = @($stdout) + @($stderr)

function Select-LogLines {
    param(
        [string[]]$Lines,
        [string]$Pattern,
        [int]$Last = 20
    )
    return @($Lines | Where-Object { $_ -match $Pattern } | Select-Object -Last $Last)
}

$fatalLines = Select-LogLines $allLines "fatal_exception="
$recoveredLines = Select-LogLines `
    $allLines `
    "emulated_guest_fs_load|sse4a_instruction_recovered" `
    30
$unresolvedLines = Select-LogLines $allLines "unresolved_import|auto_stub" 30
$videoLines = Select-LogLines $allLines "videoout|video_out|flip|vblank|present" 30
$gpuLines = Select-LogLines `
    $allLines `
    "native_shadow|agc|gpu|shader|spirv|d3d12|vulkan" `
    60
$nativeDrawMatches = [regex]::Matches(
    ($allLines -join "`n"),
    "native_gpu\.draw_processed count=(\d+)")
$nativeFlipMatches = [regex]::Matches(
    ($allLines -join "`n"),
    "native_gpu\.flip_processed count=(\d+)")
$nativeDrawProcessed = if ($nativeDrawMatches.Count -eq 0) {
    0
} else {
    [int64]($nativeDrawMatches |
        ForEach-Object { $_.Groups[1].Value } |
        Measure-Object -Maximum).Maximum
}
$nativeFlipProcessed = if ($nativeFlipMatches.Count -eq 0) {
    0
} else {
    [int64]($nativeFlipMatches |
        ForEach-Object { $_.Groups[1].Value } |
        Measure-Object -Maximum).Maximum
}
$nativeSubmitFailures = @(
    $allLines | Where-Object {
        $_ -match "native_gpu\.submit_(draw|flip)_failed"
    }
).Count
$nidMatches = [regex]::Matches(
    ($allLines -join "`n"),
    "nid=([A-Za-z0-9+/_-]+)")
$uniqueNids = @(
    $nidMatches |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
)

$summary = [Collections.Generic.List[string]]::new()
$summary.Add("probe=$probeName")
$summary.Add("executable=$exePath")
$summary.Add("package=$Package")
$summary.Add("app0=$App0")
$summary.Add("backend=$GpuBackend")
$summary.Add("gpu_trace=$([bool]$TraceGpu)")
$summary.Add("trace_draws=$([bool]$TraceDraws)")
$summary.Add("trace_frame_packets=$([bool]$TraceFramePackets)")
$summary.Add("trace_hle=$([bool]$TraceHle)")
$summary.Add("log_fiber=$([bool]$LogFiber)")
$summary.Add("trace_fs_recovery=$([bool]$TraceFsRecovery)")
$summary.Add("hle_stall_ms=$HleStallMs")
$summary.Add("native_gpu_shadow=$([bool]$NativeGpuShadow)")
$summary.Add("native_videoout=$([bool]$NativeVideoOut)")
$summary.Add("native_gpu_runtime=$([bool]$NativeGpuRuntime)")
$summary.Add("native_gpu_shader_dir=$env:PS5GPU_NATIVE_SHADER_DIR")
$summary.Add("native_gpu_capture=$([bool]$NativeGpuCapture)")
$summary.Add("native_gpu_capture_path=$env:PS5GPU_NATIVE_CAPTURE_PATH")
$summary.Add("native_gpu_capture_flip=$NativeGpuCaptureFlip")
$summary.Add("native_frame_dump_path=$env:PS5GPU_NATIVE_FRAME_DUMP_PATH")
$summary.Add("native_frame_dump_address=$NativeFrameDumpAddress")
$summary.Add("native_frame_dump_size=$NativeFrameDumpWidth`x$NativeFrameDumpHeight")
$summary.Add("native_frame_dump_state=$NativeFrameDumpState")
$summary.Add("native_frame_dump_flip=$NativeFrameDumpFlip")
$summary.Add("native_videoout_nids=$NativeVideoOutNids")
$summary.Add("native_real_composition=$([bool]$NativeRealComposition)")
$summary.Add("native_real_composition_mode=$NativeRealCompositionMode")
$summary.Add("native_state29_mode=$NativeState29Mode")
$summary.Add("native_state28_copy_image=$NativeState28CopyImage")
$summary.Add("native_post_copy_state=$NativePostCopyState")
$summary.Add("native_sample_copy_state=$NativeSampleCopyState")
$summary.Add("native_live_graphics_min_state=$NativeLiveGraphicsMinState")
$summary.Add("native_compile_graphics=$([bool]$NativeCompileGraphics)")
$summary.Add("native_live_shader_cache_dir=$liveShaderCacheDir")
$summary.Add("package_args=$(if ($PackageArgs) { 1 } else { 0 })")
$summary.Add("buffer_pool=$(if ($BufferPool) { 1 } else { 0 })")
$summary.Add("cleared_shader_cache=$(if ($ClearShaderCache) { 1 } else { 0 })")
$summary.Add("force_solid_fragment=$([bool]$ForceSolidFragment)")
$summary.Add("capture_frame=$([bool]$CaptureFrame)")
$summary.Add("force_gpu_waits=$([bool]$ForceGpuWaits)")
$summary.Add("stop_after_native_flip=$StopAfterNativeFlip")
$summary.Add("stop_after_flip_stall_seconds=$StopAfterFlipStallSeconds")
$summary.Add("first_native_flip_seconds=$(
    if ($firstNativeFlipSeconds -lt 0) {
        "-1"
    } else {
        [Math]::Round($firstNativeFlipSeconds, 2)
    })")
$summary.Add("last_progress_seconds=$(
    [Math]::Round($lastProgressSeconds, 2))")
$summary.Add("last_progress_kind=$lastProgressKind")
$summary.Add("stop_on_compute_readback=$([bool]$StopOnComputeReadback)")
$summary.Add("gpu_capture_dir=$env:SHARPEMU_GPU_CAPTURE_DIR")
$summary.Add("elapsed_seconds=$([Math]::Round($stopwatch.Elapsed.TotalSeconds, 2))")
$summary.Add("stop_reason=$stopReason")
$summary.Add("exit_code=$exitCode")
$summary.Add("fatal_lines=$($fatalLines.Count)")
$summary.Add("unresolved_lines=$($unresolvedLines.Count)")
$summary.Add("unique_nids=$($uniqueNids.Count)")
$summary.Add("video_lines=$($videoLines.Count)")
$summary.Add("gpu_shader_lines=$($gpuLines.Count)")
$summary.Add("native_gpu_draws_processed=$nativeDrawProcessed")
$summary.Add("native_gpu_flips_processed=$nativeFlipProcessed")
$summary.Add("native_gpu_submit_failures=$nativeSubmitFailures")

foreach ($section in @(
        @("FATAL", $fatalLines),
        @("RECOVERED_EXCEPTIONS", $recoveredLines),
        @("UNRESOLVED", $unresolvedLines),
        @("NIDS", $uniqueNids),
        @("VIDEOOUT", $videoLines),
        @("GPU_SHADER", $gpuLines)
    )) {
    $summary.Add("")
    $summary.Add("[$($section[0])]")
    if ($section[1].Count -eq 0) {
        $summary.Add("(none)")
    } else {
        foreach ($line in $section[1]) {
            $summary.Add([string]$line)
        }
    }
}

[IO.File]::WriteAllLines(
    $summaryPath,
    $summary,
    [Text.UTF8Encoding]::new($false))

Write-Host "[done] reason=$stopReason exit=$exitCode elapsed=$([Math]::Round($stopwatch.Elapsed.TotalSeconds, 2))s"
Write-Host "[done] fatal=$($fatalLines.Count) unresolved=$($unresolvedLines.Count) nids=$($uniqueNids.Count) gpu=$($gpuLines.Count)"
Write-Host "[done] $summaryPath"

[pscustomobject]@{
    ProbeDirectory = $probeDir
    Executable = $exePath
    StopReason = $stopReason
    ExitCode = $exitCode
    ElapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 2)
    FatalLines = $fatalLines.Count
    UnresolvedLines = $unresolvedLines.Count
    UniqueNids = $uniqueNids.Count
    VideoLines = $videoLines.Count
    GpuShaderLines = $gpuLines.Count
}
