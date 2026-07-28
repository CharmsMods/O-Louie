[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$ToolsDir = $PSScriptRoot
$Root = Split-Path -Parent $ToolsDir
$DepsDir = Join-Path $Root "_deps"
$PrepareScript = Join-Path $ToolsDir "PrepareFfmpegRootFromArchive.ps1"
$RootVerifier = Join-Path $ToolsDir "VerifyFfmpegRoot.ps1"
$GateChecker = Join-Path $ToolsDir "CheckFfmpegMuxerGate.ps1"
$MuxerVerifier = Join-Path $ToolsDir "VerifyFfmpegMuxer.ps1"
$Mp4Inspector = Join-Path $ToolsDir "InspectMp4Artifact.ps1"
$SourceBuildScript = Join-Path $ToolsDir "BuildFfmpegLgplFromSource.ps1"

function Stop-Test {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Error $Message
    exit 1
}

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@(
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
        Stop-Test "$Description must be inside the repository root: $targetPath"
    }

    return $targetPath
}

function Invoke-ScriptRequired {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    $commandArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath) + $Arguments
    & powershell.exe @commandArgs
    if ($LASTEXITCODE -ne 0) {
        Stop-Test "$Description failed with exit code $LASTEXITCODE."
    }
}

function Invoke-ScriptExpectFailure {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    $commandArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath) + $Arguments
    & powershell.exe @commandArgs
    if ($LASTEXITCODE -eq 0) {
        Stop-Test "$Description unexpectedly succeeded."
    }
    Write-Host "$Description failed as expected."
}

function Invoke-ScriptExpectFailureContaining {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string]$ExpectedText
    )

    $commandArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath) + $Arguments
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $outputLines = & powershell.exe @commandArgs 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $output = $outputLines | Out-String
    if ($exitCode -eq 0) {
        Stop-Test "$Description unexpectedly succeeded."
    }

    if (-not $output.Contains($ExpectedText)) {
        Stop-Test "$Description failed, but output did not contain '$ExpectedText'. Output: $output"
    }

    Write-Host "$Description failed as expected."
}

function Invoke-ScriptCaptureRequired {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    $commandArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ScriptPath) + $Arguments
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $outputLines = & powershell.exe @commandArgs 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $output = $outputLines | Out-String
    if ($exitCode -ne 0) {
        Stop-Test "$Description failed with exit code $exitCode. Output: $output"
    }

    return $output
}

function Assert-TextContains {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not $Text.Contains($Expected)) {
        Stop-Test "$Description did not contain '$Expected'. Output: $Text"
    }
}

function Invoke-NativeRequired {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        Stop-Test "$Description failed with exit code $LASTEXITCODE."
    }
}

function Invoke-NativeExpectFailure {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -eq 0) {
        Stop-Test "$Description unexpectedly succeeded."
    }
    Write-Host "$Description failed as expected."
}

function Write-FixtureFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    [System.IO.File]::WriteAllText($Path, "synthetic FFmpeg gate-tool fixture")
}

function Add-UInt32BigEndian {
    param(
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)][uint32]$Value
    )

    if ($null -eq $Bytes) {
        Stop-Test "Byte accumulator is required."
    }

    $Bytes.Add([byte](($Value -shr 24) -band 0xff))
    $Bytes.Add([byte](($Value -shr 16) -band 0xff))
    $Bytes.Add([byte](($Value -shr 8) -band 0xff))
    $Bytes.Add([byte]($Value -band 0xff))
}

function Add-AsciiBytes {
    param(
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)][string]$Text
    )

    if ($null -eq $Bytes) {
        Stop-Test "Byte accumulator is required."
    }

    foreach ($byte in [System.Text.Encoding]::ASCII.GetBytes($Text)) {
        $Bytes.Add($byte)
    }
}

function Add-Mp4Box {
    param(
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)][string]$Type,
        [byte[]]$Payload = @()
    )

    if ($null -eq $Bytes) {
        Stop-Test "Byte accumulator is required."
    }

    Add-UInt32BigEndian -Bytes $Bytes -Value ([uint32](8 + $Payload.Count))
    Add-AsciiBytes -Bytes $Bytes -Text $Type
    foreach ($byte in $Payload) {
        $Bytes.Add($byte)
    }
}

function Write-SyntheticMp4Artifact {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$MissingMoov,
        [switch]$InvalidFirstBox,
        [switch]$EmptyMdat,
        [switch]$EmptyMoov
    )

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path))

    $bytes = [System.Collections.Generic.List[byte]]::new()
    if ($InvalidFirstBox) {
        Add-Mp4Box -Bytes $bytes -Type "free" -Payload ([byte[]]@(0, 0, 0, 0))
    }

    Add-Mp4Box -Bytes $bytes -Type "ftyp" -Payload ([byte[]]@(
        0x69, 0x73, 0x6f, 0x6d,
        0x00, 0x00, 0x02, 0x00,
        0x69, 0x73, 0x6f, 0x6d,
        0x6d, 0x70, 0x34, 0x32))
    $mdatPayload = [byte[]]@(1, 2, 3, 4, 5, 6, 7, 8)
    if ($EmptyMdat) {
        $mdatPayload = [byte[]]@()
    }
    Add-Mp4Box -Bytes $bytes -Type "mdat" -Payload $mdatPayload

    if (-not $MissingMoov) {
        $moovPayload = [byte[]]@(0, 0, 0, 0)
        if ($EmptyMoov) {
            $moovPayload = [byte[]]@()
        }
        Add-Mp4Box -Bytes $bytes -Type "moov" -Payload $moovPayload
    }

    [System.IO.File]::WriteAllBytes($Path, $bytes.ToArray())
}

function Write-SyntheticVerificationReport {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [string]$FfmpegRoot = "<not supplied>",
        [string]$FfmpegIncludeDir = "<not supplied>",
        [string]$FfmpegLibraryDir = "<not supplied>",
        [string]$FfmpegBinaryDir = "<not supplied>",
        [Parameter(Mandatory = $true)][string]$RecordTestOutputPath,
        [Parameter(Mandatory = $true)][string]$RecordTestMp4,
        [string]$GeneratedUtc = "",
        [string]$VideoMp4ArtifactSizeBytes = "",
        [string]$VideoMp4ArtifactLastWriteUtc = "",
        [string]$FfmpegArchiveInput = "<not supplied>",
        [string]$WgcSmoke = "skipped",
        [string]$WgcSmokeOptions = "duration_ms=3000 width=1920 height=1080 fps=60 bitrate_mbps=20",
        [string]$Verifier = "tools\VerifyFfmpegMuxer.ps1",
        [switch]$OmitSchemaVersion,
        [switch]$OmitFfmpegArchiveInput,
        [switch]$OmitWgcSmoke,
        [switch]$OmitWgcSmokeOptions
    )

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    $artifactInfo = Get-Item -LiteralPath $RecordTestMp4
    if ([string]::IsNullOrWhiteSpace($GeneratedUtc)) {
        $GeneratedUtc = (Get-Date).ToUniversalTime().ToString("o")
    }

    if ([string]::IsNullOrWhiteSpace($VideoMp4ArtifactSizeBytes)) {
        $VideoMp4ArtifactSizeBytes = $artifactInfo.Length.ToString()
    }

    if ([string]::IsNullOrWhiteSpace($VideoMp4ArtifactLastWriteUtc)) {
        $VideoMp4ArtifactLastWriteUtc = $artifactInfo.LastWriteTimeUtc.ToString("o")
    }

    $lines = @(
        "O'Louie FFmpeg muxer verification report",
        "schema_version: 1",
        "generated_utc: $GeneratedUtc",
        "configuration: $Configuration",
        "ffmpeg_root: $FfmpegRoot",
        "ffmpeg_include_dir: $FfmpegIncludeDir",
        "ffmpeg_library_dir: $FfmpegLibraryDir",
        "ffmpeg_binary_dir: $FfmpegBinaryDir",
        "ffmpeg_archive_input: $FfmpegArchiveInput",
        "record_test_output_dir: $RecordTestOutputPath",
        "video_mp4_artifact: $RecordTestMp4",
        "video_mp4_artifact_size_bytes: $VideoMp4ArtifactSizeBytes",
        "video_mp4_artifact_last_write_utc: $VideoMp4ArtifactLastWriteUtc",
        "record_test: passed",
        "mp4_artifact_inspection: passed",
        "wgc_smoke: $WgcSmoke",
        "wgc_smoke_options: $WgcSmokeOptions",
        "verifier: $Verifier"
    )

    if ($OmitSchemaVersion) {
        $lines = @($lines | Where-Object { -not $_.StartsWith("schema_version:") })
    }

    if ($OmitFfmpegArchiveInput) {
        $lines = @($lines | Where-Object { -not $_.StartsWith("ffmpeg_archive_input:") })
    }

    if ($OmitWgcSmoke) {
        $lines = @($lines | Where-Object { -not $_.StartsWith("wgc_smoke:") })
    }

    if ($OmitWgcSmokeOptions) {
        $lines = @($lines | Where-Object { -not $_.StartsWith("wgc_smoke_options:") })
    }

    [System.IO.File]::WriteAllLines($Path, [string[]]$lines, [System.Text.Encoding]::UTF8)
}

