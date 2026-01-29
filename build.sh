#!/bin/bash

# Workshop System Resonator Build Script
# Builds the resonator for Music Thing Modular Workshop System

set -e  # Exit on any error

echo "========================================="
echo "Workshop System Resonator Build Script"
echo "========================================="

# Check for required tools
echo "Checking for required build tools..."

if ! command -v cmake &> /dev/null; then
    echo "ERROR: cmake not found!"
    echo "Please install cmake:"
    echo "  Ubuntu/Debian: sudo apt install cmake"
    echo "  Arch Linux: sudo pacman -S cmake"
    echo "  macOS: brew install cmake"
    exit 1
fi

if ! command -v arm-none-eabi-gcc &> /dev/null; then
    echo "ERROR: ARM GCC toolchain not found!"
    echo "Please install arm-none-eabi-gcc:"
    echo "  Ubuntu/Debian: sudo apt install gcc-arm-none-eabi"
    echo "  Arch Linux: sudo pacman -S arm-none-eabi-gcc"
    echo "  macOS: brew install --cask gcc-arm-embedded"
    exit 1
fi

if ! command -v make &> /dev/null; then
    echo "ERROR: make not found!"
    echo "Please install build-essential:"
    echo "  Ubuntu/Debian: sudo apt install build-essential"
    echo "  Arch Linux: sudo pacman -S base-devel"
    echo "  macOS: xcode-select --install"
    exit 1
fi

echo "All required tools found!"

# Check if Pico SDK is available
if [ -z "$PICO_SDK_PATH" ]; then
    echo "Warning: PICO_SDK_PATH not set. Attempting to auto-download SDK..."
    export PICO_SDK_FETCH_FROM_GIT=1
fi

# Create build directory
echo "Creating build directory..."
mkdir -p build
cd build

# Configure with CMake
echo "Configuring project with CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build the project
echo "Building resonator..."
make -j$(nproc)

# Check if .uf2 file was created
if [ -f "resonator.uf2" ]; then
    echo ""
    echo "========================================="
    echo "BUILD SUCCESSFUL!"
    echo "========================================="
    echo "Generated files:"
    echo "  resonator.uf2    - Flash file for Workshop System"
    echo "  resonator.elf    - Debug executable"
    echo "  resonator.bin    - Binary file"
    echo "  resonator.hex    - Hex file"
    echo ""
    echo "To flash to Workshop System:"
    echo "1. Hold down BOOTSEL button on the computer module"
    echo "2. Connect USB cable"
    echo "3. Release BOOTSEL button"
    echo "4. Copy resonator.uf2 to the RPI-RP2 drive"
    echo ""
    echo "File location: $(pwd)/resonator.uf2"
    echo "========================================="
else
    echo ""
    echo "========================================="
    echo "BUILD FAILED!"
    echo "========================================="
    echo "The .uf2 file was not generated. Check the build output for errors."
    exit 1
fi
