<#
.SYNOPSIS
    Hardware-in-the-loop chain test for the AutoDoor firmware.

.DESCRIPTION
    Drives the whole door chain without touching the hardware: key presses and
    the presence sensors are injected over SWD, and every assertion is made
    against what the firmware actually prints on the serial console.

    WHY INJECT INSTEAD OF PRESS
    ---------------------------
    A hand on a dupont wire is not reproducible: contact bounce turns an intended
    long press into a short one, and the same gesture gives different results on
    two tries. Injecting the input events exercises the very same code path - the
    real main loop consumes them - while removing the mechanics from the equation.
    A failure here is a firmware failure.

    WHAT CAN BE INJECTED
    --------------------
    Keys (Hardware/Input/Key.c, s_keys[] indexed by KeyId_t):
        0 KEY1 start/stop   1 KEY2 mode   2 KEY3 manual open   3 KEY4 close/estop
        .shortEvent / .longEvent  - one-shot pending events

    Sensors (Hardware/Input/Sensor.c, s_outside / s_inside):
        .event  - one-shot "a person arrived" event, what the edge would set
        .stable - the debounced LEVEL ("a person is standing there")

    Both are writable because Debounce_Tick1ms() returns early when the pin has
    not changed and nothing is pending, so an injected .stable survives. That
    makes it possible to test the level-based safety net separately from the
    edge-triggered path - the case where a sensor edge is lost and only the level
    can still stop the door from closing on somebody.

    A key press cannot be injected as a level: keys have no level consumer, the
    application only reads the event flags.

.PARAMETER Port
    COM port of the board, e.g. COM7.

.PARAMETER Elf
    The ELF matching the firmware on the target; symbol names are needed to write
    the injected variables. Defaults to build\AutoDoor.elf.

.PARAMETER SkipFlash
    Do not reflash; test whatever is already on the target. By default the script
    flashes build\AutoDoor.elf first, so the test can never run against a stale
    image.

.EXAMPLE
    powershell -File tools\bench-chain.ps1 -Port COM7

.NOTES
    Requires the default build (no limit switches fitted, so the door arrives on a
    calibrated travel time): the chain relies on the door reaching its end by time.
    The script checks this and refuses to run otherwise.
#>

[CmdletBinding()]
param(
    [string]$Port = 'COM7',
    [int]$Baud = 115200,
    [string]$Elf = '',
    [string]$Gdb = '',
    [int]$GdbPort = 3333,
    [string]$OpenOcd = '',
    [switch]$SkipFlash,
    [switch]$KeepServer
)

$ErrorActionPreference = 'Stop'

