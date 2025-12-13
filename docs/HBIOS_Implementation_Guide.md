# HBIOS Implementation Guide for CPMEmu

## Overview

This document describes the RomWBW HBIOS architecture and what the emulator needs to implement for proper boot support. It addresses boot phases, banking, register conventions, and function requirements.

## Boot Phases

### Phase 1: ROM Startup (Bank 0 at address 0)

When the CPU resets, ROM bank 0 (BID_BOOT = 0x00) is mapped to the lower 32K. Execution starts at address 0x0000.

**What happens:**
1. Basic CPU initialization (DI, IM 1)
2. HBIOS copies itself from ROM bank 0 to RAM bank 0x80 (BID_BIOS)
3. The HBIOS proxy (512 bytes at 0xFE00-0xFFFF) is installed in common RAM
4. Hardware drivers are initialized and device tables are built
5. Control transfers to the boot loader (romldr.asm) in ROM bank 1

**Banking state:**
- Lower 32K: Initially ROM bank 0, then RAM bank 0x80 (BIOS bank)
- Upper 32K: Always "common" RAM (bank 0x8F on standard 512K systems)

### Phase 2: Boot Loader (romldr.asm)

The boot loader runs from ROM bank 1 (BID_IMG0) and presents the boot menu.

**Key activities:**
1. Relocates itself to 0x8000 (start of common RAM)
2. Switches lower 32K to user bank (0x8E)
3. Copies page zero template to user bank
4. Displays boot menu and waits for user selection
5. Loads selected OS image

**Important HBIOS calls made by boot loader:**
- `BF_SYSPEEK` (0xFA): Read bytes from BIOS bank
- `BF_SYSSETBNK` (0xF2): Switch banks
- `BF_SYSGET` (0xF8): Query system info (console dev, disk count, etc.)
- `BF_CIOIN/OUT/IST/OST` (0x00-0x03): Console I/O
- `BF_DIOREAD` (0x13): Read disk sectors for disk boot

### Phase 3: OS Boot

The boot loader copies the selected OS image to RAM and transfers control.

**ROM boot (selecting M, Z, etc.):**
1. OS image is copied from ROM bank 1 to user RAM at 0xD000
2. RST 08 vector (JP 0xFFF0) is set up in user page zero
3. JP to OS entry point (typically CBIOS at 0xD000)

**Disk boot (selecting unit.slice):**
1. Boot sector read from disk
2. Boot loader loaded and executed
3. OS loaded from disk

## Memory Banking Architecture

### Bank IDs

```
ROM Banks (physical 0x00-0x0F):
  0x00 = BID_BOOT   - HBIOS kernel
  0x01 = BID_IMG0   - Boot loader, Monitor, OS images
  0x02 = BID_IMG1   - ROM applications
  0x03 = BID_IMG2   - Utilities, fonts
  0x04-0x0F         - ROM disk data

RAM Banks (physical 0x80-0x8F for 512K):
  0x80 = BID_BIOS   - Running HBIOS copy
  0x81-0x88         - RAM disk data
  0x89-0x8B         - Application banks
  0x8C              - CP/M 3 buffers
  0x8D              - CP/M 3 OS
  0x8E = BID_USR    - User TPA (where OS runs)
  0x8F = BID_COM    - Common bank (upper 32K always mapped here)
```

### Proxy at 0xFFE0-0xFFFF

The HBIOS proxy lives in common RAM and provides the interface for calling HBIOS:

