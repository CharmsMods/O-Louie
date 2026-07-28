[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",

    [string]$FfmpegRoot = $env:OLOUIE_FFMPEG_ROOT,

    [string]$FfmpegIncludeDir = $env:OLOUIE_FFMPEG_INCLUDE_DIR,

    [string]$FfmpegLibraryDir = $env:OLOUIE_FFMPEG_LIBRARY_DIR,

    [string]$FfmpegBinaryDir = $env:OLOUIE_FFMPEG_BINARY_DIR,

    [string[]]$FfmpegArchivePath = @(),

    [string]$FfmpegArchiveDestinationDir = "",

    [switch]$ForceFfmpegArchivePreparation,

    [string]$RecordTestOutputDir = "",

    [string]$VerificationReportPath = "",

    [switch]$RunWgcSmoke,

    [int]$WgcDurationMs = 3000,

    [uint32]$Width = 1920,

    [uint32]$Height = 1080,

    [uint32]$Fps = 60,

    [uint32]$BitrateMbps = 20
)

$ErrorActionPreference = "Stop"

$ToolsDir = $PSScriptRoot
$Root = Split-Path -Parent $ToolsDir
$BuildDir = Join-Path $Root "build-ffmpeg"
$Verifier = Join-Path $ToolsDir "VerifyFfmpegRoot.ps1"
$ArchivePreparer = Join-Path $ToolsDir "PrepareFfmpegRootFromArchive.ps1"
$Mp4Inspector = Join-Path $ToolsDir "InspectMp4Artifact.ps1"
$BuildScript = Join-Path $Root "build.ps1"

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$Arguments = @()
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

function Add-OptionalArg {
    param(
        [System.Collections.Generic.List[string]]$Args,
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$Value
    )

    if (-not [string]::IsNullOrWhiteSpace($Value)) {
        $Args.Add($Name)
        $Args.Add($Value)
    }
}

function Test-ArchivePathsSupplied {
    foreach ($archivePath in $FfmpegArchivePath) {
        if (-not [string]::IsNullOrWhiteSpace($archivePath)) {
            return $true
        }
    }

    return $false
}

function Get-RepoRelativeFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function ConvertTo-ReportValue {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return "<not supplied>"
    }

    return $Value
}

function Assert-PositiveIntegerOption {
    param(
        [Parameter(Mandatory = $true)][int64]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($Value -le 0) {
        throw "$Name must be a positive integer."
    }
}

function Assert-ParentDirectoryPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $parentPath = Split-Path -Parent $Path
    while (-not [string]::IsNullOrWhiteSpace($parentPath)) {
        if (Test-Path -LiteralPath $parentPath -PathType Leaf) {
            throw "$Name parent must be a directory, not an existing file: $parentPath"
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

function Assert-VerificationReportPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace([System.IO.Path]::GetFileName($Path))) {
        throw "VerificationReportPath must name a report file, not a directory: $Path"
    }

    if (Test-Path -LiteralPath $Path -PathType Container) {
        throw "VerificationReportPath must name a report file, not a directory: $Path"
    }

    Assert-ParentDirectoryPath -Path $Path -Name "VerificationReportPath"
}

function Assert-RecordTestOutputPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        throw "RecordTestOutputDir must name an output directory, not an existing file: $Path"
    }

    Assert-ParentDirectoryPath -Path $Path -Name "RecordTestOutputDir"
}

function Get-ComparablePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar))
}

function Assert-PathInsideRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $rootPath = Get-ComparablePath -Path $Root
    $targetPath = Get-ComparablePath -Path $Path
    $rootPrefix = $rootPath + [System.IO.Path]::DirectorySeparatorChar

    if (-not $targetPath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Name must be inside the repository root: $targetPath"
    }
}

function Assert-FfmpegArchiveDestinationPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    Assert-PathInsideRoot -Path $Path -Name "FfmpegArchiveDestinationDir"

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        throw "FfmpegArchiveDestinationDir must name a directory, not an existing file: $Path"
    }

    Assert-ParentDirectoryPath -Path $Path -Name "FfmpegArchiveDestinationDir"
}

