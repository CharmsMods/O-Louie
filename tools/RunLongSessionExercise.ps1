[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",
    [ValidateRange(30, 7200)]
    [int]$DurationSeconds = 3905,
    [ValidateRange(5, 300)]
    [int]$SampleIntervalSeconds = 30,
    [string]$EvidenceDirectory,
    [string]$FfprobePath,
    [switch]$AllowShortRun
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build-ffmpeg"
}

if ($DurationSeconds -le 3600 -and -not $AllowShortRun) {
    throw "DurationSeconds must exceed one hour unless -AllowShortRun is supplied for preflight."
}

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$binaryDirectory = Join-Path $resolvedBuild $Configuration
$appPath = Join-Path $binaryDirectory "O'Louie.exe"
$controlPath = Join-Path $binaryDirectory "O'LouieRuntimeControl.exe"
$stimulusPath = Join-Path $binaryDirectory "O'LouieLongSessionStimulus.exe"
$recordTestsPath = Join-Path $binaryDirectory "O'LouieRecordTests.exe"
$inspectorPath = Join-Path $PSScriptRoot "InspectMp4Artifact.ps1"
$supportPath = Join-Path $PSScriptRoot "LongSessionExerciseSupport.ps1"

foreach ($requiredPath in @($appPath, $controlPath, $stimulusPath, $recordTestsPath, $inspectorPath, $supportPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required long-session exercise dependency is missing: $requiredPath"
    }
}

. $supportPath

$ffprobe = Find-OLouieFfprobe -PreferredPath $FfprobePath
if ($null -eq $ffprobe) {
    throw "ffprobe.exe is required before starting a long-session evidence run."
}

if (Get-Process -Name "O'Louie" -ErrorAction SilentlyContinue) {
    throw "O'Louie is already running. Exit it before starting the bounded exercise."
}

$runName = "long-session-{0}" -f (Get-Date -Format "yyyyMMdd-HHmmss")
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $EvidenceDirectory = Join-Path $resolvedBuild $runName
}
$evidenceRoot = [System.IO.Path]::GetFullPath($EvidenceDirectory)
$artifactDirectory = Join-Path $evidenceRoot "artifacts"
$recoveryFixtureDirectory = Join-Path $evidenceRoot "isolated-recovery-fixture"
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $recoveryFixtureDirectory -Force | Out-Null

$localRoot = Join-Path $env:LOCALAPPDATA "O'Louie"
$settingsPath = Join-Path $localRoot "settings\settings.json"
$logPath = Join-Path $localRoot "logs\O'Louie.log"
$sessionRoot = Join-Path $localRoot "sessions"
$settingsBackupPath = Join-Path $evidenceRoot "settings-before.json"
$resourceCsvPath = Join-Path $evidenceRoot "resource-samples.csv"
$commandCsvPath = Join-Path $evidenceRoot "runtime-commands.csv"
$runLogPath = Join-Path $evidenceRoot "olouie-run.log"
$diagnosticsLogPath = Join-Path $evidenceRoot "diagnostics-snapshots.log"
$artifactCsvPath = Join-Path $evidenceRoot "artifacts.csv"
$inspectionLogPath = Join-Path $evidenceRoot "mp4-inspection.log"
$summaryPath = Join-Path $evidenceRoot "summary.json"
$cleanupPath = Join-Path $evidenceRoot "cleanup.json"
$settingsAfterPath = Join-Path $evidenceRoot "settings-after.json"

$settingsExisted = Test-Path -LiteralPath $settingsPath -PathType Leaf
if (-not $settingsExisted) {
    throw "The configured settings file is required for the long-session gate: $settingsPath"
}
Copy-Item -LiteralPath $settingsPath -Destination $settingsBackupPath -Force
$configuredSettings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
if (-not $configuredSettings.audio.system_mix -or -not $configuredSettings.audio.mic) {
    throw "The long-session gate requires both system audio and microphone capture to be enabled."
}
$configuredSettings.output_directory = $artifactDirectory
$exerciseSettingsJson = $configuredSettings | ConvertTo-Json -Depth 10 -Compress
[System.IO.File]::WriteAllText((Join-Path $evidenceRoot "settings-during.json"), $exerciseSettingsJson, [System.Text.UTF8Encoding]::new($false))

