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
# - the pinned RomWBW release unpacked at ~/esrc/RomWBW-v<pin>
#   (the pin lives in src/romwbw_pin.h; run roms/verify_romwbw_pin.sh to
#   check a finished tree against it)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(dirname "$SCRIPT_DIR")/src"
OUTPUT_ROM="${1:-$SCRIPT_DIR/emu_avw.rom}"

# The RomWBW release to build against comes from the pin, not from a literal
# here: bank 0 is assembled from emu_hbios.asm, which stamps the pinned
# version into the HCB, so banks 1-15 have to come from the same release or
# the guest's CBIOS reports an HBIOS/CBIOS version mismatch.
PIN_H="$SRC_DIR/romwbw_pin.h"
ROMWBW_PIN=$(sed -n 's/^#define ROMWBW_PIN_STR "\(.*\)".*/\1/p' "$PIN_H" 2>/dev/null | head -1)
if [ -z "$ROMWBW_PIN" ]; then
    echo "Error: cannot read ROMWBW_PIN_STR from $PIN_H"
    exit 1
fi

# RomWBW paths
ROMWBW_DIR="${ROMWBW_DIR:-$HOME/esrc/RomWBW-v$ROMWBW_PIN}"
ROMWBW_ROM="$ROMWBW_DIR/Binary/SBC_simh_std.rom"

# Assembler tools
UM80="${UM80:-um80}"
UL80="${UL80:-ul80}"

echo "========================================"
echo "Building emulator ROM from source"
echo "  pinned RomWBW release: v$ROMWBW_PIN"
echo "========================================"
echo ""

# Fall back to the copy of the pinned release's ROM kept in this repo, so a
# fresh clone can rebuild bank 0 without downloading the 199MB Package.zip.
# Only banks 1-15 are taken from it, and only after confirming it carries the
# pinned version - an unpinned ROM here is exactly the mistake to catch.
if [ ! -f "$ROMWBW_ROM" ] && [ -f "$SCRIPT_DIR/SBC_simh_std.rom" ]; then
    bundled_ver=$(od -An -tx1 -j 261 -N 2 "$SCRIPT_DIR/SBC_simh_std.rom" |
                  tr -d ' \n')
    want_ver=$(printf '%x%x%x%x' \
        "$(sed -n 's/^#define ROMWBW_PIN_MAJOR \([0-9]*\).*/\1/p' "$PIN_H")" \
        "$(sed -n 's/^#define ROMWBW_PIN_MINOR \([0-9]*\).*/\1/p' "$PIN_H")" \
        "$(sed -n 's/^#define ROMWBW_PIN_UPDATE \([0-9]*\).*/\1/p' "$PIN_H")" \
        "$(sed -n 's/^#define ROMWBW_PIN_PATCH \([0-9]*\).*/\1/p' "$PIN_H")")
    if [ "$bundled_ver" = "$want_ver" ]; then
        echo "Using the bundled RomWBW v$ROMWBW_PIN ROM: $SCRIPT_DIR/SBC_simh_std.rom"
        echo ""
        ROMWBW_ROM="$SCRIPT_DIR/SBC_simh_std.rom"
    else
        echo "Ignoring $SCRIPT_DIR/SBC_simh_std.rom: HCB version bytes are"
        echo "$bundled_ver, expected $want_ver for the pinned v$ROMWBW_PIN."
        echo ""
    fi
fi

# Check for RomWBW source ROM
if [ ! -f "$ROMWBW_ROM" ]; then
    echo "Error: RomWBW ROM not found at: $ROMWBW_ROM"
    echo ""
    echo "Please unpack RomWBW v$ROMWBW_PIN at $ROMWBW_DIR"
    echo "or set the ROMWBW_DIR environment variable."
    echo ""
    echo "Download the pinned release Package.zip from:"
    echo "  https://github.com/wwarthen/RomWBW/releases/tag/v$ROMWBW_PIN"
    echo ""
    echo "Use that release specifically. A different one produces banks 1-15"
    echo "whose CBIOS does not match the HBIOS version in bank 0, and the"
    echo "guest reports 'HBIOS/CBIOS Version Mismatch'."
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

# Copy our HBIOS to bank 0 (first 32KB). $SCRIPT_DIR, not a bare name: this
# read the file relative to the caller's cwd, so running the script from
# anywhere but roms/ left bank 0 all zeros - a 512KB ROM that looks fine and
# dies at startup with no output. dd's stderr is no longer discarded either.
dd if="$SCRIPT_DIR/emu_hbios_32k.bin" of="$OUTPUT_ROM" bs=32768 count=1 conv=notrunc

# Copy banks 1-15 from RomWBW ROM (skip first 32KB, copy remaining 480KB)
dd if="$ROMWBW_ROM" of="$OUTPUT_ROM" bs=32768 skip=1 seek=1 count=15 conv=notrunc

OUTPUT_SIZE=$(stat -c%s "$OUTPUT_ROM" 2>/dev/null || stat -f%z "$OUTPUT_ROM" 2>/dev/null)
echo "  Created: $OUTPUT_ROM ($OUTPUT_SIZE bytes)"

# Never leave a broken ROM behind: confirm bank 0 really carries the HCB with
# the pinned version before declaring success.
built_hcb=$(od -An -tx1 -j 259 -N 4 "$OUTPUT_ROM" | tr -d ' \n')
want_hcb="57a8$(printf '%x%x%x%x' \
    "$(sed -n 's/^#define ROMWBW_PIN_MAJOR \([0-9]*\).*/\1/p' "$PIN_H")" \
    "$(sed -n 's/^#define ROMWBW_PIN_MINOR \([0-9]*\).*/\1/p' "$PIN_H")" \
    "$(sed -n 's/^#define ROMWBW_PIN_UPDATE \([0-9]*\).*/\1/p' "$PIN_H")" \
    "$(sed -n 's/^#define ROMWBW_PIN_PATCH \([0-9]*\).*/\1/p' "$PIN_H")")"
if [ "$built_hcb" != "$want_hcb" ]; then
    echo ""
    echo "Error: the ROM just built has HCB bytes $built_hcb at 0x103,"
    echo "expected $want_hcb for the pinned RomWBW v$ROMWBW_PIN."
    echo "Removing $OUTPUT_ROM so a broken image is not left behind."
    rm -f "$OUTPUT_ROM"
    exit 1
fi
echo "  Verified: HCB $built_hcb (RomWBW v$ROMWBW_PIN)"

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
