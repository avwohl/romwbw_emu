# Downstream Integration Guide

This document explains how to integrate the RomWBW emulator core into downstream projects (iOS, macOS, Windows, etc.).

## Overview

The RomWBW emulator is structured with a shared core that all platforms use:

```
src/
├── emu_init.h           # Shared initialization functions
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

1. **emu_init.cc** - Shared initialization
2. **hbios_dispatch.cc** - HBIOS function handling
3. **hbios_cpu.cc** - CPU port I/O
4. Your platform's `emu_io_*.cc` implementation

Plus these headers:
- `emu_init.h`
- `emu_io.h`
- `hbios_dispatch.h`
- `hbios_cpu.h`
- `romwbw_mem.h`

## Critical: Shadow RAM Fix (December 2024)

### The Bug

The `romwbw_mem.h` memory system had a bug where shadow RAM was checked for ALL ROM banks instead of only bank 0. This caused incorrect behavior when:

1. Shadow bits were set for addresses 0x000-0x1FF (page zero + HCB)
2. The boot loader (romldr) ran from ROM bank 1
3. romldr tried to read HCB data from addresses 0x100-0x1FF
4. **Bug**: Reads returned shadow RAM content (bank 0's data) instead of ROM bank 1's content

### The Fix

In `romwbw_mem.h`, the `fetch_banked()` function now checks `current_bank == 0x00` before using shadow RAM:

```cpp
// BEFORE (broken):
if (get_shadow_bit(addr)) {
    return ram[phys];  // Shadow RAM for ANY ROM bank - WRONG!
}

// AFTER (fixed):
if (current_bank == 0x00 && get_shadow_bit(addr)) {
    return ram[phys];  // Shadow RAM only for bank 0 - CORRECT
}
```

### What You Need to Do

1. **Pull the latest romwbw_mem.h** - Contains the critical shadow RAM fix
2. **Pull the latest emu_init.cc** - Now calls `emu_copy_hcb_to_shadow_ram()` at the end of initialization
3. **Use `emu_complete_init()`** - This handles all HCB setup correctly

The `emu_complete_init()` function now:
1. Patches APITYPE in ROM
2. Copies HCB to RAM (simple copy)
3. Sets up HBIOS ident signatures
4. Populates disk tables
5. Copies HCB to shadow RAM with shadow bits set (must be last!)

**After `emu_complete_init()`**, call `emu_setup_reset_callback()` to enable reboot:
```cpp
emu_complete_init(&memory, &hbios, disk_slices);
emu_setup_reset_callback(&memory, &cpu, &hbios);  // Enable 'R' reboot command
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
8. Copies HCB to shadow RAM with shadow bits set

### 4. Implement RAM Bank Initialization

Your `HBIOSCPUDelegate` must implement `initializeRamBankIfNeeded`:

```cpp
class MyEmulator : public HBIOSCPUDelegate {
  HBIOSDispatch hbios;

  void initializeRamBankIfNeeded(uint8_t bank) override {
    // Use HBIOSDispatch's shared bitmap - DO NOT create your own!
    emu_init_ram_bank(&memory, bank, hbios.getInitializedBanksBitmap());
  }
  // ... other delegate methods
};
```

This is called automatically by hbios_cpu when switching to a RAM bank.

**Important:** Use `hbios.getInitializedBanksBitmap()` to share the bitmap with
HBIOSDispatch's SYSSETBNK handler. This ensures both port I/O and HBIOS function
calls use the same initialization tracking. See "Unified RAM Bank Initialization"
below for details.

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
```

## Full Example

```cpp
#include "emu_init.h"
#include "hbios_cpu.h"
#include "hbios_dispatch.h"

