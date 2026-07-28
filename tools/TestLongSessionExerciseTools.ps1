$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "LongSessionExerciseSupport.ps1")
. (Join-Path $PSScriptRoot "LongSessionEvidence.ps1")

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($Expected -ne $Actual) {
        throw "$Description expected '$Expected', got '$Actual'."
    }
}

Assert-Equal "none" (Get-OLouieRecorderLogState -LogText "") "empty log state"
Assert-Equal "recording" (Get-OLouieRecorderLogState -LogText "Recording started.") "recording log state"
Assert-Equal "stopping" (Get-OLouieRecorderLogState -LogText "Stopping and saving the recording.") "stopping log state"
Assert-Equal "saved" (Get-OLouieRecorderLogState -LogText "Stopping and saving the recording.`nRecording saved: clip.mp4") "saved log state"
Assert-Equal "failed" (Get-OLouieRecorderLogState -LogText "[error] Recording failed during finalization") "failed log state"
Assert-Equal "recording" (Get-OLouieRecorderLogState -LogText "Recording saved: old.mp4`nRecording started.") "new run after prior save"
Assert-Equal "failed" (Get-OLouieRecorderLogState -LogText "Diagnostics snapshot: recorder=failed packets=12") "diagnostics failure state"
$diagnosticsMetricLine = "Diagnostics snapshot: packet_bearing_audio_tracks=1, audio_tracks=2, failed_exports=0."
Assert-Equal 2 (Get-OLouieDiagnosticsMetric -Line $diagnosticsMetricLine -Name "audio_tracks") "configured audio-track metric"
Assert-Equal 1 (Get-OLouieDiagnosticsMetric -Line $diagnosticsMetricLine -Name "packet_bearing_audio_tracks") "packet-bearing audio-track metric"
Assert-Equal 0 (Get-OLouieDiagnosticsMetric -Line $diagnosticsMetricLine -Name "failed_exports") "failed-export metric"

$flatSamples = @(
    [pscustomobject]@{ ElapsedSeconds = 0; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 60; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 120; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 180; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 }
)
$flatTrend = Get-LongSessionResourceTrend -Samples $flatSamples -WindowSize 2
Assert-Equal $true $flatTrend.Available "flat trend availability"
Assert-Equal 0 $flatTrend.WorkingSetGrowthBytes "flat working-set growth"
Assert-Equal 0 $flatTrend.PrivateBytesGrowth "flat private-byte growth"
Assert-Equal 0 $flatTrend.HandleCountGrowth "flat handle growth"

$linearSamples = @(
    [pscustomobject]@{ ElapsedSeconds = 0; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 1200; WorkingSetBytes = 200; PrivateBytes = 400; HandleCount = 20; ThreadCount = 6 },
    [pscustomobject]@{ ElapsedSeconds = 2400; WorkingSetBytes = 300; PrivateBytes = 600; HandleCount = 30; ThreadCount = 8 },
    [pscustomobject]@{ ElapsedSeconds = 3600; WorkingSetBytes = 400; PrivateBytes = 800; HandleCount = 40; ThreadCount = 10 }
)
$linearTrend = Get-LongSessionResourceTrend -Samples $linearSamples -WindowSize 2
Assert-Equal 200 $linearTrend.WorkingSetGrowthBytes "linear working-set growth"
Assert-Equal 400 $linearTrend.PrivateBytesGrowth "linear private-byte growth"
Assert-Equal 20 $linearTrend.HandleCountGrowth "linear handle growth"
Assert-Equal 200 $linearTrend.WorkingSetGrowthBytesPerHour "linear working-set hourly growth"

