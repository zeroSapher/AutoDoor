# ---------------------------------------------------------------------------
# CMake toolchain file for the ARM bare-metal (arm-none-eabi) cross compiler.
#
# Usage:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build
#
# The output directory is deliberately lower-case "build": that is what the
# stm32-toolchain-mcp server assumes by default (buildDir defaults to "build" in
# stm32_build / stm32_clean), so an agent can build this project with no extra
# arguments.
#
# If the toolchain is not on PATH, point TOOLCHAIN_PREFIX at its bin directory:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
#         -DTOOLCHAIN_PREFIX=D:/ST/gcc-arm-none-eabi/bin
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED TOOLCHAIN_PREFIX)
  set(TOOLCHAIN_PREFIX "" CACHE PATH "Directory holding arm-none-eabi-* binaries")
endif()

if(TOOLCHAIN_PREFIX)
  # Normalise so the path can be given with either slash style.
  file(TO_CMAKE_PATH "${TOOLCHAIN_PREFIX}" TOOLCHAIN_PREFIX)
  set(_TOOLCHAIN_BIN "${TOOLCHAIN_PREFIX}/")
else()
  set(_TOOLCHAIN_BIN "")
endif()

# The Windows builds of the GNU Arm toolchain ship .exe binaries, so the suffix
# has to be spelled out for CMake to accept the path as an existing tool.
if(CMAKE_HOST_WIN32)
  set(_TOOLCHAIN_EXE ".exe")
else()
  set(_TOOLCHAIN_EXE "")
endif()

set(CMAKE_C_COMPILER   "${_TOOLCHAIN_BIN}arm-none-eabi-gcc${_TOOLCHAIN_EXE}")
set(CMAKE_ASM_COMPILER "${_TOOLCHAIN_BIN}arm-none-eabi-gcc${_TOOLCHAIN_EXE}")
set(CMAKE_CXX_COMPILER "${_TOOLCHAIN_BIN}arm-none-eabi-g++${_TOOLCHAIN_EXE}")

set(CMAKE_OBJCOPY "${_TOOLCHAIN_BIN}arm-none-eabi-objcopy${_TOOLCHAIN_EXE}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${_TOOLCHAIN_BIN}arm-none-eabi-objdump${_TOOLCHAIN_EXE}" CACHE FILEPATH "" FORCE)
set(CMAKE_SIZE    "${_TOOLCHAIN_BIN}arm-none-eabi-size${_TOOLCHAIN_EXE}"    CACHE FILEPATH "" FORCE)
set(CMAKE_AR      "${_TOOLCHAIN_BIN}arm-none-eabi-ar${_TOOLCHAIN_EXE}"      CACHE FILEPATH "" FORCE)

# Attempting to run target binaries during configure would fail.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
