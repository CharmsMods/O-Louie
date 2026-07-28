. (Join-Path $PSScriptRoot "LongSessionExerciseSupport.ps1")

function Get-LongSessionEvidenceStatus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
        [string]$FfprobePath,
        [scriptblock]$ArtifactReprobe
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    $warnings = [System.Collections.Generic.List[string]]::new()
    $durationIssues = [System.Collections.Generic.List[string]]::new()
    $root = [System.IO.Path]::GetFullPath($EvidenceDirectory)

    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        $errors.Add("Evidence directory does not exist: $root")
        return [pscustomobject]@{
            EvidenceDirectory = $root
            Status = "Invalid"
            EvidenceValid = $false
            DurationRequirementSatisfied = $false
            ResourceSpanSeconds = 0.0
            ArtifactCount = 0
            ReinspectedArtifactCount = 0
            ResourceTrend = $null
            Errors = @($errors)
            Warnings = @($warnings)
            DurationIssues = @($durationIssues)
            Summary = $null
        }
    }

    $requiredFiles = @(
        "summary.json",
        "resource-samples.csv",
        "runtime-commands.csv",
        "artifacts.csv",
        "diagnostics-snapshots.log",
        "mp4-inspection.log",
        "isolated-recovery-test.log",
        "sessions-before.json",
        "sessions-after.json",
        "settings-before.json",
        "settings-during.json"
    )
    foreach ($name in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) {
            $errors.Add("Required evidence file is missing: $name")
        }
    }

    $summary = $null
    $summaryPath = Join-Path $root "summary.json"
    if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
        try {
            $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        } catch {
            $errors.Add("summary.json is not valid JSON: $($_.Exception.Message)")
        }
    }

    $requiredSummaryProperties = @(
        "Success",
        "RequestedDurationSeconds",
        "RequestedDurationCompleted",
        "StopReason",
        "ObservedRecordingSeconds",
        "ResourceSampleCount",
        "ResourceTrend",
        "AllSamplesResponding",
        "RuntimeCommandCount",
        "RuntimeCommandFailures",
        "DiagnosticsSnapshotCount",
        "Mp4ArtifactCount",
        "PartialArtifactCount",
        "FfprobeAvailable",
        "PreexistingSessionDifferenceCount",
        "IsolatedRecoveryFixtureExitCode",
        "EvidenceDirectory"
    )
    if ($null -ne $summary) {
        foreach ($name in $requiredSummaryProperties) {
            if ($null -eq $summary.PSObject.Properties[$name]) {
                $errors.Add("summary.json is missing required property '$name'.")
            }
        }
    }

    if ($null -eq $summary -or $errors.Count -ne 0) {
        return [pscustomobject]@{
            EvidenceDirectory = $root
            Status = "Invalid"
            EvidenceValid = $false
            DurationRequirementSatisfied = $false
            ResourceSpanSeconds = 0.0
            ArtifactCount = 0
            ReinspectedArtifactCount = 0
            ResourceTrend = $null
            Errors = @($errors)
            Warnings = @($warnings)
            DurationIssues = @($durationIssues)
            Summary = $summary
        }
    }

    $schemaVersion = 0
    if ($null -ne $summary.PSObject.Properties["EvidenceSchemaVersion"]) {
        $schemaVersion = [int]$summary.EvidenceSchemaVersion
    } else {
        $warnings.Add("summary.json predates evidence schema versioning.")
    }

    $artifactReprobeCount = 0
    $artifactReprobeAvailable = $null -ne $ArtifactReprobe
    if ($schemaVersion -ge 3 -and -not $artifactReprobeAvailable) {
        try {
            $resolvedFfprobe = Find-OLouieFfprobe -PreferredPath $FfprobePath
            if ($null -eq $resolvedFfprobe) {
                $warnings.Add("ffprobe.exe is unavailable for schema-3 artifact reinspection.")
            } else {
                $inspectorPath = Join-Path $PSScriptRoot "InspectMp4Artifact.ps1"
                if (-not (Test-Path -LiteralPath $inspectorPath -PathType Leaf)) {
                    $errors.Add("MP4 artifact inspector is missing: $inspectorPath")
                } else {
                    $ArtifactReprobe = {
                        param([Parameter(Mandatory = $true)][string]$ArtifactPath)

                        return Get-OLouieMp4ReinspectionFacts `
                            -Path $ArtifactPath `
                            -InspectorPath $inspectorPath `
                            -FfprobePath $resolvedFfprobe
                    }.GetNewClosure()
                    $artifactReprobeAvailable = $true
                }
            }
        } catch {
            $errors.Add("Schema-3 artifact reinspection setup failed: $($_.Exception.Message)")
        }
    }

    $settingsDuring = $null
    $systemAudioRequested = $false
    $microphoneRequested = $false
    $separateAudioTracksRequested = $false
    $configuredAudioSampleRate = 0
    $configuredVideoEncoderBackend = ""
    $settingsDuringPath = Join-Path $root "settings-during.json"
    try {
        $settingsDuring = Get-Content -LiteralPath $settingsDuringPath -Raw | ConvertFrom-Json
        if ($null -eq $settingsDuring.PSObject.Properties["output_directory"] -or
            $null -eq $settingsDuring.PSObject.Properties["audio"] -or
            $null -eq $settingsDuring.PSObject.Properties["video"] -or
            $null -eq $settingsDuring.audio.PSObject.Properties["system_mix"] -or
            $null -eq $settingsDuring.audio.PSObject.Properties["mic"] -or
            $null -eq $settingsDuring.audio.PSObject.Properties["separate_tracks"] -or
            $null -eq $settingsDuring.audio.PSObject.Properties["sample_rate"] -or
            $null -eq $settingsDuring.video.PSObject.Properties["encoder_backend"]) {
            $errors.Add("settings-during.json is missing required output/audio/video fields.")
        } else {
            $configuredOutput = [System.IO.Path]::GetFullPath([string]$settingsDuring.output_directory)
            $expectedOutput = [System.IO.Path]::GetFullPath((Join-Path $root "artifacts"))
            if (-not [string]::Equals($configuredOutput, $expectedOutput, [System.StringComparison]::OrdinalIgnoreCase)) {
                $errors.Add("settings-during.json output directory does not match the evidence artifact directory.")
            }
            $systemAudioRequested = $settingsDuring.audio.system_mix -eq $true
            $microphoneRequested = $settingsDuring.audio.mic -eq $true
            $separateAudioTracksRequested = $settingsDuring.audio.separate_tracks -eq $true
            $configuredAudioSampleRate = [int]$settingsDuring.audio.sample_rate
            $configuredVideoEncoderBackend = [string]$settingsDuring.video.encoder_backend
        }
    } catch {
        $errors.Add("settings-during.json is invalid: $($_.Exception.Message)")
    }

    $schema2SummaryProperties = @(
        "FinalDiagnostics",
        "SystemAudioRequested",
        "MicrophoneRequested",
        "SeparateAudioTracksRequested",
        "ConfiguredAudioSampleRate",
        "ConfiguredVideoEncoderBackend",
        "ConfiguredAudioTrackCount",
        "PacketBearingAudioTrackCount",
        "SubmittedExportCount",
        "SavedExportCount",
        "FailedExportCount",
        "FullArtifactCount"
    )
    $schema2SummaryMissing = @()
    if ($schemaVersion -ge 2) {
        $schema2SummaryMissing = @($schema2SummaryProperties | Where-Object {
            $null -eq $summary.PSObject.Properties[$_]
        })
        foreach ($name in $schema2SummaryMissing) {
            $errors.Add("Schema-2 summary.json is missing required property '$name'.")
        }
    }
    $schema3ResourceSummaryProperties = @(
        "WorkingSetFirstBytes",
        "WorkingSetLastBytes",
        "WorkingSetMaximumBytes",
        "PrivateBytesFirst",
        "PrivateBytesLast",
        "PrivateBytesMaximum",
        "HandleCountFirst",
        "HandleCountLast",
        "HandleCountMaximum"
    )
    $schema3ResourceSummaryMissing = @()
    if ($schemaVersion -ge 3) {
        $schema3ResourceSummaryMissing = @($schema3ResourceSummaryProperties | Where-Object {
            $null -eq $summary.PSObject.Properties[$_]
        })
        foreach ($name in $schema3ResourceSummaryMissing) {
            $errors.Add("Schema-3 summary.json is missing resource property '$name'.")
        }
    }

    try {
        $reportedRoot = [System.IO.Path]::GetFullPath([string]$summary.EvidenceDirectory)
        if (-not [string]::Equals($root, $reportedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            $errors.Add("summary.json names a different evidence directory: $reportedRoot")
        }
    } catch {
        $errors.Add("summary.json contains an invalid EvidenceDirectory path.")
    }
    if ($summary.Success -ne $true) {
        $errors.Add("The exercise summary does not report success.")
    }
    if ([int]$summary.RuntimeCommandFailures -ne 0) {
        $errors.Add("The exercise summary reports runtime command failures.")
    }
    if ([int]$summary.PartialArtifactCount -ne 0) {
        $errors.Add("The exercise summary reports partial artifacts.")
    }
    if ([int]$summary.PreexistingSessionDifferenceCount -ne 0) {
        $errors.Add("The exercise summary reports changes to pre-existing sessions.")
    }
    if ([int]$summary.IsolatedRecoveryFixtureExitCode -ne 0) {
        $errors.Add("The isolated recovery fixture did not exit successfully.")
    }
    if ($schemaVersion -ge 2 -and $schema2SummaryMissing.Count -eq 0) {
        if ($summary.SystemAudioRequested -ne $systemAudioRequested -or
            $summary.MicrophoneRequested -ne $microphoneRequested -or
            $summary.SeparateAudioTracksRequested -ne $separateAudioTracksRequested -or
            [int]$summary.ConfiguredAudioSampleRate -ne $configuredAudioSampleRate -or
            [string]$summary.ConfiguredVideoEncoderBackend -ne $configuredVideoEncoderBackend) {
            $errors.Add("Schema-2 summary configuration does not match settings-during.json.")
        }
    }

    $artifactRows = @()
    $artifactCsvPath = Join-Path $root "artifacts.csv"
    if (Test-Path -LiteralPath $artifactCsvPath -PathType Leaf) {
        $artifactRows = @(Import-Csv -LiteralPath $artifactCsvPath)
    }
    if ($artifactRows.Count -ne [int]$summary.Mp4ArtifactCount) {
        $errors.Add("Artifact CSV count $($artifactRows.Count) does not match summary count $($summary.Mp4ArtifactCount).")
    }
    if ($artifactRows.Count -lt 2) {
        $errors.Add("The exercise did not retain a full recording plus at least one background export.")
    }
    $artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $root "artifacts"))
    $validatedArtifactPaths = [System.Collections.Generic.List[string]]::new()
    $fullArtifactRows = [System.Collections.Generic.List[object]]::new()
    $allArtifactsHaveExpectedVideo = $true
    $allArtifactsHaveAacAudio = $true
    $fullArtifactHasDualAac = $false
    $fullArtifactDurationSeconds = 0.0
    $fullArtifactAudioFormatsValid = $false
    $schema2ArtifactProperties = @(
        "ArtifactKind",
        "DurationSeconds",
        "FormatSizeBytes",
        "VideoStreamCount",
        "H264StreamCount",
        "AudioStreamCount",
        "AacStreamCount",
        "AudioSampleRates",
        "AudioChannels"
    )
    $schema3ArtifactProperties = @("Sha256")
    foreach ($row in $artifactRows) {
        try {
            $artifactPath = [System.IO.Path]::GetFullPath([string]$row.Path)
            $artifactParent = [System.IO.Path]::GetDirectoryName($artifactPath)
            if (-not [string]::Equals($artifactRoot, $artifactParent, [System.StringComparison]::OrdinalIgnoreCase)) {
                $errors.Add("Artifact is outside the exercise artifact directory: $artifactPath")
                continue
            }
            if (-not [string]::Equals([System.IO.Path]::GetFileName($artifactPath), [string]$row.Name, [System.StringComparison]::OrdinalIgnoreCase)) {
                $errors.Add("Artifact name/path mismatch: $($row.Name)")
            }
            if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
                $errors.Add("Artifact file is missing: $artifactPath")
                continue
            }
            $file = Get-Item -LiteralPath $artifactPath
            if ($file.Length -ne [int64]$row.Length) {
                $errors.Add("Artifact size changed for $($row.Name): expected $($row.Length), found $($file.Length).")
            }
            if ($row.BoxInspection -ne "passed") {
                $errors.Add("Artifact box inspection did not pass for $($row.Name).")
            }
            if ($summary.FfprobeAvailable -eq $true -and $row.Ffprobe -ne "passed") {
                $errors.Add("ffprobe inspection did not pass for $($row.Name).")
            }
            if ($schemaVersion -ge 2 -and $summary.FfprobeAvailable -eq $true) {
                $missingArtifactProperties = @($schema2ArtifactProperties | Where-Object {
                    $null -eq $row.PSObject.Properties[$_]
                })
                if ($missingArtifactProperties.Count -ne 0) {
                    $errors.Add("Schema-2 artifact row '$($row.Name)' is missing: $($missingArtifactProperties -join ', ').")
                    $allArtifactsHaveExpectedVideo = $false
                    $allArtifactsHaveAacAudio = $false
                } else {
                    $expectedKind = if ($file.BaseName -match "-clip-\d+$") {
                        "clip"
                    } elseif ($file.BaseName -match "-bookmark-\d+$") {
                        "bookmark"
                    } else {
                        "full"
                    }
                    if ([string]$row.ArtifactKind -ne $expectedKind) {
                        $errors.Add("Artifact kind does not match the file name for $($row.Name).")
                    }
                    if ([int64]$row.FormatSizeBytes -ne $file.Length) {
                        $errors.Add("ffprobe format size does not match the file size for $($row.Name).")
                    }
                    $videoCount = [int]$row.VideoStreamCount
                    $h264Count = [int]$row.H264StreamCount
                    $audioCount = [int]$row.AudioStreamCount
                    $aacCount = [int]$row.AacStreamCount
                    if ($videoCount -ne 1 -or $h264Count -ne 1) {
                        $allArtifactsHaveExpectedVideo = $false
                    }
                    if ($audioCount -lt 1 -or $aacCount -ne $audioCount) {
                        $allArtifactsHaveAacAudio = $false
                    }
                    $rates = @(([string]$row.AudioSampleRates -split ";") | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
                    $channels = @(([string]$row.AudioChannels -split ";") | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
                    if ($rates.Count -ne $audioCount -or $channels.Count -ne $audioCount) {
                        $errors.Add("Audio stream metadata count does not match ffprobe stream count for $($row.Name).")
                    }
                    if ($expectedKind -eq "full") {
                        $fullArtifactRows.Add($row)
                        $fullArtifactDurationSeconds = [double]$row.DurationSeconds
                        $fullArtifactHasDualAac = $audioCount -ge 2 -and $aacCount -eq $audioCount
                        $fullArtifactAudioFormatsValid = @($rates | Where-Object { [int]$_ -ne 48000 }).Count -eq 0 -and
                            @($channels | Where-Object { [int]$_ -ne 2 }).Count -eq 0
                    }
                }
            }
            if ($schemaVersion -ge 3) {
                $missingArtifactProperties = @($schema3ArtifactProperties | Where-Object {
                    $null -eq $row.PSObject.Properties[$_]
                })
                if ($missingArtifactProperties.Count -ne 0) {
                    $errors.Add("Schema-3 artifact row '$($row.Name)' is missing: $($missingArtifactProperties -join ', ').")
                } elseif ([string]$row.Sha256 -notmatch "^[A-Fa-f0-9]{64}$") {
                    $errors.Add("Artifact SHA-256 is malformed for $($row.Name).")
                } else {
                    $currentHash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash
                    if (-not [string]::Equals($currentHash, [string]$row.Sha256, [System.StringComparison]::OrdinalIgnoreCase)) {
                        $errors.Add("Artifact SHA-256 changed for $($row.Name).")
                    }
                }

                if ($artifactReprobeAvailable) {
                    try {
                        $freshFacts = & $ArtifactReprobe $artifactPath
                        $requiredFreshProperties = @(
                            "DurationSeconds",
                            "FormatSizeBytes",
                            "VideoStreamCount",
                            "H264StreamCount",
                            "AudioStreamCount",
                            "AacStreamCount",
                            "AudioSampleRates",
                            "AudioChannels"
                        )
                        $missingFreshProperties = @($requiredFreshProperties | Where-Object {
                            $null -eq $freshFacts -or $null -eq $freshFacts.PSObject.Properties[$_]
                        })
                        if ($missingFreshProperties.Count -ne 0) {
                            throw "fresh probe facts are missing: $($missingFreshProperties -join ', ')"
                        }
                        if ([math]::Abs([double]$freshFacts.DurationSeconds - [double]$row.DurationSeconds) -gt 0.001 -or
                            [int64]$freshFacts.FormatSizeBytes -ne [int64]$row.FormatSizeBytes -or
                            [int]$freshFacts.VideoStreamCount -ne [int]$row.VideoStreamCount -or
                            [int]$freshFacts.H264StreamCount -ne [int]$row.H264StreamCount -or
                            [int]$freshFacts.AudioStreamCount -ne [int]$row.AudioStreamCount -or
                            [int]$freshFacts.AacStreamCount -ne [int]$row.AacStreamCount -or
                            [string]$freshFacts.AudioSampleRates -ne [string]$row.AudioSampleRates -or
                            [string]$freshFacts.AudioChannels -ne [string]$row.AudioChannels) {
                            $errors.Add("Fresh structural/ffprobe facts do not match artifacts.csv for $($row.Name).")
                        }
                        $artifactReprobeCount++
                    } catch {
                        $errors.Add("Artifact reinspection failed for $($row.Name): $($_.Exception.Message)")
                    }
                }
            }
            $validatedArtifactPaths.Add($artifactPath)
        } catch {
            $errors.Add("Artifact row '$($row.Name)' is invalid: $($_.Exception.Message)")
        }
    }
    if ($schemaVersion -ge 2 -and $summary.FfprobeAvailable -eq $true) {
        if ($fullArtifactRows.Count -ne 1) {
            $errors.Add("Schema-2 evidence must identify exactly one full recording artifact.")
        }
        if ($schema2SummaryMissing.Count -eq 0 -and [int]$summary.FullArtifactCount -ne $fullArtifactRows.Count) {
            $errors.Add("Full artifact count does not match the schema-2 summary.")
        }
    }
    if (Test-Path -LiteralPath $artifactRoot -PathType Container) {
        $actualPartials = @(Get-ChildItem -LiteralPath $artifactRoot -File -Recurse | Where-Object {
            $_.Name -like "*.partial*" -or $_.Name -like "*.tmp" -or $_.Name -like "*.temp"
        })
        if ($actualPartials.Count -ne 0) {
            $errors.Add("Partial files are present in the artifact directory.")
        }
    }
    $inspectionPath = Join-Path $root "mp4-inspection.log"
    if (Test-Path -LiteralPath $inspectionPath -PathType Leaf) {
        $inspectionText = Get-Content -LiteralPath $inspectionPath -Raw
        $inspectionPasses = [regex]::Matches($inspectionText, "MP4 artifact inspection succeeded\.").Count
        if ($inspectionPasses -ne $artifactRows.Count) {
            $errors.Add("MP4 inspection log pass count $inspectionPasses does not match artifact count $($artifactRows.Count).")
        }
        foreach ($artifactPath in $validatedArtifactPaths) {
            if ($inspectionText.IndexOf($artifactPath, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
                $errors.Add("MP4 inspection log does not name artifact: $artifactPath")
            }
        }
    }

    $resourceRows = @()
    $resourcePath = Join-Path $root "resource-samples.csv"
    if (Test-Path -LiteralPath $resourcePath -PathType Leaf) {
        $resourceRows = @(Import-Csv -LiteralPath $resourcePath)
    }
    if ($resourceRows.Count -ne [int]$summary.ResourceSampleCount) {
        $errors.Add("Resource sample count $($resourceRows.Count) does not match summary count $($summary.ResourceSampleCount).")
    }
    if ($resourceRows.Count -lt 2) {
        $errors.Add("At least two resource samples are required.")
    }
    $resourceSpanSeconds = 0.0
    $resourceTrend = $null
    $resourceRowsValid = $true
    $lastElapsed = -1.0
    $requiredResourceProperties = @(
        "ElapsedSeconds",
        "Responding",
        "WorkingSetBytes",
        "PrivateBytes",
        "HandleCount",
        "ThreadCount"
    )
    foreach ($row in $resourceRows) {
        try {
            $missingResourceProperties = @($requiredResourceProperties | Where-Object {
                $null -eq $row.PSObject.Properties[$_] -or
                    [string]::IsNullOrWhiteSpace([string]$row.PSObject.Properties[$_].Value)
            })
            if ($missingResourceProperties.Count -ne 0) {
                throw "missing values: $($missingResourceProperties -join ', ')"
            }
            $elapsed = [double]$row.ElapsedSeconds
            $workingSet = [int64]$row.WorkingSetBytes
            $privateBytes = [int64]$row.PrivateBytes
            $handleCount = [int64]$row.HandleCount
            $threadCount = [int64]$row.ThreadCount
            if ([double]::IsNaN($elapsed) -or [double]::IsInfinity($elapsed) -or
                $elapsed -lt 0.0 -or $workingSet -lt 0 -or $privateBytes -lt 0 -or
                $handleCount -lt 0 -or $threadCount -lt 0) {
                throw "negative or non-finite metric"
            }
            if ($elapsed -lt $lastElapsed) {
                $errors.Add("Resource sample elapsed times are not monotonic.")
                $resourceRowsValid = $false
                break
            }
            $lastElapsed = $elapsed
            if ([string]$row.Responding -ne "True") {
                $errors.Add("A resource sample reports that O'Louie was not responding.")
                $resourceRowsValid = $false
                break
            }
        } catch {
            $errors.Add("A resource sample contains invalid data: $($_.Exception.Message)")
            $resourceRowsValid = $false
            break
        }
    }
    if ($resourceRows.Count -ge 2 -and $resourceRowsValid) {
        $resourceSpanSeconds = [double]$resourceRows[-1].ElapsedSeconds - [double]$resourceRows[0].ElapsedSeconds
        $resourceTrend = Get-LongSessionResourceTrend -Samples $resourceRows -WindowSize 5
    }
    if ($summary.AllSamplesResponding -ne $true) {
        $errors.Add("The exercise summary does not report all samples responding.")
    }
    if ($null -eq $resourceTrend -or $resourceTrend.Available -ne $true -or $resourceTrend.SampleCount -ne $resourceRows.Count) {
        $errors.Add("The resource trend is unavailable or does not cover every resource sample.")
    }
    if ($schemaVersion -ge 3 -and $schema3ResourceSummaryMissing.Count -eq 0 -and $null -ne $resourceTrend) {
        $workingSetValues = [int64[]]@($resourceRows | ForEach-Object { [int64]$_.WorkingSetBytes })
        $privateValues = [int64[]]@($resourceRows | ForEach-Object { [int64]$_.PrivateBytes })
        $handleValues = [int64[]]@($resourceRows | ForEach-Object { [int64]$_.HandleCount })
        $expectedResourceSummary = [ordered]@{
            WorkingSetFirstBytes = $workingSetValues[0]
            WorkingSetLastBytes = $workingSetValues[-1]
            WorkingSetMaximumBytes = ($workingSetValues | Measure-Object -Maximum).Maximum
            PrivateBytesFirst = $privateValues[0]
            PrivateBytesLast = $privateValues[-1]
            PrivateBytesMaximum = ($privateValues | Measure-Object -Maximum).Maximum
            HandleCountFirst = $handleValues[0]
            HandleCountLast = $handleValues[-1]
            HandleCountMaximum = ($handleValues | Measure-Object -Maximum).Maximum
        }
        foreach ($entry in $expectedResourceSummary.GetEnumerator()) {
            if ([int64]$summary.PSObject.Properties[$entry.Key].Value -ne [int64]$entry.Value) {
                $errors.Add("Summary resource metric '$($entry.Key)' does not match resource-samples.csv.")
            }
        }

        $trendProperties = @(
            "Available",
            "SampleCount",
            "WindowSize",
            "ElapsedHours",
            "WorkingSetHeadMedianBytes",
            "WorkingSetTailMedianBytes",
            "WorkingSetGrowthBytes",
            "WorkingSetGrowthBytesPerHour",
            "PrivateBytesHeadMedian",
            "PrivateBytesTailMedian",
            "PrivateBytesGrowth",
            "PrivateBytesGrowthPerHour",
            "HandleCountHeadMedian",
            "HandleCountTailMedian",
            "HandleCountGrowth",
            "HandleCountGrowthPerHour",
            "ThreadCountHeadMedian",
            "ThreadCountTailMedian",
            "ThreadCountGrowth",
            "ThreadCountGrowthPerHour"
        )
        foreach ($name in $trendProperties) {
            $reportedProperty = $summary.ResourceTrend.PSObject.Properties[$name]
            $currentProperty = $resourceTrend.PSObject.Properties[$name]
            if ($null -eq $reportedProperty -or $null -eq $currentProperty) {
                $errors.Add("Resource trend property '$name' is missing.")
            } elseif ($name -eq "Available") {
                if ([bool]$reportedProperty.Value -ne [bool]$currentProperty.Value) {
                    $errors.Add("Resource trend property '$name' does not match resource-samples.csv.")
                }
            } elseif ($name -in @("SampleCount", "WindowSize")) {
                if ([int]$reportedProperty.Value -ne [int]$currentProperty.Value) {
                    $errors.Add("Resource trend property '$name' does not match resource-samples.csv.")
                }
            } elseif ([math]::Abs([double]$reportedProperty.Value - [double]$currentProperty.Value) -gt 0.0005) {
                $errors.Add("Resource trend property '$name' does not match resource-samples.csv.")
            }
        }
    }

    $commandRows = @()
    $commandPath = Join-Path $root "runtime-commands.csv"
    if (Test-Path -LiteralPath $commandPath -PathType Leaf) {
        $commandRows = @(Import-Csv -LiteralPath $commandPath)
    }
    if ($commandRows.Count -ne [int]$summary.RuntimeCommandCount) {
        $errors.Add("Runtime command count $($commandRows.Count) does not match summary count $($summary.RuntimeCommandCount).")
    }
    $failedCommands = @($commandRows | Where-Object { [int]$_.ExitCode -ne 0 })
    if ($failedCommands.Count -ne [int]$summary.RuntimeCommandFailures) {
        $errors.Add("Runtime command failure count does not match the summary.")
    }
    if (@($commandRows | Where-Object Command -eq "toggle-recording").Count -lt 1) {
        $errors.Add("Runtime command evidence does not contain the recording start command.")
    }
    if (@($commandRows | Where-Object Command -eq "exit").Count -ne 1) {
        $errors.Add("Runtime command evidence must contain exactly one normal exit command.")
    }
    $exportCommandCount = @($commandRows | Where-Object {
        $_.Command -ne "toggle-recording" -and $_.Command -ne "exit"
    }).Count
    if ($artifactRows.Count -ne ($exportCommandCount + 1)) {
        $errors.Add("Artifact count does not equal one full recording plus every queued clip/bookmark command.")
    }

    $diagnosticLines = @()
    $diagnosticsPath = Join-Path $root "diagnostics-snapshots.log"
    if (Test-Path -LiteralPath $diagnosticsPath -PathType Leaf) {
        $diagnosticLines = @([System.IO.File]::ReadAllLines($diagnosticsPath) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        if ($diagnosticLines.Count -ne [int]$summary.DiagnosticsSnapshotCount) {
            $errors.Add("Diagnostics snapshot count $($diagnosticLines.Count) does not match summary count $($summary.DiagnosticsSnapshotCount).")
        }
    }
    if ($schemaVersion -ge 2 -and $schema2SummaryMissing.Count -eq 0 -and $diagnosticLines.Count -ne 0) {
        $lastDiagnostics = $diagnosticLines[-1]
        if ([string]$summary.FinalDiagnostics -ne $lastDiagnostics) {
            $errors.Add("Final diagnostics in summary.json do not match diagnostics-snapshots.log.")
        }
        $diagnosticMetrics = @{
            ConfiguredAudioTrackCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "audio_tracks"
            PacketBearingAudioTrackCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "packet_bearing_audio_tracks"
            SubmittedExportCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "submitted_exports"
            SavedExportCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "saved_exports"
            FailedExportCount = Get-OLouieDiagnosticsMetric -Line $lastDiagnostics -Name "failed_exports"
        }
        foreach ($entry in $diagnosticMetrics.GetEnumerator()) {
            if ($null -eq $entry.Value -or [int64]$summary.($entry.Key) -ne [int64]$entry.Value) {
                $errors.Add("Summary metric '$($entry.Key)' does not match final diagnostics.")
            }
        }
    }

    $beforePath = Join-Path $root "sessions-before.json"
    $afterPath = Join-Path $root "sessions-after.json"
    if ((Test-Path -LiteralPath $beforePath -PathType Leaf) -and (Test-Path -LiteralPath $afterPath -PathType Leaf)) {
        $beforeText = Get-Content -LiteralPath $beforePath -Raw
        $afterText = Get-Content -LiteralPath $afterPath -Raw
        if ($beforeText -ne $afterText) {
            $errors.Add("Before/after pre-existing session inventories differ.")
        }
    }
    $differencePath = Join-Path $root "preexisting-session-diff.json"
    if (-not (Test-Path -LiteralPath $differencePath -PathType Leaf)) {
        $warnings.Add("preexisting-session-diff.json is missing; matching before/after inventories were used instead.")
    } else {
        try {
            $differenceText = Get-Content -LiteralPath $differencePath -Raw
            $difference = $differenceText | ConvertFrom-Json
            $differenceCount = if ($null -eq $difference) {
                0
            } elseif ($difference -is [System.Array]) {
                $difference.Count
            } else {
                1
            }
            if ($differenceCount -ne 0) {
                $errors.Add("preexisting-session-diff.json is not empty.")
            }
        } catch {
            $errors.Add("preexisting-session-diff.json is not valid JSON.")
        }
    }

    $cleanupValid = $false
    $cleanupSchemaVersion = 0
    $cleanupPath = Join-Path $root "cleanup.json"
    if (Test-Path -LiteralPath $cleanupPath -PathType Leaf) {
        try {
            $cleanup = Get-Content -LiteralPath $cleanupPath -Raw | ConvertFrom-Json
            if ($null -ne $cleanup.PSObject.Properties["EvidenceSchemaVersion"]) {
                $cleanupSchemaVersion = [int]$cleanup.EvidenceSchemaVersion
            }
            $cleanupProperties = @(
                "EvidenceSchemaVersion",
                "Success",
                "SettingsRestored",
                "SettingsBeforeSha256",
                "SettingsAfterSha256",
                "AppProcessExited",
                "StimulusProcessExited",
                "OLouieProcessCount"
            )
            $cleanupMissing = @($cleanupProperties | Where-Object { $null -eq $cleanup.PSObject.Properties[$_] })
            if ($cleanupMissing.Count -ne 0) {
                $errors.Add("cleanup.json is missing: $($cleanupMissing -join ', ').")
            } elseif ([int]$cleanup.EvidenceSchemaVersion -lt 1 -or
                      $cleanup.Success -ne $true -or
                      $cleanup.SettingsRestored -ne $true -or
                      $cleanup.AppProcessExited -ne $true -or
                      $cleanup.StimulusProcessExited -ne $true -or
                      [int]$cleanup.OLouieProcessCount -ne 0) {
                $errors.Add("cleanup.json does not report complete settings/process cleanup.")
            } else {
                $settingsAfterPath = Join-Path $root "settings-after.json"
                if (-not (Test-Path -LiteralPath $settingsAfterPath -PathType Leaf)) {
                    $errors.Add("settings-after.json is missing despite cleanup evidence.")
                } else {
                    $beforeHash = (Get-FileHash -LiteralPath (Join-Path $root "settings-before.json") -Algorithm SHA256).Hash
                    $afterHash = (Get-FileHash -LiteralPath $settingsAfterPath -Algorithm SHA256).Hash
                    if ($beforeHash -ne $afterHash -or
                        $beforeHash -ne [string]$cleanup.SettingsBeforeSha256 -or
                        $afterHash -ne [string]$cleanup.SettingsAfterSha256) {
                        $errors.Add("The retained before/after settings files do not match.")
                    } else {
                        $cleanupValid = $true
                    }
                }
            }
        } catch {
            $errors.Add("cleanup.json is invalid: $($_.Exception.Message)")
        }
    } else {
        $warnings.Add("cleanup.json is missing; this evidence predates post-cleanup proof.")
    }

    if ($schemaVersion -lt 3) {
        $durationIssues.Add("Evidence schema version 3 or newer is required for hashed, freshly reinspected stream proof.")
    }
    if ([double]$summary.RequestedDurationSeconds -le 3600.0) {
        $durationIssues.Add("Requested duration did not exceed one hour.")
    }
    if ($summary.RequestedDurationCompleted -ne $true) {
        $durationIssues.Add("The requested duration was not completed.")
    }
    if ([string]$summary.StopReason -ne "duration") {
        $durationIssues.Add("The recording ended for '$($summary.StopReason)' instead of the duration boundary.")
    }
    if ([double]$summary.ObservedRecordingSeconds -le 3600.0) {
        $durationIssues.Add("Observed recording time did not exceed one hour.")
    }
    if ($resourceSpanSeconds -le 3600.0) {
        $durationIssues.Add("Resource samples do not span more than one hour.")
    }
    if ($null -eq $resourceTrend -or [double]$resourceTrend.ElapsedHours -le 1.0) {
        $durationIssues.Add("Resource trend evidence does not span more than one hour.")
    }
    if ($summary.FfprobeAvailable -ne $true) {
        $durationIssues.Add("Duration-complete evidence requires independent ffprobe inspection.")
    }
    if (@($commandRows | Where-Object Command -eq "toggle-recording").Count -lt 2) {
        $durationIssues.Add("Duration-complete evidence requires runner-owned start and stop commands.")
    }
    if (-not $cleanupValid) {
        $durationIssues.Add("Post-cleanup settings and process evidence is required.")
    }
    if ($cleanupSchemaVersion -lt 3) {
        $durationIssues.Add("Schema-3 post-cleanup evidence is required.")
    }
    if (-not $systemAudioRequested -or -not $microphoneRequested -or -not $separateAudioTracksRequested) {
        $durationIssues.Add("System audio, microphone, and separate audio tracks must be enabled in settings-during.json.")
    }
    if ($configuredAudioSampleRate -ne 48000) {
        $durationIssues.Add("The configured audio sample rate must be 48000 Hz.")
    }
    if ($configuredVideoEncoderBackend -ne "media_foundation_hardware") {
        $durationIssues.Add("The configured video backend must remain hardware-only Media Foundation H.264.")
    }
    if ($schemaVersion -ge 2 -and $schema2SummaryMissing.Count -eq 0) {
        if ([int]$summary.ConfiguredAudioTrackCount -lt 2) {
            $durationIssues.Add("Final diagnostics do not report both configured direct audio tracks.")
        }
        if ([int]$summary.PacketBearingAudioTrackCount -lt 2) {
            $durationIssues.Add("Final diagnostics do not report packet-bearing system and microphone tracks.")
        }
        if ([int]$summary.SubmittedExportCount -ne $exportCommandCount -or
            [int]$summary.SavedExportCount -ne $exportCommandCount -or
            [int]$summary.FailedExportCount -ne 0) {
            $durationIssues.Add("Final diagnostics do not report successful completion of every scheduled export.")
        }
    }
    if ($schemaVersion -ge 2 -and $summary.FfprobeAvailable -eq $true) {
        if (-not $allArtifactsHaveExpectedVideo) {
            $durationIssues.Add("Every MP4 must contain exactly one H.264 video stream.")
        }
        if (-not $allArtifactsHaveAacAudio) {
            $durationIssues.Add("Every MP4 must contain one or more AAC audio streams and no other audio codec.")
        }
        if ($fullArtifactRows.Count -ne 1 -or $fullArtifactDurationSeconds -le 3600.0) {
            $durationIssues.Add("The independently probed full MP4 duration must exceed one hour.")
        }
        if (-not $fullArtifactHasDualAac) {
            $durationIssues.Add("The full MP4 must contain packet-bearing system and microphone AAC streams.")
        }
        if (-not $fullArtifactAudioFormatsValid) {
            $durationIssues.Add("The full MP4 audio streams must be 48 kHz stereo AAC.")
        }
    }
    if ($schemaVersion -ge 3 -and $artifactReprobeCount -ne $artifactRows.Count) {
        $durationIssues.Add("Every retained MP4 must pass fresh structural and ffprobe reinspection.")
    }

    $evidenceValid = $errors.Count -eq 0
    $durationSatisfied = $evidenceValid -and $durationIssues.Count -eq 0
    $status = if (-not $evidenceValid) { "Invalid" } elseif ($durationSatisfied) { "Verified" } else { "Preflight" }
    return [pscustomobject]@{
        EvidenceDirectory = $root
        Status = $status
        EvidenceValid = $evidenceValid
        DurationRequirementSatisfied = $durationSatisfied
        ResourceSpanSeconds = [math]::Round($resourceSpanSeconds, 3)
        ArtifactCount = $artifactRows.Count
        ReinspectedArtifactCount = $artifactReprobeCount
        ResourceTrend = $resourceTrend
        Errors = @($errors)
        Warnings = @($warnings)
        DurationIssues = @($durationIssues)
        Summary = $summary
    }
}