function New-SyntheticFfmpegRoot {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [switch]$IncludeHeaders,
        [switch]$IncludeLibraries,
        [switch]$IncludeBinaries
    )

    if ($IncludeHeaders) {
        Write-FixtureFile -Path (Join-Path $RootPath "include\libavformat\avformat.h")
        Write-FixtureFile -Path (Join-Path $RootPath "include\libavcodec\avcodec.h")
        Write-FixtureFile -Path (Join-Path $RootPath "include\libavutil\avutil.h")
        Write-FixtureFile -Path (Join-Path $RootPath "include\libswresample\swresample.h")
    }

    if ($IncludeLibraries) {
        Write-FixtureFile -Path (Join-Path $RootPath "lib\avformat.lib")
        Write-FixtureFile -Path (Join-Path $RootPath "lib\avcodec.lib")
        Write-FixtureFile -Path (Join-Path $RootPath "lib\avutil.lib")
        Write-FixtureFile -Path (Join-Path $RootPath "lib\swresample.lib")
    }

    if ($IncludeBinaries) {
        Write-FixtureFile -Path (Join-Path $RootPath "bin\avformat-60.dll")
        Write-FixtureFile -Path (Join-Path $RootPath "bin\avcodec-60.dll")
        Write-FixtureFile -Path (Join-Path $RootPath "bin\avutil-60.dll")
        Write-FixtureFile -Path (Join-Path $RootPath "bin\swresample-4.dll")
    }
}

function New-ZipFromDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$ZipPath
    )

    Compress-Archive -LiteralPath $Directory -DestinationPath $ZipPath -Force
    return $ZipPath
}

function Remove-RepoDirectoryIfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    [void](Assert-PathInsideRoot -Path $Path -Description $Description)
    Remove-Item -LiteralPath $Path -Recurse -Force
}

function Get-CmakeFfmpegConfigureArgs {
    param(
        [string]$FfmpegRoot,
        [string]$FfmpegIncludeDir,
        [string]$FfmpegLibraryDir,
        [string]$FfmpegBinaryDir,
        [Parameter(Mandatory = $true)][string]$BuildDir
    )

    $generator = $env:CMAKE_GENERATOR
    if ([string]::IsNullOrWhiteSpace($generator)) {
        $generator = "Visual Studio 17 2022"
    }

    $configureArgs = @(
        "-S", $Root,
        "-B", $BuildDir,
        "-G", $generator,
        "-DOLOUIE_BUILD_TESTS=OFF",
        "-DOLOUIE_ENABLE_FFMPEG=ON"
    )

    if (-not [string]::IsNullOrWhiteSpace($FfmpegRoot)) {
        $configureArgs += "-DOLOUIE_FFMPEG_ROOT=$FfmpegRoot"
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegIncludeDir)) {
        $configureArgs += "-DOLOUIE_FFMPEG_INCLUDE_DIR=$FfmpegIncludeDir"
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegLibraryDir)) {
        $configureArgs += "-DOLOUIE_FFMPEG_LIBRARY_DIR=$FfmpegLibraryDir"
    }
    if (-not [string]::IsNullOrWhiteSpace($FfmpegBinaryDir)) {
        $configureArgs += "-DOLOUIE_FFMPEG_BINARY_DIR=$FfmpegBinaryDir"
    }

    if ($generator -like "Visual Studio*") {
        $configureArgs += @("-A", "x64")
    }

    return $configureArgs
}

function Test-CmakeFfmpegConfigure {
    param(
        [string]$FfmpegRoot,
        [string]$FfmpegIncludeDir,
        [string]$FfmpegLibraryDir,
        [string]$FfmpegBinaryDir,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$Scenario
    )

    Write-Host "Testing $Scenario CMake FFmpeg configure path..."
    $configureArgs = Get-CmakeFfmpegConfigureArgs `
        -FfmpegRoot $FfmpegRoot `
        -FfmpegIncludeDir $FfmpegIncludeDir `
        -FfmpegLibraryDir $FfmpegLibraryDir `
        -FfmpegBinaryDir $FfmpegBinaryDir `
        -BuildDir $BuildDir
    Invoke-NativeRequired -FilePath "cmake" -Arguments $configureArgs -Description "$Scenario CMake FFmpeg configure"
}

function Test-PreparedRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string[]]$ArchivePaths,
        [Parameter(Mandatory = $true)][string]$Scenario
    )

    Write-Host "Testing $Scenario..."
    $inspectArgs = @("-ArchivePath", ($ArchivePaths -join ","), "-InspectOnly")
    Invoke-ScriptRequired -ScriptPath $PrepareScript -Arguments $inspectArgs -Description "$Scenario archive inspection"
    Invoke-ScriptRequired `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegArchivePath", ($ArchivePaths -join ",")) `
        -Description "$Scenario archive candidate gate inspection"
    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegArchivePath", ($ArchivePaths -join ","), "-RequireReady") `
        -Description "$Scenario archive candidate require-ready check"

    $prepareArgs = @("-ArchivePath", ($ArchivePaths -join ","), "-DestinationDir", $Destination, "-Force")
    Invoke-ScriptRequired -ScriptPath $PrepareScript -Arguments $prepareArgs -Description "$Scenario archive preparation"
    Invoke-ScriptRequired -ScriptPath $RootVerifier -Arguments @("-FfmpegRoot", $Destination) -Description "$Scenario root verification"
    Invoke-ScriptRequired -ScriptPath $GateChecker -Arguments @("-FfmpegRoot", $Destination, "-RequireReady") -Description "$Scenario gate ready check"
    Test-CmakeFfmpegConfigure `
        -FfmpegRoot $Destination `
        -BuildDir (Join-Path $tempRoot ("cmake-" + [System.Guid]::NewGuid().ToString("N"))) `
        -Scenario $Scenario
}

function Test-InvalidArchiveInspectionFails {
    param(
        [Parameter(Mandatory = $true)][string[]]$ArchivePaths,
        [Parameter(Mandatory = $true)][string]$Scenario
    )

    Write-Host "Testing $Scenario archive inspection failure..."
    Invoke-ScriptExpectFailure `
        -ScriptPath $PrepareScript `
        -Arguments @("-ArchivePath", ($ArchivePaths -join ","), "-InspectOnly") `
        -Description "$Scenario archive inspection"
    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegArchivePath", ($ArchivePaths -join ",")) `
        -Description "$Scenario archive candidate gate inspection"
    Invoke-ScriptExpectFailure `
        -ScriptPath $MuxerVerifier `
        -Arguments @("-FfmpegArchivePath", ($ArchivePaths -join ",")) `
        -Description "$Scenario archive-to-muxer verifier preparation"
}

function Test-ArchivePreparationDestinationPathValidation {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$AttemptRoot
    )

    Write-Host "Testing FFmpeg archive preparation destination path validation..."

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)

    $destinationFile = Join-Path $AttemptRoot "ffmpeg-destination-file"
    [System.IO.File]::WriteAllText($destinationFile, "not a destination directory")

    try {
        Invoke-ScriptExpectFailureContaining `
            -ScriptPath $PrepareScript `
            -Arguments @(
                "-ArchivePath", $ArchivePath,
                "-DestinationDir", $destinationFile,
                "-Force") `
            -Description "archive preparation destination file target check" `
            -ExpectedText "DestinationDir must name a directory, not an existing file"

        if (-not (Test-Path -LiteralPath $destinationFile -PathType Leaf)) {
            Stop-Test "FFmpeg archive preparation removed the destination file target during validation."
        }

        $destinationAncestorFile = Join-Path $AttemptRoot "ffmpeg-destination-ancestor-file"
        [System.IO.File]::WriteAllText($destinationAncestorFile, "not a destination ancestor directory")
        $destinationUnderFileAncestor = Join-Path $destinationAncestorFile "nested\ffmpeg"

        Invoke-ScriptExpectFailureContaining `
            -ScriptPath $PrepareScript `
            -Arguments @(
                "-ArchivePath", $ArchivePath,
                "-DestinationDir", $destinationUnderFileAncestor,
                "-Force") `
            -Description "archive preparation destination ancestor-file target check" `
            -ExpectedText "DestinationDir parent must be a directory, not an existing file"

        if (-not (Test-Path -LiteralPath $destinationAncestorFile -PathType Leaf)) {
            Stop-Test "FFmpeg archive preparation removed the destination ancestor-file target during validation."
        }
    } finally {
        if (Test-Path -LiteralPath $AttemptRoot) {
            [void](Assert-PathInsideRoot -Path $AttemptRoot -Description "archive destination validation attempt root")
            Remove-Item -LiteralPath $AttemptRoot -Recurse -Force
        }
    }
}

function Test-ExplicitSplitDirs {
    param([Parameter(Mandatory = $true)][string]$SplitRoot)

    Write-Host "Testing explicit split FFmpeg directories..."
    New-SyntheticFfmpegRoot -RootPath $SplitRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries
    $includeDir = Join-Path $SplitRoot "include"
    $libraryDir = Join-Path $SplitRoot "lib"
    $binaryDir = Join-Path $SplitRoot "bin"
    $splitArgs = @(
        "-FfmpegIncludeDir", $includeDir,
        "-FfmpegLibraryDir", $libraryDir,
        "-FfmpegBinaryDir", $binaryDir
    )

    Invoke-ScriptRequired `
        -ScriptPath $RootVerifier `
        -Arguments $splitArgs `
        -Description "explicit split directory root verification"

    Invoke-ScriptRequired `
        -ScriptPath $GateChecker `
        -Arguments ($splitArgs + @("-RequireReady")) `
        -Description "explicit split directory gate ready check"

    Test-CmakeFfmpegConfigure `
        -FfmpegIncludeDir $includeDir `
        -FfmpegLibraryDir $libraryDir `
        -FfmpegBinaryDir $binaryDir `
        -BuildDir (Join-Path $tempRoot ("cmake-split-dirs-" + [System.Guid]::NewGuid().ToString("N"))) `
        -Scenario "explicit split FFmpeg directories"
}

