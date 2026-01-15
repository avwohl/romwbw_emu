# Boot Configuration Guide

**Date:** 2026-01-15
**Status:** Complete - Full NVRAM implementation with persistence

## Overview

The emulator supports RomWBW's boot configuration system via emulated RTC NVRAM. Boot settings can be configured through:

1. **Command line** (`--boot=`) - For scripted/automated use
2. **SYSCONF utility** (`W` at boot menu) - Interactive configuration
3. **Programmatic API** (`setBootOption()`) - For GUI apps

Settings persist across sessions in `~/.config/romwbw_emu/nvram.json`.

## Quick Start

### Command Line Examples

```bash
# Auto-boot CP/M from ROM
./romwbw_emu --romwbw=roms/emu_avw.rom --boot=C

# Auto-boot from disk 0, slice 0
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disk.img --boot=0

# Auto-boot from disk 2, slice 3
./romwbw_emu --romwbw=roms/emu_avw.rom --disk2=disk.img --boot=2.3

# Show boot menu (no auto-boot)
./romwbw_emu --romwbw=roms/emu_avw.rom --boot=H
```

### Boot Format Reference

| Format | Description |
|--------|-------------|
| `C` | Boot ROM app C (CP/M 2.2) |
| `Z` | Boot ROM app Z (ZSDOS) |
| `B` | Boot ROM app B (BASIC) |
| `H` | Show help menu |
| `0` | Boot disk unit 0, slice 0 |
| `2.3` | Boot disk unit 2, slice 3 |

## Using SYSCONF (Interactive)

Press `W` at the boot menu to access the SYSCONF utility:

```
Boot [H=Help]: W

RomWBW System Config Utility, Version 1.0

Current Configuration:
  [BO] / Boot Options: ROM (App = "H")
  [AB] / Auto Boot: Disabled

Commands:
  (P)rint - Display current settings
  (S)et {switch} {values} - Set a switch
  (R)eset - Reset NVRAM to defaults
  (H)elp - Help menu
  (Q)uit - Exit to boot menu

$
```

### SYSCONF Commands

| Command | Description | Example |
|---------|-------------|---------|
| `P` | Print current settings | `P` |
| `S BO D,u,s` | Boot from disk unit u, slice s | `S BO D,2,3` |
| `S BO R,app` | Boot ROM application | `S BO R,C` |
| `S AB E,t` | Enable autoboot with timeout | `S AB E,5` |
| `S AB E,0` | Enable immediate autoboot | `S AB E,0` |
| `S AB D` | Disable autoboot | `S AB D` |
| `R` | Reset to defaults | `R` |
| `H` | Show help | `H` |
| `H BO` | Help for boot options | `H BO` |
| `H AB` | Help for autoboot | `H AB` |
| `Q` | Quit SYSCONF | `Q` |

### Example: Configure Disk Boot

```
$ S BO D,2,3       <- Set boot to disk 2, slice 3
$ S AB E,0         <- Enable autoboot (immediate)
$ P                <- Verify settings
Current Configuration:
  [BO] / Boot Options: Disk (Unit 2, Slice 3)
  [AB] / Auto Boot: Enabled (Timeout 0 seconds)
$ Q                <- Quit (settings saved on emulator exit)
```

### Example: Configure ROM App Boot

```
$ S BO R,C         <- Set boot to ROM app 'C' (CP/M)
$ S AB E,5         <- Enable autoboot with 5 second countdown
$ P
Current Configuration:
  [BO] / Boot Options: ROM (App = "C")
  [AB] / Auto Boot: Enabled (Timeout 5 seconds)
$ Q
```

## NVRAM Persistence

### File Location

`~/.config/romwbw_emu/nvram.json`

### JSON Format

```json
{
  "description": "RomWBW NVRAM boot configuration",
  "nvram": ["57", "43", "80", "20", "A4"],
  "decoded": {
    "initialized": true,
    "boot_type": "rom",
    "boot_app": "C",
    "autoboot": true,
    "timeout": 0
  }
}
```

### NVRAM Byte Layout

