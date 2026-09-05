/*
 * Shared Emulator Initialization
 *
 * This module provides initialization functions shared between all platforms
 * (CLI, WebAssembly, iOS, Mac, Windows). These functions handle:
 *
 *   - ROM loading and patching
 *   - HCB (HBIOS Configuration Block) setup
 *   - RAM bank initialization for CP/M 3
 *   - HBIOS ident signature setup
 *   - Disk unit table and drive map population
 *   - MBR validation
 *
 * All platforms should call these functions during startup to ensure
 * consistent behavior. See DOWNSTREAM.md for porting instructions.
 *
 * I/O operations use standard C file I/O (fopen/fread/fwrite/fclose)
 * which is available on all target platforms including WebAssembly.
 */

#ifndef EMU_INIT_H
#define EMU_INIT_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Forward declarations
class banked_mem;
class HBIOSDispatch;
class qkz80;

//=============================================================================
// Disk Size Constants (shared across all platforms)
//=============================================================================

static constexpr size_t HD1K_SINGLE_SIZE = 8388608;      // 8 MB exactly
static constexpr size_t HD1K_PREFIX_SIZE = 1048576;      // 1 MB prefix
static constexpr size_t HD512_SINGLE_SIZE = 8519680;     // 8.32 MB

// Partition types
static constexpr uint8_t PART_TYPE_ROMWBW = 0x2E;  // RomWBW hd1k partition
static constexpr uint8_t PART_TYPE_FAT16 = 0x06;   // FAT16 (incompatible)
static constexpr uint8_t PART_TYPE_FAT32 = 0x0B;   // FAT32 (incompatible)

// HCB field offsets (relative to HCB base at 0x100)
static constexpr uint16_t HCB_APITYPE = 0x12;     // CB_APITYPE
static constexpr uint16_t HCB_DEVCNT = 0x0C;      // CB_DEVCNT (device count)
static constexpr uint16_t HCB_DRVMAP = 0x20;      // CB_DRVMAP (drive map base)
static constexpr uint16_t HCB_DISKUT = 0x60;      // CB_DISKUT (disk unit table base)
static constexpr uint16_t HCB_RAMD_BNKS = 0xDD;   // CB_RAMD_BNKS (RAM disk banks)
static constexpr uint16_t HCB_ROMD_BNKS = 0xDF;   // CB_ROMD_BNKS (ROM disk banks)

// Absolute addresses in memory
static constexpr uint16_t HCB_BASE = 0x100;
static constexpr uint16_t DISKUT_BASE = HCB_BASE + HCB_DISKUT;  // 0x160
static constexpr uint16_t DRVMAP_BASE = HCB_BASE + HCB_DRVMAP;  // 0x120

// Device types for disk unit table
static constexpr uint8_t DIODEV_MD = 0x00;        // Memory disk
static constexpr uint8_t DIODEV_HDSK = 0x09;      // Hard disk
static constexpr uint8_t DIODEV_EMPTY = 0xFF;     // Empty slot

//=============================================================================
// RomWBW Release Identification
//=============================================================================
//
// The RomWBW release in play is a property of the ROM that was loaded, not of
// this binary. Every guest-visible use of it - HBF_SYSVER, the NVRAM checksum
// seed, the HBIOS ident block, the CBIOS page-zero stamp - reads it back out
// of the loaded ROM through the functions below rather than holding a copy.
// That is on purpose: two of those sites exist only in emulated RAM, where no
// verifier that inspects ROM bytes can see them, so a stored copy that was
// never updated would be invisible until a guest printed the wrong version.
//
// See src/romwbw_pin.h for which releases this core is checked against.

// A RomWBW release, in the two packed bytes RomWBW itself uses:
//   ver = major<<4 | minor, upd = update<<4 | patch.
// v3.5.1 is {0x35, 0x10}; v3.6.0 is {0x36, 0x00}.
struct emu_romwbw_release {
  uint8_t ver;
  uint8_t upd;
};

// Enough room for "255.15.15.15" and its terminator.
static constexpr size_t EMU_ROMWBW_STR_MAX = 16;

