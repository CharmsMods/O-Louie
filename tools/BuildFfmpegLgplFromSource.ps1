[CmdletBinding()]
param(
    [ValidatePattern("^[0-9]+(\.[0-9]+){1,2}$")]
    [string]$Version = "8.1.2",

    [string]$SourceUrl = "",

    [string]$SignatureUrl = "",

    [string]$DestinationDir = "",

    [string]$WorkDir = "",

    [string]$TarPath = "",

    [string]$GpgPath = "",

    [string]$BashPath = "",

    [string]$VcVarsAllPath = "",

    [string]$ProvenanceDocPath = "",

    [int]$Jobs = 0,

    [switch]$Force,

    [switch]$DescribePlan,

    [switch]$CheckPrerequisites
)

$ErrorActionPreference = "Stop"

$ToolsDir = $PSScriptRoot
$Root = Split-Path -Parent $ToolsDir
$DepsDir = Join-Path $Root "_deps"
$RootVerifier = Join-Path $ToolsDir "VerifyFfmpegRoot.ps1"
$FfmpegReleaseKeyUrl = "https://ffmpeg.org/ffmpeg-devel.asc"
$FfmpegReleaseKeyFingerprint = "FCF986EA15E6E293A5644F10B4322F04D67658D8"

function Stop-SourceBuild {
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
        Stop-SourceBuild "$Description must be inside the repository root: $targetPath"
    }

    return $targetPath
}

function Assert-GeneratedPathUnderDeps {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $depsPath = Get-NormalizedPath -Path $DepsDir
    $targetPath = Get-NormalizedPath -Path $Path
    $depsPrefix = $depsPath + [System.IO.Path]::DirectorySeparatorChar

    if (-not $targetPath.StartsWith($depsPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Stop-SourceBuild "$Description must be under the repository _deps directory: $targetPath"
    }

    return $targetPath
}

function Test-SameOrDescendantPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Ancestor
    )

    $targetPath = Get-NormalizedPath -Path $Path
    $ancestorPath = Get-NormalizedPath -Path $Ancestor
    if ([System.String]::Equals($targetPath, $ancestorPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }

    $ancestorPrefix = $ancestorPath + [System.IO.Path]::DirectorySeparatorChar
    return $targetPath.StartsWith($ancestorPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SeparateGeneratedDirectories {
    param(
        [Parameter(Mandatory = $true)][string]$FirstPath,
        [Parameter(Mandatory = $true)][string]$FirstName,
        [Parameter(Mandatory = $true)][string]$SecondPath,
        [Parameter(Mandatory = $true)][string]$SecondName
    )

    if (Test-SameOrDescendantPath -Path $FirstPath -Ancestor $SecondPath) {
        Stop-SourceBuild "$FirstName and $SecondName must not contain each other: $FirstPath, $SecondPath"
    }

    if (Test-SameOrDescendantPath -Path $SecondPath -Ancestor $FirstPath) {
        Stop-SourceBuild "$FirstName and $SecondName must not contain each other: $FirstPath, $SecondPath"
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
            Stop-SourceBuild "$Name parent must be a directory, not an existing file: $parentPath"
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

function Assert-DirectoryTarget {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        Stop-SourceBuild "$Name must name a directory, not an existing file: $Path"
    }

    Assert-ParentDirectoryPath -Path $Path -Name $Name
}

function Assert-FileTarget {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ([string]::IsNullOrWhiteSpace([System.IO.Path]::GetFileName($Path))) {
        Stop-SourceBuild "$Name must name a file, not a directory: $Path"
    }

    if (Test-Path -LiteralPath $Path -PathType Container) {
        Stop-SourceBuild "$Name must name a file, not an existing directory: $Path"
    }

    Assert-ParentDirectoryPath -Path $Path -Name $Name
}

function Get-DefaultSourceUrl {
    param([Parameter(Mandatory = $true)][string]$RequestedVersion)

    return "https://ffmpeg.org/releases/ffmpeg-$RequestedVersion.tar.xz"
}

function Get-DefaultSignatureUrl {
    param([Parameter(Mandatory = $true)][string]$RequestedVersion)

    return "https://ffmpeg.org/releases/ffmpeg-$RequestedVersion.tar.xz.asc"
}

function Assert-OfficialReleaseUrl {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$ExpectedPath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    try {
        $uri = [System.Uri]$Url
    } catch {
        Stop-SourceBuild "$Name must be a valid absolute URL: $Url"
    }

    if (-not $uri.IsAbsoluteUri -or $uri.Scheme -ne "https") {
        Stop-SourceBuild "$Name must be an https URL: $Url"
    }

    if ($uri.Host -ne "ffmpeg.org" -and $uri.Host -ne "www.ffmpeg.org") {
        Stop-SourceBuild "$Name must use the official ffmpeg.org host: $Url"
    }

    if ($uri.AbsolutePath -ne $ExpectedPath) {
        Stop-SourceBuild "$Name must point to the pinned FFmpeg $Version release path '$ExpectedPath': $Url"
    }
}

function Resolve-ToolPath {
    param(
        [string]$Path,
        [Parameter(Mandatory = $true)][string]$CommandName,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        try {
            $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
        } catch {
            Stop-SourceBuild "$Description was not found: $Path"
        }

        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            Stop-SourceBuild "$Description must be a file: $resolved"
        }

        return $resolved
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        Stop-SourceBuild "$Description was not found on PATH."
    }

    return $command.Source
}

function New-PrerequisiteResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Available,
        [Parameter(Mandatory = $true)][string]$Detail,
        [string]$Path = ""
    )

    return [pscustomobject]@{
        Name = $Name
        Available = $Available
        Detail = $Detail
        Path = $Path
    }
}

