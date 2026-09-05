# Downstream Integration Guide

This document explains how to integrate the RomWBW emulator core into downstream projects (iOS, macOS, Windows, etc.).

Dated migration notices for specific core releases live in docs/ - most
recently [docs/DOWNSTREAM_2026-08-26.md](docs/DOWNSTREAM_2026-08-26.md), the
R8 read-name sync. **It changes the build contract the same way the one before
it did: the core declares `emu_host_file_get_read_name()` and does not define
it, so every port that compiles this core must add it or fail to link** -
`return "";` is a correct and complete answer, and that notice also covers
`HBF_HOST_GETRNAME` (0xEA), a refused-rather-than-truncated host path, a length
cap on `emu_host_path_basename()`, and new `r8.com`/`w8.com` in the disk images.
Before it,
[docs/DOWNSTREAM_2026-08-25.md](docs/DOWNSTREAM_2026-08-25.md) is the
W8 host-path sync. **It changes the build contract: the core no longer defines
`emu_host_path_caps()`, so every port that compiles this core must add it or
fail to link** - that link error is the deliberate signal to read that notice,
which also covers `HBF_HOST_GETNAME` (0xE8), the shared `emu_host_path_basename()`,
a tightened `emu_host_file_get_write_name()` contract, and - not enforced by any
compiler, so read it - a disk-image refresh, an R8 destructive-delete fix, and a
release ordering (`docs/RELEASE_ORDER_2026-08-25.md`) that keeps the two from
being shipped in the wrong order. Before it,
[docs/DOWNSTREAM_2026-08-23.md](docs/DOWNSTREAM_2026-08-23.md) is the
all-ports to-do list for the v1.35 -> v1.36 sync (control keys reach the guest,
one dead platform function removed), and
[docs/DOWNSTREAM_2026-08-07.md](docs/DOWNSTREAM_2026-08-07.md) covers the
v1.34 -> v1.35 RomWBW version pin, ROM validation and file-I/O hardening, and
[docs/DOWNSTREAM_2026-07-21.md](docs/DOWNSTREAM_2026-07-21.md) the
v1.33 -> v1.34 platform API change - both still apply if you have not taken
them yet.

## Related: Front-End Feature Parity

This document owns core-engine correctness: the Z80 CPU, HBIOS services, banked
memory, and shadow RAM. Front-end UX parity across ports (keyboard maps,
scrollback, copy/paste, disk catalog, help system, config profiles) is cataloged
separately in the Windows port's FEATURE_PARITY.md at
https://github.com/avwohl/z80cpmw/blob/master/FEATURE_PARITY.md
(raw: https://raw.githubusercontent.com/avwohl/z80cpmw/master/FEATURE_PARITY.md).
Check both documents when assessing parity for a new port; the checklist
contents are not duplicated here.

As of July 2026, one correction to that catalog: it lists romwbw_emu as
CLI-only, but this repo also ships a browser/WASM frontend (`web/`) that already
covers several of its items — xterm.js scrollback, the manifest-disk write
warning with per-disk suppression, R8/W8 host file transfer via browser
picker/download, same-origin server disk selection, and (new in v1.34)
localStorage persistence of UI selections plus a dirty-disk warning before tab
close.

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
bool emu_console_input_exhausted();  // GUI/WASM: return false (see v1.34 notes)
bool emu_console_input_eof();        // GUI/WASM: return false (see v1.34 notes)

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

### Boot Countdown Very Slow (Fixed January 2025)

**Symptom**: Autoboot countdown (3, 2, 1...) takes 10+ seconds per number instead
of 1 second. The 'R' reboot command also takes 30+ seconds.

**Cause**: Bug in `SYSGET_CPUINFO` returned `HL=4000` (KHz) but romldr reads `L`
for MHz. With L=160, the delay loop ran 80x too many iterations.

**Fix**: Pull latest `hbios_dispatch.cc`. Now returns `H=0` (CPU variant),
`L=4` (MHz), `DE=4000` (KHz) matching RomWBW's expected format.

### Silent Crash at Startup on Windows (MSVC builds)

**Symptom**: An MSVC-built app that compiles this core dies instantly at
emulator start — no dialog, no log, just a crash to desktop — typically on a
machine other than the build machine.

**Cause**: VS 2022 17.10+ toolsets (v14.40+) make `std::mutex`'s constructor
constexpr, removing the runtime `_Mtx_init_in_situ` call. If the platform
layer's own threading code uses `std::mutex` (the emulator core itself uses
none) and the target machine's system msvcp140.dll predates the build toolset,
the first `_Mtx_lock` faults.