function Assert-VerificationReportRecordOutputPathConflict {
    param(
        [Parameter(Mandatory = $true)][string]$RecordTestOutputPath,
        [Parameter(Mandatory = $true)][string]$VerificationReportPath
    )

    $recordOutputPath = Get-ComparablePath -Path $RecordTestOutputPath
    $reportPath = Get-ComparablePath -Path $VerificationReportPath

    if ([System.String]::Equals($recordOutputPath, $reportPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "VerificationReportPath must not be the same path as RecordTestOutputDir: $VerificationReportPath"
    }
}

function Write-VerificationReport {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [string]$FfmpegRoot,

        [string]$FfmpegIncludeDir,

        [string]$FfmpegLibraryDir,

        [string]$FfmpegBinaryDir,

        [string[]]$FfmpegArchivePath = @(),

        [Parameter(Mandatory = $true)]
        [string]$RecordTestOutputPath,

        [Parameter(Mandatory = $true)]
        [string]$RecordTestMp4,

        [Parameter(Mandatory = $true)]
        [string]$WgcSmokeStatus,

        [Parameter(Mandatory = $true)]
        [int]$WgcDurationMs,

        [Parameter(Mandatory = $true)]
        [uint32]$Width,

        [Parameter(Mandatory = $true)]
        [uint32]$Height,

        [Parameter(Mandatory = $true)]
        [uint32]$Fps,

        [Parameter(Mandatory = $true)]
        [uint32]$BitrateMbps
    )

    $archiveInput = "<not supplied>"
    if ($FfmpegArchivePath.Count -gt 0) {
        $archiveInput = $FfmpegArchivePath -join "; "
    }

    $reportDir = Split-Path -Parent $Path
    if ([string]::IsNullOrWhiteSpace($reportDir)) {
        $reportDir = (Get-Location).Path
    } else {
        [void][System.IO.Directory]::CreateDirectory($reportDir)
    }

    $artifactInfo = Get-Item -LiteralPath $RecordTestMp4

    $lines = @(
        "O'Louie FFmpeg muxer verification report",
        "schema_version: 1",
        "generated_utc: $((Get-Date).ToUniversalTime().ToString("o"))",
        "configuration: $Configuration",
        "ffmpeg_root: $(ConvertTo-ReportValue -Value $FfmpegRoot)",
        "ffmpeg_include_dir: $(ConvertTo-ReportValue -Value $FfmpegIncludeDir)",
        "ffmpeg_library_dir: $(ConvertTo-ReportValue -Value $FfmpegLibraryDir)",
        "ffmpeg_binary_dir: $(ConvertTo-ReportValue -Value $FfmpegBinaryDir)",
        "ffmpeg_archive_input: $archiveInput",
        "record_test_output_dir: $RecordTestOutputPath",
        "video_mp4_artifact: $RecordTestMp4",
        "video_mp4_artifact_size_bytes: $($artifactInfo.Length)",
        "video_mp4_artifact_last_write_utc: $($artifactInfo.LastWriteTimeUtc.ToString("o"))",
        "record_test: passed",
        "mp4_artifact_inspection: passed",
        "wgc_smoke: $WgcSmokeStatus",
        "wgc_smoke_options: duration_ms=$WgcDurationMs width=$Width height=$Height fps=$Fps bitrate_mbps=$BitrateMbps",
        "verifier: tools\VerifyFfmpegMuxer.ps1"
    )

    $reportFileName = [System.IO.Path]::GetFileName($Path)
    $tempPath = Join-Path $reportDir (".$reportFileName.tmp-$([System.Guid]::NewGuid().ToString("N"))")

    try {
        [System.IO.File]::WriteAllLines($tempPath, [string[]]$lines, [System.Text.Encoding]::UTF8)
        Move-Item -LiteralPath $tempPath -Destination $Path -Force
    } catch {
        Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
        throw
    }
}

function Clear-ExistingVerificationReport {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return
    }

    Write-Host "Removing previous FFmpeg muxer verification report before this run:"
    Write-Host "  $Path"
    Remove-Item -LiteralPath $Path -Force
}

Assert-PositiveIntegerOption -Value $WgcDurationMs -Name "WgcDurationMs"
Assert-PositiveIntegerOption -Value $Width -Name "Width"
Assert-PositiveIntegerOption -Value $Height -Name "Height"
Assert-PositiveIntegerOption -Value $Fps -Name "Fps"
Assert-PositiveIntegerOption -Value $BitrateMbps -Name "BitrateMbps"

