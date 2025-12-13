#!/bin/bash
#
# Build all ROM binaries for cpmemu
#
# Usage: ./build_all.sh [source_rom]
#
# If source_rom is provided, also builds the combined emu_romwbw.rom
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/../src"

echo "=== Building emu_hbios ==="

# Assemble
um80 -g "$SRC_DIR/emu_hbios.asm"
echo "  Assembled emu_hbios.asm"

# Link
ul80 -o "$SCRIPT_DIR/emu_hbios.bin" -p 0000 "$SRC_DIR/emu_hbios.rel"
echo "  Linked emu_hbios.bin ($(stat -c%s "$SCRIPT_DIR/emu_hbios.bin") bytes)"

# Pad to 32KB
dd if=/dev/zero bs=32768 count=1 of="$SCRIPT_DIR/emu_hbios_32k.bin" 2>/dev/null
dd if="$SCRIPT_DIR/emu_hbios.bin" of="$SCRIPT_DIR/emu_hbios_32k.bin" conv=notrunc 2>/dev/null
echo "  Padded to emu_hbios_32k.bin (32768 bytes)"

# Clean up intermediate files
rm -f "$SRC_DIR/emu_hbios.rel" "$SRC_DIR/emu_hbios.sym"

# Build combined ROM if source ROM provided or exists
SOURCE_ROM="${1:-$SCRIPT_DIR/SBC_simh_std.rom}"
if [ -f "$SOURCE_ROM" ]; then
    echo ""
    echo "=== Building emu_romwbw.rom ==="
    "$SCRIPT_DIR/build_emu_rom.sh" "$SOURCE_ROM"
else
    echo ""
    echo "No source ROM found. To build emu_romwbw.rom, run:"
    echo "  ./build_all.sh <path_to_SBC_simh_std.rom>"
fi

echo ""
echo "=== Build complete ==="
