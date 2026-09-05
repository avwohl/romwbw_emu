/*
 * Shared Emulator Initialization - Implementation
 *
 * This module provides initialization functions shared between all platforms.
 * Uses standard C file I/O (fopen/fread/fwrite/fclose) which works on all
 * target platforms including WebAssembly with Emscripten.
 */

#include "emu_init.h"
#include "romwbw_pin.h"
#include "romwbw_mem.h"
#include "hbios_dispatch.h"
#include "qkz80.h"
#include "emu_io.h"
#include <cstdio>
#include <cstring>

//=============================================================================
// ROM Loading
//=============================================================================

// Offsets of the HBIOS configuration block (HCB) fields inside ROM bank 0.
// The HCB starts at 0x100; see src/emu_hbios.asm, which assembles ours.
static const size_t HCB_MARKER0 = 0x103;  // 'W'
static const size_t HCB_MARKER1 = 0x104;  // ~'W' = 0xA8
static const size_t HCB_VERSION = 0x105;  // major<<4 | minor
static const size_t HCB_UPDATE = 0x106;   // update<<4 | patch
static const size_t HCB_PLATFORM = 0x107; // 0 = EMU, non-zero = real hardware

//=============================================================================
// RomWBW Release Identification
//=============================================================================

// The releases this core has been checked against, expanded from the one list
// in romwbw_pin.h so the table and the message below cannot disagree.
namespace {

struct SupportedRelease {
  uint8_t ver;
  uint8_t upd;
  const char* note;
};

const SupportedRelease kSupportedReleases[] = {
#define EMU_SUPPORTED_ROW(MAJ, MIN, UPD, PAT, NOTE) \
  {(uint8_t)(((MAJ) << 4) | (MIN)), (uint8_t)(((UPD) << 4) | (PAT)), NOTE},
    ROMWBW_SUPPORTED_RELEASES(EMU_SUPPORTED_ROW)
#undef EMU_SUPPORTED_ROW
};

const size_t kSupportedCount = sizeof(kSupportedReleases) / sizeof(kSupportedReleases[0]);

bool g_allow_untested_romwbw = false;

}  // namespace

bool emu_romwbw_release_of_image(const uint8_t* rom, size_t size,
                                 emu_romwbw_release* out) {
  if (!rom || !out || size <= HCB_UPDATE) return false;
  // No marker means there is no HCB where the version would be, and reading
  // two bytes out of the middle of an arbitrary file is how you get a
  // confident wrong answer.
  if (rom[HCB_MARKER0] != 'W' || rom[HCB_MARKER1] != 0xA8) return false;
  out->ver = rom[HCB_VERSION];
  out->upd = rom[HCB_UPDATE];
  return true;
}

bool emu_romwbw_release_loaded(const banked_mem* memory,
                               emu_romwbw_release* out) {
  if (!memory || !out) return false;
  // read_bank() is the ROM accessor that does not care whether the caller
  // holds a non-const banked_mem, and bank 0 is where the HCB lives.
  if (memory->read_bank(0x00, (uint16_t)HCB_MARKER0) != 'W' ||
      memory->read_bank(0x00, (uint16_t)HCB_MARKER1) != 0xA8) {
    return false;
  }
  out->ver = memory->read_bank(0x00, (uint16_t)HCB_VERSION);
  out->upd = memory->read_bank(0x00, (uint16_t)HCB_UPDATE);
  return true;
}

const char* emu_romwbw_release_str(emu_romwbw_release r, char* buf, size_t n) {
  if (!buf || n == 0) return "";
  const int major = r.ver >> 4;
  const int minor = r.ver & 0x0F;
  const int update = r.upd >> 4;
  const int patch = r.upd & 0x0F;
  // RomWBW writes "3.5.1" for patch 0 and only shows a fourth component when
  // there is one, so a version printed here matches the CBIOS banner and the
  // release tag rather than being a fourth spelling of the same thing.
  if (patch == 0) {
    snprintf(buf, n, "%d.%d.%d", major, minor, update);
  } else {
    snprintf(buf, n, "%d.%d.%d.%d", major, minor, update, patch);
  }
  return buf;
}