if (-not $Elf) { $Elf = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\AutoDoor.elf' }

function Find-Tool {
    param([string[]]$Candidates, [string]$Name)
    foreach ($c in $Candidates) { if ($c -and (Test-Path $c)) { return $c } }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "$Name not found. Pass it explicitly."
}

if (-not $Gdb) {
    $Gdb = Find-Tool @('D:\ST\gcc-arm-none-eabi\bin\arm-none-eabi-gdb.exe') 'arm-none-eabi-gdb'
}
if (-not $OpenOcd) {
    $OpenOcd = Find-Tool @('D:\ST\openocd\bin\openocd.exe') 'openocd'
}
if (-not (Test-Path $Elf)) { throw "ELF not found: $Elf (build it with tools\build.ps1)" }

# Forward slashes for anything handed to GDB: its command parser treats a
# backslash as an escape, so "file D:\code\..." arrives as "D:code..." and the
# open fails. The MCP server converts paths for exactly the same reason.
$ElfGdb = $Elf.Replace('\', '/')

# --- KeyId_t indices (Hardware/Input/Key.h) --------------------------------
$KEY_START = 0; $KEY_MODE = 1; $KEY_OPEN = 2; $KEY_CLOSE_ESTOP = 3

# --- one serial port is opened for the whole run ---------------------------
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.Encoding    = [System.Text.Encoding]::ASCII
$sp.ReadTimeout = 100
$rxBuf = New-Object byte[] 4096

$results = @()
$stepNo  = 0
$ocdProc = $null

function Read-New {
    $sb = New-Object System.Text.StringBuilder
    try {
        while ($sp.BytesToRead -gt 0) {
            $n = $sp.Read($rxBuf, 0, $rxBuf.Length)
            if ($n -gt 0) { [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($rxBuf, 0, $n)) }
        }
    } catch [System.TimeoutException] { }
    return $sb.ToString()
}

function Wait-Text {
    param([string]$Pattern, [int]$TimeoutMs = 8000)
    $sw  = [System.Diagnostics.Stopwatch]::StartNew()
    $acc = New-Object System.Text.StringBuilder
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $chunk = Read-New
        if ($chunk.Length -gt 0) { [void]$acc.Append($chunk) }
        if ([regex]::IsMatch($acc.ToString(), $Pattern)) { return $acc.ToString() }
        Start-Sleep -Milliseconds 40
    }
    return $acc.ToString()
}

function Show([string]$s) { return $s.Replace("`r", '\r').Replace("`n", '\n') }

function Note([string]$msg) { Write-Host "      $msg" }

# --- SWD injection ---------------------------------------------------------

<#
  Run GDB and return @{ Code; Output } without ever letting it throw.

  GDB prints harmless warnings on stderr ("could not convert 'Door.c' from the
  host encoding"), and with $ErrorActionPreference = 'Stop' a native command
  writing to stderr while it is redirected with 2>&1 becomes a TERMINATING error
  in PowerShell. That turned a perfectly good `load` into "flash failed" and hid
  the real reason. The exit code is the only thing that decides success here, so
  the preference is relaxed locally - never globally.
#>
function Invoke-GdbRaw {
    param([string[]]$GdbArgs)
    $ErrorActionPreference = 'Continue'
    $out  = & $Gdb @GdbArgs 2>&1
    $code = $LASTEXITCODE
    return @{ Code = $code; Output = (($out | ForEach-Object { "$_" }) -join "`n") }
}

function Invoke-Gdb {
    param([string[]]$Commands)
    $gdbArgs = @('--batch', '--nx', '--quiet', '-ex', "file $ElfGdb", '-ex', "target extended-remote 127.0.0.1:$GdbPort", '-ex', 'monitor halt')
    foreach ($c in $Commands) { $gdbArgs += @('-ex', $c) }
    $gdbArgs += @('-ex', 'monitor resume')
    $r = Invoke-GdbRaw $gdbArgs
    if ($r.Code -ne 0) {
        throw "gdb failed (exit $($r.Code)): $($Commands -join '; ')`n$($r.Output)"
    }
    return $r.Output
}

function Inject-Key {
    param([int]$Index, [ValidateSet('short', 'long')]$Kind)
    [void](Invoke-Gdb @("set var 'Key.c'::s_keys[$Index].${Kind}Event = 1"))
    Note "SWD: KEY$($Index + 1) $Kind press"
}

function Inject-Sensor {
    # arrive    : a person walks in - the edge event AND the level it implies
    # leave     : they walk away - both cleared
    # level-on  : only the level, no event. This is the lost-edge case.
    # level-off : drop the level, no event.
    param([ValidateSet('outside', 'inside')]$Which, [ValidateSet('arrive', 'leave', 'level-on', 'level-off')]$What)

    $v = "'Sensor.c'::s_$Which"
    switch ($What) {
        'arrive'    { [void](Invoke-Gdb @("set var $v.stable = 1", "set var $v.event = 1")) }
        'leave'     { [void](Invoke-Gdb @("set var $v.stable = 0", "set var $v.event = 0")) }
        'level-on'  { [void](Invoke-Gdb @("set var $v.event = 0", "set var $v.stable = 1")) }
        'level-off' { [void](Invoke-Gdb @("set var $v.event = 0", "set var $v.stable = 0")) }
    }
    Note "SWD: $Which sensor -> $What"
}

# --- serial commands -------------------------------------------------------

function Send-Cmd {
    param([string]$Cmd)
    $sp.Write($Cmd + "`r`n")
}

function Get-Status {
    [void](Read-New)
    Send-Cmd 'STATUS?'
    # The complete line, terminator and all: matching the bare "OK STATUS" prefix
    # can catch a half-received line and yield a status table full of blanks.
    $t = Wait-Text 'OK STATUS[^\r\n]*I2C=\S+\r' 3000
    $line = ($t -split "`r?`n" | Where-Object { $_ -match '^OK STATUS.*I2C=' } | Select-Object -Last 1)
    if (-not $line) { return $null }
    $h = @{}
    foreach ($k in @('RUN', 'MODE', 'DOOR', 'FAULT', 'DELAY', 'LIMITS')) {
        if ($line -match "$k=(\S+)") { $h[$k] = $Matches[1] }
    }
    return $h
}

function Get-State {
    $s = Get-Status
    if (-not $s) { return '(no reply)' }
    return "RUN=$($s['RUN']) MODE=$($s['MODE']) DOOR=$($s['DOOR']) FAULT=$($s['FAULT'])"
}

# --- assertions ------------------------------------------------------------

function Step {
    param(
        [string]$Name,
        [scriptblock]$Action,
        [string]$Expect = '',          # regex that must appear in the output
        [string]$NotExpect = '',       # regex that must NOT appear
        [int]$TimeoutMs = 8000,
        [int]$QuietMs = 1500
    )

    $script:stepNo++
    [void](Read-New)
    & $Action

    if ($NotExpect) {
        $seen = Wait-Text '(?!)' $QuietMs      # never matches: drain the window
        $ok = -not [regex]::IsMatch($seen, $NotExpect)
        $what = "NOT /$NotExpect/ within ${QuietMs}ms"
    } else {
        $seen = Wait-Text $Expect $TimeoutMs
        $ok = [regex]::IsMatch($seen, $Expect)
        $what = "/$Expect/"
    }

    $script:results += [pscustomobject]@{
        No = $script:stepNo; Name = $Name; Pass = [bool]$ok
        Expect = $what; Reply = $seen; Status = (Get-State)
    }

    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1,-46} {2}" -f $tag, $Name, (Get-State))
    if (-not $ok) { Write-Host ("      expected {0}" -f $what) }
}

<#
  Assert on an internal variable instead of on serial output.

  Screen navigation prints nothing at all, so the only way to test it is to read
  the firmware's own state back over SWD. Reading halts the target, so it happens
  after a short pause - the main loop has to have consumed the injected event
  first, and it does that within microseconds of the resume.
#>
function Step-Var {
    param(
        [string]$Name,
        [scriptblock]$Action,
        [string]$Expr,
        [string]$ExpectRegex
    )

    $script:stepNo++
    [void](Read-New)
    & $Action

    # Poll instead of sleeping a fixed amount and reading once. The main loop can
    # be inside a 1 KB I2C panel flush (~90 ms) when the event lands, so a single
    # read a quarter of a second later caught the screen still unchanged - the
    # value had moved on by the next step's read. That is a flaky assertion, not a
    # firmware defect, and a fixed sleep would only move the flakiness around.
    $deadline = (Get-Date).AddMilliseconds(2000)
    $out = ''
    do {
        Start-Sleep -Milliseconds 200
        $out = Invoke-Gdb @("print $Expr")
        $ok  = [regex]::IsMatch($out, $ExpectRegex)
    } while ((-not $ok) -and ((Get-Date) -lt $deadline))

    # GDB also echoes the function it halted in, so take the LAST "$N = value".
    $shown = $out.Trim()
    $m = [regex]::Matches($out, '\$\d+\s*=\s*([^\r\n]+)')
    if ($m.Count -gt 0) { $shown = $m[$m.Count - 1].Groups[1].Value.Trim() }

    $script:results += [pscustomobject]@{
        No = $script:stepNo; Name = $Name; Pass = [bool]$ok
        Expect = "$Expr ~ /$ExpectRegex/"; Reply = $shown; Status = ''
    }

    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("[{0}] {1,-46} {2}" -f $tag, $Name, $shown)
    if (-not $ok) { Write-Host ("      expected {0} ~ /{1}/" -f $Expr, $ExpectRegex) }
}

# ---------------------------------------------------------------------------
# Set-up
# ---------------------------------------------------------------------------

Write-Host "AutoDoor bench chain test"
Write-Host "  port      : $Port"
Write-Host "  elf       : $Elf"
Write-Host "  gdb       : $Gdb"
Write-Host ''

# An OpenOCD GDB server is needed for the injections. Reuse one if it is
# already listening, otherwise start one and own its lifetime.
function Test-GdbPort {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $GdbPort); $c.Close(); return $true
    } catch { return $false }
}