function Test-ToolAvailable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ExplicitPath,
        [Parameter(Mandatory = $true)][string]$CommandName,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        try {
            $resolved = (Resolve-Path -LiteralPath $ExplicitPath -ErrorAction Stop).Path
        } catch {
            return New-PrerequisiteResult `
                -Name $Name `
                -Available $false `
                -Detail "$Description was not found at explicit path: $ExplicitPath"
        }

        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            return New-PrerequisiteResult `
                -Name $Name `
                -Available $false `
                -Detail "$Description explicit path is not a file: $resolved"
        }

        return New-PrerequisiteResult `
            -Name $Name `
            -Available $true `
            -Detail "$Description found at explicit path." `
            -Path $resolved
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        return New-PrerequisiteResult `
            -Name $Name `
            -Available $false `
            -Detail "$Description was not found on PATH."
    }

    return New-PrerequisiteResult `
        -Name $Name `
        -Available $true `
        -Detail "$Description found on PATH." `
        -Path $command.Source
}

function Test-BashCommandAvailable {
    param(
        [Parameter(Mandatory = $true)][string]$BashPath,
        [Parameter(Mandatory = $true)][string]$CommandName,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $outputLines = & $BashPath -lc "command -v $CommandName" 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $output = ($outputLines | Out-String).Trim()
    if ($exitCode -ne 0 -or [string]::IsNullOrWhiteSpace($output)) {
        return New-PrerequisiteResult `
            -Name $CommandName `
            -Available $false `
            -Detail "$Description was not found inside the selected bash environment."
    }

    return New-PrerequisiteResult `
        -Name $CommandName `
        -Available $true `
        -Detail "$Description found inside the selected bash environment." `
        -Path $output
}

