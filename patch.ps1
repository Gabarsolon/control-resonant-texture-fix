<#
.SYNOPSIS
    Control Resonant Texture Streaming Fix Patcher (PowerShell)
.DESCRIPTION
    Applies the full 5-point reverse-engineered patch suite to CONTROLResonant.exe:
    - Disables Tile Defrag on Over Budget
    - Disables Tile Defrag on Failed Allocation
    - Enables Force max res textures
    - Disables Fit to pool bias downscaling (20.0f -> 0.0f)
    - Expands Tile Heap Reserve from 512 MB to 2048 MB
#>

[CmdletBinding()]
param(
    [Parameter(Position=0)]
    [string]$ExePath,

    [switch]$Revert
)

$ErrorActionPreference = 'Stop'

function Find-ExePath {
    param([string]$Specified)
    if ($Specified -and (Test-Path $Specified)) { return (Resolve-Path $Specified).Path }
    
    $candidates = @(
        ".\CONTROLResonant.exe",
        "..\CONTROLResonant.exe",
        "E:\torrente\CONTROL.Resonant-InsaneRamZes\CONTROLResonant.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    return $null
}

$target = Find-ExePath -Specified $ExePath
if (-not $target) {
    Write-Error "Could not locate CONTROLResonant.exe. Please specify the path: .\patch.ps1 -ExePath 'path\to\CONTROLResonant.exe'"
    exit 1
}

Write-Host "Target: $target" -ForegroundColor Cyan

$bak = "$target.bak"
if (-not (Test-Path $bak)) {
    Copy-Item $target $bak
    Write-Host "Created backup: $bak" -ForegroundColor Green
} else {
    Write-Host "Backup already exists: $bak" -ForegroundColor Gray
}

$bytes = [System.IO.File]::ReadAllBytes($target)

$patches = @(
    @{
        Name  = "Tile Defrag: Trigger On Over Budget"
        Orig  = [byte[]]@(0x66, 0xC7, 0x05, 0x21, 0xA8, 0xBC, 0x05, 0x01, 0x01)
        Patch = [byte[]]@(0x66, 0xC7, 0x05, 0x21, 0xA8, 0xBC, 0x05, 0x00, 0x00)
    },
    @{
        Name  = "Tile Defrag: Trigger On Failed Allocation"
        Orig  = [byte[]]@(0x66, 0xC7, 0x05, 0x59, 0xA8, 0xBC, 0x05, 0x01, 0x01)
        Patch = [byte[]]@(0x66, 0xC7, 0x05, 0x59, 0xA8, 0xBC, 0x05, 0x00, 0x00)
    },
    @{
        Name  = "Force max res textures"
        Orig  = [byte[]]@(0x66, 0xC7, 0x05, 0xD9, 0xA8, 0xA5, 0x05, 0x00, 0x00)
        Patch = [byte[]]@(0x66, 0xC7, 0x05, 0xD9, 0xA8, 0xA5, 0x05, 0x01, 0x01)
    },
    @{
        Name  = "Fit to pool: Bias limit (20.0f -> 0.0f)"
        Orig  = [byte[]]@(0x00, 0x00, 0xA0, 0x41, 0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x80, 0x41)
        Patch = [byte[]]@(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x80, 0x41)
    },
    @{
        Name  = "Tile Heap: Reserve (512 MB -> 2048 MB)"
        Orig  = [byte[]]@(0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00)
        Patch = [byte[]]@(0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00)
    }
)

function Find-Sequence {
    param([byte[]]$Haystack, [byte[]]$Needle)
    for ($i = 0; $i -le $Haystack.Length - $Needle.Length; $i++) {
        $found = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) {
                $found = $false
                break
            }
        }
        if ($found) { return $i }
    }
    return -1
}

if ($Revert) {
    Write-Host "`nReverting to vanilla defaults..." -ForegroundColor Yellow
    foreach ($p in $patches) {
        $idx = Find-Sequence -Haystack $bytes -Needle $p.Patch
        if ($idx -ge 0) {
            [System.Array]::Copy($p.Orig, 0, $bytes, $idx, $p.Orig.Length)
            Write-Host "[-] Reverted $($p.Name) at 0x$($idx.ToString('X'))" -ForegroundColor Yellow
        }
    }
    [System.IO.File]::WriteAllBytes($target, $bytes)
    Write-Host "`nReverted successfully." -ForegroundColor Green
} else {
    Write-Host "`nApplying full texture streaming fix suite..." -ForegroundColor Cyan
    foreach ($p in $patches) {
        $idx = Find-Sequence -Haystack $bytes -Needle $p.Orig
        if ($idx -ge 0) {
            [System.Array]::Copy($p.Patch, 0, $bytes, $idx, $p.Patch.Length)
            Write-Host "[+] Patched $($p.Name) at 0x$($idx.ToString('X'))" -ForegroundColor Green
        } elseif ((Find-Sequence -Haystack $bytes -Needle $p.Patch) -ge 0) {
            Write-Host "[*] $($p.Name) is already patched." -ForegroundColor Gray
        } else {
            Write-Warning "Could not find sequence for $($p.Name)"
        }
    }
    [System.IO.File]::WriteAllBytes($target, $bytes)
    Write-Host "`nAll patches applied successfully!" -ForegroundColor Green
}
