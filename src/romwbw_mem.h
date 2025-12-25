#ifndef ROMWBW_MEM_H
#define ROMWBW_MEM_H

#include "qkz80_mem.h"
#include <cstdint>
#include <cstring>
#include <cstdio>

/*
 * RomWBW Banked Memory
 *
 * Extends qkz80_cpu_mem with bank-switched memory support for RomWBW.
 *
 * Memory model (when banking enabled):
 *   - 512KB ROM (16 x 32KB banks, IDs 0x00-0x0F)
 *   - 512KB RAM (16 x 32KB banks, IDs 0x80-0x8F)
 *   - CPU sees 64KB: lower 32KB banked, upper 32KB fixed to common bank
 *
 * When banking disabled, behaves as flat 64KB memory (compatible with
 * existing altair_emu modes).
 *
 * Bank ID convention:
 *   - Bit 7 = 0: ROM bank (0x00-0x0F)
 *   - Bit 7 = 1: RAM bank (0x80-0x8F)
 *
 * I/O ports (MM_SBC style, directly wired to select_bank):
 *   - Port 0x78: Bank selector (ROM or RAM based on bit 7)
 *   - Port 0x7C: Same (both ports do the same thing in SBC)
 */
class banked_mem : public qkz80_cpu_mem {
public:
    static const size_t ROM_SIZE = 512 * 1024;
    static const size_t RAM_SIZE = 512 * 1024;
    static const size_t BANK_SIZE = 32 * 1024;
    static const uint16_t BANK_BOUNDARY = 0x8000;
    static const uint8_t COMMON_BANK = 0x8F;

private:
    uint8_t* rom;
    uint8_t* ram;
    uint8_t current_bank;
    bool banking_enabled;
    bool debug;

    // Optional write protection
    uint16_t rom_protect_start;

    // BIOS trap range (for CP/M mode)
    uint16_t bios_trap_start;
    uint16_t bios_trap_end;

    // Optional tracing (compatible with altair_emu's cpm_mem)
    uint8_t* code_bitmap;
    uint8_t* data_read_bitmap;
    uint8_t* data_write_bitmap;
    bool tracing_enabled;

public:
    banked_mem() :
        rom(nullptr), ram(nullptr),
        current_bank(0x00), banking_enabled(false), debug(false),
        rom_protect_start(0),
        bios_trap_start(0), bios_trap_end(0),
        code_bitmap(nullptr), data_read_bitmap(nullptr), data_write_bitmap(nullptr),
        tracing_enabled(false)
    {
    }

    ~banked_mem() override {
        delete[] rom;
        delete[] ram;
        delete[] code_bitmap;
        delete[] data_read_bitmap;
        delete[] data_write_bitmap;
    }

    // Enable banked memory mode (allocates ROM/RAM)
    void enable_banking() {
        if (banking_enabled) return;

        rom = new uint8_t[ROM_SIZE];
        ram = new uint8_t[RAM_SIZE];
        memset(rom, 0xFF, ROM_SIZE);  // ROM erased state
        memset(ram, 0x00, RAM_SIZE);
        banking_enabled = true;
        current_bank = 0x00;
    }

    bool is_banking_enabled() const { return banking_enabled; }

    void set_debug(bool enable) { debug = enable; }

    // Clear RAM for clean state when loading a new ROM
    // (following porting notes: reset state when loading a new ROM)
    void clear_ram() {
        if (!banking_enabled || !ram) return;
        memset(ram, 0x00, RAM_SIZE);
        // Also clear shadow bitmap
        memset(shadow_bitmap, 0, SHADOW_BITMAP_SIZE);
    }

    // Bank selection - called from I/O handler
    void select_bank(uint8_t bank_id) {
        if (!banking_enabled) return;
        if (debug && bank_id != current_bank) {
            fprintf(stderr, "[BANK] 0x%02X -> 0x%02X (%s %d)\n",
                    current_bank, bank_id,
                    (bank_id & 0x80) ? "RAM" : "ROM",
                    bank_id & 0x0F);
        }
        current_bank = bank_id;
    }

    uint8_t get_current_bank() const { return current_bank; }

    // ROM protection (for non-banked modes)
    void set_rom_protect(uint16_t start) { rom_protect_start = start; }
    void set_rom_start(uint16_t start) { rom_protect_start = start; }  // Alias for compatibility

    // BIOS trap range (for CP/M mode)
    void set_bios_range(uint16_t start, uint16_t end) {
        bios_trap_start = start;
        bios_trap_end = end;
    }

