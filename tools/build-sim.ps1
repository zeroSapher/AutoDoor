<#
.SYNOPSIS
    Build the AutoDoor firmware for the Proteus simulation.

.DESCRIPTION
    Compiles the SAME application, door state machine and command protocol as the
    real firmware, using the OLED12864 I2C display model.

    What changes versus tools/build.ps1:
            - Display backend: Display_Oled.c + SSD1306-compatible OLED driver
            - EEPROM uses EEPROM_Stub.c because persistence is not part of the display
                simulation; the application must report that storage is unavailable.
      - Defines AUTODOOR_SIM_BUILD so the firmware can report which variant it is.
      - Output goes to build-sim/ so the two builds never overwrite each other.

    IMPORTANT: the simulation build can exercise the OLED I2C display and GPIO
    inputs, but not EEPROM persistence or the real NC motor power-cut circuit. See
    docs/Proteus仿真方案.md for what the simulation does and does not validate.

.PARAMETER DebugBuild
    Build with -Og -g3. The default is -Os -g.

.PARAMETER Clean
    Delete the build directory before building.

.PARAMETER NoLimits
    Bench-only build: simulate door position instead of reading PA0/PA1 limit
    switches. This is useful while drawing the Proteus schematic, but it removes
    limit fault detection and must not be used for a real door.

.PARAMETER Toolchain
    Directory holding arm-none-eabi-gcc. Defaults to D:\ST\gcc-arm-none-eabi\bin.

.EXAMPLE
    .\build-sim.ps1
    .\build-sim.ps1 -Clean -DebugBuild
#>

[CmdletBinding()]
param(
    [switch]$DebugBuild,
    [switch]$Clean,
    [switch]$NoLimits,
    [string]$Toolchain
)

$ErrorActionPreference = 'Stop'

$Root     = $PSScriptRoot | Split-Path -Parent
$BuildDir = Join-Path $Root 'build-sim'
$Target   = 'AutoDoorSim'
$LdScript = Join-Path $Root 'Linker/stm32f103c8t6.ld'

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

# Shared source list and include paths.
. (Join-Path $PSScriptRoot 'sources.ps1')

# Simulation backend instead of the OLED one.
$Sources = $BaseSources + $SimBackend

$McFlags  = @('-mcpu=cortex-m3', '-mthumb', '-mfloat-abi=soft')
$LimitsFlag = if ($NoLimits) { @('-DAUTODOOR_NO_LIMITS=1') } else { @() }
$Defines  = $DefineBase + $LimitsFlag + @('-DAUTODOOR_SIM_BUILD=1')
$IncFlags = $IncludeDirs | ForEach-Object { "-I$Root/$_" }

$OptFlags = if ($DebugBuild) { @('-Og', '-g3') } else { @('-Os', '-g') }

$CFlags = $McFlags + $Defines + $IncFlags + @(
    '-Wall', '-Wextra', '-Wshadow', '-Wdouble-promotion'
) + $OptFlags + @(
    '-ffunction-sections', '-fdata-sections', '-std=gnu11'
)

Write-Host 'AutoDoor SIMULATION build (OLED12864 I2C display backend)' -ForegroundColor Cyan
Write-Host "Toolchain : $Gcc" -ForegroundColor Cyan
Write-Host "Version   : $((& $Gcc --version | Select-Object -First 1))" -ForegroundColor Cyan
if ($NoLimits) {
    Write-Host 'BENCH BUILD: PA0/PA1 are disabled; door position is simulated' -ForegroundColor Magenta
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Objects  = @()
$Failures = 0
$Warnings = 0

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

$Elf = Join-Path $BuildDir "$Target.elf"
$Hex = Join-Path $BuildDir "$Target.hex"

$LdFlags = @(
    "-T$LdScript"
    '-Wl,--gc-sections'
    "-Wl,-Map=$BuildDir/$Target.map"
    '-Wl,--print-memory-usage'
    '--specs=nano.specs'
    '--specs=nosys.specs'
)

Write-Host "`nLinking..." -ForegroundColor Cyan
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

# Proteus loads a .hex or .elf for the STM32 model; the .hex is what its
# "Program File" field expects.
& $Objcopy -O ihex $Elf $Hex | Out-Null

Write-Host "`n=== Section sizes ===" -ForegroundColor Green
& $Size $Elf

Write-Host "`n=== Artifacts ===" -ForegroundColor Green
Get-ChildItem $BuildDir -File |
    Where-Object { $_.Extension -in '.elf', '.hex' } |
    Select-Object Name, @{ n = 'Bytes'; e = { $_.Length } } |
    Format-Table -AutoSize

Write-Host "Load $Hex into the STM32 model's Program File property in Proteus." -ForegroundColor Cyan
