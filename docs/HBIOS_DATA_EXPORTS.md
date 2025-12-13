# HBIOS Data Exports

This document describes the data structures and tables that HBIOS exports to the rest of the RomWBW system (ROM loader, CBIOS, applications).

Source: `~/esrc/RomWBW/Source/HBIOS/hbios.inc`

## Overview

HBIOS exports data in two ways:
1. **Static data blocks** - Fixed structures in memory (HCB, HBX proxy)
2. **Function queries** - Data returned via HBIOS function calls (SYSGET, DIOCAP, etc.)

---

## 1. HBIOS Configuration Block (HCB)

**Location:** `HCB_LOC` = 0x0100 (in HBIOS bank, which is bank 0x80)
**Size:** `HCB_SIZ` = 0x0100 (256 bytes)

The HCB is a static data block that the CBIOS copies to its own memory during initialization. It contains system configuration that was determined at build time and during HBIOS startup.

### HCB Structure (from hbios.inc)

| Offset | Name | Size | Description |
|--------|------|------|-------------|
| 0x00 | (entry) | 3 | JP HB_START instruction |
| 0x03 | HCB_MARKER | 2 | Marker bytes: 'W', ~'W' (0x57, 0xA8) |
| 0x05 | HCB_VERSION | 2 | Version: byte0=(major<<4\|minor), byte1=(update<<4\|patch) |
| 0x07 | HCB_PLATFORM | 1 | Platform ID (see PLT_* constants) |
| 0x08 | HCB_CPUMHZ | 1 | CPU speed in MHz |
| 0x09 | HCB_CPUKHZ | 2 | CPU speed in KHz (word) |
| 0x0B | HCB_RAMBANKS | 1 | RAM size in 32KB banks (RAMSIZE/32) |
| 0x0C | HCB_ROMBANKS | 1 | ROM size in 32KB banks (ROMSIZE/32) |
| 0x0D | HCB_BOOTVOL | 2 | Boot volume: MSB=dev/unit, LSB=LU (set by loader) |
| 0x0F | HCB_BOOTBID | 1 | Bank ID of ROM page booted from (set by loader) |
| 0x10 | HCB_SERDEV | 1 | Primary serial device/unit (unit #0 by default) |
| 0x11 | HCB_CRTDEV | 1 | CRT display device/unit (0xFF until after init) |
| 0x12 | HCB_CONDEV | 1 | Active console device/unit (0xFF until after init) |
| 0x13 | HCB_DIAGLVL | 1 | Diagnostic output level |
| 0x14 | HCB_BOOTMODE | 1 | Boot mode (ROM/APP/IMG) |
| 0x20 | HCB_HEAP | 2 | Heap start address |
| 0x22 | HCB_HEAPTOP | 2 | Heap top address |
| 0x30 | CB_SWITCHES | 1 | NVR status: 0=none, 1=exists but not configured |
| 0x31 | CB_SW_AB_OPT | 2 | Auto boot options (used by ROMLDR) |
| 0x33 | CB_SW_AB_CFG | 1 | Auto boot config |
| 0x34 | CB_SW_CKSUM | 1 | Checksum (XOR=0) |
| 0xD8 | HCB_BIDCOM | 1 | Common bank ID (upper 32KB) |
| 0xD9 | HCB_BIDUSR | 1 | User/TPA bank ID |
| 0xDA | HCB_BIDBIOS | 1 | BIOS bank ID |
| 0xDB | HCB_BIDAUX | 1 | Auxiliary bank ID |
| 0xDC | HCB_BIDRAMD0 | 1 | First RAM disk bank ID |
| 0xDD | HCB_RAMD_BNKS | 1 | RAM disk bank count |
| 0xDE | HCB_BIDROMD0 | 1 | First ROM disk bank ID |
| 0xDF | HCB_ROMD_BNKS | 1 | ROM disk bank count |
| 0xE0 | HCB_BIDAPP0 | 1 | First application bank ID |
| 0xE1 | HCB_APP_BNKS | 1 | Application bank count |

### Platform IDs (PLT_*)

| ID | Name | Description |
|----|------|-------------|
| 0 | PLT_NONE | Undefined |
| 1 | PLT_SBC | SBC ECB Z80 SBC |
| 7 | PLT_RCZ80 | RCBus w/ Z80 |
| 13 | PLT_MBC | Multi Board Computer |
| ... | ... | (see hbios.inc for full list) |

### When HCB is Read

- **CBIOS initialization**: Copies entire HCB to user memory to access bank IDs, disk configuration
- **ROM loader**: Reads boot volume, console device settings

---

## 2. HBIOS Proxy Block (HBX)

**Location:** `HBX_XFC` = 0xFFE0 (top of common memory, 32 bytes)
**Size:** 32 bytes (0xFFE0-0xFFFF)

The proxy block is installed in the upper 32 bytes of memory and provides:
- State variables for bank switching (data area: 0xFFE0-0xFFEF)
- Fixed entry points for HBIOS calls (jump table: 0xFFF0-0xFFFF)

### HBX Data Area (HBX_XFCDAT = 0xFFE0)

| Offset | Address | Name | Size | Description |
|--------|---------|------|------|-------------|
| +0 | 0xFFE0 | HB_CURBNK | 1 | Currently active low memory bank ID |
| +1 | 0xFFE1 | HB_INVBNK | 1 | Bank active at time of HBIOS invocation |
| +2 | 0xFFE2 | HB_SRCADR | 2 | Bank copy: source address |
| +4 | 0xFFE4 | HB_SRCBNK | 1 | Bank copy: source bank ID |
| +5 | 0xFFE5 | HB_DSTADR | 2 | Bank copy: destination address |
| +7 | 0xFFE7 | HB_DSTBNK | 1 | Bank copy: destination bank ID |
| +8 | 0xFFE8 | HB_CPYLEN | 2 | Bank copy: length |
| +14 | 0xFFEE | HB_RTCVAL | 1 | RTC latch shadow value |
| +15 | 0xFFEF | HB_LOCK | 1 | HBIOS mutex lock |

### HBX Jump Table (HBX_XFCFNS = 0xFFF0)

| Offset | Address | Name | Description |
|--------|---------|------|-------------|
| +0 | 0xFFF0 | HB_INVOKE | Invoke HBIOS function (B=func, C=unit) |
| +3 | 0xFFF3 | HB_BNKSEL | Select low memory bank (A=bank ID) |
| +6 | 0xFFF6 | HB_BNKCPY | Interbank memory copy |
| +9 | 0xFFF9 | HB_BNKCALL | Interbank function call |
| +12 | 0xFFFC | HB_IDENT | Pointer to HBIOS ident data block |

---

## 3. SYSGET Function Queries (Function 0xF8)

SYSGET allows querying system information at runtime. Call with B=0xF8, C=subfunction.

### Device Counts

| Subfunction | Name | Returns |
|-------------|------|---------|
| 0x00 | CIOCNT | E = character device count |
| 0x10 | DIOCNT | E = disk device count |
| 0x20 | RTCCNT | E = RTC device count |
| 0x40 | VDACNT | E = video device count |
| 0x50 | SNDCNT | E = sound device count |

### Device Function Tables

| Subfunction | Name | Returns |
|-------------|------|---------|
| 0x01 | CIOFN | HL = CIO function table, DE = unit data |
| 0x11 | DIOFN | HL = DIO function table, DE = unit data |
| 0x41 | VDAFN | HL = VDA function table, DE = unit data |
| 0x51 | SNDFN | HL = SND function table, DE = unit data |

### Timer/Clock

| Subfunction | Name | Returns |
|-------------|------|---------|
| 0xD0 | TIMER | DE:HL = 32-bit timer tick count |
| 0xD1 | SECONDS | DE:HL = 32-bit seconds count |

### System Info

| Subfunction | Name | Returns |
|-------------|------|---------|
| 0xE0 | BOOTINFO | HL = boot volume, L = boot bank ID |
| 0xF0 | CPUINFO | CPU type, speed info |
| 0xF1 | MEMINFO | D = ROM banks, E = RAM banks |
| 0xF2 | BNKINFO | D = BIOS bank, E = user bank |
| 0xF3 | CPUSPD | Clock multiplier, wait states |
| 0xF4 | PANEL | L = front panel switch values |

---

## 4. Disk Information Queries

### DIOCAP (0x1A) - Disk Capacity

**Input:** B=0x1A, C=unit
**Returns:**
- DE:HL = block count (32-bit)
- BC = block size in bytes

### DIOGEOM (0x1B) - Disk Geometry

**Input:** B=0x1B, C=unit
**Returns:**
- HL = cylinder count
- D = heads (bits 0-6), LBA flag (bit 7)
- E = sectors per track
- BC = block size

### DIODEVICE (0x17) - Device Info

**Input:** B=0x17, C=unit
**Returns:**
- D = device type
- E = device number
- C = device attributes (bit 5 = high capacity)

---

## 5. Emulator Implementation Notes

For the emulator, these data exports need to be provided:

### Static Data (in memory)

1. **HCB at 0x0100** (in BIOS bank):
   - Set platform, CPU speed, RAM/ROM bank counts
   - Configure bank IDs for RAM disk, ROM disk, user, BIOS
   - Set console device

2. **HBX at 0xFFE0-0xFFFF**:
   - Initialize bank state variables
   - Entry points trap to emulator

### Function Queries (trap handlers)

When PC reaches 0xFFF0 (HB_INVOKE), handle based on B register:

| B | Function | What to Return |
|---|----------|----------------|
| 0xF8 | SYSGET | Dispatch on C for system info |
| 0x1A | DIOCAP | Return disk size for unit C |
| 0x1B | DIOGEOM | Return disk geometry for unit C |
| 0x17 | DIODEVICE | Return device type/attrs for unit C |

### Data Flow

```
ROM Loader / CBIOS
        |
        | Reads HCB from BIOS bank (0x0100)
        | Calls SYSGET for device counts
        | Calls DIOCAP/DIOGEOM for disk info
        v
    HBIOS (trapped by emulator)
        |
        | Emulator provides responses from C++ data
        v
    Emulator internal disk/device state
```

---

## 6. Device Type Constants

### Character Device IDs (CIODEV_*)

| ID | Name | Description |
|----|------|-------------|
| 0x00 | CIODEV_UART | 16550-style UART |
| 0x01 | CIODEV_ASCI | Z180 ASCI |
| 0x02 | CIODEV_TERM | Terminal |
| 0x05 | CIODEV_SIO | Z80 SIO |
| 0x06 | CIODEV_ACIA | 6850 ACIA |

### Disk Device IDs (DIODEV_*)

| ID | Name | Description |
|----|------|-------------|
| 0x00 | DIODEV_MD | Memory disk (ROM/RAM) |
| 0x01 | DIODEV_FD | Floppy disk |
| 0x03 | DIODEV_IDE | IDE hard disk |
| 0x06 | DIODEV_SD | SD card |

### Media IDs (MID_*)

| ID | Name | Description |
|----|------|-------------|
| 0 | MID_NONE | No media |
| 1 | MID_MDROM | ROM disk |
| 2 | MID_MDRAM | RAM disk |
| 4 | MID_HD | Hard disk |
| 5 | MID_FD720 | 720KB floppy |
| 6 | MID_FD144 | 1.44MB floppy |

---

## 7. Error Codes (ERR_*)

| Code | Name | Description |
|------|------|-------------|
| 0 | ERR_NONE | Success |
| -1 | ERR_UNDEF | Undefined error |
| -2 | ERR_NOTIMPL | Function not implemented |
| -3 | ERR_NOFUNC | Invalid function |
| -4 | ERR_NOUNIT | Invalid unit number |
| -5 | ERR_NOMEM | Out of memory |
| -7 | ERR_NOMEDIA | Media not present |
| -8 | ERR_NOHW | Hardware not present |
| -9 | ERR_IO | I/O error |
| -10 | ERR_READONLY | Write to read-only media |
| -11 | ERR_TIMEOUT | Device timeout |

---

## Sources

- Local: `~/esrc/RomWBW/Source/HBIOS/hbios.inc`
- [RomWBW hbios.inc](https://github.com/wwarthen/RomWBW/blob/master/Source/HBIOS/hbios.inc)
- [RomWBW API.txt](https://github.com/wwarthen/RomWBW/blob/master/Source/HBIOS/API.txt)
- [RomWBW cbios.asm](https://github.com/wwarthen/RomWBW/blob/master/Source/CBIOS/cbios.asm)