function Test-PartialSplitDirsFail {
    param([Parameter(Mandatory = $true)][string]$SplitRoot)

    Write-Host "Testing partial split FFmpeg directory failure..."
    New-SyntheticFfmpegRoot -RootPath $SplitRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries
    $includeDir = Join-Path $SplitRoot "include"
    $libraryDir = Join-Path $SplitRoot "lib"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegIncludeDir", $includeDir, "-RequireReady") `
        -Description "partial split directory gate ready check"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegIncludeDir", $includeDir, "-FfmpegLibraryDir", $libraryDir, "-RequireReady") `
        -Description "partial include/library split directory gate ready check"
}

function Test-ArchiveInputConflictsFail {
    param(
        [Parameter(Mandatory = $true)][string]$ConflictRoot,
        [Parameter(Mandatory = $true)][string]$ArchivePath
    )

    Write-Host "Testing FFmpeg archive input conflict failure..."
    New-SyntheticFfmpegRoot -RootPath $ConflictRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries
    $includeDir = Join-Path $ConflictRoot "include"
    $libraryDir = Join-Path $ConflictRoot "lib"
    $binaryDir = Join-Path $ConflictRoot "bin"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegArchivePath", $ArchivePath, "-FfmpegRoot", $ConflictRoot, "-RequireReady") `
        -Description "archive plus root gate ready check"

    Invoke-ScriptExpectFailure `
        -ScriptPath $MuxerVerifier `
        -Arguments @("-FfmpegArchivePath", $ArchivePath, "-FfmpegRoot", $ConflictRoot) `
        -Description "archive plus root muxer verifier check"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegArchivePath", $ArchivePath, "-RunVerification") `
        -Description "archive plus gate run-verification check" `
        -ExpectedText "-FfmpegArchivePath on the gate checker is inspect-only and cannot be combined with -RunVerification."

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegArchivePath", $ArchivePath,
            "-FfmpegIncludeDir", $includeDir,
            "-FfmpegLibraryDir", $libraryDir,
            "-FfmpegBinaryDir", $binaryDir,
            "-RequireReady"
        ) `
        -Description "archive plus split directory gate ready check"

    Invoke-ScriptExpectFailure `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegArchivePath", $ArchivePath,
            "-FfmpegIncludeDir", $includeDir,
            "-FfmpegLibraryDir", $libraryDir,
            "-FfmpegBinaryDir", $binaryDir
        ) `
        -Description "archive plus split directory muxer verifier check"
}

function Test-MuxerVerifierRejectsArchivePreparationOptionsWithoutArchive {
    param(
        [Parameter(Mandatory = $true)][string]$FfmpegRoot,
        [Parameter(Mandatory = $true)][string]$AttemptRoot
    )

    Write-Host "Testing FFmpeg muxer verifier archive-preparation option requirement validation..."
    New-SyntheticFfmpegRoot -RootPath $FfmpegRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)
    $reportPath = Join-Path $AttemptRoot "verification-report.txt"
    $staleReportText = "stale FFmpeg muxer verification report"

    $destinationPath = Join-Path $AttemptRoot "ffmpeg-destination"
    [System.IO.File]::WriteAllText($reportPath, $staleReportText)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegRoot", $FfmpegRoot,
            "-FfmpegArchiveDestinationDir", $destinationPath,
            "-RecordTestOutputDir", (Join-Path $AttemptRoot "record-output-destination-option-check"),
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier archive-destination without archive input check" `
        -ExpectedText "-FfmpegArchiveDestinationDir requires -FfmpegArchivePath."

    if ((Get-Content -LiteralPath $reportPath -Raw) -ne $staleReportText) {
        Stop-Test "FFmpeg muxer verifier removed or changed a stale report before archive-destination option validation."
    }

    if (Test-Path -LiteralPath $destinationPath) {
        Stop-Test "FFmpeg muxer verifier created an archive destination when no archive input was supplied."
    }

    [System.IO.File]::WriteAllText($reportPath, $staleReportText)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegRoot", $FfmpegRoot,
            "-ForceFfmpegArchivePreparation",
            "-RecordTestOutputDir", (Join-Path $AttemptRoot "record-output-force-option-check"),
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier archive-force without archive input check" `
        -ExpectedText "-ForceFfmpegArchivePreparation requires -FfmpegArchivePath."

    if ((Get-Content -LiteralPath $reportPath -Raw) -ne $staleReportText) {
        Stop-Test "FFmpeg muxer verifier removed or changed a stale report before archive-force option validation."
    }
}

function Test-MuxerVerifierWgcOptionValidation {
    Write-Host "Testing FFmpeg muxer verifier WGC/report option validation..."

    $cases = @(
        [pscustomobject]@{
            Arguments = @("-WgcDurationMs", "0")
            Description = "zero WGC duration muxer verifier check"
            Expected = "WgcDurationMs must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-WgcDurationMs", "-1")
            Description = "negative WGC duration muxer verifier check"
            Expected = "WgcDurationMs must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-Width", "0")
            Description = "zero WGC width muxer verifier check"
            Expected = "Width must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-Height", "0")
            Description = "zero WGC height muxer verifier check"
            Expected = "Height must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-Fps", "0")
            Description = "zero WGC FPS muxer verifier check"
            Expected = "Fps must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-BitrateMbps", "0")
            Description = "zero WGC bitrate muxer verifier check"
            Expected = "BitrateMbps must be a positive integer."
        }
    )

    foreach ($case in $cases) {
        Invoke-ScriptExpectFailureContaining `
            -ScriptPath $MuxerVerifier `
            -Arguments $case.Arguments `
            -Description $case.Description `
            -ExpectedText $case.Expected
    }
}