class Emulator : public HBIOSCPUDelegate {
  banked_mem memory;
  hbios_cpu cpu;
  HBIOSDispatch hbios;
  // NOTE: No local initialized_ram_banks - use hbios.getInitializedBanksBitmap()

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
    emu_setup_reset_callback(&memory, &cpu, &hbios);  // Enable 'R' reboot
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
    // Use shared bitmap from HBIOSDispatch for unified tracking
    emu_init_ram_bank(&memory, bank, hbios.getInitializedBanksBitmap());
  }

  void onHalt() override { /* handle halt */ }
  void onUnimplementedOpcode(uint8_t op, uint16_t pc) override { /* handle error */ }
  void logDebug(const char* fmt, ...) override { /* optional debug logging */ }
};
```

## Troubleshooting

### System Hangs After 'D' Command

**Symptom**: Boot menu shows device list but then hangs.

**Cause**: `populateDiskUnitTable()` called multiple times.

**Fix**: Use `emu_complete_init()` which handles this correctly.

### Device List Shows Blank

**Symptom**: Boot menu shows no devices, or `D` command shows empty list.

**Cause**: `emu_complete_init()` not called, or called before disks loaded.

**Fix**: Ensure you call `emu_complete_init()` AFTER loading any disk images.

### CP/M 3 Crashes on Bank Switch

**Symptom**: System crashes or hangs when switching to CP/M 3.

**Cause**: `initializeRamBankIfNeeded()` not implemented.

**Fix**: Implement the delegate method using `emu_init_ram_bank()`.

### REBOOT Command Fails

**Symptom**: REBOOT.COM shows "Unknown system" or similar error.

**Cause**: HBIOS ident signatures not set up at 0xFE00/0xFF00.

**Fix**: Ensure `emu_complete_init()` is called.

### romldr Reads Wrong HCB Data

**Symptom**: Boot loader reads incorrect configuration, wrong disk count, etc.

**Cause**: Missing shadow RAM fix in romwbw_mem.h.

**Fix**: Pull latest romwbw_mem.h with the `current_bank == 0x00` check.

## ROM Requirements

The emulator requires a ROM with port 0xEF HBIOS proxy code.

Standard RomWBW ROMs (like `SBC_simh_std.rom`) contain real HBIOS code and expect
actual hardware. These will NOT work with the emulator.

Use one of the `emu_*.rom` files that contain proxy code which outputs to port 0xEF
for HBIOS dispatch. The emulator intercepts port 0xEF and handles HBIOS calls in C++.

## Console Output

The `writeConsoleString()` function now writes directly to `emu_console_write_char()`
instead of the output buffer. This ensures consistent display across all platforms.

For CLI (blocking mode), CIOIN automatically flushes output before blocking.

For web/WASM (non-blocking mode), output goes directly to the display.

## Unified RAM Bank Initialization (January 2025)

### The Problem

Previously, there were two independent RAM bank initialization systems:

1. **Port I/O path** - Called via `initializeRamBankIfNeeded()` delegate method when
   hbios_cpu detected a bank switch via port I/O
2. **SYSSETBNK path** - Called via HBIOS function 0xF1 in hbios_dispatch.cc

Each had its own `initialized_ram_banks` bitmap. If a bank was initialized via one
path, the other path didn't know and would re-initialize it. While harmless (same
data copied twice), it was wasteful and the SYSSETBNK path was missing the CBIOS
page zero stamp at 0x40-0x55 that tools like ASSIGN and MODE require.

### The Fix

Now there is a single shared bitmap owned by `HBIOSDispatch`:

```cpp
// In HBIOSDispatch (hbios_dispatch.h)
uint16_t* getInitializedBanksBitmap() { return &initialized_ram_banks; }
```

Both initialization paths now use this shared bitmap:

- **Port I/O**: Delegate calls `emu_init_ram_bank(&memory, bank, hbios.getInitializedBanksBitmap())`
- **SYSSETBNK**: Calls `emu_init_ram_bank(memory, new_bank, &initialized_ram_banks)`

### What You Need to Do

**Remove any local `initialized_ram_banks` variable** from your emulator class.
Instead, use `hbios.getInitializedBanksBitmap()`:

```cpp
// BEFORE (deprecated):
class MyEmulator : public HBIOSCPUDelegate {
  uint16_t initialized_ram_banks = 0;  // DON'T DO THIS
  void initializeRamBankIfNeeded(uint8_t bank) override {
    emu_init_ram_bank(&memory, bank, &initialized_ram_banks);
  }
};

// AFTER (correct):
class MyEmulator : public HBIOSCPUDelegate {
  HBIOSDispatch hbios;
  void initializeRamBankIfNeeded(uint8_t bank) override {
    emu_init_ram_bank(&memory, bank, hbios.getInitializedBanksBitmap());
  }
};
```

This ensures:
1. Single bitmap tracks all RAM bank initialization
2. No redundant re-initialization
3. CBIOS page zero stamp (0x40-0x55) is always installed correctly

## NVRAM Boot Configuration (January 2025)

RomWBW stores boot configuration in RTC NVRAM. The emulator supports both
programmatic configuration via the C++ API and interactive configuration via
the ROM's built-in SYSCONF utility ('W' command at the boot menu).

### String-Based API

The NVRAM interface uses printable strings for all get/set operations:

```cpp
// Set boot option - accepts these formats:
hbios.setNvramSetting("C");     // Boot ROM app C (CP/M 2.2)
hbios.setNvramSetting("Z");     // Boot ROM app Z (ZSDOS)
hbios.setNvramSetting("0");     // Boot from disk unit 0, slice 0
hbios.setNvramSetting("2.3");   // Boot from disk unit 2, slice 3
hbios.setNvramSetting("H");     // Show boot menu (help)
hbios.setNvramSetting("");      // Clear - uninitialized, shows menu

// Get current boot option - returns same format
std::string setting = hbios.getNvramSetting();
// Returns: "C", "Z", "0", "2.3", etc. or "" if uninitialized

// Check if NVRAM needs to be persisted (modified since last read)
if (hbios.hasNvramChange()) {
    std::string setting = hbios.getNvramSetting();  // clears dirty flag
    saveToStorage(setting);  // your persistence code
}

// Check if NVRAM has a valid boot option set
bool initialized = hbios.isNvramInitialized();
```

### Persistence

NVRAM is NOT persisted across sessions by default. To add persistence:

```cpp
// On app startup - restore saved setting:
std::string saved = loadFromStorage();  // your storage code
if (!saved.empty()) {
    hbios.setNvramSetting(saved);
}