    bool is_bios_trap(uint16_t pc) const {
        return pc >= bios_trap_start && pc < bios_trap_end;
    }

    // Tracing support (allocates bitmaps on first use)
    void enable_tracing(bool enable) {
        if (enable && !tracing_enabled) {
            code_bitmap = new uint8_t[8192]();
            data_read_bitmap = new uint8_t[8192]();
            data_write_bitmap = new uint8_t[8192]();
        }
        tracing_enabled = enable;
    }

    bool is_tracing() const { return tracing_enabled; }

    // Memory access
    qkz80_uint8 fetch_mem(qkz80_uint16 addr, bool is_instruction = false) override {
        if (tracing_enabled) {
            if (is_instruction) {
                code_bitmap[addr >> 3] |= (1 << (addr & 7));
            } else {
                data_read_bitmap[addr >> 3] |= (1 << (addr & 7));
            }
        }

        if (!banking_enabled) {
            return qkz80_cpu_mem::fetch_mem(addr, is_instruction);
        }

        // Banked mode
        if (addr < BANK_BOUNDARY) {
            return fetch_banked(addr);
        } else {
            // Upper 32KB: common RAM (bank 0x8F)
            uint32_t phys = ((COMMON_BANK & 0x0F) * BANK_SIZE) + (addr - BANK_BOUNDARY);
            uint8_t val = ram[phys];
            return val;
        }
    }

    void store_mem(qkz80_uint16 addr, qkz80_uint8 byte) override {
        if (tracing_enabled) {
            data_write_bitmap[addr >> 3] |= (1 << (addr & 7));
        }

        if (!banking_enabled) {
            // Non-banked: optional ROM protection
            if (rom_protect_start && addr >= rom_protect_start) {
                return;  // Ignore writes to ROM
            }
            qkz80_cpu_mem::store_mem(addr, byte);
            return;
        }

        // Banked mode
        if (addr < BANK_BOUNDARY) {
            store_banked(addr, byte);
        } else {
            // Upper 32KB: common RAM
            // NOTE: No write protection - let ROM code handle everything.
            // The ROM's emu_hbios.asm copies the proxy (including ident bytes)
            // from 0x0500 to 0xFE00-0xFFFF via LDIR at startup.

            uint32_t phys = ((COMMON_BANK & 0x0F) * BANK_SIZE) + (addr - BANK_BOUNDARY);
            ram[phys] = byte;
        }
    }

    // Load ROM image
    bool load_rom_file(const char* filename) {
        if (!banking_enabled) {
            fprintf(stderr, "Error: Banking not enabled\n");
            return false;
        }

        FILE* fp = fopen(filename, "rb");
        if (!fp) {
            fprintf(stderr, "Error: Cannot open ROM: %s\n", filename);
            return false;
        }

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size <= 0 || size > (long)ROM_SIZE) {
            fprintf(stderr, "Error: Invalid ROM size: %ld\n", size);
            fclose(fp);
            return false;
        }

        size_t read = fread(rom, 1, size, fp);
        fclose(fp);

        if (read != (size_t)size) {
            fprintf(stderr, "Error: ROM read incomplete\n");
            return false;
        }

