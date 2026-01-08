# Boot Option Implementation

**Date:** 2026-01-08
**Status:** Working - NVRAM switches approach implemented

## Summary

The `--boot=CMD` option is now implemented using emulated RTC NVRAM switches. This replaces the previous character queueing approach which didn't work because the ROM boot loader polls input and consumed characters before processing them.

## How It Works

### RTC NVRAM Emulation

RomWBW systems with RTC chips (like DS1305, DS1307) store boot configuration in battery-backed NVRAM. The emulator now provides this functionality:

1. **NVRAM Switches Structure (4 bytes):**
   - Byte 0: Status ('W' = initialized)
   - Byte 1: Boot character (ROM app key) or disk slice number
   - Byte 2: Boot options (bit 7 = ROM/DISK flag, bits 0-6 = disk unit)
   - Byte 3: Autoboot flags (bit 5 = auto-enable, bits 0-3 = timeout)

2. **HBIOS SYSGET_SWITCH Function:**
   - D=0xFF: Returns status byte in A, Z flag set if 'W'
   - D=1 (NVSW_BOOTOPTS): Returns L=boot char/slice, H=boot options
   - D=3 (NVSW_AUTOBOOT): Returns L=autoboot flags

3. **Boot Loader Flow:**
   - Checks NVRAM status (D=0xFF)
   - If initialized ('W'), reads autoboot settings
   - If autoboot enabled, reads boot options after timeout
   - Boots from configured ROM app or disk

### Requirements

**Note:** Only `emu_avw.rom` works with this emulator. Other RomWBW ROMs (SBC_simh_std.rom, etc.) are built for hardware and won't work correctly without the emulator's HBIOS stub.

### Usage

```bash
# Boot from disk unit 0 (first hard disk)
./src/romwbw_emu --romwbw=roms/emu_avw.rom --disk0=mydisk.img --boot=0

# Boot from disk unit 0, slice 2
./src/romwbw_emu --romwbw=roms/emu_avw.rom --disk0=mydisk.img --boot=0.2

# Boot ROM application 'C' (CP/M)
./src/romwbw_emu --romwbw=roms/emu_avw.rom --boot=C

# Show help menu (default)
./src/romwbw_emu --romwbw=roms/emu_avw.rom --boot=H
```

## Implementation Details

### Files Changed

| File | Changes |
|------|---------|
| `src/hbios_dispatch.h` | Added NVRAM switch enums, `setBootOption()` method, `nvram_switches[4]` member |
| `src/hbios_dispatch.cc` | Implemented SYSGET_SWITCH with D=0xFF/1/3, added `setBootOption()` |
| `src/romwbw_emu.cc` | Replaced character queueing with `setBootOption()` call |

### Key Code Changes

**SYSGET_SWITCH Handler (hbios_dispatch.cc:1473-1530):**
```cpp
case SYSGET_SWITCH: {
  uint8_t switch_num = cpu->regs.DE.get_high();
  if (switch_num == 0xFF) {
    // Return NVRAM status in A (Z flag set if 'W')
    // Must return early to preserve A register
    uint8_t status = nvram_switches[0];
    cpu->regs.AF.set_high(status);
    // ... set Z flag based on status == 'W' ...
    doRet();
    return;  // Don't call setResult() which would overwrite A
  } else if (switch_num == NVSW_BOOTOPTS) {
    cpu->regs.HL.set_low(nvram_switches[1]);
    cpu->regs.HL.set_high(nvram_switches[2]);
  } else if (switch_num == NVSW_AUTOBOOT) {
    cpu->regs.HL.set_low(nvram_switches[3]);
  }
  break;
}
```

**setBootOption (hbios_dispatch.cc:2458-2525):**
```cpp
void HBIOSDispatch::setBootOption(const std::string& boot_str) {
  if (boot_str.empty()) {
    nvram_switches[0] = 0;  // Not initialized
    return;
  }

  nvram_switches[0] = 'W';  // Initialized

  if (isalpha(boot_str[0])) {
    // ROM app boot
    nvram_switches[1] = toupper(boot_str[0]);  // App key
    nvram_switches[2] = BOPTS_ROM;              // ROM flag
    nvram_switches[3] = ABOOT_AUTO;             // Enable autoboot
  } else if (isdigit(boot_str[0])) {
    // Disk boot - parse unit[.slice]
    // ... parse unit and slice ...
    nvram_switches[1] = slice;
    nvram_switches[2] = unit;  // Bit 7 clear = disk boot
    nvram_switches[3] = ABOOT_AUTO;
  }
}
```

