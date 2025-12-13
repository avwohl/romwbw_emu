#!/bin/bash
#
# Build emulator ROM by overlaying emu_hbios on a real RomWBW ROM
#
# Usage: ./build_emu_rom.sh <source_rom> [output_rom]
#
# The source ROM should be a 512KB RomWBW ROM (e.g., SBC_simh_std.rom)
# that contains the romldr, OS images (CP/M, ZSDOS, etc), and ROM disk.
#
# This script overlays our emu_hbios_32k.bin (first 32KB) which provides:
# - Custom HBIOS proxy that traps RST 08 calls
# - HCB configuration for the emulator
# - Boot menu (romldr replacement)
#
# The rest of the ROM (banks 1-15) is preserved from the source ROM,
# keeping all the OS images and ROM disk intact.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EMU_HBIOS="$SCRIPT_DIR/emu_hbios_32k.bin"

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <source_rom> [output_rom]"
    echo ""
    echo "Build an emulator ROM by overlaying emu_hbios on a real RomWBW ROM."
    echo ""
    echo "Arguments:"
    echo "  source_rom   A 512KB RomWBW ROM file (e.g., SBC_simh_std.rom)"
    echo "  output_rom   Output file (default: emu_romwbw.rom)"
    echo ""
    echo "Example:"
    echo "  $0 SBC_simh_std.rom"
    echo "  $0 ~/RomWBW/Binary/SBC_std.rom custom_emu.rom"
    exit 1
fi

SOURCE_ROM="$1"
OUTPUT_ROM="${2:-$SCRIPT_DIR/emu_romwbw.rom}"

# Verify source ROM exists
if [ ! -f "$SOURCE_ROM" ]; then
    echo "Error: Source ROM not found: $SOURCE_ROM"
    exit 1
fi

# Check source ROM size (should be 512KB = 524288 bytes)
SOURCE_SIZE=$(stat -f%z "$SOURCE_ROM" 2>/dev/null || stat -c%s "$SOURCE_ROM" 2>/dev/null)
if [ "$SOURCE_SIZE" -ne 524288 ]; then
    echo "Warning: Source ROM is $SOURCE_SIZE bytes (expected 524288 for 512KB)"
    if [ "$SOURCE_SIZE" -lt 32768 ]; then
        echo "Error: Source ROM too small"
        exit 1
    fi
fi

# Verify emu_hbios exists
if [ ! -f "$EMU_HBIOS" ]; then
    echo "Error: emu_hbios_32k.bin not found at $EMU_HBIOS"
    echo "Please assemble emu_hbios.asm first."
    exit 1
fi

# Check emu_hbios size (should be 32KB = 32768 bytes)
HBIOS_SIZE=$(stat -f%z "$EMU_HBIOS" 2>/dev/null || stat -c%s "$EMU_HBIOS" 2>/dev/null)
if [ "$HBIOS_SIZE" -ne 32768 ]; then
    echo "Error: emu_hbios_32k.bin is $HBIOS_SIZE bytes (expected 32768)"
    exit 1
fi

# Build the ROM:
# 1. Copy source ROM to output
# 2. Overlay first 32KB with emu_hbios
echo "Building emulator ROM..."
echo "  Source: $SOURCE_ROM ($SOURCE_SIZE bytes)"
echo "  HBIOS:  $EMU_HBIOS ($HBIOS_SIZE bytes)"
echo "  Output: $OUTPUT_ROM"

# Copy source ROM
cp "$SOURCE_ROM" "$OUTPUT_ROM"

# Overlay first 32KB with emu_hbios
dd if="$EMU_HBIOS" of="$OUTPUT_ROM" bs=32768 count=1 conv=notrunc 2>/dev/null

# Verify output
OUTPUT_SIZE=$(stat -f%z "$OUTPUT_ROM" 2>/dev/null || stat -c%s "$OUTPUT_ROM" 2>/dev/null)
echo ""
echo "Created $OUTPUT_ROM ($OUTPUT_SIZE bytes)"
echo ""

# Show what's in bank 0 (our HBIOS)
echo "Bank 0 (emu_hbios):"
echo "  0x0000-0x00FF: RST vectors, jump to 0xFFF0"
echo "  0x0100-0x01FF: HCB (HBIOS Configuration Block)"
echo "  0x0200+: Boot loader and HBIOS proxy code"
echo ""

# Show what's preserved from source ROM
echo "Banks 1-15 (from source ROM):"
echo "  Bank 1: romldr (RomWBW boot loader)"
echo "  Banks 2+: OS images, ROM disk content"
echo ""
echo "Use with: altair_emu --romwbw $OUTPUT_ROM"
