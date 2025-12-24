# Downstream Integration Guide

This document explains how to integrate the RomWBW emulator core into downstream projects (iOS, macOS, Windows, etc.).

## Overview

The RomWBW emulator is structured with a shared core that all platforms use:

```
src/
├── emu_init.h           # Shared initialization functions (NEW)
├── emu_init.cc          # Implementation of shared init
├── emu_io.h             # Platform abstraction interface
├── hbios_dispatch.h     # HBIOS function dispatcher
├── hbios_dispatch.cc    # HBIOS implementation
├── hbios_cpu.h          # Z80 CPU with HBIOS port I/O
├── hbios_cpu.cc         # Port I/O handlers
└── romwbw_mem.h         # Banked memory system
```

## Required Files for All Platforms

Add these source files to your build:

1. **emu_init.cc** - Shared initialization (NEW - required)
2. **hbios_dispatch.cc** - HBIOS function handling
3. **hbios_cpu.cc** - CPU port I/O
4. Your platform's `emu_io_*.cc` implementation

Plus these headers:
- `emu_init.h`
- `emu_io.h`
- `hbios_dispatch.h`
- `hbios_cpu.h`
- `romwbw_mem.h`

## Key Changes (December 2024)

### New Shared Initialization Module

All platforms must now use `emu_init.h` functions for proper initialization. This fixes:

1. **Device list blank in ROM** - The disk unit table at HCB+0x60 was not being populated
2. **CP/M 3 bank switching issues** - RAM banks need initialization with page zero/HCB copies

### Before (Broken Pattern)

```cpp
// DON'T DO THIS - incomplete initialization
uint8_t* rom = memory.get_rom();
rom[0x0112] = 0x00;  // Patch APITYPE
memcpy(ram, rom, 512);  // Copy HCB
hbios.initMemoryDisks();
// Missing: HBIOS ident, drive map, device count, etc.
```

### After (Correct Pattern)

```cpp
// DO THIS - complete shared initialization
#include "emu_init.h"

// After loading ROM, call the complete initialization:
emu_complete_init(&memory, &hbios, disk_slices);
```

## Initialization Sequence

### 1. Load ROM

```cpp
// From file:
emu_load_rom(&memory, "/path/to/romwbw.rom");

// From buffer:
emu_load_rom_from_buffer(&memory, data, size);
```

### 2. Load Disks (Optional)

```cpp
hbios.loadDisk(0, disk_data, disk_size);
// or
hbios.loadDiskFromFile(0, "/path/to/disk.img");
```

### 3. Complete Initialization

```cpp
// disk_slices is optional: array of per-disk slice counts, or nullptr for defaults
int disk_slices[16] = {4, 4, 4, ...};  // Optional
emu_complete_init(&memory, &hbios, disk_slices);
```

This function performs:
1. Patches APITYPE at 0x0112 to indicate HBIOS (not UNA)
2. Copies HCB from ROM bank 0 to RAM bank 0x80
3. Sets up HBIOS ident signatures at 0xFE00, 0xFF00, and pointer at 0xFFFC
4. Initializes memory disks from HCB configuration
5. Populates disk unit table at 0x160 (HCB+0x60)
6. Populates drive map at 0x120 (HCB+0x20)
7. Updates device count at 0x10C (HCB+0x0C)

### 4. Implement RAM Bank Initialization

Your `HBIOSCPUDelegate` must implement `initializeRamBankIfNeeded`:

```cpp
class MyEmulator : public HBIOSCPUDelegate {
  uint16_t initialized_ram_banks = 0;  // Bitmap for banks 0x80-0x8F

  void initializeRamBankIfNeeded(uint8_t bank) override {
    emu_init_ram_bank(&memory, bank, &initialized_ram_banks);
  }
  // ... other delegate methods
};
```

This is called automatically by hbios_cpu when switching to a RAM bank. It ensures
CP/M 3 (and other OSes using bank switching) have proper page zero and HCB in each bank.

## Platform-Specific emu_io Implementation

Each platform needs an `emu_io_*.cc` implementation. See these references:

- **CLI/Unix**: `src/emu_io_cli.cc` - Uses termios, standard file I/O
- **WebAssembly**: `src/emu_io_wasm.cc` - Uses Emscripten, JavaScript callbacks
- **iOS/macOS**: `iOSCPM/Core/emu_io_ios.mm` - Uses Foundation, SwiftUI callbacks

Key functions to implement:
```cpp
// Console I/O
void emu_console_write_char(uint8_t ch);
int emu_console_read_char();
bool emu_console_has_input();
void emu_console_queue_char(int ch);

// Logging
void emu_log(const char* fmt, ...);
void emu_error(const char* fmt, ...);
void emu_status(const char* fmt, ...);

// File I/O (standard C I/O works on all platforms)
// fopen/fread/fwrite/fclose are used by emu_init.cc
```

## Standard C File I/O

The shared initialization code uses standard C file I/O functions:
- `fopen()`
- `fread()`
- `fwrite()`
- `fclose()`
- `fseek()`
- `ftell()`

These work on all target platforms:
- Unix/Linux/macOS: Native support
- iOS: Works for app bundle resources and documents directory
- Windows: Works with MSVC CRT
- WebAssembly: Emscripten provides virtual filesystem

## Disk Validation

Before attaching disk images, validate them:

```cpp
size_t disk_size;
const char* error = emu_validate_disk_image("/path/to/disk.img", &disk_size);
if (error) {
  // Handle error
}
```

## Full Example (iOS-style)