// Periodically or on shutdown - check for changes and save:
if (hbios.hasNvramChange()) {
    std::string setting = hbios.getNvramSetting();
    saveToStorage(setting);
}
```

Platform-specific storage examples:
- iOS/macOS: `UserDefaults.standard.string(forKey: "nvram")`
- Android: `SharedPreferences.getString("nvram", "")`
- Web: `localStorage.getItem("nvram")`

### Example: iOS/Swift Boot Picker

```swift
Picker("Boot Device", selection: $bootSelection) {
    Text("Show Menu").tag("H")
    Text("CP/M 2.2").tag("C")
    Text("ZSDOS").tag("Z")
    ForEach(0..<diskCount, id: \.self) { disk in
        Text("Disk \(disk)").tag("\(disk)")
    }
}
.onChange(of: bootSelection) { newValue in
    emulator.hbios.setNvramSetting(newValue)
}
```

### ROM SYSCONF Utility

Users can also configure boot options interactively by pressing 'W' at the
boot menu. The ROM's SYSCONF utility reads and writes NVRAM directly, and
changes are visible via `getNvramSetting()` / `hasNvramChange()`.

## Manifest Disk Write Warning (January 2025)

Apps that download disk images from a manifest (auto-updates) face a UX challenge:
users may write data (save games, files) to these disks, but their changes can be
lost when the app updates and re-downloads the disk images.

The emulator now provides a simple polling mechanism to help UIs warn users about
this scenario.

### How It Works

1. **Mark manifest disks** when loading them:
   ```cpp
   hbios.loadDisk(0, disk_data, disk_size);
   hbios.setDiskIsManifest(0, true);  // This disk is from the manifest
   ```

2. **Allow per-disk warning suppression** (for "Don't warn again" checkbox):
   ```cpp
   hbios.setDiskWarningSuppressed(0, true);  // User checked "Don't warn"
   ```

3. **Poll for write warning** in your UI loop:
   ```cpp
   // In your run loop or frame callback:
   if (hbios.pollManifestWriteWarning()) {
       showWarningDialog("Changes to this disk may be lost when the app updates. "
                         "Use 'Copy Disk' to preserve your changes.");
   }
   ```

### Behavior

- `pollManifestWriteWarning()` returns `true` **once** per session, on the first
  write to a manifest disk that doesn't have warning suppressed
- After returning `true`, it returns `false` for the rest of the session
- Writes to non-manifest disks don't trigger the warning
- Writes to manifest disks with `warning_suppressed=true` don't trigger it
- The "shown" flag is **static** - it persists across `hbios.reset()` and object
  recreation, only resetting when the app/page is fully restarted

### API Reference

```cpp
// Mark a disk as managed by app manifest (can be overwritten on update)
void setDiskIsManifest(int unit, bool is_manifest);

// Suppress warning for this disk (user checked "Don't warn about overwrites")
void setDiskWarningSuppressed(int unit, bool suppressed);

// Poll for manifest write warning - returns true once per session
bool pollManifestWriteWarning();
```

### UI Implementation Suggestions

1. **Disk selector**: Add a checkbox "Don't warn about overwrites" per disk
   - When checked, call `setDiskWarningSuppressed(unit, true)`
   - Persist this preference locally

2. **Warning dialog**: Show a clear message like:
   > "This disk may be replaced when the app updates. Any changes you make could
   > be lost. To keep changes permanently, copy this disk to your local storage."

3. **Copy disk feature**: Provide a way for users to copy a manifest disk to
   local storage, creating a user-owned version that won't be overwritten

## Migration Checklist

- [ ] Pull latest `romwbw_mem.h` with shadow RAM fix
- [ ] Pull latest `emu_init.cc` and `emu_init.h`
- [ ] Pull latest `hbios_dispatch.cc` and `hbios_dispatch.h`
- [ ] Replace manual HCB patching with `emu_complete_init()`
- [ ] Remove local `initialized_ram_banks` variable from your emulator class
- [ ] Update `initializeRamBankIfNeeded()` to use `hbios.getInitializedBanksBitmap()`
- [ ] Remove any manual HCB shadow setup (now handled by emu_complete_init)
- [ ] Add `emu_setup_reset_callback()` call after `emu_complete_init()` for reboot support
- [ ] Test device list with `D` command at boot menu
- [ ] Test CP/M 3 boot and operation
- [ ] Test REBOOT command
- [ ] Test ASSIGN command (verifies CBIOS stamp at 0x40)
- [ ] Test SYSCONF utility ('W' command at boot menu)
- [ ] Update NVRAM code to use new string-based API:
  - [ ] Replace `setBootOption()` with `setNvramSetting()`
  - [ ] Replace raw byte access with `getNvramSetting()` / `hasNvramChange()`
- [ ] Implement manifest disk write warning UI (optional but recommended)
  - [ ] Call `setDiskIsManifest()` when loading manifest-managed disks
  - [ ] Poll `pollManifestWriteWarning()` in UI loop
  - [ ] Show warning dialog when poll returns true
  - [ ] Add "Don't warn" checkbox with `setDiskWarningSuppressed()`