```
0xFFE0 (+0)  HB_CURBNK   - Currently active lower bank ID
0xFFE1 (+1)  HB_INVBNK   - Bank that was active when HBIOS was invoked
0xFFE2 (+2)  HB_SRCADR   - BNKCPY source address
0xFFE4 (+4)  HB_SRCBNK   - BNKCPY source bank
0xFFE5 (+5)  HB_DSTADR   - BNKCPY destination address
0xFFE7 (+7)  HB_DSTBNK   - BNKCPY destination bank
0xFFE8 (+8)  HB_CPYLEN   - BNKCPY length
0xFFEE (+14) HB_RTCVAL   - RTC latch shadow
0xFFEF (+15) HB_LOCK     - HBIOS mutex lock

0xFFF0 (+16) HB_INVOKE   - JP HBX_INVOKE (HBIOS entry point)
0xFFF3 (+19) HB_BNKSEL   - JP HBX_BNKSEL (bank select)
0xFFF6 (+22) HB_BNKCPY   - JP HBX_BNKCPY (interbank copy)
0xFFF9 (+25) HB_BNKCALL  - JP HBX_BNKCALL (interbank call)
0xFFFC (+28) HB_IDENT    - Address of HBIOS ident block
```

### HBIOS Invocation Flow

1. Caller sets up registers (B=function, C=unit, etc.)
2. Caller executes `RST 08` or `CALL 0xFFF0`
3. Proxy saves caller's bank (HB_INVBNK)
4. Proxy switches to HBIOS bank (0x80)
5. Proxy calls HB_DISPATCH with original registers
6. On return, proxy restores caller's bank
7. Return to caller with results

**For our emulator:** We intercept at 0xFFF0 and handle HBIOS functions directly in C++, so we don't need the bank switching logic.

## HBIOS Function Reference

### Register Conventions (All Functions)

**Inputs:**
- B = Function code
- C = Unit number (for device functions) or subfunction
- Other registers as documented per function

**Outputs:**
- A = Result code (0 = success, negative = error)
- Carry flag mirrors bit 7 of A (set on error)
- Other registers as documented per function

**Preserved registers:** HBIOS generally preserves IX, IY, and the alternate register set. Specific function documentation should be consulted.

### Error Codes

```
ERR_NONE    (0)   - Success
ERR_UNDEF   (-1)  - Undefined error
ERR_NOTIMPL (-2)  - Function not implemented
ERR_NOFUNC  (-3)  - Invalid function
ERR_NOUNIT  (-4)  - Invalid unit number
ERR_NOMEM   (-5)  - Out of memory
ERR_RANGE   (-6)  - Parameter out of range
ERR_NOMEDIA (-7)  - Media not present
ERR_NOHW    (-8)  - Hardware not present
ERR_IO      (-9)  - I/O error
ERR_READONLY(-10) - Write to read-only media
ERR_TIMEOUT (-11) - Device timeout
ERR_BADCFG  (-12) - Invalid configuration
ERR_INTERNAL(-13) - Internal error
```

### Character I/O Functions (0x00-0x06)

#### CIOIN (0x00) - Character Input
```
Entry: B=0x00, C=unit
Exit:  A=status, E=character received
Notes: Blocks until character available
```

#### CIOOUT (0x01) - Character Output
```
Entry: B=0x01, C=unit, E=character
Exit:  A=status
Notes: Blocks until output buffer has space
```

#### CIOIST (0x02) - Input Status
```
Entry: B=0x02, C=unit
Exit:  A=status, E=bytes pending (or 0xFF if >255)
```

#### CIOOST (0x03) - Output Status
```
Entry: B=0x03, C=unit
Exit:  A=status, E=buffer space available (or 0xFF if >255)
```

#### CIOINIT (0x04) - Initialize Device
```
Entry: B=0x04, C=unit, DE=line characteristics, L=terminal type
Exit:  A=status
```

#### CIOQUERY (0x05) - Query Configuration
```
Entry: B=0x05, C=unit
Exit:  A=status, DE=line characteristics, L=terminal type
```

#### CIODEVICE (0x06) - Device Information
```
Entry: B=0x06, C=unit
Exit:  A=status, D=device type, E=device number, C=attributes, H=mode, L=base I/O
```

### Disk I/O Functions (0x10-0x1B)

#### DIOSTATUS (0x10) - Disk Status
```
Entry: B=0x10, C=unit
Exit:  A=status of previous operation
```

#### DIORESET (0x11) - Reset Disk Interface
```
Entry: B=0x11, C=unit
Exit:  A=status
```

