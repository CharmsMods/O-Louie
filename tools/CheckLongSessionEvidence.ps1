[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [string]$FfprobePath,
    [switch]$RequireCompletedDuration
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "LongSessionEvidence.ps1")

$result = Get-LongSessionEvidenceStatus -EvidenceDirectory $EvidenceDirectory -FfprobePath $FfprobePath
Write-Host "O'Louie long-session evidence: $($result.Status)"
Write-Host "  Evidence: $($result.EvidenceDirectory)"
Write-Host "  Artifacts: $($result.ArtifactCount)"
if ($result.ReinspectedArtifactCount -gt 0) {
    Write-Host "  Freshly reinspected artifacts: $($result.ReinspectedArtifactCount)"
}
Write-Host "  Resource span: $($result.ResourceSpanSeconds) seconds"
if ($null -ne $result.ResourceTrend) {
    Write-Host ("  Resource growth: working={0} bytes private={1} bytes handles={2} threads={3}" -f
        $result.ResourceTrend.WorkingSetGrowthBytes,
        $result.ResourceTrend.PrivateBytesGrowth,
        $result.ResourceTrend.HandleCountGrowth,
        $result.ResourceTrend.ThreadCountGrowth)
}

foreach ($warning in $result.Warnings) {
    Write-Warning $warning
}
foreach ($errorMessage in $result.Errors) {
    Write-Error $errorMessage -ErrorAction Continue
}
if (-not $result.EvidenceValid) {
    exit 1
}

if ($result.DurationRequirementSatisfied) {
    Write-Host "  Duration gate: verified"
    exit 0
}

Write-Host "  Duration gate: pending"
foreach ($issue in $result.DurationIssues) {
    Write-Host "    - $issue"
}
if ($RequireCompletedDuration) {
    exit 1
}
exit 0
