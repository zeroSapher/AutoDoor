<#
.SYNOPSIS
    Verify that the three build manifests list the same sources.

.DESCRIPTION
    This project has three independent entry points - CMakeLists.txt,
    tools/sources.ps1 (used by the PowerShell builds) and the Keil .uvprojx -
    because no single one of them can serve all three toolchains. That
    duplication has already caused two real failures:

      1. The test build (round 5) derived its list from CMakeLists.txt and failed
         with "Sensor.h: No such file or directory".
      2. CMakeLists.txt was never updated when App/Display_Oled.c was added, so
         the CMake build linked with undefined `DispBk_*` references. Nobody
         noticed because only the PowerShell path was being built.

    A comment saying "keep these in sync" did not prevent either. This check does,
    on the grounds that a mismatch should fail loudly rather than turn up as an
    undefined reference at link time.

    The check is a pure text comparison - it does not run CMake, Ninja or Keil,
    so it works anywhere the repository can be read.

.PARAMETER Quiet
    Print only the verdict, not the per-manifest detail.

.EXAMPLE
    .\tools\check-sources.ps1
#>

[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$Root = $PSScriptRoot | Split-Path -Parent

# ---------------------------------------------------------------------------
# 1. PowerShell build manifests
# ---------------------------------------------------------------------------
# tools/sources.ps1 declares the lists; dot-source it and put the pieces back
# together exactly as the build scripts do.
. (Join-Path $PSScriptRoot 'sources.ps1')

$psReal = ($BaseSources + $RealBackend) | Sort-Object -Unique
$psSim  = ($BaseSources + $SimBackend)  | Sort-Object -Unique

# ---------------------------------------------------------------------------
# 2. CMake manifest
# ---------------------------------------------------------------------------
$cmakeText = Get-Content (Join-Path $Root 'CMakeLists.txt') -Raw
$cmake = [regex]::Matches($cmakeText, '(?m)^\s+((?:Core|Drivers|Hardware|App)/[\w./]+\.(?:c|s))\s*$') |
         ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique

# ---------------------------------------------------------------------------
# 3. Keil manifest
# ---------------------------------------------------------------------------
# <FilePath>..\Hardware\Motor\Motor.c</FilePath> -> Hardware/Motor/Motor.c
$keilText = Get-Content (Join-Path $Root 'MDK-ARM\AutoDoor.uvprojx') -Raw
$keil = [regex]::Matches($keilText, '<FilePath>\.\.\\([\w\\.]+\.(?:c|s))</FilePath>') |
        ForEach-Object { $_.Groups[1].Value.Replace('\', '/') } | Sort-Object -Unique

# ---------------------------------------------------------------------------
# Documented, intentional differences
# ---------------------------------------------------------------------------
# The startup file is the ONE place where the manifests legitimately differ, and
# it is a hard constraint rather than an oversight:
#
#   Keil        : Drivers/CMSIS/startup_stm32f10x_md.s      (ARM/Keil assembler)
#   GCC builds  : Drivers/CMSIS/startup_stm32f10x_md_gcc.c  (GNU as cannot read .s)
#
# Both define the vector table and Reset_Handler, so linking both would be a
# duplicate-symbol error. Every other difference is treated as a defect.
$StartupKeil = 'Drivers/CMSIS/startup_stm32f10x_md.s'
$StartupGcc  = 'Drivers/CMSIS/startup_stm32f10x_md_gcc.c'

# ---------------------------------------------------------------------------
# Compare
# ---------------------------------------------------------------------------
$problems = @()

function Compare-Manifests {
    param([string]$NameA, [string[]]$A, [string]$NameB, [string[]]$B)

    $onlyA = $A | Where-Object { $_ -notin $B }
    $onlyB = $B | Where-Object { $_ -notin $A }

    foreach ($f in $onlyA) { $script:problems += "$NameA has '$f' but $NameB does not" }
    foreach ($f in $onlyB) { $script:problems += "$NameB has '$f' but $NameA does not" }
}

# Keil builds the real firmware, so it must match the real PowerShell list,
# except for the startup file explained above.
$keilComparable  = $keil   | Where-Object { $_ -ne $StartupKeil }
$psRealComparable = $psReal | Where-Object { $_ -ne $StartupGcc }

Compare-Manifests 'CMakeLists.txt'      $cmake            'sources.ps1(real)' $psReal
Compare-Manifests 'Keil .uvprojx'       $keilComparable   'sources.ps1(real)' $psRealComparable

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
if (-not $Quiet) {
    Write-Host "Build manifest contents" -ForegroundColor Cyan
    Write-Host ("  CMakeLists.txt      : {0} sources" -f $cmake.Count)
    Write-Host ("  sources.ps1 (real)  : {0} sources" -f $psReal.Count)
    Write-Host ("  sources.ps1 (sim)   : {0} sources" -f $psSim.Count)
    Write-Host ("  Keil .uvprojx       : {0} sources" -f $keil.Count)
    Write-Host ""

    # Which files differ between the real and simulation builds, and why.
    $realOnly = $psReal | Where-Object { $_ -notin $psSim }
    $simOnly  = $psSim  | Where-Object { $_ -notin $psReal }
    if ($realOnly -or $simOnly) {
        Write-Host "Real-only (expected: OLED + EEPROM driver):" -ForegroundColor DarkGray
        $realOnly | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        Write-Host "Sim-only (expected: 1602 + EEPROM stub):" -ForegroundColor DarkGray
        $simOnly | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        Write-Host ""
    }
}

if ($problems.Count -gt 0) {
    Write-Host "$($problems.Count) MANIFEST MISMATCH(ES):" -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "A file listed in one manifest but not another means that build will" -ForegroundColor Yellow
    Write-Host "either omit needed code (undefined reference) or double-define symbols." -ForegroundColor Yellow
    exit 1
}

Write-Host "All three build manifests agree." -ForegroundColor Green
exit 0