**Fix**: Define `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` in the project's
preprocessor definitions, or ship the matching VC++ runtime DLLs app-local with
the installer; doing both is harmless. No upstream patch is needed — the
upstream core uses no `std::mutex`, so the bug can only come from platform-layer
code. See z80cpmw commit b73c013 for a worked example (vcxproj define plus
NSIS/MSIX redist bundling).

## ROM Requirements

The emulator requires a ROM with port 0xEF HBIOS proxy code.

Standard RomWBW ROMs (like `SBC_simh_std.rom`) contain real HBIOS code and expect
actual hardware. These will NOT work with the emulator.

Use one of the `emu_*.rom` files that contain proxy code which outputs to port 0xEF
for HBIOS dispatch. The emulator intercepts port 0xEF and handles HBIOS calls in C++.

## RomWBW Version: runtime, not a pin (September 2026, v1.39)

**The compile-time pin is gone.** This core used to emulate exactly one
RomWBW release, and refused to load a ROM from any other. It now reads the
release out of the loaded ROM's HBIOS Configuration Block at run time, so one
binary boots any release in `ROMWBW_SUPPORTED_RELEASES`
(`src/romwbw_pin.h`) - today v3.5.1 and v3.6.0:

```c
#define ROMWBW_SUPPORTED_RELEASES(X)                       \
  X(3, 5, 1, 0, "2025-05-21 release; checked 2026-09-05")  \
  X(3, 6, 0, 0, "2026-03-28 release; checked 2026-09-05")
```

That is the change that lets a client offer the user a choice of RomWBW
version instead of filtering the list down to the one its binary was built
for. `ROMWBW_PIN_MAJOR` and friends no longer exist; `ROMWBW_DEFAULT_*` in
the same header names the release **this tree's own** `roms/` and `disks/`
artifacts are cut from, which is a build input, not a constraint on loading.

Five things report a version to the guest, and every one of them now derives
it from the loaded ROM: `HBF_SYSVER`, the NVRAM checksum seed, the HBIOS
ident block, the CBIOS page-zero stamp at `0x42`/`0x43`, and the load-time
check. Two of those exist only in emulated RAM - nothing that inspects a
built ROM can see them, which is why they are derived rather than stored.

### What did NOT change: ROM and disks must still match

The emulator will now load either release. The **guest** still will not
tolerate a mixed pair: a CBIOS compares its own build against `HBF_SYSVER`
and prints

```
*** WARNING: HBIOS/CBIOS Version Mismatch ***
```

So the pairing rule is unchanged and is now enforced *only* by that warning,
not by a refusal at load time. Ship `emu_avw-...-3.6.0.rom` with 3.6.0 disk
images, or 3.5.1 with 3.5.1 - never one of each.

### Why it matters

A guest's CBIOS compares its own build against the HBIOS version this core
reports. Mismatched, it prints
`*** WARNING: HBIOS/CBIOS Version Mismatch ***`, and in the worst case the
ROM never reaches the boot loader and the emulator sits there producing no
output at all. Three of the ROMs shipped in `roms/` before v1.35 did exactly
that, and nothing told the user why.

### What you must do