| Byte | Name | Description |
|------|------|-------------|
| 0 | Signature | `57` ('W') = initialized, `00` = not initialized |
| 1 | Boot L | ROM: app char ('C'), Disk: slice number |
| 2 | Boot H | `80` = ROM, `00-7F` = disk unit number |
| 3 | Autoboot | `20` = enabled, `00` = disabled; bits 0-3 = timeout |
| 4 | Checksum | XOR of bytes 0-3 with version bytes |

### Manual Editing

You can edit the JSON file directly:

```json
{
  "nvram": ["57", "00", "02", "20", "XX"],
  "decoded": {
    "boot_type": "disk",
    "boot_unit": 2,
    "boot_slice": 0,
    "autoboot": true,
    "timeout": 0
  }
}
```

The checksum (byte 4) will be recalculated on load.

## Programmatic API (For Downstream Emulators)

### Setting Boot Options

```cpp
#include "hbios_dispatch.h"

HBIOSDispatch hbios;

// Boot ROM app 'C' (CP/M)
hbios.setBootOption("C");

// Boot disk unit 2, slice 3
hbios.setBootOption("2.3");

// Show boot menu
hbios.setBootOption("H");

// Clear boot option
hbios.setBootOption("");
```

### Direct NVRAM Access

```cpp
// Read NVRAM (5 bytes)
const uint8_t* nvram = hbios.getNvram();

// Write NVRAM (recalculates checksum)
uint8_t data[5] = {'W', 'C', 0x80, 0x20, 0};
hbios.setNvram(data);

// Check if initialized
if (hbios.isNvramInitialized()) {
    // NVRAM has valid configuration
}
```

### Example: iOS Boot Picker

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
    emulator.hbios.setBootOption(newValue)
}
```

## HBIOS Functions Implemented

### RTC NVRAM Functions

| Function | Code | Description |
|----------|------|-------------|
| RTCGETTIM | 0x20 | Get current time (BCD format) |
| RTCSETTIM | 0x21 | Set time (ignored) |
| RTCGETBYT | 0x22 | Get NVRAM byte: C=index, returns E=value |
| RTCSETBYT | 0x23 | Set NVRAM byte: C=index, E=value |
| RTCGETBLK | 0x24 | Get NVRAM block: HL=buffer (5 bytes) |
| RTCSETBLK | 0x25 | Set NVRAM block: HL=buffer (5 bytes) |
| RTCDEVICE | 0x28 | Device info: returns C=0x40 (emulated) |

### System Switch Functions

| Function | Code | Description |
|----------|------|-------------|
| SYSGET_SWITCH | F8/C0 | Get switch value |
| SYSSET_SWITCH | F9/C0 | Set switch value |
| SYSGET_RTCCNT | F8/20 | Get RTC count (returns 1) |

### SYSGET_SWITCH Return Values (D=0xFF)

| A Value | Z Flag | Meaning |
|---------|--------|---------|
| 0 | NZ | No NVRAM hardware |
| 1 | NZ | NVRAM present but not initialized |
| 'W' (0x57) | Z | NVRAM present and initialized |

## Troubleshooting

### "NVRAM Not Found" in SYSCONF

This shouldn't happen anymore. If it does, check that:
- SYSGET_RTCCNT returns 1 (not 0)
- SYSGET_SWITCH with D=0xFF returns A=1 (not A=0) when uninitialized

### Boot Settings Not Persisting

- Check that `~/.config/romwbw_emu/` directory exists
- Check file permissions on `nvram.json`
- Ensure emulator exits cleanly (not killed with SIGKILL)

### --boot Overrides Saved Settings

This is intentional. Command line options take precedence over saved settings.
To use saved settings, omit the `--boot` option.

## References

- RomWBW Source: `Source/HBIOS/sysconf.asm` - SYSCONF utility
- RomWBW Source: `Source/HBIOS/romldr.asm` - Boot loader NVRAM handling
- RomWBW Source: `Source/HBIOS/hbios.asm` - HBIOS switch functions
- RomWBW Source: `Source/Doc/SystemGuide.md` - NVRAM documentation