function Test-MuxerVerifierClearsStaleReportOnAttempt {
    param(
        [Parameter(Mandatory = $true)][string]$InvalidRoot,
        [Parameter(Mandatory = $true)][string]$AttemptRoot
    )

    Write-Host "Testing FFmpeg muxer verifier stale-report cleanup..."
    New-SyntheticFfmpegRoot -RootPath $InvalidRoot -IncludeHeaders -IncludeLibraries

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)
    $reportPath = Join-Path $AttemptRoot "verification-report.txt"
    [System.IO.File]::WriteAllText($reportPath, "stale FFmpeg muxer verification report")

    Invoke-ScriptExpectFailure `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegRoot", $InvalidRoot,
            "-RecordTestOutputDir", (Join-Path $AttemptRoot "record-output"),
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier stale-report cleanup on failed root verification"

    if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
        Stop-Test "FFmpeg muxer verifier left a stale verification report after a failed verification attempt."
    }
}

function Test-MuxerVerifierArchiveDestinationPathValidation {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$AttemptRoot,
        [Parameter(Mandatory = $true)][string]$OutsideRoot
    )

    Write-Host "Testing FFmpeg muxer verifier archive-destination path validation..."

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)
    $reportPath = Join-Path $AttemptRoot "verification-report.txt"
    $staleReportText = "stale FFmpeg muxer verification report"

    $destinationFile = Join-Path $AttemptRoot "ffmpeg-destination-file"
    [System.IO.File]::WriteAllText($destinationFile, "not a destination directory")
    [System.IO.File]::WriteAllText($reportPath, $staleReportText)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegArchivePath", $ArchivePath,
            "-FfmpegArchiveDestinationDir", $destinationFile,
            "-RecordTestOutputDir", (Join-Path $AttemptRoot "record-output-file-target-check"),
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier archive-destination file target check" `
        -ExpectedText "FfmpegArchiveDestinationDir must name a directory, not an existing file"

    if ((Get-Content -LiteralPath $reportPath -Raw) -ne $staleReportText) {
        Stop-Test "FFmpeg muxer verifier removed or changed a stale report before archive-destination file target validation."
    }

    if (-not (Test-Path -LiteralPath $destinationFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the archive-destination file target during validation."
    }

    $destinationAncestorFile = Join-Path $AttemptRoot "ffmpeg-destination-ancestor-file"
    [System.IO.File]::WriteAllText($destinationAncestorFile, "not a destination ancestor directory")
    $destinationUnderFileAncestor = Join-Path $destinationAncestorFile "nested\ffmpeg"
    [System.IO.File]::WriteAllText($reportPath, $staleReportText)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegArchivePath", $ArchivePath,
            "-FfmpegArchiveDestinationDir", $destinationUnderFileAncestor,
            "-RecordTestOutputDir", (Join-Path $AttemptRoot "record-output-ancestor-check"),
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier archive-destination ancestor-file target check" `
        -ExpectedText "FfmpegArchiveDestinationDir parent must be a directory, not an existing file"

    if ((Get-Content -LiteralPath $reportPath -Raw) -ne $staleReportText) {
        Stop-Test "FFmpeg muxer verifier removed or changed a stale report before archive-destination ancestor-file validation."
    }

    if (-not (Test-Path -LiteralPath $destinationAncestorFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the archive-destination ancestor-file target during validation."
    }

    $outsideDestination = Join-Path $OutsideRoot "ffmpeg"
    [System.IO.File]::WriteAllText($reportPath, $staleReportText)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-FfmpegArchivePath", $ArchivePath,
            "-FfmpegArchiveDestinationDir", $outsideDestination,
            "-RecordTestOutputDir", (Join-Path $AttemptRoot "record-output-outside-root-check"),
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier archive-destination outside-root check" `
        -ExpectedText "FfmpegArchiveDestinationDir must be inside the repository root"

    if ((Get-Content -LiteralPath $reportPath -Raw) -ne $staleReportText) {
        Stop-Test "FFmpeg muxer verifier removed or changed a stale report before archive-destination outside-root validation."
    }

    if (Test-Path -LiteralPath $outsideDestination) {
        Stop-Test "FFmpeg muxer verifier created an outside-root archive destination during validation."
    }
}

function Test-MuxerVerifierRejectsReportDirectoryPath {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg muxer verifier report-directory target rejection..."

    $reportDirectory = Join-Path $AttemptRoot "verification-report-target"
    [void][System.IO.Directory]::CreateDirectory($reportDirectory)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @("-VerificationReportPath", $reportDirectory) `
        -Description "muxer verifier report-directory target check" `
        -ExpectedText "VerificationReportPath must name a report file, not a directory"

    $reportDirectoryChildren = Get-ChildItem -LiteralPath $reportDirectory -Force -ErrorAction SilentlyContinue
    if ($reportDirectoryChildren.Count -ne 0) {
        Stop-Test "FFmpeg muxer verifier wrote files into a verification-report directory target."
    }
}

function Test-MuxerVerifierRejectsRecordOutputFilePath {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg muxer verifier record-output file target rejection..."

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)
    $recordOutputFile = Join-Path $AttemptRoot "record-output"
    $reportPath = Join-Path $AttemptRoot "verification-report.txt"
    [System.IO.File]::WriteAllText($recordOutputFile, "not a record-output directory")

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-RecordTestOutputDir", $recordOutputFile,
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier record-output file target check" `
        -ExpectedText "RecordTestOutputDir must name an output directory, not an existing file"

    if (-not (Test-Path -LiteralPath $recordOutputFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the record-output file target during validation."
    }

    if (Test-Path -LiteralPath $reportPath) {
        Stop-Test "FFmpeg muxer verifier created a verification report after record-output file target rejection."
    }
}

function Test-MuxerVerifierRejectsReportRecordOutputSamePath {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg muxer verifier report/record-output same-path rejection..."

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)
    $sharedPath = Join-Path $AttemptRoot "record-output"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-RecordTestOutputDir", $sharedPath,
            "-VerificationReportPath", $sharedPath) `
        -Description "muxer verifier report/record-output same-path check" `
        -ExpectedText "VerificationReportPath must not be the same path as RecordTestOutputDir"

    if (Test-Path -LiteralPath $sharedPath) {
        Stop-Test "FFmpeg muxer verifier created a shared report/record-output target during validation."
    }
}

