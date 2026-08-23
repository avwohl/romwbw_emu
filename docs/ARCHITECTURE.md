# RomWBW Emulator Architecture

This document describes the architecture of the RomWBW emulator, focusing on the shared HBIOS implementation that enables cross-platform support.

## Design Goals

1. **Single HBIOS Implementation** - One codebase for all platforms
2. **Platform Abstraction** - Clean separation between emulation and I/O
3. **Easy Porting** - Minimal code needed for new platforms
4. **Feature Parity** - Same capabilities across CLI, Web, iOS, macOS

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                      Application Layer                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │   CLI    │  │   Web    │  │   iOS    │  │  macOS   │            │
│  │  main()  │  │  main()  │  │ ViewController │ AppDelegate │       │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘            │
│       │             │             │             │                   │
│  ┌────┴─────┐  ┌────┴─────┐  ┌────┴─────┐  ┌────┴─────┐            │
│  │emu_io   │  │emu_io   │  │emu_io   │  │emu_io   │            │
│  │_cli.cc  │  │_wasm.cc │  │_ios.mm  │  │_macos.mm│            │
│  └────┬─────┘  └────┴─────┘  └────┬─────┘  └────┬─────┘            │
└───────┼─────────────┼─────────────┼─────────────┼───────────────────┘
        │             │             │             │
        └─────────────┴──────┬──────┴─────────────┘
                             │
┌────────────────────────────┼────────────────────────────────────────┐
│                    Shared Emulation Layer                            │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │              emu_io.h (Interface)                  │              │
│  │  Console, Disk, File, Time, Video, DSKY,          │              │
│  │  Aux/Printer, Host File Transfer, Logging         │              │
│  └─────────────────────────┬─────────────────────────┘              │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │       emu_io_common.cc (Shared Portable I/O)       │              │
│  │  • File I/O, disk I/O, time, random               │              │
│  │  • Platform-independent implementations           │              │
│  └─────────────────────────┬─────────────────────────┘              │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │       emu_init.cc / emu_init.h (Initialization)    │              │
│  │  • ROM loading (emu_load_rom)                     │              │
│  │  • HCB setup, disk tables                        │              │
│  └─────────────────────────┬─────────────────────────┘              │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │       hbios_cpu.cc / hbios_cpu.h (CPU Glue)        │              │
│  │  • I/O port emulation                             │              │
│  │  • HBIOS trap detection                           │              │
│  └─────────────────────────┬─────────────────────────┘              │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │           hbios_dispatch.cc (Shared HBIOS)         │              │
│  │                                                    │              │
│  │  Character I/O (CIO)    │  Disk I/O (DIO)         │              │
│  │  • Console input/output │  • Read/write sectors   │              │
│  │  • Status checking      │  • Seek, capacity       │              │
│  │                         │  • Up to 16 units       │              │
│  │  ─────────────────────────────────────────────    │              │
│  │  RTC/NVRAM              │  Video (VDA)            │              │
│  │  • Get/set time         │  • Cursor positioning   │              │
│  │  • 64-byte NVRAM        │  • Clear, scroll        │              │
│  │  • Persistence callback │  • Character attributes │              │
│  │  ─────────────────────────────────────────────    │              │
│  │  System (SYS)           │  Sound (SND)            │              │
│  │  • Reset (warm/cold)    │  • Beep, notes          │              │
│  │  • Bank switching       │  • Volume control       │              │
│  │  • Memory peek/poke     │                         │              │
│  └─────────────────────────┬─────────────────────────┘              │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │              romwbw_mem.h (Memory)                 │              │
│  │  • 512KB ROM (16 x 32KB banks)                    │              │
│  │  • 512KB RAM (16 x 32KB banks)                    │              │
│  │  • Bank switching via I/O ports                   │              │
│  └─────────────────────────┬─────────────────────────┘              │
│                            │                                         │
│  ┌─────────────────────────┴─────────────────────────┐              │
│  │                qkz80 (CPU Core)                    │              │
│  │  • Full Z80 instruction set                       │              │
│  │  • Undocumented opcodes                           │              │
│  │  • 8080 compatibility mode                        │              │
│  └───────────────────────────────────────────────────┘              │
└─────────────────────────────────────────────────────────────────────┘
```

## Component Details

### qkz80 (CPU Core)

The Z80 CPU emulator from the cpmemu project. Provides:

- All documented Z80 instructions
- Undocumented instructions (IX/IY bit operations, etc.)
- DD/FD prefix chaining (last prefix wins)
- 8080 compatibility mode
- Register access via `cpu.regs.*`

```cpp
#include "qkz80.h"

