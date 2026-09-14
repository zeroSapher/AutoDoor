<#
.SYNOPSIS
    Build, flash and inspect the AutoDoor firmware without needing make, CMake or
    Ninja installed.

.DESCRIPTION
    Drives arm-none-eabi-gcc directly. The compiler flags, linker script and
    source list match CMakeLists.txt, so both build paths produce the same
    firmware.

    This script exists because a bare Windows toolchain usually ships GCC but not
    GNU make; it gives the project a build path that needs nothing beyond the
    toolchain itself.

.PARAMETER DebugBuild
    Build with -Og -g3. The default is -Os -g.

.PARAMETER Clean
    Delete the build directory before building.

.PARAMETER Flash
    After a successful build, write the .bin with st-flash.

.PARAMETER Erase
    Mass-erase the target flash and exit.

.PARAMETER Toolchain
    Directory holding arm-none-eabi-gcc. Defaults to D:\ST\gcc-arm-none-eabi\bin
    when it exists, otherwise the tools are taken from PATH.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Clean -DebugBuild
    .\build.ps1 -Flash
#>

[CmdletBinding()]
param(
    [switch]$DebugBuild,
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Erase,
    [switch]$NoLimits,
    [string]$Toolchain
)

$ErrorActionPreference = 'Stop'

$Root     = $PSScriptRoot | Split-Path -Parent   # tools/ -> project root
$BuildDir = Join-Path $Root 'build'
$Target   = 'AutoDoor'
$LdScript = Join-Path $Root 'Linker/stm32f103c8t6.ld'

