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

# The display differs by build, and both panels must never be linked at once:
# the OLED backend pulls in the bitmap driver and its font tables, the LCD
# backend pulls in the HD44780 driver, and only one set of DispBk_* symbols may
# exist. See App/Display_Backend.h.
$RealBackend = @(
    'Hardware/OLED/OLED.c'
    'Hardware/OLED/OLED_Data.c'
    'Hardware/EEPROM/EEPROM.c'
    'App/Display_Oled.c'
)

$SimBackend = @(
    'Hardware/LCD1602/LCD1602.c'
    'Hardware/EEPROM/EEPROM_Stub.c'
    'App/Display_Lcd.c'
)

$IncludeDirs = @(
    'Core'
    'Drivers/CMSIS'
    'Drivers/StdPeriph'
    'Hardware/Delay'
    'Hardware/MyI2C'
    'Hardware/OLED'
    'Hardware/LCD1602'
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