qkz80 cpu(&memory);
cpu.set_cpu_mode(qkz80::MODE_Z80);
cpu.regs.PC.set_pair16(0x0000);
cpu.execute();  // Execute one instruction
```

### romwbw_mem.h (Memory System)

Bank-switched memory emulating RomWBW hardware:

- **ROM:** 512KB (banks 0x00-0x0F), read-only
- **RAM:** 512KB (banks 0x80-0x8F), read-write
- **Bank Selection:** Port 0x78 (RAM) / 0x7C (ROM)
- **Shadow:** RAM 0x8000-0xFFFF shadows ROM when RAM bank selected

```cpp
#include "romwbw_mem.h"

banked_mem memory;
memory.enable_banking();
memory.select_bank(0x80);  // Select RAM bank 0
uint8_t byte = memory.fetch_mem(0x1234);
memory.store_mem(0x1234, 0xFF);
```

### emu_io.h (I/O Abstraction)

Platform-independent interface for all emulator I/O. Functions are grouped by subsystem:

```cpp
// Platform utilities (strcasecmp, strncasecmp, sleep_ms)
// Init/cleanup (emu_io_init, emu_io_cleanup)
// Console I/O (has_input, read_char, queue_char, clear_queue,
//              write_char, check_escape)
// Aux devices (printer_set_file, printer_out, printer_ready,
//              aux_set_input_file, aux_set_output_file, aux_in, aux_out)
// Debug/logging (set_debug, log, error, fatal, status, disk_flush_all)
// File I/O (file_load, file_load_to_mem, file_save, file_exists, file_size)
// Disk I/O (disk_open, disk_close, disk_read, disk_write, disk_flush,
//           disk_size)  -- uses emu_disk_handle = void*
// Time (emu_get_time)  -- uses emu_time struct
// Random (emu_random)
// Video/VDA (video_get_caps, video_clear, video_set_cursor,
//            video_get_cursor, video_write_char, video_scroll_up, ...)
// DSKY (dsky_show_hex, dsky_show_segments, dsky_set_leds,
//       dsky_beep, dsky_get_key)
// Host file transfer (host_file_get_state, host_file_open_read,
//                     host_file_open_write, host_file_read_byte,
//                     host_file_write_byte, host_file_close_read, ...)
```

Each platform implements the platform-specific functions in `emu_io_*.cc`:
- `emu_io_cli.cc` - Terminal I/O with termios
- `emu_io_wasm.cc` - JavaScript callbacks via Emscripten

Portable implementations shared across all platforms live in `emu_io_common.cc` (file I/O, disk I/O, time, random).

### hbios_dispatch.cc (Shared HBIOS)

The core HBIOS implementation. Handles all RomWBW system calls:

```cpp
#include "hbios_dispatch.h"

HBIOSDispatch hbios;
hbios.setCPU(&cpu);
hbios.setMemory(&memory);

// Check for HBIOS trap at current PC
if (hbios.checkTrap(cpu.regs.PC.get_pair16())) {
    int trap_type = hbios.getTrapType(pc);
    hbios.handleCall(trap_type);
}

// Callbacks for platform-specific behavior
hbios.setResetCallback([](uint8_t type) {
    // Handle warm/cold boot
});
hbios.setNvramSaveCallback([](const uint8_t* data, int size) {
    // Persist NVRAM to storage
});
```

#### HBIOS Function Groups

| Group | Functions | Description |
|-------|-----------|-------------|
| CIO (0x00-0x06) | CIOIN, CIOOUT, CIOIST, CIOOST | Character I/O |
| DIO (0x10-0x1B) | DIOREAD, DIOWRITE, DIOSEEK, etc. | Disk I/O |
| RTC (0x20-0x28) | RTCGETTIM, RTCGETBYT, RTCSETBLK | Time and NVRAM |
| VDA (0x40-0x4F) | VDAINI, VDACLS, VDASCP, etc. | Video display |
| SND (0x50-0x56) | SNDRESET, SNDBEEP, SNDNOTE | Sound |
| SYS (0xF0-0xFF) | SYSRESET, SYSGET, SYSSET, etc. | System control |

## NVRAM Implementation

NVRAM provides 64 bytes of persistent storage for system configuration:

```cpp
// In HBIOSDispatch class
uint8_t nvram[64];
NvramCallback nvram_save_callback;

// RTC functions access NVRAM
case HBF_RTCGETBYT:  // Get byte by index
case HBF_RTCSETBYT:  // Set byte by index
case HBF_RTCGETBLK:  // Get entire block
case HBF_RTCSETBLK:  // Set entire block
```

Platform persistence:
- **Web:** in WASM memory only - not persisted; a page reload discards it (the page persists only UI control selections in localStorage)
- **CLI:** Plain text file `$XDG_CONFIG_HOME/romwbw_emu/nvram` (default `~/.config/romwbw_emu/nvram`); contains a single line such as "C" or "2.3". `XDG_CONFIG_HOME` support is new in v1.34.
- **iOS:** UserDefaults

## Disk I/O Architecture

The emulator supports multiple disk formats:

```cpp
struct HBDisk {
    bool is_open = false;
    std::string path;
    std::vector<uint8_t> data;       // For in-memory disks
    void* handle = nullptr;          // For file-backed disks (emu_disk_handle)
    bool file_backed = false;
    size_t size = 0;
    uint32_t current_lba = 0;        // Current LBA position (set by DIOSEEK)
    int max_slices = 8;              // Slices for drive letter assignment

