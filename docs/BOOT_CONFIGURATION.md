# Boot Configuration Guide

**Date:** 2026-01-15
**Status:** Complete - Full NVRAM implementation with persistence

## Overview

The emulator supports RomWBW's boot configuration system via emulated RTC NVRAM. Boot settings can be configured through:

1. **Command line** (`--boot=`) - For scripted/automated use
2. **SYSCONF utility** (`W` at boot menu) - Interactive configuration
3. **Programmatic API** (`setNvramSetting()`) - For GUI apps

Settings persist across sessions in `$XDG_CONFIG_HOME/romwbw_emu/nvram` (default `~/.config/romwbw_emu/nvram`); `XDG_CONFIG_HOME` support is new in v1.34.

## Quick Start

### Command Line Examples

```bash
# Auto-boot CP/M from ROM
./romwbw_emu --romwbw=roms/emu_avw.rom --boot=C

# Auto-boot from the first hard disk (unit 2), slice 0
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disk.img --boot=2

# Auto-boot from the first hard disk (unit 2), slice 3
./romwbw_emu --romwbw=roms/emu_avw.rom --disk0=disk.img --boot=2.3

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
| `2` | Boot disk unit 2 (first hard disk), slice 0 |
| `2.3` | Boot disk unit 2, slice 3 |

Boot unit numbering: unit 0 is the RAM disk and unit 1 is the ROM disk - neither contains a bootable OS, so booting `0` reports "No system image on disk". Hard disks are units 2 and up in `--disk0`..`--disk15` order (`--disk0` = unit 2). Press `D` at the boot menu to list the disk units.

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

`$XDG_CONFIG_HOME/romwbw_emu/nvram` if `XDG_CONFIG_HOME` is set (it must be an absolute path), otherwise `~/.config/romwbw_emu/nvram`.

`XDG_CONFIG_HOME` support is new in v1.34; a setting saved by an older version in `~/.config/romwbw_emu/nvram` is still picked up when the new location is empty, and migrates to the new location on the next clean exit.

### File Format

The NVRAM file is a plain text file containing a single line with the boot setting string. For example:

```
C
```

Valid contents include any boot format string such as `C`, `Z`, `H`, `2`, `2.3`, etc.
See the Boot Format Reference table above for the full list.

### Manual Editing

You can edit the file directly with any text editor. Just write the desired boot
setting string (e.g., `C` or `2.3`) as the file contents.

## Programmatic API (For Downstream Emulators)

### Setting Boot Options

```cpp
#include "hbios_dispatch.h"

HBIOSDispatch hbios;

// Boot ROM app 'C' (CP/M)
hbios.setNvramSetting("C");

// Boot disk unit 2, slice 3
hbios.setNvramSetting("2.3");

// Show boot menu
hbios.setNvramSetting("H");

// Clear boot option
hbios.setNvramSetting("");

// Read the current setting
std::string setting = hbios.getNvramSetting();

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
        // Hard disks are boot units 2+ (units 0/1 are the RAM/ROM disks)
        Text("Disk \(disk)").tag("\(disk + 2)")
    }
}
.onChange(of: bootSelection) { newValue in
    emulator.hbios.setNvramSetting(newValue)
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

- Check that the config directory exists (`$XDG_CONFIG_HOME/romwbw_emu`, default `~/.config/romwbw_emu`)
- Check file permissions on `nvram`
- Ensure emulator exits cleanly (not killed with SIGKILL)

### --boot Overrides Saved Settings

This is intentional. Command line options take precedence over saved settings.
To use saved settings, omit the `--boot` option.

It overrides for **that run only** and is not written back to `nvram`. It used
to be saved at exit like any other NVRAM state, which meant any automated run
that booted the emulator silently replaced whatever the developer had
configured. A target the guest sets with `SYSCONF` during the run is a different
thing and is still saved.

### Getting Back to the Menu

```bash
./romwbw_emu --romwbw=roms/emu_avw.rom --boot=none
```

`--boot=none` (or `off`) removes the persisted setting and comes up at the boot
menu. `--boot=H` shows the menu for one run without forgetting anything;
`--boot=none` is what forgets it. Clearing the in-emulator NVRAM is not enough
on its own - the setting lives in a file that outlives the run.

It removes one file: the `nvram` under the config directory that run selected
(`$XDG_CONFIG_HOME/romwbw_emu/nvram`, or `~/.config/romwbw_emu/nvram` when
`XDG_CONFIG_HOME` is unset). A setting saved before `XDG_CONFIG_HOME` support
lives in `~/.config` whatever `XDG_CONFIG_HOME` says; that file is still read as
a migration fallback and is **not** removed, because a run with
`XDG_CONFIG_HOME` pointed at a temp directory would then delete a real setting
outside it. `--boot=none` names that file when it finds one - delete it by hand
to clear it too.

## References

- RomWBW Source: `Source/HBIOS/sysconf.asm` - SYSCONF utility
- RomWBW Source: `Source/HBIOS/romldr.asm` - Boot loader NVRAM handling
- RomWBW Source: `Source/HBIOS/hbios.asm` - HBIOS switch functions
- RomWBW Source: `Source/Doc/SystemGuide.md` - NVRAM documentation
