[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$ArchivePath,

    [string]$DestinationDir = "",

    [switch]$InspectOnly,

    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ToolsDir = $PSScriptRoot
$Root = Split-Path -Parent $ToolsDir
$DepsDir = Join-Path $Root "_deps"
$Verifier = Join-Path $ToolsDir "VerifyFfmpegRoot.ps1"

if ([string]::IsNullOrWhiteSpace($DestinationDir)) {
    $DestinationDir = Join-Path $Root "_deps\ffmpeg"
}

function Stop-Preparation {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Error $Message
    exit 1
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FullPath -Path $Path).TrimEnd([char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar))
}

function Assert-PathInsideRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $rootPath = Get-NormalizedPath -Path $Root
    $targetPath = Get-NormalizedPath -Path $Path
    $rootPrefix = $rootPath + [System.IO.Path]::DirectorySeparatorChar

    if (-not $targetPath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Stop-Preparation "$Description must be inside the repository root: $targetPath"
    }

    return $targetPath
}

function Assert-ParentDirectoryPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $parentPath = Split-Path -Parent $Path
    while (-not [string]::IsNullOrWhiteSpace($parentPath)) {
        if (Test-Path -LiteralPath $parentPath -PathType Leaf) {
            Stop-Preparation "$Name parent must be a directory, not an existing file: $parentPath"
        }

        if (Test-Path -LiteralPath $parentPath -PathType Container) {
            return
        }

        $nextParentPath = Split-Path -Parent $parentPath
        if ([System.String]::Equals($nextParentPath, $parentPath, [System.StringComparison]::OrdinalIgnoreCase)) {
            return
        }

        $parentPath = $nextParentPath
    }
}

function Test-FfmpegRootCandidate {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Test-FfmpegIncludeRoot -Path $Path) -and
        (Test-FfmpegLibraryRoot -Path $Path) -and
        (Test-FfmpegBinaryRoot -Path $Path)
}

function Test-FfmpegIncludeRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    $requiredHeaders = @(
        "include\libavformat\avformat.h",
        "include\libavcodec\avcodec.h",
        "include\libavutil\avutil.h",
        "include\libswresample\swresample.h"
    )

    foreach ($relativePath in $requiredHeaders) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relativePath) -PathType Leaf)) {
            return $false
        }
    }

    return $true
}

function Test-FfmpegLibraryRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    $requiredLibraries = @(
        "lib\avformat.lib",
        "lib\avcodec.lib",
        "lib\avutil.lib",
        "lib\swresample.lib"
    )

    foreach ($relativePath in $requiredLibraries) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relativePath) -PathType Leaf)) {
            return $false
        }
    }

    return $true
}

function Test-FfmpegBinaryRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    $binaryDir = Join-Path $Path "bin"
    if (-not (Test-Path -LiteralPath $binaryDir -PathType Container)) {
        return $false
    }

    foreach ($dllName in @("avformat", "avcodec", "avutil", "swresample")) {
        $matches = Get-ChildItem -LiteralPath $binaryDir -Filter "$dllName*.dll" -File -ErrorAction SilentlyContinue
        if ($matches.Count -eq 0) {
            return $false
        }
    }

    return $true
}

function Find-FfmpegRootByTest {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDir,
        [Parameter(Mandatory = $true)][scriptblock]$Test
    )

    if (& $Test $BaseDir) {
        return $BaseDir
    }

    $directories = Get-ChildItem -LiteralPath $BaseDir -Directory -Recurse -ErrorAction SilentlyContinue |
        Sort-Object -Property FullName
    foreach ($directory in $directories) {
        if (& $Test $directory.FullName) {
            return $directory.FullName
        }
    }

    return $null
}

function Find-FfmpegRootCandidate {
    param([Parameter(Mandatory = $true)][string]$BaseDir)

    return Find-FfmpegRootByTest -BaseDir $BaseDir -Test {
        param([string]$Path)
        Test-FfmpegRootCandidate -Path $Path
    }
}

function Find-FfmpegIncludeRoot {
    param([Parameter(Mandatory = $true)][string]$BaseDir)

    return Find-FfmpegRootByTest -BaseDir $BaseDir -Test {
        param([string]$Path)
        Test-FfmpegIncludeRoot -Path $Path
    }
}

function Find-FfmpegLibraryRoot {
    param([Parameter(Mandatory = $true)][string]$BaseDir)

    return Find-FfmpegRootByTest -BaseDir $BaseDir -Test {
        param([string]$Path)
        Test-FfmpegLibraryRoot -Path $Path
    }
}