if (-not (Test-GdbPort)) {
    Write-Host "starting OpenOCD on gdb port $GdbPort ..."
    # One single argument string, not an array: Start-Process joins an array with
    # spaces and drops the inner quotes, so "-c" and "gdb_port 3333" would arrive
    # as two separate arguments and OpenOCD would reject the loose port number.
    $argLine = '-f interface/stlink.cfg -f target/stm32f1x.cfg -c "gdb_port ' + $GdbPort + '"'
    $ocdProc = Start-Process -FilePath $OpenOcd -PassThru -WindowStyle Hidden -ArgumentList $argLine
    $deadline = (Get-Date).AddSeconds(15)
    while (-not (Test-GdbPort) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    if (-not (Test-GdbPort)) {
        try { Stop-Process -Id $ocdProc.Id -Force -ErrorAction SilentlyContinue } catch { }
        throw "OpenOCD did not open port $GdbPort (is another debugger holding the ST-Link?)"
    }
    Write-Host "OpenOCD ready (pid $($ocdProc.Id))"
}

$sp.Open()

$fatal  = $null
$failed = @()

try {
    if (-not $SkipFlash) {
        Write-Host 'flashing...'
        $r = Invoke-GdbRaw @('--batch', '--nx', '--quiet', '-ex', "file $ElfGdb",
                             '-ex', "target extended-remote 127.0.0.1:$GdbPort",
                             '-ex', 'monitor reset halt', '-ex', 'load', '-ex', 'monitor reset run')
        if ($r.Code -ne 0) { throw "flash failed`n$($r.Output)" }
        Start-Sleep -Milliseconds 800
    }

    # Clean slate first: a run that was interrupted could have left an injected
    # level asserting presence, which would then block the closing move below.
    Inject-Sensor 'outside' 'leave'
    Inject-Sensor 'inside' 'leave'

    # Precondition: the chain depends on simulated limits.
    $st = Get-Status
    if (-not $st) { throw "no reply from $Port - is the firmware running and the console wired?" }
    Write-Host ''
    Write-Host "precondition: $(Get-State) LIMITS=$($st['LIMITS'])"
    if ($st['LIMITS'] -ne 'TIMED') {
        throw "this chain needs the default build (LIMITS=TIMED). Build with tools\build.ps1 (use -WithLimits only if you actually fitted limit switches)."
    }

    # Speed the chain up: 1 s auto-close instead of 5 s. Not persisted (no EEPROM).
    Send-Cmd 'DELAY=1000'
    [void](Wait-Text 'OK DELAY' 3000)
    Send-Cmd 'MODE=AUTO'
    [void](Wait-Text 'OK MODE|ERR' 3000)

    # Normalise to a closed, disabled door, whatever the previous run left.
    #
    # A fresh flash already satisfies this, but a -SkipFlash rerun does not, and
    # the chain asserts on the IDLE -> OPENING transition in section A: a door
    # that is already open can never produce it, so those steps would fail for a
    # reason that has nothing to do with the firmware. RESET both clears a fault
    # and drops the enable latch, which is the state step 1 expects to start from.
    Send-Cmd 'RESET'
    [void](Wait-Text 'OK RESET' 3000)
    $st = Get-Status
    if ($st -and $st['DOOR'] -ne 'IDLE') {
        Note "door was $($st['DOOR']) - closing it before starting"
        Inject-Key $KEY_START 'short'          # enable, so the door is allowed to move
        Start-Sleep -Milliseconds 300
        Send-Cmd 'DOOR=CLOSE'
        [void](Wait-Text '-> IDLE\b' 12000)
        Send-Cmd 'RESET'                       # ...and disabled again
        [void](Wait-Text 'OK RESET' 3000)
    }
    Write-Host ''

    # -----------------------------------------------------------------------
    Write-Host '--- A. keys ---'
    # -----------------------------------------------------------------------

    Step 'KEY1 starts the system' { Inject-Key $KEY_START 'short' } `
         -Expect 'SYSTEM_START'

    Step 'KEY2 switches to MANUAL' { Inject-Key $KEY_MODE 'short' } `
         -Expect 'MODE_CHANGE'

    Step 'KEY3 opens (manual open works in MANUAL)' { Inject-Key $KEY_OPEN 'short' } `
         -Expect 'OPEN_START'

    Step 'door reaches OPEN' { } -Expect 'OPEN_DONE' -TimeoutMs 6000

    Step 'MANUAL mode does NOT auto-close' { } -NotExpect 'CLOSE_START' -QuietMs 2500

    Step 'KEY4 closes (short press)' { Inject-Key $KEY_CLOSE_ESTOP 'short' } `
         -Expect 'CLOSE_START'

    Step 'door returns to IDLE' { } -Expect '-> IDLE\b' -TimeoutMs 6000

    Step 'KEY2 back to AUTO' { Inject-Key $KEY_MODE 'short' } -Expect 'MODE_CHANGE'

    # -----------------------------------------------------------------------
    Write-Host ''
    Write-Host '--- B. simulated IR sensors (edge events) ---'
    # -----------------------------------------------------------------------

    Step 'outside sensor opens the door' { Inject-Sensor 'outside' 'arrive' } `
         -Expect 'ENTER_IN'

    Step 'door reaches OPEN' { } -Expect 'OPEN_DONE' -TimeoutMs 6000

    Step 'person leaves -> auto-close after DELAY' {
        Inject-Sensor 'outside' 'leave'
    } -Expect 'CLOSE_START' -TimeoutMs 6000

    Step 'door returns to IDLE' { } -Expect '-> IDLE\b' -TimeoutMs 6000

    Step 'inside sensor opens the door too' { Inject-Sensor 'inside' 'arrive' } `
         -Expect 'EXIT_OUT'

    Step 'door reaches OPEN' { } -Expect 'OPEN_DONE' -TimeoutMs 6000
    Step 'person leaves' { Inject-Sensor 'inside' 'leave' } -Expect 'CLOSE_START' -TimeoutMs 6000
    Step 'door returns to IDLE' { } -Expect '-> IDLE\b' -TimeoutMs 6000

    # -----------------------------------------------------------------------
    Write-Host ''
    Write-Host '--- C. anti-crush: a person arriving while the door closes ---'
    # -----------------------------------------------------------------------

    Step 'outside sensor opens the door' { Inject-Sensor 'outside' 'arrive' } -Expect 'ENTER_IN'
    Step 'door reaches OPEN' { } -Expect 'OPEN_DONE' -TimeoutMs 6000
    Step 'person leaves, door starts closing' {
        Inject-Sensor 'outside' 'leave'
    } -Expect 'CLOSE_START' -TimeoutMs 6000

    Step 'a person during CLOSING reverses the door' {
        Inject-Sensor 'inside' 'arrive'
    } -Expect 'REVERSE' -TimeoutMs 4000

    Step 'it re-opens instead of closing' { } -Expect 'OPEN_DONE' -TimeoutMs 6000

    Step 'door stays open while the person is there' { } -NotExpect 'CLOSE_START' -QuietMs 2500
    Step 'person leaves -> closes' { Inject-Sensor 'inside' 'leave' } -Expect 'CLOSE_START' -TimeoutMs 6000
    Step 'door returns to IDLE' { } -Expect '-> IDLE\b' -TimeoutMs 6000

    # -----------------------------------------------------------------------
    Write-Host ''
    Write-Host '--- D. the lost-edge safety net (level only, no event) ---'
    # -----------------------------------------------------------------------

    Step 'outside sensor opens the door' { Inject-Sensor 'outside' 'arrive' } -Expect 'ENTER_IN'
    Step 'door reaches OPEN' { } -Expect 'OPEN_DONE' -TimeoutMs 6000
    Step 'person leaves, door starts closing' {
        Inject-Sensor 'outside' 'leave'
    } -Expect 'CLOSE_START' -TimeoutMs 6000

    # No event this time - only the level, which is what survives a missed edge.
    Step 'LEVEL alone during CLOSING still reverses' {
        Inject-Sensor 'outside' 'level-on'
    } -Expect 'REVERSE' -TimeoutMs 4000

    Step 'it re-opens' { } -Expect 'OPEN_DONE' -TimeoutMs 6000
    Step 'held level keeps it open (no auto-close)' { } -NotExpect 'CLOSE_START' -QuietMs 2500
    Step 'level released -> closes' { Inject-Sensor 'outside' 'level-off' } -Expect 'CLOSE_START' -TimeoutMs 6000
    Step 'door returns to IDLE' { } -Expect '-> IDLE\b' -TimeoutMs 6000

    # -----------------------------------------------------------------------
    Write-Host ''
    Write-Host '--- E. refusal paths: fault, disabled, occupied ---'
    # -----------------------------------------------------------------------

    Step 'open the door again' { Inject-Key $KEY_OPEN 'short' } -Expect 'OPEN_START'
    Step 'door reaches OPEN' { } -Expect 'OPEN_DONE' -TimeoutMs 6000

    Step 'a held level, then KEY4: close must be refused' {
        Inject-Sensor 'outside' 'level-on'
        Start-Sleep -Milliseconds 200
        Inject-Key $KEY_CLOSE_ESTOP 'short'
    } -NotExpect 'CLOSE_START' -QuietMs 2000

    Step 'the door is still OPEN' { } -NotExpect 'CLOSING' -QuietMs 500

    Step 'level released, KEY4 now closes' {
        Inject-Sensor 'outside' 'level-off'
        Start-Sleep -Milliseconds 200
        Inject-Key $KEY_CLOSE_ESTOP 'short'
    } -Expect 'CLOSE_START' -TimeoutMs 4000

    Step 'door returns to IDLE' { } -Expect '-> IDLE\b' -TimeoutMs 6000

    Step 'KEY4 long press is the emergency stop' { Inject-Key $KEY_CLOSE_ESTOP 'long' } `
         -Expect 'ESTOP' -TimeoutMs 4000

    Step 'the fault is latched' { Send-Cmd 'STATUS?' } -Expect 'FAULT=ESTOP' -TimeoutMs 3000

    Step 'KEY3 while faulted must not open' { Inject-Key $KEY_OPEN 'short' } `
         -NotExpect 'OPEN_START' -QuietMs 2000

    Step 'a sensor while faulted must not open either' {
        Inject-Sensor 'outside' 'arrive'
    } -NotExpect 'OPEN_START' -QuietMs 2000

    Step 'RESET clears the latch' { Send-Cmd 'RESET' } -Expect 'OK RESET' -TimeoutMs 3000

    # Door_Reset() drops the enable latch as well, so RESET leaves the system
    # DISABLED - the same state a power-up starts in. That is what makes the
    # refusal steps here meaningful, and it is why the chain re-enables with KEY1
    # afterwards instead of assuming the door is still running.
    Step 'fault is gone and the system is disabled' { Send-Cmd 'STATUS?' } `
         -Expect 'FAULT=NONE' -TimeoutMs 3000

    Step 'sensor while disabled must not open' {
        Inject-Sensor 'outside' 'arrive'
    } -NotExpect 'OPEN_START' -QuietMs 2000

    Step 'KEY2 while disabled must not change mode' {
        Inject-Key $KEY_MODE 'short'
    } -NotExpect 'MODE_CHANGE' -QuietMs 2000

    Step 'KEY1 enables the system' { Inject-Key $KEY_START 'short' } -Expect 'SYSTEM_START' -TimeoutMs 4000

    Step 'the same sensor now opens the door' {
        Inject-Sensor 'outside' 'arrive'
    } -Expect 'OPEN_START' -TimeoutMs 4000

    Step 'KEY1 stops it again' { Inject-Key $KEY_START 'short' } -Expect 'SYSTEM_STOP' -TimeoutMs 4000

    Step 'and the door is unresponsive again' {
        Inject-Sensor 'outside' 'arrive'
    } -NotExpect 'OPEN_START' -QuietMs 2000

    # -----------------------------------------------------------------------
    Write-Host ''
    Write-Host '--- F. screen navigation: a long press must always get you home ---'
    # -----------------------------------------------------------------------

    # There are three screens and KEY2 long press cycles STATUS -> EVENT -> LOG.
    # The event screen ("Recent events") is the one that looks like a log view,
    # and with no EEPROM it is the only one that shows anything - the log browser
    # just says "(no records)". Long pressing KEY3 on it used to do nothing, which
    # is exactly how it was reported. None of this reaches the console, so the
    # screen is read back over SWD.
    Step-Var 'KEY2 long -> event screen' { Inject-Key $KEY_MODE 'long' } `
             -Expr "'Display.c'::s_screen" -ExpectRegex 'DISP_SCREEN_EVENT'

    Step-Var 'KEY3 long leaves the EVENT screen' { Inject-Key $KEY_OPEN 'long' } `
             -Expr "'Display.c'::s_screen" -ExpectRegex 'DISP_SCREEN_STATUS'

    Step-Var 'KEY2 long -> event screen again' { Inject-Key $KEY_MODE 'long' } `
             -Expr "'Display.c'::s_screen" -ExpectRegex 'DISP_SCREEN_EVENT'

    Step-Var 'KEY2 long -> log screen' { Inject-Key $KEY_MODE 'long' } `
             -Expr "'Display.c'::s_screen" -ExpectRegex 'DISP_SCREEN_LOG'

    Step-Var 'KEY3 long leaves the LOG screen' { Inject-Key $KEY_OPEN 'long' } `
             -Expr "'Display.c'::s_screen" -ExpectRegex 'DISP_SCREEN_STATUS'

    Step-Var 'KEY3 long on the status screen is a harmless no-op' { Inject-Key $KEY_OPEN 'long' } `
             -Expr "'Display.c'::s_screen" -ExpectRegex 'DISP_SCREEN_STATUS'

    # -----------------------------------------------------------------------
    Write-Host ''
    Write-Host '--- G. runtime parameters: TRAVEL= ---'
    # -----------------------------------------------------------------------

    # The SG90 build has no duty to set: the servo's speed is the servo's, and the
    # only knob shaping motion is the travel time. SPEED= answers ERR N/A here
    # deliberately, so the first step pins that down rather than leaving a stale
    # "OK SPEED=50%" expectation in the chain.
    Step 'SPEED= is refused on the servo build' { Send-Cmd 'SPEED=50' } `
         -Expect 'ERR N/A - SG90 speed is fixed' -TimeoutMs 3000

    # The parenthesised figure is the pulse movement per 20 ms servo frame - the
    # number that decides whether the door sweeps or steps - so it is asserted too,
    # not just the milliseconds that were typed.
    Step 'TRAVEL= sets the travel time' { Send-Cmd 'TRAVEL=1800' } `
         -Expect 'OK TRAVEL=1800ms \(11 us of pulse per 20 ms frame\)' -TimeoutMs 3000

    Step 'STATUS? reports the travel time' { Send-Cmd 'STATUS?' } `
         -Expect 'TRAVEL=1800ms' -TimeoutMs 3000

    # 600..4000 is what Motor_SetTravelMs() accepts. Below the floor the pulse
    # moves less than the SG90 dead band per frame and the door steps; the point of
    # refusing is that the operator is told, instead of watching a door that
    # "moves in three jumps".
    Step 'a travel time below the floor is refused' { Send-Cmd 'TRAVEL=200' } `
         -Expect 'ERR RANGE 600\.\.4000' -TimeoutMs 3000

    Step 'a non-numeric travel time is refused' { Send-Cmd 'TRAVEL=fast' } `
         -Expect 'ERR BAD_ARG' -TimeoutMs 3000

    Step 'DEFAULTS puts the travel time back' { Send-Cmd 'DEFAULTS' } `
         -Expect 'travel=1500ms' -TimeoutMs 3000
} catch {
    # Remembered, not rethrown here: exit inside a finally block terminates the
    # script and would swallow the reason. It is reported after the summary.
    $fatal = $_
} finally {
    # Leave the board the way a power-up would: no injected inputs, defaults
    # restored, system disabled. The sensors are released first so nothing is
    # left asserting presence.
    try {
        if ($sp.IsOpen) {
            Send-Cmd 'RESET'
            Start-Sleep -Milliseconds 200
            Send-Cmd 'DELAY=5000'
            Start-Sleep -Milliseconds 200
            Send-Cmd 'MODE=AUTO'
            Start-Sleep -Milliseconds 400
        }
    } catch { }
    try { [void](Invoke-Gdb @("set var 'Sensor.c'::s_outside.stable = 0", "set var 'Sensor.c'::s_outside.event = 0",
                              "set var 'Sensor.c'::s_inside.stable = 0", "set var 'Sensor.c'::s_inside.event = 0")) } catch { }
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()

    $pass = @($results | Where-Object { $_.Pass }).Count
    Write-Host ''
    Write-Host ("=== {0}/{1} passed ===" -f $pass, $results.Count)
    $failed = @($results | Where-Object { -not $_.Pass })
    if ($failed.Count) {
        Write-Host ''
        foreach ($f in $failed) {
            Write-Host ("FAILED [{0}] {1}" -f $f.No, $f.Name)
            Write-Host ("  expected : {0}" -f $f.Expect)
            Write-Host ("  state    : {0}" -f $f.Status)
            Write-Host ("  seen     : {0}" -f (Show $f.Reply.Trim()))
        }
    }

    # Only stop OpenOCD if this script started it; otherwise it belongs to
    # whoever launched it (an IDE, or a previous run that passed -KeepServer).
    if ($ocdProc -and -not $KeepServer) {
        try { Stop-Process -Id $ocdProc.Id -Force -ErrorAction SilentlyContinue } catch { }
        Write-Host ''
        Write-Host "OpenOCD (pid $($ocdProc.Id)) stopped; the ST-Link is free again."
    }
}

if ($fatal) {
    Write-Host ''
    Write-Host ("ABORTED before finishing: {0}" -f $fatal.Exception.Message)
    exit 2
}
if ($failed.Count) { exit 1 }
exit 0
