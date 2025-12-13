# RomWBW Integration with cpmemu

## Executive Summary

This document describes how to integrate the RomWBW system with the cpmemu/altair emulator to bring the full RomWBW ecosystem (multiple operating systems, applications, and disk images) to a browser via WebAssembly.

RomWBW is a sophisticated ROM-based operating system loader that provides a **Hardware BIOS (HBIOS)** abstraction layer. By emulating the minimal hardware interface that HBIOS expects, we can run the complete RomWBW system including:
- CP/M 2.2, ZSDOS, CP/M 3, ZPM3, and other operating systems
- ROM-resident applications (BASIC, Forth, games)
- Full disk image support with multiple slices

---

## Architecture Overview

### Current cpmemu Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    cpmemu / altair_emu                   │
├─────────────────────────────────────────────────────────┤
│  qkz80 CPU Core (8080/Z80)                              │
│    - Register set (AF, BC, DE, HL, SP, PC, IX, IY, I, R)│
│    - IFF1/IFF2 interrupt flip-flops                     │
│    - Interrupt modes (IM0, IM1, IM2)                    │
│    - ~5 cycles/instruction timing                       │
├─────────────────────────────────────────────────────────┤
│  qkz80_cpu_mem (polymorphic)                            │
│    - 64KB flat address space                            │
│    - virtual fetch_mem()/store_mem() for subclassing    │
├─────────────────────────────────────────────────────────┤
│  BDOS/BIOS Trap Layer                                   │
│    - Intercepts calls at magic addresses                │
│    - Routes to C++ handlers for file/console I/O        │
└─────────────────────────────────────────────────────────┘
```

### RomWBW Architecture

```
┌─────────────────────────────────────────────────────────┐
│                Operating Systems (CP/M, ZSDOS, etc.)    │
├─────────────────────────────────────────────────────────┤
│                CBIOS (OS-specific BIOS adapter)         │
├─────────────────────────────────────────────────────────┤
│                HBIOS Proxy (512 bytes at 0xFE00)        │
│                  │                                      │
│                  ▼                                      │
│       Bank Switch to HBIOS bank (0x80)                  │
│                  │                                      │
│                  ▼                                      │
├─────────────────────────────────────────────────────────┤
│                Full HBIOS (32KB in bank 0x80)           │
│    - Serial I/O drivers                                 │
│    - Disk I/O drivers                                   │
│    - Memory management                                  │
│    - Timer/interrupt services                           │
├─────────────────────────────────────────────────────────┤
│                Hardware Abstraction                     │
│    - I/O ports for bank switching                       │
│    - Serial port registers                              │
│    - Disk controller interfaces                         │
└─────────────────────────────────────────────────────────┘
```

### Integrated Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Browser / WebAssembly                 │
├─────────────────────────────────────────────────────────┤
│                    romwbw_emu.cc                        │
│    - Extended qkz80 with bank-switched memory           │
│    - I/O port emulation                                 │
│    - HBIOS call interception (optional optimization)    │
├─────────────────────────────────────────────────────────┤
│  romwbw_mem : public qkz80_cpu_mem                      │
│    - 512KB ROM + 512KB RAM (banked)                     │
│    - Lower 32KB: switchable bank window                 │
│    - Upper 32KB: fixed common area                      │
├─────────────────────────────────────────────────────────┤
│  Virtual I/O Devices                                    │
│    - Serial: Console I/O via xterm.js                   │
│    - Disk: Virtual disk images (ROM disk + CF/SD)       │
│    - Timer: JavaScript setInterval() for tick           │
└─────────────────────────────────────────────────────────┘
```

---

## Memory Banking Implementation

### RomWBW Memory Model

RomWBW uses a simple 32KB banking scheme:

- **Physical Memory**: 512KB ROM + 512KB RAM (typical)
- **CPU Address Space**: 64KB
  - `0x0000-0x7FFF` (32KB): **Banked** - selectable from any ROM/RAM bank
  - `0x8000-0xFFFF` (32KB): **Fixed** - always maps to common RAM (bank 0x8F)

### Bank ID Convention

| Bank ID Range | Type | Purpose |
|---------------|------|---------|
| `0x00-0x0F`   | ROM  | ROM banks (HBIOS, loader, OS images, ROM disk) |
| `0x80-0x8F`   | RAM  | RAM banks (HBIOS runtime, RAM disk, TPA, common) |

### Key RAM Banks

