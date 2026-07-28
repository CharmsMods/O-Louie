function Get-OLouieRecorderLogState {
    [CmdletBinding()]
    param([AllowEmptyString()][string]$LogText = "")

    $latestIndex = -1
    $latestState = "none"
    $events = @(
        [pscustomobject]@{
            State = "recording"
            Pattern = "Recording started\.|Diagnostics snapshot:[^\r\n]*\brecorder=recording\b"
        },
        [pscustomobject]@{
            State = "stopping"
            Pattern = "Stopping and saving the recording\.|Diagnostics snapshot:[^\r\n]*\brecorder=stopping\b"
        },
        [pscustomobject]@{
            State = "saved"
            Pattern = "Recording saved:|Diagnostics snapshot:[^\r\n]*\brecorder=saved\b"
        },
        [pscustomobject]@{
            State = "failed"
            Pattern = "(?:\[error\]\s*)?Recording failed|Diagnostics snapshot:[^\r\n]*\brecorder=failed\b"
        }
    )

    foreach ($event in $events) {
        $matches = [regex]::Matches(
            $LogText,
            $event.Pattern,
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        foreach ($match in $matches) {
            if ($match.Index -gt $latestIndex) {
                $latestIndex = $match.Index
                $latestState = $event.State
            }
        }
    }

    return $latestState
}

function Get-OLouieDiagnosticsMetric {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Line,
        [Parameter(Mandatory = $true)][ValidatePattern("^[a-z][a-z0-9_]*$")][string]$Name
    )

    $pattern = "(?:^|[,\s]){0}=(-?\d+)(?:[,\s]|\.|$)" -f [regex]::Escape($Name)
    $match = [regex]::Match($Line, $pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        return $null
    }
    return [int64]$match.Groups[1].Value
}

function Find-OLouieFfprobe {
    [CmdletBinding()]
    param([string]$PreferredPath)

    if (-not [string]::IsNullOrWhiteSpace($PreferredPath)) {
        $resolved = (Resolve-Path -LiteralPath $PreferredPath -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "ffprobe path is not a file: $resolved"
        }
        return $resolved
    }

    $command = Get-Command "ffprobe.exe" -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $packageRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if (Test-Path -LiteralPath $packageRoot -PathType Container) {
        $packaged = Get-ChildItem -LiteralPath $packageRoot -Filter "ffprobe.exe" -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like "*Gyan.FFmpeg*" } |
            Select-Object -First 1
        if ($null -ne $packaged) {
            return $packaged.FullName
        }
    }
    return $null
}

function Get-OLouieMp4ProbeFacts {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$FfprobePath
    )

    $resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        throw "MP4 probe path is not a file: $resolvedPath"
    }
    $probeExecutable = Find-OLouieFfprobe -PreferredPath $FfprobePath
    if ($null -eq $probeExecutable) {
        throw "ffprobe.exe is unavailable."
    }

    $probeOutput = & $probeExecutable -v error -show_entries "format=duration,size:stream=index,codec_name,codec_type,sample_rate,channels" -of json $resolvedPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "ffprobe failed for ${resolvedPath}: $probeOutput"
    }

    try {
        $probe = $probeOutput | ConvertFrom-Json
        if ($null -eq $probe.PSObject.Properties["format"] -or
            $null -eq $probe.format.PSObject.Properties["duration"] -or
            $null -eq $probe.format.PSObject.Properties["size"]) {
            throw "format duration or size is missing"
        }
        $streams = if ($null -eq $probe.PSObject.Properties["streams"]) { @() } else { @($probe.streams) }
        $videoStreams = @($streams | Where-Object { $_.codec_type -eq "video" })
        $audioStreams = @($streams | Where-Object { $_.codec_type -eq "audio" })
        return [pscustomobject]@{
            DurationSeconds = [double]$probe.format.duration
            FormatSizeBytes = [int64]$probe.format.size
            VideoStreamCount = $videoStreams.Count
            H264StreamCount = @($videoStreams | Where-Object { $_.codec_name -eq "h264" }).Count
            AudioStreamCount = $audioStreams.Count
            AacStreamCount = @($audioStreams | Where-Object { $_.codec_name -eq "aac" }).Count
            AudioSampleRates = @($audioStreams | ForEach-Object { [string]$_.sample_rate }) -join ";"
            AudioChannels = @($audioStreams | ForEach-Object { [string]$_.channels }) -join ";"
            ProbeOutput = $probeOutput.Trim()
        }
    } catch {
        throw "ffprobe returned invalid stream metadata for ${resolvedPath}: $($_.Exception.Message)"
    }
}

