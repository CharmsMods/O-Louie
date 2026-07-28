[CmdletBinding()]
param(
    [string]$FfmpegRoot = $env:OLOUIE_FFMPEG_ROOT,

    [string]$FfmpegIncludeDir = $env:OLOUIE_FFMPEG_INCLUDE_DIR,

    [string]$FfmpegLibraryDir = $env:OLOUIE_FFMPEG_LIBRARY_DIR,

    [string]$FfmpegBinaryDir = $env:OLOUIE_FFMPEG_BINARY_DIR
)

$ErrorActionPreference = "Stop"

function Stop-Verification {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Error $Message
    exit 1
}

function Resolve-RequiredDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        Stop-Verification "$Name is required."
    }

    try {
        return (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    } catch {
        Stop-Verification "$Name does not exist: $Path"
    }
}

function Require-File {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDir,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $path = Join-Path $BaseDir $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Stop-Verification "Missing $Description`: $path"
    }
    return $path
}

function Require-Dll {
    param(
        [Parameter(Mandatory = $true)][string]$BinaryDir,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $matches = Get-ChildItem -LiteralPath $BinaryDir -Filter "$Name*.dll" -File -ErrorAction SilentlyContinue |
        Sort-Object -Property Name
    if ($matches.Count -eq 0) {
        Stop-Verification "Missing FFmpeg runtime DLL matching $Name*.dll under $BinaryDir"
    }
    return $matches[0].FullName
}

if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot)) {
    $resolvedRoot = Resolve-RequiredDirectory -Path $FfmpegRoot -Name "FfmpegRoot"
    if ([string]::IsNullOrWhiteSpace($FfmpegIncludeDir)) {
        $FfmpegIncludeDir = Join-Path $resolvedRoot "include"
    }
    if ([string]::IsNullOrWhiteSpace($FfmpegLibraryDir)) {
        $FfmpegLibraryDir = Join-Path $resolvedRoot "lib"
    }
    if ([string]::IsNullOrWhiteSpace($FfmpegBinaryDir)) {
        $FfmpegBinaryDir = Join-Path $resolvedRoot "bin"
    }
} elseif ([string]::IsNullOrWhiteSpace($FfmpegIncludeDir) -or
          [string]::IsNullOrWhiteSpace($FfmpegLibraryDir) -or
          [string]::IsNullOrWhiteSpace($FfmpegBinaryDir)) {
    Stop-Verification "Provide -FfmpegRoot or explicit -FfmpegIncludeDir, -FfmpegLibraryDir, and -FfmpegBinaryDir values."
}

$includeDir = Resolve-RequiredDirectory -Path $FfmpegIncludeDir -Name "FfmpegIncludeDir"
$libraryDir = Resolve-RequiredDirectory -Path $FfmpegLibraryDir -Name "FfmpegLibraryDir"
$binaryDir = Resolve-RequiredDirectory -Path $FfmpegBinaryDir -Name "FfmpegBinaryDir"

$headers = @(
    Require-File -BaseDir $includeDir -RelativePath "libavformat\avformat.h" -Description "libavformat header"
    Require-File -BaseDir $includeDir -RelativePath "libavcodec\avcodec.h" -Description "libavcodec header"
    Require-File -BaseDir $includeDir -RelativePath "libavutil\avutil.h" -Description "libavutil header"
    Require-File -BaseDir $includeDir -RelativePath "libswresample\swresample.h" -Description "libswresample header"
)

$libraries = @(
    Require-File -BaseDir $libraryDir -RelativePath "avformat.lib" -Description "MSVC avformat import library"
    Require-File -BaseDir $libraryDir -RelativePath "avcodec.lib" -Description "MSVC avcodec import library"
    Require-File -BaseDir $libraryDir -RelativePath "avutil.lib" -Description "MSVC avutil import library"
    Require-File -BaseDir $libraryDir -RelativePath "swresample.lib" -Description "MSVC swresample import library"
)

$runtimeDlls = @(
    Require-Dll -BinaryDir $binaryDir -Name "avformat"
    Require-Dll -BinaryDir $binaryDir -Name "avcodec"
    Require-Dll -BinaryDir $binaryDir -Name "avutil"
    Require-Dll -BinaryDir $binaryDir -Name "swresample"
)

Write-Host "FFmpeg root verification succeeded."
Write-Host "  Include: $includeDir"
Write-Host "  Library: $libraryDir"
Write-Host "  Binary:  $binaryDir"
Write-Host "  Headers:"
$headers | ForEach-Object { Write-Host "    $_" }
Write-Host "  Import libraries:"
$libraries | ForEach-Object { Write-Host "    $_" }
Write-Host "  Runtime DLLs:"
$runtimeDlls | ForEach-Object { Write-Host "    $_" }

if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot)) {
    Write-Host "  Build command:"
    Write-Host "    .\build.ps1 -Configuration Debug -EnableFfmpeg -FfmpegRoot `"$resolvedRoot`""
} else {
    Write-Host "  Build command:"
    Write-Host "    .\build.ps1 -Configuration Debug -EnableFfmpeg -FfmpegIncludeDir `"$includeDir`" -FfmpegLibraryDir `"$libraryDir`" -FfmpegBinaryDir `"$binaryDir`""
}