function Find-FfmpegBinaryRoot {
    param([Parameter(Mandatory = $true)][string]$BaseDir)

    return Find-FfmpegRootByTest -BaseDir $BaseDir -Test {
        param([string]$Path)
        Test-FfmpegBinaryRoot -Path $Path
    }
}

function Get-FfmpegArchiveLayout {
    param([Parameter(Mandatory = $true)][string]$BaseDir)

    $candidate = Find-FfmpegRootCandidate -BaseDir $BaseDir
    if (-not [string]::IsNullOrWhiteSpace($candidate)) {
        return [pscustomobject]@{
            CompleteRoot = $candidate
            IncludeRoot = $candidate
            LibraryRoot = $candidate
            BinaryRoot = $candidate
            IsComplete = $true
        }
    }

    $includeRoot = Find-FfmpegIncludeRoot -BaseDir $BaseDir
    $libraryRoot = Find-FfmpegLibraryRoot -BaseDir $BaseDir
    $binaryRoot = Find-FfmpegBinaryRoot -BaseDir $BaseDir

    return [pscustomobject]@{
        CompleteRoot = $null
        IncludeRoot = $includeRoot
        LibraryRoot = $libraryRoot
        BinaryRoot = $binaryRoot
        IsComplete = (-not [string]::IsNullOrWhiteSpace($includeRoot)) -and
            (-not [string]::IsNullOrWhiteSpace($libraryRoot)) -and
            (-not [string]::IsNullOrWhiteSpace($binaryRoot))
    }
}

function Write-LayoutComponent {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        Write-Host "  $Name`: missing"
    } else {
        Write-Host "  $Name`: $Path"
    }
}

function Write-FfmpegArchiveLayout {
    param([Parameter(Mandatory = $true)]$Layout)

    if (-not [string]::IsNullOrWhiteSpace($Layout.CompleteRoot)) {
        Write-Host "Archive set contains a complete FFmpeg include/lib/bin layout:"
        Write-Host "  Root: $($Layout.CompleteRoot)"
        return
    }

    Write-Host "Archive set component scan:"
    Write-LayoutComponent -Name "Include root" -Path $Layout.IncludeRoot
    Write-LayoutComponent -Name "Library root" -Path $Layout.LibraryRoot
    Write-LayoutComponent -Name "Binary root" -Path $Layout.BinaryRoot
}

function Copy-FfmpegComponent {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$ComponentName,
        [Parameter(Mandatory = $true)][string]$DestinationRoot
    )

    $componentPath = Join-Path $SourceRoot $ComponentName
    if (-not (Test-Path -LiteralPath $componentPath -PathType Container)) {
        Stop-Preparation "Missing $ComponentName component at $componentPath"
    }

    Copy-Item -LiteralPath $componentPath -Destination $DestinationRoot -Recurse
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

$archiveInputs = @()
foreach ($archiveGroup in $ArchivePath) {
    foreach ($archive in ($archiveGroup -split ",")) {
        $trimmedArchive = $archive.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmedArchive)) {
            $archiveInputs += $trimmedArchive
        }
    }
}

$resolvedArchives = @()
foreach ($archive in $archiveInputs) {
    try {
        $resolvedArchive = (Resolve-Path -LiteralPath $archive -ErrorAction Stop).Path
    } catch {
        Stop-Preparation "ArchivePath does not exist: $archive"
    }

    if (-not (Test-Path -LiteralPath $resolvedArchive -PathType Leaf)) {
        Stop-Preparation "ArchivePath must be a file: $resolvedArchive"
    }

    if ([System.IO.Path]::GetExtension($resolvedArchive) -ne ".zip") {
        Stop-Preparation "Only .zip archives are supported. Extract other archive formats manually, then run tools\VerifyFfmpegRoot.ps1."
    }

    $resolvedArchives += $resolvedArchive
}

if ($resolvedArchives.Count -eq 0) {
    Stop-Preparation "At least one ArchivePath value is required."
}