function Test-MuxerVerifierRejectsEvidenceTargetParentFilePath {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg muxer verifier evidence-target parent-file rejection..."

    [void][System.IO.Directory]::CreateDirectory($AttemptRoot)

    $reportParentFile = Join-Path $AttemptRoot "report-parent-file"
    [System.IO.File]::WriteAllText($reportParentFile, "not a report parent directory")
    $reportUnderFileParent = Join-Path $reportParentFile "verification-report.txt"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @("-VerificationReportPath", $reportUnderFileParent) `
        -Description "muxer verifier report parent-file target check" `
        -ExpectedText "VerificationReportPath parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $reportParentFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the report parent-file target during validation."
    }

    $reportAncestorFile = Join-Path $AttemptRoot "report-ancestor-file"
    [System.IO.File]::WriteAllText($reportAncestorFile, "not a report ancestor directory")
    $reportUnderFileAncestor = Join-Path $reportAncestorFile "nested\verification-report.txt"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @("-VerificationReportPath", $reportUnderFileAncestor) `
        -Description "muxer verifier report ancestor-file target check" `
        -ExpectedText "VerificationReportPath parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $reportAncestorFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the report ancestor-file target during validation."
    }

    $recordOutputParentFile = Join-Path $AttemptRoot "record-output-parent-file"
    $reportPath = Join-Path $AttemptRoot "verification-report.txt"
    [System.IO.File]::WriteAllText($recordOutputParentFile, "not a record-output parent directory")
    $recordOutputUnderFileParent = Join-Path $recordOutputParentFile "record-output"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-RecordTestOutputDir", $recordOutputUnderFileParent,
            "-VerificationReportPath", $reportPath) `
        -Description "muxer verifier record-output parent-file target check" `
        -ExpectedText "RecordTestOutputDir parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $recordOutputParentFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the record-output parent-file target during validation."
    }

    if (Test-Path -LiteralPath $reportPath) {
        Stop-Test "FFmpeg muxer verifier created a verification report after record-output parent-file rejection."
    }

    $recordOutputAncestorFile = Join-Path $AttemptRoot "record-output-ancestor-file"
    $reportPathAfterAncestorCheck = Join-Path $AttemptRoot "verification-report-ancestor-check.txt"
    [System.IO.File]::WriteAllText($recordOutputAncestorFile, "not a record-output ancestor directory")
    $recordOutputUnderFileAncestor = Join-Path $recordOutputAncestorFile "nested\record-output"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $MuxerVerifier `
        -Arguments @(
            "-RecordTestOutputDir", $recordOutputUnderFileAncestor,
            "-VerificationReportPath", $reportPathAfterAncestorCheck) `
        -Description "muxer verifier record-output ancestor-file target check" `
        -ExpectedText "RecordTestOutputDir parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $recordOutputAncestorFile -PathType Leaf)) {
        Stop-Test "FFmpeg muxer verifier removed the record-output ancestor-file target during validation."
    }

    if (Test-Path -LiteralPath $reportPathAfterAncestorCheck) {
        Stop-Test "FFmpeg muxer verifier created a verification report after record-output ancestor-file rejection."
    }
}

function Test-GateCheckerEvidenceTargetPathValidation {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg gate checker evidence-target path validation..."

    $ffmpegRoot = Join-Path $AttemptRoot "ffmpeg-root"
    New-SyntheticFfmpegRoot -RootPath $ffmpegRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries

    $reportDirectory = Join-Path $AttemptRoot "verification-report-target"
    [void][System.IO.Directory]::CreateDirectory($reportDirectory)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-VerificationReportPath", $reportDirectory) `
        -Description "gate checker report-directory target check" `
        -ExpectedText "VerificationReportPath must name a report file, not a directory"

    $reportDirectoryChildren = Get-ChildItem -LiteralPath $reportDirectory -Force -ErrorAction SilentlyContinue
    if ($reportDirectoryChildren.Count -ne 0) {
        Stop-Test "FFmpeg gate checker wrote files into a verification-report directory target."
    }

    $recordOutputFile = Join-Path $AttemptRoot "record-output-file"
    $reportPath = Join-Path $AttemptRoot "verification-report.txt"
    [System.IO.File]::WriteAllText($recordOutputFile, "not a record-output directory")

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-RecordTestOutputDir", $recordOutputFile,
            "-VerificationReportPath", $reportPath) `
        -Description "gate checker record-output file target check" `
        -ExpectedText "RecordTestOutputDir must name an output directory, not an existing file"

    if (-not (Test-Path -LiteralPath $recordOutputFile -PathType Leaf)) {
        Stop-Test "FFmpeg gate checker removed the record-output file target during validation."
    }

    if (Test-Path -LiteralPath $reportPath) {
        Stop-Test "FFmpeg gate checker created a verification report after record-output file target rejection."
    }

    $sharedPath = Join-Path $AttemptRoot "shared-record-output"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-RecordTestOutputDir", $sharedPath,
            "-VerificationReportPath", $sharedPath) `
        -Description "gate checker report/record-output same-path check" `
        -ExpectedText "VerificationReportPath must not be the same path as RecordTestOutputDir"

    if (Test-Path -LiteralPath $sharedPath) {
        Stop-Test "FFmpeg gate checker created a shared report/record-output target during validation."
    }

    $reportParentFile = Join-Path $AttemptRoot "gate-report-parent-file"
    [System.IO.File]::WriteAllText($reportParentFile, "not a report parent directory")
    $reportUnderFileParent = Join-Path $reportParentFile "verification-report.txt"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-VerificationReportPath", $reportUnderFileParent) `
        -Description "gate checker report parent-file target check" `
        -ExpectedText "VerificationReportPath parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $reportParentFile -PathType Leaf)) {
        Stop-Test "FFmpeg gate checker removed the report parent-file target during validation."
    }

    $reportAncestorFile = Join-Path $AttemptRoot "gate-report-ancestor-file"
    [System.IO.File]::WriteAllText($reportAncestorFile, "not a report ancestor directory")
    $reportUnderFileAncestor = Join-Path $reportAncestorFile "nested\verification-report.txt"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-VerificationReportPath", $reportUnderFileAncestor) `
        -Description "gate checker report ancestor-file target check" `
        -ExpectedText "VerificationReportPath parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $reportAncestorFile -PathType Leaf)) {
        Stop-Test "FFmpeg gate checker removed the report ancestor-file target during validation."
    }

    $recordOutputParentFile = Join-Path $AttemptRoot "gate-record-output-parent-file"
    $reportPathAfterParentCheck = Join-Path $AttemptRoot "verification-report-parent-check.txt"
    [System.IO.File]::WriteAllText($recordOutputParentFile, "not a record-output parent directory")
    $recordOutputUnderFileParent = Join-Path $recordOutputParentFile "record-output"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-RecordTestOutputDir", $recordOutputUnderFileParent,
            "-VerificationReportPath", $reportPathAfterParentCheck) `
        -Description "gate checker record-output parent-file target check" `
        -ExpectedText "RecordTestOutputDir parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $recordOutputParentFile -PathType Leaf)) {
        Stop-Test "FFmpeg gate checker removed the record-output parent-file target during validation."
    }

    if (Test-Path -LiteralPath $reportPathAfterParentCheck) {
        Stop-Test "FFmpeg gate checker created a verification report after record-output parent-file rejection."
    }

    $recordOutputAncestorFile = Join-Path $AttemptRoot "gate-record-output-ancestor-file"
    $reportPathAfterAncestorCheck = Join-Path $AttemptRoot "verification-report-ancestor-check.txt"
    [System.IO.File]::WriteAllText($recordOutputAncestorFile, "not a record-output ancestor directory")
    $recordOutputUnderFileAncestor = Join-Path $recordOutputAncestorFile "nested\record-output"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-RecordTestOutputDir", $recordOutputUnderFileAncestor,
            "-VerificationReportPath", $reportPathAfterAncestorCheck) `
        -Description "gate checker record-output ancestor-file target check" `
        -ExpectedText "RecordTestOutputDir parent must be a directory, not an existing file"

    if (-not (Test-Path -LiteralPath $recordOutputAncestorFile -PathType Leaf)) {
        Stop-Test "FFmpeg gate checker removed the record-output ancestor-file target during validation."
    }

    if (Test-Path -LiteralPath $reportPathAfterAncestorCheck) {
        Stop-Test "FFmpeg gate checker created a verification report after record-output ancestor-file rejection."
    }
}

function Test-GateCheckerReadyRunCommandPreservesEvidenceTargets {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg gate checker ready-run evidence-target command reporting..."

    $ffmpegRoot = Join-Path $AttemptRoot "ffmpeg-root"
    New-SyntheticFfmpegRoot -RootPath $ffmpegRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries

    $recordOutputDir = Join-Path $AttemptRoot "custom record output"
    $reportPath = Join-Path $AttemptRoot "custom reports\mux verification report.txt"
    $recordOutputFullPath = [System.IO.Path]::GetFullPath($recordOutputDir)
    $reportFullPath = [System.IO.Path]::GetFullPath($reportPath)

    $rootOutput = Invoke-ScriptCaptureRequired `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegRoot", $ffmpegRoot,
            "-RecordTestOutputDir", $recordOutputDir,
            "-VerificationReportPath", $reportPath) `
        -Description "gate checker root ready-run evidence-target command reporting"

    Assert-TextContains `
        -Text $rootOutput `
        -Expected "O'Louie FFmpeg muxer gate: Ready for configured muxer verification" `
        -Description "root ready-run output"
    Assert-TextContains `
        -Text $rootOutput `
        -Expected "-RecordTestOutputDir `"$recordOutputFullPath`"" `
        -Description "root ready-run output"
    Assert-TextContains `
        -Text $rootOutput `
        -Expected "-VerificationReportPath `"$reportFullPath`"" `
        -Description "root ready-run output"

    $splitRoot = Join-Path $AttemptRoot "split root"
    New-SyntheticFfmpegRoot -RootPath $splitRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries
    $includeDir = Join-Path $splitRoot "include"
    $libraryDir = Join-Path $splitRoot "lib"
    $binaryDir = Join-Path $splitRoot "bin"
    $splitRecordOutputDir = Join-Path $AttemptRoot "split record output"
    $splitReportPath = Join-Path $AttemptRoot "split reports\mux verification report.txt"
    $splitRecordOutputFullPath = [System.IO.Path]::GetFullPath($splitRecordOutputDir)
    $splitReportFullPath = [System.IO.Path]::GetFullPath($splitReportPath)

    $splitOutput = Invoke-ScriptCaptureRequired `
        -ScriptPath $GateChecker `
        -Arguments @(
            "-FfmpegIncludeDir", $includeDir,
            "-FfmpegLibraryDir", $libraryDir,
            "-FfmpegBinaryDir", $binaryDir,
            "-RecordTestOutputDir", $splitRecordOutputDir,
            "-VerificationReportPath", $splitReportPath) `
        -Description "gate checker split-directory ready-run evidence-target command reporting"

    Assert-TextContains `
        -Text $splitOutput `
        -Expected "O'Louie FFmpeg muxer gate: Ready for configured muxer verification" `
        -Description "split ready-run output"
    Assert-TextContains `
        -Text $splitOutput `
        -Expected "-FfmpegIncludeDir `"$includeDir`"" `
        -Description "split ready-run output"
    Assert-TextContains `
        -Text $splitOutput `
        -Expected "-FfmpegLibraryDir `"$libraryDir`"" `
        -Description "split ready-run output"
    Assert-TextContains `
        -Text $splitOutput `
        -Expected "-FfmpegBinaryDir `"$binaryDir`"" `
        -Description "split ready-run output"
    Assert-TextContains `
        -Text $splitOutput `
        -Expected "-RecordTestOutputDir `"$splitRecordOutputFullPath`"" `
        -Description "split ready-run output"
    Assert-TextContains `
        -Text $splitOutput `
        -Expected "-VerificationReportPath `"$splitReportFullPath`"" `
        -Description "split ready-run output"
}

