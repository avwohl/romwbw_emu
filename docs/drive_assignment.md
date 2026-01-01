# RomWBW Drive Letter Assignment

## How Real RomWBW Works

### Architecture Overview

RomWBW has three layers involved in drive assignment:

1. **HBIOS** - Hardware abstraction layer
   - Maintains disk unit table (what devices exist)
   - Provides device enumeration via SYSGET_DIOCNT, DIODEVICE, DIOMEDIA, DIOCAP
   - Stores boot volume in HCB (CB_BOOTVOL at offset 0x0D)

2. **Boot Loader** (romldr.asm)
   - User selects boot device (e.g., "2.1" for unit 2, slice 1)
   - Calls SYSSET_BOOTINFO to store boot volume in HCB
   - Loads and jumps to OS image

3. **CBIOS** (cbios.asm)
   - **Builds drive map dynamically at cold boot** (DRV_INIT routine)
   - Queries HBIOS for device list via SYSGET/DIODEVICE calls
   - Reads boot volume from HCB (CB_BOOTVOL)
   - Allocates drive letters based on boot source and device enumeration

### Key Insight: CBIOS Builds Drive Map

The CBIOS does NOT use a pre-populated drive map from HBIOS. Instead:
1. It reads CB_BOOTVOL from HCB to know which slice was booted
2. It enumerates all disk units via HBIOS calls
3. It builds drive map on heap at runtime (DRVMAPADR)

### Slice Allocation Algorithm

The CBIOS counts hard disk units and allocates slices:

| # of Hard Disks | Slices per Disk |
|-----------------|-----------------|
| 1 disk          | 8 slices        |
| 2 disks         | 4 slices each   |
| 3+ disks        | 2 slices each   |

Formula: `slices_per_disk = max(2, 8 / num_hard_disks)`

### Boot Source Affects A: Assignment

**ROM boot** (default):
- A: = RAM disk (MD0:0)
- B: = ROM disk (MD1:0)
- C: onwards = floppies, then hard disk slices

**Disk boot** (e.g., boot to slice 2.1):
- A: = boot slice (HDSK0:1 in this case)
- B: = RAM disk (MD0:0)
- C: = ROM disk (MD1:0)
- D: onwards = floppies, then remaining hard disk slices

The boot slice is identified by CB_BOOTVOL in HCB, which the boot loader
sets via SYSSET_BOOTINFO before loading the OS.

## Emulator Implementation

### What the Emulator Provides

**HBIOS API Calls** that CBIOS uses to enumerate devices:
- `SYSGET_DIOCNT` - returns count of disk units
- `DIODEVICE` - returns device type/attributes for each unit
- `DIOMEDIA` - returns media status
- `DIOCAP` - returns capacity (determines slice count)
- `SYSSET_BOOTINFO` - stores boot volume in HCB CB_BOOTVOL

These are implemented in `hbios_dispatch.cc` and read from internal
C++ structures (`md_disks[]` for memory disks, `disks[]` for hard disks).

### What the Emulator Does NOT Do

- Pre-populate disk unit table at 0x160 - no real HBIOS has this
- Pre-populate drive map at 0x120 - CBIOS ignores it, builds its own

CBIOS builds all tables dynamically using HBIOS API calls at boot time.

### D Command

The 'D' command in the boot loader calls PRTSUM (at ROM vector $0406),
which uses the same HBIOS API calls above. Since our emu_hbios.bin
doesn't include PRTSUM, the D command doesn't work. To use D, boot
with `--romldr` option which preserves the full RomWBW boot menu.

## ASSIGN Command

Users can reassign drive letters at runtime using the `ASSIGN` command:
- `ASSIGN` - show current assignments
- `ASSIGN D:=HDSK0:2` - assign specific drive
- Assignments take effect immediately without reboot

## Options for Downstream Clients

### RAM Disk Control

RAM disks are always created by RomWBW (from ROM configuration). The emulator
initializes them from HCB_RAMD_BNKS and HCB_ROMD_BNKS settings in the ROM image.

If a client wants to disable RAM/ROM disks, they would need a custom ROM image
with those bank counts set to 0.

### Slice Count Control

The CBIOS determines slice count based on disk capacity reported by DIOCAP.
The emulator can limit apparent capacity via the `--max-slices` option or
`setDiskSliceCount()` API to control how many slices appear for each disk.