#### DIOSEEK (0x12) - Seek to Sector
```
Entry: B=0x12, C=unit
       D bit 7: Address type (0=CHS, 1=LBA)
       For LBA: DE:HL = 32-bit LBA
       For CHS: D bits 0-6=head, E=sector, HL=track
Exit:  A=status
Notes: Establishes current sector for subsequent read/write
```

#### DIOREAD (0x13) - Read Sectors
```
Entry: B=0x13, C=unit, HL=buffer address, D=buffer bank, E=sector count
Exit:  A=status, E=sectors actually read
Notes: Reads from current position set by DIOSEEK
       Increments current sector after each read
       If buffer not in upper 32K, uses double buffering
```

#### DIOWRITE (0x14) - Write Sectors
```
Entry: B=0x14, C=unit, HL=buffer address, D=buffer bank, E=sector count
Exit:  A=status, E=sectors actually written
```

#### DIOVERIFY (0x15) - Verify Sectors
```
Entry: B=0x15, C=unit, HL=buffer address, D=buffer bank, E=sector count
Exit:  A=status, E=sectors verified
```

#### DIOFORMAT (0x16) - Format Track
```
Entry: B=0x16, C=unit, D=head, HL=cylinder, E=fill byte
Exit:  A=status
```

#### DIODEVICE (0x17) - Device Information
```
Entry: B=0x17, C=unit
Exit:  A=status, D=device type, E=device number, C=attributes, H=mode, L=base I/O

Device Types:
  0x00 = MD (Memory Disk - ROM/RAM)
  0x01 = FD (Floppy)
  0x02 = RF (RAM Floppy)
  0x03 = IDE
  0x04 = ATAPI
  0x05 = PPIDE
  0x06 = SD
  ...

Attributes byte:
  Bit 7: 1=Floppy, 0=Hard disk type
  For Hard Disk:
    Bit 6: Removable
    Bits 5-3: Type (0=Hard, 1=CF, 2=SD, 3=USB, 4=ROM, 5=RAM, 6=RAMF)
```

#### DIOMEDIA (0x18) - Media Information
```
Entry: B=0x18, C=unit, E bit 0=enable discovery
Exit:  A=status, E=media ID

Media IDs:
  0x00 = MID_NONE
  0x01 = MID_MDROM (ROM disk)
  0x02 = MID_MDRAM (RAM disk)
  0x03 = MID_RF (RAM floppy)
  0x04 = MID_HD (Hard disk)
  0x05-0x09 = Various floppy formats
  0x0A = MID_HDNEW (HD with 1K sectors)
```

#### DIODEFMED (0x19) - Define Media
```
Entry: B=0x19, C=unit, E=media ID
Exit:  A=status
```

#### DIOCAP (0x1A) - Capacity
```
Entry: B=0x1A, C=unit
Exit:  A=status, DE:HL=total blocks, BC=block size
```

#### DIOGEOM (0x1B) - Geometry
```
Entry: B=0x1B, C=unit
Exit:  A=status, HL=cylinders, D bits 0-6=heads, D bit 7=LBA capable, E=sectors, BC=block size
```

### System Functions (0xF0-0xFC)

#### SYSRESET (0xF0) - System Reset
```
Entry: B=0xF0, C=reset type
       C=0x00: Internal (release transient heap)
       C=0x01: Warm (restart boot loader)
       C=0x02: Cold (full system restart)
       C=0x03: User (called from JP 0 handler)
Exit:  Does not return for warm/cold
```

#### SYSVER (0xF1) - Get Version
```
Entry: B=0xF1, C=0
Exit:  A=status, D=major.minor (BCD), E=update.patch (BCD), L=platform ID
```

#### SYSSETBNK (0xF2) - Set Bank
```
Entry: B=0xF2, C=new bank ID
Exit:  A=status, C=previous bank ID
Notes: Takes effect upon return from HBIOS
```

#### SYSGETBNK (0xF3) - Get Bank
```
Entry: B=0xF3
Exit:  A=status, C=current bank ID
```

#### SYSSETCPY (0xF4) - Setup Bank Copy
```
Entry: B=0xF4, D=dest bank, E=source bank, HL=byte count
Exit:  A=status
Notes: Sets up parameters for subsequent BNKCPY call
```