function Test-GateCheckerWgcOptionValidation {
    Write-Host "Testing FFmpeg gate checker WGC verification option validation..."

    $cases = @(
        [pscustomobject]@{
            Arguments = @("-RunWgcSmoke")
            Description = "WGC smoke gate check without run-verification"
            Expected = "WGC smoke options require -RunVerification."
        },
        [pscustomobject]@{
            Arguments = @("-WgcDurationMs", "3000")
            Description = "WGC duration gate check without run-verification"
            Expected = "WGC smoke options require -RunVerification."
        },
        [pscustomobject]@{
            Arguments = @("-RunVerification", "-WgcDurationMs", "0")
            Description = "zero WGC duration gate run-verification check"
            Expected = "WgcDurationMs must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-RunVerification", "-WgcDurationMs", "-1")
            Description = "negative WGC duration gate run-verification check"
            Expected = "WgcDurationMs must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-RunVerification", "-Width", "0")
            Description = "zero WGC width gate run-verification check"
            Expected = "Width must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-RunVerification", "-Height", "0")
            Description = "zero WGC height gate run-verification check"
            Expected = "Height must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-RunVerification", "-Fps", "0")
            Description = "zero WGC FPS gate run-verification check"
            Expected = "Fps must be a positive integer."
        },
        [pscustomobject]@{
            Arguments = @("-RunVerification", "-BitrateMbps", "0")
            Description = "zero WGC bitrate gate run-verification check"
            Expected = "BitrateMbps must be a positive integer."
        }
    )

    foreach ($case in $cases) {
        Invoke-ScriptExpectFailureContaining `
            -ScriptPath $GateChecker `
            -Arguments $case.Arguments `
            -Description $case.Description `
            -ExpectedText $case.Expected
    }
}

function Test-GateRunVerificationRequiresPreparedInput {
    Write-Host "Testing FFmpeg gate checker run-verification input requirement..."

    $defaultFfmpegRoot = Join-Path $DepsDir "ffmpeg"
    if (Test-Path -LiteralPath $defaultFfmpegRoot -PathType Container) {
        Write-Host "Skipping missing-root run-verification check because _deps\ffmpeg exists."
        return
    }

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-RunVerification") `
        -Description "gate run-verification without prepared FFmpeg input" `
        -ExpectedText "-RunVerification requires a prepared FFmpeg root or explicit split include/lib/bin directories."
}

function Test-FfmpegSourceBuildDescribePlan {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg LGPL source build describe-plan path..."

    $destinationPath = Join-Path $AttemptRoot "ffmpeg"
    $workPath = Join-Path $AttemptRoot "work"
    $provenancePath = Join-Path $AttemptRoot "ffmpeg-source-provenance.md"

    $output = Invoke-ScriptCaptureRequired `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-DescribePlan",
            "-DestinationDir", $destinationPath,
            "-WorkDir", $workPath,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build describe-plan"

    Assert-TextContains `
        -Text $output `
        -Expected "O'Louie FFmpeg LGPL source build plan" `
        -Description "FFmpeg source build plan heading"
    Assert-TextContains `
        -Text $output `
        -Expected "Source URL: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz" `
        -Description "FFmpeg source build plan source URL"
    Assert-TextContains `
        -Text $output `
        -Expected "Signature URL: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz.asc" `
        -Description "FFmpeg source build plan signature URL"
    Assert-TextContains `
        -Text $output `
        -Expected "Automatic binary bundle download: disabled" `
        -Description "FFmpeg source build plan binary-bundle statement"
    Assert-TextContains `
        -Text $output `
        -Expected "Optional explicit tool paths: -TarPath, -GpgPath, -BashPath, -VcVarsAllPath" `
        -Description "FFmpeg source build plan explicit tool path statement"
    Assert-TextContains `
        -Text $output `
        -Expected "MSYS2 bash/make/nasm/cmp" `
        -Description "FFmpeg source build plan MSYS2 tool statement"
    Assert-TextContains `
        -Text $output `
        -Expected "--enable-shared" `
        -Description "FFmpeg source build plan shared-library flag"
    Assert-TextContains `
        -Text $output `
        -Expected "--disable-static" `
        -Description "FFmpeg source build plan static-library flag"
    Assert-TextContains `
        -Text $output `
        -Expected "--disable-gpl" `
        -Description "FFmpeg source build plan GPL flag"
    Assert-TextContains `
        -Text $output `
        -Expected "--disable-nonfree" `
        -Description "FFmpeg source build plan nonfree flag"

    if (Test-Path -LiteralPath $AttemptRoot) {
        Stop-Test "FFmpeg source build describe-plan unexpectedly created files under $AttemptRoot."
    }
}

function Test-FfmpegSourceBuildPrerequisiteCheck {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg LGPL source build prerequisite-check path..."

    $destinationPath = Join-Path $AttemptRoot "ffmpeg"
    $workPath = Join-Path $AttemptRoot "work"
    $provenancePath = Join-Path $AttemptRoot "ffmpeg-source-provenance.md"
    $missingGpgPath = Join-Path $AttemptRoot "missing-gpg.exe"
    $missingBashPath = Join-Path $AttemptRoot "missing-bash.exe"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-CheckPrerequisites",
            "-GpgPath", $missingGpgPath,
            "-BashPath", $missingBashPath,
            "-DestinationDir", $destinationPath,
            "-WorkDir", $workPath,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build prerequisite check with missing explicit tools" `
        -ExpectedText "GPG was not found at explicit path"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-CheckPrerequisites",
            "-BashPath", $missingBashPath,
            "-DestinationDir", $destinationPath,
            "-WorkDir", $workPath,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build prerequisite check with missing bash" `
        -ExpectedText "MSYS2 bash was not found at explicit path"

    if (Test-Path -LiteralPath $AttemptRoot) {
        Stop-Test "FFmpeg source build prerequisite-check unexpectedly created files under $AttemptRoot."
    }
}

function Test-FfmpegSourceBuildGeneratedPathGuards {
    param([Parameter(Mandatory = $true)][string]$AttemptRoot)

    Write-Host "Testing FFmpeg LGPL source build generated path guards..."

    $insideDestination = Join-Path $AttemptRoot "ffmpeg"
    $insideWork = Join-Path $AttemptRoot "work"
    $provenancePath = Join-Path $AttemptRoot "ffmpeg-source-provenance.md"
    $outsideDestination = Join-Path $Root "src\ffmpeg-source-build-path-guard-attempt"
    $outsideWork = Join-Path $Root "src\ffmpeg-source-work-path-guard-attempt"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-DescribePlan",
            "-DestinationDir", $outsideDestination,
            "-WorkDir", $insideWork,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build outside-_deps destination guard" `
        -ExpectedText "DestinationDir must be under the repository _deps directory"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-DescribePlan",
            "-DestinationDir", $insideDestination,
            "-WorkDir", $outsideWork,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build outside-_deps work guard" `
        -ExpectedText "WorkDir must be under the repository _deps directory"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-DescribePlan",
            "-DestinationDir", $DepsDir,
            "-WorkDir", $insideWork,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build _deps root destination guard" `
        -ExpectedText "DestinationDir must be under the repository _deps directory"

    $overlapDestination = Join-Path $AttemptRoot "overlap"
    $overlapWork = Join-Path $overlapDestination "work"
    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $SourceBuildScript `
        -Arguments @(
            "-DescribePlan",
            "-DestinationDir", $overlapDestination,
            "-WorkDir", $overlapWork,
            "-ProvenanceDocPath", $provenancePath) `
        -Description "FFmpeg LGPL source build overlapping generated directory guard" `
        -ExpectedText "DestinationDir and WorkDir must not contain each other"

    foreach ($path in @($AttemptRoot, $outsideDestination, $outsideWork)) {
        if (Test-Path -LiteralPath $path) {
            Stop-Test "FFmpeg source build path guard unexpectedly created $path."
        }
    }
}

function Test-InvalidFfmpegRootFails {
    param([Parameter(Mandatory = $true)][string]$InvalidRoot)

    Write-Host "Testing incomplete FFmpeg root failure paths..."
    New-SyntheticFfmpegRoot -RootPath $InvalidRoot -IncludeHeaders -IncludeLibraries

    Invoke-ScriptExpectFailure `
        -ScriptPath $RootVerifier `
        -Arguments @("-FfmpegRoot", $InvalidRoot) `
        -Description "Incomplete FFmpeg root verification"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $InvalidRoot, "-RequireReady") `
        -Description "Incomplete FFmpeg gate ready check"

    $invalidBuildDir = Join-Path $tempRoot ("cmake-invalid-" + [System.Guid]::NewGuid().ToString("N"))
    $configureArgs = Get-CmakeFfmpegConfigureArgs -FfmpegRoot $InvalidRoot -BuildDir $invalidBuildDir
    Invoke-NativeExpectFailure `
        -FilePath "cmake" `
        -Arguments $configureArgs `
        -Description "Incomplete FFmpeg CMake configure"
}