function Get-SessionInventory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string[]]$RestrictToDirectories
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return @()
    }
    $files = if ($PSBoundParameters.ContainsKey("RestrictToDirectories")) {
        foreach ($directory in $RestrictToDirectories) {
            if (Test-Path -LiteralPath $directory -PathType Container) {
                Get-ChildItem -LiteralPath $directory -File -Recurse -Force
            }
        }
    } else {
        Get-ChildItem -LiteralPath $Root -File -Recurse -Force
    }
    return @($files | Sort-Object FullName | ForEach-Object {
        $hash = $null
        if ($_.Length -le 4MB) {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
        [pscustomobject]@{
            Path = $_.FullName
            Length = $_.Length
            LastWriteTimeUtc = $_.LastWriteTimeUtc.ToString("o")
            Sha256 = $hash
        }
    })
}

function Read-LogTail {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int64]$Offset
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        if ($Offset -gt $stream.Length) {
            $Offset = 0
        }
        [void]$stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $remaining = [int]($stream.Length - $Offset)
        $bytes = New-Object byte[] $remaining
        $read = $stream.Read($bytes, 0, $remaining)
        return [System.Text.Encoding]::UTF8.GetString($bytes, 0, $read)
    } finally {
        $stream.Dispose()
    }
}

function Wait-ForLogPattern {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][int64]$Offset,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $text = Read-LogTail -Path $logPath -Offset $Offset
        if ($text -match $Pattern) {
            return $text
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $Description."
}

function Get-RecordingHealth {
    param([Parameter(Mandatory = $true)][int64]$Offset)

    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
        return "none"
    }
    $length = (Get-Item -LiteralPath $logPath).Length
    $recentOffset = [math]::Max($Offset, $length - 65536)
    $recent = Read-LogTail -Path $logPath -Offset $recentOffset
    $state = Get-OLouieRecorderLogState -LogText $recent
    if ($state -eq "stopping") {
        $completionText = Wait-ForLogPattern -Pattern "Recording (saved:|failed)" -TimeoutSeconds 900 -Offset $Offset -Description "manually stopped recording finalization"
        $state = Get-OLouieRecorderLogState -LogText $completionText
        $script:recordingStopped = $true
        $script:recordingTerminal = $true
    }
    if ($state -eq "saved") {
        $script:recordingStopped = $true
        $script:recordingTerminal = $true
        return $state
    }
    if ($state -eq "failed") {
        $script:recordingStopped = $true
        $script:recordingTerminal = $true
        throw "O'Louie entered a failed terminal state before the requested duration elapsed."
    }
    return $state
}

$commandRows = [System.Collections.Generic.List[object]]::new()
function Invoke-RuntimeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][double]$ElapsedSeconds,
        [string]$Argument
    )

    $arguments = @($Command)
    if (-not [string]::IsNullOrWhiteSpace($Argument)) {
        $arguments += $Argument
    }
    $output = & $controlPath @arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    $commandRows.Add([pscustomobject]@{
        TimestampUtc = [DateTime]::UtcNow.ToString("o")
        ElapsedSeconds = [math]::Round($ElapsedSeconds, 3)
        Command = $Command
        ExitCode = $exitCode
        Output = $output.Trim()
    })
    $commandRows | Export-Csv -LiteralPath $commandCsvPath -NoTypeInformation
    if ($exitCode -ne 0) {
        throw "Runtime command '$Command' failed: $($output.Trim())"
    }
}

