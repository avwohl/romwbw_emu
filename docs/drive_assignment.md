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

CBIOS builds the map it uses rather than reading the pre-populated one:
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

### What the Emulator Pre-populates

Both of these, on every run, before the guest starts:

- The disk unit table at `0x160` (`DISKUT_BASE`), written by
  `populateDiskUnitTable()` in `hbios_dispatch.cc` - sixteen four-byte entries
  into ROM and into RAM bank 0x80.
- The drive map at `0x120` (`DRVMAP_BASE`), written by
  `emu_populate_drive_map()` in `emu_init.cc`.

CBIOS then builds its own map at cold boot out of HBIOS calls, which is the map
a running guest uses - so the pre-populated tables are what a guest sees before
`DRV_INIT` runs, not a substitute for it. `--debug` prints the `[DISKUT]` lines
as they are written.

### D Command

`D` at the boot menu works, on both releases, with no extra flags. It calls
PRTSUM at ROM vector `$0406`, and `emu_hbios.asm` routes that bank call to the
emulator deliberately (the `cp 006h` test), where `HBIOSDispatch::handlePRTSUM`
prints the device summary from the same C++ structures. Measured on 3.6.0 it
lists `Disk 0 MD0:`, `Disk 1 MD1:` and one line per attached image.

## ASSIGN Command

Users can reassign drive letters at runtime using the `ASSIGN` command:
- `ASSIGN` - show current assignments
- `ASSIGN D:=HDSK0:2` - point a letter at a unit and slice. It refuses if that
  filesystem already has a letter (`Multiple drive letters reference one
  filesystem, aborting!`), which with one hard disk it usually does, since the
  automatic map already spreads eight slices over `C:`..`J:`
- Assignments take effect immediately without reboot

## Options for Downstream Clients

### RAM Disk Control

RAM disks are always created by RomWBW (from ROM configuration). The emulator
initializes them from HCB_RAMD_BNKS and HCB_ROMD_BNKS settings in the ROM image.

If a client wants to disable RAM/ROM disks, they would need a custom ROM image
with those bank counts set to 0.

### Slice Count and Drive Letters

The CBIOS determines slice count based on disk capacity reported by DIOCAP.
The emulator reports full disk capacity, allowing OS tools to see all slices.

The `setDiskSliceCount()` API only affects how many drive letters are
auto-assigned per disk (following the CBIOS formula above). It does not
limit which slices can be accessed - users can always use ASSIGN or other
OS tools to access any slice on the disk.