// Read the release a ROM IMAGE declares, out of its HCB at 0x105/0x106.
// Returns false - leaving *out untouched - when the image is too short or
// carries no 'W' 0xA8 HCB marker, i.e. when there is no version to read.
// Use this to inspect an image before offering it in a picker.
bool emu_romwbw_release_of_image(const uint8_t* rom, size_t size,
                                 emu_romwbw_release* out);

// The same, for the ROM already loaded into bank 0 of `memory`. This is the
// call every guest-visible site uses. It returns false before a ROM is
// loaded, which is a caller bug rather than a condition to paper over: the
// whole init sequence runs after emu_load_rom().
bool emu_romwbw_release_loaded(const banked_mem* memory,
                               emu_romwbw_release* out);

// Format as "3.5.1" (or "3.6.0.2" when the patch nibble is non-zero) into
// buf, which needs EMU_ROMWBW_STR_MAX bytes. Returns buf.
const char* emu_romwbw_release_str(emu_romwbw_release r, char* buf, size_t n);

// Has this core been checked against that release? The list is
// ROMWBW_SUPPORTED_RELEASES in src/romwbw_pin.h.
bool emu_romwbw_release_supported(emu_romwbw_release r);

// The supported releases as "3.5.1, 3.6.0", for messages. Static storage.
const char* emu_romwbw_supported_list();

// Load a release this core has never been checked against anyway.
//
// Off by default, and it stays a deliberate act: the refusal is the only
// thing standing between a user and a ROM whose CBIOS may call an HBIOS
// function this dispatcher does not implement, which fails as a hang or as
// wrong output rather than as an error. A port that exposes this should say
// "untested", not "advanced". emu_validate_rom_hcb() warns loudly and
// proceeds when it is set.
void emu_set_allow_untested_romwbw(bool allow);
bool emu_allow_untested_romwbw();

//=============================================================================
// ROM Loading
//=============================================================================

// Check a ROM's HBIOS configuration block. Returns nullptr when the ROM is
// usable, or a message naming the problem: a missing/corrupt HCB marker, or a
// RomWBW release this core has not been checked against (see
// ROMWBW_SUPPORTED_RELEASES in src/romwbw_pin.h). A stock hardware ROM
// (CB_PLATFORM != 0) only logs a warning and still returns nullptr.
// The returned pointer is to static storage, valid until the next call.
// Both emu_load_rom() and emu_load_rom_from_buffer() call this and fail the
// load on a non-null result, so ports get the check for free.
const char* emu_validate_rom_hcb(const uint8_t* rom, size_t size);

// Load ROM image from file into banked memory
// Returns: true on success, false on failure
// Note: Uses standard fopen/fread which works on all platforms
bool emu_load_rom(banked_mem* memory, const char* path);

// Load ROM image from memory buffer
// data: pointer to ROM data
// size: size in bytes (max 512KB)
// Returns: true on success
bool emu_load_rom_from_buffer(banked_mem* memory, const uint8_t* data, size_t size);

// Load full RomWBW ROM into banks 1-15, preserving bank 0 (emu_hbios)
// This is used when booting with the real romldr boot menu
// path: path to .rom file (512KB)
// Returns: true on success
bool emu_load_romldr_rom(banked_mem* memory, const char* path);

//=============================================================================
// HCB (HBIOS Configuration Block) Setup
//=============================================================================

// Patch APITYPE in ROM's HCB to indicate HBIOS (not UNA)
// This must be called before copying HCB to RAM
// Sets byte at 0x0112 to 0x00 (HBIOS) instead of 0xFF (UNA)
void emu_patch_apitype(banked_mem* memory);

// Copy HCB from ROM bank 0 to RAM bank 0x80
// This copies the first 512 bytes including page zero and HCB
// Call emu_patch_apitype() before this function
void emu_copy_hcb_to_ram(banked_mem* memory);

// Copy HCB from ROM to shadow RAM with shadow bits set
// This is the preferred method after ROM modifications (disk tables, etc.)
// Unlike emu_copy_hcb_to_ram(), this uses store_mem() to set shadow bits,
// ensuring reads from ROM bank 0 will return the shadow RAM content.
// IMPORTANT: Only needed after modifying ROM (disk tables, drive map, etc.)
void emu_copy_hcb_to_shadow_ram(banked_mem* memory);