#### SYSBNKCPY (0xF5) - Execute Bank Copy
```
Entry: B=0xF5, HL=source address, DE=dest address
Exit:  A=status
Notes: Uses banks/count from previous SETCPY call
```

#### SYSALLOC (0xF6) - Allocate Heap Memory
```
Entry: B=0xF6, HL=size
Exit:  A=status, HL=address of allocated block
```

#### SYSFREE (0xF7) - Free Heap Memory
```
Entry: B=0xF7, HL=address
Exit:  A=status
Notes: Not fully implemented in real HBIOS
```

#### SYSGET (0xF8) - Get System Information
```
Entry: B=0xF8, C=subfunction
Exit:  Varies by subfunction

Subfunctions:
  0x00 CIOCNT:    Returns E=serial unit count
  0x01 CIOFN:     Returns HL=function addr, DE=data addr
  0x10 DIOCNT:    Returns E=disk unit count
  0x11 DIOFN:     Returns HL=function addr, DE=data addr
  0x20 RTCCNT:    Returns E=RTC unit count
  0x30 DSKYCNT:   Returns E=DSKY count
  0x40 VDACNT:    Returns E=video unit count
  0x41 VDAFN:     Returns HL=function addr, DE=data addr
  0x50 SNDCNT:    Returns E=sound unit count
  0x51 SNDFN:     Returns HL=function addr, DE=data addr
  0xC0 SWITCH:    NVRAM switches (D=key), returns L=value
  0xD0 TIMER:     Returns DE:HL=32-bit timer ticks
  0xD1 SECONDS:   Returns DE:HL=seconds, C=ticks in current second
  0xE0 BOOTINFO:  Returns DE=boot volume, L=boot bank
  0xF0 CPUINFO:   Returns H=CPU variant, L=MHz, DE=KHz, BC=oscillator KHz
  0xF1 MEMINFO:   Returns D=ROM banks, E=RAM banks
  0xF2 BNKINFO:   Returns D=BIOS bank ID (0x80), E=User bank ID (0x8E)
  0xF3 CPUSPD:    Returns L=clock mult, D=mem waits, E=I/O waits
  0xF4 PANEL:     Returns L=front panel switch value
  0xF5 APPBNKS:   Returns app bank info
```

#### SYSSET (0xF9) - Set System Parameters
```
Entry: B=0xF9, C=subfunction, other regs per subfunction

Subfunctions:
  0xC0 SWITCH:    Set NVRAM switch (D=key, L=value)
  0xD0 TIMER:     Set timer (DE:HL=value)
  0xD1 SECONDS:   Set seconds (DE:HL=value)
  0xE0 BOOTINFO:  Set boot info (DE=volume, L=bank)
  0xF3 CPUSPD:    Set CPU speed
  0xF4 PANEL:     Set panel LEDs (L=value)
```

#### SYSPEEK (0xFA) - Read Byte from Bank
```
Entry: B=0xFA, D=bank, HL=address
Exit:  A=status, E=byte value
```

#### SYSPOKE (0xFB) - Write Byte to Bank
```
Entry: B=0xFB, D=bank, HL=address, E=byte value
Exit:  A=status
```

#### SYSINT (0xFC) - Interrupt Management
```
Entry: B=0xFC, C=subfunction
  0x00 INTINF: Query interrupt info
       Exit: D=int mode, E=vector table size
  0x10 INTGET: Get vector (E=position)
       Exit: HL=vector
  0x20 INTSET: Set vector (E=position, HL=new vector)
       Exit: HL=previous vector
```

### Extension Functions (0xE0)

#### EXTSLICE (0xE0) - Slice Calculation
```
Entry: B=0xE0, D=disk unit, E=slice number
Exit:  A=status, B=device attributes, C=media ID, DE:HL=LBA offset
Notes: Used for HD1K partition support
```

## Emulator Implementation Notes

### What Must Be Implemented

