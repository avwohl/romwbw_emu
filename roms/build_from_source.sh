#!/bin/bash
#
# Build emulator ROM from RomWBW source
#
# This script builds a complete ROM by:
# 1. Using RomWBW SBC_simh_std.rom for banks 1-15 (romldr, OS images, ROM disk)
# 2. Assembling our emu_hbios.asm for bank 0 (minimal HBIOS with OUT 0xEF dispatch)
# 3. Combining them into a single 512KB ROM
#
# Usage: ./build_from_source.sh [output_rom]
#
# Requirements:
# - um80 and ul80 assemblers (Z80 tools)
# - RomWBW v3.5.1 source/binary at ~/esrc/RomWBW-v3.5.1
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(dirname "$SCRIPT_DIR")/src"
OUTPUT_ROM="${1:-$SCRIPT_DIR/emu_avw.rom}"

# RomWBW paths
ROMWBW_DIR="${ROMWBW_DIR:-$HOME/esrc/RomWBW-v3.5.1}"
ROMWBW_ROM="$ROMWBW_DIR/Binary/SBC_simh_std.rom"

# Assembler tools
UM80="${UM80:-um80}"
UL80="${UL80:-ul80}"

echo "========================================"
echo "Building emulator ROM from source"
echo "========================================"
echo ""

# Check for RomWBW source ROM
if [ ! -f "$ROMWBW_ROM" ]; then
    echo "Error: RomWBW ROM not found at: $ROMWBW_ROM"
    echo ""
    echo "Please ensure RomWBW v3.5.1 is installed at $ROMWBW_DIR"
    echo "or set ROMWBW_DIR environment variable."
    echo ""
    echo "You can download RomWBW from:"
    echo "  https://github.com/wwarthen/RomWBW/releases"
    exit 1
fi

# Check for assembler tools
if ! command -v "$UM80" &> /dev/null; then
    echo "Error: um80 assembler not found"
    echo "Please install um80 or set UM80 environment variable."
    exit 1
fi

if ! command -v "$UL80" &> /dev/null; then
    echo "Error: ul80 linker not found"
    echo "Please install ul80 or set UL80 environment variable."
    exit 1
fi

# Check source file exists
if [ ! -f "$SRC_DIR/emu_hbios.asm" ]; then
    echo "Error: emu_hbios.asm not found at: $SRC_DIR/emu_hbios.asm"
    exit 1
fi

echo "Source ROM:  $ROMWBW_ROM"
echo "HBIOS src:   $SRC_DIR/emu_hbios.asm"
echo "Output:      $OUTPUT_ROM"
echo ""

# Step 1: Assemble emu_hbios.asm
echo "Step 1: Assembling emu_hbios.asm..."

# Clean up old files
rm -f "$SRC_DIR/emu_hbios.rel" "$SRC_DIR/emu_hbios.sym" "$SCRIPT_DIR/emu_hbios.bin"

# Assemble (creates .rel in same dir as source)
$UM80 -g "$SRC_DIR/emu_hbios.asm"
if [ ! -f "$SRC_DIR/emu_hbios.rel" ]; then
    echo "Error: Assembly failed - emu_hbios.rel not created"
    exit 1
fi

# Link
$UL80 -o "$SCRIPT_DIR/emu_hbios.bin" -p 0000 "$SRC_DIR/emu_hbios.rel"
if [ ! -f "$SCRIPT_DIR/emu_hbios.bin" ]; then
    echo "Error: Link failed - emu_hbios.bin not created"
    exit 1
fi

HBIOS_SIZE=$(stat -c%s "$SCRIPT_DIR/emu_hbios.bin" 2>/dev/null || stat -f%z "$SCRIPT_DIR/emu_hbios.bin" 2>/dev/null)
echo "  Assembled: emu_hbios.bin ($HBIOS_SIZE bytes)"

# Clean up intermediate files
rm -f "$SRC_DIR/emu_hbios.rel" "$SRC_DIR/emu_hbios.sym"

# Step 2: Pad to 32KB
echo "Step 2: Padding HBIOS to 32KB..."

# Create 32KB file filled with zeros
dd if=/dev/zero bs=32768 count=1 of="$SCRIPT_DIR/emu_hbios_32k.bin" 2>/dev/null

# Copy assembled code at the start
dd if="$SCRIPT_DIR/emu_hbios.bin" of="$SCRIPT_DIR/emu_hbios_32k.bin" conv=notrunc 2>/dev/null

PADDED_SIZE=$(stat -c%s "$SCRIPT_DIR/emu_hbios_32k.bin" 2>/dev/null || stat -f%z "$SCRIPT_DIR/emu_hbios_32k.bin" 2>/dev/null)
echo "  Created: emu_hbios_32k.bin ($PADDED_SIZE bytes)"

# Step 3: Build combined ROM
echo "Step 3: Building combined ROM..."

# Start with empty 512KB file
dd if=/dev/zero bs=524288 count=1 of="$OUTPUT_ROM" 2>/dev/null

# Copy our HBIOS to bank 0 (first 32KB)
dd if=emu_hbios_32k.bin of="$OUTPUT_ROM" bs=32768 count=1 conv=notrunc 2>/dev/null

# Copy banks 1-15 from RomWBW ROM (skip first 32KB, copy remaining 480KB)
dd if="$ROMWBW_ROM" of="$OUTPUT_ROM" bs=32768 skip=1 seek=1 count=15 conv=notrunc 2>/dev/null

OUTPUT_SIZE=$(stat -c%s "$OUTPUT_ROM" 2>/dev/null || stat -f%z "$OUTPUT_ROM" 2>/dev/null)
echo "  Created: $OUTPUT_ROM ($OUTPUT_SIZE bytes)"

echo ""
echo "========================================"
echo "Build complete!"
echo "========================================"
echo ""
echo "ROM Layout:"
echo "  Bank 0 (0x00000-0x07FFF): emu_hbios (emulator HBIOS proxy)"
echo "  Bank 1 (0x08000-0x0FFFF): romldr (RomWBW boot loader)"
echo "  Bank 2+: OS images (CP/M, ZSDOS), ROM disk"
echo ""
echo "To use:"
echo "  ./romwbw_emu --romwbw $OUTPUT_ROM --hbdisk0=<disk_image>"
echo ""