function Test-Mp4ArtifactInspector {
    param([Parameter(Mandatory = $true)][string]$InspectorRoot)

    Write-Host "Testing MP4 artifact inspector..."
    [void][System.IO.Directory]::CreateDirectory($InspectorRoot)

    $validMp4 = Join-Path $InspectorRoot "valid.mp4"
    Write-SyntheticMp4Artifact -Path $validMp4
    Invoke-ScriptRequired `
        -ScriptPath $Mp4Inspector `
        -Arguments @("-Path", $validMp4) `
        -Description "valid MP4 artifact inspection"

    $missingMoov = Join-Path $InspectorRoot "missing-moov.mp4"
    Write-SyntheticMp4Artifact -Path $missingMoov -MissingMoov
    Invoke-ScriptExpectFailure `
        -ScriptPath $Mp4Inspector `
        -Arguments @("-Path", $missingMoov) `
        -Description "missing-moov MP4 artifact inspection"

    $emptyMdat = Join-Path $InspectorRoot "empty-mdat.mp4"
    Write-SyntheticMp4Artifact -Path $emptyMdat -EmptyMdat
    Invoke-ScriptExpectFailure `
        -ScriptPath $Mp4Inspector `
        -Arguments @("-Path", $emptyMdat) `
        -Description "empty-mdat MP4 artifact inspection"

    $emptyMoov = Join-Path $InspectorRoot "empty-moov.mp4"
    Write-SyntheticMp4Artifact -Path $emptyMoov -EmptyMoov
    Invoke-ScriptExpectFailure `
        -ScriptPath $Mp4Inspector `
        -Arguments @("-Path", $emptyMoov) `
        -Description "empty-moov MP4 artifact inspection"

    $invalidFirstBox = Join-Path $InspectorRoot "invalid-first-box.mp4"
    Write-SyntheticMp4Artifact -Path $invalidFirstBox -InvalidFirstBox
    Invoke-ScriptExpectFailure `
        -ScriptPath $Mp4Inspector `
        -Arguments @("-Path", $invalidFirstBox) `
        -Description "invalid-first-box MP4 artifact inspection"

    $tooSmall = Join-Path $InspectorRoot "too-small.mp4"
    [System.IO.File]::WriteAllBytes($tooSmall, [byte[]]@(0, 1, 2, 3))
    Invoke-ScriptExpectFailure `
        -ScriptPath $Mp4Inspector `
        -Arguments @("-Path", $tooSmall) `
        -Description "too-small MP4 artifact inspection"
}