$resourceRows = [System.Collections.Generic.List[object]]::new()
$lastCpuSeconds = $null
$lastSampleUtc = $null
function Add-ResourceSample {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [Parameter(Mandatory = $true)][double]$ElapsedSeconds
    )

    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    $now = [DateTime]::UtcNow
    $cpuSeconds = $process.TotalProcessorTime.TotalSeconds
    $cpuPercent = 0.0
    if ($null -ne $script:lastCpuSeconds -and $null -ne $script:lastSampleUtc) {
        $wallSeconds = ($now - $script:lastSampleUtc).TotalSeconds
        if ($wallSeconds -gt 0) {
            $cpuPercent = 100.0 * ($cpuSeconds - $script:lastCpuSeconds) /
                $wallSeconds / [Environment]::ProcessorCount
        }
    }
    $script:lastCpuSeconds = $cpuSeconds
    $script:lastSampleUtc = $now
    $cDrive = Get-PSDrive -Name C
    $dDrive = Get-PSDrive -Name D -ErrorAction SilentlyContinue
    $row = [pscustomobject]@{
        TimestampUtc = $now.ToString("o")
        ElapsedSeconds = [math]::Round($ElapsedSeconds, 3)
        Responding = $process.Responding
        CpuPercentOfMachine = [math]::Round($cpuPercent, 3)
        CpuTotalSeconds = [math]::Round($cpuSeconds, 3)
        WorkingSetBytes = $process.WorkingSet64
        PrivateBytes = $process.PrivateMemorySize64
        HandleCount = $process.HandleCount
        ThreadCount = $process.Threads.Count
        CFreeBytes = $cDrive.Free
        DFreeBytes = if ($null -ne $dDrive) { $dDrive.Free } else { $null }
    }
    $resourceRows.Add($row)
    $resourceRows | Export-Csv -LiteralPath $resourceCsvPath -NoTypeInformation
    Write-Host ("sample {0,7:n1}s responding={1} cpu={2,6:n2}% working={3,8:n1}MB private={4,8:n1}MB handles={5}" -f
        $ElapsedSeconds, $row.Responding, $row.CpuPercentOfMachine,
        ($row.WorkingSetBytes / 1MB), ($row.PrivateBytes / 1MB), $row.HandleCount)
}

function Get-ExerciseSchedule {
    param([Parameter(Mandatory = $true)][int]$Duration)

    if ($Duration -le 600) {
        return @(
            [pscustomobject]@{ At = 12; Command = "first-clip" },
            [pscustomobject]@{ At = 24; Command = "bookmark" },
            [pscustomobject]@{ At = 36; Command = "second-clip" },
            [pscustomobject]@{ At = 48; Command = "first-clip" },
            [pscustomobject]@{ At = 49; Command = "bookmark" },
            [pscustomobject]@{ At = 60; Command = "bookmark" }
        ) | Where-Object { $_.At -lt ($Duration - 5) }
    }

    return @(
        [pscustomobject]@{ At = 35; Command = "first-clip" },
        [pscustomobject]@{ At = 120; Command = "bookmark" },
        [pscustomobject]@{ At = 300; Command = "second-clip" },
        [pscustomobject]@{ At = 600; Command = "first-clip" },
        [pscustomobject]@{ At = 601; Command = "bookmark" },
        [pscustomobject]@{ At = 602; Command = "first-clip" },
        [pscustomobject]@{ At = 900; Command = "third-clip" },
        [pscustomobject]@{ At = 1500; Command = "bookmark" },
        [pscustomobject]@{ At = 2100; Command = "second-clip" },
        [pscustomobject]@{ At = 2700; Command = "first-clip" },
        [pscustomobject]@{ At = 3300; Command = "bookmark" },
        [pscustomobject]@{ At = 3650; Command = "first-clip" }
    ) | Where-Object { $_.At -lt ($Duration - 5) }
}

if (-not ([System.Management.Automation.PSTypeName]"OLouie.NativeMethods").Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace OLouie {
    public static class NativeMethods {
        [DllImport("kernel32.dll")]
        public static extern uint SetThreadExecutionState(uint flags);
    }
}
"@
}

$executionStateContinuous = [uint32]2147483648
$executionStateSystemRequired = [uint32]0x00000001
$executionStateDisplayRequired = [uint32]0x00000002
[void][OLouie.NativeMethods]::SetThreadExecutionState(
    $executionStateContinuous -bor $executionStateSystemRequired -bor $executionStateDisplayRequired)

