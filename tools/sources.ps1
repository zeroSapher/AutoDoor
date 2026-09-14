# ===========================================================================
# AutoDoor shared build definition.
#
# Dot-sourced by tools/build.ps1 (real firmware) and tools/build-sim.ps1
# (Proteus simulation firmware). Keeping one authoritative list here is the
# only way the two builds cannot silently drift apart - and "keep these lists in
# sync" comments do not survive contact with a deadline.
#
# Defines:
#   $Target        output base name
#   $BaseSources   translation units for both builds
#   $IncludeDirs   include search path
#   $DefineBase    preprocessor defines common to both builds
#   $PerFileFlags  per-file compiler option overrides
# ===========================================================================

$BaseSources = @(
    'Core/main.c'
    'Core/stm32f10x_it.c'
    'Drivers/CMSIS/system_stm32f10x.c'
    'Drivers/CMSIS/core_cm3.c'
    'Drivers/CMSIS/startup_stm32f10x_md_gcc.c'
    'Drivers/StdPeriph/misc.c'
    'Drivers/StdPeriph/stm32f10x_rcc.c'
    'Drivers/StdPeriph/stm32f10x_gpio.c'
    'Drivers/StdPeriph/stm32f10x_exti.c'
    'Drivers/StdPeriph/stm32f10x_usart.c'
    'Drivers/StdPeriph/stm32f10x_tim.c'
    'Hardware/Delay/Delay.c'
    'Hardware/MyI2C/MyI2C.c'
    'Hardware/UART/UART.c'
    'Hardware/Motor/Motor.c'
    'Hardware/Limit/Limit.c'
    'Hardware/Buzzer/Buzzer.c'
    'Hardware/Led/StatusLed.c'
    'Hardware/Input/Debounce.c'
    'Hardware/Input/Exti.c'
    'Hardware/Input/Sensor.c'
    'Hardware/Input/Key.c'
    'App/Log.c'
    'App/Door.c'
    'App/Cmd.c'
    'App/Display.c'
    'App/Fmt.c'
)

# The real and simulation builds use the same OLED backend. The simulation keeps
# the EEPROM stub because the Proteus setup only models the display, not
# persistent AT24C32 storage.
$RealBackend = @(
    'Hardware/OLED/OLED.c'
    'Hardware/OLED/OLED_Data.c'
    'Hardware/EEPROM/EEPROM.c'
    'App/Display_Oled.c'
)

$SimBackend = @(
    'Hardware/OLED/OLED.c'
    'Hardware/OLED/OLED_Data.c'
    'Hardware/EEPROM/EEPROM_Stub.c'
    'App/Display_Oled.c'
)

# ---------------------------------------------------------------------------
# Retained but NOT compiled: the HD44780 1602 backend
# ---------------------------------------------------------------------------
#   App/Display_Lcd.c
#   Hardware/LCD1602/LCD1602.c
#   Hardware/LCD1602/LCD1602.h
#
# The simulation used to drive a 1602 because Proteus was assumed to have no
# OLED model; it now drives a 128x64 SSD1306-compatible OLED, so the 1602 backend
# is in no manifest. It is kept deliberately rather than deleted, because the
# Proteus OLED model has not been exercised yet and the 1602 is the more reliable
# Proteus component - if the OLED model turns out to be unusable, this is the
# fallback for the first execution-based verification this project has ever had.
#
# Naming them here is the point. A file that appears in no manifest is invisible
# to tools/check-sources.ps1, which compares manifests; that is the same shape as
# the orphaned-net check in tools/gen-hardware.py, which exists because "not
# referenced any more" must be a decision someone wrote down, not an accident.
#
# To revive it: re-add the LCD1602_* pin block to Core/main.h (it is in git at
# 02ea5c7), swap the two entries in $SimBackend below, restore the six nets in
# tools/gen-hardware.py's NET_TABLE, and add 'Hardware/LCD1602' back to
# $IncludeDirs.
#
# To delete it instead: `git rm App/Display_Lcd.c Hardware/LCD1602/` - nothing
# else references it.

$IncludeDirs = @(
    'Core'
    'Drivers/CMSIS'
    'Drivers/StdPeriph'
    'Hardware/Delay'
    'Hardware/MyI2C'
    'Hardware/OLED'
    'Hardware/EEPROM'
    'Hardware/UART'
    'Hardware/Motor'
    'Hardware/Limit'
    'Hardware/Buzzer'
    'Hardware/Led'
    'Hardware/Input'
    'App'
)

$DefineBase = @('-DUSE_STDPERIPH_DRIVER', '-DSTM32F10X_MD', '-DHSE_VALUE=8000000')

# The font tables are flat initialisers without inner braces, and the Chinese
# glyph rows deliberately drop the NUL terminator. Both are valid C and
# intentional upstream, so the diagnostics are disabled for that file only.
$PerFileFlags = @{
    'Hardware/OLED/OLED_Data.c' = @('-Wno-missing-braces', '-Wno-unterminated-string-initialization')
}