# ---------------------------------------------------------------------------
# Locate the toolchain
# ---------------------------------------------------------------------------
if (-not $Toolchain -and (Test-Path 'D:\ST\gcc-arm-none-eabi\bin')) {
    $Toolchain = 'D:\ST\gcc-arm-none-eabi\bin'
}
if ($Toolchain) { $Toolchain = $Toolchain.TrimEnd('\', '/') + '\' }

function Get-Tool {
    param([string]$Name, [switch]$Optional)

    if ($Toolchain) {
        $exe = "$Toolchain$Name.exe"
        if (Test-Path $exe) { return $exe }
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    if ($Optional) { return $null }
    throw "Cannot find $Name. Pass -Toolchain <dir> or put it on PATH."
}

$Gcc     = Get-Tool 'arm-none-eabi-gcc'
$Objcopy = Get-Tool 'arm-none-eabi-objcopy'
$Size    = Get-Tool 'arm-none-eabi-size'
$StFlash = Get-Tool 'st-flash' -Optional

$Version = (& $Gcc --version | Select-Object -First 1)
Write-Host "Toolchain : $Gcc" -ForegroundColor Cyan
Write-Host "Version   : $Version" -ForegroundColor Cyan

if ($Erase) {
    if (-not $StFlash) { throw 'st-flash not found; cannot erase.' }
    Write-Host 'Mass-erasing target flash...' -ForegroundColor Yellow
    & $StFlash erase
    exit $LASTEXITCODE
}

# ---------------------------------------------------------------------------
# Source list - shared with tools/build-sim.ps1 via sources.ps1 so the real and
# simulation builds cannot drift apart.
# ---------------------------------------------------------------------------
. (Join-Path $PSScriptRoot 'sources.ps1')

$Sources = $BaseSources + $RealBackend

$McFlags  = @('-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft')
$IncFlags = $IncludeDirs | ForEach-Object { "-I$Root/$_" }

$OptFlags = if ($DebugBuild) { @('-Og', '-g3') } else { @('-Os', '-g') }

# Bench mode: build without limit switches fitted. The firmware simulates the
# door position instead of reading PA0/PA1. See the note in Core/main.h for what
# that removes - in short, there is no software fast-stop and no limit fault
# detection, and the only overrun protection left is the hardware NC contact.
$LimitsFlag = if ($NoLimits) { @('-DAUTODOOR_NO_LIMITS=1') } else { @() }

$CFlags = $McFlags + $DefineBase + $LimitsFlag + $IncFlags + @(
    '-Wall', '-Wextra', '-Wshadow', '-Wdouble-promotion'
) + $OptFlags + @(
    '-ffunction-sections', '-fdata-sections', '-std=gnu11'
)

if ($NoLimits) {
    Write-Host 'BENCH BUILD: limit switches disabled, position is simulated' -ForegroundColor Magenta
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Objects  = @()
$Failures = 0
$Warnings = 0

# PowerShell promotes a native command's stderr to a terminating error under
# $ErrorActionPreference = 'Stop', which would abort on the first compiler
# diagnostic - i.e. exactly when we most want to print the message. The
# preference is relaxed for the compile loop and success is judged purely on
# $LASTEXITCODE.
$ErrorActionPreference = 'Continue'

foreach ($Rel in $Sources) {
    $Src = Join-Path $Root $Rel
    if (-not (Test-Path $Src)) {
        Write-Host "MISSING  $Rel" -ForegroundColor Red
        $Failures++
        continue
    }

    $Obj = Join-Path $BuildDir (($Rel -replace '[/\\]', '_') + '.o')
    $Extra = @()
    if ($PerFileFlags.ContainsKey($Rel)) { $Extra = $PerFileFlags[$Rel] }

    $Output = & $Gcc @CFlags @Extra -c $Src -o $Obj 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED   $Rel" -ForegroundColor Red
        $Output | Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }
        $Failures++
    }
    else {
        if ($Output) {
            $Warnings++
            Write-Host "WARNINGS $Rel" -ForegroundColor Yellow
            $Output | Select-Object -First 12 | ForEach-Object { Write-Host "  $_" }
        }
        $Objects += $Obj
    }
}

Write-Host ("`ncompiled {0}/{1} files, {2} failure(s), {3} file(s) with warnings" -f `
            $Objects.Count, $Sources.Count, $Failures, $Warnings)

if ($Failures -gt 0) {
    throw "$Failures translation unit(s) failed to compile."
}

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
$Elf = Join-Path $BuildDir "$Target.elf"
$Hex = Join-Path $BuildDir "$Target.hex"
$Bin = Join-Path $BuildDir "$Target.bin"
$Map = Join-Path $BuildDir "$Target.map"

$LdFlags = @(
    "-T$LdScript"
    '-Wl,--gc-sections'
    "-Wl,-Map=$Map"
    '-Wl,--print-memory-usage'
    '--specs=nano.specs'
    '--specs=nosys.specs'
)

Write-Host "`nLinking..." -ForegroundColor Cyan
# The nosys stubs intentionally warn about unimplemented syscalls; they are noise
# for a bare-metal image. stderr is merged and judged solely on the exit code,
# because PowerShell promotes native stderr to a terminating error under
# $ErrorActionPreference = 'Stop' and would abort on a perfectly good link.
$LinkOutput = & {
    $ErrorActionPreference = 'Continue'
    & $Gcc @McFlags @Objects @LdFlags -o $Elf 2>&1
}
$LinkExit = $LASTEXITCODE
$LinkOutput |
    Where-Object { $_ -notmatch 'not implemented and will always fail' -and
                   $_ -notmatch 'does not take linker garbage' -and
                   $_ -notmatch 'RWX permissions' } |
    ForEach-Object { Write-Host "  $_" }

if ($LinkExit -ne 0) { throw 'Link failed.' }

& $Objcopy -O ihex   $Elf $Hex | Out-Null
& $Objcopy -O binary $Elf $Bin | Out-Null

Write-Host "`n=== Section sizes ===" -ForegroundColor Green
& $Size $Elf

Write-Host "`n=== Artifacts ===" -ForegroundColor Green
Get-ChildItem $BuildDir -File |
    Where-Object { $_.Extension -in '.elf', '.hex', '.bin' } |
    Select-Object Name, @{ n = 'Bytes'; e = { $_.Length } } |
    Format-Table -AutoSize

# ---------------------------------------------------------------------------
# Flash
# ---------------------------------------------------------------------------
if ($Flash) {
    if (-not $StFlash) { throw 'st-flash not found; cannot flash.' }
    Write-Host "Writing $Bin to 0x08000000..." -ForegroundColor Yellow
    & $StFlash write $Bin 0x08000000
    if ($LASTEXITCODE -ne 0) { throw 'Flash failed.' }
    Write-Host 'Flash complete. Reset the target to run the new firmware.' -ForegroundColor Green
}