| Bank ID | Purpose |
|---------|---------|
| `0x80`  | HBIOS runtime code (copied from ROM bank 0 at boot) |
| `0x81-0x88` | RAM disk (256KB) |
| `0x89-0x8B` | Application banks |
| `0x8E`  | User TPA (where OS loads) |
| `0x8F`  | Common area (always visible in upper 32KB) |

### Memory Manager I/O Ports

For MM_SBC style banking (simplest to emulate):

```
Port 0x78 (MPCL_RAM): RAM bank selector (write-only)
Port 0x7C (MPCL_ROM): ROM bank selector (write-only)

Bank selection logic:
- If bank ID bit 7 is SET (0x80-0x8F): Select RAM bank
- If bank ID bit 7 is CLEAR (0x00-0x0F): Select ROM bank
```

### Implementation: `romwbw_mem` Class

```cpp
class romwbw_mem : public qkz80_cpu_mem {
    // Physical memory
    uint8_t rom[512 * 1024];      // 512KB ROM (16 x 32KB banks)
    uint8_t ram[512 * 1024];      // 512KB RAM (16 x 32KB banks)

    // Banking state
    uint8_t current_bank;          // Currently selected bank ID

    // Constants
    static const uint16_t BANK_SIZE = 32768;
    static const uint16_t BANK_BOUNDARY = 0x8000;
    static const uint8_t COMMON_BANK = 0x8F;

public:
    romwbw_mem() : current_bank(0x00) {}

    // Bank selection (called when I/O port 0x78 or 0x7C is written)
    void select_bank(uint8_t bank_id) {
        current_bank = bank_id;
    }

    uint8_t get_current_bank() const { return current_bank; }

    // Memory access with banking
    qkz80_uint8 fetch_mem(qkz80_uint16 addr, bool is_instruction = false) override {
        if (addr < BANK_BOUNDARY) {
            // Lower 32KB - banked region
            return fetch_banked(addr);
        } else {
            // Upper 32KB - always common RAM (bank 0x8F)
            uint32_t phys = ((COMMON_BANK & 0x0F) * BANK_SIZE) + (addr - BANK_BOUNDARY);
            return ram[phys];
        }
    }

    void store_mem(qkz80_uint16 addr, qkz80_uint8 byte) override {
        if (addr < BANK_BOUNDARY) {
            // Lower 32KB - banked region
            store_banked(addr, byte);
        } else {
            // Upper 32KB - always common RAM
            uint32_t phys = ((COMMON_BANK & 0x0F) * BANK_SIZE) + (addr - BANK_BOUNDARY);
            ram[phys] = byte;
        }
    }

private:
    uint8_t fetch_banked(uint16_t addr) {
        if (current_bank & 0x80) {
            // RAM bank
            uint32_t phys = ((current_bank & 0x0F) * BANK_SIZE) + addr;
            return ram[phys];
        } else {
            // ROM bank
            uint32_t phys = (current_bank * BANK_SIZE) + addr;
            return rom[phys];
        }
    }

    void store_banked(uint16_t addr, uint8_t byte) {
        if (current_bank & 0x80) {
            // RAM bank - writable
            uint32_t phys = ((current_bank & 0x0F) * BANK_SIZE) + addr;
            ram[phys] = byte;
        }
        // ROM bank - writes are silently ignored
    }

    // Load ROM image
    void load_rom(const uint8_t* data, size_t size) {
        memcpy(rom, data, std::min(size, sizeof(rom)));
    }
};
```

---

## I/O Port Emulation

### Required I/O Ports

| Port | Direction | Purpose |
|------|-----------|---------|
| 0x78 | Write | RAM bank selector |
| 0x7C | Write | ROM bank selector |
| 0x68 | Read/Write | UART data (optional - for HBIOS serial) |
| 0x69 | Read | UART status (optional) |

### I/O Port Handler

The qkz80 CPU needs I/O port callbacks:

```cpp
class romwbw_io {
    romwbw_mem* mem;

public:
    uint8_t port_in(uint8_t port) {
        switch (port) {
            case 0x69:  // UART status
                return 0x01;  // RX ready (if console has input)
            default:
                return 0xFF;
        }
    }

    void port_out(uint8_t port, uint8_t value) {
        switch (port) {
            case 0x78:  // RAM bank select
            case 0x7C:  // ROM bank select
                mem->select_bank(value);
                break;
            case 0x68:  // UART data
                putchar(value);  // Console output
                break;
        }
    }
};
```

---

## HBIOS Integration Strategy