$archivePathsSupplied = Test-ArchivePathsSupplied
if ($archivePathsSupplied) {
    if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot) -or
        -not [string]::IsNullOrWhiteSpace($FfmpegIncludeDir) -or
        -not [string]::IsNullOrWhiteSpace($FfmpegLibraryDir) -or
        -not [string]::IsNullOrWhiteSpace($FfmpegBinaryDir)) {
        throw "Use either -FfmpegArchivePath or -FfmpegRoot/-FfmpegIncludeDir/-FfmpegLibraryDir/-FfmpegBinaryDir, not both."
    }
} else {
    if (-not [string]::IsNullOrWhiteSpace($FfmpegArchiveDestinationDir)) {
        throw "-FfmpegArchiveDestinationDir requires -FfmpegArchivePath."
    }

    if ($ForceFfmpegArchivePreparation) {
        throw "-ForceFfmpegArchivePreparation requires -FfmpegArchivePath."
    }
}

if ([string]::IsNullOrWhiteSpace($RecordTestOutputDir)) {
    $RecordTestOutputDir = Join-Path $BuildDir "ffmpeg-record-test-output"
}
$recordTestOutputPath = Get-RepoRelativeFullPath -Path $RecordTestOutputDir
Assert-RecordTestOutputPath -Path $recordTestOutputPath
if ([string]::IsNullOrWhiteSpace($VerificationReportPath)) {
    $VerificationReportPath = Join-Path $recordTestOutputPath "verification-report.txt"
}
$verificationReportFullPath = Get-RepoRelativeFullPath -Path $VerificationReportPath
Assert-VerificationReportPath -Path $verificationReportFullPath
Assert-VerificationReportRecordOutputPathConflict -RecordTestOutputPath $recordTestOutputPath -VerificationReportPath $verificationReportFullPath

$preparedFfmpegRoot = ""
if ($archivePathsSupplied) {
    if ([string]::IsNullOrWhiteSpace($FfmpegArchiveDestinationDir)) {
        $FfmpegArchiveDestinationDir = Join-Path $Root "_deps\ffmpeg"
    }
    $preparedFfmpegRoot = Get-RepoRelativeFullPath -Path $FfmpegArchiveDestinationDir
    Assert-FfmpegArchiveDestinationPath -Path $preparedFfmpegRoot
}

Clear-ExistingVerificationReport -Path $verificationReportFullPath

if ($archivePathsSupplied) {
    Write-Host "Preparing FFmpeg root from user-supplied archive(s) for muxer verification..."
    $prepareArgs = [System.Collections.Generic.List[string]]::new()
    $prepareArgs.Add("-NoProfile")
    $prepareArgs.Add("-ExecutionPolicy")
    $prepareArgs.Add("Bypass")
    $prepareArgs.Add("-File")
    $prepareArgs.Add($ArchivePreparer)
    $prepareArgs.Add("-ArchivePath")
    $prepareArgs.Add(($FfmpegArchivePath -join ","))
    $prepareArgs.Add("-DestinationDir")
    $prepareArgs.Add($preparedFfmpegRoot)
    if ($ForceFfmpegArchivePreparation) {
        $prepareArgs.Add("-Force")
    }

    Invoke-Native -FilePath "powershell.exe" -Arguments $prepareArgs.ToArray()
    $FfmpegRoot = $preparedFfmpegRoot
}

$ffmpegArgs = [System.Collections.Generic.List[string]]::new()
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegRoot" -Value $FfmpegRoot
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegIncludeDir" -Value $FfmpegIncludeDir
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegLibraryDir" -Value $FfmpegLibraryDir
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegBinaryDir" -Value $FfmpegBinaryDir

Write-Host "Verifying FFmpeg root layout..."
$verifyArgs = [System.Collections.Generic.List[string]]::new()
$verifyArgs.Add("-NoProfile")
$verifyArgs.Add("-ExecutionPolicy")
$verifyArgs.Add("Bypass")
$verifyArgs.Add("-File")
$verifyArgs.Add($Verifier)
foreach ($arg in $ffmpegArgs) {
    $verifyArgs.Add($arg)
}
Invoke-Native -FilePath "powershell.exe" -Arguments $verifyArgs.ToArray()

