/*
 * Shared Emulator Initialization - Implementation
 *
 * This module provides initialization functions shared between all platforms.
 * Uses standard C file I/O (fopen/fread/fwrite/fclose) which works on all
 * target platforms including WebAssembly with Emscripten.
 */

#include "emu_init.h"
#include "romwbw_mem.h"
#include "hbios_dispatch.h"
#include "emu_io.h"
#include <cstdio>
#include <cstring>

//=============================================================================
// ROM Loading
//=============================================================================

bool emu_load_rom(banked_mem* memory, const char* path) {
  if (!memory || !path) {
    emu_error("[EMU_INIT] Invalid parameters to emu_load_rom\n");
    return false;
  }

  if (!memory->is_banking_enabled()) {
    emu_error("[EMU_INIT] Banking not enabled\n");
    return false;
  }

  FILE* fp = fopen(path, "rb");
  if (!fp) {
    emu_error("[EMU_INIT] Cannot open ROM: %s\n", path);
    return false;
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (size <= 0 || size > (long)banked_mem::ROM_SIZE) {
    emu_error("[EMU_INIT] Invalid ROM size: %ld\n", size);
    fclose(fp);
    return false;
  }

  uint8_t* rom = memory->get_rom();
  if (!rom) {
    emu_error("[EMU_INIT] ROM memory not allocated\n");
    fclose(fp);
    return false;
  }

  size_t bytes_read = fread(rom, 1, size, fp);
  fclose(fp);

  if (bytes_read != (size_t)size) {
    emu_error("[EMU_INIT] ROM read incomplete\n");
    return false;
  }

  emu_log("[EMU_INIT] Loaded %ld bytes ROM from %s\n", size, path);
  return true;
}

bool emu_load_rom_from_buffer(banked_mem* memory, const uint8_t* data, size_t size) {
  if (!memory || !data || size == 0) {
    emu_error("[EMU_INIT] Invalid parameters to emu_load_rom_from_buffer\n");
    return false;
  }

  if (!memory->is_banking_enabled()) {
    memory->enable_banking();
  }

  uint8_t* rom = memory->get_rom();
  if (!rom) {
    emu_error("[EMU_INIT] ROM memory not allocated\n");
    return false;
  }

  // Note: Don't call clear_ram() here - it clears the shadow bitmap which
  // is needed for ROM overlay writes. RAM is already zeroed by enable_banking().

  // Copy ROM data (up to 512KB)
  size_t copy_size = (size > banked_mem::ROM_SIZE) ? banked_mem::ROM_SIZE : size;
  memcpy(rom, data, copy_size);

  emu_log("[EMU_INIT] Loaded %zu bytes ROM from buffer\n", copy_size);
  return true;
}

bool emu_load_romldr_rom(banked_mem* memory, const char* path) {
  if (!memory || !path) {
    emu_error("[EMU_INIT] Invalid parameters to emu_load_romldr_rom\n");
    return false;
  }

  FILE* fp = fopen(path, "rb");
  if (!fp) {
    emu_error("[EMU_INIT] Cannot open romldr ROM: %s\n", path);
    return false;
  }

  fseek(fp, 0, SEEK_END);
  long file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  uint8_t* rom = memory->get_rom();
  if (!rom) {
    fclose(fp);
    return false;
  }

  // Save bank 0 (our emu_hbios) before loading
  uint8_t bank0_save[banked_mem::BANK_SIZE];
  memcpy(bank0_save, rom, banked_mem::BANK_SIZE);

  // Load full ROM
  size_t bytes_read = fread(rom, 1, file_size, fp);
  fclose(fp);

  // Restore bank 0 with our emu_hbios code
  memcpy(rom, bank0_save, banked_mem::BANK_SIZE);

  emu_log("[EMU_INIT] Loaded %zu bytes romldr (banks 1-15 from %s)\n", bytes_read, path);
  emu_log("[EMU_INIT] Bank 0 preserved (emu_hbios)\n");

  return true;
}

//=============================================================================
// HCB Setup
//=============================================================================

void emu_patch_apitype(banked_mem* memory) {
  if (!memory) return;

  uint8_t* rom = memory->get_rom();
  if (!rom) return;

  // Patch APITYPE at HCB_BASE + HCB_APITYPE (0x0112) to 0x00 (HBIOS)
  // instead of 0xFF (UNA). This is required for REBOOT and other
  // utilities to recognize this as an HBIOS system.
  uint16_t apitype_addr = HCB_BASE + HCB_APITYPE;
  rom[apitype_addr] = 0x00;

  emu_log("[EMU_INIT] Patched APITYPE at 0x%04X to HBIOS (0x00)\n", apitype_addr);
}

void emu_copy_hcb_to_ram(banked_mem* memory) {
  if (!memory) return;

  uint8_t* rom = memory->get_rom();
  uint8_t* ram = memory->get_ram();
  if (!rom || !ram) return;

  // Copy first 512 bytes (page zero + HCB) from ROM bank 0 to RAM bank 0x80
  memcpy(ram, rom, 512);

  emu_log("[EMU_INIT] Copied HCB from ROM bank 0 to RAM bank 0x80\n");
}

void emu_copy_hcb_to_shadow_ram(banked_mem* memory) {
  if (!memory) return;

  uint8_t* rom = memory->get_rom();
  if (!rom) return;

  // Copy first 512 bytes from ROM to shadow RAM using store_mem().
  // This sets shadow bits so reads from ROM bank 0 will return shadow RAM content.
  // We select ROM bank 0 mode, then use store_mem() which writes to shadow RAM
  // and sets the shadow bit for each address.
  uint8_t saved_bank = memory->get_current_bank();
  memory->select_bank(0x00);  // ROM bank 0 mode

  for (int i = 0; i < 512; i++) {
    memory->store_mem(i, rom[i]);  // Writes to shadow RAM and sets shadow bit
  }

  memory->select_bank(saved_bank);  // Restore previous bank
  emu_log("[EMU_INIT] Copied HCB to shadow RAM with shadow bits set\n");
}

void emu_setup_hbios_ident(banked_mem* memory) {
  if (!memory) return;

  uint8_t* ram = memory->get_ram();
  if (!ram) return;

  // Common area 0x8000-0xFFFF maps to bank 0x8F (index 15 = 0x0F)
  // Physical offset in RAM = bank_index * 32KB + (addr - 0x8000)
  const uint32_t COMMON_BASE = 0x0F * banked_mem::BANK_SIZE;  // Bank 0x8F = index 15

  // Create ident block at 0xFF00 in common area
  // Format: 'W', ~'W' (0xA8), combined version
  uint32_t ident_phys = COMMON_BASE + (0xFF00 - 0x8000);
  ram[ident_phys + 0] = 'W';       // Signature byte 1
  ram[ident_phys + 1] = ~'W';      // Signature byte 2 (0xA8)
  ram[ident_phys + 2] = 0x35;      // Combined version: (major << 4) | minor = (3 << 4) | 5

  // Also create ident block at 0xFE00 (some REBOOT versions may look there)
  uint32_t ident_phys2 = COMMON_BASE + (0xFE00 - 0x8000);
  ram[ident_phys2 + 0] = 'W';
  ram[ident_phys2 + 1] = ~'W';
  ram[ident_phys2 + 2] = 0x35;

  // Store pointer to ident block at 0xFFFC (little-endian)
  uint32_t ptr_phys = COMMON_BASE + (0xFFFC - 0x8000);
  ram[ptr_phys + 0] = 0x00;        // Low byte of 0xFF00
  ram[ptr_phys + 1] = 0xFF;        // High byte of 0xFF00

  emu_log("[EMU_INIT] Set up HBIOS ident at 0xFE00 and 0xFF00, pointer at 0xFFFC\n");
}

//=============================================================================
// RAM Bank Initialization
//=============================================================================

bool emu_init_ram_bank(banked_mem* memory, uint8_t bank, uint16_t* initialized_bitmap) {
  if (!memory || !initialized_bitmap) return false;

  // Only initialize RAM banks 0x80-0x8F
  if (!(bank & 0x80) || (bank & 0x70)) return false;

  uint8_t bank_idx = bank & 0x0F;
  if (*initialized_bitmap & (1 << bank_idx)) return false;  // Already initialized

  emu_log("[EMU_INIT] Initializing RAM bank 0x%02X with page zero and HCB\n", bank);

  // Copy page zero (0x0000-0x0100) from ROM bank 0 - contains RST vectors
  for (uint16_t addr = 0x0000; addr < 0x0100; addr++) {
    uint8_t byte = memory->read_bank(0x00, addr);
    memory->write_bank(bank, addr, byte);
  }

  // Copy HCB (0x0100-0x0200) from ROM bank 0 - system configuration
  for (uint16_t addr = 0x0100; addr < 0x0200; addr++) {
    uint8_t byte = memory->read_bank(0x00, addr);
    memory->write_bank(bank, addr, byte);
  }

  // Patch APITYPE to HBIOS (0x00) instead of UNA (0xFF)
  memory->write_bank(bank, HCB_BASE + HCB_APITYPE, 0x00);

  *initialized_bitmap |= (1 << bank_idx);
  return true;
}

//=============================================================================
// Disk Unit Table and Drive Map
//=============================================================================

// Note: We don't populate the disk unit table (0x160) or drive map (0x120)
// because real RomWBW CBIOS builds these dynamically using HBIOS API calls
// (SYSGET_DIOCNT, DIODEVICE, DIOCAP), not from pre-populated tables.
// The emu_populate_disk_unit_table, emu_populate_drive_map, and
// emu_populate_disk_tables functions have been removed as they were unused.

//=============================================================================
// Disk Image Validation
//=============================================================================

const char* emu_check_disk_mbr(const uint8_t* data, size_t size) {
  // Only check for 8MB single-slice images - these are the problematic ones
  if (size != HD1K_SINGLE_SIZE || !data) {
    return nullptr;
  }

  // Check for MBR signature
  if (data[510] != 0x55 || data[511] != 0xAA) {
    return nullptr;  // No MBR - probably raw hd1k slice, OK
  }

  // Has MBR signature - check partition types
  bool has_romwbw_partition = false;
  bool has_fat_partition = false;

  for (int p = 0; p < 4; p++) {
    int offset = 0x1BE + (p * 16);
    uint8_t ptype = data[offset + 4];
    if (ptype == PART_TYPE_ROMWBW) {
      has_romwbw_partition = true;
    }
    if (ptype == PART_TYPE_FAT16 || ptype == PART_TYPE_FAT32) {
      has_fat_partition = true;
    }
  }

  if (has_romwbw_partition) {
    return nullptr;  // Has proper RomWBW partition, OK
  }

  if (has_fat_partition) {
    return "WARNING: disk has FAT16/FAT32 MBR but no RomWBW partition - may not work correctly";
  }

  // Has MBR but no RomWBW partition and no FAT - check first bytes
  // A proper hd1k slice starts with Z80 boot code (JR or JP instruction)
  if (data[0] == 0x18 || data[0] == 0xC3) {
    return nullptr;  // Looks like Z80 boot code - probably just has stale MBR signature
  }

  return "WARNING: disk has MBR but no RomWBW partition (0x2E) - format may be invalid";
}

const char* emu_check_disk_mbr_file(const char* path, size_t size) {
  // Only check for 8MB single-slice images
  if (size != HD1K_SINGLE_SIZE) {
    return nullptr;
  }

  FILE* fp = fopen(path, "rb");
  if (!fp) return nullptr;

  uint8_t mbr[512];
  size_t bytes_read = fread(mbr, 1, 512, fp);
  fclose(fp);

  if (bytes_read != 512) return nullptr;

  return emu_check_disk_mbr(mbr, size);
}

const char* emu_validate_disk_image(const char* path, size_t* out_size) {
  FILE* fp = fopen(path, "rb");
  if (!fp) {
    return "file does not exist";
  }

  fseek(fp, 0, SEEK_END);
  size_t size = ftell(fp);
  fclose(fp);

  if (out_size) *out_size = size;

  // Check for valid hd1k sizes
  if (size == HD1K_SINGLE_SIZE) {
    // Check MBR for potential issues with single-slice images
    const char* mbr_warning = emu_check_disk_mbr_file(path, size);
    if (mbr_warning) {
      emu_log("[DISK] %s: %s\n", path, mbr_warning);
    }
    return nullptr;  // Valid size: single-slice hd1k (8MB)
  }

  // Check for combo disk: 1MB prefix + N * 8MB slices
  if (size > HD1K_PREFIX_SIZE && ((size - HD1K_PREFIX_SIZE) % HD1K_SINGLE_SIZE) == 0) {
    return nullptr;  // Valid: combo hd1k with prefix
  }

  // Check for hd512 sizes
  if (size == HD512_SINGLE_SIZE) {
    return nullptr;  // Valid: single-slice hd512 (8.32MB)
  }
  if (size > 0 && (size % HD512_SINGLE_SIZE) == 0) {
    return nullptr;  // Valid: multi-slice hd512
  }

  return "invalid disk size (must be 8MB for hd1k or 8.32MB for hd512)";
}

//=============================================================================
// Complete Initialization Sequence
//=============================================================================

void emu_complete_init(banked_mem* memory, HBIOSDispatch* hbios,
                       const int* /* disk_slices - unused */) {
  if (!memory) {
    emu_error("[EMU_INIT] Memory is null in emu_complete_init\n");
    return;
  }

  emu_log("[EMU_INIT] Starting complete initialization sequence\n");

  // 1. Patch APITYPE in ROM
  emu_patch_apitype(memory);

  // 2. Copy HCB to RAM (simple copy for early access)
  emu_copy_hcb_to_ram(memory);

  // 3. Set up HBIOS ident signatures
  emu_setup_hbios_ident(memory);

  // 4. Initialize memory disks from HCB configuration (if hbios provided)
  if (hbios) {
    hbios->initMemoryDisks();
  }

  // Note: We don't populate disk unit table (0x160) or drive map (0x120) -
  // CBIOS builds these dynamically using HBIOS API calls.

  // 5. Final HCB copy to shadow RAM with shadow bits set
  // This must be done AFTER all ROM modifications
  // so that reads from ROM bank 0 addresses 0x000-0x1FF return the final values.
  emu_copy_hcb_to_shadow_ram(memory);

  emu_log("[EMU_INIT] Complete initialization finished\n");
}
