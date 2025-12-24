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
// ROM Loading
//=============================================================================

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

// Set up HBIOS ident signatures in common RAM area
// Creates signature blocks at 0xFF00 and 0xFE00 in bank 0x8F
// Also sets up pointer at 0xFFFC
// Required for REBOOT and other utilities to recognize HBIOS
void emu_setup_hbios_ident(banked_mem* memory);

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

#endif // EMU_INIT_H