    // Partition/slice info (detected from MBR on first EXTSLICE call)
    bool partition_probed = false;
    uint32_t partition_base_lba = 0; // 2048 for hd1k, 0 for hd512
    uint32_t slice_size = 16640;     // Sectors per slice
    bool is_hd1k = false;            // True for hd1k format

    // Manifest disk flag (app-managed, may be auto-updated)
    bool is_manifest = false;
    bool warning_suppressed = false;
    bool dirty = false;
};

// Up to 16 disk units
HBDisk disks[16];
```

Disk operations:
1. **DIOSEEK** - Set LBA position
2. **DIOREAD** - Read sectors to memory
3. **DIOWRITE** - Write sectors from memory
4. **DIOCAP** - Report capacity

## Adding a New Platform

### Step 1: Implement emu_io.h

Create `emu_io_yourplatform.cc` implementing all platform-specific functions
declared in `emu_io.h`. See `emu_io_cli.cc` and `emu_io_wasm.cc` for
reference implementations. Key groups:

```cpp
#include "emu_io.h"

// Platform utilities
int emu_strcasecmp(const char* s1, const char* s2) { /* ... */ }
int emu_strncasecmp(const char* s1, const char* s2, size_t n) { /* ... */ }
void emu_sleep_ms(int ms) { /* ... */ }
void emu_set_debug(bool enable) { /* ... */ }

// Init/cleanup
void emu_io_init() { /* Initialize your I/O system */ }
void emu_io_cleanup() { /* Restore terminal state, etc. */ }

// Console I/O (has_input, read_char, queue_char, clear_queue,
//              write_char, check_escape)
bool emu_console_has_input() { /* ... */ }
int emu_console_read_char() { /* ... */ }
void emu_console_write_char(uint8_t ch) { /* ... */ }
// ... plus queue_char, clear_queue, check_escape

// Aux devices (printer_set_file, printer_out, printer_ready,
//              aux_set_input_file, aux_set_output_file, aux_in, aux_out)

// Debug/logging (log, error, fatal, status, disk_flush_all)
void emu_log(const char* fmt, ...) { /* ... */ }
void emu_error(const char* fmt, ...) { /* ... */ }
void emu_fatal(const char* fmt, ...) { /* ... abort(); */ }
void emu_status(const char* fmt, ...) { /* ... */ }

// File I/O (file_load, file_load_to_mem, file_save, file_exists, file_size)
bool emu_file_exists(const std::string& path) { /* ... */ }
size_t emu_file_size(const std::string& path) { /* ... */ }
// ... plus file_load, file_load_to_mem, file_save

// Video/VDA, DSKY, Host file transfer - see emu_io.h for full list
```

### Step 2: Create Main Program

```cpp
#include "qkz80.h"
#include "romwbw_mem.h"
#include "hbios_dispatch.h"
#include "emu_init.h"
#include "emu_io.h"

banked_mem memory;
qkz80 cpu(&memory);
HBIOSDispatch hbios;

int main() {
    emu_io_init();

    // Load ROM using shared init helper
    memory.enable_banking();
    emu_load_rom("path/to/rom.rom", memory);

    // Set up HBIOS (emu_init handles HCB setup, disk tables, etc.)
    hbios.setCPU(&cpu);
    hbios.setMemory(&memory);
    hbios.setResetCallback(handle_reset);
    hbios.setNvramSaveCallback(handle_nvram_save);

    // Load persisted NVRAM
    // ... load from storage and call hbios.loadNvram() ...

    // Main loop
    cpu.set_cpu_mode(qkz80::MODE_Z80);
    cpu.regs.PC.set_pair16(0x0000);

    while (running) {
        uint16_t pc = cpu.regs.PC.get_pair16();

        if (hbios.checkTrap(pc)) {
            hbios.handleCall(hbios.getTrapType(pc));
            continue;
        }

        cpu.execute();
    }
}
```

### Step 3: Build

Compile and link with:
- `emu_io_common.cc` (shared portable I/O: file, disk, time)
- `emu_init.cc` (shared initialization: ROM loading, HCB setup, disk tables)
- `hbios_dispatch.cc` (shared HBIOS implementation)
- `hbios_cpu.cc` (I/O port emulation, HBIOS trap detection)
- `qkz80` library (Z80 CPU core)
- Your `emu_io_yourplatform.cc` (platform-specific I/O)

## Memory Map

```
Address Range    Bank Type    Description
─────────────────────────────────────────
0x0000-0x7FFF    ROM/RAM      Lower 32KB (bank-switched)
0x8000-0xFFFF    RAM          Upper 32KB (common area, always RAM)