$initialSessionDirectories = @()
if (Test-Path -LiteralPath $sessionRoot -PathType Container) {
    $initialSessionDirectories = @(Get-ChildItem -LiteralPath $sessionRoot -Directory -Force | ForEach-Object FullName)
}
$initialInventory = Get-SessionInventory -Root $sessionRoot -RestrictToDirectories $initialSessionDirectories
$initialInventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceRoot "sessions-before.json") -Encoding UTF8

$logOffset = if (Test-Path -LiteralPath $logPath -PathType Leaf) {
    (Get-Item -LiteralPath $logPath).Length
} else {
    0L
}

$appProcess = $null
$stimulusProcess = $null
$recordingStarted = $false
$recordingStopped = $false
$recordingTerminal = $false
$success = $false
$stopReason = "duration"
$stopRequestedElapsedSeconds = $null
$priorDiagnosticsLogging = [Environment]::GetEnvironmentVariable("OLOUIE_DIAGNOSTICS_LOGGING", "Process")
$env:OLOUIE_DIAGNOSTICS_LOGGING = "1"
$startedUtc = [DateTime]::UtcNow
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

try {
    [System.IO.File]::WriteAllText($settingsPath, $exerciseSettingsJson, [System.Text.UTF8Encoding]::new($false))
    $stimulusProcess = Start-Process -FilePath $stimulusPath -ArgumentList ($DurationSeconds + 300) -PassThru
    $appProcess = Start-Process -FilePath $appPath -PassThru
    [void](Wait-ForLogPattern -Pattern "O'Louie tray recorder shell is ready\." -TimeoutSeconds 30 -Offset $logOffset -Description "the tray shell to become ready")

    Invoke-RuntimeCommand -Command "toggle-recording" -ElapsedSeconds $stopwatch.Elapsed.TotalSeconds
    [void](Wait-ForLogPattern -Pattern "Recording started\." -TimeoutSeconds 45 -Offset $logOffset -Description "recording startup")
    $recordingStarted = $true
    $recordingClock = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "Recording started; bounded duration is $DurationSeconds seconds."

    $schedule = @(Get-ExerciseSchedule -Duration $DurationSeconds)
    $nextCommand = 0
    $nextSample = 0.0
    $nextHealthCheck = 1.0
    while ($recordingClock.Elapsed.TotalSeconds -lt $DurationSeconds) {
        if ($appProcess.HasExited) {
            throw "O'Louie exited unexpectedly with code $($appProcess.ExitCode)."
        }
        $elapsed = $recordingClock.Elapsed.TotalSeconds
        if ($elapsed -ge $nextHealthCheck) {
            $health = Get-RecordingHealth -Offset $logOffset
            if ($health -eq "saved") {
                $stopReason = "external-stop"
                $stopRequestedElapsedSeconds = $elapsed
                break
            }
            $nextHealthCheck += 1.0
        }
        while ($nextCommand -lt $schedule.Count -and $elapsed -ge $schedule[$nextCommand].At) {
            Invoke-RuntimeCommand -Command $schedule[$nextCommand].Command -ElapsedSeconds $elapsed
            $nextCommand++
            $elapsed = $recordingClock.Elapsed.TotalSeconds
        }
        if ($elapsed -ge $nextSample) {
            Add-ResourceSample -ProcessId $appProcess.Id -ElapsedSeconds $elapsed
            $nextSample += $SampleIntervalSeconds
        }
        Start-Sleep -Milliseconds 200
    }

    $health = Get-RecordingHealth -Offset $logOffset
    if ($health -eq "saved") {
        $stopReason = "external-stop"
    }
    if ($null -eq $stopRequestedElapsedSeconds) {
        $stopRequestedElapsedSeconds = $recordingClock.Elapsed.TotalSeconds
    }
    if (-not $recordingStopped) {
        Invoke-RuntimeCommand -Command "toggle-recording" -ElapsedSeconds $stopRequestedElapsedSeconds
        $recordingStopped = $true
        $completionText = Wait-ForLogPattern -Pattern "Recording (saved:|failed)" -TimeoutSeconds 900 -Offset $logOffset -Description "full-recording finalization"
        $terminalState = Get-OLouieRecorderLogState -LogText $completionText
        $recordingTerminal = $true
        if ($terminalState -ne "saved") {
            throw "The recording reached a failed terminal state."
        }
    }
    Add-ResourceSample -ProcessId $appProcess.Id -ElapsedSeconds $recordingClock.Elapsed.TotalSeconds
    Invoke-RuntimeCommand -Command "exit" -ElapsedSeconds $recordingClock.Elapsed.TotalSeconds
    if (-not $appProcess.WaitForExit(30000)) {
        throw "O'Louie did not exit within 30 seconds after its normal Exit command."
    }

    $runLog = Read-LogTail -Path $logPath -Offset $logOffset
    [System.IO.File]::WriteAllText($runLogPath, $runLog, [System.Text.UTF8Encoding]::new($false))
    $diagnosticLines = @($runLog -split "`r?`n" | Where-Object { $_ -like "*Diagnostics snapshot:*" })
    [System.IO.File]::WriteAllLines($diagnosticsLogPath, $diagnosticLines, [System.Text.UTF8Encoding]::new($false))
    if ($diagnosticLines.Count -lt 2) {
        throw "The run did not retain multiple diagnostics snapshots."
    }

    $mp4Files = @(Get-ChildItem -LiteralPath $artifactDirectory -Filter "*.mp4" -File | Sort-Object Name)
    if ($mp4Files.Count -lt 2) {
        throw "Expected a full recording and at least one export, found $($mp4Files.Count) MP4 files."
    }
    $artifactRows = [System.Collections.Generic.List[object]]::new()
    $inspectionLines = [System.Collections.Generic.List[string]]::new()
    foreach ($file in $mp4Files) {
        $inspectionOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $inspectorPath -Path $file.FullName 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "MP4 inspection failed for $($file.FullName): $inspectionOutput"
        }
        $inspectionLines.Add("=== $($file.FullName) ===")
        $inspectionLines.Add($inspectionOutput.Trim())
        $probeStatus = "unavailable"
        $probeOutput = ""
        $artifactKind = if ($file.BaseName -match "-clip-\d+$") {
            "clip"
        } elseif ($file.BaseName -match "-bookmark-\d+$") {
            "bookmark"
        } else {
            "full"
        }
        $durationSeconds = $null
        $formatSizeBytes = $null
        $videoStreamCount = $null
        $h264StreamCount = $null
        $audioStreamCount = $null
        $aacStreamCount = $null
        $audioSampleRates = ""
        $audioChannels = ""
        if ($null -ne $ffprobe) {
            $probeFacts = Get-OLouieMp4ProbeFacts -Path $file.FullName -FfprobePath $ffprobe
            $durationSeconds = $probeFacts.DurationSeconds
            $formatSizeBytes = $probeFacts.FormatSizeBytes
            $videoStreamCount = $probeFacts.VideoStreamCount
            $h264StreamCount = $probeFacts.H264StreamCount
            $audioStreamCount = $probeFacts.AudioStreamCount
            $aacStreamCount = $probeFacts.AacStreamCount
            $audioSampleRates = $probeFacts.AudioSampleRates
            $audioChannels = $probeFacts.AudioChannels
            if ($formatSizeBytes -ne $file.Length) {
                throw "ffprobe format size $formatSizeBytes does not match file size $($file.Length)."
            }
            $probeStatus = "passed"
            $inspectionLines.Add($probeFacts.ProbeOutput)
        }
        $artifactRows.Add([pscustomobject]@{
            Name = $file.Name
            Path = $file.FullName
            ArtifactKind = $artifactKind
            Length = $file.Length
            Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString("o")
            BoxInspection = "passed"
            Ffprobe = $probeStatus
            DurationSeconds = $durationSeconds
            FormatSizeBytes = $formatSizeBytes
            VideoStreamCount = $videoStreamCount
            H264StreamCount = $h264StreamCount
            AudioStreamCount = $audioStreamCount
            AacStreamCount = $aacStreamCount
            AudioSampleRates = $audioSampleRates
            AudioChannels = $audioChannels
        })
    }
    $artifactRows | Export-Csv -LiteralPath $artifactCsvPath -NoTypeInformation
    [System.IO.File]::WriteAllLines($inspectionLogPath, $inspectionLines, [System.Text.UTF8Encoding]::new($false))

    $partialFiles = @(Get-ChildItem -LiteralPath $artifactDirectory -File -Recurse | Where-Object {
        $_.Name -like "*.partial*" -or $_.Name -like "*.tmp" -or $_.Name -like "*.temp"
    })
    if ($partialFiles.Count -ne 0) {
        throw "Partial output artifacts remain: $($partialFiles.FullName -join ', ')"
    }

    $env:OLOUIE_RECORD_TEST_OUTPUT_DIR = $recoveryFixtureDirectory
    try {
        $recoveryTestOutput = & $recordTestsPath 2>&1 | Out-String
        $recoveryTestExitCode = $LASTEXITCODE
    } finally {
        Remove-Item Env:OLOUIE_RECORD_TEST_OUTPUT_DIR -ErrorAction SilentlyContinue
    }
    [System.IO.File]::WriteAllText((Join-Path $evidenceRoot "isolated-recovery-test.log"), $recoveryTestOutput, [System.Text.UTF8Encoding]::new($false))
    if ($recoveryTestExitCode -ne 0) {
        throw "The isolated recovery fixture failed: $recoveryTestOutput"
    }

    $finalInventory = Get-SessionInventory -Root $sessionRoot -RestrictToDirectories $initialSessionDirectories
    $finalInventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceRoot "sessions-after.json") -Encoding UTF8
    $beforeComparable = @($initialInventory | ForEach-Object { "{0}|{1}|{2}|{3}" -f $_.Path, $_.Length, $_.LastWriteTimeUtc, $_.Sha256 })
    $afterComparable = @($finalInventory | ForEach-Object { "{0}|{1}|{2}|{3}" -f $_.Path, $_.Length, $_.LastWriteTimeUtc, $_.Sha256 })
    $recoveryDifference = @(Compare-Object -ReferenceObject $beforeComparable -DifferenceObject $afterComparable)
    ConvertTo-Json -InputObject @($recoveryDifference) -Depth 4 |
        Set-Content -LiteralPath (Join-Path $evidenceRoot "preexisting-session-diff.json") -Encoding UTF8
    if ($recoveryDifference.Count -ne 0) {
        throw "One or more pre-existing session files changed during the exercise."
    }

    $lastDiagnostics = $diagnosticLines[-1]
    $configuredAudioTrackCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "audio_tracks"
    $packetBearingAudioTrackCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "packet_bearing_audio_tracks"
    $submittedExportCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "submitted_exports"
    $savedExportCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "saved_exports"
    $failedExportCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "failed_exports"
    foreach ($metric in @(
        @{ Name = "audio_tracks"; Value = $configuredAudioTrackCount },
        @{ Name = "packet_bearing_audio_tracks"; Value = $packetBearingAudioTrackCount },
        @{ Name = "submitted_exports"; Value = $submittedExportCount },
        @{ Name = "saved_exports"; Value = $savedExportCount },
        @{ Name = "failed_exports"; Value = $failedExportCount }
    )) {
        if ($null -eq $metric.Value) {
            throw "Final diagnostics are missing required metric '$($metric.Name)'."
        }
    }
    $resourceTrend = Get-LongSessionResourceTrend -Samples @($resourceRows) -WindowSize 5
    $summary = [ordered]@{
        EvidenceSchemaVersion = 3
        RunName = $runName
        Success = $true
        StartedUtc = $startedUtc.ToString("o")
        CompletedUtc = [DateTime]::UtcNow.ToString("o")
        RequestedDurationSeconds = $DurationSeconds
        RequestedDurationCompleted = ($stopReason -eq "duration")
        StopReason = $stopReason
        StopRequestedElapsedSeconds = [math]::Round($stopRequestedElapsedSeconds, 3)
        ObservedRecordingSeconds = [math]::Round($stopRequestedElapsedSeconds, 3)
        ResourceSampleCount = $resourceRows.Count
        ResourceTrend = $resourceTrend
        AllSamplesResponding = -not ($resourceRows.Responding -contains $false)
        WorkingSetFirstBytes = $resourceRows[0].WorkingSetBytes
        WorkingSetLastBytes = $resourceRows[-1].WorkingSetBytes
        WorkingSetMaximumBytes = ($resourceRows | Measure-Object WorkingSetBytes -Maximum).Maximum
        PrivateBytesFirst = $resourceRows[0].PrivateBytes
        PrivateBytesLast = $resourceRows[-1].PrivateBytes
        PrivateBytesMaximum = ($resourceRows | Measure-Object PrivateBytes -Maximum).Maximum
        HandleCountFirst = $resourceRows[0].HandleCount
        HandleCountLast = $resourceRows[-1].HandleCount
        HandleCountMaximum = ($resourceRows | Measure-Object HandleCount -Maximum).Maximum
        RuntimeCommandCount = $commandRows.Count
        RuntimeCommandFailures = @($commandRows | Where-Object ExitCode -ne 0).Count
        DiagnosticsSnapshotCount = $diagnosticLines.Count
        FinalDiagnostics = $lastDiagnostics
        SystemAudioRequested = [bool]$configuredSettings.audio.system_mix
        MicrophoneRequested = [bool]$configuredSettings.audio.mic
        SeparateAudioTracksRequested = [bool]$configuredSettings.audio.separate_tracks
        ConfiguredAudioSampleRate = [int]$configuredSettings.audio.sample_rate
        ConfiguredVideoEncoderBackend = [string]$configuredSettings.video.encoder_backend
        ConfiguredAudioTrackCount = $configuredAudioTrackCount
        PacketBearingAudioTrackCount = $packetBearingAudioTrackCount
        SubmittedExportCount = $submittedExportCount
        SavedExportCount = $savedExportCount
        FailedExportCount = $failedExportCount
        Mp4ArtifactCount = $artifactRows.Count
        FullArtifactCount = @($artifactRows | Where-Object ArtifactKind -eq "full").Count
        PartialArtifactCount = $partialFiles.Count
        FfprobeAvailable = $null -ne $ffprobe
        InitialSessionDirectoryCount = $initialSessionDirectories.Count
        PreexistingSessionDifferenceCount = $recoveryDifference.Count
        IsolatedRecoveryFixtureExitCode = $recoveryTestExitCode
        EvidenceDirectory = $evidenceRoot
        CleanupEvidencePath = $cleanupPath
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $success = $true
} finally {
    if ($null -ne $appProcess -and -not $appProcess.HasExited) {
        if ($recordingStarted -and -not $recordingStopped -and -not $recordingTerminal) {
            try {
                Invoke-RuntimeCommand -Command "toggle-recording" -ElapsedSeconds $stopwatch.Elapsed.TotalSeconds
                $recordingStopped = $true
                $completionText = Wait-ForLogPattern -Pattern "Recording (saved:|failed)" -TimeoutSeconds 900 -Offset $logOffset -Description "cancelled recording finalization"
                $terminalState = Get-OLouieRecorderLogState -LogText $completionText
                $recordingTerminal = $true
                if ($terminalState -ne "saved") {
                    Write-Warning "The cancelled recording reached a failed terminal state."
                }
            } catch {
                Write-Warning $_
            }
        }
        try {
            Invoke-RuntimeCommand -Command "exit" -ElapsedSeconds $stopwatch.Elapsed.TotalSeconds
            [void]$appProcess.WaitForExit(15000)
        } catch {
            Write-Warning $_
        }
        if (-not $appProcess.HasExited) {
            Stop-Process -Id $appProcess.Id -Force -ErrorAction SilentlyContinue
            [void]$appProcess.WaitForExit(10000)
        }
    }
    if ($null -ne $stimulusProcess -and -not $stimulusProcess.HasExited) {
        Stop-Process -Id $stimulusProcess.Id -Force -ErrorAction SilentlyContinue
        [void]$stimulusProcess.WaitForExit(10000)
    }

    $cleanupErrors = [System.Collections.Generic.List[string]]::new()
    $settingsRestored = $false
    $settingsBeforeHash = $null
    $settingsAfterHash = $null
    try {
        if ($settingsExisted -and (Test-Path -LiteralPath $settingsBackupPath -PathType Leaf)) {
            Copy-Item -LiteralPath $settingsBackupPath -Destination $settingsPath -Force
            Copy-Item -LiteralPath $settingsPath -Destination $settingsAfterPath -Force
            $settingsBeforeHash = (Get-FileHash -LiteralPath $settingsBackupPath -Algorithm SHA256).Hash
            $settingsAfterHash = (Get-FileHash -LiteralPath $settingsAfterPath -Algorithm SHA256).Hash
            $settingsRestored = $settingsBeforeHash -eq $settingsAfterHash
            if (-not $settingsRestored) {
                $cleanupErrors.Add("The restored settings do not match the pre-run settings.")
            }
        } else {
            $cleanupErrors.Add("The pre-run settings backup is unavailable for restoration.")
        }
    } catch {
        $cleanupErrors.Add("Settings restoration failed: $($_.Exception.Message)")
    }
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        $retainedRunLog = Read-LogTail -Path $logPath -Offset $logOffset
        [System.IO.File]::WriteAllText($runLogPath, $retainedRunLog, [System.Text.UTF8Encoding]::new($false))
        $retainedDiagnostics = @($retainedRunLog -split "`r?`n" | Where-Object { $_ -like "*Diagnostics snapshot:*" })
        [System.IO.File]::WriteAllLines($diagnosticsLogPath, $retainedDiagnostics, [System.Text.UTF8Encoding]::new($false))
    }
    if ($null -eq $priorDiagnosticsLogging) {
        Remove-Item Env:OLOUIE_DIAGNOSTICS_LOGGING -ErrorAction SilentlyContinue
    } else {
        $env:OLOUIE_DIAGNOSTICS_LOGGING = $priorDiagnosticsLogging
    }
    [void][OLouie.NativeMethods]::SetThreadExecutionState($executionStateContinuous)

    $appExited = $null -eq $appProcess -or $appProcess.HasExited
    $stimulusExited = $null -eq $stimulusProcess -or $stimulusProcess.HasExited
    $olouieProcessCount = @(Get-Process -Name "O'Louie" -ErrorAction SilentlyContinue).Count
    if (-not $appExited) {
        $cleanupErrors.Add("The exercise-owned O'Louie process is still running.")
    }
    if (-not $stimulusExited) {
        $cleanupErrors.Add("The long-session stimulus process is still running.")
    }
    if ($olouieProcessCount -ne 0) {
        $cleanupErrors.Add("One or more O'Louie processes remain after cleanup.")
    }
    $cleanupSucceeded = $settingsRestored -and $appExited -and $stimulusExited -and
        $olouieProcessCount -eq 0 -and $cleanupErrors.Count -eq 0
    $cleanup = [ordered]@{
        EvidenceSchemaVersion = 3
        Success = ($success -and $cleanupSucceeded)
        RunSucceededBeforeCleanup = $success
        CompletedUtc = [DateTime]::UtcNow.ToString("o")
        SettingsRestored = $settingsRestored
        SettingsBeforeSha256 = $settingsBeforeHash
        SettingsAfterSha256 = $settingsAfterHash
        AppProcessExited = $appExited
        StimulusProcessExited = $stimulusExited
        OLouieProcessCount = $olouieProcessCount
        Errors = @($cleanupErrors)
    }
    try {
        [System.IO.File]::WriteAllText(
            $cleanupPath,
            ($cleanup | ConvertTo-Json -Depth 4),
            [System.Text.UTF8Encoding]::new($false))
    } catch {
        $cleanupErrors.Add("Cleanup evidence publication failed: $($_.Exception.Message)")
    }
    foreach ($cleanupError in $cleanupErrors) {
        Write-Warning $cleanupError
    }
    if ($success -and (-not $cleanupSucceeded -or $cleanupErrors.Count -ne 0)) {
        throw "Long-session exercise cleanup verification failed."
    }
    if (-not $success) {
        Write-Warning "Long-session exercise did not complete; retained evidence at $evidenceRoot"
    }
}

if ($success) {
    Write-Host "Long-session exercise succeeded. Evidence: $evidenceRoot"
}
