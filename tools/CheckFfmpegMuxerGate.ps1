[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",

    [string]$FfmpegRoot = $env:OLOUIE_FFMPEG_ROOT,

    [string]$FfmpegIncludeDir = $env:OLOUIE_FFMPEG_INCLUDE_DIR,

    [string]$FfmpegLibraryDir = $env:OLOUIE_FFMPEG_LIBRARY_DIR,

    [string]$FfmpegBinaryDir = $env:OLOUIE_FFMPEG_BINARY_DIR,

    [string[]]$FfmpegArchivePath = @(),

    [string]$RecordTestOutputDir = "",

    [string]$VerificationReportPath = "",

    [switch]$RunVerification,

    [switch]$RequireReady,

    [switch]$RequireVerified,

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
$DefaultFfmpegRoot = Join-Path $Root "_deps\ffmpeg"
$BuildDir = Join-Path $Root "build-ffmpeg"
$RootVerifier = Join-Path $ToolsDir "VerifyFfmpegRoot.ps1"
$MuxerVerifier = Join-Path $ToolsDir "VerifyFfmpegMuxer.ps1"
$ArchiveInspector = Join-Path $ToolsDir "PrepareFfmpegRootFromArchive.ps1"
$Mp4Inspector = Join-Path $ToolsDir "InspectMp4Artifact.ps1"
$VerificationReportFutureTolerance = [System.TimeSpan]::FromMinutes(5)
$RecordTestOutputDirWasSupplied = $PSBoundParameters.ContainsKey("RecordTestOutputDir") -and
    (-not [string]::IsNullOrWhiteSpace($RecordTestOutputDir))
$VerificationReportPathWasSupplied = $PSBoundParameters.ContainsKey("VerificationReportPath") -and
    (-not [string]::IsNullOrWhiteSpace($VerificationReportPath))

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

function Format-CommandArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -match '^[A-Za-z0-9_./\\:-]+$') {
        return $Value
    }

    $escaped = $Value -replace '`', '``'
    $escaped = $escaped -replace '"', '`"'
    return "`"$escaped`""
}

function Format-CommandLine {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $parts = foreach ($argument in $Arguments) {
        Format-CommandArgument -Value $argument
    }

    return $parts -join " "
}

function Add-EvidenceTargetArgs {
    param(
        [System.Collections.Generic.List[string]]$Args,
        [Parameter(Mandatory = $true)][string]$RecordTestOutputPath,
        [Parameter(Mandatory = $true)][string]$VerificationReportPath
    )

    if ($RecordTestOutputDirWasSupplied) {
        $Args.Add("-RecordTestOutputDir")
        $Args.Add($RecordTestOutputPath)
    }

    if ($VerificationReportPathWasSupplied) {
        $Args.Add("-VerificationReportPath")
        $Args.Add($VerificationReportPath)
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    & $FilePath @Arguments
    $script:LastNativeExitCode = $LASTEXITCODE
}

function Test-SplitDirsSupplied {
    return (-not [string]::IsNullOrWhiteSpace($FfmpegIncludeDir)) -and
        (-not [string]::IsNullOrWhiteSpace($FfmpegLibraryDir)) -and
        (-not [string]::IsNullOrWhiteSpace($FfmpegBinaryDir))
}

function Test-AnySplitDirsSupplied {
    return (-not [string]::IsNullOrWhiteSpace($FfmpegIncludeDir)) -or
        (-not [string]::IsNullOrWhiteSpace($FfmpegLibraryDir)) -or
        (-not [string]::IsNullOrWhiteSpace($FfmpegBinaryDir))
}

function Test-ArchivePathsSupplied {
    foreach ($archivePath in $FfmpegArchivePath) {
        if (-not [string]::IsNullOrWhiteSpace($archivePath)) {
            return $true
        }
    }

    return $false
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

function Get-RepoRelativeFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar))
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

function Assert-VerificationReportRecordOutputPathConflict {
    param(
        [Parameter(Mandatory = $true)][string]$RecordTestOutputPath,
        [Parameter(Mandatory = $true)][string]$VerificationReportPath
    )

    $recordOutputPath = Get-NormalizedPath -Path $RecordTestOutputPath
    $reportPath = Get-NormalizedPath -Path $VerificationReportPath

    if ([System.String]::Equals($recordOutputPath, $reportPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "VerificationReportPath must not be the same path as RecordTestOutputDir: $VerificationReportPath"
    }
}

function Read-VerificationReport {
    param([Parameter(Mandatory = $true)][string]$Path)

    $lines = Get-Content -LiteralPath $Path
    if ($lines.Count -eq 0 -or $lines[0] -ne "O'Louie FFmpeg muxer verification report") {
        throw "verification report has an unexpected title."
    }

    $values = @{}
    for ($lineIndex = 1; $lineIndex -lt $lines.Count; ++$lineIndex) {
        $line = $lines[$lineIndex]
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        $separatorIndex = $line.IndexOf(":")
        if ($separatorIndex -lt 0) {
            throw "verification report contains malformed line $($lineIndex + 1)."
        }

        $key = $line.Substring(0, $separatorIndex).Trim()
        $value = $line.Substring($separatorIndex + 1).Trim()
        if ([string]::IsNullOrWhiteSpace($key)) {
            throw "verification report contains malformed line $($lineIndex + 1)."
        }

        if ($values.ContainsKey($key)) {
            throw "verification report contains duplicate '$key'."
        }

        $values[$key] = $value
    }

    return $values
}

function New-EvidenceResult {
    param(
        [bool]$Verified,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    return [pscustomobject]@{
        Verified = $Verified
        Reason = $Reason
    }
}

function Test-ReportValue {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$Expected
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    if ($Report[$Key] -ne $Expected) {
        return "verification report '$Key' is '$($Report[$Key])', expected '$Expected'."
    }

    return ""
}

function Test-ReportKnownKeys {
    param(
        [Parameter(Mandatory = $true)]$Report
    )

    $knownKeys = @{}
    foreach ($key in @(
            "schema_version",
            "generated_utc",
            "configuration",
            "ffmpeg_root",
            "ffmpeg_include_dir",
            "ffmpeg_library_dir",
            "ffmpeg_binary_dir",
            "ffmpeg_archive_input",
            "record_test_output_dir",
            "video_mp4_artifact",
            "video_mp4_artifact_size_bytes",
            "video_mp4_artifact_last_write_utc",
            "record_test",
            "mp4_artifact_inspection",
            "wgc_smoke",
            "wgc_smoke_options",
            "verifier")) {
        $knownKeys[$key] = $true
    }

    foreach ($key in ($Report.Keys | Sort-Object)) {
        if (-not $knownKeys.ContainsKey($key)) {
            return "verification report contains unexpected key '$key'."
        }
    }

    return ""
}

function Test-ReportNonEmptyValue {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    if ([string]::IsNullOrWhiteSpace($Report[$Key])) {
        return "verification report '$Key' is empty."
    }

    return ""
}

function Test-ReportValueInSet {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string[]]$AllowedValues
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    if ($AllowedValues -notcontains $Report[$Key]) {
        return "verification report '$Key' is '$($Report[$Key])', expected one of: $($AllowedValues -join ', ')."
    }

    return ""
}

function Test-ReportWgcSmokeOptions {
    param(
        [Parameter(Mandatory = $true)]$Report
    )

    $key = "wgc_smoke_options"
    $nonEmptyCheck = Test-ReportNonEmptyValue -Report $Report -Key $key
    if (-not [string]::IsNullOrWhiteSpace($nonEmptyCheck)) {
        return $nonEmptyCheck
    }

    $requiredKeys = @("duration_ms", "width", "height", "fps", "bitrate_mbps")
    $values = @{}
    foreach ($part in ($Report[$key] -split "\s+")) {
        if ([string]::IsNullOrWhiteSpace($part)) {
            continue
        }

        $separatorIndex = $part.IndexOf("=")
        if ($separatorIndex -lt 1 -or $separatorIndex -eq ($part.Length - 1)) {
            return "verification report '$key' contains malformed option '$part'."
        }

        $optionKey = $part.Substring(0, $separatorIndex)
        $optionValue = $part.Substring($separatorIndex + 1)
        if ($requiredKeys -notcontains $optionKey) {
            return "verification report '$key' contains unexpected option '$optionKey'."
        }

        if ($values.ContainsKey($optionKey)) {
            return "verification report '$key' contains duplicate option '$optionKey'."
        }

        $values[$optionKey] = $optionValue
    }

    foreach ($requiredKey in $requiredKeys) {
        if (-not $values.ContainsKey($requiredKey)) {
            return "verification report '$key' is missing option '$requiredKey'."
        }

        [int64]$number = 0
        if (-not [int64]::TryParse($values[$requiredKey], [ref]$number)) {
            return "verification report '$key' option '$requiredKey' is '$($values[$requiredKey])', expected a positive integer."
        }

        if ($number -le 0) {
            return "verification report '$key' option '$requiredKey' is '$number', expected a positive integer."
        }
    }

    return ""
}

function Test-ReportPathValue {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$ExpectedPath
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    try {
        $actual = Get-NormalizedPath -Path $Report[$Key]
        $expected = Get-NormalizedPath -Path $ExpectedPath
        if (-not [string]::Equals($actual, $expected, [System.StringComparison]::OrdinalIgnoreCase)) {
            return "verification report '$Key' points at '$($Report[$Key])', expected '$ExpectedPath'."
        }
    } catch {
        return "verification report '$Key' contains an invalid path: $($Report[$Key])"
    }

    return ""
}

function Test-ReportOptionalPathValue {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key,
        [string]$ExpectedPath
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    $actualValue = $Report[$Key]
    if ([string]::IsNullOrWhiteSpace($ExpectedPath)) {
        if ($actualValue -ne "<not supplied>") {
            return "verification report '$Key' is '$actualValue', expected '<not supplied>'."
        }

        return ""
    }

    if ($actualValue -eq "<not supplied>") {
        return "verification report '$Key' is '<not supplied>', expected '$ExpectedPath'."
    }

    try {
        $actual = Get-NormalizedPath -Path $actualValue
        $expected = Get-NormalizedPath -Path $ExpectedPath
        if (-not [string]::Equals($actual, $expected, [System.StringComparison]::OrdinalIgnoreCase)) {
            return "verification report '$Key' points at '$actualValue', expected '$ExpectedPath'."
        }
    } catch {
        return "verification report '$Key' contains an invalid path: $actualValue"
    }

    return ""
}

function Test-ReportInt64Value {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][int64]$Expected
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    [int64]$actual = 0
    if (-not [int64]::TryParse($Report[$Key], [ref]$actual)) {
        return "verification report '$Key' is '$($Report[$Key])', expected an integer."
    }

    if ($actual -ne $Expected) {
        return "verification report '$Key' is '$actual', expected '$Expected'."
    }

    return ""
}

function Test-ReportUtcTimestampValue {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$ExpectedUtcText
    )

    if (-not $Report.ContainsKey($Key)) {
        return "verification report is missing '$Key'."
    }

    try {
        $actualUtcText = ([System.DateTimeOffset]::Parse($Report[$Key])).UtcDateTime.ToString("o")
    } catch {
        return "verification report '$Key' is not a valid timestamp."
    }

    if ($actualUtcText -ne $ExpectedUtcText) {
        return "verification report '$Key' is '$actualUtcText', expected '$ExpectedUtcText'."
    }

    return ""
}

function Test-MuxerVerificationEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$ReportPath,
        [Parameter(Mandatory = $true)][string]$RecordTestOutputPath,
        [Parameter(Mandatory = $true)][string]$RecordTestMp4,
        [string]$FfmpegRoot,
        [string]$FfmpegIncludeDir,
        [string]$FfmpegLibraryDir,
        [string]$FfmpegBinaryDir
    )

    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
        return New-EvidenceResult -Verified $false -Reason "no success report found at $ReportPath."
    }

    if (-not (Test-Path -LiteralPath $RecordTestMp4 -PathType Leaf)) {
        return New-EvidenceResult -Verified $false -Reason "no preserved MP4 artifact found at $RecordTestMp4."
    }

    try {
        $artifactInfo = Get-Item -LiteralPath $RecordTestMp4
    } catch {
        return New-EvidenceResult -Verified $false -Reason "could not read preserved MP4 artifact metadata: $($_.Exception.Message)"
    }

    try {
        $report = Read-VerificationReport -Path $ReportPath
    } catch {
        return New-EvidenceResult -Verified $false -Reason $_.Exception.Message
    }

    if (-not $report.ContainsKey("generated_utc")) {
        return New-EvidenceResult -Verified $false -Reason "verification report is missing 'generated_utc'."
    }

    try {
        $generatedUtc = ([System.DateTimeOffset]::Parse($report["generated_utc"])).UtcDateTime
    } catch {
        return New-EvidenceResult -Verified $false -Reason "verification report 'generated_utc' is not a valid timestamp."
    }

    if ($generatedUtc -lt $artifactInfo.LastWriteTimeUtc) {
        return New-EvidenceResult -Verified $false -Reason "verification report 'generated_utc' is older than the preserved MP4 artifact."
    }

    if ($generatedUtc -gt (Get-Date).ToUniversalTime().Add($VerificationReportFutureTolerance)) {
        return New-EvidenceResult -Verified $false -Reason "verification report 'generated_utc' is more than 5 minutes in the future."
    }

    foreach ($check in @(
            (Test-ReportKnownKeys -Report $report),
            (Test-ReportValue -Report $report -Key "schema_version" -Expected "1"),
            (Test-ReportValue -Report $report -Key "verifier" -Expected "tools\VerifyFfmpegMuxer.ps1"),
            (Test-ReportValue -Report $report -Key "configuration" -Expected $Configuration),
            (Test-ReportOptionalPathValue -Report $report -Key "ffmpeg_root" -ExpectedPath $FfmpegRoot),
            (Test-ReportOptionalPathValue -Report $report -Key "ffmpeg_include_dir" -ExpectedPath $FfmpegIncludeDir),
            (Test-ReportOptionalPathValue -Report $report -Key "ffmpeg_library_dir" -ExpectedPath $FfmpegLibraryDir),
            (Test-ReportOptionalPathValue -Report $report -Key "ffmpeg_binary_dir" -ExpectedPath $FfmpegBinaryDir),
            (Test-ReportNonEmptyValue -Report $report -Key "ffmpeg_archive_input"),
            (Test-ReportValue -Report $report -Key "record_test" -Expected "passed"),
            (Test-ReportValue -Report $report -Key "mp4_artifact_inspection" -Expected "passed"),
            (Test-ReportValueInSet -Report $report -Key "wgc_smoke" -AllowedValues @("skipped", "passed")),
            (Test-ReportWgcSmokeOptions -Report $report),
            (Test-ReportPathValue -Report $report -Key "record_test_output_dir" -ExpectedPath $RecordTestOutputPath),
            (Test-ReportPathValue -Report $report -Key "video_mp4_artifact" -ExpectedPath $RecordTestMp4),
            (Test-ReportInt64Value -Report $report -Key "video_mp4_artifact_size_bytes" -Expected $artifactInfo.Length),
            (Test-ReportUtcTimestampValue -Report $report -Key "video_mp4_artifact_last_write_utc" -ExpectedUtcText $artifactInfo.LastWriteTimeUtc.ToString("o")))) {
        if (-not [string]::IsNullOrWhiteSpace($check)) {
            return New-EvidenceResult -Verified $false -Reason $check
        }
    }

    Write-Host "Inspecting preserved FFmpeg muxer MP4 artifact for status evidence..."
    $script:LastNativeExitCode = 0
    Invoke-Native -FilePath "powershell.exe" -Arguments @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $Mp4Inspector,
        "-Path",
        $RecordTestMp4)
    if ($script:LastNativeExitCode -ne 0) {
        return New-EvidenceResult -Verified $false -Reason "preserved MP4 artifact inspection failed."
    }

    return New-EvidenceResult -Verified $true -Reason "success report and preserved MP4 artifact are present, current, and consistent."
}

function Write-PendingGate {
    Write-Host "O'Louie FFmpeg muxer gate: Pending"
    Write-Host "  Reason: no local FFmpeg development/runtime root was supplied or found at _deps\ffmpeg."
    Write-Host "  Prepare from one or more user-selected LGPL dynamic ZIP archives:"
    Write-Host "    .\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath <archive.zip>[,<archive.zip>] -DestinationDir .\_deps\ffmpeg"
    Write-Host "  To inspect archives without preparing _deps\ffmpeg:"
    Write-Host "    .\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath <archive.zip>[,<archive.zip>] -InspectOnly"
    Write-Host "  To prepare and verify from archives in one explicit local command:"
    Write-Host "    .\tools\VerifyFfmpegMuxer.ps1 -Configuration $Configuration -FfmpegArchivePath <archive.zip>[,<archive.zip>]"
    Write-Host "  Or build a local LGPL shared root from the pinned official source release:"
    Write-Host "    .\tools\BuildFfmpegLgplFromSource.ps1 -DescribePlan"
    Write-Host "    .\tools\BuildFfmpegLgplFromSource.ps1 -CheckPrerequisites"
    Write-Host "    .\tools\BuildFfmpegLgplFromSource.ps1 -Force"
    Write-Host "  Then verify the configured muxer path:"
    Write-Host "    .\tools\VerifyFfmpegMuxer.ps1 -Configuration $Configuration -FfmpegRoot .\_deps\ffmpeg"
    Write-Host "  Existing split include/lib/bin directories can be supplied directly with -FfmpegIncludeDir, -FfmpegLibraryDir, and -FfmpegBinaryDir."
}

function Write-ArchiveCandidatePendingGate {
    Write-Host "O'Louie FFmpeg muxer gate: Pending"
    Write-Host "  Reason: the supplied archive set inspected successfully, but no local FFmpeg root has been prepared or muxer verification run."
    Write-Host "  Prepare the inspected archive set:"
    Write-Host "    .\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath <archive.zip>[,<archive.zip>] -DestinationDir .\_deps\ffmpeg"
    Write-Host "  Then verify the configured muxer path:"
    Write-Host "    .\tools\VerifyFfmpegMuxer.ps1 -Configuration $Configuration -FfmpegRoot .\_deps\ffmpeg"
    Write-Host "  Or prepare and verify it in one explicit local command:"
    Write-Host "    .\tools\VerifyFfmpegMuxer.ps1 -Configuration $Configuration -FfmpegArchivePath <archive.zip>[,<archive.zip>]"
}

function Write-ArchiveInputConflictGate {
    Write-Host "O'Louie FFmpeg muxer gate: Not ready"
    Write-Host "  Reason: -FfmpegArchivePath is archive-inspection input and cannot be combined with -FfmpegRoot or split include/lib/bin directories."
    Write-Host "  Use one status input mode at a time: inspect archives, or check a prepared root or split-directory set."
}

function Write-ArchiveRunVerificationConflictGate {
    Write-Host "O'Louie FFmpeg muxer gate: Not ready"
    Write-Host "  Reason: -FfmpegArchivePath on the gate checker is inspect-only and cannot be combined with -RunVerification."
    Write-Host "  To prepare archives and run the muxer verifier in one command, use:"
    Write-Host "    .\tools\VerifyFfmpegMuxer.ps1 -Configuration $Configuration -FfmpegArchivePath <archive.zip>[,<archive.zip>]"
    Write-Host "  Or prepare the archive set first, then run gate verification against the prepared root."
}

function Write-RunVerificationMissingRootGate {
    Write-Host "O'Louie FFmpeg muxer gate: Not ready"
    Write-Host "  Reason: -RunVerification requires a prepared FFmpeg root or explicit split include/lib/bin directories."
    Write-Host "  Prepare from one or more user-selected LGPL dynamic ZIP archives:"
    Write-Host "    .\tools\PrepareFfmpegRootFromArchive.ps1 -ArchivePath <archive.zip>[,<archive.zip>] -DestinationDir .\_deps\ffmpeg"
    Write-Host "  Or prepare and verify from archives in one explicit local command:"
    Write-Host "    .\tools\VerifyFfmpegMuxer.ps1 -Configuration $Configuration -FfmpegArchivePath <archive.zip>[,<archive.zip>]"
    Write-Host "  Or build a local LGPL shared root from the pinned official source release:"
    Write-Host "    .\tools\BuildFfmpegLgplFromSource.ps1 -DescribePlan"
    Write-Host "    .\tools\BuildFfmpegLgplFromSource.ps1 -CheckPrerequisites"
    Write-Host "    .\tools\BuildFfmpegLgplFromSource.ps1 -Force"
    Write-Host "  Existing split include/lib/bin directories can be supplied directly with -FfmpegIncludeDir, -FfmpegLibraryDir, and -FfmpegBinaryDir."
}

$wgcOptionSupplied = $RunWgcSmoke -or
    $PSBoundParameters.ContainsKey("WgcDurationMs") -or
    $PSBoundParameters.ContainsKey("Width") -or
    $PSBoundParameters.ContainsKey("Height") -or
    $PSBoundParameters.ContainsKey("Fps") -or
    $PSBoundParameters.ContainsKey("BitrateMbps")
if ($wgcOptionSupplied -and -not $RunVerification) {
    throw "WGC smoke options require -RunVerification."
}

Assert-PositiveIntegerOption -Value $WgcDurationMs -Name "WgcDurationMs"
Assert-PositiveIntegerOption -Value $Width -Name "Width"
Assert-PositiveIntegerOption -Value $Height -Name "Height"
Assert-PositiveIntegerOption -Value $Fps -Name "Fps"
Assert-PositiveIntegerOption -Value $BitrateMbps -Name "BitrateMbps"

if (Test-ArchivePathsSupplied) {
    if ($RunVerification) {
        Write-ArchiveRunVerificationConflictGate
        exit 1
    }

    if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot) -or (Test-AnySplitDirsSupplied)) {
        Write-ArchiveInputConflictGate
        exit 1
    }

    $archiveInspectArgs = [System.Collections.Generic.List[string]]::new()
    $archiveInspectArgs.Add("-NoProfile")
    $archiveInspectArgs.Add("-ExecutionPolicy")
    $archiveInspectArgs.Add("Bypass")
    $archiveInspectArgs.Add("-File")
    $archiveInspectArgs.Add($ArchiveInspector)
    $archiveInspectArgs.Add("-ArchivePath")
    $archiveInspectArgs.Add(($FfmpegArchivePath -join ","))
    $archiveInspectArgs.Add("-InspectOnly")

    Write-Host "Inspecting FFmpeg archive candidate for the current muxer gate..."
    $script:LastNativeExitCode = 0
    Invoke-Native -FilePath "powershell.exe" -Arguments $archiveInspectArgs.ToArray()
    $archiveInspectExitCode = $script:LastNativeExitCode
    if ($archiveInspectExitCode -ne 0) {
        Write-Host "O'Louie FFmpeg muxer gate: Not ready"
        Write-Host "  Reason: the supplied archive set failed inspect-only layout verification."
        exit $archiveInspectExitCode
    }

    Write-ArchiveCandidatePendingGate
    if ($RequireReady -or $RequireVerified) {
        exit 2
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($FfmpegRoot) -and -not (Test-SplitDirsSupplied)) {
    if (Test-AnySplitDirsSupplied) {
        Write-Host "O'Louie FFmpeg muxer gate: Not ready"
        Write-Host "  Reason: explicit split FFmpeg directories are incomplete. Provide all three of -FfmpegIncludeDir, -FfmpegLibraryDir, and -FfmpegBinaryDir, or provide -FfmpegRoot."
        exit 1
    }

    if (Test-Path -LiteralPath $DefaultFfmpegRoot -PathType Container) {
        $FfmpegRoot = $DefaultFfmpegRoot
    } else {
        if ($RunVerification) {
            Write-RunVerificationMissingRootGate
            exit 2
        }

        Write-PendingGate
        if ($RequireReady -or $RequireVerified) {
            exit 2
        }
        exit 0
    }
}

$ffmpegArgs = [System.Collections.Generic.List[string]]::new()
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegRoot" -Value $FfmpegRoot
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegIncludeDir" -Value $FfmpegIncludeDir
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegLibraryDir" -Value $FfmpegLibraryDir
Add-OptionalArg -Args $ffmpegArgs -Name "-FfmpegBinaryDir" -Value $FfmpegBinaryDir

$rootVerifyArgs = [System.Collections.Generic.List[string]]::new()
$rootVerifyArgs.Add("-NoProfile")
$rootVerifyArgs.Add("-ExecutionPolicy")
$rootVerifyArgs.Add("Bypass")
$rootVerifyArgs.Add("-File")
$rootVerifyArgs.Add($RootVerifier)
foreach ($arg in $ffmpegArgs) {
    $rootVerifyArgs.Add($arg)
}

Write-Host "Checking FFmpeg root layout for the current muxer gate..."
$script:LastNativeExitCode = 0
Invoke-Native -FilePath "powershell.exe" -Arguments $rootVerifyArgs.ToArray()
$rootExitCode = $script:LastNativeExitCode
if ($rootExitCode -ne 0) {
    Write-Host "O'Louie FFmpeg muxer gate: Not ready"
    Write-Host "  Reason: the supplied FFmpeg root or split directories failed layout verification."
    exit $rootExitCode
}

if ([string]::IsNullOrWhiteSpace($RecordTestOutputDir)) {
    $RecordTestOutputDir = Join-Path $BuildDir "ffmpeg-record-test-output"
}
$recordTestOutputPath = Get-RepoRelativeFullPath -Path $RecordTestOutputDir
Assert-RecordTestOutputPath -Path $recordTestOutputPath
$recordTestMp4 = Join-Path $recordTestOutputPath "O'LouieRecordTests\exports\h264.mp4"
if ([string]::IsNullOrWhiteSpace($VerificationReportPath)) {
    $VerificationReportPath = Join-Path $recordTestOutputPath "verification-report.txt"
}
$verificationReportFullPath = Get-RepoRelativeFullPath -Path $VerificationReportPath
Assert-VerificationReportPath -Path $verificationReportFullPath
Assert-VerificationReportRecordOutputPathConflict -RecordTestOutputPath $recordTestOutputPath -VerificationReportPath $verificationReportFullPath

if (-not $RunVerification) {
    $evidence = Test-MuxerVerificationEvidence `
        -ReportPath $verificationReportFullPath `
        -RecordTestOutputPath $recordTestOutputPath `
        -RecordTestMp4 $recordTestMp4 `
        -FfmpegRoot $FfmpegRoot `
        -FfmpegIncludeDir $FfmpegIncludeDir `
        -FfmpegLibraryDir $FfmpegLibraryDir `
        -FfmpegBinaryDir $FfmpegBinaryDir
    if ($evidence.Verified) {
        Write-Host "O'Louie FFmpeg muxer gate: Verified"
        Write-Host "  Report: $verificationReportFullPath"
        Write-Host "  Artifact: $recordTestMp4"
        Write-Host "  Evidence: $($evidence.Reason)"
        exit 0
    }

    Write-Host "O'Louie FFmpeg muxer gate: Ready for configured muxer verification"
    Write-Host "  Previous verification evidence: $($evidence.Reason)"
    Write-Host "  Expected report: $verificationReportFullPath"
    Write-Host "  Expected artifact: $recordTestMp4"
    $suggestedVerifyArgs = [System.Collections.Generic.List[string]]::new()
    $suggestedVerifyArgs.Add(".\tools\VerifyFfmpegMuxer.ps1")
    $suggestedVerifyArgs.Add("-Configuration")
    $suggestedVerifyArgs.Add($Configuration)
    if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot)) {
        $suggestedVerifyArgs.Add("-FfmpegRoot")
        $suggestedVerifyArgs.Add($FfmpegRoot)
    } else {
        $suggestedVerifyArgs.Add("-FfmpegIncludeDir")
        $suggestedVerifyArgs.Add($FfmpegIncludeDir)
        $suggestedVerifyArgs.Add("-FfmpegLibraryDir")
        $suggestedVerifyArgs.Add($FfmpegLibraryDir)
        $suggestedVerifyArgs.Add("-FfmpegBinaryDir")
        $suggestedVerifyArgs.Add($FfmpegBinaryDir)
    }
    Add-EvidenceTargetArgs `
        -Args $suggestedVerifyArgs `
        -RecordTestOutputPath $recordTestOutputPath `
        -VerificationReportPath $verificationReportFullPath
    Write-Host "  Run:"
    Write-Host "    $(Format-CommandLine -Arguments ($suggestedVerifyArgs.ToArray()))"
    if ($RequireVerified) {
        exit 2
    }
    exit 0
}

$muxerVerifyArgs = [System.Collections.Generic.List[string]]::new()
$muxerVerifyArgs.Add("-NoProfile")
$muxerVerifyArgs.Add("-ExecutionPolicy")
$muxerVerifyArgs.Add("Bypass")
$muxerVerifyArgs.Add("-File")
$muxerVerifyArgs.Add($MuxerVerifier)
$muxerVerifyArgs.Add("-Configuration")
$muxerVerifyArgs.Add($Configuration)
foreach ($arg in $ffmpegArgs) {
    $muxerVerifyArgs.Add($arg)
}
$muxerVerifyArgs.Add("-RecordTestOutputDir")
$muxerVerifyArgs.Add($recordTestOutputPath)
$muxerVerifyArgs.Add("-VerificationReportPath")
$muxerVerifyArgs.Add($verificationReportFullPath)
$muxerVerifyArgs.Add("-WgcDurationMs")
$muxerVerifyArgs.Add($WgcDurationMs.ToString())
$muxerVerifyArgs.Add("-Width")
$muxerVerifyArgs.Add($Width.ToString())
$muxerVerifyArgs.Add("-Height")
$muxerVerifyArgs.Add($Height.ToString())
$muxerVerifyArgs.Add("-Fps")
$muxerVerifyArgs.Add($Fps.ToString())
$muxerVerifyArgs.Add("-BitrateMbps")
$muxerVerifyArgs.Add($BitrateMbps.ToString())
if ($RunWgcSmoke) {
    $muxerVerifyArgs.Add("-RunWgcSmoke")
}

Write-Host "Running configured FFmpeg muxer verification..."
$script:LastNativeExitCode = 0
Invoke-Native -FilePath "powershell.exe" -Arguments $muxerVerifyArgs.ToArray()
$muxerExitCode = $script:LastNativeExitCode
if ($muxerExitCode -ne 0) {
    Write-Host "O'Louie FFmpeg muxer gate: Verification failed"
    exit $muxerExitCode
}

$evidence = Test-MuxerVerificationEvidence `
    -ReportPath $verificationReportFullPath `
    -RecordTestOutputPath $recordTestOutputPath `
    -RecordTestMp4 $recordTestMp4 `
    -FfmpegRoot $FfmpegRoot `
    -FfmpegIncludeDir $FfmpegIncludeDir `
    -FfmpegLibraryDir $FfmpegLibraryDir `
    -FfmpegBinaryDir $FfmpegBinaryDir
if (-not $evidence.Verified) {
    Write-Host "O'Louie FFmpeg muxer gate: Verification evidence failed"
    Write-Host "  Reason: $($evidence.Reason)"
    exit 1
}

Write-Host "O'Louie FFmpeg muxer gate: Verified"
Write-Host "  Report: $verificationReportFullPath"
Write-Host "  Artifact: $recordTestMp4"