function Test-GateVerificationReportEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$FfmpegRoot,
        [Parameter(Mandatory = $true)][string]$EvidenceRoot
    )

    Write-Host "Testing FFmpeg gate verification-report evidence..."
    New-SyntheticFfmpegRoot -RootPath $FfmpegRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "missing FFmpeg muxer verification-report evidence check"

    $mp4Path = Join-Path $EvidenceRoot "O'LouieRecordTests\exports\h264.mp4"
    $reportPath = Join-Path $EvidenceRoot "verification-report.txt"
    Write-SyntheticMp4Artifact -Path $mp4Path
    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -OmitSchemaVersion

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "legacy FFmpeg muxer verification-report schema check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -Verifier "tools\UnexpectedVerifier.ps1"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "mismatched FFmpeg muxer verification-report verifier check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Release" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "mismatched FFmpeg muxer verification-report configuration check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path
    [System.IO.File]::AppendAllLines($reportPath, [string[]]@("configuration: Release"), [System.Text.Encoding]::UTF8)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "duplicate-key FFmpeg muxer verification-report check" `
        -ExpectedText "verification report contains duplicate 'configuration'."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path
    [System.IO.File]::AppendAllLines($reportPath, [string[]]@("malformed verification report line"), [System.Text.Encoding]::UTF8)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "malformed-line FFmpeg muxer verification-report check" `
        -ExpectedText "verification report contains malformed line 19."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path
    [System.IO.File]::AppendAllLines($reportPath, [string[]]@("unexpected_report_key: present"), [System.Text.Encoding]::UTF8)

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "unexpected-key FFmpeg muxer verification-report check" `
        -ExpectedText "verification report contains unexpected key 'unexpected_report_key'."

    $backdatedReportUtc = (Get-Item -LiteralPath $mp4Path).LastWriteTimeUtc.AddMinutes(-1).ToString("o")
    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -GeneratedUtc $backdatedReportUtc

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "backdated FFmpeg muxer verification-report timestamp check"

    $futureDatedReportUtc = (Get-Date).ToUniversalTime().AddMinutes(10).ToString("o")
    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -GeneratedUtc $futureDatedReportUtc

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "future-dated FFmpeg muxer verification-report timestamp check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -OmitFfmpegArchiveInput

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "missing FFmpeg muxer verification-report archive-input check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -FfmpegArchiveInput ""

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "empty FFmpeg muxer verification-report archive-input check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmoke "failed"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "invalid FFmpeg muxer verification-report WGC smoke status check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions ""

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "empty FFmpeg muxer verification-report WGC smoke options check" `
        -ExpectedText "verification report 'wgc_smoke_options' is empty."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions "duration_ms=3000 width=1920 height=1080 fps=60"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "missing-option FFmpeg muxer verification-report WGC smoke options check" `
        -ExpectedText "verification report 'wgc_smoke_options' is missing option 'bitrate_mbps'."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions "duration_ms=0 width=1920 height=1080 fps=60 bitrate_mbps=20"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "nonpositive FFmpeg muxer verification-report WGC smoke option check" `
        -ExpectedText "verification report 'wgc_smoke_options' option 'duration_ms' is '0', expected a positive integer."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions "duration_ms=3000 width=1920 height=1080 fps=60 bitrate_mbps=20 color=bt709"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "unexpected-option FFmpeg muxer verification-report WGC smoke options check" `
        -ExpectedText "verification report 'wgc_smoke_options' contains unexpected option 'color'."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions "duration_ms=3000 duration_ms=4000 width=1920 height=1080 fps=60 bitrate_mbps=20"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "duplicate-option FFmpeg muxer verification-report WGC smoke options check" `
        -ExpectedText "verification report 'wgc_smoke_options' contains duplicate option 'duration_ms'."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions "duration_ms=abc width=1920 height=1080 fps=60 bitrate_mbps=20"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "noninteger-option FFmpeg muxer verification-report WGC smoke options check" `
        -ExpectedText "verification report 'wgc_smoke_options' option 'duration_ms' is 'abc', expected a positive integer."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -WgcSmokeOptions "duration_ms=3000 width=1920 height=1080 fps=60 bitrate_mbps"

    Invoke-ScriptExpectFailureContaining `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "malformed-option FFmpeg muxer verification-report WGC smoke options check" `
        -ExpectedText "verification report 'wgc_smoke_options' contains malformed option 'bitrate_mbps'."

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot (Join-Path $EvidenceRoot "different-ffmpeg-root") `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "mismatched FFmpeg muxer verification-report root check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path `
        -VideoMp4ArtifactSizeBytes "1"

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "mismatched FFmpeg muxer verification-report artifact metadata check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegRoot $FfmpegRoot `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path

    Invoke-ScriptRequired `
        -ScriptPath $GateChecker `
        -Arguments @("-FfmpegRoot", $FfmpegRoot, "-RecordTestOutputDir", $EvidenceRoot, "-RequireVerified") `
        -Description "valid root-only FFmpeg muxer verification-report evidence check"
}

function Test-GateSplitDirectoryVerificationReportEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$SplitRoot,
        [Parameter(Mandatory = $true)][string]$EvidenceRoot
    )

    Write-Host "Testing FFmpeg gate split-directory verification-report evidence..."
    New-SyntheticFfmpegRoot -RootPath $SplitRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries

    $includeDir = Join-Path $SplitRoot "include"
    $libraryDir = Join-Path $SplitRoot "lib"
    $binaryDir = Join-Path $SplitRoot "bin"
    $splitArgs = @(
        "-FfmpegIncludeDir", $includeDir,
        "-FfmpegLibraryDir", $libraryDir,
        "-FfmpegBinaryDir", $binaryDir,
        "-RecordTestOutputDir", $EvidenceRoot,
        "-RequireVerified"
    )

    $mp4Path = Join-Path $EvidenceRoot "O'LouieRecordTests\exports\h264.mp4"
    $reportPath = Join-Path $EvidenceRoot "verification-report.txt"
    Write-SyntheticMp4Artifact -Path $mp4Path
    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegIncludeDir (Join-Path $EvidenceRoot "different-include") `
        -FfmpegLibraryDir $libraryDir `
        -FfmpegBinaryDir $binaryDir `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path

    Invoke-ScriptExpectFailure `
        -ScriptPath $GateChecker `
        -Arguments $splitArgs `
        -Description "mismatched FFmpeg muxer verification-report split-directory check"

    Write-SyntheticVerificationReport `
        -Path $reportPath `
        -Configuration "Debug" `
        -FfmpegIncludeDir $includeDir `
        -FfmpegLibraryDir $libraryDir `
        -FfmpegBinaryDir $binaryDir `
        -RecordTestOutputPath $EvidenceRoot `
        -RecordTestMp4 $mp4Path

    Invoke-ScriptRequired `
        -ScriptPath $GateChecker `
        -Arguments $splitArgs `
        -Description "valid FFmpeg muxer verification-report split-directory check"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("olouie_ffmpeg_gate_tools_" + [System.Guid]::NewGuid().ToString("N"))
$combinedDestination = Join-Path $DepsDir "ffmpeg-gate-test-combined"
$splitDestination = Join-Path $DepsDir "ffmpeg-gate-test-split"

try {
    [void][System.IO.Directory]::CreateDirectory($tempRoot)

    $combinedRoot = Join-Path $tempRoot "combined-root"
    New-SyntheticFfmpegRoot -RootPath $combinedRoot -IncludeHeaders -IncludeLibraries -IncludeBinaries
    $combinedZip = New-ZipFromDirectory -Directory $combinedRoot -ZipPath (Join-Path $tempRoot "ffmpeg-combined.zip")

    $devRoot = Join-Path $tempRoot "dev-root"
    New-SyntheticFfmpegRoot -RootPath $devRoot -IncludeHeaders -IncludeLibraries
    $devZip = New-ZipFromDirectory -Directory $devRoot -ZipPath (Join-Path $tempRoot "ffmpeg-dev.zip")

    $sharedRoot = Join-Path $tempRoot "shared-root"
    New-SyntheticFfmpegRoot -RootPath $sharedRoot -IncludeBinaries
    $sharedZip = New-ZipFromDirectory -Directory $sharedRoot -ZipPath (Join-Path $tempRoot "ffmpeg-shared.zip")

    Test-PreparedRoot -Destination $combinedDestination -ArchivePaths @($combinedZip) -Scenario "combined FFmpeg archive"
    Test-PreparedRoot -Destination $splitDestination -ArchivePaths @($devZip, $sharedZip) -Scenario "split FFmpeg archives"
    Test-InvalidArchiveInspectionFails -ArchivePaths @($devZip) -Scenario "incomplete FFmpeg"
    Test-ArchivePreparationDestinationPathValidation `
        -ArchivePath $combinedZip `
        -AttemptRoot (Join-Path $DepsDir "ffmpeg-destination-validation-attempt")
    Test-ExplicitSplitDirs -SplitRoot (Join-Path $tempRoot "explicit-split-root")
    Test-PartialSplitDirsFail -SplitRoot (Join-Path $tempRoot "partial-split-root")
    Test-ArchiveInputConflictsFail -ConflictRoot (Join-Path $tempRoot "archive-conflict-root") -ArchivePath $combinedZip
    Test-MuxerVerifierRejectsArchivePreparationOptionsWithoutArchive `
        -FfmpegRoot (Join-Path $tempRoot "archive-option-root") `
        -AttemptRoot (Join-Path $tempRoot "archive-option-attempt")
    Test-MuxerVerifierWgcOptionValidation
    Test-MuxerVerifierClearsStaleReportOnAttempt `
        -InvalidRoot (Join-Path $tempRoot "stale-report-invalid-root") `
        -AttemptRoot (Join-Path $tempRoot "stale-report-attempt")
    Test-MuxerVerifierArchiveDestinationPathValidation `
        -ArchivePath $combinedZip `
        -AttemptRoot (Join-Path $DepsDir "ffmpeg-muxer-destination-validation-attempt") `
        -OutsideRoot (Join-Path $tempRoot "outside-archive-destination")
    Test-MuxerVerifierRejectsReportDirectoryPath -AttemptRoot (Join-Path $tempRoot "report-directory-target-attempt")
    Test-MuxerVerifierRejectsRecordOutputFilePath -AttemptRoot (Join-Path $tempRoot "record-output-file-target-attempt")
    Test-MuxerVerifierRejectsReportRecordOutputSamePath -AttemptRoot (Join-Path $tempRoot "report-record-output-same-path-attempt")
    Test-MuxerVerifierRejectsEvidenceTargetParentFilePath -AttemptRoot (Join-Path $tempRoot "muxer-evidence-target-parent-file-attempt")
    Test-GateCheckerEvidenceTargetPathValidation -AttemptRoot (Join-Path $tempRoot "gate-evidence-target-path-attempt")
    Test-GateCheckerReadyRunCommandPreservesEvidenceTargets -AttemptRoot (Join-Path $tempRoot "gate-ready-run-command-attempt")
    Test-GateCheckerWgcOptionValidation
    Test-GateRunVerificationRequiresPreparedInput
    Test-FfmpegSourceBuildDescribePlan -AttemptRoot (Join-Path $DepsDir "ffmpeg-source-plan-attempt")
    Test-FfmpegSourceBuildPrerequisiteCheck -AttemptRoot (Join-Path $DepsDir "ffmpeg-source-prereq-attempt")
    Test-FfmpegSourceBuildGeneratedPathGuards -AttemptRoot (Join-Path $DepsDir "ffmpeg-source-path-guard-attempt")
    Test-InvalidFfmpegRootFails -InvalidRoot (Join-Path $tempRoot "invalid-root")
    Test-Mp4ArtifactInspector -InspectorRoot (Join-Path $tempRoot "mp4-inspector")
    Test-GateVerificationReportEvidence `
        -FfmpegRoot (Join-Path $tempRoot "verified-report-root") `
        -EvidenceRoot (Join-Path $tempRoot "verified-report-evidence")
    Test-GateSplitDirectoryVerificationReportEvidence `
        -SplitRoot (Join-Path $tempRoot "verified-split-report-root") `
        -EvidenceRoot (Join-Path $tempRoot "verified-split-report-evidence")

    Write-Host "FFmpeg gate tool self-test succeeded."
    Write-Host "This validates repository tooling, expected file layout, archive inspection, archive preparation destination path validation, archive candidate gate reporting, verifier archive-input failure routing, verifier archive-preparation option requirement validation, verifier WGC/report option validation, muxer verifier stale-report cleanup, muxer verifier archive-destination path validation, muxer verifier report-directory target rejection, muxer verifier record-output file target rejection, muxer verifier report/record-output same-path rejection, muxer verifier evidence-target parent/ancestor-file rejection, gate checker evidence-target path validation, gate checker ready-run evidence-target command reporting, gate checker WGC run-verification option validation, gate run-verification input requirement reporting, FFmpeg source build dry-plan, prerequisite reporting, explicit tool path reporting, and generated path guards, MP4 artifact box/payload inspection, verification-report evidence recognition, verification-report schema/provenance/configuration/timestamp/future-timestamp/archive/duplicate-key/malformed-line/unexpected-key/WGC-smoke option-schema detail checks, root/split FFmpeg-input consistency checks, partial split-directory rejection, gate-status and muxer-verifier archive/root input conflict rejection, gate archive run-verification conflict reporting, archive-input field presence/empty/root-only semantics, artifact metadata consistency checks, root and split-directory FFmpeg option wiring, and explicit failure behavior for incomplete roots; it does not validate real FFmpeg binaries, linkability, licensing, source download, source signature verification, source build, or MP4 muxing."
} finally {
    Remove-RepoDirectoryIfExists -Path $combinedDestination -Description "combined test destination"
    Remove-RepoDirectoryIfExists -Path $splitDestination -Description "split test destination"
    Remove-RepoDirectoryIfExists -Path (Join-Path $DepsDir "ffmpeg-muxer-destination-validation-attempt") -Description "muxer destination validation attempt"
    Remove-RepoDirectoryIfExists -Path (Join-Path $DepsDir "ffmpeg-source-plan-attempt") -Description "source build plan attempt"
    Remove-RepoDirectoryIfExists -Path (Join-Path $DepsDir "ffmpeg-source-prereq-attempt") -Description "source build prerequisite attempt"
    Remove-RepoDirectoryIfExists -Path (Join-Path $DepsDir "ffmpeg-source-path-guard-attempt") -Description "source build path guard attempt"

    if (Test-Path -LiteralPath $DepsDir) {
        $remaining = Get-ChildItem -LiteralPath $DepsDir -Force -ErrorAction SilentlyContinue
        if ($remaining.Count -eq 0) {
            [void](Assert-PathInsideRoot -Path $DepsDir -Description "test dependency directory")
            Remove-Item -LiteralPath $DepsDir -Force
        }
    }

    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