1. **Ship a matching set.** Bundle or download `emu_*.rom` and `hd1k_*.img`
   from the same RomWBW release. Do not mix a ROM from one release with disk
   images from another. You may now ship two complete sets and let the user
   choose - that is the point of this change - but each set has to stay
   internally matched. [avwohl/romwbw_disks](https://github.com/avwohl/romwbw_disks)
   publishes matched sets per release with both versions in every filename,
   for exactly this reason.
2. **Check your tree.** Run `roms/verify_romwbw_pin.sh` in CI or before
   cutting a build. It checks every ROM (HCB marker, release, `CB_PLATFORM`),
   every disk image (the `CBIOS v<ver> [WBW]` string in its boot slices), the
   **pairing** between them, and the built binary's supported list. Exit
   status 0 means every artifact names a release this core can run; 1 names
   each problem. `make -C src test` now runs it.
3. **Report the release in your About screen** if you show a version, but
   report the one that was LOADED, not a constant - there is no constant any
   more. `emu_romwbw_release_loaded()` (`emu_init.h`) returns it once a ROM
   is in memory; `emu_romwbw_supported_list()` returns "3.5.1, 3.6.0" for a
   screen shown before any ROM is loaded.

### New: ROM loads now fail loudly

`emu_load_rom()` and `emu_load_rom_from_buffer()` call the new
`emu_validate_rom_hcb()` and **return false** on a ROM this core cannot run,
where they previously accepted it and produced a dead emulator:

	Condition	Result
	HCB marker at 0x103 is not `57 A8`	load fails - corrupt or not a RomWBW ROM
	Release is not in ROMWBW_SUPPORTED_RELEASES	load fails - names the release and the supported list
	`CB_PLATFORM != 0` (stock hardware ROM)	warning only, load proceeds

The middle row is the one that changed in v1.39. It used to read "HCB version
bytes differ from the pin", and it fired on a perfectly good v3.6.0 ROM. It
now fires only on a release nobody has checked this core against - bank 0 is
our HBIOS proxy, and a release whose CBIOS calls a function the dispatcher
does not implement would load and then hang, which is far worse than a
refusal. `emu_set_allow_untested_romwbw(true)` overrides it with a warning
(the CLI's `--allow-untested-romwbw`); a port that exposes it should say
"untested", not "advanced".

GUI ports are the main beneficiaries: you load ROMs from a bundle or a
download and have no console to notice a silent failure on. **Handle the new
`false` return** - show the user an error rather than starting a CPU that
will never print anything. `emu_validate_rom_hcb()` is public if you want to
check an image before offering it in a picker.

### Adding a RomWBW release

Adding one is not a version bump, and it no longer requires a coordinated
release across all ports: a port that has not rebuilt simply does not offer
the new release, and one that has offers both.

Adding a release IS a claim that somebody ran it. To add one:

1. Build its `emu_*.rom` and disk images. That is
   [romwbw_disks](https://github.com/avwohl/romwbw_disks)' job:
   `tools/build_all.sh <ver>`.
2. Boot them. `romwbw_disks/tools/boot_test.sh` asserts the CBIOS banner,
   the CP/M prompt, no version-mismatch warning on a matched pair, and a
   warning on a mismatched one.
3. Round-trip a file through `R8`/`W8`. That exercises the private
   `0xE1`-`0xEA` host block, which upstream RomWBW knows nothing about and
   which no upstream test covers.
4. Add the `X()` line to `ROMWBW_SUPPORTED_RELEASES` with the date checked.

There is no `proto.asm` to diff. Earlier revisions of this section named one
as required reading; RomWBW ships no `Source/HBIOS/proto.asm` in any release
- the files carrying that information are `Source/HBIOS/hbios.asm` and
`Source/Doc/SystemGuide.md`. Booting the release and exercising the host
block is the check that actually found nothing wrong with v3.6.0.

**Do not build from `archive/romwbw-v3.6.0/SBC_simh_std_v360.rom`.** It is a
`v3.6.0-dev.46` snapshot from 2025-12-12, not the release, and its HCB reads
`36 00` exactly as the release does - so no version check can tell them
apart.

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

### Clear NVRAM UI (Recommended)

If a user configures autoboot via SYSCONF with zero timeout, they may get
stuck unable to access the boot menu. Provide a "Clear Boot Config" button
or menu option that calls:

```cpp
hbios.setNvramSetting("");  // Clears NVRAM, boot menu will display
```

This is especially important for GUI apps where users can't easily restart
the emulator with different command-line options.

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

## Disk Commit System (January 2025)

The emulator ensures disk writes are reliably committed to storage through multiple
mechanisms:

### Automatic Flush Points

1. **Warm boot flush**: When a CP/M program ends (warm boot / SYSRESET type 0x01),
   all disk data is flushed via `emu_disk_flush_all()`. This happens automatically
   in `emu_init.cc` - no action needed from downstream clients.

2. **Per-write flush**: Each individual disk write calls `emu_disk_flush()` on the
   file handle immediately. This ensures data safety even if the app crashes.

3. **Periodic flush**: Call `checkPeriodicFlush()` from your main loop or timer.
   If any writes have occurred and 20+ seconds have passed since the last flush,
   it flushes all disks. Safe to call frequently - it only does work when needed.

### Implementation

```cpp
// In your main run loop or frame callback:
hbios.checkPeriodicFlush();  // Call every frame/tick - internally rate-limited
```

The function returns `true` if a flush was performed, `false` otherwise.

### API Reference

```cpp
// Check and perform periodic flush (call from main loop)
// Returns true if flush was performed, false otherwise
bool checkPeriodicFlush();
```

### Notes

- `emu_disk_flush_all()` is implemented in both `emu_io_cli.cc` and `emu_io_wasm.cc`
- The warm boot hook is already set up by `emu_setup_reset_callback()`
- Downstream clients just need to call `checkPeriodicFlush()` periodically

## Platform API Changes (July 2026, v1.34)

v1.34 changes the `emu_io.h` platform interface in two places, fixes host file
transfer and disk write error handling, and documents a platform contract that
was previously implicit. Every port that provides its own `emu_io_*.cc`
implementation needs the first two changes to compile.

### emu_host_file_close_write() Now Returns bool

```cpp
// BEFORE:
void emu_host_file_close_write();

// AFTER:
bool emu_host_file_close_write();  // false = final flush/close failed
```

A `false` return means the final flush or close failed and the written file may
be truncated. This matters because byte writes go through stdio buffering, so a
disk-full error can surface only at the final flush inside `fclose()` —
previously nobody checked, and W8 reported success for a truncated export.

`HBF_HOST_CLOSE` (C=1, close write file) now returns A=0xFF on failure, and the
W8 utility prints "Host file close failed - file may be truncated".

Update your platform's implementation:

- CLI reference: return `fclose() == 0` (true when nothing was open)
- WASM reference: return `true` (a browser download cannot fail synchronously)
- Windows port (z80cpmw): the deferred-write close in `emu_io_windows.cpp`
  should return true only if the `fopen` succeeded, `fwrite` wrote the full
  buffer, and `fclose` returned 0 — this also fixes its existing silent drop
  of write errors

### New Required Function: emu_console_input_exhausted()

```cpp
// Returns true once a non-interactive stdin (pipe/file) has hit EOF and the
// guest has read past it - no further input can ever arrive.
bool emu_console_input_exhausted();
```

The CLI returns true once a piped stdin hit EOF and the guest read past it; the
main loop then exits through the end of `main()` so NVRAM and trace persistence
still run. GUI and WASM ports should simply `return false;` (input can always
arrive later). Only the CLI main loop consults it, but every platform layer
must define it to link.

A companion function was added in the same release:

```cpp
// Weaker form: EOF has been *detected* on a non-interactive stdin (nothing
// queued), whether or not the guest has read past it.
bool emu_console_input_eof();
```

The CLI main loop combines it with `isConsoleIdle()` to wind down guests that
only poll input status (e.g. the romldr boot menu) on a closed pipe. GUI and
WASM ports: `return false;` here too.

### HBF_HOST_READ Waits for the Browser File Picker

When blocking is disallowed (`setBlockingAllowed(false)`) and the host-file
state is `HOST_FILE_WAITING_READ`, `HBF_HOST_READ` in `hbios_dispatch.cc` now
pauses the guest by rewinding PC to re-execute the call — the same mechanism
`HBF_CIOIN` uses for console input. This makes browser R8 wait for the file
picker instead of importing 0 bytes. `isWaitingForInput()` now includes this
wait, so run loops that already gate on it need no changes. Native (blocking)
ports are unaffected.

### Latent-Bug Fix Ports Should Replicate: emu_file_load_to_mem()

`emu_file_load_to_mem()` in `emu_io_common.cc` had a `size_t` underflow when
`offset > mem_size` — the same class of bug as the v1.33 truncated-disk fix
(fed8155). Upstream now checks `offset >= mem_size` and returns 0 before
subtracting. The Windows port's `emu_io_windows.cpp` contains a verbatim copy
of this function (around lines 344-361) that compiles independently of the
shared core and needs the identical guard.

### DIOWRITE Hardening (Recompile Only)

`HBF_DIOWRITE` now checks the return value of `emu_disk_write()` and reports
HBR_IO plus a partial block count in E on a short write (host disk full or I/O
error). Writes past the end of an in-memory disk image no longer grow the image
— the write path is bound-checked like the read path and reports a partial
count. Disk offsets are computed in 64-bit so a large LBA can no longer wrap
the 32-bit LBA*512 multiply. Ports that embed the shared core get all of these
for free on recompile; no platform-layer changes are needed.

### Platform Contract: Flush Dirty In-Memory Disks on Stop

Platform stop paths must flush/persist any dirty in-memory disks. This was
previously undocumented, and it is exactly the contract that bit the Windows
port (fixed in its commit 70ce7b1). File-backed disks are flushed per-write by
the core (see "Disk Commit System" above), but in-memory disks are the
platform's responsibility whenever emulation stops or the app exits.

## Platform Contract: Ctrl-A..Ctrl-Z Belong to the Guest

CP/M has no function keys, so F1-F12 are free for host UI. Every Ctrl-letter is
the opposite: it is ordinary ASCII that the guest reads. `^R` (0x12) is the
CCP's retype-line, `^E`/`^S`/`^D`/`^X` are the WordStar cursor diamond, `^Q` is
the prefix for the whole `^Qx` family, and `^C`, `^K`, `^O`, `^P` and `^V` are
bound by the software people actually run. A port that takes one takes it away
from every program its users run. (Which port maps which key is cataloged in
FEATURE_PARITY.md, referred to at the top of this document; this is the rule
that governs all of them.)

A port that binds a Ctrl-letter anyway must:

- **Make it configurable** — in the same settings file as everything else, not
  a compile-time decision.
- **Default to passing it through** to the guest, including for configs written
  before the setting existed, so an upgrade does not silently arm it.
- **Not advertise a shortcut it no longer owns.** Menu accelerator hints, help
  screens and README text must be built from what is actually bound.
- **Never put an unconfirmed machine reset on one.** A reset that cold-starts
  from ROM, clears the screen and drops the scrollback belongs on a menu item
  or behind a confirmation, whatever key reaches it.

This is exactly the contract that bit the Windows port (fixed in its commit
e35f336, shipped in 1.0.20). `Ctrl+R` was an unconditional accelerator for
ID_EMU_RESET while F1 and F5 sat behind `keyboard.f1ToCpm`/`f5ToCpm`;
`TranslateAccelerator` swallows a matched keystroke whole, so no `WM_CHAR` 0x12
was ever synthesized and the terminal saw nothing, while the `WM_COMMAND` it
sent instead cold-restarted the machine with no confirmation. The user reported
it as "Ctrl R exits me from CPM". It is now `keyboard.ctrlRToCpm`, default true.

The terminal layer counts as a binding too. A POSIX raw mode that leaves `IXON`
set hands `^S`/`^Q` to XON/XOFF flow control, and one that leaves `IEXTEN` set
loses `^V` (VLNEXT) and `^O` (VDISCARD) on BSD/XNU, where those are gated on
IEXTEN alone rather than inside the ICANON block as on Linux — half the
WordStar diamond, gone before any emulator code runs. Clear `IXON` in
`c_iflag` and `IEXTEN` in `c_lflag`; cpmemu's `enable_raw_mode()` (its commit
1584295) spells out the full set, including the clears it deliberately skips.
Leave `IXOFF` alone: it throttles a fast sender and never consumes a typed
`^S`. In a browser, a Ctrl-letter that the page does not `preventDefault()`
stays the browser's own shortcut — and `Ctrl+Shift`+letter is not cancelled by
xterm.js at all, so the page has to do it.

If your attention key is itself a Ctrl-letter — this repo's CLI `--escape`
defaults to `^E`, WordStar cursor-up — say where you document it that the key
is reserved by the emulator and taken away from the guest, give the user a way
to turn it off entirely, and make sure the reserved key is not also delivered
to the guest. Do not use `^@` as the "off" spelling: terminals send NUL for
Ctrl+Space, so binding it moves the theft rather than ending it.

## Migration Checklist

Unprefixed items are the original migration, written between `abbde53` and
`cd4e2da` (2025-12-25 to 2026-01-16) — before this file started tagging items
by release. A `vN.NN:` prefix
names the romwbw_emu release whose sources first require the item, so a port
syncing past that tag owes everything up to and including it. The `v1.36:`
items are inside the `v1.36` tag (`04ad2b6`, 2026-08-25); the `v1.37:` items
come from the commits after it, and the read-name build contract
([docs/DOWNSTREAM_2026-08-26.md](docs/DOWNSTREAM_2026-08-26.md), `322ca8e`) is
one of them - so a port that took the tag has the `emu_host_path_caps()` half
of the link contract and not the `emu_host_file_get_read_name()` half.

One thing to know before reading those prefixes as download links: `v1.36` was
tagged and never packaged. No `release.yml` run exists for it, so there is no
deb, rpm, web build or `roms/` asset under that tag - `gh release list` shows
`v1.35` (2026-08-07) as the newest release with assets, and `v1.37` is the next
one. A port that syncs *sources* by tag therefore sees v1.36; a port that
downloads *release assets* goes from v1.35 straight to v1.37. The tag itself
stays where it is: it is the contract the `v1.36:` items below are measured
against, and it is not being moved or retracted.

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
- [ ] Call `checkPeriodicFlush()` from main loop or timer (ensures disk commits)
- [ ] v1.34: Update `emu_host_file_close_write()` to return `bool` (false = file may be truncated)
- [ ] v1.34: Implement `emu_console_input_exhausted()` and `emu_console_input_eof()` (GUI/WASM: just `return false;`)
- [ ] v1.34: If your port copied `emu_file_load_to_mem()`, add the `offset >= mem_size` guard
- [ ] v1.34: Verify your stop/exit path flushes or persists dirty in-memory disks
- [ ] v1.35: Handle a `false` return from `emu_load_rom*()` - show the user an error instead of starting a dead CPU
- [ ] v1.35: Ship ROM and disk images from the same release; run `roms/verify_romwbw_pin.sh` before cutting a build
- [ ] v1.35: If your About screen shows a version, add the RomWBW release
- [ ] v1.39: `ROMWBW_PIN_*` is gone. If you referenced `ROMWBW_PIN_STR`, use `emu_romwbw_release_loaded()` after a ROM is loaded, or `emu_romwbw_supported_list()` before
- [ ] v1.39: A v3.6.0 ROM now loads. If you filter a catalog down to one RomWBW version, you can stop - offer the list instead
- [ ] v1.39: Namespace any persisted NVRAM blob per RomWBW release - the checksum seed mixes in the version bytes, so a blob saved under one release silently resets under another
- [ ] v1.35: If your port copied `emu_file_load()`/`emu_file_save()`/`emu_disk_*()`, take the hardening below
- [ ] v1.36: Audit host shortcuts for Ctrl-letters; any survivor must be configurable and default to the guest
- [ ] v1.36: Confirm no reset/reboot is reachable from an unconfirmed keystroke
- [ ] v1.36: If you set POSIX raw mode, clear `IXON` (^S/^Q) and `IEXTEN` (^V/^O)
- [ ] v1.36: Delete your `emu_console_check_ctrl_c_exit()` definition - the declaration is gone from `emu_io.h`. Keep `emu_console_check_escape()`
- [ ] v1.36: `emu_console_read_char()` may return `EMU_CONSOLE_RETRY` (-2); if your backend never reserves a key it never returns it, but do not treat a negative return as a byte
- [ ] v1.36: Make `emu_host_file_get_write_name()` return the *effective* destination, not an echo of the requested name - W8 prints it to the user now (`HBF_HOST_GETNAME`). Return `""`/`nullptr` outside an open write, and after a failed open
- [ ] v1.36: If your backend cannot honour a directory (browser, sandboxed app), reduce the requested path with the shared `emu_host_path_basename()` rather than your own split - it takes both separators and refuses `.`/`..`
- [ ] v1.36: Refresh any bundled `hd1k_*` images: `r8.com` and `w8.com` both changed (W8 no longer truncates binaries at the first `^Z`). `disks/rebuild_disk_utils.sh` builds and installs; `disks/verify_disk_utils.sh` checks
- [ ] v1.37: Define `emu_host_file_get_read_name()` or fail to link. `return "";` is a correct answer - R8 then prints what it was asked for, as before. Answer properly only if your backend resolves, redirects or sandboxes a read path; a backend whose read is a file picker should return `""`, as the browser does. See [docs/DOWNSTREAM_2026-08-26.md](docs/DOWNSTREAM_2026-08-26.md)
- [ ] v1.37: Refresh bundled images again - `r8.com` and `w8.com` changed once more (R8 names the file it opened; W8 tells a CP/M read error from end of file, which matters on ZSDOS and CP/M 3 and not on CP/M 2.2)
- [ ] v1.37: If you scrape R8/W8 output, a failed open is two lines now: the message, then `  Asked for: <path>`
- [ ] v1.37: If your `emu_io_cleanup()` closes state that has to survive a mode switch (the CLI's did - printer/aux redirection), move it to process exit
- [ ] v1.37: MSVC ports can drop any C4267 suppression on `hbios_dispatch.cc` - `8eeb227` cast all six sites (a `size_t` loop index promoting a `uint16_t` guest address in `write_to_bank`/`read_from_bank`, where truncating to sixteen bits *is* the Z80 64K wrap HBIOS wants). Grep for the MSBuild spelling, not the compiler flag: `z80cpmw` still carries it as `<DisableSpecificWarnings>4267` at `z80cpmw/z80cpmw.vcxproj:260`, which a search for `/wd4267` reports as already gone
