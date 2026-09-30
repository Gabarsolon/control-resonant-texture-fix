<#
.SYNOPSIS
    Control Resonant Texture Streaming Fix Patcher (PowerShell)
.DESCRIPTION
    Disables aggressive over-budget mipmap purging and forces maximum resolution textures
    in Remedy's Northlight Engine (Control Resonant).
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

$defragOrig   = [byte[]]@(0x66, 0xC7, 0x05, 0x21, 0xA8, 0xBC, 0x05, 0x01, 0x01)
$defragPatch  = [byte[]]@(0x66, 0xC7, 0x05, 0x21, 0xA8, 0xBC, 0x05, 0x00, 0x00)

$forceOrig    = [byte[]]@(0x66, 0xC7, 0x05, 0xD9, 0xA8, 0xA5, 0x05, 0x00, 0x00)
$forcePatch   = [byte[]]@(0x66, 0xC7, 0x05, 0xD9, 0xA8, 0xA5, 0x05, 0x01, 0x01)

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
    
    $idx1 = Find-Sequence -Haystack $bytes -Needle $defragPatch
    if ($idx1 -ge 0) {
        [System.Array]::Copy($defragOrig, 0, $bytes, $idx1, $defragOrig.Length)
        Write-Host "[-] Reverted Tile Defrag at offset 0x$($idx1.ToString('X'))" -ForegroundColor Yellow
    }

    $idx2 = Find-Sequence -Haystack $bytes -Needle $forcePatch
    if ($idx2 -ge 0) {
        [System.Array]::Copy($forceOrig, 0, $bytes, $idx2, $forceOrig.Length)
        Write-Host "[-] Reverted Force max res at offset 0x$($idx2.ToString('X'))" -ForegroundColor Yellow
    }

    [System.IO.File]::WriteAllBytes($target, $bytes)
    Write-Host "`nReverted successfully." -ForegroundColor Green
} else {
    Write-Host "`nApplying texture streaming fixes..." -ForegroundColor Cyan

    $idx1 = Find-Sequence -Haystack $bytes -Needle $defragOrig
    if ($idx1 -ge 0) {
        [System.Array]::Copy($defragPatch, 0, $bytes, $idx1, $defragPatch.Length)
        Write-Host "[+] Patched Tile Defrag:Trigger On Over Budget -> DISABLED at offset 0x$($idx1.ToString('X'))" -ForegroundColor Green
    } elseif ((Find-Sequence -Haystack $bytes -Needle $defragPatch) -ge 0) {
        Write-Host "[*] Tile Defrag:Trigger On Over Budget is already patched (DISABLED)." -ForegroundColor Gray
    } else {
        Write-Warning "Could not find Tile Defrag pattern."
    }

    $idx2 = Find-Sequence -Haystack $bytes -Needle $forceOrig
    if ($idx2 -ge 0) {
        [System.Array]::Copy($forcePatch, 0, $bytes, $idx2, $forcePatch.Length)
        Write-Host "[+] Patched Force max res textures -> ENABLED at offset 0x$($idx2.ToString('X'))" -ForegroundColor Green
    } elseif ((Find-Sequence -Haystack $bytes -Needle $forcePatch) -ge 0) {
        Write-Host "[*] Force max res textures is already patched (ENABLED)." -ForegroundColor Gray
    } else {
        Write-Warning "Could not find Force max res pattern."
    }

    [System.IO.File]::WriteAllBytes($target, $bytes)
    Write-Host "`nPatch applied successfully! Textures will remain crisp." -ForegroundColor Green
}