### Option 1: Full Native Execution (Recommended)

Let RomWBW's HBIOS run natively in Z80 code with only hardware emulated:

**Pros:**
- Most authentic behavior
- All RomWBW features work automatically
- No need to reimplement HBIOS functions

**Cons:**
- Need to emulate more hardware (serial ports, disk controller)
- Slightly slower due to bank switching overhead

**Implementation:**
1. Load 512KB ROM image
2. Emulate bank switching I/O ports
3. Emulate serial port for console I/O
4. Provide virtual disk via HBIOS disk interface

### Option 2: HBIOS Call Interception (Hybrid)

Intercept HBIOS calls at the proxy (0xFE00) and handle in C++:

**Pros:**
- Faster execution (no bank switching for BIOS calls)
- Can provide modern implementations

**Cons:**
- Complex - must reimplement all HBIOS functions
- Risk of compatibility issues

**Implementation:**
1. Detect calls to HB_INVOKE (0xFFF0)
2. Read function code from B register
3. Dispatch to C++ handlers

### Recommended Approach

Start with **Option 1** (full native execution) as it requires less code and provides the most authentic behavior. The bank-switched memory is straightforward to implement, and we only need to emulate:

1. Memory banking (I/O ports 0x78, 0x7C)
2. Serial console (one UART)
3. Virtual disk (one or two devices)

---

## Virtual Disk Implementation

### Disk Interface via HBIOS

RomWBW's HBIOS provides disk access through function calls:

```
BF_DIOSEEK ($12):  Seek to sector
BF_DIOREAD ($13):  Read sectors
BF_DIOWRITE ($14): Write sectors
```

### Virtual Disk Strategy

For the emulator, we can:

1. **ROM Disk (MD0)**: Embedded in ROM banks 4-15 (384KB)
   - Read-only, pre-populated with utilities
   - Automatically available at boot

2. **RAM Disk (MD1)**: Uses RAM banks 0x81-0x88 (256KB)
   - Cleared at boot, volatile
   - Already handled by memory banking

3. **Virtual Hard Disk**: Emulated CF/SD card
   - Backed by file or IndexedDB (WebAssembly)
   - Multiple slices, each 8MB

### IDE/CF Card Emulation

For CF card emulation (simplest hard disk interface):

```cpp
class virtual_cf {
    std::vector<uint8_t> data;  // Disk image
    uint32_t current_lba;

public:
    // IDE registers (base + offset)
    enum Registers {
        DATA    = 0,
        ERROR   = 1,
        SECCNT  = 2,
        LBA0    = 3,
        LBA1    = 4,
        LBA2    = 5,
        LBA3    = 6,  // + drive select
        STATUS  = 7,
        COMMAND = 7,
    };

    uint8_t read_reg(int reg);
    void write_reg(int reg, uint8_t value);

    // Commands
    void read_sector();
    void write_sector();
};
```

---

## Interrupt Support

### RomWBW Interrupt Requirements

RomWBW supports IM1 and IM2 interrupts for:
- Timer ticks (50Hz typical)
- Disk activity timeouts
- MP/M scheduling

### Current cpmemu Support

The recent commit `2fce336` added:
- Maskable interrupt (INT) support
- Non-maskable interrupt (NMI) support
- Configurable cycle-based triggering

### Integration

For RomWBW, we need periodic timer interrupts:

```cpp
// Configure 50Hz timer interrupt (RST 7 typical)
maskable_int_config.enabled = true;
maskable_int_config.cycle_min = cpu_cycles_per_second / 50;
maskable_int_config.cycle_max = cpu_cycles_per_second / 50;
maskable_int_config.use_rst = true;
maskable_int_config.rst_num = 7;
```

The existing interrupt infrastructure handles:
- Checking IFF1/IFF2 flags
- Pushing PC to stack
- Jumping to vector address
- Respecting DI/EI state

---

## Boot Sequence

### RomWBW Boot Process

1. CPU starts at ROM address 0x0000 (bank 0x00)
2. HBIOS initializes hardware
3. HBIOS proxy installed at 0xFE00
4. Boot loader displays menu
5. User selects OS (or auto-boot)
6. OS image copied to user bank (0x8E)
7. Bank switched, OS starts

### Emulator Boot Process

```cpp
void romwbw_boot() {
    // 1. Load ROM image
    mem.load_rom(romwbw_rom, sizeof(romwbw_rom));

    // 2. Initialize CPU state
    cpu.regs.PC.set_pair16(0x0000);
    cpu.regs.SP.set_pair16(0xFFFF);

    // 3. Select ROM bank 0
    mem.select_bank(0x00);

    // 4. Start execution
    while (running) {
        cpu.execute();
        check_interrupts();
        handle_io();
    }
}
```