$destinationPath = $null
if (-not $InspectOnly) {
    $destinationPath = Assert-PathInsideRoot -Path $DestinationDir -Description "DestinationDir"
    $destinationParent = Split-Path -Parent $destinationPath
    if ([string]::IsNullOrWhiteSpace($destinationParent)) {
        Stop-Preparation "DestinationDir is invalid: $DestinationDir"
    }

    if (Test-Path -LiteralPath $destinationPath -PathType Leaf) {
        Stop-Preparation "DestinationDir must name a directory, not an existing file: $destinationPath"
    }

    Assert-ParentDirectoryPath -Path $destinationPath -Name "DestinationDir"
    [void][System.IO.Directory]::CreateDirectory($destinationParent)

    if (Test-Path -LiteralPath $destinationPath -PathType Container) {
        if (-not $Force) {
            Stop-Preparation "Destination already exists: $destinationPath. Re-run with -Force to replace it."
        }

        [void](Assert-PathInsideRoot -Path $destinationPath -Description "DestinationDir")
        Remove-Item -LiteralPath $destinationPath -Recurse -Force
    }
}

$extractDir = Join-Path $DepsDir (".ffmpeg_extract_" + [System.Guid]::NewGuid().ToString("N"))
$extractPath = Assert-PathInsideRoot -Path $extractDir -Description "Temporary extraction directory"

try {
    [void][System.IO.Directory]::CreateDirectory($DepsDir)
    [void][System.IO.Directory]::CreateDirectory($extractPath)

    Write-Host "Extracting user-supplied FFmpeg archive(s)..."
    foreach ($resolvedArchive in $resolvedArchives) {
        Write-Host "  $resolvedArchive"
        Expand-Archive -LiteralPath $resolvedArchive -DestinationPath $extractPath -Force
    }

    $layout = Get-FfmpegArchiveLayout -BaseDir $extractPath
    if ($InspectOnly) {
        Write-FfmpegArchiveLayout -Layout $layout
        if (-not $layout.IsComplete) {
            Stop-Preparation "Archive set does not contain the expected FFmpeg include/lib/bin layout with MSVC import libraries."
        }

        Write-Host "FFmpeg archive inspection succeeded. No FFmpeg root was prepared."
        exit 0
    }

    if (-not [string]::IsNullOrWhiteSpace($layout.CompleteRoot)) {
        $candidatePath = Assert-PathInsideRoot -Path $layout.CompleteRoot -Description "FFmpeg root candidate"
        Write-Host "Preparing FFmpeg root at $destinationPath"
        Move-Item -LiteralPath $candidatePath -Destination $destinationPath
    } else {
        if (-not $layout.IsComplete) {
            Write-FfmpegArchiveLayout -Layout $layout
            Stop-Preparation "Archive set does not contain the expected FFmpeg include/lib/bin layout with MSVC import libraries."
        }

        [void](Assert-PathInsideRoot -Path $layout.IncludeRoot -Description "FFmpeg include root")
        [void](Assert-PathInsideRoot -Path $layout.LibraryRoot -Description "FFmpeg library root")
        [void](Assert-PathInsideRoot -Path $layout.BinaryRoot -Description "FFmpeg binary root")

        Write-Host "Preparing merged FFmpeg root at $destinationPath"
        [void][System.IO.Directory]::CreateDirectory($destinationPath)
        Copy-FfmpegComponent -SourceRoot $layout.IncludeRoot -ComponentName "include" -DestinationRoot $destinationPath
        Copy-FfmpegComponent -SourceRoot $layout.LibraryRoot -ComponentName "lib" -DestinationRoot $destinationPath
        Copy-FfmpegComponent -SourceRoot $layout.BinaryRoot -ComponentName "bin" -DestinationRoot $destinationPath
    }

    Write-Host "Verifying prepared FFmpeg root..."
    Invoke-Native -FilePath "powershell.exe" -Arguments @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $Verifier,
        "-FfmpegRoot",
        $destinationPath)

    Write-Host "FFmpeg root prepared: $destinationPath"
    Write-Host "Next verification command:"
    Write-Host "  .\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot `"$destinationPath`""
} finally {
    if (Test-Path -LiteralPath $extractPath) {
        [void](Assert-PathInsideRoot -Path $extractPath -Description "Temporary extraction directory")
        Remove-Item -LiteralPath $extractPath -Recurse -Force
    }

    if (Test-Path -LiteralPath $DepsDir) {
        $remaining = Get-ChildItem -LiteralPath $DepsDir -Force -ErrorAction SilentlyContinue
        if ($remaining.Count -eq 0) {
            [void](Assert-PathInsideRoot -Path $DepsDir -Description "Dependency directory")
            Remove-Item -LiteralPath $DepsDir -Force
        }
    }
}
