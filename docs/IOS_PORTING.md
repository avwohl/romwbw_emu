# iOS Client Porting Guide

This document describes the changes required to update an iOS client based on this cpmemu project to support the new RomWBW system.

## Overview of Changes (December 2024)

The cpmemu project has been significantly updated for full RomWBW HBIOS emulation. If your iOS client was based on an earlier version (CP/M 2.2 only mode), you'll need to make the following changes.

## Required Changes

### 1. APITYPE Patching (Critical)

The ROM image contains an API type field at offset 0x0112 that must be patched to indicate HBIOS (not UNA). Without this patch, utilities like REBOOT will show "ERROR: UNA not supported by application".

**When loading ROM, add this patch:**

```cpp
// After loading ROM into memory:
rom[0x0112] = 0x00;  // CB_APITYPE = HBIOS (not 0xFF = UNA)
```

This must be done BEFORE copying the HCB (first 512 bytes) to RAM.

### 2. HBIOS Ident Signatures (Critical)

REBOOT and other utilities check for HBIOS identification signatures in the common memory area. These must be set up after loading the ROM:

```cpp
void setup_hbios_ident() {
    // Common area 0x8000-0xFFFF maps to bank 0x8F (index 15)
    // Physical offset in RAM = bank_index * 32KB + (addr - 0x8000)
    const uint32_t COMMON_BASE = 0x0F * 32768;  // Bank 0x8F

    // Create ident block at 0xFF00 in common area
    uint32_t ident_phys = COMMON_BASE + (0xFF00 - 0x8000);
    ram[ident_phys + 0] = 'W';       // Signature byte 1
    ram[ident_phys + 1] = ~'W';      // Signature byte 2 (0xA8)
    ram[ident_phys + 2] = 0x35;      // Version: (major << 4) | minor = (3 << 4) | 5

    // Also at 0xFE00 (some utilities check here)
    uint32_t ident_phys2 = COMMON_BASE + (0xFE00 - 0x8000);
    ram[ident_phys2 + 0] = 'W';
    ram[ident_phys2 + 1] = ~'W';
    ram[ident_phys2 + 2] = 0x35;

    // Pointer to ident at 0xFFFC (little-endian)
    uint32_t ptr_phys = COMMON_BASE + (0xFFFC - 0x8000);
    ram[ptr_phys + 0] = 0x00;        // Low byte of 0xFF00
    ram[ptr_phys + 1] = 0xFF;        // High byte of 0xFF00
}
```

### 3. SYSRESET Handler (Critical)

The REBOOT command calls HBIOS function 0xF0 (SYSRESET) with C=0x01 (warm boot) or C=0x02 (cold boot). If you're using `HBIOSDispatch`, register a reset callback:

```cpp
// Define the callback
void handle_sysreset(uint8_t reset_type) {
    // reset_type: 0x01 = warm boot, 0x02 = cold boot

    // 1. Switch to ROM bank 0
    memory.select_bank(0x00);

    // 2. Clear any pending console input
    // (implementation dependent on your I/O layer)

    // 3. Set PC to 0 to restart from ROM
    cpu.regs.PC.set_pair16(0x0000);
}

// Register during initialization
hbios.setResetCallback(handle_sysreset);
```

If you have your own HBIOS handling (not using `HBIOSDispatch`), add this case:

```cpp
case 0xF0: {  // SYSRESET
    uint8_t reset_type = cpu->regs.BC.get_low();  // C register
    if (reset_type == 0x01 || reset_type == 0x02) {
        memory.select_bank(0x00);
        // Clear input state
        cpu->regs.PC.set_pair16(0x0000);
        return;  // Don't do normal RET
    }
    break;
}
```

### 4. ROM Loading Sequence

The correct sequence for loading a RomWBW ROM is:

```cpp
// 1. Enable banking (512KB ROM + 512KB RAM)
memory.enable_banking();

// 2. Load ROM image
memcpy(memory.get_rom(), rom_data, rom_size);

// 3. Patch APITYPE
memory.get_rom()[0x0112] = 0x00;

// 4. Copy HCB to RAM bank 0x80
memcpy(memory.get_ram(), memory.get_rom(), 512);

// 5. Set up HBIOS ident signatures
setup_hbios_ident();

// 6. Initialize CPU and HBIOS dispatch
cpu.regs.PC.set_pair16(0x0000);
memory.select_bank(0x00);  // Start in ROM bank 0
```

## Files to Reference

The WebAssembly implementation in `web/romwbw_web.cc` is a good reference for iOS porting. Key sections:

- `romwbw_load_rom()` - ROM loading with APITYPE patch
- `setup_hbios_ident()` - HBIOS identification setup
- `handle_sysreset()` - SYSRESET callback
- `romwbw_start()` - Initialization sequence

## Shared Code

The following files can be shared between platforms:

- `src/hbios_dispatch.h` / `src/hbios_dispatch.cc` - HBIOS function handlers
- `src/emu_io.h` - Platform abstraction interface
- `src/romwbw_mem.h` - Banked memory implementation
- `src/qkz80.*` - Z80 CPU emulator

For iOS, create `emu_io_ios.cc` implementing the platform abstraction functions defined in `emu_io.h`.

## Optional: Memory Disk Support

The CLI version (`src/cpmemu.cc`) has full support for MD0 (RAM disk) and MD1 (ROM disk) which read/write directly to bank memory. If you need this functionality, see `init_memory_disks()` and the DIO handling in cpmemu.cc for reference.

## Testing

After implementing these changes, test with:

1. Boot to CP/M prompt - should work without errors
2. Run `REBOOT /W` - should warm boot successfully
3. Run `REBOOT /C` - should cold boot successfully
4. Check that no "UNA not supported" errors appear