```cpp
#include "emu_init.h"
#include "hbios_cpu.h"
#include "hbios_dispatch.h"

class Emulator : public HBIOSCPUDelegate {
  banked_mem memory;
  hbios_cpu cpu;
  HBIOSDispatch hbios;
  uint16_t initialized_ram_banks = 0;

public:
  Emulator() : cpu(&memory, this) {
    memory.enable_banking();
    hbios.setCPU(&cpu);
    hbios.setMemory(&memory);
    hbios.setBlockingAllowed(false);  // UI apps should not block
  }

  bool loadROM(const uint8_t* data, size_t size) {
    if (!emu_load_rom_from_buffer(&memory, data, size)) {
      return false;
    }
    emu_complete_init(&memory, &hbios, nullptr);
    return true;
  }

  bool loadDisk(int unit, const uint8_t* data, size_t size) {
    return hbios.loadDisk(unit, data, size);
  }

  void start() {
    cpu.set_cpu_mode(qkz80::MODE_Z80);
    cpu.regs.PC.set_pair16(0x0000);
    cpu.regs.SP.set_pair16(0x0000);
    memory.select_bank(0);
  }

  void runBatch(int count = 50000) {
    for (int i = 0; i < count; i++) {
      cpu.execute();
    }
    // Flush output
    while (hbios.hasOutputChars()) {
      auto chars = hbios.getOutputChars();
      for (uint8_t ch : chars) {
        emu_console_write_char(ch);
      }
    }
  }

  // HBIOSCPUDelegate implementation
  banked_mem* getMemory() override { return &memory; }
  HBIOSDispatch* getHBIOS() override { return &hbios; }

  void initializeRamBankIfNeeded(uint8_t bank) override {
    emu_init_ram_bank(&memory, bank, &initialized_ram_banks);
  }

  void onHalt() override { /* handle halt */ }
  void onUnimplementedOpcode(uint8_t op, uint16_t pc) override { /* handle error */ }
  void logDebug(const char* fmt, ...) override { /* optional debug logging */ }
};
```

## Important Notes

### Avoid Duplicate Disk Table Population

`HBIOSDispatch::initMemoryDisks()` internally calls `populateDiskUnitTable()`.
Do NOT call `populateDiskUnitTable()` again after `initMemoryDisks()` - this
can cause hangs or other issues.

The correct flow is:
```cpp
// emu_complete_init handles this correctly:
// 1. Calls initMemoryDisks() which populates disk unit table
// 2. Calls emu_populate_drive_map() to set up drive letters
// 3. Updates device count
```

If you're calling these manually, ensure you don't duplicate the disk table population.

## Troubleshooting

### System Hangs After 'D' Command

**Symptom**: Boot menu shows device list but then hangs.

**Cause**: `populateDiskUnitTable()` called multiple times.

**Fix**: Use `emu_complete_init()` which handles this correctly, or ensure
you only call `populateDiskUnitTable()` once (via `initMemoryDisks()`).

### Device List Shows Blank

**Symptom**: Boot menu shows no devices, or `D` command shows empty list.

**Cause**: `emu_complete_init()` not called, or called before disks loaded.

**Fix**: Ensure you call `emu_complete_init()` AFTER loading any disk images.

### CP/M 3 Crashes on Bank Switch

**Symptom**: System crashes or hangs when switching to CP/M 3.

**Cause**: `initializeRamBankIfNeeded()` not implemented or returning without action.

**Fix**: Implement the delegate method using `emu_init_ram_bank()`:
```cpp
void initializeRamBankIfNeeded(uint8_t bank) override {
  emu_init_ram_bank(&memory, bank, &initialized_ram_banks);
}
```

### REBOOT Command Fails

**Symptom**: REBOOT.COM shows "Unknown system" or similar error.

**Cause**: HBIOS ident signatures not set up at 0xFE00/0xFF00.

**Fix**: Ensure `emu_complete_init()` is called (it sets up the signatures).

## ROM Requirements

**IMPORTANT**: The emulator requires a ROM with port 0xEF HBIOS proxy code.

Standard RomWBW ROMs (like `SBC_simh_std.rom`) contain real HBIOS code and expect
actual hardware. These will NOT work with the emulator.

Use one of the `emu_*.rom` files that contain proxy code which outputs to port 0xEF
for HBIOS dispatch. The emulator intercepts port 0xEF and handles HBIOS calls in C++.

## Console Output Buffering

CIOOUT (console output) writes characters to an internal `output_buffer`. Your main
loop MUST poll and flush this buffer:

```cpp
void flush_output() {
  while (hbios.hasOutputChars()) {
    std::vector<uint8_t> chars = hbios.getOutputChars();
    for (uint8_t ch : chars) {
      emu_console_write_char(ch);
    }
  }
}

// In main loop:
flush_output();  // Call before blocking on input
```

For CLI (blocking mode), CIOIN automatically flushes output before blocking to
ensure prompts are displayed.

For web/WASM (non-blocking mode), call `flush_output()` on each main loop iteration.

## Migration Checklist

- [ ] Add `emu_init.cc` to your build
- [ ] Add `#include "emu_init.h"` to your emulator code
- [ ] Replace manual HCB patching with `emu_complete_init()`
- [ ] Implement `initializeRamBankIfNeeded()` using `emu_init_ram_bank()`
- [ ] Remove any manual `setup_hbios_ident()` calls (now handled by emu_complete_init)
- [ ] Remove manual disk unit table population (now handled by emu_complete_init)
- [ ] Implement `flush_output()` to poll and display console output
- [ ] Use an `emu_*.rom` file with port 0xEF proxy code
- [ ] Test device list with `D` command at boot menu
- [ ] Test CP/M 3 boot and operation