## Testing Results

With `--boot=0 --debug`, the output shows:

```
[BOOT] Disk boot: unit=0 slice=0 (switches: 57 00 00 20)
...
[SYSGET_SWITCH] status check: A=0x57 ('W') INITIALIZED
NV Switches Found
[SYSGET_SWITCH] AUTOBOOT: ENABLED, timeout=0 sec (L=0x20)
AutoBoot in 0 Seconds (<esc> aborts, <enter> now)...
[SYSGET_SWITCH] BOOTOPTS: Disk unit=0 slice=0 (H=0x00 L=0x00)
Booting Disk Unit 0, Slice 0, Sector 0x00000000..
```

The boot mechanism works correctly. If the disk doesn't have a bootable system image, the error "No system image on disk" is shown, which is expected behavior.

## Previous Approach (Character Queueing)

The previous implementation queued characters to console input:

```cpp
// OLD CODE - didn't work
if (!boot_string.empty()) {
  for (char ch : boot_string) emu_console_queue_char(ch);
  emu_console_queue_char('\r');
}
```

This failed because:
1. The ROM boot loader polls console input during initialization
2. Characters were consumed before the boot menu appeared
3. Input was timing-dependent and unreliable

## Porting to Downstream Projects (iOS, Web, etc.)

The boot option feature is implemented in the shared `HBIOSDispatch` class, so downstream projects automatically get the SYSGET_SWITCH handling. To enable auto-boot:

### What You Need To Do

1. **Call `setBootOption()` before starting emulation:**
   ```cpp
   HBIOSDispatch* hbios = emulator->getHBIOS();
   hbios->setBootOption("0");      // Boot from disk unit 0
   hbios->setBootOption("0.2");    // Boot from disk unit 0, slice 2
   hbios->setBootOption("C");      // Boot ROM app 'C' (CP/M)
   hbios->setBootOption("");       // No auto-boot (show menu)
   ```

2. **Get the boot option from your platform:**
   - **iOS:** From UI picker, settings, or passed via URL scheme
   - **Web:** From URL query parameter (e.g., `?boot=0`)
   - **Mac:** From command line or preferences

### What's Already Done For You

The `HBIOSDispatch` class handles everything else:
- NVRAM switch storage (`nvram_switches[4]`)
- SYSGET_SWITCH function dispatch (D=0xFF, D=1, D=3)
- Parsing boot string into switch values

### Example Integration (iOS)

```objc
// In your emulator setup code
NSString *bootOption = [[NSUserDefaults standardUserDefaults] stringForKey:@"bootOption"];
if (bootOption) {
    hbios->setBootOption([bootOption UTF8String]);
}
```

### Example Integration (Web/WASM)

```javascript
// Get boot option from URL: emulator.html?boot=0
const urlParams = new URLSearchParams(window.location.search);
const bootOption = urlParams.get('boot') || '';
Module.ccall('set_boot_option', null, ['string'], [bootOption]);
```

### API Summary

| Method | Description |
|--------|-------------|
| `setBootOption(const std::string&)` | Configure NVRAM for auto-boot |
| `nvram_switches[4]` | Internal storage (don't access directly) |

### Boot Option Format

| Format | Action |
|--------|--------|
| `""` (empty) | Show boot menu (no auto-boot) |
| `"H"` | Show help menu |
| `"C"`, `"Z"`, etc. | Boot ROM application with that key |
| `"0"`, `"1"`, etc. | Boot from disk unit N, slice 0 |
| `"0.2"` | Boot from disk unit 0, slice 2 |

## References

- RomWBW Source: `Source/HBIOS/romldr.asm` - Boot loader NVRAM handling
- RomWBW Source: `Source/HBIOS/hbios.asm` - SYS_GETSWITCH implementation
- RomWBW Source: `Source/HBIOS/hbios.inc` - NVSW_* and BOPTS_* constants