function Get-OLouieMp4ReinspectionFacts {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$InspectorPath,
        [string]$FfprobePath
    )

    $resolvedInspector = (Resolve-Path -LiteralPath $InspectorPath -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolvedInspector -PathType Leaf)) {
        throw "MP4 artifact inspector path is not a file: $resolvedInspector"
    }
    $inspectionOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedInspector -Path $Path 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "MP4 structure reinspection failed: $inspectionOutput"
    }
    return Get-OLouieMp4ProbeFacts -Path $Path -FfprobePath $FfprobePath
}

function Get-MedianNumber {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0) {
        throw "A median requires at least one value."
    }
    $sorted = @($Values | Sort-Object)
    $middle = [math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-LongSessionResourceTrend {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Samples,
        [ValidateRange(1, 1000)][int]$WindowSize = 5
    )

    $items = @($Samples)
    if ($items.Count -lt 2) {
        return [pscustomobject]@{
            Available = $false
            SampleCount = $items.Count
            WindowSize = 0
            ElapsedHours = 0.0
        }
    }

    $effectiveWindow = [int][math]::Min($WindowSize, [math]::Floor($items.Count / 2))
    $head = @($items | Select-Object -First $effectiveWindow)
    $tail = @($items | Select-Object -Last $effectiveWindow)
    $elapsedHours = ([double]$items[-1].ElapsedSeconds - [double]$items[0].ElapsedSeconds) / 3600.0

    $headWorkingSet = Get-MedianNumber -Values ([double[]]@($head | ForEach-Object { [double]$_.WorkingSetBytes }))
    $tailWorkingSet = Get-MedianNumber -Values ([double[]]@($tail | ForEach-Object { [double]$_.WorkingSetBytes }))
    $headPrivate = Get-MedianNumber -Values ([double[]]@($head | ForEach-Object { [double]$_.PrivateBytes }))
    $tailPrivate = Get-MedianNumber -Values ([double[]]@($tail | ForEach-Object { [double]$_.PrivateBytes }))
    $headHandles = Get-MedianNumber -Values ([double[]]@($head | ForEach-Object { [double]$_.HandleCount }))
    $tailHandles = Get-MedianNumber -Values ([double[]]@($tail | ForEach-Object { [double]$_.HandleCount }))
    $headThreads = Get-MedianNumber -Values ([double[]]@($head | ForEach-Object { [double]$_.ThreadCount }))
    $tailThreads = Get-MedianNumber -Values ([double[]]@($tail | ForEach-Object { [double]$_.ThreadCount }))

    $workingSetGrowth = $tailWorkingSet - $headWorkingSet
    $privateGrowth = $tailPrivate - $headPrivate
    $handleGrowth = $tailHandles - $headHandles
    $threadGrowth = $tailThreads - $headThreads
    $perHourDivisor = if ($elapsedHours -gt 0.0) { $elapsedHours } else { 1.0 }

    return [pscustomobject]@{
        Available = $true
        SampleCount = $items.Count
        WindowSize = $effectiveWindow
        ElapsedHours = [math]::Round($elapsedHours, 6)
        WorkingSetHeadMedianBytes = [int64]$headWorkingSet
        WorkingSetTailMedianBytes = [int64]$tailWorkingSet
        WorkingSetGrowthBytes = [int64]$workingSetGrowth
        WorkingSetGrowthBytesPerHour = [math]::Round($workingSetGrowth / $perHourDivisor, 3)
        PrivateBytesHeadMedian = [int64]$headPrivate
        PrivateBytesTailMedian = [int64]$tailPrivate
        PrivateBytesGrowth = [int64]$privateGrowth
        PrivateBytesGrowthPerHour = [math]::Round($privateGrowth / $perHourDivisor, 3)
        HandleCountHeadMedian = [math]::Round($headHandles, 3)
        HandleCountTailMedian = [math]::Round($tailHandles, 3)
        HandleCountGrowth = [math]::Round($handleGrowth, 3)
        HandleCountGrowthPerHour = [math]::Round($handleGrowth / $perHourDivisor, 3)
        ThreadCountHeadMedian = [math]::Round($headThreads, 3)
        ThreadCountTailMedian = [math]::Round($tailThreads, 3)
        ThreadCountGrowth = [math]::Round($threadGrowth, 3)
        ThreadCountGrowthPerHour = [math]::Round($threadGrowth / $perHourDivisor, 3)
    }
}