1. **Console I/O (0x00-0x03)**: Essential for any interaction
2. **SYSVER (0xF1)**: Boot loader checks version
3. **SYSGET subfunctions**:
   - DIOCNT (0x10): Boot loader needs disk count
   - CIOCNT (0x00): Boot loader needs console count
   - BNKINFO (0xF2): OS images query this
   - MEMINFO (0xF1): OS images query this
   - BOOTINFO (0xE0): OS queries boot info
4. **SYSSETBNK/GETBNK (0xF2/0xF3)**: Boot loader switches banks
5. **SYSPEEK (0xFA)**: Boot loader reads from BIOS bank
6. **SYSSETCPY/BNKCPY (0xF4/0xF5)**: Boot loader copies OS images
7. **Disk functions (0x10-0x1B)**: For disk boot capability
8. **CIODEVICE (0x06)**: Boot loader queries console devices

### Banking for the Emulator

The emulator should:
1. Track the "current bank" but not actually switch memory
2. For PEEK/POKE, read/write from the appropriate bank's memory
3. For BNKCPY, copy between bank memories
4. For disk read/write with buffer bank, write to correct bank's memory

### ROM Disk Implementation

ROM disk (unit 0, MD0) reads from ROM banks starting at BID_ROMD0:
- Each 32K bank = 64 sectors (512 bytes each)
- LBA 0-63 = Bank 0x04, LBA 64-127 = Bank 0x05, etc.

RAM disk (unit 1, MD1) reads from RAM banks starting at BID_RAMD0 (0x81):
- Same layout as ROM disk

### Typical Boot Sequence

1. HBIOS initializes, presents boot menu
2. User selects "M" for CP/M 2.2
3. Boot loader:
   - Calls SYSGET BNKINFO to get BIOS/USER bank IDs
   - Calls SYSSETBNK to switch to user bank (0x8E)
   - Calls SYSSETCPY/BNKCPY to copy OS image from ROM bank 1 to user RAM
   - Sets up page zero with RST 08 -> JP 0xFFF0
   - JPs to OS entry point (0xD000)
4. OS (CBIOS) runs, uses RST 08 for HBIOS calls

### What Can Be Stubbed/Skipped

- RTC functions (0x20-0x28): Return no devices
- DSKY functions (0x30-0x38): Return no devices
- VDA functions (0x40-0x4F): Return no devices (unless implementing video)
- Sound functions (0x50-0x5F): Return no devices (unless implementing sound)
- SYSINT (0xFC): Can return interrupt mode 0 with 0 vectors
- SYSALLOC/FREE: Boot loader doesn't need heap

## CRITICAL BUGS IN CURRENT EMULATOR

### BUG #1: Function Code Mapping Error (CONFIRMED)

The emulator has **WRONG function codes** for system functions 0xF3 and 0xF5:

**Current emulator (WRONG):**
```cpp
HBF_SYSBNKCPY= 0xF3,  // Bank copy - WRONG!
HBF_SYSSETCPY= 0xF4,
HBF_SYSBCALL = 0xF5,  // Bank call - WRONG!
```

**Correct mapping (from hbios.inc):**
```
BF_SYSSETBNK = 0xF2  ; Set bank
BF_SYSGETBNK = 0xF3  ; Get bank  <-- MISSING in emulator!
BF_SYSSETCPY = 0xF4  ; Set copy params
BF_SYSBNKCPY = 0xF5  ; Execute bank copy <-- Mapped to BCALL!
```

**Impact:** When boot loader calls:
- SYSGETBNK (0xF3) to query current bank -> Emulator executes BNKCPY!
- SYSBNKCPY (0xF5) to copy OS image -> Emulator executes something else!

**Fix required in altair_emu.cc:**
```cpp
HBF_SYSSETBNK= 0xF2,  // Set bank
HBF_SYSGETBNK= 0xF3,  // Get bank (ADD THIS!)
HBF_SYSSETCPY= 0xF4,  // Set copy params
HBF_SYSBNKCPY= 0xF5,  // Execute bank copy (FIX THIS!)
HBF_SYSALLOC = 0xF6,
// Note: There is no SYSBCALL function - remove it!
```

