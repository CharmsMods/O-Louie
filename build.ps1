[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",

    [switch]$NoTests,

    [switch]$EnableFfmpeg,

    [string]$FfmpegRoot = $env:OLOUIE_FFMPEG_ROOT,

    [string]$FfmpegIncludeDir = $env:OLOUIE_FFMPEG_INCLUDE_DIR,

    [string]$FfmpegLibraryDir = $env:OLOUIE_FFMPEG_LIBRARY_DIR,

    [string]$FfmpegBinaryDir = $env:OLOUIE_FFMPEG_BINARY_DIR,

    [string]$ImGuiRoot = $env:OLOUIE_IMGUI_ROOT
)

$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$BuildDirName = "build"
if ($EnableFfmpeg) {
    $BuildDirName = "build-ffmpeg"
}
$BuildDir = Join-Path $Root $BuildDirName

if ([string]::IsNullOrWhiteSpace($ImGuiRoot)) {
    $ImGuiRoot = Join-Path $Root "_deps\imgui"
}

$ImGuiPrepareParameters = @{
    DestinationDir = $ImGuiRoot
}
if (Test-Path -LiteralPath (Join-Path $ImGuiRoot "imgui.h") -PathType Leaf) {
    $ImGuiPrepareParameters.CheckExisting = $true
}
& (Join-Path $Root "tools\PrepareDearImGui.ps1") @ImGuiPrepareParameters
if (-not $?) {
    throw "Dear ImGui preparation failed."
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

$Generator = $env:CMAKE_GENERATOR
if ([string]::IsNullOrWhiteSpace($Generator)) {
    $Generator = "Visual Studio 17 2022"
}

$BuildTests = "ON"
if ($NoTests) {
    $BuildTests = "OFF"
}

$ConfigureArgs = @("-S", $Root, "-B", $BuildDir, "-G", $Generator, "-DOLOUIE_BUILD_TESTS=$BuildTests", "-DOLOUIE_IMGUI_ROOT=$ImGuiRoot")

if ($EnableFfmpeg) {
    if ([string]::IsNullOrWhiteSpace($FfmpegRoot) -and
        ([string]::IsNullOrWhiteSpace($FfmpegIncludeDir) -or
         [string]::IsNullOrWhiteSpace($FfmpegLibraryDir) -or
         [string]::IsNullOrWhiteSpace($FfmpegBinaryDir))) {
        throw "-EnableFfmpeg requires -FfmpegRoot or explicit -FfmpegIncludeDir/-FfmpegLibraryDir/-FfmpegBinaryDir values."
    }

    $ConfigureArgs += "-DOLOUIE_ENABLE_FFMPEG=ON"
    if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot)) {
        $ConfigureArgs += "-DOLOUIE_FFMPEG_ROOT=$FfmpegRoot"
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegIncludeDir)) {
        $ConfigureArgs += "-DOLOUIE_FFMPEG_INCLUDE_DIR=$FfmpegIncludeDir"
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegLibraryDir)) {
        $ConfigureArgs += "-DOLOUIE_FFMPEG_LIBRARY_DIR=$FfmpegLibraryDir"
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegBinaryDir)) {
        $ConfigureArgs += "-DOLOUIE_FFMPEG_BINARY_DIR=$FfmpegBinaryDir"
    }
} else {
    $ConfigureArgs += "-DOLOUIE_ENABLE_FFMPEG=OFF"
}

if ($Generator -like "Visual Studio*") {
    $ConfigureArgs += @("-A", "x64")
}

Invoke-Native cmake @ConfigureArgs
Invoke-Native cmake --build $BuildDir --config $Configuration

if (-not $NoTests) {
    Invoke-Native ctest --test-dir $BuildDir -C $Configuration --output-on-failure
}