bool emu_romwbw_release_supported(emu_romwbw_release r) {
  for (size_t i = 0; i < kSupportedCount; i++) {
    if (kSupportedReleases[i].ver == r.ver && kSupportedReleases[i].upd == r.upd) {
      return true;
    }
  }
  return false;
}

const char* emu_romwbw_supported_list() {
  static char list[128];
  static bool built = false;
  if (built) return list;

  list[0] = '\0';
  size_t used = 0;
  for (size_t i = 0; i < kSupportedCount; i++) {
    char one[EMU_ROMWBW_STR_MAX];
    emu_romwbw_release r = {kSupportedReleases[i].ver, kSupportedReleases[i].upd};
    emu_romwbw_release_str(r, one, sizeof(one));
    // Truncating the list would understate what this binary can run, so stop
    // at the last entry that fits whole rather than emitting half a version.
    const char* sep = (used == 0) ? "" : ", ";
    size_t need = strlen(sep) + strlen(one);
    if (used + need + 1 > sizeof(list)) break;
    memcpy(list + used, sep, strlen(sep));
    used += strlen(sep);
    memcpy(list + used, one, strlen(one));
    used += strlen(one);
    list[used] = '\0';
  }
  built = true;
  return list;
}

void emu_set_allow_untested_romwbw(bool allow) { g_allow_untested_romwbw = allow; }

bool emu_allow_untested_romwbw() { return g_allow_untested_romwbw; }

// The release to fall back on when a guest-visible site is asked for a
// version and no ROM is loaded. Reaching this is a bug in the caller - the
// whole init sequence runs after emu_load_rom() - so every use logs first.
// It exists so that such a bug produces this tree's own default rather than
// stamping 00 00 into page zero, which a guest reads as RomWBW v0.0.0.
static emu_romwbw_release emu_romwbw_release_or_default(const banked_mem* memory,
                                                        const char* site) {
  emu_romwbw_release r;
  if (emu_romwbw_release_loaded(memory, &r)) return r;

  char buf[EMU_ROMWBW_STR_MAX];
  emu_romwbw_release fallback = {(uint8_t)ROMWBW_DEFAULT_VER_BYTE,
                                 (uint8_t)ROMWBW_DEFAULT_UPD_BYTE};
  emu_error("[EMU_INIT] %s: no HBIOS configuration block in ROM bank 0 - the "
            "ROM is not loaded yet. Reporting v%s to the guest; load the ROM "
            "before initialisation.\n",
            site, emu_romwbw_release_str(fallback, buf, sizeof(buf)));
  return fallback;
}