        fprintf(stderr, "[ROMWBW] Loaded %ld bytes from %s\n", size, filename);
        return true;
    }

    // Direct bank access (for disk DMA)
    uint8_t read_bank(uint8_t bank_id, uint16_t offset) const {
        if (!banking_enabled || offset >= BANK_SIZE) return 0xFF;
        if (bank_id & 0x80) {
            return ram[((bank_id & 0x0F) * BANK_SIZE) + offset];
        } else {
            return rom[(bank_id * BANK_SIZE) + offset];
        }
    }

    void write_bank(uint8_t bank_id, uint16_t offset, uint8_t value) {
        if (!banking_enabled || offset >= BANK_SIZE) return;

        if (bank_id & 0x80) {
            // RAM bank - write directly, no protection needed
            // The ROM's emu_hbios.asm handles all HBIOS setup
            uint32_t phys = ((bank_id & 0x0F) * BANK_SIZE) + offset;
            ram[phys] = value;
        }
        // ROM writes ignored
    }

    // Raw access for initialization
    uint8_t* get_rom() { return rom; }
    uint8_t* get_ram() { return ram; }

    // Tracing queries (compatible with altair_emu)
    bool was_executed(uint16_t addr) const {
        return tracing_enabled && (code_bitmap[addr >> 3] & (1 << (addr & 7)));
    }
    bool was_data_read(uint16_t addr) const {
        return tracing_enabled && (data_read_bitmap[addr >> 3] & (1 << (addr & 7)));
    }
    bool was_data_written(uint16_t addr) const {
        return tracing_enabled && (data_write_bitmap[addr >> 3] & (1 << (addr & 7)));
    }

    // Write trace data to file in ud80-compatible script format
    void write_trace_script(const char* filename, uint16_t org_addr) {
        FILE* f = fopen(filename, "w");
        if (!f) {
            fprintf(stderr, "Cannot open trace file: %s\n", filename);
            return;
        }

        fprintf(f, "# Execution trace generated by altair_emu\n");
        fprintf(f, "# Addresses executed as code vs accessed as data only\n");
        fprintf(f, "# Use with: python3 -m um80.ud80 binary.bin $(cat %s)\n\n", filename);

        // Find contiguous ranges of data-only addresses (not executed)
        uint16_t range_start = 0;
        bool in_data_range = false;

        for (uint32_t addr = org_addr; addr < 0x10000; addr++) {
            bool is_code = was_executed(addr);
            bool is_data = (was_data_read(addr) || was_data_written(addr)) && !is_code;

            if (is_data && !in_data_range) {
                range_start = addr;
                in_data_range = true;
            } else if (!is_data && in_data_range) {
                if (addr - 1 > range_start) {
                    fprintf(f, "-d %04X-%04X\n", range_start, addr - 1);
                } else {
                    fprintf(f, "-d %04X-%04X\n", range_start, range_start);
                }
                in_data_range = false;
            }
        }

        if (in_data_range) {
            fprintf(f, "-d %04X-%04X\n", range_start, 0xFFFF);
        }

        fprintf(f, "\n# Entry points (start of executed code regions)\n");
        bool was_code = false;
        for (uint32_t addr = org_addr; addr < 0x10000; addr++) {
            bool is_code = was_executed(addr);
            if (is_code && !was_code) {
                fprintf(f, "-e %04X\n", addr);
            }
            was_code = is_code;
        }

        fclose(f);
        fprintf(stderr, "Trace written to %s\n", filename);
    }

private:
    // Shadow RAM: tracks which addresses have been written to when in ROM mode
    // When ROM is selected and we write to lower 32KB, it goes to RAM bank 0x80
    // When reading, if the address was written to, read from RAM instead of ROM
    static const size_t SHADOW_BITMAP_SIZE = BANK_SIZE / 8;
    uint8_t shadow_bitmap[SHADOW_BITMAP_SIZE] = {0};  // 4KB bitmap for 32KB

    void set_shadow_bit(uint16_t addr) {
        shadow_bitmap[addr >> 3] |= (1 << (addr & 7));
    }

    bool get_shadow_bit(uint16_t addr) const {
        return shadow_bitmap[addr >> 3] & (1 << (addr & 7));
    }

    uint8_t fetch_banked(uint16_t addr) const {
        if (current_bank & 0x80) {
            // RAM bank selected - read from RAM
            uint32_t phys = ((current_bank & 0x0F) * BANK_SIZE) + addr;
            return ram[phys];
        } else {
            // ROM bank selected
            // Shadow RAM only applies to bank 0 - other ROM banks read directly
            // This is required for bank switching to work correctly
            if (current_bank == 0x00 && get_shadow_bit(addr)) {
                uint32_t phys = (0 * BANK_SIZE) + addr;  // Bank 0x80 (shadow RAM)
                return ram[phys];
            }
            // Read from ROM - mask to 4 bits since we only have 16 ROM banks
            uint32_t phys = ((current_bank & 0x0F) * BANK_SIZE) + addr;
            return rom[phys];
        }
    }

private:

    void store_banked(uint16_t addr, uint8_t byte) {
        if (current_bank & 0x80) {
            // Current bank is RAM - write directly
            uint32_t phys = ((current_bank & 0x0F) * BANK_SIZE) + addr;
            ram[phys] = byte;
        } else {
            // Current bank is ROM - write to shadow RAM (bank 0x80)
            // This is how real MM_SBC hardware works: when executing from ROM,
            // writes go to the corresponding RAM bank (shadow RAM / ROM overlay)
            uint32_t phys = (0 * BANK_SIZE) + addr;  // Bank 0x80 index 0
            ram[phys] = byte;
            set_shadow_bit(addr);  // Mark this address as shadowed
        }
    }

};

#endif // ROMWBW_MEM_H