---

## WebAssembly Considerations

### Emscripten Build

```makefile
EMCC_FLAGS = -O2 \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_main","_key_input","_get_screen"]' \
    -s ASYNCIFY=1 \
    --preload-file romwbw.rom \
    --preload-file hd512_combo.img

romwbw.js: romwbw_web.cc
    emcc $(EMCC_FLAGS) -o $@ $<
```

### JavaScript Integration

```javascript
// Terminal setup (xterm.js)
const term = new Terminal();
term.onData(data => {
    for (const ch of data) {
        Module._key_input(ch.charCodeAt(0));
    }
});

// Output callback
Module.printChar = (ch) => {
    term.write(String.fromCharCode(ch));
};

// Disk image handling (IndexedDB for persistence)
async function loadDiskImage(name) {
    const db = await openDatabase();
    return await db.get('disks', name);
}
```

---

## Implementation Status

### ✅ Phase 1: Memory Banking (COMPLETED)
- [x] Design `romwbw_mem` class
- [x] Implement bank switching logic
- [x] Test with simple banking tests
- Banking works: 512KB ROM + 512KB RAM with 32KB window

### ✅ Phase 2: Fake HBIOS Trap System (COMPLETED)
- [x] Create trap handler for PC=0x0008 (RST 08) and PC>=0xFE00
- [x] Implement console I/O functions (IN, OUT, IST, OST)
- [x] Implement system functions (VER, GETBNK, SETBNK)
- [x] Test with simple ROMs - working perfectly!
- [x] Test with TPA-style programs - working!

**How It Works:**
- No hardware I/O port emulation needed
- When PC reaches 0x0008 or 0xFE00+, trap fires
- Function code read from B register
- C++ handler emulates HBIOS function
- Results returned in registers
- RET simulated to return to caller

**Tested:**
- ✅ RST 08 calls
- ✅ Direct CALL to 0xFE00
- ✅ Console output working
- ✅ Programs running in TPA (0x0100)

### Phase 3: Disk Support (IN PROGRESS)
- [ ] Implement HBIOS disk functions (SEEK $12, READ $13, WRITE $14)
- [ ] Map virtual disk images to host files
- [ ] Support LBA addressing

### Phase 4: Testing with Real Applications
- [ ] Test with RomWBW .COM applications
- [ ] Test booting CP/M under fake HBIOS
- [ ] Performance testing

### Phase 5: WebAssembly
- [ ] Create `romwbw_web.cc` wrapper
- [ ] Build with Emscripten
- [ ] Create HTML/JS interface
- [ ] Test in browser

### Phase 6: Enhancements
- [ ] IndexedDB disk persistence
- [ ] Multiple disk images
- [ ] File upload/download
- [ ] Terminal improvements

---

## File Structure

```
cpmemu/
├── src/
│   ├── romwbw_mem.h        # Banked memory class
│   ├── romwbw_mem.cc
│   ├── romwbw_io.h         # I/O port handlers
│   ├── romwbw_io.cc
│   ├── romwbw_disk.h       # Virtual disk implementation
│   ├── romwbw_disk.cc
│   ├── romwbw_emu.cc       # Main RomWBW emulator
│   └── ... (existing files)
├── web/
│   ├── romwbw_web.cc       # WebAssembly wrapper
│   ├── romwbw.html         # Web interface
│   ├── romwbw.js           # JavaScript glue
│   └── Makefile
├── roms/
│   └── romwbw.rom          # RomWBW ROM image (from Binary/)
├── disks/
│   └── hd512_combo.img     # Disk image (from Binary/)
└── docs/
    └── ROMWBW_INTEGRATION.md  # This document
```

---

## References

- RomWBW Repository: `~/esrc/RomWBW`
- HBIOS API: `/home/wohl/esrc/RomWBW/Source/HBIOS/API.txt`
- Memory Layout: `/home/wohl/esrc/RomWBW/Source/HBIOS/layout.inc`
- Bank Layout: `/home/wohl/esrc/RomWBW/Source/HBIOS/hbios.inc`
- Pre-built ROM: `/home/wohl/esrc/RomWBW/Binary/*.rom`
- Pre-built Disks: `/home/wohl/esrc/RomWBW/Binary/*.img`