Bank Register (Port 0x78/0x7C):
  Bit 7: 0=ROM, 1=RAM
  Bits 3-0: Bank number (0-15)

Examples:
  0x00 = ROM bank 0 (boot ROM)
  0x80 = RAM bank 0
  0x8F = RAM bank 15 (common area)
```

## HBIOS Entry Point

HBIOS calls are made via RST 08 which jumps to 0xFFF0:

```
Address  Function
0xFFF0   Main HBIOS entry
0xFFF3   Interrupt vector
0xFFF6   NMI vector
```

The emulator traps execution at 0xFFF0 and dispatches to the appropriate handler based on the B register (function code).

## Thread Safety

The current implementation is **not thread-safe**. For platforms requiring threading:

1. Wrap CPU execution in a mutex
2. Queue input characters from I/O thread
3. Use atomic flags for running state

## Performance Considerations

- **Instruction batching:** Execute multiple instructions per UI update
- **I/O polling:** Only check console status periodically
- **Memory access:** Direct array access, no virtual methods
- **Disk caching:** Keep small disks in memory, large ones file-backed

## ROM Compatibility

### Why Standard ROMs Don't Work

Standard RomWBW ROMs (`*_std.rom`) contain real HBIOS code that attempts to access hardware I/O ports. This emulator does not emulate hardware - instead, it intercepts HBIOS calls and handles them in C++.

When a standard ROM runs:
1. It immediately tries to access hardware ports (e.g., port 0xC0 for RTC latch)
2. The emulator returns 0x00 for unknown port reads
3. HBIOS trapping is never enabled (no signal to port 0xEE)
4. The Z80 HBIOS code runs and hangs waiting for hardware responses

### EMU ROMs

The `emu_*` ROMs have the first 32KB replaced with `emu_hbios` code that:
1. Skips all hardware initialization
2. Signals the emulator via port 0xEE to enable HBIOS interception
3. Routes all HBIOS calls through dispatch addresses the emulator traps

The rest of the ROM (banks 1-15) is preserved from the original, keeping OS images and ROM disk intact.

### Building EMU ROMs

Use `roms/build_emu_rom.sh` to create an EMU ROM from any standard RomWBW ROM:

```bash
cd roms
./build_emu_rom.sh SBC_simh_std.rom emu_romwbw.rom
./build_emu_rom.sh RCZ80_std.rom emu_rcz80.rom
```

### ROM Types

An emulator ROM is one whose HCB declares `CB_PLATFORM = 0` (EMU); that is
what `roms/verify_romwbw_pin.sh` keys off, and what the emulator now checks
at load time.

| ROM | Description | Works? |
|-----|-------------|--------|
| `emu_avw.rom` | SBC/SIMH with emu_hbios overlay (the default) | Yes |
| `emu_romwbw.rom` | Byte-identical to `emu_avw.rom` | Yes |
| `emu_rcz80.rom` | RCZ80 with emu_hbios overlay | Yes |
| `SBC_simh_std.rom` | Standard SBC/SIMH ROM | No - build input only |
| `RCZ80_std.rom` | Standard RCZ80 ROM | No - build input only |
| `SBC_emu.rom` | Standard SBC ROM despite the name (`CB_PLATFORM=1`) | No - build input only |

The three "build input only" ROMs are stock RomWBW images: they contain a
real HBIOS that drives hardware this emulator does not provide. They are
kept because `roms/build_emu_rom.sh` overlays our bank 0 onto them. Passing
one to `--romwbw` now prints a warning rather than failing silently.

## Version Compatibility

The shared HBIOS emulates **one** RomWBW release, not RomWBW 3.x in general.
That release is pinned in [`../src/romwbw_pin.h`](../src/romwbw_pin.h)
(currently v3.5.1) and everything version-dependent derives from it: the
version `HBF_SYSVER` reports, the NVRAM checksum seed, the HCB stamped into
`emu_hbios.asm`, and the ROM `roms/build_from_source.sh` overlays.

A guest's CBIOS compares its own build against the version this core
reports, so a ROM or a boot slice from a different release prints
`*** WARNING: HBIOS/CBIOS Version Mismatch ***` - or never reaches the boot
loader at all. `roms/verify_romwbw_pin.sh` checks a whole tree against the
pin; see the "RomWBW version pin" section of `../DOWNSTREAM.md` for what
re-pinning would involve.

Key compatibility points:

- HBIOS function codes match RomWBW hbios.inc
- Bank switching matches SBC hardware
- NVRAM size matches DS1302/DS1307 chips
- Disk geometry matches hd1k format (512-byte sectors)
