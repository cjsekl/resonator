cmake_minimum_required(VERSION 3.13)

# Set board type for Raspberry Pi Pico
set(PICO_BOARD pico)

# Include Pico SDK
include(pico_sdk_import.cmake)

# Project name and languages
project(resonator C CXX ASM)

# Set C++ standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Add executable target
add_executable(resonator
    main.cpp
    usb_descriptors.cpp
)

# Link libraries required by the Pico SDK and ComputerCard
target_link_libraries(resonator
    pico_stdlib
    hardware_gpio
    hardware_pwm
    hardware_adc
    hardware_dma
    hardware_spi
    hardware_i2c
    hardware_flash
    hardware_irq
    hardware_clocks
    hardware_sync
    pico_multicore
    pico_unique_id
    tinyusb_device
    tinyusb_board
)

# Note: No longer using copy_to_ram - multicore_lockout pauses audio during flash writes

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(resonator)

target_compile_definitions(resonator PUBLIC PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

# Disable stdio (using TinyUSB MIDI directly)
pico_enable_stdio_usb(resonator 0)
pico_enable_stdio_uart(resonator 0)

# Set compiler flags for optimization
target_compile_options(resonator PRIVATE
    -Wall
    -Wextra
    -O2
    -ffast-math
    -funroll-loops
)

# Define preprocessor macros
target_compile_definitions(resonator PRIVATE
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
)

# Include directories
target_include_directories(resonator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
