cmake_minimum_required(VERSION 3.13...3.27)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(pico_swi_tool C CXX ASM)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Check if PICO_BOARD is set to "pico2" and define the preprocessor macro PICO2.
if(PICO_BOARD STREQUAL "pico2")
    add_compile_definitions(PICO2)
endif()


# Ensure PICO_SDK_PATH is set
if (NOT DEFINED PICO_SDK_PATH)
  message(FATAL_ERROR "PICO_SDK_PATH is not set. Please set it to the path of your pico-sdk.")
endif()

# Create an executable from main.c
add_executable(pico_swi_tool swi_tool.c)

# Enable USB stdio (disable UART stdio)
pico_enable_stdio_usb(pico_swi_tool 1)
pico_enable_stdio_uart(pico_swi_tool 0)

target_link_libraries(pico_swi_tool pico_stdlib pico_multicore)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(pico_swi_tool)