$outlierSamples = @(
    [pscustomobject]@{ ElapsedSeconds = 0; WorkingSetBytes = 10000; PrivateBytes = 20000; HandleCount = 1000; ThreadCount = 100 },
    [pscustomobject]@{ ElapsedSeconds = 60; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 120; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 180; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 240; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
    [pscustomobject]@{ ElapsedSeconds = 300; WorkingSetBytes = 99999; PrivateBytes = 99999; HandleCount = 9999; ThreadCount = 999 }
)
$outlierTrend = Get-LongSessionResourceTrend -Samples $outlierSamples -WindowSize 3
Assert-Equal 0 $outlierTrend.WorkingSetGrowthBytes "median-window working-set outlier resistance"
Assert-Equal 0 $outlierTrend.PrivateBytesGrowth "median-window private-byte outlier resistance"
Assert-Equal 0 $outlierTrend.HandleCountGrowth "median-window handle outlier resistance"

function Write-TestJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $json = ConvertTo-Json -InputObject $Value -Depth 10
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function New-LongSessionEvidenceFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][bool]$CompletedDuration,
        [Parameter(Mandatory = $true)][hashtable]$ProbeFacts,
        [ValidateRange(1, 2)][int]$PacketBearingAudioTracks = 2
    )

    [void](New-Item -ItemType Directory -Path $Root -Force)
    $artifactRoot = Join-Path $Root "artifacts"
    [void](New-Item -ItemType Directory -Path $artifactRoot -Force)
    $artifactPaths = @(
        (Join-Path $artifactRoot "run.mp4"),
        (Join-Path $artifactRoot "run-clip-1.mp4")
    )
    [System.IO.File]::WriteAllBytes($artifactPaths[0], [byte[]](1, 2, 3, 4))
    [System.IO.File]::WriteAllBytes($artifactPaths[1], [byte[]](5, 6, 7))
    $duration = if ($CompletedDuration) { 3665.0 } else { 37.0 }
    $artifactRows = @(for ($artifactIndex = 0; $artifactIndex -lt $artifactPaths.Count; $artifactIndex++) {
        $file = Get-Item -LiteralPath $artifactPaths[$artifactIndex]
        $audioSampleRates = @((1..$PacketBearingAudioTracks) | ForEach-Object { "48000" }) -join ";"
        $audioChannels = @((1..$PacketBearingAudioTracks) | ForEach-Object { "2" }) -join ";"
        [pscustomobject]@{
            Name = $file.Name
            Path = $file.FullName
            ArtifactKind = if ($artifactIndex -eq 0) { "full" } else { "clip" }
            Length = $file.Length
            Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            LastWriteTimeUtc = $file.LastWriteTimeUtc.ToString("o")
            BoxInspection = "passed"
            Ffprobe = "passed"
            DurationSeconds = if ($artifactIndex -eq 0) { $duration } else { 12.0 }
            FormatSizeBytes = $file.Length
            VideoStreamCount = 1
            H264StreamCount = 1
            AudioStreamCount = $PacketBearingAudioTracks
            AacStreamCount = $PacketBearingAudioTracks
            AudioSampleRates = $audioSampleRates
            AudioChannels = $audioChannels
        }
        $ProbeFacts[$file.FullName] = [pscustomobject]@{
            DurationSeconds = if ($artifactIndex -eq 0) { $duration } else { 12.0 }
            FormatSizeBytes = $file.Length
            VideoStreamCount = 1
            H264StreamCount = 1
            AudioStreamCount = $PacketBearingAudioTracks
            AacStreamCount = $PacketBearingAudioTracks
            AudioSampleRates = $audioSampleRates
            AudioChannels = $audioChannels
        }
    })
    $artifactRows | Export-Csv -LiteralPath (Join-Path $Root "artifacts.csv") -NoTypeInformation
    $inspectionText = @(
        "=== $($artifactPaths[0]) ===",
        "MP4 artifact inspection succeeded.",
        "=== $($artifactPaths[1]) ===",
        "MP4 artifact inspection succeeded."
    ) -join "`n"
    [System.IO.File]::WriteAllText(
        (Join-Path $Root "mp4-inspection.log"),
        $inspectionText,
        [System.Text.UTF8Encoding]::new($false))

    $middle = $duration / 2.0
    $resourceRows = @(
        [pscustomobject]@{ ElapsedSeconds = 0; Responding = $true; WorkingSetBytes = 100; PrivateBytes = 200; HandleCount = 10; ThreadCount = 4 },
        [pscustomobject]@{ ElapsedSeconds = $middle; Responding = $true; WorkingSetBytes = 110; PrivateBytes = 210; HandleCount = 11; ThreadCount = 4 },
        [pscustomobject]@{ ElapsedSeconds = $duration; Responding = $true; WorkingSetBytes = 120; PrivateBytes = 220; HandleCount = 12; ThreadCount = 5 }
    )
    $resourceRows | Export-Csv -LiteralPath (Join-Path $Root "resource-samples.csv") -NoTypeInformation
    $resourceTrend = Get-LongSessionResourceTrend -Samples $resourceRows -WindowSize 5

    $commandRows = [System.Collections.Generic.List[object]]::new()
    $commandRows.Add([pscustomobject]@{ TimestampUtc = [DateTime]::UtcNow.ToString("o"); ElapsedSeconds = 0; Command = "toggle-recording"; ExitCode = 0; Output = "started" })
    $commandRows.Add([pscustomobject]@{ TimestampUtc = [DateTime]::UtcNow.ToString("o"); ElapsedSeconds = 12; Command = "first-clip"; ExitCode = 0; Output = "queued" })
    if ($CompletedDuration) {
        $commandRows.Add([pscustomobject]@{ TimestampUtc = [DateTime]::UtcNow.ToString("o"); ElapsedSeconds = $duration; Command = "toggle-recording"; ExitCode = 0; Output = "stopped" })
    }
    $commandRows.Add([pscustomobject]@{ TimestampUtc = [DateTime]::UtcNow.ToString("o"); ElapsedSeconds = $duration; Command = "exit"; ExitCode = 0; Output = "exited" })
    $commandRows | Export-Csv -LiteralPath (Join-Path $Root "runtime-commands.csv") -NoTypeInformation

    $finalDiagnostics = "Diagnostics snapshot: recorder=saved, audio_tracks=2, packet_bearing_audio_tracks=$PacketBearingAudioTracks, submitted_exports=1, saved_exports=1, failed_exports=0."
    [System.IO.File]::WriteAllLines(
        (Join-Path $Root "diagnostics-snapshots.log"),
        [string[]]@("Diagnostics snapshot: recorder=recording", $finalDiagnostics),
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $Root "isolated-recovery-test.log"), "passed", [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $Root "settings-before.json"), "{}", [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $Root "settings-after.json"), "{}", [System.Text.UTF8Encoding]::new($false))
    Write-TestJson -Path (Join-Path $Root "settings-during.json") -Value ([ordered]@{
        output_directory = [System.IO.Path]::GetFullPath($artifactRoot)
        video = [ordered]@{ encoder_backend = "media_foundation_hardware" }
        audio = [ordered]@{
            system_mix = $true
            mic = $true
            separate_tracks = $true
            sample_rate = 48000
        }
    })
    [System.IO.File]::WriteAllText((Join-Path $Root "sessions-before.json"), "[]", [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $Root "sessions-after.json"), "[]", [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $Root "preexisting-session-diff.json"), "[]", [System.Text.UTF8Encoding]::new($false))

    $settingsHash = (Get-FileHash -LiteralPath (Join-Path $Root "settings-before.json") -Algorithm SHA256).Hash
    Write-TestJson -Path (Join-Path $Root "cleanup.json") -Value ([ordered]@{
        EvidenceSchemaVersion = 3
        Success = $true
        RunSucceededBeforeCleanup = $true
        SettingsRestored = $true
        SettingsBeforeSha256 = $settingsHash
        SettingsAfterSha256 = $settingsHash
        AppProcessExited = $true
        StimulusProcessExited = $true
        OLouieProcessCount = 0
        Errors = @()
    })

    Write-TestJson -Path (Join-Path $Root "summary.json") -Value ([ordered]@{
        EvidenceSchemaVersion = 3
        Success = $true
        RequestedDurationSeconds = if ($CompletedDuration) { 3665 } else { 120 }
        RequestedDurationCompleted = $CompletedDuration
        StopReason = if ($CompletedDuration) { "duration" } else { "external-stop" }
        ObservedRecordingSeconds = $duration
        ResourceSampleCount = $resourceRows.Count
        ResourceTrend = $resourceTrend
        AllSamplesResponding = $true
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
        RuntimeCommandFailures = 0
        DiagnosticsSnapshotCount = 2
        FinalDiagnostics = $finalDiagnostics
        SystemAudioRequested = $true
        MicrophoneRequested = $true
        SeparateAudioTracksRequested = $true
        ConfiguredAudioSampleRate = 48000
        ConfiguredVideoEncoderBackend = "media_foundation_hardware"
        ConfiguredAudioTrackCount = 2
        PacketBearingAudioTrackCount = $PacketBearingAudioTracks
        SubmittedExportCount = 1
        SavedExportCount = 1
        FailedExportCount = 0
        Mp4ArtifactCount = $artifactRows.Count
        FullArtifactCount = 1
        PartialArtifactCount = 0
        FfprobeAvailable = $true
        PreexistingSessionDifferenceCount = 0
        IsolatedRecoveryFixtureExitCode = 0
        EvidenceDirectory = [System.IO.Path]::GetFullPath($Root)
    })
}

$tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $tempBase ("olouie_long_session_evidence_" + [guid]::NewGuid().ToString("N"))
if (-not $fixtureRoot.StartsWith($tempBase, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to create long-session fixtures outside the system temp directory."
}
try {
    $fixtureProbeFacts = @{}
    $artifactReprobe = {
        param([Parameter(Mandatory = $true)][string]$ArtifactPath)

        $resolvedPath = [System.IO.Path]::GetFullPath($ArtifactPath)
        if (-not $fixtureProbeFacts.ContainsKey($resolvedPath)) {
            throw "No synthetic fresh-probe facts exist for $resolvedPath."
        }
        return $fixtureProbeFacts[$resolvedPath]
    }.GetNewClosure()
    $completeRoot = Join-Path $fixtureRoot "complete"
    $preflightRoot = Join-Path $fixtureRoot "preflight"
    $missingAudioRoot = Join-Path $fixtureRoot "missing-audio"
    $staleProbeRoot = Join-Path $fixtureRoot "stale-probe"
    $staleTrendRoot = Join-Path $fixtureRoot "stale-trend"
    $invalidResourceRoot = Join-Path $fixtureRoot "invalid-resource"
    New-LongSessionEvidenceFixture -Root $completeRoot -CompletedDuration $true -ProbeFacts $fixtureProbeFacts
    New-LongSessionEvidenceFixture -Root $preflightRoot -CompletedDuration $false -ProbeFacts $fixtureProbeFacts
    New-LongSessionEvidenceFixture -Root $missingAudioRoot -CompletedDuration $true -ProbeFacts $fixtureProbeFacts -PacketBearingAudioTracks 1
    New-LongSessionEvidenceFixture -Root $staleProbeRoot -CompletedDuration $true -ProbeFacts $fixtureProbeFacts
    New-LongSessionEvidenceFixture -Root $staleTrendRoot -CompletedDuration $true -ProbeFacts $fixtureProbeFacts
    New-LongSessionEvidenceFixture -Root $invalidResourceRoot -CompletedDuration $true -ProbeFacts $fixtureProbeFacts

    $completeEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $completeRoot -ArtifactReprobe $artifactReprobe
    if (-not $completeEvidence.EvidenceValid) {
        throw "Complete evidence fixture was rejected: $($completeEvidence.Errors -join '; ')"
    }
    Assert-Equal "Verified" $completeEvidence.Status "complete evidence status"
    Assert-Equal $true $completeEvidence.EvidenceValid "complete evidence validity"
    Assert-Equal $true $completeEvidence.DurationRequirementSatisfied "complete duration requirement"
    Assert-Equal 2 $completeEvidence.ReinspectedArtifactCount "complete fresh artifact reinspection count"

    $preflightEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $preflightRoot -ArtifactReprobe $artifactReprobe
    Assert-Equal "Preflight" $preflightEvidence.Status "preflight evidence status"
    Assert-Equal $true $preflightEvidence.EvidenceValid "preflight evidence validity"
    Assert-Equal $false $preflightEvidence.DurationRequirementSatisfied "preflight duration requirement"

    $missingAudioEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $missingAudioRoot -ArtifactReprobe $artifactReprobe
    Assert-Equal "Preflight" $missingAudioEvidence.Status "missing-audio evidence status"
    Assert-Equal $true $missingAudioEvidence.EvidenceValid "missing-audio evidence validity"
    Assert-Equal $false $missingAudioEvidence.DurationRequirementSatisfied "missing-audio duration requirement"

    $staleProbeRows = @(Import-Csv -LiteralPath (Join-Path $staleProbeRoot "artifacts.csv"))
    ($staleProbeRows | Where-Object ArtifactKind -eq "full").AacStreamCount = "1"
    $staleProbeRows | Export-Csv -LiteralPath (Join-Path $staleProbeRoot "artifacts.csv") -NoTypeInformation
    $staleProbeEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $staleProbeRoot -ArtifactReprobe $artifactReprobe
    Assert-Equal "Invalid" $staleProbeEvidence.Status "stale-probe evidence status"
    Assert-Equal $false $staleProbeEvidence.EvidenceValid "stale-probe evidence validity"

    $staleTrendSummaryPath = Join-Path $staleTrendRoot "summary.json"
    $staleTrendSummary = Get-Content -LiteralPath $staleTrendSummaryPath -Raw | ConvertFrom-Json
    $staleTrendSummary.ResourceTrend.WorkingSetGrowthBytes = [int64]$staleTrendSummary.ResourceTrend.WorkingSetGrowthBytes + 1
    Write-TestJson -Path $staleTrendSummaryPath -Value $staleTrendSummary
    $staleTrendEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $staleTrendRoot -ArtifactReprobe $artifactReprobe
    Assert-Equal "Invalid" $staleTrendEvidence.Status "stale-trend evidence status"
    Assert-Equal $false $staleTrendEvidence.EvidenceValid "stale-trend evidence validity"

    $invalidResourcePath = Join-Path $invalidResourceRoot "resource-samples.csv"
    $invalidResourceRows = @(Import-Csv -LiteralPath $invalidResourcePath)
    $invalidResourceRows[0].HandleCount = "-1"
    $invalidResourceRows | Export-Csv -LiteralPath $invalidResourcePath -NoTypeInformation
    $invalidResourceEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $invalidResourceRoot -ArtifactReprobe $artifactReprobe
    Assert-Equal "Invalid" $invalidResourceEvidence.Status "invalid-resource evidence status"
    Assert-Equal $false $invalidResourceEvidence.EvidenceValid "invalid-resource evidence validity"

    $tamperedPath = Join-Path $completeRoot "artifacts\run.mp4"
    $tamperedBytes = [System.IO.File]::ReadAllBytes($tamperedPath)
    $tamperedBytes[0] = $tamperedBytes[0] -bxor 0xff
    [System.IO.File]::WriteAllBytes($tamperedPath, $tamperedBytes)
    $tamperedEvidence = Get-LongSessionEvidenceStatus -EvidenceDirectory $completeRoot -ArtifactReprobe $artifactReprobe
    Assert-Equal "Invalid" $tamperedEvidence.Status "tampered evidence status"
    Assert-Equal $false $tamperedEvidence.EvidenceValid "tampered evidence validity"
} finally {
    if ((Test-Path -LiteralPath $fixtureRoot -PathType Container) -and
        $fixtureRoot.StartsWith($tempBase, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

Write-Host "Long-session exercise tool tests passed."