const char* emu_validate_rom_hcb(const uint8_t* rom, size_t size) {
  // Large enough for the unsupported-release message, which interpolates the
  // whole supported list plus the release found twice.  -Wformat-truncation
  // catches this at 256.
  static char msg[512];

  if (!rom || size <= HCB_PLATFORM) {
    return "ROM is too small to contain an HBIOS configuration block";
  }

  // A bad marker means there is no HCB where the boot loader expects one.
  // Nothing downstream can recover from that: the guest starts executing and
  // produces no output at all, which is the hardest failure to diagnose.
  if (rom[HCB_MARKER0] != 'W' || rom[HCB_MARKER1] != 0xA8) {
    snprintf(msg, sizeof(msg),
             "no HBIOS configuration block at 0x%03zX (marker is %02X %02X, "
             "expected 57 A8) - the image is not a RomWBW ROM or is corrupt",
             HCB_MARKER0, rom[HCB_MARKER0], rom[HCB_MARKER1]);
    return msg;
  }

  // Which RomWBW release this ROM is, and whether this core has been checked
  // against it. The version itself is no longer a compile-time constant -
  // everything guest-visible reads it back out of this ROM - so what is left
  // to refuse is a release nobody has run. Bank 0 of an emu_*.rom is our
  // HBIOS proxy and the C++ dispatcher behind it implements a specific set of
  // functions; a release whose CBIOS calls something it does not implement
  // would load and then hang or misbehave, which is far harder to diagnose
  // than a refusal at load time.
  emu_romwbw_release release;
  if (!emu_romwbw_release_of_image(rom, size, &release)) {
    // Unreachable: the marker test above already returned. Kept so that a
    // later reordering cannot silently skip the version check.
    return "ROM has no readable HBIOS configuration block";
  }
  if (!emu_romwbw_release_supported(release)) {
    char found[EMU_ROMWBW_STR_MAX];
    emu_romwbw_release_str(release, found, sizeof(found));
    if (!emu_allow_untested_romwbw()) {
      snprintf(msg, sizeof(msg),
               "ROM is built for RomWBW v%s, which this emulator has not been "
               "checked against (it can run %s) - use one of those, or add "
               "v%s to ROMWBW_SUPPORTED_RELEASES in src/romwbw_pin.h once you "
               "have booted it",
               found, emu_romwbw_supported_list(), found);
      return msg;
    }
    emu_error("[EMU_INIT] Warning: loading RomWBW v%s anyway - this emulator "
              "has only been checked against %s. If the guest hangs or prints "
              "nothing, an HBIOS function it calls is probably not "
              "implemented.\n",
              found, emu_romwbw_supported_list());
  }

  // Non-fatal: a stock RomWBW ROM for real hardware has a real HBIOS in bank
  // 0 that drives hardware we do not emulate, instead of the port 0xEF proxy.
  // It may get as far as the boot loader, so warn rather than refuse.
  if (rom[HCB_PLATFORM] != 0) {
    emu_error("[EMU_INIT] Warning: ROM declares platform %d, not 0 (EMU). "
              "This looks like a stock RomWBW ROM for real hardware; the "
              "emulator needs an emu_*.rom with the port 0xEF HBIOS proxy.\n",
              rom[HCB_PLATFORM]);
  }

  return nullptr;
}

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

  const char* bad = emu_validate_rom_hcb(rom, (size_t)size);
  if (bad) {
    emu_error("[EMU_INIT] %s: %s\n", path, bad);
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

  // Same check as the file path: GUI ports load the ROM from a bundle or a
  // download, so they are exactly the callers that can end up with the wrong
  // or a truncated image and no console to notice it on.
  const char* bad = emu_validate_rom_hcb(rom, copy_size);
  if (bad) {
    emu_error("[EMU_INIT] ROM from buffer: %s\n", bad);
    return false;
  }

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

  if (file_size <= 0 || file_size > (long)banked_mem::ROM_SIZE) {
    emu_error("[EMU_INIT] Invalid romldr ROM size: %ld (max %zu bytes)\n",
              file_size, (size_t)banked_mem::ROM_SIZE);
    fclose(fp);
    return false;
  }

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

  // Restore bank 0 with our emu_hbios code (before any error return, so
  // emu_hbios is intact either way)
  memcpy(rom, bank0_save, banked_mem::BANK_SIZE);

  if (bytes_read != (size_t)file_size) {
    emu_error("[EMU_INIT] romldr ROM read incomplete\n");
    return false;
  }

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
    memory->store_mem((uint16_t)i, rom[i]);  // Writes to shadow RAM and sets shadow bit
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

  // The version comes from the ROM that was just loaded, not from a constant.
  // This is one of the two sites that exist only in emulated RAM: no verifier
  // that reads bytes out of a built ROM can see it, so a stale copy here
  // would surface as a guest reporting the wrong version and nothing else.
  emu_romwbw_release release =
      emu_romwbw_release_or_default(memory, "emu_setup_hbios_ident");

  // Create ident block at 0xFF00 in common area
  // Format: 'W', ~'W' (0xA8), combined version
  uint32_t ident_phys = COMMON_BASE + (0xFF00 - 0x8000);
  ram[ident_phys + 0] = 'W';       // Signature byte 1
  ram[ident_phys + 1] = ~'W';      // Signature byte 2 (0xA8)
  ram[ident_phys + 2] = release.ver;  // (major << 4) | minor

  // Also create ident block at 0xFE00 (some REBOOT versions may look there)
  uint32_t ident_phys2 = COMMON_BASE + (0xFE00 - 0x8000);
  ram[ident_phys2 + 0] = 'W';
  ram[ident_phys2 + 1] = ~'W';
  ram[ident_phys2 + 2] = release.ver;

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

  // Install CBIOS page zero stamp at 0x40 (required by ASSIGN, MODE, etc.)
  // Format: 'W', ~'W', version (major<<4|minor), update<<4, CBX pointer
  //
  // The other RAM-only version site. ASSIGN and MODE read these two bytes out
  // of page zero, so a wrong value here is visible to the user as a wrong
  // version in a guest utility and invisible to everything else.
  emu_romwbw_release release =
      emu_romwbw_release_or_default(memory, "emu_init_ram_bank");
  memory->write_bank(bank, 0x40, 'W');            // Marker byte 1
  memory->write_bank(bank, 0x41, ~'W');           // Marker byte 2 (0xA8)
  memory->write_bank(bank, 0x42, release.ver);    // major<<4 | minor
  memory->write_bank(bank, 0x43, release.upd);    // update<<4 | patch
  // CBX pointer at 0x44-0x45: point to our CBX block at 0x50
  memory->write_bank(bank, 0x44, 0x50);       // Low byte
  memory->write_bank(bank, 0x45, 0x00);       // High byte

  // Set up minimal CBX block at 0x50
  // CBX+0: DEVMAP pointer (not used by ASSIGN, set to 0)
  memory->write_bank(bank, 0x50, 0x00);
  memory->write_bank(bank, 0x51, 0x00);
  // CBX+2: DRVMAP pointer - point to HCB drive map at 0x120
  memory->write_bank(bank, 0x52, 0x20);       // Low byte of 0x120
  memory->write_bank(bank, 0x53, 0x01);       // High byte of 0x120
  // CBX+4: DPBMAP pointer (point to dummy area, ASSIGN may not use it)
  memory->write_bank(bank, 0x54, 0x00);
  memory->write_bank(bank, 0x55, 0x00);

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

int emu_populate_disk_unit_table(banked_mem* memory, HBIOSDispatch* hbios) {
  if (!memory || !hbios) return 0;

  // The disk unit table population is now handled by HBIOSDispatch::populateDiskUnitTable()
  // which writes to both ROM (for boot loader) and RAM bank 0x80 (working copy)
  hbios->populateDiskUnitTable();

  // Return a count (estimated based on what we know)
  // The actual count is managed internally by HBIOSDispatch
  return 0;  // HBIOSDispatch handles the actual count
}

int emu_populate_drive_map(banked_mem* memory, HBIOSDispatch* hbios,
                           const int* disk_slices) {
  if (!memory) return 0;

  uint8_t* rom = memory->get_rom();
  if (!rom) return 0;

  // Read memory disk configuration from HCB
  uint8_t ramd_banks = rom[HCB_BASE + HCB_RAMD_BNKS];  // CB_RAMD_BNKS at 0x1DD
  uint8_t romd_banks = rom[HCB_BASE + HCB_ROMD_BNKS];  // CB_ROMD_BNKS at 0x1DF

  int drive_letter = 0;  // 0=A, 1=B, etc.

  // First, mark all drive map entries as unused (0xFF) in both ROM and RAM
  for (int i = 0; i < 16; i++) {
    rom[DRVMAP_BASE + i] = 0xFF;
    memory->write_bank(0x80, (uint16_t)(DRVMAP_BASE + i), 0xFF);
  }

  // IMPORTANT: Hard disks are assigned FIRST so boot disk is A:
  // This matches real RomWBW behavior where the boot device becomes A:

  // Assign hard disk slices (if hbios provided)
  if (hbios) {
    for (int hd = 0; hd < 16 && drive_letter < 16; hd++) {
      if (hbios->isDiskLoaded(hd)) {
        // Unit number: HD0 = unit 2, HD1 = unit 3, etc.
        int unit = hd + 2;

        // Get slice count for this disk (default 4)
        int num_slices = disk_slices ? disk_slices[hd] : 4;
        if (num_slices < 1) num_slices = 1;
        if (num_slices > 8) num_slices = 8;

        // Assign each slice to a drive letter
        for (int slice = 0; slice < num_slices && drive_letter < 16; slice++) {
          uint8_t map_value = (uint8_t)(((slice & 0x0F) << 4) | (unit & 0x0F));
          rom[DRVMAP_BASE + drive_letter] = map_value;
          memory->write_bank(0x80, (uint16_t)(DRVMAP_BASE + drive_letter), map_value);
          emu_log("[EMU_INIT] Drive %c: = HDSK%d:%d (unit=%d, map=0x%02X)\n",
                  'A' + drive_letter, hd, slice, unit, map_value);
          drive_letter++;
        }
      }
    }
  }

  // Assign memory disks AFTER hard disks
  // MD0 (RAM disk) if enabled
  if (ramd_banks > 0 && drive_letter < 16) {
    rom[DRVMAP_BASE + drive_letter] = 0x00;  // Unit 0, slice 0
    memory->write_bank(0x80, (uint16_t)(DRVMAP_BASE + drive_letter), 0x00);
    emu_log("[EMU_INIT] Drive %c: = MD0 (RAM disk)\n", 'A' + drive_letter);
    drive_letter++;
  }

  // MD1 (ROM disk) if enabled
  if (romd_banks > 0 && drive_letter < 16) {
    rom[DRVMAP_BASE + drive_letter] = 0x01;  // Unit 1, slice 0
    memory->write_bank(0x80, (uint16_t)(DRVMAP_BASE + drive_letter), 0x01);
    emu_log("[EMU_INIT] Drive %c: = MD1 (ROM disk)\n", 'A' + drive_letter);
    drive_letter++;
  }

  emu_log("[EMU_INIT] Drive map: assigned %d drive letters\n", drive_letter);

  return drive_letter;
}

void emu_populate_disk_tables(banked_mem* memory, HBIOSDispatch* hbios,
                              const int* disk_slices) {
  if (!memory) return;

  // Populate disk unit table (via HBIOSDispatch)
  if (hbios) {
    emu_populate_disk_unit_table(memory, hbios);
  }

  // Populate drive map
  int drive_count = emu_populate_drive_map(memory, hbios, disk_slices);

  // Update device count in HCB
  uint8_t* rom = memory->get_rom();
  if (rom) {
    rom[HCB_BASE + HCB_DEVCNT] = (uint8_t)drive_count;
    memory->write_bank(0x80, HCB_BASE + HCB_DEVCNT, (uint8_t)drive_count);
    emu_log("[EMU_INIT] Set device count to %d\n", drive_count);
  }
}

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

  // Measure in 64 bits and check it. `size_t size = ftell(fp)` turned a
  // failed measurement (an unseekable path such as a pipe or device opens
  // fine) into SIZE_MAX, which then fell through the size tests below to the
  // generic "invalid disk size" message quoting a 16-exabyte figure.
  emu_off_t end = -1;
  if (emu_fseek(fp, 0, SEEK_END) == 0) end = emu_ftell(fp);
  fclose(fp);

  if (end < 0 || (uint64_t)end > (uint64_t)SIZE_MAX) {
    if (out_size) *out_size = 0;
    return "not a regular file (cannot determine size)";
  }
  size_t size = (size_t)end;

  if (out_size) *out_size = size;

  // Check for valid hd1k sizes
  if (size == HD1K_SINGLE_SIZE) {
    // Check MBR for potential issues with single-slice images
    const char* mbr_warning = emu_check_disk_mbr_file(path, size);
    if (mbr_warning) {
      emu_error("[DISK] %s: %s\n", path, mbr_warning);
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
// ROM App Bank Initialization
//=============================================================================

// Copy OS application images from ROM banks to RAM banks
// The romldr expects apps in RAM banks 0x89+ (as configured by CB_BIDAPP0)
// These are copied from ROM banks 2, 3, etc. (after HBIOS and romldr)
void emu_copy_rom_apps_to_ram(banked_mem* memory) {
  if (!memory) return;

  // Read CB_BIDAPP0 and CB_APP_BNKS from ROM bank 0
  uint8_t app_ram_start = memory->read_bank(0x00, 0x1E0);  // CB_BIDAPP0
  uint8_t app_count = memory->read_bank(0x00, 0x1E1);      // CB_APP_BNKS

  if (app_count == 0) {
    emu_log("[EMU_INIT] No ROM apps configured (CB_APP_BNKS=0)\n");
    return;
  }

  // ROM apps start at bank 2 (after HBIOS bank 0 and romldr bank 1)
  const uint8_t ROM_APP_START_BANK = 2;

  emu_log("[EMU_INIT] Copying %d ROM apps: ROM banks 0x%02X+ -> RAM banks 0x%02X+\n",
          app_count, ROM_APP_START_BANK, app_ram_start);

  for (int i = 0; i < app_count; i++) {
    uint8_t rom_bank = (uint8_t)(ROM_APP_START_BANK + i);
    uint8_t ram_bank = (uint8_t)(app_ram_start + i);

    // Copy entire 32KB bank from ROM to RAM
    for (uint16_t addr = 0; addr < 0x8000; addr++) {
      uint8_t byte = memory->read_bank(rom_bank, addr);
      memory->write_bank(ram_bank, addr, byte);
    }

    // Check for valid app signature at start of bank
    // First few bytes should be Z80 code (JP or JR instruction typically)
    uint8_t first_byte = memory->read_bank(ram_bank, 0);
    emu_log("[EMU_INIT]   App %d: ROM bank 0x%02X -> RAM bank 0x%02X (first byte: 0x%02X)\n",
            i, rom_bank, ram_bank, first_byte);
  }
}

//=============================================================================
// Complete Initialization Sequence
//=============================================================================

void emu_complete_init(banked_mem* memory, HBIOSDispatch* hbios,
                       const int* disk_slices) {
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

  // 4. Copy ROM app images to RAM banks (for romldr boot menu)
  emu_copy_rom_apps_to_ram(memory);

  // 5. Populate disk tables (if hbios provided)
  if (hbios) {
    // Initialize memory disks from HCB configuration
    // Note: initMemoryDisks() calls populateDiskUnitTable() internally
    hbios->initMemoryDisks();

    // Populate drive map and device count only if disk_slices provided
    // (CLI provides this, web/iOS may not need it)
    if (disk_slices) {
      int drive_count = emu_populate_drive_map(memory, hbios, disk_slices);

      // Update device count in HCB
      uint8_t* rom = memory->get_rom();
      if (rom) {
        rom[HCB_BASE + HCB_DEVCNT] = (uint8_t)drive_count;
        memory->write_bank(0x80, HCB_BASE + HCB_DEVCNT, (uint8_t)drive_count);
        emu_log("[EMU_INIT] Set device count to %d\n", drive_count);
      }
    }
  }

  // 6. Final HCB copy to shadow RAM with shadow bits set
  // This must be done AFTER all ROM modifications (disk tables, drive map, etc.)
  // so that reads from ROM bank 0 addresses 0x000-0x1FF return the final values.
  // This is required for the romwbw_mem.h shadow RAM fix (shadow only applies to bank 0).
  emu_copy_hcb_to_shadow_ram(memory);

  emu_log("[EMU_INIT] Complete initialization finished\n");
}

//=============================================================================
// Reset Callback Setup
//=============================================================================

// Static pointers for the reset callback (captured by lambda)
static banked_mem* s_reset_memory = nullptr;
static qkz80* s_reset_cpu = nullptr;
static HBIOSDispatch* s_reset_hbios = nullptr;

void emu_setup_reset_callback(banked_mem* memory, qkz80* cpu, HBIOSDispatch* hbios) {
  if (!memory || !cpu || !hbios) {
    emu_error("[EMU_INIT] Invalid parameters to emu_setup_reset_callback\n");
    return;
  }

  // Store pointers for callback
  s_reset_memory = memory;
  s_reset_cpu = cpu;
  s_reset_hbios = hbios;

  // Register the reset callback with HBIOS
  // Keep this minimal - just switch bank and set PC, same as Android
  hbios->setResetCallback([](uint8_t reset_type) {
    emu_log("[SYSRESET] %s boot - restarting\n",
            reset_type == 0x01 ? "Warm" : "Cold");

    // Flush disk data on warm boot (program ended)
    if (reset_type == 0x01) {
      emu_disk_flush_all();
    }

    // Switch to ROM bank 0
    if (s_reset_memory) {
      s_reset_memory->select_bank(0x00);
    }

    // Set PC to 0 to restart from ROM
    if (s_reset_cpu) {
      s_reset_cpu->regs.PC.set_pair16(0x0000);
    }
  });

  emu_log("[EMU_INIT] Reset callback registered\n");
}
