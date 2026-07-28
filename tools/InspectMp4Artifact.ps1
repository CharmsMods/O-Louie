[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [int64]$MinimumSizeBytes = 32
)

$ErrorActionPreference = "Stop"

function Stop-Inspection {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Error $Message
    exit 1
}

function Read-RequiredBytes {
    param(
        [Parameter(Mandatory = $true)][System.IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)][int]$Count,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $buffer = New-Object byte[] $Count
    $read = $Stream.Read($buffer, 0, $Count)
    if ($read -ne $Count) {
        Stop-Inspection "Could not read $Description at offset $($Stream.Position - $read)."
    }

    return $buffer
}

function Get-UInt32BigEndian {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][int]$Offset
    )

    return ([uint64]$Bytes[$Offset] -shl 24) -bor
        ([uint64]$Bytes[$Offset + 1] -shl 16) -bor
        ([uint64]$Bytes[$Offset + 2] -shl 8) -bor
        [uint64]$Bytes[$Offset + 3]
}

function Get-UInt64BigEndian {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $value = [uint64]0
    foreach ($byte in $Bytes) {
        $value = ($value -shl 8) -bor [uint64]$byte
    }

    return $value
}

function Test-BoxType {
    param([Parameter(Mandatory = $true)][string]$Type)

    if ($Type.Length -ne 4) {
        return $false
    }

    foreach ($ch in $Type.ToCharArray()) {
        $code = [int][char]$ch
        if ($code -lt 0x20 -or $code -gt 0x7e) {
            return $false
        }
    }

    return $true
}

function Read-BoxHeader {
    param([Parameter(Mandatory = $true)][System.IO.FileStream]$Stream)

    $offset = $Stream.Position
    $header = Read-RequiredBytes -Stream $Stream -Count 8 -Description "MP4 box header"
    $size32 = Get-UInt32BigEndian -Bytes $header -Offset 0
    $type = [System.Text.Encoding]::ASCII.GetString($header, 4, 4)

    if (-not (Test-BoxType -Type $type)) {
        Stop-Inspection "Invalid MP4 box type at offset $offset."
    }

    $headerSize = [int64]8
    if ($size32 -eq 1) {
        $largeSizeBytes = Read-RequiredBytes -Stream $Stream -Count 8 -Description "extended MP4 box size"
        $boxSize = [int64](Get-UInt64BigEndian -Bytes $largeSizeBytes)
        $headerSize = 16
    } elseif ($size32 -eq 0) {
        $boxSize = $Stream.Length - $offset
    } else {
        $boxSize = [int64]$size32
    }

    if ($boxSize -lt $headerSize) {
        Stop-Inspection "MP4 box '$type' at offset $offset has invalid size $boxSize."
    }

    if ($offset + $boxSize -gt $Stream.Length) {
        Stop-Inspection "MP4 box '$type' at offset $offset extends beyond end of file."
    }

    return [pscustomobject]@{
        Type = $type
        Offset = $offset
        Size = $boxSize
        HeaderSize = $headerSize
    }
}

function Get-BoxPayloadSize {
    param([Parameter(Mandatory = $true)]$Box)

    return [int64]$Box.Size - [int64]$Box.HeaderSize
}

function Test-AnyBoxHasPayload {
    param([Parameter(Mandatory = $true)]$Boxes)

    foreach ($box in $Boxes) {
        if ((Get-BoxPayloadSize -Box $box) -gt 0) {
            return $true
        }
    }

    return $false
}

try {
    $resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
} catch {
    Stop-Inspection "MP4 artifact does not exist: $Path"
}

if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
    Stop-Inspection "MP4 artifact path is not a file: $resolvedPath"
}

$fileInfo = Get-Item -LiteralPath $resolvedPath
if ($fileInfo.Length -lt $MinimumSizeBytes) {
    Stop-Inspection "MP4 artifact is too small: $($fileInfo.Length) bytes."
}

$boxes = @()
$stream = [System.IO.File]::Open($resolvedPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
try {
    while ($stream.Position -lt $stream.Length) {
        if ($stream.Length - $stream.Position -lt 8) {
            Stop-Inspection "Trailing bytes after final MP4 box at offset $($stream.Position)."
        }

        $box = Read-BoxHeader -Stream $stream
        $boxes += $box
        $stream.Position = $box.Offset + $box.Size
    }
} finally {
    $stream.Dispose()
}

if ($boxes.Count -eq 0) {
    Stop-Inspection "MP4 artifact contains no top-level boxes."
}

if ($boxes[0].Type -ne "ftyp") {
    Stop-Inspection "MP4 artifact first top-level box is '$($boxes[0].Type)', expected 'ftyp'."
}

$ftypPayloadSize = Get-BoxPayloadSize -Box $boxes[0]
if ($ftypPayloadSize -lt 8) {
    Stop-Inspection "MP4 artifact 'ftyp' box payload is too small: $ftypPayloadSize bytes."
}

$mdatBoxes = @($boxes | Where-Object { $_.Type -eq "mdat" })
if ($mdatBoxes.Count -eq 0) {
    Stop-Inspection "MP4 artifact does not contain a top-level 'mdat' media data box."
}

if (-not (Test-AnyBoxHasPayload -Boxes $mdatBoxes)) {
    Stop-Inspection "MP4 artifact contains no media data payload bytes in top-level 'mdat' boxes."
}

$moovBoxes = @($boxes | Where-Object { $_.Type -eq "moov" })
if ($moovBoxes.Count -eq 0) {
    Stop-Inspection "MP4 artifact does not contain a top-level 'moov' movie metadata box."
}

if (-not (Test-AnyBoxHasPayload -Boxes $moovBoxes)) {
    Stop-Inspection "MP4 artifact contains no movie metadata payload bytes in top-level 'moov' boxes."
}

Write-Host "MP4 artifact inspection succeeded."
Write-Host "  Path: $resolvedPath"
Write-Host "  Size: $($fileInfo.Length) bytes"
Write-Host "  Top-level boxes:"
foreach ($box in $boxes) {
    Write-Host ("    {0} offset={1} size={2}" -f $box.Type, $box.Offset, $box.Size)
}