// Set up HBIOS ident signatures in common RAM area
// Creates signature blocks at 0xFF00 and 0xFE00 in bank 0x8F
// Also sets up pointer at 0xFFFC
// Required for REBOOT and other utilities to recognize HBIOS
void emu_setup_hbios_ident(banked_mem* memory);

// Copy ROM app images from ROM banks to RAM banks
// The romldr expects apps in RAM banks 0x89+ (as configured by CB_BIDAPP0)
// These are copied from ROM banks 2, 3, etc. (after HBIOS and romldr)
void emu_copy_rom_apps_to_ram(banked_mem* memory);

//=============================================================================
// RAM Bank Initialization (for CP/M 3 bank switching)
//=============================================================================

// Initialize a RAM bank on first access
// Copies page zero (RST vectors) and HCB from ROM bank 0
// Patches APITYPE to indicate HBIOS
// bank: the RAM bank ID (0x80-0x8F)
// initialized_bitmap: pointer to uint16_t bitmap tracking which banks are initialized
// Returns: true if bank was initialized, false if already initialized or invalid
bool emu_init_ram_bank(banked_mem* memory, uint8_t bank, uint16_t* initialized_bitmap);

//=============================================================================
// Disk Unit Table and Drive Map
//=============================================================================

// Disk configuration for unit table population
struct emu_disk_config {
  bool is_loaded;              // True if disk is attached
  int max_slices;              // Maximum slices to expose (1-8)
  // Note: More fields can be added as needed
};

// Populate disk unit table in HCB
// This writes device info to 0x160 (HCB+0x60)
// memory: banked memory with loaded ROM
// hbios: HBIOS dispatch with loaded disks (for disk info)
// Returns: number of devices written to table
int emu_populate_disk_unit_table(banked_mem* memory, HBIOSDispatch* hbios);

// Populate drive map in HCB
// This writes drive letter assignments to 0x120 (HCB+0x20)
// memory: banked memory with loaded ROM
// hbios: HBIOS dispatch with loaded disks
// disk_slices: array of max slice counts per disk (nullptr = use defaults)
// Returns: number of drive letters assigned
int emu_populate_drive_map(banked_mem* memory, HBIOSDispatch* hbios,
                           const int* disk_slices);

// Combined function: populate both disk unit table and drive map
// This is the main function to call after loading disks
// Also updates CB_DEVCNT to match number of logical drives
void emu_populate_disk_tables(banked_mem* memory, HBIOSDispatch* hbios,
                              const int* disk_slices);

//=============================================================================
// Disk Image Validation
//=============================================================================

// Check if MBR has valid RomWBW partition
// Returns: warning message string, or nullptr if OK
// Only checks 8MB single-slice images (the problematic size)
const char* emu_check_disk_mbr(const uint8_t* data, size_t size);

// Check disk MBR from file
// path: path to disk image file
// size: file size in bytes
// Returns: warning message or nullptr if OK
const char* emu_check_disk_mbr_file(const char* path, size_t size);

// Validate disk image file - returns error message or nullptr if valid
// Also optionally returns file size via out_size
const char* emu_validate_disk_image(const char* path, size_t* out_size = nullptr);

//=============================================================================
// Complete Initialization Sequence
//=============================================================================

// Perform all ROM initialization in correct order
// This is the main function downstream projects should call
// Steps:
//   1. Patch APITYPE in ROM
//   2. Copy HCB to RAM
//   3. Set up HBIOS ident signatures
//   4. Populate disk tables (if hbios provided)
//   5. Initialize memory disks from HCB
//
// memory: banked memory with loaded ROM
// hbios: HBIOS dispatch (can be nullptr if skipping disk setup)
// disk_slices: per-disk slice counts (can be nullptr for defaults)
void emu_complete_init(banked_mem* memory, HBIOSDispatch* hbios = nullptr,
                       const int* disk_slices = nullptr);

//=============================================================================
// Reset Callback Setup
//=============================================================================

// Set up the SYSRESET callback for ROM reboot command ('R' at boot menu)
// This must be called after emu_complete_init() to enable reboot functionality.
// The callback switches to ROM bank 0 and resets PC to 0x0000.
//
// memory: banked memory
// cpu: Z80 CPU instance
// hbios: HBIOS dispatch to register callback with
void emu_setup_reset_callback(banked_mem* memory, qkz80* cpu, HBIOSDispatch* hbios);

#endif // EMU_INIT_H