Write-Host "Building FFmpeg-enabled O'Louie targets and running CTest..."
$buildArgs = [System.Collections.Generic.List[string]]::new()
$buildArgs.Add("-NoProfile")
$buildArgs.Add("-ExecutionPolicy")
$buildArgs.Add("Bypass")
$buildArgs.Add("-File")
$buildArgs.Add($BuildScript)
$buildArgs.Add("-Configuration")
$buildArgs.Add($Configuration)
$buildArgs.Add("-EnableFfmpeg")
foreach ($arg in $ffmpegArgs) {
    $buildArgs.Add($arg)
}
Invoke-Native -FilePath "powershell.exe" -Arguments $buildArgs.ToArray()

$recordTests = Join-Path $BuildDir "$Configuration\O'LouieRecordTests.exe"
if (-not (Test-Path -LiteralPath $recordTests -PathType Leaf)) {
    throw "Could not find FFmpeg-enabled O'LouieRecordTests at $recordTests."
}

Write-Host "Running FFmpeg-enabled O'LouieRecordTests directly..."
$previousRecordTestOutputDir = $env:OLOUIE_RECORD_TEST_OUTPUT_DIR
try {
    $env:OLOUIE_RECORD_TEST_OUTPUT_DIR = $recordTestOutputPath
    Invoke-Native -FilePath $recordTests
} finally {
    if ($null -eq $previousRecordTestOutputDir) {
        Remove-Item Env:\OLOUIE_RECORD_TEST_OUTPUT_DIR -ErrorAction SilentlyContinue
    } else {
        $env:OLOUIE_RECORD_TEST_OUTPUT_DIR = $previousRecordTestOutputDir
    }
}
Write-Host "FFmpeg-enabled record test output preserved at:"
Write-Host "  $recordTestOutputPath"
$recordTestMp4 = Join-Path $recordTestOutputPath "O'LouieRecordTests\exports\h264.mp4"
if (-not (Test-Path -LiteralPath $recordTestMp4 -PathType Leaf)) {
    throw "Expected FFmpeg video-only MP4 test artifact was not produced: $recordTestMp4"
}
Write-Host "FFmpeg video-only MP4 test artifact:"
Write-Host "  $recordTestMp4"
Write-Host "Inspecting FFmpeg video-only MP4 artifact..."
Invoke-Native -FilePath "powershell.exe" -Arguments @(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $Mp4Inspector,
    "-Path",
    $recordTestMp4)

$wgcSmokeStatus = "skipped"
if ($RunWgcSmoke) {
    $encodeSmoke = Join-Path $BuildDir "$Configuration\O'LouieEncodeSmoke.exe"
    if (-not (Test-Path -LiteralPath $encodeSmoke -PathType Leaf)) {
        throw "Could not find FFmpeg-enabled O'LouieEncodeSmoke at $encodeSmoke."
    }

    Write-Host "Running FFmpeg-enabled WGC-to-H.264-to-MP4 smoke..."
    Invoke-Native -FilePath $encodeSmoke -Arguments @("--h264-wgc-submit", $WgcDurationMs, $Width, $Height, $Fps, $BitrateMbps)
    $wgcSmokeStatus = "passed"
} else {
    Write-Host "Skipping WGC smoke. Re-run with -RunWgcSmoke after screen-capture consent is acceptable."
}

Write-VerificationReport `
    -Path $verificationReportFullPath `
    -Configuration $Configuration `
    -FfmpegRoot $FfmpegRoot `
    -FfmpegIncludeDir $FfmpegIncludeDir `
    -FfmpegLibraryDir $FfmpegLibraryDir `
    -FfmpegBinaryDir $FfmpegBinaryDir `
    -FfmpegArchivePath $FfmpegArchivePath `
    -RecordTestOutputPath $recordTestOutputPath `
    -RecordTestMp4 $recordTestMp4 `
    -WgcSmokeStatus $wgcSmokeStatus `
    -WgcDurationMs $WgcDurationMs `
    -Width $Width `
    -Height $Height `
    -Fps $Fps `
    -BitrateMbps $BitrateMbps
Write-Host "FFmpeg muxer verification report:"
Write-Host "  $verificationReportFullPath"
Write-Host "FFmpeg muxer verification sequence completed."
