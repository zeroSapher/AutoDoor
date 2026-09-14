cmake_minimum_required(VERSION 3.20)

project(AutoDoor C ASM)

# ---------------------------------------------------------------------------
# Build directory convention
# ---------------------------------------------------------------------------
# Configure with -B build (lower-case). The stm32-toolchain-mcp server defaults
# buildDir to "build" in stm32_build / stm32_clean, and tools/build.ps1 and the
# Makefile use the same name, so all entry points share one output tree.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # the StdPeriph library is GNU-C flavoured

set(TARGET        AutoDoor)
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/Linker/stm32f103c8t6.ld)

# Cortex-M3, little-endian, Thumb only.
set(MCU_FLAGS -mcpu=cortex-m3 -mthumb -mfloat-abi=soft)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(CMSIS_SOURCES
  Drivers/CMSIS/system_stm32f10x.c
  Drivers/CMSIS/core_cm3.c
)

# Startup and vector table.
#
# Drivers/CMSIS/startup_stm32f10x_md.s is the ST-supplied Keil/ARM-syntax file
# (AREA/EXPORT/PROC/DCD) and cannot be assembled by GNU as, so the GNU build uses
# the equivalent C implementation. The Keil project uses the .s. Never add both:
# each defines Reset_Handler and the vector table.
set(STARTUP_SOURCE
  Drivers/CMSIS/startup_stm32f10x_md_gcc.c
)

# Only the StdPeriph modules listed in Core/stm32f10x_conf.h are compiled, which
# keeps build times and code size down. Add a file here when a new module is
# enabled in the config header.
set(STDPERIPH_SOURCES
  Drivers/StdPeriph/misc.c
  Drivers/StdPeriph/stm32f10x_rcc.c
  Drivers/StdPeriph/stm32f10x_gpio.c
  Drivers/StdPeriph/stm32f10x_exti.c
  Drivers/StdPeriph/stm32f10x_usart.c
  Drivers/StdPeriph/stm32f10x_tim.c
)

set(HARDWARE_SOURCES
  Hardware/Delay/Delay.c
  Hardware/MyI2C/MyI2C.c
  Hardware/OLED/OLED.c
  Hardware/OLED/OLED_Data.c
  Hardware/EEPROM/EEPROM.c
  Hardware/UART/UART.c
  Hardware/Motor/Motor.c
  Hardware/Limit/Limit.c
  Hardware/Buzzer/Buzzer.c
  Hardware/Led/StatusLed.c
  Hardware/Input/Debounce.c
  Hardware/Input/Exti.c
  Hardware/Input/Sensor.c
  Hardware/Input/Key.c
)

set(CORE_SOURCES
  Core/main.c
  Core/stm32f10x_it.c
)

set(APP_SOURCES_LIST
  App/Log.c
  App/Door.c
  App/Cmd.c
  App/Display.c
  App/Display_Oled.c
  App/Fmt.c
)

set(APP_SOURCES
  ${CMSIS_SOURCES}
  ${STDPERIPH_SOURCES}
  ${HARDWARE_SOURCES}
  ${CORE_SOURCES}
  ${APP_SOURCES_LIST}
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(${TARGET} ${APP_SOURCES} ${STARTUP_SOURCE})

# A bare-metal CMake target has no default suffix; name the artifact explicitly
# so flashing tools and the MCP key off the .elf extension.
set_target_properties(${TARGET} PROPERTIES SUFFIX ".elf")

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
target_include_directories(${TARGET} PRIVATE
  Core
  Drivers/CMSIS
  Drivers/StdPeriph
  Hardware/Delay
  Hardware/MyI2C
  Hardware/OLED
  Hardware/EEPROM
  Hardware/UART
  Hardware/Motor
  Hardware/Limit
  Hardware/Buzzer
  Hardware/Led
  Hardware/Input
  App
)

# ---------------------------------------------------------------------------
# Defines
# ---------------------------------------------------------------------------
#   USE_STDPERIPH_DRIVER : makes stm32f10x.h pull in stm32f10x_conf.h
#   STM32F10X_MD         : medium-density device (64/128 KB flash)
#   HSE_VALUE            : crystal on the board, used to derive 72 MHz
target_compile_definitions(${TARGET} PRIVATE
  USE_STDPERIPH_DRIVER
  STM32F10X_MD
  HSE_VALUE=8000000
)

# ---------------------------------------------------------------------------
# Compile options
# ---------------------------------------------------------------------------
target_compile_options(${TARGET} PRIVATE
  ${MCU_FLAGS}
  -Wall
  -Wextra
  -Wshadow
  -Wdouble-promotion
  -Og
  -g3
  -ffunction-sections
  -fdata-sections
)

# The JiangXie font tables are flat array initialisers without inner braces, and
# the Chinese glyph rows are 3-byte arrays deliberately initialised from a 3-byte
# UTF-8 sequence. Both are valid C and intentional upstream, so the diagnostics
# are silenced for that translation unit only rather than editing the font data.
set_source_files_properties(Hardware/OLED/OLED_Data.c PROPERTIES
  COMPILE_OPTIONS "-Wno-missing-braces;-Wno-unterminated-string-initialization"
)

# ---------------------------------------------------------------------------
# Link options
# ---------------------------------------------------------------------------
# --specs=nano.specs selects the small newlib; -u _printf_float would be needed
# to make printf handle %f (it costs several KB of flash and is not used here).
target_link_options(${TARGET} PRIVATE
  ${MCU_FLAGS}
  -T${LINKER_SCRIPT}
  -Wl,--gc-sections
  -Wl,-Map=${CMAKE_BINARY_DIR}/${TARGET}.map
  -Wl,--print-memory-usage
  --specs=nano.specs
  --specs=nosys.specs
)

# ---------------------------------------------------------------------------
# Post-build: .hex and .bin for st-flash / OpenOCD
# ---------------------------------------------------------------------------
add_custom_command(TARGET ${TARGET} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${CMAKE_BINARY_DIR}/${TARGET}.hex
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${CMAKE_BINARY_DIR}/${TARGET}.bin
  COMMAND ${CMAKE_SIZE}    $<TARGET_FILE:${TARGET}>
  COMMENT "Generating ${TARGET}.hex / ${TARGET}.bin"
)

# Convenience targets.
add_custom_target(flash
  COMMAND st-flash write ${CMAKE_BINARY_DIR}/${TARGET}.bin 0x08000000
  DEPENDS ${TARGET}
  COMMENT "Flashing ${TARGET}.bin with st-flash"
)

add_custom_target(erase
  COMMAND st-flash erase
  COMMENT "Mass-erasing the target flash"
)
