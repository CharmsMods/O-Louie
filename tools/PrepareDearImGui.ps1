[CmdletBinding()]
param(
    [string]$DestinationDir,
    [string]$ArchivePath,
    [switch]$Force,
    [switch]$CheckExisting,
    [switch]$DescribePlan
)

$ErrorActionPreference = "Stop"

$Version = "1.92.8"
$Tag = "v$Version"
$SourceUrl = "https://github.com/ocornut/imgui/archive/refs/tags/$Tag.zip"
$ExpectedSha256 = "27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870"
$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$DepsRoot = [System.IO.Path]::GetFullPath((Join-Path $Root "_deps"))

if ([string]::IsNullOrWhiteSpace($DestinationDir)) {
    $DestinationDir = Join-Path $DepsRoot "imgui"
}
$DestinationDir = [System.IO.Path]::GetFullPath($DestinationDir)

function Assert-DependencyPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $prefix = $DepsRoot.TrimEnd('\') + '\'
    if (-not $Path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description must be a child of $DepsRoot."
    }
}

function Test-DearImGuiRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $required = @(
        "imgui.h",
        "imgui.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
        "backends\imgui_impl_win32.h",
        "backends\imgui_impl_win32.cpp",
        "backends\imgui_impl_dx11.h",
        "backends\imgui_impl_dx11.cpp",
        "LICENSE.txt"
    )
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relative) -PathType Leaf)) {
            throw "Dear ImGui root is missing $relative under $Path."
        }
    }

    $versionLine = Select-String -LiteralPath (Join-Path $Path "imgui.h") -Pattern '^#define IMGUI_VERSION\s+"([^\"]+)"$' | Select-Object -First 1
    if ($null -eq $versionLine -or $versionLine.Matches[0].Groups[1].Value -ne $Version) {
        throw "Dear ImGui root does not report the pinned version $Version."
    }
}

if ($DescribePlan) {
    Write-Output "Dear ImGui preparation plan"
    Write-Output "  Version:     $Version"
    Write-Output "  Source:      $SourceUrl"
    Write-Output "  SHA-256:     $ExpectedSha256"
    Write-Output "  Destination: $DestinationDir"
    exit 0
}

if ($CheckExisting) {
    Test-DearImGuiRoot -Path $DestinationDir
    Write-Output "Dear ImGui $Version is ready at $DestinationDir."
    exit 0
}

Assert-DependencyPath -Path $DestinationDir -Description "Dear ImGui destination"

if ((Test-Path -LiteralPath $DestinationDir) -and -not $Force) {
    Test-DearImGuiRoot -Path $DestinationDir
    Write-Output "Dear ImGui $Version is already ready at $DestinationDir."
    exit 0
}

New-Item -ItemType Directory -Path $DepsRoot -Force | Out-Null

$downloadedArchive = $false
if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
    $ArchivePath = Join-Path $DepsRoot "imgui-$Tag.zip"
    Invoke-WebRequest -UseBasicParsing -Uri $SourceUrl -OutFile $ArchivePath
    $downloadedArchive = $true
}
$ArchivePath = [System.IO.Path]::GetFullPath($ArchivePath)

if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
    throw "Dear ImGui archive was not found: $ArchivePath"
}

$actualHash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $ExpectedSha256) {
    throw "Dear ImGui archive SHA-256 mismatch. Expected $ExpectedSha256 but found $actualHash."
}

$staging = Join-Path $DepsRoot ("imgui-staging-" + [guid]::NewGuid().ToString("N"))
Assert-DependencyPath -Path $staging -Description "Dear ImGui staging directory"

try {
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $staging
    $source = Join-Path $staging "imgui-$Version"
    Test-DearImGuiRoot -Path $source

    if (Test-Path -LiteralPath $DestinationDir) {
        Assert-DependencyPath -Path $DestinationDir -Description "Dear ImGui replacement destination"
        Remove-Item -LiteralPath $DestinationDir -Recurse -Force
    }
    Move-Item -LiteralPath $source -Destination $DestinationDir
    Test-DearImGuiRoot -Path $DestinationDir
} finally {
    if (Test-Path -LiteralPath $staging) {
        Assert-DependencyPath -Path $staging -Description "Dear ImGui staging cleanup"
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}

Write-Output "Dear ImGui $Version prepared successfully."
Write-Output "  Source:      $SourceUrl"
Write-Output "  SHA-256:     $actualHash"
Write-Output "  Destination: $DestinationDir"
if ($downloadedArchive) {
    Write-Output "  Archive:     $ArchivePath"
}