function Test-MsvcToolsAvailable {
    if (-not [string]::IsNullOrWhiteSpace($VcVarsAllPath)) {
        try {
            $resolved = (Resolve-Path -LiteralPath $VcVarsAllPath -ErrorAction Stop).Path
        } catch {
            return New-PrerequisiteResult `
                -Name "MSVC x64 tools" `
                -Available $false `
                -Detail "VcVarsAllPath was not found: $VcVarsAllPath"
        }

        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            return New-PrerequisiteResult `
                -Name "MSVC x64 tools" `
                -Available $false `
                -Detail "VcVarsAllPath is not a file: $resolved"
        }

        return New-PrerequisiteResult `
            -Name "MSVC x64 tools" `
            -Available $true `
            -Detail "vcvarsall.bat was found at explicit path." `
            -Path $resolved
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($null -ne $cl) {
        return New-PrerequisiteResult `
            -Name "MSVC x64 tools" `
            -Available $true `
            -Detail "cl.exe was found on PATH." `
            -Path $cl.Source
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $installPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($installPath)) {
            $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return New-PrerequisiteResult `
                    -Name "MSVC x64 tools" `
                    -Available $true `
                    -Detail "vcvarsall.bat was found through vswhere." `
                    -Path $candidate
            }
        }
    }

    return New-PrerequisiteResult `
        -Name "MSVC x64 tools" `
        -Available $false `
        -Detail "MSVC build tools were not found. Run from a Visual Studio Developer PowerShell, or pass -VcVarsAllPath."
}

function Move-MsvcImportLibrariesToLibraryDir {
    param([Parameter(Mandatory = $true)][string]$DestinationPath)

    $binaryDir = Join-Path $DestinationPath "bin"
    $libraryDir = Join-Path $DestinationPath "lib"
    [void][System.IO.Directory]::CreateDirectory($libraryDir)

    Write-Host "Normalizing FFmpeg MSVC import-library layout..."
    foreach ($name in @("avformat.lib", "avcodec.lib", "avutil.lib", "swresample.lib")) {
        $sourcePath = Join-Path $binaryDir $name
        $targetPath = Join-Path $libraryDir $name
        if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
            Write-Host "  $targetPath"
            continue
        }

        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            Stop-SourceBuild "Built MSVC import library was not found in bin or lib: $name"
        }

        Move-Item -LiteralPath $sourcePath -Destination $targetPath
        Write-Host "  $targetPath"
    }
}

function Find-VcVarsAll {
    if (-not [string]::IsNullOrWhiteSpace($VcVarsAllPath)) {
        try {
            $resolved = (Resolve-Path -LiteralPath $VcVarsAllPath -ErrorAction Stop).Path
        } catch {
            Stop-SourceBuild "VcVarsAllPath was not found: $VcVarsAllPath"
        }

        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            Stop-SourceBuild "VcVarsAllPath must be a file: $resolved"
        }

        return $resolved
    }

    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($null -ne $cl) {
        return ""
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
        $installPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($installPath)) {
            $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    Stop-SourceBuild "MSVC build tools were not found. Run from a Visual Studio Developer PowerShell, or pass -VcVarsAllPath."
}

function ConvertTo-MsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path).Replace("\", "/")
    if ($fullPath -match "^([A-Za-z]):/(.*)$") {
        return "/" + $matches[1].ToLowerInvariant() + "/" + $matches[2]
    }

    Stop-SourceBuild "Could not convert path to an MSYS path: $Path"
}

function Quote-Bash {
    param([Parameter(Mandatory = $true)][string]$Value)

    return "'" + $Value.Replace("'", "'\''") + "'"
}

function Get-FfmpegConfigureFlags {
    param([Parameter(Mandatory = $true)][string]$InstallPrefix)

    return @(
        "--prefix=$InstallPrefix",
        "--toolchain=msvc",
        "--arch=x86_64",
        "--target-os=win64",
        "--enable-shared",
        "--disable-static",
        "--disable-programs",
        "--disable-doc",
        "--disable-debug",
        "--disable-network",
        "--disable-autodetect",
        "--disable-gpl",
        "--disable-nonfree",
        "--disable-version3",
        "--disable-avdevice",
        "--disable-avfilter",
        "--disable-swscale",
        "--disable-encoders",
        "--disable-decoders",
        "--disable-hwaccels",
        "--disable-devices",
        "--disable-filters",
        "--enable-avcodec",
        "--enable-avformat",
        "--enable-avutil",
        "--enable-swresample"
    )
}

function Write-Plan {
    param(
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$WorkPath,
        [Parameter(Mandatory = $true)][string]$ProvenancePath,
        [Parameter(Mandatory = $true)][string[]]$ConfigureFlags
    )

    Write-Host "O'Louie FFmpeg LGPL source build plan"
    Write-Host "  Version: $Version"
    Write-Host "  Source URL: $SourceUrl"
    Write-Host "  Signature URL: $SignatureUrl"
    Write-Host "  Release key URL: $FfmpegReleaseKeyUrl"
    Write-Host "  Destination: $DestinationPath"
    Write-Host "  Work directory: $WorkPath"
    Write-Host "  Provenance doc: $ProvenancePath"
    Write-Host "  Automatic binary bundle download: disabled"
    Write-Host "  Required local tools: gpg, tar, MSYS2 bash/make/nasm/cmp, MSVC x64 tools"
    Write-Host "  Optional explicit tool paths: -TarPath, -GpgPath, -BashPath, -VcVarsAllPath"
    Write-Host "  Configure flags:"
    foreach ($flag in $ConfigureFlags) {
        Write-Host "    $flag"
    }
}

function Write-PrerequisiteCheck {
    param(
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$WorkPath,
        [Parameter(Mandatory = $true)][string]$ProvenancePath,
        [Parameter(Mandatory = $true)][string[]]$ConfigureFlags
    )

    $checks = [System.Collections.Generic.List[object]]::new()
    $checks.Add((Test-ToolAvailable `
                -Name "tar" `
                -ExplicitPath $TarPath `
                -CommandName "tar.exe" `
                -Description "tar"))
    $checks.Add((Test-ToolAvailable `
                -Name "GPG" `
                -ExplicitPath $GpgPath `
                -CommandName "gpg.exe" `
                -Description "GPG"))
    $bashCheck = Test-ToolAvailable `
        -Name "MSYS2 bash" `
        -ExplicitPath $BashPath `
        -CommandName "bash.exe" `
        -Description "MSYS2 bash"
    $checks.Add($bashCheck)
    if ($bashCheck.Available) {
        $checks.Add((Test-BashCommandAvailable `
                    -BashPath $bashCheck.Path `
                    -CommandName "make" `
                    -Description "MSYS2 make"))
        $checks.Add((Test-BashCommandAvailable `
                    -BashPath $bashCheck.Path `
                    -CommandName "nasm" `
                    -Description "NASM"))
        $checks.Add((Test-BashCommandAvailable `
                    -BashPath $bashCheck.Path `
                    -CommandName "cmp" `
                    -Description "MSYS2 cmp (diffutils)"))
    } else {
        $checks.Add((New-PrerequisiteResult `
                    -Name "make" `
                    -Available $false `
                    -Detail "MSYS2 make could not be checked because bash is missing."))
        $checks.Add((New-PrerequisiteResult `
                    -Name "nasm" `
                    -Available $false `
                    -Detail "NASM could not be checked because bash is missing."))
        $checks.Add((New-PrerequisiteResult `
                    -Name "cmp" `
                    -Available $false `
                    -Detail "MSYS2 cmp could not be checked because bash is missing."))
    }
    $checks.Add((Test-MsvcToolsAvailable))

    Write-Plan `
        -DestinationPath $DestinationPath `
        -WorkPath $WorkPath `
        -ProvenancePath $ProvenancePath `
        -ConfigureFlags $ConfigureFlags
    Write-Host "  Prerequisites:"

    $missingCount = 0
    foreach ($check in $checks) {
        $status = "ok"
        if (-not $check.Available) {
            $status = "missing"
            $missingCount += 1
        }

        Write-Host "    [$status] $($check.Name): $($check.Detail)"
        if ($check.Available -and -not [string]::IsNullOrWhiteSpace($check.Path)) {
            Write-Host "      $($check.Path)"
        }
    }

    if ($missingCount -gt 0) {
        Write-Host "  Result: missing prerequisites. No download, extraction, build, or file write was attempted."
        exit 1
    }

    Write-Host "  Result: prerequisites available. No download, extraction, build, or file write was attempted."
    exit 0
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Description
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        Stop-SourceBuild "$Description failed with exit code $LASTEXITCODE."
    }
}

function Invoke-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    Write-Host "Downloading:"
    Write-Host "  $Url"
    Invoke-WebRequest -Uri $Url -OutFile $OutputPath
}

function Write-ProvenanceDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$ArchiveSha256,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string[]]$ConfigureFlags
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [void][System.IO.Directory]::CreateDirectory($parent)
    }

    $lines = @(
        "# Local FFmpeg Source Build Provenance",
        "",
        "Generated UTC: $((Get-Date).ToUniversalTime().ToString("o"))",
        "",
        "This file records a local development-only FFmpeg build for O'Louie. It is not a bundling, redistribution, or packaging record.",
        "",
        "- FFmpeg version: $Version",
        "- Source URL: $SourceUrl",
        "- Signature URL: $SignatureUrl",
        "- Release key URL: $FfmpegReleaseKeyUrl",
        "- Release key fingerprint: $FfmpegReleaseKeyFingerprint",
        "- Source archive SHA-256: $ArchiveSha256",
        "- Verified with GPG signature: yes",
        "- Destination root: $DestinationPath",
        "- Source archive path: $ArchivePath",
        "- Linkage direction: dynamic shared libraries",
        "- License posture: LGPL-oriented, with GPL, nonfree, and version3 components disabled by configure flags",
        "",
        "Configure flags:",
        ""
    )

    foreach ($flag in $ConfigureFlags) {
        $lines += "- ``$flag``"
    }

    [System.IO.File]::WriteAllLines($Path, [string[]]$lines, [System.Text.Encoding]::UTF8)
}

if ([string]::IsNullOrWhiteSpace($DestinationDir)) {
    $DestinationDir = Join-Path $Root "_deps\ffmpeg"
}
if ([string]::IsNullOrWhiteSpace($WorkDir)) {
    $WorkDir = Join-Path $Root "_deps\ffmpeg-source-build"
}
if ([string]::IsNullOrWhiteSpace($ProvenanceDocPath)) {
    $ProvenanceDocPath = Join-Path $Root "docs\ffmpeg_source_build_provenance.md"
}
if ([string]::IsNullOrWhiteSpace($SourceUrl)) {
    $SourceUrl = Get-DefaultSourceUrl -RequestedVersion $Version
}
if ([string]::IsNullOrWhiteSpace($SignatureUrl)) {
    $SignatureUrl = Get-DefaultSignatureUrl -RequestedVersion $Version
}
if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
}

Assert-OfficialReleaseUrl -Url $SourceUrl -ExpectedPath "/releases/ffmpeg-$Version.tar.xz" -Name "SourceUrl"
Assert-OfficialReleaseUrl -Url $SignatureUrl -ExpectedPath "/releases/ffmpeg-$Version.tar.xz.asc" -Name "SignatureUrl"

$destinationPath = Assert-PathInsideRoot -Path $DestinationDir -Description "DestinationDir"
$workPath = Assert-PathInsideRoot -Path $WorkDir -Description "WorkDir"
$provenancePath = Assert-PathInsideRoot -Path $ProvenanceDocPath -Description "ProvenanceDocPath"
[void](Assert-GeneratedPathUnderDeps -Path $destinationPath -Description "DestinationDir")
[void](Assert-GeneratedPathUnderDeps -Path $workPath -Description "WorkDir")
Assert-DirectoryTarget -Path $destinationPath -Name "DestinationDir"
Assert-DirectoryTarget -Path $workPath -Name "WorkDir"
Assert-FileTarget -Path $provenancePath -Name "ProvenanceDocPath"
Assert-SeparateGeneratedDirectories `
    -FirstPath $destinationPath `
    -FirstName "DestinationDir" `
    -SecondPath $workPath `
    -SecondName "WorkDir"

$installPrefix = ConvertTo-MsysPath -Path $destinationPath
$configureFlags = Get-FfmpegConfigureFlags -InstallPrefix $installPrefix

if ($DescribePlan) {
    Write-Plan `
        -DestinationPath $destinationPath `
        -WorkPath $workPath `
        -ProvenancePath $provenancePath `
        -ConfigureFlags $configureFlags
    exit 0
}

if ($CheckPrerequisites) {
    Write-PrerequisiteCheck `
        -DestinationPath $destinationPath `
        -WorkPath $workPath `
        -ProvenancePath $provenancePath `
        -ConfigureFlags $configureFlags
}

$tarPath = Resolve-ToolPath -Path $TarPath -CommandName "tar.exe" -Description "tar"
$gpgPath = Resolve-ToolPath -Path $GpgPath -CommandName "gpg.exe" -Description "GPG"
$bashResolvedPath = Resolve-ToolPath -Path $BashPath -CommandName "bash.exe" -Description "MSYS2 bash"
$vcVarsPath = Find-VcVarsAll

if (Test-Path -LiteralPath $destinationPath) {
    if (-not $Force) {
        Stop-SourceBuild "Destination already exists: $destinationPath. Re-run with -Force to replace it."
    }

    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}

if (Test-Path -LiteralPath $workPath) {
    if (-not $Force) {
        Stop-SourceBuild "WorkDir already exists: $workPath. Re-run with -Force to replace it."
    }

    Remove-Item -LiteralPath $workPath -Recurse -Force
}

[void][System.IO.Directory]::CreateDirectory($workPath)
$downloadDir = Join-Path $workPath "download"
$extractDir = Join-Path $workPath "source"
[void][System.IO.Directory]::CreateDirectory($downloadDir)
[void][System.IO.Directory]::CreateDirectory($extractDir)

$sourceArchivePath = Join-Path $downloadDir ([System.IO.Path]::GetFileName(([System.Uri]$SourceUrl).AbsolutePath))
$signaturePath = Join-Path $downloadDir ([System.IO.Path]::GetFileName(([System.Uri]$SignatureUrl).AbsolutePath))

Invoke-VerifiedDownload -Url $SourceUrl -OutputPath $sourceArchivePath
Invoke-VerifiedDownload -Url $SignatureUrl -OutputPath $signaturePath

Write-Host "Verifying FFmpeg source archive signature with GPG..."
Write-Host "  If this fails because the FFmpeg release key is missing, import it deliberately from $FfmpegReleaseKeyUrl and rerun."
Invoke-Native -FilePath $gpgPath -Arguments @("--verify", $signaturePath, $sourceArchivePath) -Description "FFmpeg source signature verification"

$archiveHash = (Get-FileHash -LiteralPath $sourceArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "FFmpeg source archive SHA-256:"
Write-Host "  $archiveHash"

Write-Host "Extracting FFmpeg source archive..."
Invoke-Native -FilePath $tarPath -Arguments @("-xf", $sourceArchivePath, "-C", $extractDir) -Description "FFmpeg source extraction"

$sourceDir = Join-Path $extractDir "ffmpeg-$Version"
if (-not (Test-Path -LiteralPath $sourceDir -PathType Container)) {
    Stop-SourceBuild "Expected extracted FFmpeg source directory was not found: $sourceDir"
}

$msysSourceDir = ConvertTo-MsysPath -Path $sourceDir
$msysBuildScript = ConvertTo-MsysPath -Path (Join-Path $workPath "build-ffmpeg-lgpl.sh")
$configureCommand = "./configure " + (($configureFlags | ForEach-Object { Quote-Bash -Value $_ }) -join " ")
$buildScriptPath = Join-Path $workPath "build-ffmpeg-lgpl.sh"
$buildScriptLines = @(
    "set -euo pipefail",
    "cd $(Quote-Bash -Value $msysSourceDir)",
    "command -v make >/dev/null || { echo 'MSYS2 make is required.' >&2; exit 1; }",
    "command -v nasm >/dev/null || { echo 'NASM is required.' >&2; exit 1; }",
    "command -v cmp >/dev/null || { echo 'MSYS2 cmp (diffutils) is required.' >&2; exit 1; }",
    $configureCommand,
    "make -j$Jobs",
    "make install"
)
[System.IO.File]::WriteAllLines($buildScriptPath, [string[]]$buildScriptLines, [System.Text.Encoding]::ASCII)

Write-Host "Building FFmpeg LGPL shared libraries from source..."
$bashCommand = 'export PATH=/usr/local/bin:/usr/bin:/bin:$PATH; exec ' + (Quote-Bash -Value $msysBuildScript)
if ([string]::IsNullOrWhiteSpace($vcVarsPath)) {
    Invoke-Native -FilePath $bashResolvedPath -Arguments @("-c", $bashCommand) -Description "FFmpeg source build"
} else {
    $cmdLine = "call `"$vcVarsPath`" x64 && `"$bashResolvedPath`" -c `"$bashCommand`""
    Invoke-Native -FilePath "cmd.exe" -Arguments @("/d", "/c", $cmdLine) -Description "FFmpeg source build"
}

Move-MsvcImportLibrariesToLibraryDir -DestinationPath $destinationPath

Write-Host "Verifying locally built FFmpeg root..."
Invoke-Native -FilePath "powershell.exe" -Arguments @(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $RootVerifier,
    "-FfmpegRoot",
    $destinationPath) -Description "FFmpeg root verification"

Write-ProvenanceDocument `
    -Path $provenancePath `
    -ArchivePath $sourceArchivePath `
    -ArchiveSha256 $archiveHash `
    -DestinationPath $destinationPath `
    -ConfigureFlags $configureFlags

Write-Host "FFmpeg LGPL source build completed."
Write-Host "  FFmpeg root: $destinationPath"
Write-Host "  Provenance doc: $provenancePath"
Write-Host "Next verification command:"
Write-Host "  .\tools\VerifyFfmpegMuxer.ps1 -Configuration Debug -FfmpegRoot `"$destinationPath`""