### BUG #2: Fictional SYSBCALL Function

The emulator has `HBF_SYSBCALL = 0xF5` but **this function does not exist in HBIOS!**

`HB_BNKCALL` is a **proxy entry point** at address 0xFFF9 that allows inter-bank function calls. It is NOT invoked via RST 08 with a function code. Code calls it directly: `CALL 0xFFF9` with IX=target address, A=target bank.

The emulator's elaborate BCALL handling code (lines 2367-2473) handles a function that RomWBW never calls. Meanwhile, real SYSBNKCPY (0xF5) calls are being mishandled.

### Other Issues to Verify

1. **Bank state tracking**: HB_INVBNK should hold the bank that was active when HBIOS was called
2. **Disk buffer banking**: When D register specifies buffer bank, data goes to that bank

### Summary of Required Fixes

The current function code enum should be:
```cpp
enum HBIOSFunc {
  // Character I/O (0x00-0x06)
  HBF_CIOIN    = 0x00,
  HBF_CIOOUT   = 0x01,
  HBF_CIOIST   = 0x02,
  HBF_CIOOST   = 0x03,
  HBF_CIOINIT  = 0x04,
  HBF_CIOQUERY = 0x05,
  HBF_CIODEVICE= 0x06,

  // Disk I/O (0x10-0x1B)
  HBF_DIOSTATUS= 0x10,
  HBF_DIORESET = 0x11,
  HBF_DIOSEEK  = 0x12,
  HBF_DIOREAD  = 0x13,
  HBF_DIOWRITE = 0x14,
  HBF_DIOVERIFY= 0x15,
  HBF_DIOFORMAT= 0x16,
  HBF_DIODEVICE= 0x17,
  HBF_DIOMEDIA = 0x18,
  HBF_DIODEFMED= 0x19,
  HBF_DIOCAP   = 0x1A,
  HBF_DIOGEOM  = 0x1B,

  // Extension (0xE0)
  HBF_EXTSLICE = 0xE0,

  // System (0xF0-0xFC)
  HBF_SYSRESET = 0xF0,
  HBF_SYSVER   = 0xF1,
  HBF_SYSSETBNK= 0xF2,
  HBF_SYSGETBNK= 0xF3,  // FIX: Was missing, mapped to BNKCPY
  HBF_SYSSETCPY= 0xF4,
  HBF_SYSBNKCPY= 0xF5,  // FIX: Was mapped to fictional BCALL
  HBF_SYSALLOC = 0xF6,
  HBF_SYSFREE  = 0xF7,
  HBF_SYSGET   = 0xF8,
  HBF_SYSSET   = 0xF9,
  HBF_SYSPEEK  = 0xFA,
  HBF_SYSPOKE  = 0xFB,
  HBF_SYSINT   = 0xFC,
};
```

And add handlers for:
1. **SYSGETBNK (0xF3)**: Return current bank ID in C
2. Fix **SYSBNKCPY (0xF5)**: Execute interbank copy using params from SETCPY

## HCB (HBIOS Control Block) at 0x100

The HCB in HBIOS bank contains configuration data:
```
0x100: JP HB_START
0x103: Marker 'W', ~'W'
0x105: Version bytes
0x107: Platform ID
0x108: CPU MHz
0x109: CPU KHz (word)
0x10B: RAM bank count
0x10C: ROM bank count
0x10D: Boot volume (word)
0x10F: Boot bank ID
0x110: Serial device
0x111: CRT device
0x112: Console device
0x113: Diagnostic level
0x114: Boot mode
...
0x1D8: BID_COM (common bank)
0x1D9: BID_USR (user bank)
0x1DA: BID_BIOS (BIOS bank)
0x1DB: BID_AUX
0x1DC: BID_RAMD0 (first RAM disk bank)
0x1DD: RAM disk bank count
0x1DE: BID_ROMD0 (first ROM disk bank)
0x1DF: ROM disk bank count
```

The boot loader uses SYSPEEK to read values from the HCB.
