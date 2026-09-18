/*
 * HBIOS Dispatch - Shared RomWBW HBIOS Handler Implementation
 *
 * Uses emu_io.h for all platform-independent I/O operations.
 */

#include "hbios_dispatch.h"
#include "emu_init.h"
#include "emu_io.h"
#include "qkz80.h"
#include "qkz80_cpu_flags.h"
#include "romwbw_mem.h"
#include <chrono>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdarg>

// Static member definition - survives object recreation within session
bool HBIOSDispatch::manifest_warning_shown = false;

//=============================================================================
// Constructor/Destructor
//=============================================================================

HBIOSDispatch::HBIOSDispatch() {
  reset();
}

HBIOSDispatch::~HBIOSDispatch() {
  // Close any open disk handles
  for (int i = 0; i < 16; i++) {
    closeDisk(i);
  }
  // Close any open host files (managed by emu_io)
  emu_host_file_close_read();
  emu_host_file_close_write();
}

bool HBIOSDispatch::isWaitingForInput() const {
  // The host-file wait self-clears: JS moves the state to READING
  // (_emu_host_file_load) or IDLE (_emu_host_file_cancel), after which the
  // rewound HBF_HOST_READ call proceeds.
  return waiting_for_input ||
         (waiting_for_host_file &&
          emu_host_file_get_state() == HOST_FILE_WAITING_READ);
}

void HBIOSDispatch::clearWaitingState() {
  // Clear input waiting state without full reset
  // Used by SYSRESET to ensure clean state after reboot
  waiting_for_input = false;
  waiting_for_host_file = false;
  emu_state = HBIOS_RUNNING;
  output_buffer.clear();
  input_buffer.clear();
}

void HBIOSDispatch::reset() {
  trapping_enabled = false;
  waiting_for_input = false;
  waiting_for_host_file = false;
  idle_poll_count = 0;
  manifest_write_pending = false;  // Clear pending flag
  // Note: manifest_warning_shown is static, persists across resets within session
  emu_state = HBIOS_RUNNING;
  output_buffer.clear();
  input_buffer.clear();
  main_entry = 0xFFF0;

  signal_state = 0;
  signal_addr = 0;
  cur_bank = 0;
  bnkcpy_src_bank = 0x8E;
  bnkcpy_dst_bank = 0x8E;
  bnkcpy_count = 0;
  heap_ptr = 0x0200;  // Reset heap to start of HCB
  initialized_ram_banks = 0;  // Reset RAM bank initialization tracking
  // Note: disks are NOT cleared here - they persist across reset.
  // Call closeAllDisks() explicitly before loading new disk configuration.

  vda_rows = 25;
  vda_cols = 80;
  vda_cursor_row = 0;
  vda_cursor_col = 0;
  vda_color = 0x07;
  vda_rub = 0x00;

  for (int i = 0; i < 4; i++) {
    snd_volume[i] = 0;
    snd_period[i] = 0;
  }
  snd_duration = 100;

  // Close any open host files (managed by emu_io)
  emu_host_file_close_read();
  emu_host_file_close_write();
  host_transfer_mode = 0;  // Auto mode
  host_cmd_line.clear();

  // Reset memory disks
  for (int i = 0; i < 2; i++) {
    md_disks[i].current_lba = 0;
    md_disks[i].start_bank = 0;
    md_disks[i].num_banks = 0;
    md_disks[i].is_rom = false;
    md_disks[i].is_enabled = false;
  }
}

// Legacy setDebug interface - uses emu_log as the debug function
void HBIOSDispatch::setDebug(bool enable) {
  debug_log = enable ? emu_log : nullptr;
}

//=============================================================================
// State Machine I/O Methods
//=============================================================================

std::vector<uint8_t> HBIOSDispatch::getOutputChars() {
  std::vector<uint8_t> result = std::move(output_buffer);
  output_buffer.clear();
  return result;
}

void HBIOSDispatch::provideInputChar(int ch) {
  if (ch == '\n') ch = '\r';  // LF -> CR for CP/M
  input_buffer.push_back(ch);
  if (emu_state == HBIOS_NEEDS_INPUT) {
    emu_state = HBIOS_RUNNING;
    waiting_for_input = false;
  }
}

void HBIOSDispatch::queueInputChars(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    int ch = data[i];
    if (ch == '\n') ch = '\r';
    input_buffer.push_back(ch);
  }
  if (emu_state == HBIOS_NEEDS_INPUT && !input_buffer.empty()) {
    emu_state = HBIOS_RUNNING;
    waiting_for_input = false;
  }
}

void HBIOSDispatch::queueInputChar(int ch) {
  if (ch == '\n') ch = '\r';
  input_buffer.push_back(ch);
  if (emu_state == HBIOS_NEEDS_INPUT) {
    emu_state = HBIOS_RUNNING;
    waiting_for_input = false;
  }
}

int HBIOSDispatch::readInputChar() {
  if (input_buffer.empty()) return -1;
  int ch = input_buffer.front();
  input_buffer.erase(input_buffer.begin());
  return ch;
}

void HBIOSDispatch::clearInputBuffer() {
  input_buffer.clear();
}

//=============================================================================
// Disk Management
//=============================================================================

// The smallest thing that can be a disk. One 512-byte sector is already far
// below any real CP/M volume, but it is the bound that matters: everything
// downstream divides by 512 and indexes by LBA, and a zero-length image
// produced a mounted unit with no sectors that the guest could seek into.
// z80cpmw reported the same hole in its own loader (a 0-byte file mounted as an
// empty disk rather than being refused), and asked for it to be closed here as
// well so the two do not diverge.
static const size_t EMU_MIN_DISK_SIZE = 512;

bool HBIOSDispatch::loadDisk(int unit, const uint8_t* data, size_t size) {
  if (unit < 0 || unit >= 16) return false;
  if (!data || size < EMU_MIN_DISK_SIZE) {
    emu_error("[HBIOS] Refusing disk %d: %zu bytes is not a disk image "
              "(minimum %zu)\n", unit, size, EMU_MIN_DISK_SIZE);
    return false;
  }

  closeDisk(unit);

  disks[unit].data.assign(data, data + size);
  disks[unit].size = size;
  disks[unit].is_open = true;
  disks[unit].file_backed = false;
  disks[unit].handle = nullptr;

  // Always log disk loads (visible in status output)
  emu_status("[HBIOS] Loaded disk %d: %zu bytes (in-memory)\n", unit, size);
  return true;
}

bool HBIOSDispatch::loadDiskFromFile(int unit, const std::string& path) {
  if (unit < 0 || unit >= 16) return false;

  closeDisk(unit);

  emu_disk_handle handle = emu_disk_open(path, "rw");
  if (!handle) {
    // Try read-only
    handle = emu_disk_open(path, "r");
    if (!handle) {
      emu_fatal("[HBIOS] Cannot open disk file: %s\n", path.c_str());
    }
  }

  size_t size = emu_disk_size(handle);
  if (size < EMU_MIN_DISK_SIZE) {
    emu_disk_close(handle);
    emu_error("[HBIOS] Refusing disk %d: %s is %zu bytes, not a disk image "
              "(minimum %zu)\n", unit, path.c_str(), size, EMU_MIN_DISK_SIZE);
    return false;
  }

  disks[unit].handle = handle;
  disks[unit].path = path;
  disks[unit].size = size;
  disks[unit].is_open = true;
  disks[unit].file_backed = true;

  if (debug_log) {
    debug_log("[HBIOS] Loaded disk %d: %s (%zu bytes)\n", unit, path.c_str(), disks[unit].size);
  }
  return true;
}

void HBIOSDispatch::closeDisk(int unit) {
  if (unit < 0 || unit >= 16) return;

  if (disks[unit].file_backed && disks[unit].handle) {
    emu_disk_close((emu_disk_handle)disks[unit].handle);
  }
  disks[unit].handle = nullptr;
  disks[unit].data.clear();
  disks[unit].is_open = false;
  disks[unit].file_backed = false;
  disks[unit].size = 0;
  disks[unit].path.clear();
  disks[unit].dirty = false;  // a freshly loaded image starts clean
  // Reset partition detection state so new disk will be probed correctly
  disks[unit].current_lba = 0;
  disks[unit].partition_probed = false;
  disks[unit].partition_base_lba = 0;
  // ...and the SIZE, which was left behind.  RomWBW zeroes its whole working
  // block at the top of every EXT_SLICE call, so a partition size from a
  // previous medium cannot survive into the next one's bounds check.  Here it
  // did: swap a partitioned image for a bare one and the bare one was bounded
  // against the old image's partition.
  disks[unit].partition_sectors = 0;
  disks[unit].slice_size = 16640;  // Default hd512
  disks[unit].is_hd1k = false;
}

void HBIOSDispatch::closeAllDisks() {
  for (int i = 0; i < 16; i++) {
    closeDisk(i);
  }
}

void HBIOSDispatch::flushAllDisks() {
  for (int i = 0; i < 16; i++) {
    if (disks[i].is_open && disks[i].handle) {
      emu_disk_flush((emu_disk_handle)disks[i].handle);
    }
  }
}

bool HBIOSDispatch::isDiskLoaded(int unit) const {
  if (unit < 0 || unit >= 16) return false;
  return disks[unit].is_open;
}

const HBDisk& HBIOSDispatch::getDisk(int unit) const {
  static HBDisk empty;
  if (unit < 0 || unit >= 16) return empty;
  return disks[unit];
}

void HBIOSDispatch::setDiskSliceCount(int unit, int slices) {
  if (unit < 0 || unit >= 16) return;
  if (slices < 1) slices = 1;
  if (slices > 8) slices = 8;
  emu_log("[HBIOS] setDiskSliceCount: unit=%d slices=%d (was %d)\n", unit, slices, disks[unit].max_slices);
  disks[unit].max_slices = slices;
}

void HBIOSDispatch::setDiskIsManifest(int unit, bool is_manifest) {
  if (unit < 0 || unit >= 16) return;
  disks[unit].is_manifest = is_manifest;
}

void HBIOSDispatch::setDiskWarningSuppressed(int unit, bool suppressed) {
  if (unit < 0 || unit >= 16) return;
  disks[unit].warning_suppressed = suppressed;
}

bool HBIOSDispatch::pollManifestWriteWarning() {
  if (manifest_write_pending) {
    manifest_write_pending = false;
    manifest_warning_shown = true;  // Don't trigger again this session
    return true;
  }
  return false;
}

bool HBIOSDispatch::isDiskDirty(int unit) const {
  if (unit < 0 || unit >= 16) return false;
  return disks[unit].is_open && !disks[unit].file_backed && disks[unit].dirty;
}

void HBIOSDispatch::clearDiskDirty(int unit) {
  if (unit < 0 || unit >= 16) return;
  disks[unit].dirty = false;
}

const uint8_t* HBIOSDispatch::getDiskData(int unit) const {
  if (unit < 0 || unit >= 16) return nullptr;
  if (!disks[unit].is_open || disks[unit].file_backed) return nullptr;
  if (disks[unit].data.empty()) return nullptr;
  return disks[unit].data.data();
}

size_t HBIOSDispatch::getDiskDataSize(int unit) const {
  if (unit < 0 || unit >= 16) return 0;
  if (!disks[unit].is_open || disks[unit].file_backed) return 0;
  return disks[unit].data.size();
}

bool HBIOSDispatch::checkPeriodicFlush() {
  // Only flush if there are pending writes
  if (!disk_writes_pending) {
    return false;
  }

  // Check if enough time has passed since last flush
  time_t now = time(nullptr);
  if (last_periodic_flush == 0) {
    // First time - initialize the timer
    last_periodic_flush = now;
    return false;
  }

  if (now - last_periodic_flush < PERIODIC_FLUSH_INTERVAL) {
    return false;  // Not enough time elapsed
  }

  // Time to flush - call emu_disk_flush_all()
  emu_disk_flush_all();
  disk_writes_pending = false;
  last_periodic_flush = now;
  if (debug_log) debug_log("[HBIOS] Periodic disk flush (20s interval)\n");
  return true;
}

//=============================================================================
// Memory Disk Initialization
//=============================================================================

void HBIOSDispatch::initMemoryDisks() {
  if (debug_log) debug_log("[MD] initMemoryDisks called\n");
  if (!memory) {
    emu_error("[MD] Warning: memory not available, memory disks disabled\n");
    return;
  }

  // Note: HCB setup (APITYPE patching, shadow RAM copy) is handled by
  // emu_complete_init() in emu_init.cc. This function only reads HCB
  // configuration and sets up memory disk structures.

  // HCB (HBIOS Configuration Block) is at 0x0100 in ROM bank 0
  // Memory disk configuration is at:
  // CB_BIDRAMD0 = HCB + 0xDC = 0x1DC (RAM disk start bank)
  // CB_RAMD_BNKS = HCB + 0xDD = 0x1DD (RAM disk bank count)
  // CB_BIDROMD0 = HCB + 0xDE = 0x1DE (ROM disk start bank)
  // CB_ROMD_BNKS = HCB + 0xDF = 0x1DF (ROM disk bank count)
  const uint16_t HCB_BASE = 0x0100;

  uint8_t ramd_start = memory->read_bank(0x00, HCB_BASE + 0xDC);
  uint8_t ramd_banks = memory->read_bank(0x00, HCB_BASE + 0xDD);
  uint8_t romd_start = memory->read_bank(0x00, HCB_BASE + 0xDE);
  uint8_t romd_banks = memory->read_bank(0x00, HCB_BASE + 0xDF);
  if (debug_log) debug_log("[MD] HCB config: ramd_start=0x%02X ramd_banks=%d romd_start=0x%02X romd_banks=%d\n",
          ramd_start, ramd_banks, romd_start, romd_banks);

  // MD0 = RAM disk
  if (ramd_banks > 0) {
    md_disks[0].start_bank = ramd_start;
    md_disks[0].num_banks = ramd_banks;
    md_disks[0].is_rom = false;
    md_disks[0].is_enabled = true;
    md_disks[0].current_lba = 0;
    uint32_t size_kb = (uint32_t)ramd_banks * 32;
    emu_log("[MD] MD0 (RAM disk): banks 0x%02X-0x%02X, %uKB, %u sectors\n",
            ramd_start, ramd_start + ramd_banks - 1, size_kb, md_disks[0].total_sectors());
  }

  // MD1 = ROM disk
  if (romd_banks > 0) {
    md_disks[1].start_bank = romd_start;
    md_disks[1].num_banks = romd_banks;
    md_disks[1].is_rom = true;
    md_disks[1].is_enabled = true;
    md_disks[1].current_lba = 0;
    uint32_t size_kb = (uint32_t)romd_banks * 32;
    emu_log("[MD] MD1 (ROM disk): banks 0x%02X-0x%02X, %uKB, %u sectors\n",
            romd_start, romd_start + romd_banks - 1, size_kb, md_disks[1].total_sectors());
  }

  // Populate disk unit table so boot loader can enumerate all disks
  populateDiskUnitTable();
}

//=============================================================================
// Disk Unit Table Population
//=============================================================================

void HBIOSDispatch::populateDiskUnitTable() {
  if (debug_log) debug_log("[DISKUT] populateDiskUnitTable called\n");
  if (!memory) {
    emu_error("[DISKUT] Warning: memory not available\n");
    return;
  }

  // Get direct ROM pointer - write_bank() ignores ROM writes, so we need direct access
  uint8_t* rom = memory->get_rom();
  if (!rom) {
    emu_log("[DISKUT] Warning: ROM not available\n");
    return;
  }

  // The disk unit table is at HCB+0x60 (address 0x160), 16 entries of 4 bytes each
  // Format per entry:
  //   Byte 0: Device type (DIODEV_*): 0x00=MD, 0x09=HDSK, 0xFF=empty
  //   Byte 1: Unit number within device type
  //   Byte 2: Mode/attributes (0x80 = removable, etc.)
  //   Byte 3: Reserved/LU
  const uint16_t DISKUT_BASE = 0x160;  // HCB+0x60

  // First, mark all 16 entries as empty (0xFF) in both ROM and RAM bank 0x80
  for (int i = 0; i < 16; i++) {
    for (int b = 0; b < 4; b++) {
      rom[DISKUT_BASE + i * 4 + b] = 0xFF;
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + i * 4 + b), 0xFF);
    }
  }

  int disk_idx = 0;

  // Add hard disks FIRST so boot disk is at the start of the unit table
  if (debug_log) debug_log("[DISKUT] Scanning disks array for loaded disks...\n");
  for (int i = 0; i < 16 && disk_idx < 16; i++) {
    if (debug_log) debug_log("[DISKUT] disks[%d].is_open = %d, size = %zu\n", i, disks[i].is_open ? 1 : 0, disks[i].size);
    if (disks[i].is_open) {
      rom[DISKUT_BASE + disk_idx * 4 + 0] = 0x09;        // DIODEV_HDSK
      rom[DISKUT_BASE + disk_idx * 4 + 1] = (uint8_t)i;  // HDSK unit number
      rom[DISKUT_BASE + disk_idx * 4 + 2] = 0x00;        // No special attrs
      rom[DISKUT_BASE + disk_idx * 4 + 3] = 0x00;
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 0), 0x09);
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 1), (uint8_t)i);
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 2), 0x00);
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 3), 0x00);
      if (debug_log) debug_log("[DISKUT] Entry %d: HD%d (hard disk, %zu bytes)\n", disk_idx, i, disks[i].size);
      disk_idx++;
    }
  }

  // Add memory disks (MD0=RAM, MD1=ROM) AFTER hard disks
  for (int i = 0; i < 2 && disk_idx < 16; i++) {
    if (md_disks[i].is_enabled) {
      // Write to both ROM (for boot loader) and RAM bank 0x80 (working copy)
      rom[DISKUT_BASE + disk_idx * 4 + 0] = 0x00;        // DIODEV_MD
      rom[DISKUT_BASE + disk_idx * 4 + 1] = (uint8_t)i;  // Unit number
      rom[DISKUT_BASE + disk_idx * 4 + 2] = 0x00;        // No special attrs
      rom[DISKUT_BASE + disk_idx * 4 + 3] = 0x00;
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 0), 0x00);
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 1), (uint8_t)i);
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 2), 0x00);
      memory->write_bank(0x80, (uint16_t)(DISKUT_BASE + disk_idx * 4 + 3), 0x00);
      emu_log("[DISKUT] Entry %d: MD%d (memory disk)\n", disk_idx, i);
      disk_idx++;
    }
  }

  emu_log("[DISKUT] Populated %d disk entries in HCB\n", disk_idx);

  // Note: Drive map (0x120) and CB_DEVCNT are populated by emu_populate_drive_map()
  // called from emu_complete_init() after all disk tables are set up.
}

//=============================================================================
// ROM Application Management
//=============================================================================

void HBIOSDispatch::addRomApp(const std::string& name, const std::string& path, char key) {
  HBRomApp app;
  app.name = name;
  app.sys_path = path;
  app.key = key;
  app.is_loaded = emu_file_exists(path);
  rom_apps.push_back(app);
}

void HBIOSDispatch::clearRomApps() {
  rom_apps.clear();
}

int HBIOSDispatch::findRomApp(char key) const {
  char upper_key = (char)std::toupper(key);
  for (size_t i = 0; i < rom_apps.size(); i++) {
    if (std::toupper(rom_apps[i].key) == upper_key && rom_apps[i].is_loaded) {
      return (int)i;
    }
  }
  return -1;
}

//=============================================================================
// Signal Port Handler
//=============================================================================

void HBIOSDispatch::handleSignalPort(uint8_t value) {
  // Supports two protocols:
  //
  // Protocol 1 (simple status):
  //   0x01 = HBIOS starting
  //   0xFE = PREINIT point
  //   0xFF = Init complete, enable trapping
  //
  // Protocol 2 (sequential address registration, used by emu_hbios):
  //   0x02 = Start sequential registration
  //   Then sends: CIO_L, CIO_H, DIO_L, DIO_H, RTC_L, RTC_H, SYS_L, SYS_H
  //   State counts from 1-8 for these bytes
  //
  // Protocol 3 (prefixed registration):
  //   0x10-0x15 = Start registration for specific handler
  //   Then low byte, then high byte

  if (signal_state == 0) {
    // Check for special signals
    switch (value) {
      case 0x01:  // HBIOS starting
        return;

      case 0x02:  // Protocol 2: Start sequential registration (accept but ignore)
        signal_state = 1;
        return;

      case 0xFE:  // PREINIT point
        return;

      case 0xFF:  // Init complete
        return;

      // Protocol 3: Start address registration (accept but ignore)
      case 0x10:  // Start CIO registration
      case 0x11:  // Start DIO registration
      case 0x12:  // Start RTC registration
      case 0x13:  // Start SYS registration
      case 0x14:  // Start VDA registration
      case 0x15:  // Start SND registration
        signal_state = 0x80 | (value - 0x10);
        return;

      default:
        return;
    }
  }

  // Protocol 3: Prefixed registration (accept bytes, ignore values)
  if (signal_state & 0x80) {
    if (signal_addr == 0) {
      signal_addr = value;  // Low byte
    } else {
      signal_state = 0;     // High byte received, done
      signal_addr = 0;
    }
    return;
  }

  // Protocol 2: Sequential registration (accept bytes, ignore values)
  if (signal_state >= 1 && signal_state <= 8) {
    if (signal_state < 8) {
      signal_state++;
    } else {
      signal_state = 0;
    }
  }
}

//=============================================================================
// Function Dispatch
//=============================================================================

int HBIOSDispatch::getTrapTypeFromFunc(uint8_t func) {
  // Determine handler type from HBIOS function code in B register
  if (func <= 0x0F) return 0;        // CIO (0x00-0x0F)
  if (func <= 0x1F) return 1;        // DIO (0x10-0x1F)
  if (func <= 0x2F) return 2;        // RTC (0x20-0x2F)
  if (func <= 0x3F) return 6;        // DSKY (0x30-0x3F)
  if (func <= 0x4F) return 4;        // VDA (0x40-0x4F)
  if (func <= 0x5F) return 5;        // SND (0x50-0x5F)
  if (func >= 0xE0 && func <= 0xEF) return 7;  // EXT (0xE0-0xEF, includes host file)
  if (func >= 0xF0) return 3;        // SYS (0xF0-0xFF)
  return -1;
}

bool HBIOSDispatch::handleMainEntry() {
  if (!cpu) return false;

  uint8_t func = cpu->regs.BC.get_high();
  int trap_type = getTrapTypeFromFunc(func);

  switch (trap_type) {
    case 0: handleCIO(); return true;
    case 1: handleDIO(); return true;
    case 2: handleRTC(); return true;
    case 3: handleSYS(); return true;
    case 4: handleVDA(); return true;
    case 5: handleSND(); return true;
    case 6: handleDSKY(); return true;
    case 7: handleEXT(); return true;
    default:
      // $60-$DF is the reserved gap between BF_SND and BF_EXT, and RomWBW has a
      // name for what it answers there: hbios.asm:4525-4530 is
      // "CP BF_EXT / JR C,HB_DISPERR" and "HB_DISPERR: SYSCHKERR(ERR_NOFUNC)".
      // This answered HBR_FAILED ($FF = ERR_UNDEF, "undefined error"), which is
      // a different code with a different meaning - a caller probing for a
      // function cannot tell "there is no such function" from "something went
      // wrong".
      emu_log("[HBIOS] Unknown function 0x%02X (trap_type=%d)\n", func, trap_type);
      setResult(HBR_NOFUNC);
      doRet();
      return true;
  }
}

void HBIOSDispatch::handlePRTSUM() {
  // Print device summary (called by romldr 'd' command)
  // All output goes to output_buffer for caller to display
  writeConsoleString("\r\nDisk Device Summary\r\n\r\n");
  writeConsoleString(" Unit Dev       Type    Capacity\r\n");
  writeConsoleString(" ---- --------- ------- --------\r\n");

  int unit_num = 0;
  char line[80];

  // Print memory disks
  for (int i = 0; i < 2; i++) {
    if (md_disks[i].is_enabled) {
      const char* type = md_disks[i].is_rom ? "ROM" : "RAM";
      uint32_t size_kb = md_disks[i].num_banks * 32;
      snprintf(line, sizeof(line), "   %2d MD%d       %-7s %4uKB\r\n",
               unit_num, i, type, size_kb);
      writeConsoleString(line);
      unit_num++;
    }
  }

  // Print hard disks
  for (int i = 0; i < 16; i++) {
    if (disks[i].is_open) {
      uint32_t size_mb = (uint32_t)(disks[i].size / (1024 * 1024));
      snprintf(line, sizeof(line), "   %2d HDSK%d     Hard    %4uMB\r\n",
               unit_num, i, size_mb);
      writeConsoleString(line);
      unit_num++;
    }
  }

  // Print footer
  writeConsoleString("\r\n");
}

//=============================================================================
// Port 0xEF Dispatch (unified entry for all platforms)
//=============================================================================

void HBIOSDispatch::handlePortDispatch() {
  // Port-based dispatch: Z80 proxy does OUT (0xEF), A; RET
  // We handle the HBIOS call here, then Z80 continues with RET
  skip_ret = true;
  handleMainEntry();
  skip_ret = false;
}

//=============================================================================
// Helper Functions
//=============================================================================

void HBIOSDispatch::setResult(uint8_t result) {
  // EVERY HBIOS RETURN IS `OR A` OR `XOR A`, so the flags are not just Z.
  //
  // A success return is `XOR A` (A=0, Z set, S clear, C clear, P/V set for even
  // parity) and an error return is `SYSCHKERR` ending in `LD A,HB_ERR / OR A`
  // (hbios.asm:229-232), which sets S from bit 7 - set for every negative error
  // code - clears C, clears N and H, and sets P/V from the parity of A.
  //
  // This set Z and left the rest of F as the guest had it. A caller doing the
  // idiomatic `CALL HBIOS / JP M,error` - test the SIGN, which is the natural
  // test when every error is negative - read its own stale sign flag, and one
  // doing `JR C,error` read its own stale carry. Both are things an assembler
  // programmer writes without thinking, because on real hardware `OR A` has
  // just made them true.
  cpu->regs.AF.set_high(result);

  uint8_t f = cpu->regs.AF.get_low();
  // OR A clears carry, N and H.
  f &= (uint8_t)~(qkz80_cpu_flags::CY | qkz80_cpu_flags::N | qkz80_cpu_flags::H);
  // Z from the value, S from its bit 7.
  if (result == 0) f |= qkz80_cpu_flags::Z; else f &= (uint8_t)~qkz80_cpu_flags::Z;
  if (result & 0x80) f |= qkz80_cpu_flags::S; else f &= (uint8_t)~qkz80_cpu_flags::S;
  // P/V is parity for a logical operation: set when the number of set bits is
  // even, which is what OR A leaves behind.
  uint8_t bits = result;
  bits ^= (uint8_t)(bits >> 4);
  bits ^= (uint8_t)(bits >> 2);
  bits ^= (uint8_t)(bits >> 1);
  if (bits & 1) f &= (uint8_t)~qkz80_cpu_flags::P; else f |= qkz80_cpu_flags::P;
  cpu->regs.AF.set_low(f);
}

void HBIOSDispatch::recalcNvramChecksum() {
  // Calculate NVRAM checksum (byte 4)
  // The checksum is XOR of bytes 0-3 XOR with RomWBW version bytes
  // Checksum = (byte0 ^ byte1 ^ byte2 ^ byte3) ^ ((RMJ << 4) | RMN) ^ ((RUP << 4) | RTP)
  //
  // The version comes from the loaded ROM: SYSCONF, running inside that ROM,
  // seeds its checksum the same way.
  //
  // NOT every caller runs during HBIOS dispatch, which this comment used to
  // claim. romwbw_emu.cc calls setNvramSetting() for --boot and for a
  // persisted setting BEFORE emu_load_rom(), so there is no ROM to ask and
  // the seed would be zero. Before v1.44 it was a compile-time default
  // instead, which was just as wrong for every release that was not that
  // default - a 3.6.0 ROM got a 3.5.1 seed and nothing ever corrected it.
  //
  // So a checksum computed with no ROM is marked provisional and re-seeded on
  // the first guest read, where the ROM is loaded by definition.
  uint8_t xsum = nvram_switches[0] ^ nvram_switches[1] ^ nvram_switches[2] ^ nvram_switches[3];
  emu_romwbw_release release = {0, 0};
  nvram_checksum_provisional = !emu_romwbw_release_loaded(memory, &release);
  xsum ^= release.ver;  // RMJ.RMN
  xsum ^= release.upd;  // RUP.RTP
  nvram_switches[4] = xsum;
  nvram_dirty = true;
}

// Re-seed a checksum computed before the ROM was loaded. Called on every path
// that hands NVRAM bytes to the guest; a no-op once the seed is real.
void HBIOSDispatch::settleNvramChecksum() {
  if (!nvram_checksum_provisional) return;
  bool was_dirty = nvram_dirty;
  recalcNvramChecksum();
  // Re-seeding is not a modification the guest made, so it must not look like
  // one: nvram_dirty drives whether the setting is written back at exit.
  nvram_dirty = was_dirty;
}

void HBIOSDispatch::doRet() {
  // Skip synthetic RET when using I/O port dispatch
  // (Z80 proxy code has its own RET instruction)
  if (skip_ret) return;

  if (!cpu || !memory) return;

  uint16_t sp = cpu->regs.SP.get_pair16();
  uint8_t lo = memory->fetch_mem(sp);
  uint8_t hi = memory->fetch_mem(sp + 1);
  uint16_t ret_addr = (uint16_t)(lo | (hi << 8));
  cpu->regs.SP.set_pair16(sp + 2);
  cpu->regs.PC.set_pair16(ret_addr);

  if (debug_log) {
    emu_log("[HBIOS RET] SP=0x%04X -> PC=0x%04X A=0x%02X\n",
            sp, ret_addr, cpu->regs.AF.get_high());
  }
}

// Fetch a NUL-terminated string out of guest memory, refusing to guess.
// Returns false when no terminator appears within HOST_PATH_MAX bytes; `out` is
// then left holding what was scanned, for the diagnostic only - callers must
// not use it as a path.  Guest memory is 64K and wraps, so the address
// arithmetic is done in uint16_t deliberately: a path starting near 0xFFFF
// reads on round the bottom of memory, which is what a Z80 doing the same
// pointer arithmetic would see.
bool HBIOSDispatch::fetchGuestString(uint16_t addr, std::string* out) const {
  out->clear();
  for (int i = 0; i < HOST_PATH_MAX; i++) {
    uint8_t ch = memory->fetch_mem((uint16_t)(addr + i));
    if (ch == 0) return true;
    *out += (char)ch;
  }
  return false;
}

// Deliver a host path into a guest buffer whose size the guest chose.
//
// bufsize is one byte - it arrives in C - so at most 254 characters and a
// terminator fit.  A destination longer than that is not rare: the CLI
// prepends the whole working directory to every bare name, and a deep checkout
// is easily 250 characters.  Clamping would hand the guest a path chopped
// mid-component, which the utility then prints as fact - the exact failure
// these two calls exist to remove.  So keep the END of the path, where the file
// name is, and mark the cut with a leading "...", which reads as a fragment
// rather than as a wrong path.
//
// Returns false, having written nothing, when there is nothing to report or no
// room for even one character and a NUL.  The guest's buffer still holds the
// path it asked for, which is what R8 and W8 fall back to printing.
bool HBIOSDispatch::storeHostName(const char* name, uint8_t bufsize,
                                  uint16_t buf_addr) {
  if (!name || !*name || bufsize < 2) return false;

  const size_t room = (size_t)bufsize - 1;      // >= 1: bufsize >= 2
  size_t len = strlen(name);
  const char* src = name;
  std::string shortened;
  if (len > room) {
    static const char kCut[] = "...";
    const size_t cut = sizeof(kCut) - 1;
    // With no room for the marker plus a character of path, the marker alone
    // would say nothing; take the tail and let it be short.
    shortened = (room > cut) ? (std::string(kCut) + (name + (len - (room - cut))))
                             : std::string(name + (len - room));
    src = shortened.c_str();
    len = room;
  }
  for (size_t i = 0; i < len; i++) {
    memory->store_mem(buf_addr + (uint16_t)i, (uint8_t)src[i]);
  }
  memory->store_mem(buf_addr + (uint16_t)len, 0);
  return true;
}

void HBIOSDispatch::writeConsoleString(const char* str) {
  // Use direct console output (same path as CIOOUT) for consistent display
  while (*str) {
    emu_console_write_char(*str++);
  }
}

//=============================================================================
// Character I/O (CIO)
//=============================================================================

void HBIOSDispatch::handleCIO() {
  if (!cpu) return;

  uint8_t func = cpu->regs.BC.get_high();  // B = function
  uint8_t unit = cpu->regs.BC.get_low();   // C = unit
  uint8_t result = HBR_SUCCESS;

  // THE UNIT NUMBER IN C IS CHECKED FIRST, and against a count.
  //
  // Every group in RomWBW reaches its driver through HB_DISPCALC, whose first
  // act is "LD A,C / CP (IY-1) / JR NC,HB_UNITERR" (hbios.asm:7448-7452) -
  // compare the unit against the count, and answer ERR_NOUNIT if it is not
  // there.  Nothing here checked it at all, so every unit 0-255 was served by
  // the one device this emulator has and told it had succeeded: writing to a
  // second serial port printed on the console, reading from one ate the user's
  // keystrokes, and a program enumerating units until ERR_NOUNIT never stopped.
  //
  // The count is the one BF_SYSGET reports for the group; see hbios_dispatch.h.
  // CIO alone has a substitution before the check: CIO_DISPATCH does
  // "BIT 7,C / CALL NZ,CIO_SPECIAL" (hbios.asm:4541-4542), and CIO_SPECIAL
  // swaps in the active console - so $80-$FF are always valid and always mean
  // "the console".  There is one console here, so they mean unit 0.
  if (unit & 0x80) unit = 0;
  if (unit >= CIO_UNIT_COUNT) {
    setResult(HBR_NOUNIT);
    doRet();
    return;
  }

  switch (func) {
    case HBF_CIOIN: {
      // Read character - behavior depends on dispatch mode and platform
      if (!blocking_allowed && !emu_console_has_input()) {
        // Non-blocking mode (web/WASM) - no input available
        // Rewind PC to re-execute OUT instruction when input arrives
        // OUT (0xEF), A is 2 bytes, PC currently points past it
        uint16_t pc = cpu->regs.PC.get_pair16();
        cpu->regs.PC.set_pair16(pc - 2);
        waiting_for_input = true;
        return;  // Don't call setResult/doRet - will retry
      }
      // Blocking mode (CLI) - flush any pending output before blocking
      // This ensures prompts are displayed before waiting for input
      while (!output_buffer.empty()) {
        emu_console_write_char(output_buffer.front());
        output_buffer.erase(output_buffer.begin());
      }
      // Now read char (blocks if needed)
      int ch = emu_console_read_char();
      if (ch == EMU_CONSOLE_RETRY) {
        // The keystroke was the host's reserved escape key, so there is no
        // byte for the guest. Rewind over the 2-byte OUT (0xEF),A exactly as
        // the non-blocking branch above does: the main loop then sees the
        // escape and this call re-runs afterwards. Handing the guest the
        // escape byte as well is what used to make one press of ^E both move
        // the WordStar cursor and drop into sim>.
        uint16_t pc = cpu->regs.PC.get_pair16();
        cpu->regs.PC.set_pair16(pc - 2);
        return;  // Don't call setResult/doRet - will retry
      }
      if (debug_log) {
        debug_log("[CIOIN] read char: %d (0x%02X) '%c'\n", ch, ch & 0xFF, (ch >= 32 && ch < 127) ? ch : '?');
      }
      if (ch < 0) {
        // EOF - return 0x1A (^Z) as end-of-file marker
        ch = 0x1A;
      }
      cpu->regs.DE.set_low(ch & 0xFF);
      waiting_for_input = false;
      idle_poll_count = 0;
      break;
    }

    case HBF_CIOOUT: {
      // Write character to output buffer (caller retrieves and displays)
      // This is needed for web/WASM where output is polled from main loop
      uint8_t ch = cpu->regs.DE.get_low();
      if (debug_log) {
        emu_log("[CIOOUT] char: %d (0x%02X) '%c'\n", ch, ch, (ch >= 32 && ch < 127) ? ch : '?');
      }
      output_buffer.push_back(ch);
      idle_poll_count = 0;
      break;
    }

    case HBF_CIOIST: {
      // Input status - return count from emu_console
      // Returns: A = status (0=no data, non-zero=data ready)
      //          E = pending byte count (0xFF if unknown)
      // A COUNT, and it must stay positive. RomWBW's polled drivers return 1
      // for "one character waiting" - uart.asm UART_IST1 does "XOR A / INC A ;
      // ACCUM := 1 TO SIGNAL 1 CHAR WAITING", acia.asm the same - and the
      // interrupt-driven variants return a buffer utilisation count. hbios.inc
      // reserves the NEGATIVE range for error codes.
      //
      // This returned 0xFF, which has bit 7 set and therefore reads as an error
      // to anything testing the sign. HTALK.COM ships on the combo image and
      // does exactly that ("JP M,..."), so it never reached BF_CIOIN, never saw
      // the ^C that is its only exit, and span forever.
      bool has_input = emu_console_has_input();
      result = has_input ? 1 : 0;  // Count of characters waiting
      cpu->regs.DE.set_low(has_input ? 1 : 0);  // E = pending count
      // Track consecutive "no input" polls for idle detection
      if (has_input) idle_poll_count = 0; else idle_poll_count++;
      break;
    }

    case HBF_CIOOST: {
      // Output status - always ready
      // A = status (0=not ready, 1=ready), E = buffer space available
      result = 1;  // Ready to output
      cpu->regs.DE.set_low(0xFF);  // Lots of buffer space
      break;
    }

    case HBF_CIOINIT:
      // Initialize - nothing to do
      break;

    case HBF_CIOQUERY: {
      // Query the LINE CONFIGURATION, which is not the same question as
      // HBF_CIODEVICE below and used to be answered as though it were.
      //
      // RomWBW returns an encoded baud rate and framing here; a caller decodes
      // D's low 5 bits as a baud code against a 75-baud base and E as data bits
      // (Source/HBIOS/invntdev.asm:238-256). Answering with a device type and a
      // unit number meant D=0, E=0, which decodes as a real configuration -
      // "75,5,N,1" - rather than as an absent one.
      //
      // There is no serial line here to describe: the console is a host
      // terminal. Two shipped callers ask, and they check different things, so
      // this answers both.
      //
      //   invntdev (the ROM device inventory) ignores the status and tests the
      //   value: LD A,D / AND E / INC A / JP Z,PS_PRTNUL. $FFFF is how RomWBW
      //   spells "no config defined", and it prints nothing for that unit.
      //
      //   MODE.COM ships on the published images and does the opposite - it
      //   ignores the value and aborts on status: "rst 08 / ret nz". Returning
      //   $FFFF with SUCCESS made it decode $FF as an encoded baud code of 31
      //   and print "COM0: 7372800,S,8,2".
      //
      // So: the value invntdev wants, AND a status MODE.COM will stop on.
      // Returning one without the other gives one of them garbage - which is
      // how this was found, by fixing it for the inventory and then running
      // MODE.
      cpu->regs.DE.set_pair16(0xFFFF);
      cpu->regs.HL.set_pair16(0xFFFF);  // tty.asm TTY_QUERY sets both
      // TTY_QUERY returns those with SUCCESS, and that is deliberately not
      // copied: MODE.COM ignores the value and aborts on status, so success
      // here sends it on to decode $FF as an encoded baud code and print
      // "COM0: 7372800,S,8,2". Measured both ways. NOTIMPL is the answer that
      // leaves both shipped callers correct.
      result = HBR_NOTIMPL;
      break;
    }

    case HBF_CIODEVICE: {
      // FIVE registers, not two.  SystemGuide.md, Function 0x06:
      //
      //     C: Device Attributes     D: Device Type
      //     E: Device Number         H: Device Mode
      //     L: Device I/O Base Address
      //
      // This set DE to zero and touched nothing else, so C, H and L came back
      // holding whatever the caller passed in - and C is the one the guest
      // asked WITH, the unit number, handed straight back as though it were
      // the attribute byte.  D = 0 is also a claim: CIODEV_UART (hbios.inc:384).
      //
      // The console here is not a UART and not an ASCI; it is the host's
      // terminal, which is what CIODEV_TERM ($02) means, and tty.asm:138-145 is
      // the driver that answers for one:
      //
      //     LD D,CIODEV_TERM / LD E,<devnum> / LD A,<vda unit> / SET 7,A / LD C,A
      //
      // Bit 7 of C is the "terminal" attribute the guide describes ("the two
      // high bits ... 01 = Terminal"), and the low bits are the VDA unit.  H is
      // the device mode and L the I/O base; there is neither here, so H := 0
      // the way hdsk.asm answers "DRIVER HAS NO MODES", and L := 0 because
      // there is no port.
      cpu->regs.BC.set_low(0x80);   // C := attributes: terminal, VDA unit 0
      cpu->regs.DE.set_high(0x02);  // D := CIODEV_TERM
      cpu->regs.DE.set_low(0x00);   // E := device number
      cpu->regs.HL.set_pair16(0);   // H := no modes, L := no I/O base
      break;
    }

    default:
      // A guest asking for a function we do not implement is not a reason to
      // kill the host. This called emu_fatal() until 2026-09-05, so an
      // unimplemented CIO function ended the process with SIGABRT and took the
      // guest's session with it. HBR_NOFUNC is what RomWBW returns and what
      // callers are written to handle; the log keeps the gap discoverable.
      emu_error("[HBIOS CIO] Unhandled function 0x%02X (unit=%d) - returning "
                "HBR_NOFUNC\n", func, unit);
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// Disk I/O (DIO)
//=============================================================================

// THE DISK UNIT NUMBER IS A PLAIN INDEX.  There is no high-bit, nibble or
// boot-related encoding of unit numbers anywhere in HBIOS: C is an index into
// DIO_TBL, bounds-checked against the live entry count before any driver is
// reached (hbios.asm:7448-7452, HB_DISPCALC - "LD A,C / CP (IY-1) ; COMPARE TO
// COUNT / JR NC,HB_UNITERR"), and DIO_MAX is 16.  Anything at or above the
// count is ERR_NOUNIT from the dispatcher.
//
// These two mappers used to accept three invented ranges as well - 0x80-0x8F
// and 0xC0-0xCF for memory disks, 0x90-0x9F for hard disks - so unit 0x80 read
// MD0, 0xC3 read MD1 and 0x92 read hard disk 2, each answering A = 0.  They
// also ALIASED: a write to unit 0x92 and a write to unit 4 landed on the same
// image.  The direction that matters is that a guest walking unit numbers past
// the count DIOCNT reported found phantom disks that answered successfully
// where real RomWBW would have stopped it.

// Map a RomWBW unit number to a memory disk index (0=MD0, 1=MD1, 0xFF=not MD)
static uint8_t map_md_unit(uint8_t unit) {
  if (unit < 2) return unit;
  return 0xFF;  // Not a memory disk
}

// Map a RomWBW unit number to a hard disk array index.
// Units 0-1 are the memory disks; 2..17 are the sixteen hard disks.
static uint8_t map_hd_unit(uint8_t unit) {
  if (unit < 2) return 0xFF;
  if (unit < 18) return unit - 2;
  return 0xFF;
}

// Check if unit is a memory disk and if it's enabled
static bool is_md_unit(uint8_t unit, const MemDiskState* md_disks) {
  uint8_t md_idx = map_md_unit(unit);
  return (md_idx != 0xFF) && md_disks[md_idx].is_enabled;
}

// Get memory disk index for a unit (assumes is_md_unit returned true)
static uint8_t get_md_index(uint8_t unit) {
  return map_md_unit(unit);
}

void HBIOSDispatch::handleDIO() {
  if (!cpu || !memory) return;
  idle_poll_count = 0;  // Disk I/O = real work

  uint8_t func = cpu->regs.BC.get_high();  // B = function
  uint8_t raw_unit = cpu->regs.BC.get_low();   // C = unit
  uint8_t result = HBR_SUCCESS;

  // Check if this is a memory disk (MD) or hard disk (HD)
  bool is_memdisk = is_md_unit(raw_unit, md_disks);
  uint8_t md_unit = get_md_index(raw_unit);  // Map to MD index (0=MD0, 1=MD1)
  uint8_t hd_unit = map_hd_unit(raw_unit);  // Map to disk array index (for HD)
  bool is_harddisk = (hd_unit != 0xFF && hd_unit < 16 && disks[hd_unit].is_open);

  // DEBUG: Unconditional logging for DIO calls
  static int dio_log_count = 0;
  if (debug_log && dio_log_count < 100) {
    dio_log_count++;
    debug_log("[DIO #%d] func=0x%02X unit=%d hd_unit=%d is_md=%d is_hd=%d disk_open=%d\n",
            dio_log_count, func, raw_unit, hd_unit, is_memdisk ? 1 : 0, is_harddisk ? 1 : 0,
            (hd_unit < 16) ? (disks[hd_unit].is_open ? 1 : 0) : -1);
  }

  switch (func) {
    case HBF_DIOSTATUS: {
      // The answer is A and only A.  Neither md.asm nor hdsk.asm touches D or
      // E on this call (md.asm:186-194, hdsk.asm:152-160), and writing E here
      // destroyed a register the caller is entitled to keep across the call.
      if (!is_memdisk && !is_harddisk) {
        result = HBR_NOUNIT;
      }
      break;
    }

    // Reset clears error state. It does NOT rewind the seek position: RomWBW's
    // drivers leave the LBA alone, and clearing it here meant a reset between a
    // seek and a read silently moved the read to sector 0.
    case HBF_DIORESET:
      if (!is_memdisk && !is_harddisk) {
        result = HBR_NOUNIT;
      }
      // There is no error state to clear here, and the seek position is
      // deliberately left alone.
      break;

    case HBF_DIOSEEK: {
      // Seek.  DE:HL is EITHER an LBA or a CHS address, and bit 7 of D says
      // which - set means LBA.  This masked the bit off and always treated the
      // rest as an LBA, so a guest seeking by CHS landed on a sector computed
      // from its cylinder/head/sector as though they were one number.
      //
      // Every driver opens the same way (hdsk.asm:208-217, md.asm:259-268):
      //
      //     BIT 7,D              ; CHECK FOR LBA FLAG
      //     CALL Z,HB_CHS2LBA    ; CLEAR MEANS CHS, CONVERT TO LBA
      //     RES 7,D              ; CLEAR FLAG REGARDLESS
      //
      // and HB_CHS2LBA (hbios.asm:7223-7236) is head<<4 | sector into the low
      // byte, with the cylinder shifted up a byte:
      //
      //     LBA = (cylinder << 8) | (head << 4) | sector
      //
      // which is exactly 16 heads by 16 sectors - the same geometry DIOGEOM
      // reports, and the reason cylinders there are blocks/256.
      uint16_t de_reg = cpu->regs.DE.get_pair16();
      uint16_t hl_reg = cpu->regs.HL.get_pair16();
      uint32_t lba;
      if (de_reg & 0x8000) {
        lba = (((uint32_t)(de_reg & 0x7FFF) << 16) | hl_reg);
      } else {
        uint8_t head = cpu->regs.DE.get_high() & 0x0F;
        uint8_t sect = cpu->regs.DE.get_low() & 0x0F;
        lba = ((uint32_t)hl_reg << 8) | (uint32_t)((head << 4) | sect);
      }

      if (is_memdisk) {
        md_disks[md_unit].current_lba = lba;
      } else if (is_harddisk) {
        disks[hd_unit].current_lba = lba;
      } else {
        // No device at this unit - return error
        // stderr, NOT the guest's console. RomWBW answers a bad unit with
        // ERR_NOUNIT and prints nothing (HB_UNITERR); the driver trace that
        // does print is compiled out of a release build. Writing here landed
        // emulator text in the middle of whatever the guest was displaying -
        // fatal to a full-screen application and to any binary stream running
        // over the console - and leaked internal state to the guest besides.
        emu_error("[HBIOS DIOSEEK] no unit %d (hd_unit=%d is_open=%d)\n",
                  raw_unit, hd_unit, (hd_unit < 16) ? disks[hd_unit].is_open : -1);
        result = HBR_NOUNIT;
      }
      break;
    }

    case HBF_DIOREAD: {
      // Read sectors using current_lba (set by DIOSEEK)
      // Input: BC=Function/Unit, HL=Buffer Address, D=Buffer Bank (0x80=use current), E=Block Count
      // Output: A=Result, E=Blocks Read

      // Trace DIOREAD calls during CP/M 3 boot investigation
      static int dioread_trace_count = 0;
      if (debug_log && is_harddisk && dioread_trace_count < 50) {
        dioread_trace_count++;
        debug_log("[DIOREAD #%d] unit=%d LBA=%u bank=0x%02X addr=0x%04X count=%d\n",
                dioread_trace_count, raw_unit, disks[hd_unit].current_lba,
                cpu->regs.DE.get_high(), cpu->regs.HL.get_pair16(), cpu->regs.DE.get_low());
      }

      if (!is_memdisk && !is_harddisk) {
        // No device at this unit - return error with 0 blocks read
        emu_error("[HBIOS DIOREAD] no unit %d (hd_unit=%d is_open=%d)\n",
                  raw_unit, hd_unit, (hd_unit < 16) ? disks[hd_unit].is_open : -1);
        cpu->regs.DE.set_low(0);
        result = HBR_NOUNIT;
        break;
      }

      uint16_t buffer = cpu->regs.HL.get_pair16();
      uint8_t buffer_bank = cpu->regs.DE.get_high();
      uint8_t count = cpu->regs.DE.get_low();
      uint8_t blocks_read = 0;

      // Helper lambda to write byte to correct bank
      // When buffer_bank has bit 7 set (RAM bank 0x80-0x8F), use bank-aware write
      // Otherwise use store_mem() which respects current bank
      auto write_to_bank = [&](uint16_t addr, uint8_t byte) {
        if (buffer_bank & 0x80) {
          // Bank-aware write (RAM bank specified)
          if (addr >= 0x8000) {
            // Common area - write to bank 0x8F
            memory->write_bank(0x8F, addr - 0x8000, byte);
          } else {
            memory->write_bank(buffer_bank, addr, byte);
          }
        } else {
          // Use current bank (ROM bank or 0 specified)
          memory->store_mem(addr, byte);
        }
      };

      if (is_memdisk) {
        // Memory disk read - read from ROM/RAM bank memory
        MemDiskState& md = md_disks[md_unit];

        // 64 sectors per 32KB bank (512 bytes per sector)
        const uint32_t sectors_per_bank = 64;

        for (int s = 0; s < count; s++) {
          if (md.current_lba >= md.total_sectors()) {
            break;
          }

          uint32_t bank_offset = md.current_lba / sectors_per_bank;
          uint32_t sector_in_bank = md.current_lba % sectors_per_bank;
          uint8_t src_bank = (uint8_t)(md.start_bank + bank_offset);
          uint16_t src_offset = (uint16_t)(sector_in_bank * 512);

          // Copy 512 bytes from bank memory to buffer
          for (int j = 0; j < 512; j++) {
            uint8_t byte = memory->read_bank(src_bank, (uint16_t)(src_offset + j));
            write_to_bank((uint16_t)(buffer + s * 512 + j), byte);
          }

          md.current_lba++;
          blocks_read++;
        }
      } else {
        // Hard disk read
        uint32_t lba = disks[hd_unit].current_lba;

        if (disks[hd_unit].file_backed && disks[hd_unit].handle) {
          // Read from file
          uint8_t sector_buf[512];
          for (int s = 0; s < count; s++) {
            // lba is 32-bit: compute the byte offset in 64-bit so a large
            // guest seek can't wrap and land on the wrong sector
            uint64_t offset = ((uint64_t)lba + s) * 512;
            size_t read = emu_disk_read((emu_disk_handle)disks[hd_unit].handle,
                                        (size_t)offset, sector_buf, 512);
            // A PARTIAL SECTOR IS NOT A SECTOR.  This tested `read == 0`, so a
            // short read of 1..511 bytes - a truncated image, or a final
            // partial sector - fell through, copied all 512 bytes of
            // sector_buf, and counted the block as transferred.  The bytes past
            // `read` are whatever was on the stack, handed to the guest as
            // data, with A = 0 and E saying it arrived.  The loop below already
            // turns a short TRANSFER into ERR_IO; this is the same rule one
            // level down, on a short SECTOR.
            if (read != 512) {
              break;
            }
            for (size_t i = 0; i < 512; i++) {
              write_to_bank((uint16_t)(buffer + s * 512 + i), sector_buf[i]);
            }
            blocks_read++;
          }
        } else if (!disks[hd_unit].data.empty()) {
          // Read from memory buffer
          for (int s = 0; s < count; s++) {
            uint64_t offset = ((uint64_t)lba + s) * 512;
            if (offset + 512 > disks[hd_unit].data.size()) {
              break;
            }
            for (size_t i = 0; i < 512; i++) {
              write_to_bank((uint16_t)(buffer + s * 512 + i), disks[hd_unit].data[(size_t)offset + i]);
            }
            blocks_read++;
          }
        } else {
          emu_fatal("[HBIOS DIOREAD] HD%d is_open but no data (file_backed=%d, data.empty=%d)\n",
                    hd_unit, disks[hd_unit].file_backed, disks[hd_unit].data.empty());
        }

        // Update current_lba for next sequential access
        disks[hd_unit].current_lba += blocks_read;
      }

      cpu->regs.DE.set_low(blocks_read);
      // "RETURN WITH SECTORS READ IN E AND UPDATED DMA ADDRESS IN HL" - RomWBW's
      // own words. HL was left holding the ORIGINAL buffer address, so a caller
      // chaining transfers from it re-read into the same place every time.
      cpu->regs.HL.set_pair16((uint16_t)(buffer + blocks_read * 512));
      // A short transfer is an ERROR, not a success that moved fewer sectors.
      // Every end-of-media and I/O path above just breaks out of the loop, and
      // until 2026-09-05 the status stayed HBR_SUCCESS - so a read that
      // transferred nothing came back as a successful read, and CP/M used
      // whatever was already in its sector buffer as though it were the sector
      // it asked for. Stale data presented as real data is the worst failure
      // mode available to a disk driver.
      //
      // RomWBW's own drivers signal it: Source/HBIOS/md.asm MD_IOSETUP3 does
      // "OR $FF ; SIGNAL ERROR" and MD_RDSEC turns that into
      // "LD A,ERR_IO ; SIGNAL IO ERROR".
      if (blocks_read < count) {
        result = HBR_IO;
      }
      break;
    }

    case HBF_DIOWRITE: {
      // Write sectors using current_lba (set by DIOSEEK)
      // Input: BC=Function/Unit, HL=Buffer Address, D=Buffer Bank (0x80=use current), E=Block Count
      // Output: A=Result, E=Blocks Written

      if (!is_memdisk && !is_harddisk) {
        // No device at this unit - return error with 0 blocks written
        cpu->regs.DE.set_low(0);
        result = HBR_NOUNIT;
        break;
      }

      uint16_t buffer = cpu->regs.HL.get_pair16();
      uint8_t buffer_bank = cpu->regs.DE.get_high();
      uint8_t count = cpu->regs.DE.get_low();
      uint8_t blocks_written = 0;

      // Helper lambda to read byte from correct bank
      // When buffer_bank has bit 7 set (RAM bank 0x80-0x8F), use bank-aware read
      // Otherwise use fetch_mem() which respects current bank
      auto read_from_bank = [&](uint16_t addr) -> uint8_t {
        if (buffer_bank & 0x80) {
          // Bank-aware read (RAM bank specified)
          if (addr >= 0x8000) {
            // Common area - read from bank 0x8F
            return memory->read_bank(0x8F, addr - 0x8000);
          } else {
            return memory->read_bank(buffer_bank, addr);
          }
        } else {
          // Use current bank (ROM bank or 0 specified)
          return memory->fetch_mem(addr);
        }
      };

      if (is_memdisk) {
        // Memory disk write - write to RAM bank memory
        MemDiskState& md = md_disks[md_unit];

        // Check if ROM disk (read-only)
        if (md.is_rom) {
          result = HBR_READONLY;
          cpu->regs.DE.set_low(0);
          break;
        }

        const uint32_t sectors_per_bank = 64;

        for (int s = 0; s < count; s++) {
          if (md.current_lba >= md.total_sectors()) {
            break;
          }

          uint32_t bank_offset = md.current_lba / sectors_per_bank;
          uint32_t sector_in_bank = md.current_lba % sectors_per_bank;
          uint8_t dst_bank = (uint8_t)(md.start_bank + bank_offset);
          uint16_t dst_offset = (uint16_t)(sector_in_bank * 512);

          // Copy 512 bytes from buffer to bank memory
          for (int j = 0; j < 512; j++) {
            uint8_t byte = read_from_bank((uint16_t)(buffer + s * 512 + j));
            memory->write_bank(dst_bank, (uint16_t)(dst_offset + j), byte);
          }

          md.current_lba++;
          blocks_written++;
        }
      } else {
        // Hard disk write
        uint32_t lba = disks[hd_unit].current_lba;

        // Check for manifest disk write warning (first write triggers once per session)
        if (disks[hd_unit].is_manifest && !disks[hd_unit].warning_suppressed && !manifest_warning_shown) {
          manifest_write_pending = true;
        }

        if (disks[hd_unit].file_backed && disks[hd_unit].handle) {
          uint8_t sector_buf[512];
          for (int s = 0; s < count; s++) {
            uint64_t offset = ((uint64_t)lba + s) * 512;
            // THE MEDIUM DOES NOT GROW.  A write past the end of the disk is an
            // I/O error on real hardware - md.asm range-checks against the
            // media size (MD_IOSETUP, "CP RAMD_BNKS" -> "OR $FF ; SIGNAL
            // ERROR"), and the hardware drivers pass the device's out-of-range
            // answer back as ERR_IO.
            //
            // This had no bound at all on the file-backed path: emu_disk_write
            // is an fseek-and-write, so a guest seeking past the end and
            // writing EXTENDED THE HOST FILE, quietly turning a 49MB image into
            // whatever the guest asked for and reporting success. The in-memory
            // path a few lines below has always had the check; this is the same
            // one.
            if ((offset + 512) > (uint64_t)disks[hd_unit].total_sectors() * 512) {
              emu_error("[HBIOS DIOWRITE] HD%d: write past end of medium "
                        "(LBA %llu, %u sectors)\n", hd_unit,
                        (unsigned long long)(lba + s),
                        disks[hd_unit].total_sectors());
              result = HBR_IO;
              break;
            }
            for (size_t i = 0; i < 512; i++) {
              sector_buf[i] = read_from_bank((uint16_t)(buffer + s * 512 + i));
            }
            size_t written = emu_disk_write((emu_disk_handle)disks[hd_unit].handle,
                                            (size_t)offset, sector_buf, 512);
            if (written != 512) {
              // emu_disk_write is buffered fwrite, so ENOSPC can also defer to
              // the flush below; emu_disk_flush returns void, so flush-time
              // failures remain unreported.
              emu_error("[HBIOS DIOWRITE] HD%d short write at LBA %u (%zu/512 bytes) - host disk full or I/O error\n",
                        hd_unit, (unsigned)(lba + s), written);
              result = HBR_IO;
              break;
            }
            blocks_written++;
          }
          emu_disk_flush((emu_disk_handle)disks[hd_unit].handle);
        } else if (!disks[hd_unit].data.empty()) {
          for (int s = 0; s < count; s++) {
            uint64_t offset = ((uint64_t)lba + s) * 512;
            if (offset + 512 > disks[hd_unit].data.size()) {
              // Past end of image: error, like the file-backed short-write
              // path - CBIOS checks only A, so a bare partial count in E
              // would be silent data loss. Never grow the image - the size
              // is what format auto-detect and the download buffer rely on,
              // and lba is guest-controlled.
              emu_error("[HBIOS DIOWRITE] HD%d write past end of image at LBA %u (size %zu)\n",
                        hd_unit, (unsigned)(lba + s), disks[hd_unit].data.size());
              result = HBR_IO;
              break;
            }
            for (size_t i = 0; i < 512; i++) {
              disks[hd_unit].data[(size_t)offset + i] = read_from_bank((uint16_t)(buffer + s * 512 + i));
            }
            blocks_written++;
          }
          // Mark disk as dirty for persistence
          if (blocks_written > 0) {
            disks[hd_unit].dirty = true;
          }
        } else {
          emu_fatal("[HBIOS DIOWRITE] HD%d is_open but no data (file_backed=%d, data.empty=%d)\n",
                    hd_unit, disks[hd_unit].file_backed, disks[hd_unit].data.empty());
        }

        // Update current_lba for next sequential access
        disks[hd_unit].current_lba += blocks_written;
      }

      cpu->regs.DE.set_low(blocks_written);
      cpu->regs.HL.set_pair16((uint16_t)(buffer + blocks_written * 512));
      // As for the read above: fewer sectors written than asked for is ERR_IO,
      // not success. A write silently dropped is how a guest loses work and
      // never learns it did.
      if (blocks_written < count) {
        result = HBR_IO;
      }

      // Mark that disk writes occurred for periodic flush
      if (blocks_written > 0) {
        disk_writes_pending = true;
      }
      break;
    }

    case HBF_DIOVERIFY:
    case HBF_DIOFORMAT:
      // Format track - not supported in emulator
      result = HBR_NOTIMPL;
      break;

    case HBF_DIODEVICE: {
      // Disk device info report
      // Returns: D=device type, E=device number within type, C=attributes
      // Device types: 0x00=MD (memory disk), 0x09=HDSK, 0xFF=no device
      // Attributes: bit 5=high capacity (enables multiple slices), bit 6=removable
      uint8_t dev_attr = 0x00;
      if (is_memdisk) {
        cpu->regs.DE.set_high(0x00);  // DIODEV_MD (memory disk)
        cpu->regs.DE.set_low(md_unit); // Device number (0=MD0, 1=MD1)
        // Attribute byte, taken verbatim from RomWBW's own memory-disk driver
        // rather than assembled bit by bit: Source/HBIOS/md.asm:24-26 defines
        //   MD_AROM .EQU %00010100   (0x14)  ROM
        //   MD_ARAM .EQU %00010101   (0x15)  RAM
        //   MD_AFSH .EQU %00010110   (0x16)  FLASH
        // The low nibble is the device class - RomWBW reads it to decide whether
        // to size a unit in KB or MB (Source/HBIOS/invntdev.asm PS_PRTDC) - and
        // bit 4 says LBA capable, which is what CBIOS requires before it will
        // put a unit in the drive map at all.
        //
        // This was 0x00 until 2026-09-05, so both memory disks reported as hard
        // disks; then 0x04/0x05, which fixed the sizing and still dropped bit 4,
        // so ASSIGN /B= silently rebuilt the drive map without them. Use the
        // driver's constants and neither half can be got wrong on its own.
        dev_attr = md_disks[md_unit].is_rom ? 0x14 : 0x15;
        // H := 0 "DRIVER HAS NO MODES", L := 0 "NO BASE I/O ADDRESS"
        // (md.asm:244-245).  Both drivers write these on every call; neither
        // leaves them, which is what this did - so a caller printing the I/O
        // base printed whatever it happened to have in HL.
        cpu->regs.HL.set_pair16(0x0000);
      } else if (is_harddisk) {
        cpu->regs.DE.set_high(0x09);  // DIODEV_HDSK (hard disk)
        cpu->regs.DE.set_low(hd_unit); // Device number within type
        // %00110000 - non-removable hard disk, LBA capable. RomWBW's own value,
        // Source/HBIOS/hdsk.asm:192 "LD C,%00110000 ; C := ATTRIBUTES,
        // NON-REMOVABLE HARD DISK". Bit 4 was missing here too.
        dev_attr = 0x30;
        // H := 0 (no modes), L := HDSK_IO.  hdsk.asm:193-194 is
        // "LD H,0 ; DRIVER HAS NO MODES / LD L,HDSK_IO", and HDSK_IO is $FD
        // (hdsk.asm:8) - the port the emulator's own disk device answers on.
        cpu->regs.HL.set_pair16(0x00FD);
      } else {
        // No device at this unit - return error, don't crash
        cpu->regs.DE.set_high(0xFF);  // No device
        cpu->regs.DE.set_low(0xFF);
        result = HBR_NOUNIT;
        if (debug_log) {
          emu_log("[HBIOS DIODEVICE] Unit %d: no device found\n", raw_unit);
        }
        break;
      }
      cpu->regs.BC.set_low(dev_attr);  // C = device attributes
      if (debug_log) {
        emu_log("[HBIOS DIODEVICE] Unit %d: type=0x%02X num=%d attr=0x%02X\n",
                raw_unit, cpu->regs.DE.get_high(), cpu->regs.DE.get_low(), dev_attr);
      }
      break;
    }

    case HBF_DIOMEDIA: {
      // Disk media report - return media type
      // D is part of the answer and was being left as the caller passed it.
      if (is_memdisk) {
        cpu->regs.DE.set_high(0);
        cpu->regs.DE.set_low(md_disks[md_unit].is_rom ? MID_MDROM : MID_MDRAM);
      } else if (is_harddisk) {
        cpu->regs.DE.set_high(0);
        cpu->regs.DE.set_low(MID_HD);  // Hard disk media
      } else {
        // No device at this unit - return error
        cpu->regs.DE.set_low(0xFF);
        result = HBR_NOUNIT;
      }
      break;
    }

    case HBF_DIODEFMED:
      // Define disk media - not supported in emulator
      result = HBR_NOTIMPL;
      break;

    case HBF_DIOCAP: {
      // Get capacity in sectors, returned as DE:HL - DE is the HIGH word and HL
      // the LOW word, the same order HBF_DIOSEEK above documents and reads back.
      //
      // These two words were the wrong way round until 2026-09-05, and nothing
      // noticed for as long as the only caller was code we replace: under
      // RomWBW 3.5.1 the device inventory ran inside the HBIOS bank, which our
      // proxy substitutes wholesale. RomWBW 3.6.0 moved it into a ROM app
      // (Source/HBIOS/invntdev.asm), so it now runs against this dispatcher and
      // reads what we actually return - "RST 08 ; DE:HL := BLOCKS", then
      // RES 7,D to clear the LBA bit, then an 11-bit shift to megabytes.
      //
      // Swapped, the published 49MB combo (100,352 sectors, 0x00018800) came
      // back as 0x88000001, which after RES 7,D and >>11 prints as 65536MB. The
      // 256KB RAM disk printed 16384MB and the 384KB ROM disk 24576MB.
      // COPYSL.COM reads the same value from CP/M, so this was never confined
      // to a diagnostic screen.
      if (is_memdisk) {
        uint32_t sectors = md_disks[md_unit].total_sectors();
        cpu->regs.DE.set_pair16((sectors >> 16) & 0xFFFF);
        cpu->regs.HL.set_pair16(sectors & 0xFFFF);
        cpu->regs.BC.set_pair16(512);  // BC := block size, as the drivers do
      } else if (is_harddisk) {
        uint32_t sectors = disks[hd_unit].total_sectors();
        cpu->regs.DE.set_pair16((sectors >> 16) & 0xFFFF);
        cpu->regs.HL.set_pair16(sectors & 0xFFFF);
        cpu->regs.BC.set_pair16(512);
      } else {
        // No device at this unit - return 0 capacity and error
        cpu->regs.DE.set_pair16(0);
        cpu->regs.HL.set_pair16(0);
        result = HBR_NOUNIT;
      }
      break;
    }

    case HBF_DIOGEOM: {
      // Geometry. RomWBW's LBA drivers report a synthetic CHS derived from the
      // real capacity and set the LBA flag, rather than a fixed shape: ide.asm
      // and hdsk.asm build it from their own capacity with 16 heads and 63
      // sectors per track. Returning a constant 63/16/255 described a 127MB
      // disk whatever was actually attached, and answered identically for a
      // unit with nothing on it.
      if (!is_memdisk && !is_harddisk) {
        result = HBR_NOUNIT;
        break;
      }
      // The synthetic CHS is 16 HEADS BY 16 SECTORS, not 16 by 63, and each
      // value has its own register.  hdsk.asm:176-185 is the whole of it:
      //
      //     CALL HDSK_CAP      ; TOTAL BLOCKS IN DE:HL, BLOCK SIZE TO BC
      //     LD   L,H           ; DIVIDE BY 256 FOR # TRACKS
      //     LD   H,E           ; ... HIGH BYTE DISCARDED, RESULT IN HL
      //     LD   D,$80 | 16    ; HEADS / CYL = 16, SET LBA BIT
      //     LD   E,16          ; SECTORS / TRACK = 16
      //
      // So HL := cylinders = blocks / 256, D := heads with bit 7 SET to say the
      // device is LBA-capable, E := sectors per track, and BC is left holding
      // the 512 that HDSK_CAP put there - the block size, not a sector count.
      //
      // This put 63 in C, 16 in D with no LBA bit, and the low byte of the
      // cylinder count in E.  A guest reading E as sectors-per-track got a
      // number that changed with the size of the disk, and one testing bit 7 of
      // D concluded the device could not do LBA.
      // A MEMORY DISK HAS A DIFFERENT SHAPE.  md.asm:222-236 says so in its own
      // comment - "RAM/ROM DISKS ALLOW CHS STYLE ACCESS BY EMULATING A DISK
      // DEVICE WITH 1 HEAD AND 16 SECTORS / TRACK" - and divides the capacity
      // by 16 rather than by 256.  The two geometries are each self-consistent:
      // heads * sectors * cylinders comes back out as the capacity, which is
      // the property a caller doing CHS arithmetic depends on.
      uint32_t sectors;
      uint8_t heads;
      if (is_memdisk) {
        sectors = md_disks[md_unit].total_sectors();
        heads = 1;
      } else {
        sectors = disks[hd_unit].total_sectors();
        heads = 16;
      }
      uint32_t cyls = sectors / (16u * heads);
      if (cyls == 0) cyls = 1;
      if (cyls > 0xFFFF) cyls = 0xFFFF;
      cpu->regs.BC.set_pair16(512);                    // BC := block size
      cpu->regs.DE.set_high((uint8_t)(0x80 | heads));  // D  := heads, LBA bit
      cpu->regs.DE.set_low(16);                        // E  := sectors / track
      cpu->regs.HL.set_pair16((uint16_t)cyls);         // HL := cylinders
      break;
    }

    default:
      // As for CIO above: report it, do not abort. BF_DIOVERIFY (0x15) is the
      // one a real RomWBW guest could plausibly reach - every stock driver
      // stubs it to ERR_NOTIMPL rather than omitting it - and killing the
      // emulator would have been a far worse answer than the error RomWBW
      // itself gives.
      emu_error("[HBIOS DIO] Unhandled function 0x%02X (unit=%d is_md=%d is_hd=%d "
                "hd_unit=%d) - returning HBR_NOFUNC\n",
                func, raw_unit, is_memdisk, is_harddisk, hd_unit);
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// Real-Time Clock (RTC)
//=============================================================================

// Days since 1970-03-01, by the civil-from-days algorithm.  Plain arithmetic
// rather than mktime(), which is local-time and DST-dependent: what is wanted
// here is the difference between two calendar readings, not between two
// instants in somebody's timezone.
static long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

static long civil_seconds(const emu_time& t) {
  return days_from_civil(t.year, t.month, t.day) * 86400L
       + (long)t.hour * 3600L + (long)t.minute * 60L + (long)t.second;
}

long HBIOSDispatch::secondsBetween(const emu_time& a, const emu_time& b) {
  return civil_seconds(b) - civil_seconds(a);
}

void HBIOSDispatch::applyRtcOffset(emu_time* t) const {
  if (!t || rtc_offset_seconds == 0) return;

  long secs = civil_seconds(*t) + rtc_offset_seconds;
  long days = secs / 86400L;
  long rem = secs % 86400L;
  if (rem < 0) { rem += 86400L; days -= 1; }

  t->hour = (int)(rem / 3600);
  t->minute = (int)((rem % 3600) / 60);
  t->second = (int)(rem % 60);

  // civil_from_days, the inverse of the above.
  long z = days + 719468L;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  t->year = (int)(y + (m <= 2));
  t->month = (int)m;
  t->day = (int)d;
  // 1970-01-01 was a Thursday.
  long wd = (days + 4) % 7;
  if (wd < 0) wd += 7;
  t->weekday = (int)wd;
}


void HBIOSDispatch::handleRTC() {
  if (!cpu || !memory) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t result = HBR_SUCCESS;

  switch (func) {
    case HBF_RTCGETTIM: {
      // Get time into buffer at HL
      uint16_t buffer = cpu->regs.HL.get_pair16();
      emu_time t;
      emu_get_time(&t);
      applyRtcOffset(&t);

      // RomWBW format: YY MM DD HH MM SS (BCD)
      auto to_bcd = [](int v) -> uint8_t {
        return (uint8_t)(((v / 10) << 4) | (v % 10));
      };

      memory->store_mem(buffer + 0, to_bcd(t.year % 100));
      memory->store_mem(buffer + 1, to_bcd(t.month));
      memory->store_mem(buffer + 2, to_bcd(t.day));
      memory->store_mem(buffer + 3, to_bcd(t.hour));
      memory->store_mem(buffer + 4, to_bcd(t.minute));
      memory->store_mem(buffer + 5, to_bcd(t.second));
      break;
    }

    case HBF_RTCSETTIM: {
      // Set time from the buffer at HL, same BCD layout GETTIM writes.
      //
      // This was a comment and a break: it answered HBR_SUCCESS and threw the
      // time away, so a guest running DATE SET watched it take and then read
      // the host clock back.  There is no chip to write, so what is stored is
      // the OFFSET from the host clock, which keeps the clock running forward
      // afterwards instead of freezing it at the moment it was set.
      //
      // A value that is not valid BCD, or not a real date, is rejected with
      // ERR_NOTIMPL's neighbour ERR_INVALID rather than accepted - every
      // RomWBW driver returns non-zero if the write did not take
      // (Source/HBIOS/dsrtc.asm, DSRTC_SETTIM).
      uint16_t buffer = cpu->regs.HL.get_pair16();
      auto from_bcd = [](uint8_t v) -> int {
        return ((v >> 4) & 0x0F) * 10 + (v & 0x0F);
      };
      auto bcd_ok = [](uint8_t v) -> bool {
        return ((v >> 4) & 0x0F) <= 9 && (v & 0x0F) <= 9;
      };
      uint8_t raw[6];
      bool ok = true;
      for (int i = 0; i < 6; i++) {
        raw[i] = memory->fetch_mem((uint16_t)(buffer + i));
        if (!bcd_ok(raw[i])) ok = false;
      }
      if (ok) {
        emu_time want;
        want.year = 2000 + from_bcd(raw[0]);
        want.month = from_bcd(raw[1]);
        want.day = from_bcd(raw[2]);
        want.hour = from_bcd(raw[3]);
        want.minute = from_bcd(raw[4]);
        want.second = from_bcd(raw[5]);
        want.weekday = 0;
        if (want.month < 1 || want.month > 12 || want.day < 1 || want.day > 31 ||
            want.hour > 23 || want.minute > 59 || want.second > 59) {
          ok = false;
        } else {
          emu_time now;
          emu_get_time(&now);
          rtc_offset_seconds = secondsBetween(now, want);
        }
      }
      // ERR_RANGE, not a generic failure: the buffer held something that is not
      // a date. RomWBW's own drivers return non-zero when the write does not
      // take; which non-zero is driver-specific, and this is the closest of
      // hbios.inc's codes to "that is not a time".
      if (!ok) result = HBR_RANGE;
      break;
    }

    case HBF_RTCGETBYT: {
      // Get NVRAM byte by index
      // Input: C = byte index (0-4 for NVRAM switches)
      // Output: E = byte value
      settleNvramChecksum();
      uint8_t idx = cpu->regs.BC.get_low();
      uint8_t value = 0;
      if (idx < NVRAM_SIZE) {
        value = nvram_switches[idx];
      }
      cpu->regs.DE.set_low(value);
      if (debug_log) {
        emu_log("[RTC GETBYT] idx=%d -> value=0x%02X ('%c')\n",
                idx, value, (value >= 0x20 && value < 0x7F) ? value : '.');
      }
      break;
    }

    case HBF_RTCSETBYT: {
      // Set NVRAM byte by index
      // Input: C = byte index, E = byte value
      uint8_t idx = cpu->regs.BC.get_low();
      uint8_t value = cpu->regs.DE.get_low();
      if (idx < NVRAM_SIZE) {
        // STORE EXACTLY WHAT WAS GIVEN, and nothing else.
        //
        // This recomputed the checksum into index 4 whenever index 0-3 was
        // written, which is a side effect the device does not have: a write to
        // NVRAM index 1 changes index 1.  HBIOS recomputes the checksum in
        // NVSW_UPDATE and writes it as a FIFTH, SEPARATE RTCSETBYT call
        // (hbios.asm:8156-8174), so a guest writing the block byte by byte sees
        // an inconsistent checksum until it writes the last one - which is
        // exactly how SYSCONF detects a half-written block after a reset.
        // Recomputing it here hid that state and made every partial write look
        // complete.
        //
        // The places that own the switch block do the recompute instead: the
        // BF_SYSSET/SWITCH handler, which is this dispatcher's NVSW_UPDATE, and
        // the --boot/persistence path.
        nvram_switches[idx] = value;
        if (debug_log) {
          emu_log("[RTC SETBYT] idx=%d <- value=0x%02X ('%c')\n",
                  idx, value, (value >= 0x20 && value < 0x7F) ? value : '.');
        }
      } else {
        if (debug_log) {
          emu_log("[RTC SETBYT] idx=%d out of range (max %d)\n", idx, NVRAM_SIZE - 1);
        }
      }
      break;
    }

    case HBF_RTCGETBLK:
    case HBF_RTCSETBLK:
      // DECLINED, because the clock this claims to be declines.
      //
      // Eleven of the twelve 3.6.0 RTC drivers answer both with
      // SYSCHKERR(ERR_NOTIMPL) and touch nothing - simrtc.asm:86 among them,
      // and simrtc is the driver BF_RTCDEVICE now names.  The twelfth,
      // ds1501rtc.asm:326-350, moves the whole 256-byte NVRAM with LD B,0/INIR.
      //
      // This moved FIVE bytes - the switch block - and reported success, which
      // is neither: a guest asking for the NVRAM block got the four switch
      // bytes and a checksum, in a call whose contract is the whole device's
      // memory. If block access is ever wanted here, the thing to model is the
      // whole NVRAM, not the part that happens to be modelled already.
      result = HBR_NOTIMPL;
      break;

    case HBF_RTCDEVICE: {
      // D := device type, E := physical device number, H := unit mode,
      // L := base I/O address, and C is left alone - every driver writes those
      // four and none of them touches C.  dsrtc.asm:413-419 is the shape:
      //
      //     LD D,RTCDEV_DS / LD E,0 / LD H,DSRTCMODE / LD L,DSRTC_IO / XOR A
      //
      // This put a device type in C - which is the attribute byte, not the type
      // - and zeroed DE, so the type read as 0 (RTCDEV_DS, a DS1302) and H and
      // L came back holding the caller's own register.  $40 is not in the
      // RTCDEV_* table at all (hbios.inc:435-442 runs $00 to $07).
      //
      // RTCDEV_SIMH ($02) is the honest entry: it is the simulator's clock, and
      // it is what a guest can act on.  No modes and no I/O port, so H = L = 0.
      cpu->regs.DE.set_high(0x02);   // D := RTCDEV_SIMH
      cpu->regs.DE.set_low(0x00);    // E := device number
      cpu->regs.HL.set_pair16(0x0000);  // H := no modes, L := no I/O base
      break;
    }

    case HBF_RTCGETALM:
    case HBF_RTCSETALM:
      // DEFINED, AND DECLINED - which is a different answer from "no such
      // function".  Every 3.6.0 driver answers these two with
      // SYSCHKERR(ERR_NOTIMPL) = -2 (dsrtc.asm:299-302 and its ten siblings),
      // and keeps ERR_NOFUNC = -3 for a subfunction outside 0-8.  They were
      // reaching the default arm below and answering -3, so a guest probing for
      // alarm support could not tell "this clock has no alarm" from "that is
      // not a function".
      result = HBR_NOTIMPL;
      break;

    default:
      // Report it rather than returning the caller's own registers with a
      // success status.
      emu_log("[HBIOS RTC] Unhandled function 0x%02X\n", func);
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// System Functions (SYS)
//=============================================================================

void HBIOSDispatch::handleSYS() {
  if (!cpu || !memory) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t subfunc = cpu->regs.BC.get_low();
  uint8_t result = HBR_SUCCESS;

  switch (func) {
    case HBF_SYSRESET: {
      // System reset - C register: 0x01 = warm boot, 0x02 = cold boot
      uint8_t reset_type = subfunc;  // subfunc is C register
      // Always log SYSRESET since it causes reboot
      if (debug_log) debug_log("[HBIOS SYSRESET] reset_type=0x%02X\n", reset_type);

      // Subfunction 0x00 is the INTERNAL reset, and it is not a reboot: it
      // releases heap the drivers are not using, and RomWBW's warm reset does it
      // first (SYS_RESWARM opens "CALL SYS_RESINT"). It did nothing here until
      // 2026-09-06, so the heap only ever grew.
      //
      // That matters because CBIOS calls it on EVERY OS boot - cbios.asm
      // "LD BC,BC_SYSRES_INT / RST 08" - and then allocates the CCP (0x800) and
      // more. Across one emulator session the allocations accumulated until a
      // boot died with "*** Insufficient HBIOS Heap Memory ***" and a CBIOS
      // PANIC, which looks like a corrupt disk rather than a leak. A user
      // hopping between the six slices on hd1k_combo reaches it without
      // restarting the emulator.
      if (reset_type == 0x00 || reset_type == 0x01 || reset_type == 0x02) {
        heap_ptr = heap_curb;
      }
      if (reset_type == 0x01 || reset_type == 0x02) {
        // Call the reset callback if set
        if (reset_callback) {
          reset_callback(reset_type);
          // Don't call doRet() - the callback sets PC directly
          return;
        }
      }
      // C MUST BE 0, 1, 2 OR 3.  SYS_RESET compares against the four
      // BF_SYSRES_* codes and falls through to ERR_NOFUNC for anything else
      // (hbios.asm:5721-5732, hbios.inc:108-111).  Every value 4-255 was
      // reported as a successful reset here.
      //
      // $03 is the user reset: RomWBW resets the active video display through
      // TERM_RESET and returns with HL still holding the vector the caller
      // passed.  There is no addressable display here - the video is the host
      // terminal - so it is accepted and does nothing to the screen, which is
      // what a driver with no adjustable display does.
      if (reset_type > 0x03) {
        result = HBR_NOFUNC;
      }
      break;
    }

    case HBF_SYSVER: {
      // Get HBIOS version - read from the loaded ROM, because the CBIOS in a
      // boot slice compares this against its own build and prints
      // "HBIOS/CBIOS Version Mismatch" when they differ. Answering from a
      // compile-time constant is what made one binary able to boot only one
      // RomWBW release; answering from the ROM is what lets it boot any
      // release it was built to run, and still warn on a mismatched disk.
      // Format: D=major/minor (high/low nibble), E=update/patch (high/low nibble)
      emu_romwbw_release release = {0, 0};
      emu_romwbw_release_loaded(memory, &release);
      cpu->regs.DE.set_pair16((uint16_t)((release.ver << 8) | release.upd));
      cpu->regs.HL.set_low(0x01);  // Platform ID = SBC
      break;
    }

    case HBF_SYSSETBNK: {
      // Set current bank
      // Input: C = bank ID to set
      // Output: C = previous bank ID
      uint8_t new_bank = cpu->regs.BC.get_low();
      uint8_t prev_bank = memory->get_current_bank();

      // When switching to a RAM bank for the first time, initialize it
      // Uses shared emu_init_ram_bank() which copies page zero, HCB, and CBIOS stamp
      if ((new_bank & 0x80) && !(new_bank & 0x70)) {  // RAM bank 0x80-0x8F
        emu_init_ram_bank(memory, new_bank, &initialized_ram_banks);
      }

      memory->select_bank(new_bank);

      // Update PMGMT_CURBNK at 0xFFE0 for RAM banks only
      // This is needed so code that reads current bank (like CP/M 3's xbnkmov) works correctly
      // We only update for RAM banks to avoid interfering with ROM bank operations
      if (new_bank & 0x80) {
        memory->write_bank(0x8F, 0x7FE0, new_bank);  // 0xFFE0 in common bank (0x8F)
      }

      cur_bank = new_bank;
      cpu->regs.BC.set_low(prev_bank);  // Return previous bank in C
      if (debug_log) {
        emu_log("[HBIOS] SYSSETBNK bank=0x%02X (prev=0x%02X)\n", new_bank, prev_bank);
      }
      break;
    }

    case HBF_SYSGETBNK: {
      // C, not L. SYS_GETBNK is "LD A,(HB_INVBNK) / LD C,A / XOR A / RET".
      cpu->regs.BC.set_low(memory->get_current_bank());
      break;
    }

    case HBF_SYSSETCPY: {
      // Set bank copy parameters (for subsequent SYSBNKCPY call)
      // Input: D = destination bank, E = source bank, HL = byte count
      // This just stores parameters, actual copy happens in SYSBNKCPY
      bnkcpy_dst_bank = cpu->regs.DE.get_high();
      bnkcpy_src_bank = cpu->regs.DE.get_low();
      bnkcpy_count = cpu->regs.HL.get_pair16();
      break;
    }

    case HBF_SYSBNKCPY: {
      // Execute bank-to-bank memory copy using params from SYSSETCPY
      // Input: HL = source address, DE = destination address
      // Uses: bnkcpy_src_bank, bnkcpy_dst_bank, bnkcpy_count from SYSSETCPY
      uint16_t src_addr = cpu->regs.HL.get_pair16();
      uint16_t dst_addr = cpu->regs.DE.get_pair16();
      uint16_t count = bnkcpy_count;

      if (count > 0) {
        for (uint16_t i = 0; i < count; i++) {
          // Handle common area (0x8000-0xFFFF) - always bank 0x8F
          uint8_t actual_src_bank = bnkcpy_src_bank;
          uint8_t actual_dst_bank = bnkcpy_dst_bank;
          uint16_t actual_src_addr = src_addr + i;
          uint16_t actual_dst_addr = dst_addr + i;

          // Source address in common area?
          if (actual_src_addr >= 0x8000) {
            actual_src_bank = 0x8F;
            actual_src_addr -= 0x8000;
          }

          // Destination address in common area?
          if (actual_dst_addr >= 0x8000) {
            actual_dst_bank = 0x8F;
            actual_dst_addr -= 0x8000;
          }

          uint8_t byte = memory->read_bank(actual_src_bank, actual_src_addr);
          memory->write_bank(actual_dst_bank, actual_dst_addr, byte);
        }
      }

      // HL and DE come back ADVANCED PAST THE BLOCK, and that is the whole
      // point of the function's documented usage:
      //
      //   "it is not necessary to call SYSSETCPY prior to subsequent calls to
      //    SYSBNKCPY if the source/destination banks and copy length do not
      //    [change]"   - Source/Doc/SystemGuide.md, Function 0xF5
      //
      // whose table reads "HL: New Source Address" and "DE: New Destination
      // Address". A caller walking a large region with one SYSSETCPY and a run
      // of SYSBNKCPYs is the intended shape, and this returned HL and DE
      // unchanged - so every call after the first copied the SAME block again,
      // for ever, and the region past the first `count` bytes was never
      // written. Found 2026-09-18 by reading the manual against this loop.
      cpu->regs.HL.set_pair16((uint16_t)(src_addr + count));
      cpu->regs.DE.set_pair16((uint16_t)(dst_addr + count));
      break;
    }

    case HBF_SYSALLOC: {
      // Allocate memory from HBIOS heap
      // Input: HL = size requested
      // Output: A = result, HL = address of allocated block (or 0 on failure)
      // Heap is in bank 0x80 starting after HCB (0x0200) up to 0x8000
      uint16_t size = cpu->regs.HL.get_pair16();
      static int alloc_count = 0;
      alloc_count++;

      // Log first allocations and failures to debug heap issues
      if (debug_log && (alloc_count <= 20)) {
        debug_log("[HBIOS SYSALLOC #%d] REQUEST: size=0x%04X (%u) heap_ptr=0x%04X free=0x%04X\n",
                alloc_count, size, size, heap_ptr, heap_end - heap_ptr);
      }

      // EVERY ALLOCATION COSTS size + 4.  HB_ALLOC puts a four-byte header in
      // front of the block - "A 4 BYTE HEADER IS PLACED IN FRONT OF THE
      // ALLOCATED MEMORY" (hbios.asm:7536-7541) - holding the requested size
      // and the caller's return address, and hands back a pointer PAST it.
      // Charging only `size` meant this reported more free heap than RomWBW
      // has, so a guest sizing its allocations against BF_SYSGET/MEMINFO could
      // fit things here that will not fit on real hardware.
      const uint16_t ALLOC_HDR = 4;
      if ((uint32_t)heap_ptr + size + ALLOC_HDR <= heap_end) {
        uint16_t addr = (uint16_t)(heap_ptr + ALLOC_HDR);
        // The header itself: the size word, then the reference address, which
        // has no meaningful value here.
        if (memory) {
          memory->write_bank(0x80, heap_ptr, (uint8_t)(size & 0xFF));
          memory->write_bank(0x80, (uint16_t)(heap_ptr + 1), (uint8_t)(size >> 8));
          memory->write_bank(0x80, (uint16_t)(heap_ptr + 2), 0);
          memory->write_bank(0x80, (uint16_t)(heap_ptr + 3), 0);
        }
        heap_ptr = (uint16_t)(heap_ptr + size + ALLOC_HDR);
        cpu->regs.HL.set_pair16(addr);
        // Set flags: Z=1 (success), C=0 (no error)
        cpu->regs.AF.set_low(qkz80_cpu_flags::Z);
        if (debug_log) debug_log("[HBIOS SYSALLOC] SUCCESS: allocated 0x%04X, new heap_ptr=0x%04X\n", addr, heap_ptr);
      } else {
        // Out of heap memory - always log failures
        emu_log("[HBIOS SYSALLOC] FAILED: size=%u (0x%04X) exceeds available heap (ptr=0x%04X end=0x%04X)\n",
                size, size, heap_ptr, heap_end);
        cpu->regs.HL.set_pair16(0);
        // Set flags: Z=0 (failure), C=1 (error)
        cpu->regs.AF.set_low(qkz80_cpu_flags::CY);
        result = HBR_NOMEM;
      }
      break;
    }

    case HBF_SYSFREE: {
      // Free memory from HBIOS heap
      // Input: HL = address of block to free.
      // RomWBW does not implement this either - SYS_FREE is literally
      // "SYSCHKERR(ERR_NOTIMPL) / RET" - so answering success was a claim the
      // release being impersonated does not make, and a caller that frees and
      // then relies on the memory being reusable would be misled.
      if (debug_log) {
        emu_log("[HBIOS SYSFREE] addr=0x%04X (not implemented, as in RomWBW)\n",
                cpu->regs.HL.get_pair16());
      }
      result = HBR_NOTIMPL;
      break;
    }

    case HBF_SYSGET: {
      // Get system info - subfunc in C
      switch (subfunc) {
        case SYSGET_CIOCNT:
          // Number of CIO devices
          cpu->regs.DE.set_low(CIO_UNIT_COUNT);
          break;

        case SYSGET_DIOCNT: {
          // Number of DIO devices (MD + HD)
          int count = 0;
          // Count memory disks (MD0, MD1)
          for (int i = 0; i < 2; i++) {
            if (md_disks[i].is_enabled) count++;
          }
          // Count hard disks
          for (int i = 0; i < 16; i++) {
            if (disks[i].is_open) count++;
          }
          if (debug_log) debug_log("[DIOCNT] md_disks: %d,%d hd_disks: %d total=%d\n",
                  md_disks[0].is_enabled ? 1 : 0,
                  md_disks[1].is_enabled ? 1 : 0,
                  disks[0].is_open ? 1 : 0,
                  count);
          cpu->regs.DE.set_low((uint8_t)count);
          break;
        }

        case SYSGET_VDACNT:
          cpu->regs.DE.set_low(VDA_UNIT_COUNT);
          break;

        case SYSGET_SNDCNT:
          cpu->regs.DE.set_low(SND_UNIT_COUNT);
          break;

        case SYSGET_RTCCNT:
          cpu->regs.DE.set_low(RTC_UNIT_COUNT);
          break;

        case SYSGET_DSKYCNT:
          cpu->regs.DE.set_low(0);  // 0 DSKY devices
          break;

        case SYSGET_BOOTINFO:
          // D = boot unit, E = boot slice, AND L = boot bank id.  L was never
          // written, so a caller reading it got back whatever it had passed in
          // - and upstream's answer is three registers:
          //
          //     LD A,(CB_BOOTBID) / LD L,A / LD DE,(CB_BOOTVOL)
          //                                   - hbios.asm:6253-6258
          cpu->regs.DE.set_high((uint8_t)saved_boot_unit);
          cpu->regs.DE.set_low((uint8_t)saved_boot_slice);
          cpu->regs.HL.set_low((uint8_t)saved_boot_bank);
          if (debug_log) {
            emu_log("[SYSGET BOOTINFO] Returning D=%d (unit), E=%d (slice)\n",
                    saved_boot_unit, saved_boot_slice);
          }
          break;

        case SYSGET_SWITCH: {
          // Get non-volatile switch value (emulates RTC NVRAM)
          // D register contains switch number:
          //   0xFF = get NVRAM status:
          //          A=0, NZ = No RTC/NVRAM hardware
          //          A=1, NZ = RTC/NVRAM present but not initialized
          //          A='W', Z = RTC/NVRAM present and initialized
          //   1 = boot options: L=app char/slice, H=flags+unit
          //   3 = autoboot: L=flags+timeout
          uint8_t switch_num = cpu->regs.DE.get_high();
          if (switch_num == 0xFF) {
            // Return NVRAM status in A (Z flag set if 'W')
            // IMPORTANT: Must return early to preserve A register
            // (setResult would overwrite A with result code)
            uint8_t status = nvram_switches[0];
            if (status == 'W') {
              // Initialized - return 'W' with Z flag set
              cpu->regs.AF.set_high('W');
              cpu->regs.AF.set_low(cpu->regs.AF.get_low() | 0x40);  // Set Z flag
            } else {
              // Not initialized - return 1 (not 0!) with NZ flag
              // A=0 means "no NVRAM hardware", A=1 means "present but uninitialized"
              cpu->regs.AF.set_high(1);
              cpu->regs.AF.set_low(cpu->regs.AF.get_low() & ~0x40); // Clear Z flag
            }
            if (debug_log) {
              emu_log("[SYSGET_SWITCH] status check: A=0x%02X %s\n",
                      cpu->regs.AF.get_high(),
                      status == 'W' ? "INITIALIZED" : "NOT INITIALIZED");
            }
            doRet();
            return;  // Return early to preserve A register
          }

          // SWITCH_RES is the whole of the rule, and it is a TABLE (hbios.asm:
          // 6490-6521).  SWITCH_TAB is {0, 2, 0, 1, 0} and SWITCH_LEN is
          // `$ - SWITCH_TAB - 2` = 3, so:
          //
          //   D = 0   the 'W' signature byte      1 byte read, table says 0
          //   D = 1   boot options                2 bytes: L := [1], H := [2]
          //   D = 2   the second boot-options byte 1 byte
          //   D = 3   autoboot                    1 byte
          //   D >= 4  SWITCH_RES1: A := $FF, NZ   (byte 4 is the checksum)
          //
          // and the read itself is "C := (HL); if E > 1 then HL++, B := (HL)",
          // so the table entry decides the width and everything else comes back
          // with a zero high byte.
          //
          // This handled 1 and 3 only and answered every other key with HL = 0
          // and SUCCESS - so 0 and 2, which are real, read as zero, and 4-254,
          // which RomWBW rejects, read as zero too.  A caller could not tell a
          // switch that is off from one that does not exist.
          if (switch_num > SWITCH_LEN) {
            result = HBR_UNDEF;   // SWITCH_RES1: OR $FF
            break;
          }
          {
            static const uint8_t SWITCH_TAB[SWITCH_LEN + 2] = {0, 2, 0, 1, 0};
            uint8_t width = SWITCH_TAB[switch_num];
            cpu->regs.HL.set_low(nvram_switches[switch_num]);
            cpu->regs.HL.set_high(width > 1 ? nvram_switches[switch_num + 1] : 0);
            if (debug_log) {
              emu_log("[SYSGET_SWITCH] switch %u -> HL=0x%04X\n",
                      switch_num, cpu->regs.HL.get_pair16());
            }
          }
          break;
        }

        case SYSGET_TIMER: {
          // DE:HL := the 32-bit tick count, C := TICKFREQ.
          //
          // This had no case at all and fell into a default that logged, left
          // the status at success and wrote E=0. A guest could not tell that
          // from a stopped clock, and two shipped programs depend on it:
          // TIMER.COM printed a frozen value, and VGMPLAY.COM hung outright -
          // it probes the timer, spins a delay loop, reads it again and only
          // proceeds if the value moved.
          //
          // TICKFREQ is 50Hz on every RomWBW configuration in the tree, and
          // hbios.asm's Z180 timer even hard-errors if it is anything else
          // ("TICKFREQ *MUST* BE 50 FOR Z180 TIMER"). A steady_clock is the
          // right source: this is an uptime counter, not a wall clock, and it
          // must not jump when the host's date changes.
          uint32_t ticks = currentTicks();
          cpu->regs.DE.set_pair16((uint16_t)(ticks >> 16));
          cpu->regs.HL.set_pair16((uint16_t)(ticks & 0xFFFF));
          cpu->regs.BC.set_low(TICKFREQ);
          break;
        }

        case SYSGET_SECS: {
          // DE:HL := seconds, C := ticks elapsed within the current second.
          uint32_t ticks = currentTicks();
          uint32_t secs = currentSecs();
          cpu->regs.DE.set_pair16((uint16_t)(secs >> 16));
          cpu->regs.HL.set_pair16((uint16_t)(secs & 0xFFFF));
          cpu->regs.BC.set_low((uint8_t)(ticks % TICKFREQ));
          break;
        }

        case SYSGET_CPUINFO:
          // H = CPU variant, L = MHz, DE = KHz, BC = oscillator.
          // SYS_GETCPUINFO ends "LD BC,(HB_CPUOSC)"; BC was left as the caller
          // passed it.
          cpu->regs.HL.set_high(0x00);      // Z80 variant
          cpu->regs.HL.set_low(4);          // 4 MHz
          cpu->regs.DE.set_pair16(4000);    // 4000 KHz
          cpu->regs.BC.set_pair16(4000);    // oscillator, KHz
          break;

        case SYSGET_MEMINFO:
          // Memory info: D = ROM bank count, E = RAM bank count
          cpu->regs.DE.set_high(16);  // 16 ROM banks (512KB/32KB)
          cpu->regs.DE.set_low(16);   // 16 RAM banks (512KB/32KB)
          break;

        case SYSGET_BNKINFO:
          // Bank info: D = BIOS bank, E = user bank
          cpu->regs.DE.set_high(0x80);  // BIOS in bank 0x80
          cpu->regs.DE.set_low(0x8E);   // User in bank 0x8E
          break;

        case SYSGET_CPUSPD:
          // THE STOCK ROMS DO NOT ANSWER THIS.  SYS_GETCPUSPD (hbios.asm:6310)
          // has three conditional arms - a platform with switchable speed, the
          // Heath, and a Z180 - and the two ROMs this emulator is built over,
          // SBC_simh_std and RCZ80_std, match none of them, so it falls past
          // every one and returns A = $FF with NZ, leaving HL and DE alone.
          //
          // Reporting "full speed, wait states unknown" with SUCCESS was an
          // answer real hardware running this ROM does not give.
          result = HBR_UNDEF;   // OR $FF
          break;

        case SYSGET_PANEL:
          // There is no front panel here, and RomWBW has a specific answer for
          // that - SYS_GETPANEL1: "LD HL,0 / LD A,ERR_NOHW". Reporting success
          // with a zero in L instead said "a panel, with no switches set", and
          // only half of HL was written.
          cpu->regs.HL.set_pair16(0x0000);
          result = HBR_NOHW;
          break;

        case SYSGET_APPBNKS: {
          // App bank information: D = first app bank ID, E = app bank count
          // Read from HCB at CB_BIDAPP0 (0x1E0) and CB_APP_BNKS (0x1E1)
          uint8_t app_bank_start = memory->read_bank(0x80, 0x1E0);
          uint8_t app_bank_count = memory->read_bank(0x80, 0x1E1);
          // H = first bank, L = count, E = $80 (the bank size, 256 * $80 = 32KB).
          // These were being returned in D and E, so a caller following
          // SYS_GETAPPBNKS read the count out of L and got whatever it passed in.
          cpu->regs.HL.set_high(app_bank_start);
          cpu->regs.HL.set_low(app_bank_count);
          cpu->regs.DE.set_low(0x80);
          if (debug_log) {
            emu_log("[HBIOS APPBNKS] first=0x%02X count=%d\n", app_bank_start, app_bank_count);
          }
          break;
        }

        case SYSGET_DEVLIST: {
          // List available devices (custom for emulator boot menu)
          for (int i = 0; i < 16; i++) {
            if (disks[i].is_open) {
              char line[64];
              snprintf(line, sizeof(line), " %2d    HD%d:     Hard Disk\r\n", i, i);
              writeConsoleString(line);
            }
          }
          // List ROM applications
          if (!rom_apps.empty()) {
            writeConsoleString("\r\nROM Applications:\r\n");
            for (size_t i = 0; i < rom_apps.size(); i++) {
              if (rom_apps[i].is_loaded) {
                char line[64];
                snprintf(line, sizeof(line), "  %c    %s\r\n",
                        rom_apps[i].key, rom_apps[i].name.c_str());
                writeConsoleString(line);
              }
            }
          }
          break;
        }

        case 0x12: {
          // THE LEGACY SLICE CALL, AND OS BOOT LOADERS DEPEND ON IT.
          //
          // The slice calculation moved to the top level as BF_EXTSLICE ($E0) -
          // hbios.inc still records where from: "BF_EXTSLICE .EQU BF_EXT + 0 ;
          // SLICE CALCULATION (WAS BF_SYSGET_DIOMED)" - and upstream kept the
          // old spelling routed, saying why at hbios.asm:5987-5991:
          //
          //     CP  $12         ; LEFT FOR BACKWRD COMPATABILITY
          //     JP  Z,EXT_SLICE ; FUNCTION MOVED TO TOP LEVEL $E0
          //     ; REMOVING THE ABOVE CAUSED UPGRADE ISSUES FOR EARLY ADOPTERS
          //     ; SINCE OS BOOT LOADERS DEPEND ON IT. WITHOUT CAN LEAVE OS
          //     ; UNBOOTABLE AND MIGRATION HARDER - Oct 2024
          //
          // This dispatcher had no case for it and it fell to the default arm.
          // While that arm answered success with E = 0 the symptom was a loader
          // computing a slice offset of zero; now that the arm is an honest
          // ERR_NOFUNC it would be a loader that fails outright. Upstream's fix
          // is the right one either way: send it to the same code.
          //
          // Delegated by rewriting B rather than by lifting the body out of
          // handleEXT: the inputs are the same registers on both paths, so this
          // is exactly upstream's `JP EXT_SLICE`, and handleEXT does its own
          // setResult() and doRet().
          cpu->regs.BC.set_high(HBF_EXTSLICE);
          handleEXT();
          return;
        }

        default:
          // AN UNKNOWN SUBFUNCTION IS AN ERROR.  This answered E=0 and left the
          // status at HBR_SUCCESS, on the reasoning that zero is a "safe
          // default for count-type queries".  It is the opposite of safe: a
          // caller asking how many of something there are, and being told
          // "none, successfully", cannot tell that from a real zero and has no
          // way to discover the function is missing.  It is the same failure
          // this dispatcher has been bitten by in the VDA, SND and CIO groups,
          // and RomWBW's own dispatcher returns ERR_NOFUNC for a subfunction
          // it does not have.
          emu_log("[HBIOS SYSGET] Unhandled subfunction 0x%02X (DE=0x%04X HL=0x%04X)\n",
                  subfunc, cpu->regs.DE.get_pair16(), cpu->regs.HL.get_pair16());
          result = HBR_NOFUNC;
          break;
      }
      break;
    }

    case HBF_SYSPEEK: {
      // Peek byte from bank
      uint8_t bank = cpu->regs.DE.get_high();
      uint16_t addr = cpu->regs.HL.get_pair16();
      uint8_t byte;

      if (addr < 0x8000) {
        byte = memory->read_bank(bank, addr);
      } else {
        byte = memory->fetch_mem(addr);
      }
      cpu->regs.DE.set_low(byte);
      if (debug_log) {
        emu_log("[SYSPEEK] bank=0x%02X addr=0x%04X -> 0x%02X\n", bank, addr, byte);
      }
      break;
    }

    case HBF_SYSPOKE: {
      // Poke byte to bank
      uint8_t bank = cpu->regs.DE.get_high();
      uint8_t byte = cpu->regs.DE.get_low();
      uint16_t addr = cpu->regs.HL.get_pair16();

      if (addr < 0x8000) {
        memory->write_bank(bank, addr, byte);
      } else {
        memory->store_mem(addr, byte);
      }
      break;
    }

    case HBF_SYSSET: {
      // Set system info - subfunc in C
      switch (subfunc) {
        case SYSSET_TIMER: {
          // Store DE:HL as the new tick count, by moving the origin rather than
          // keeping a counter - the clock stays monotonic either way.
          uint32_t want = ((uint32_t)cpu->regs.DE.get_pair16() << 16) |
                          cpu->regs.HL.get_pair16();
          // Setting the ticks must not move the seconds: they are two counters
          // upstream, coupled only by the ISR.  See setSecs() in the header.
          uint32_t secs_before = currentSecs();
          setTicks(want);
          setSecs(secs_before);
          break;
        }

        case SYSSET_SECS: {
          uint32_t want = ((uint32_t)cpu->regs.DE.get_pair16() << 16) |
                          cpu->regs.HL.get_pair16();
          // No multiply, so no overflow, and the tick count is left alone.
          setSecs(want);
          break;
        }

        case SYSSET_SWITCH: {
          // Set non-volatile switch value (emulates RTC NVRAM)
          // D register contains switch number:
          //   0xFF = reset NVRAM to defaults
          //   1 = boot options: HL = (H:flags+unit, L:app char/slice)
          //   3 = autoboot: L = flags+timeout
          uint8_t switch_num = cpu->regs.DE.get_high();
          if (switch_num == 0xFF) {
            // Reset NVRAM to defaults
            nvram_switches[0] = 'W';         // Signature
            nvram_switches[1] = 'H';         // Help app
            nvram_switches[2] = BOPTS_ROM;   // ROM boot
            nvram_switches[3] = 0;           // No autoboot
            recalcNvramChecksum();
            if (debug_log) {
              emu_log("[SYSSET_SWITCH] RESET to defaults\n");
            }
          } else {
            // THREE PRECONDITIONS, and none of them was here.  SYS_SETSWITCH
            // (hbios.asm:6457-6482) is, in order:
            //
            //   1. CB_SWITCHES == 0 -> SWITCH_RES1, A := $FF.  No NVRAM at all.
            //      There is always NVRAM here, so this cannot fire.
            //   2. D == $FF -> NVSW_RESET.  Handled above; it is the ONLY path
            //      that may run against an uninitialised block.
            //   3. CALL SYS_GETSWITCH3 / RET NZ - the block must already read
            //      'W'.  A set before a reset is refused, and the caller has to
            //      reset first.
            //   4. CALL SWITCH_RES / RET NZ - the switch number must be in the
            //      table, so 4 and up are refused.
            //
            // Both arms below instead assigned `nvram_switches[0] = 'W'` with
            // the comment "Ensure initialized", so the first set of any switch
            // quietly initialised the block - the one thing RomWBW refuses -
            // and every unknown switch number reported success having written
            // nothing.
            if (nvram_switches[0] != 'W') {
              // SYS_GETSWITCH3 returns the status byte and the NZ flag, and
              // upstream's two failing values are $FF (CB_SWITCHES == 0, no
              // NVRAM at all) and 1 (present, not configured).
              //
              // Answer 1, NOT the raw byte.  This emulator stores "not
              // configured" as a zero byte, and zero through setResult() is
              // A = 0 with Z set - which reads as SUCCESS.  The SYSGET $FF arm
              // above already translates the same state to 1 for the same
              // reason, and says so: "A=0 means no NVRAM hardware, A=1 means
              // present but uninitialized". There is always NVRAM here, so 1 is
              // the only honest answer.
              result = 1;
              break;
            }
            if (switch_num > SWITCH_LEN) {
              result = HBR_UNDEF;   // SWITCH_RES1: OR $FF
              break;
            }
            static const uint8_t SWITCH_TAB[SWITCH_LEN + 2] = {0, 2, 0, 1, 0};
            uint8_t width = SWITCH_TAB[switch_num];
            nvram_switches[switch_num] = cpu->regs.HL.get_low();
            if (width > 1) {
              nvram_switches[switch_num + 1] = cpu->regs.HL.get_high();
            }
            // NVSW_UPDATE: the checksum is recomputed by the CALLER of the
            // driver's write, not by the write itself (hbios.asm:8156-8174).
            recalcNvramChecksum();
            if (debug_log) {
              emu_log("[SYSSET_SWITCH] switch %u := 0x%04X (%u byte(s))\n",
                      switch_num, cpu->regs.HL.get_pair16(), width ? width : 1);
            }
          }
          break;
        }

        case SYSSET_BOOTINFO: {
          // Set boot volume info (called by romldr/CPMLDR before loading OS)
          // D = boot unit, E = boot slice, L = bank (always 0)
          saved_boot_unit = cpu->regs.DE.get_high();
          saved_boot_slice = cpu->regs.DE.get_low();
          // L is the BOOT BANK ID and it was being dropped on the floor, even
          // though the comment beside it named the register.  Upstream
          // (hbios.asm:6528-6533) is three stores, not two:
          //
          //     LD A,L / LD (CB_BOOTBID),A / LD (CB_BOOTVOL),DE
          //
          // CB_BOOTBID is HCB offset $0F (emu_hbios.asm:143 has the field), so
          // 0x010F beside the CB_BOOTVOL pair written below.
          saved_boot_bank = cpu->regs.HL.get_low();
          // Update CB_BOOTVOL in HCB at 0x010D. CBIOS may read this from ROM bank 0
          // (which uses shadow RAM) or from RAM bank 0x80. Write via ROM bank 0 mode
          // to set shadow bits, ensuring reads from either path get the updated value.
          uint8_t saved_bank = memory->get_current_bank();
          memory->select_bank(0x00);  // ROM bank 0 - writes go to shadow RAM + set shadow bit
          memory->store_mem(0x010D, (uint8_t)saved_boot_slice);  // CB_BOOTVOL low byte
          memory->store_mem(0x010E, (uint8_t)saved_boot_unit);   // CB_BOOTVOL high byte
          memory->store_mem(0x010F, (uint8_t)saved_boot_bank);   // CB_BOOTBID
          memory->select_bank(saved_bank);  // Restore previous bank
          if (debug_log) {
            emu_log("[SYSSET BOOTINFO] unit=%d slice=%d -> CB_BOOTVOL=0x%02X%02X\n",
                    saved_boot_unit, saved_boot_slice, saved_boot_unit, saved_boot_slice);
          }
          break;
        }
        default:
          // As with SYSGET above: a set that did not happen must not report
          // success.  SETCPUSPD and SETPANEL both landed here, so a guest that
          // asked for a speed change or a front-panel write was told it took.
          if (debug_log) {
            emu_log("[HBIOS SYSSET] Unhandled subfunction 0x%02X\n", subfunc);
          }
          result = HBR_NOFUNC;
          break;
      }
      break;
    }

    case HBF_SYSINT: {
      // Interrupt management.  This was "just return success" with no registers
      // written at all, for every subfunction.
      //
      // INTINFO ($00) and INTGET ($10) are QUERIES - the caller reads an answer
      // out of registers this never set, so it got its own inputs back and was
      // told they were data.  INTSET ($20) is documented to return the PREVIOUS
      // vector in HL (SystemGuide.md, SYSINT), which a caller needs in order to
      // chain to it; answering success without it loses the old handler.
      //
      // There is no interrupt vector table to hand out here: the periodic tick
      // is driven by the emulator, not by a guest-installed ISR, so there is no
      // honest value for any of the three.  Decline by name instead of
      // pretending, which is what every other unimplemented function in this
      // file now does.
      if (debug_log) {
        emu_log("[HBIOS SYSINT] subfunction 0x%02X declined\n",
                cpu->regs.DE.get_low());
      }
      result = HBR_NOTIMPL;
      break;
    }

    case HBF_SYSBOOT: {
      // Boot from device (custom EMU function)
      // HL = address of command string
      uint16_t cmd_addr = cpu->regs.HL.get_pair16();

      // Read command string from memory
      char cmd_str[64];
      int i = 0;
      while (i < 63) {
        uint8_t c = memory->fetch_mem((uint16_t)(cmd_addr + i));
        if (c == 0 || c == '\r' || c == '\n') break;
        cmd_str[i++] = c;
      }
      cmd_str[i] = '\0';

      if (debug_log) {
        emu_log("[SYSBOOT] Command string: '%s'\n", cmd_str);
      }

      // Skip leading whitespace
      char* p = cmd_str;
      while (*p == ' ') p++;

      // Try to boot
      if (!bootFromDevice(p)) {
        emu_fatal("[HBIOS SYSBOOT] bootFromDevice('%s') failed\n", p);
      }
      break;
    }

    default:
      emu_error("[HBIOS SYS] Unhandled function 0x%02X (subfunc=%d) - returning "
                "HBR_NOFUNC\n", func, subfunc);
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// Video Display Adapter (VDA)
//=============================================================================

void HBIOSDispatch::handleVDA() {
  if (!cpu) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t result = HBR_SUCCESS;

  // THE UNIT NUMBER IN C IS CHECKED FIRST, and against a count.
  //
  // Every group in RomWBW reaches its driver through HB_DISPCALC, whose first
  // act is "LD A,C / CP (IY-1) / JR NC,HB_UNITERR" (hbios.asm:7448-7452) -
  // compare the unit against the count, and answer ERR_NOUNIT if it is not
  // there.  Nothing here checked it at all, so every unit 0-255 was served by
  // the one device this emulator has and told it had succeeded: writing to a
  // second serial port printed on the console, reading from one ate the user's
  // keystrokes, and a program enumerating units until ERR_NOUNIT never stopped.
  //
  // The count is the one BF_SYSGET reports for the group; see hbios_dispatch.h.
  // No substitution for this group: VDA_DISPATCH (hbios.asm:5086) goes straight
  // to HB_DISPCALL with no special unit codes.
  if (cpu->regs.BC.get_low() >= VDA_UNIT_COUNT) {
    setResult(HBR_NOUNIT);
    doRet();
    return;
  }

  switch (func) {
    case HBF_VDARES:
      // Reset is not initialise. vdu.asm puts the clear-and-home on VDAINI and
      // makes VDARES "XOR A / RET" - sharing a case label meant any guest
      // resetting the video device had its screen wiped and its cursor moved.
      break;

    case HBF_VDAINI:
      vda_cursor_row = 0;
      vda_cursor_col = 0;
      vda_color = 0x07;
  vda_rub = 0x00;
      emu_video_clear();
      emu_video_set_cursor(0, 0);  // Sync Swift cursor
      break;

    case HBF_VDADEV: {
      // Device info: D := device type, E := device number, H := attributes.
      // (Source/HBIOS/invntdev.asm:345-346, whose comment says "DISK
      // ATTRIBUTES" - that is a copy-paste in RomWBW, it is the video unit.)
      //
      // This had no case at all until 2026-09-05, which was invisible while the
      // only caller lived in the HBIOS bank our proxy replaces. RomWBW 3.6.0
      // moved the device inventory into a ROM app, and an unhandled VDA
      // function here does not report failure - it falls through returning
      // success with the caller's registers untouched. invntdev then used the
      // leftover D as an index into a 9-entry name table with no bound check
      // and printed the video adapter as "AY-3-8910", a sound chip.
      //
      // VDADEV_VDU is the generic character display in RomWBW's table, which is
      // what this emulator presents: a text-mode terminal with no accelerator.
      cpu->regs.DE.set_high(0x00);   // VDADEV_VDU
      // The unit is in C, as it is for every VDA call.
      cpu->regs.DE.set_low(cpu->regs.BC.get_low());
      cpu->regs.HL.set_high(0x00);   // No attributes claimed
      cpu->regs.HL.set_low(0x00);    // No I/O base - there is no port here
      break;
    }

    case HBF_VDAQRY: {
      // Query video config: D := ROWS, E := COLS. That order is RomWBW's, not a
      // choice - Source/HBIOS/invntdev.asm:371-378 reads it back as
      // "RST 08 ; D:=ROWS, E:=COLS" and prints E, 'x', D.
      //
      // These were swapped until 2026-09-05, which printed the geometry
      // transposed - "Text,25x80" for an 80x25 screen. Harmless on that
      // diagnostic line and not harmless to a guest that sizes a window from it.
      cpu->regs.DE.set_high((uint8_t)vda_rows);
      cpu->regs.DE.set_low((uint8_t)vda_cols);
      // C := mode, HL := 0. VDU_VDAQRY is "LD C,$00 ; MODE ZERO IS ALL WE KNOW
      // / LD DE,... / LD HL,0 ; EXTRACTION OF CURRENT BITMAP DATA NOT
      // SUPPORTED", and every other VDA driver agrees. Both were left holding
      // the caller's input.
      cpu->regs.BC.set_low(0x00);
      cpu->regs.HL.set_pair16(0x0000);
      break;
    }

    case HBF_VDASCP: {
      // Set cursor position
      vda_cursor_row = cpu->regs.DE.get_high();
      vda_cursor_col = cpu->regs.DE.get_low();
      emu_video_set_cursor(vda_cursor_row, vda_cursor_col);
      break;
    }

    // ATTRIBUTE AND COLOUR ARE TWO PIECES OF STATE, not one.
    //
    // VDASAT's E is a reverse/underline/blink bitmap and VDASCO's D and E are
    // foreground and background.  Both used to assign the same `vda_attr`, so
    // setting the colour cleared reverse-video and setting reverse-video
    // replaced the colours with a bitmap read as a colour pair.  A guest that
    // does the documented thing - pick colours once, then turn reverse on and
    // off around a highlighted field - lost its colours on the first toggle.
    case HBF_VDASAT: {
      vda_rub = cpu->regs.DE.get_low();
      emu_video_set_attr(vdaAttrByte());
      break;
    }

    case HBF_VDASCO: {
      // D = foreground, E = background (CGA 16-color)
      uint8_t fg = cpu->regs.DE.get_high();
      uint8_t bg = cpu->regs.DE.get_low();
      vda_color = (uint8_t)((bg << 4) | (fg & 0x0F));
      emu_video_set_attr(vdaAttrByte());
      break;
    }

    case HBF_VDAWRC: {
      // Write character at cursor
      uint8_t ch = cpu->regs.DE.get_low();

      // Handle control characters
      if (ch == 0x0D) {
        // Carriage return - move cursor to column 0
        vda_cursor_col = 0;
        emu_video_set_cursor(vda_cursor_row, vda_cursor_col);
        break;
      } else if (ch == 0x0A) {
        // Line feed - move cursor down one row
        vda_cursor_row++;
        if (vda_cursor_row >= vda_rows) {
          vda_cursor_row = vda_rows - 1;
          emu_video_scroll_up(1);
        }
        emu_video_set_cursor(vda_cursor_row, vda_cursor_col);
        break;
      }

      // Write printable character
      emu_video_write_char(ch);

      // Advance cursor
      vda_cursor_col++;
      if (vda_cursor_col >= vda_cols) {
        vda_cursor_col = 0;
        vda_cursor_row++;
        if (vda_cursor_row >= vda_rows) {
          vda_cursor_row = vda_rows - 1;
          emu_video_scroll_up(1);
        }
      }
      emu_video_set_cursor(vda_cursor_row, vda_cursor_col);
      break;
    }

    case HBF_VDAFIL: {
      // Fill with character
      uint8_t ch = cpu->regs.DE.get_low();
      uint16_t count = cpu->regs.HL.get_pair16();
      for (uint16_t i = 0; i < count; i++) {
        emu_video_write_char(ch);
        vda_cursor_col++;
        if (vda_cursor_col >= vda_cols) {
          vda_cursor_col = 0;
          vda_cursor_row++;
          if (vda_cursor_row >= vda_rows) {
            vda_cursor_row = vda_rows - 1;
            emu_video_scroll_up(1);
          }
        }
      }
      emu_video_set_cursor(vda_cursor_row, vda_cursor_col);
      break;
    }

    case HBF_VDASCR: {
      // E IS SIGNED.  SystemGuide.md, Function 0x4B: "If Lines (E) is positive,
      // then a forward scroll is performed.  If Lines (E) contains a negative
      // number, then a reverse scroll will be performed."
      //
      // get_low() is an unsigned byte, so E = $FF - a one-line reverse scroll,
      // which is how a full-screen editor scrolls back - arrived here as 255
      // and asked for 255 lines of forward scroll.
      int lines = (int)(int8_t)cpu->regs.DE.get_low();
      emu_video_scroll_up(lines);
      break;
    }

    case HBF_VDAKST: {
      // Keyboard status - the VDA twin of HBF_CIOIST, and it answers the same
      // way: A is the status the caller tests, E the pending count. Setting
      // only E left `result` at HBR_SUCCESS (0), so A said "no key" however
      // much was queued and a guest polling this device never read one.
      // Same as BF_CIOIST above, and wrong the same way until 2026-09-06: a
      // count, not a flag, and 0xFF reads as a negative error code.
      bool has_key = emu_console_has_input();
      result = has_key ? 1 : 0;                    // A = count waiting
      cpu->regs.DE.set_low(has_key ? 1 : 0);       // E = pending count
      // Track consecutive "no input" polls for idle detection
      if (has_key) idle_poll_count = 0; else idle_poll_count++;
      break;
    }

    case HBF_VDAKRD: {
      // Keyboard read - the VDA twin of HBF_CIOIN, and handles a missing key
      // the same way. It used to set waiting_for_input and return without
      // rewinding PC, but dispatch is a 2-byte OUT (0xEF),A followed by the
      // proxy's own RET: skipping the rewind let that RET fire with E holding
      // whatever the last call left there, so the guest took stale data for a
      // keystroke and never came back for the real one.
      if (!blocking_allowed && !emu_console_has_input()) {
        // Non-blocking (web/WASM, iOS) - rewind over the OUT so the call
        // re-runs once a key arrives.
        uint16_t pc = cpu->regs.PC.get_pair16();
        cpu->regs.PC.set_pair16(pc - 2);
        waiting_for_input = true;
        return;  // Don't call setResult/doRet - will retry
      }
      // Blocking mode (CLI) - flush pending output before we stop for a key,
      // so a prompt written just above is on screen while we wait.
      while (!output_buffer.empty()) {
        emu_console_write_char(output_buffer.front());
        output_buffer.erase(output_buffer.begin());
      }
      int ch = emu_console_read_char();
      if (ch == EMU_CONSOLE_RETRY) {
        // The keystroke was the host's reserved escape key, so there is no
        // byte for the guest. Rewind exactly as the branch above does; the
        // main loop sees the escape and this call re-runs afterwards.
        uint16_t pc = cpu->regs.PC.get_pair16();
        cpu->regs.PC.set_pair16(pc - 2);
        return;  // Don't call setResult/doRet - will retry
      }
      // A raw negative would reach the guest as 0xFF/0xFE, so clamp: EOF is
      // ^Z, the same end-of-file marker CIOIN hands back.
      if (ch < 0) ch = 0x1A;
      cpu->regs.DE.set_low(ch & 0xFF);
      // Three values, not one: E is the keycode, D the keystate bitmap (shift,
      // ctrl, alt, the lock keys) and C the scancode.  A driver with no
      // scancode support returns ZERO in C rather than leaving it, and the same
      // for D - leaving them hands the caller its own registers back as though
      // they were modifier state.  There is no scancode and no modifier
      // information behind a host terminal, so zero is the honest answer.
      cpu->regs.DE.set_high(0x00);   // D := no keystate
      cpu->regs.BC.set_low(0x00);    // C := no scancode
      waiting_for_input = false;
      idle_poll_count = 0;
      break;
    }

    case HBF_VDASCS:
      // Set cursor style, from the two nibbles of D.  A device with no
      // adjustable cursor still SUCCEEDS - tvga.asm is the precedent - so this
      // accepts it and does nothing.  It had no case, and an unhandled VDA
      // function is the failure mode the VDADEV comment above describes at
      // length: success with the caller's own registers.
      break;

    case HBF_VDAKFL:
      // Flush the keyboard buffer.  Real, cheap, and it was answering
      // ERR_NOFUNC for a function every driver implements.
      while (emu_console_has_input()) {
        (void)emu_console_read_char();
      }
      break;

    case HBF_VDACPY:
      // Copy Count (L) cells from the row/col in D/E to the cursor position,
      // without moving the cursor.  It needs to READ cells back, and emu_io.h
      // has emu_video_write_char_at() with no read twin - the video here is the
      // host terminal, which this process cannot interrogate.  Decline by name
      // rather than silently copy nothing.
      result = HBR_NOTIMPL;
      break;

    case HBF_VDARDC: {
      // Read the character AT the cursor.  This answered a hard-coded space
      // with SUCCESS and set neither B (colour) nor C (attribute), so a guest
      // reading the screen back got a blank display it was told was real, and
      // two registers of its own.
      //
      // There is nothing to read from: emu_io.h has emu_video_write_char_at()
      // and no read-back, and the video here is the host terminal, which this
      // process cannot interrogate.  So decline, the way every other function
      // without an answer now does, rather than invent one.
      result = HBR_NOTIMPL;
      break;
    }

    default:
      // Report it. An unhandled function in this group used to fall through
      // with result still HBR_SUCCESS, handing the caller its own registers
      // back as though they were an answer - which is how the ROM device
      // inventory came to print a sound chip's name in the video row.
      if (debug_log) {
        emu_log("[HBIOS VDA] Unhandled function 0x%02X\n", func);
      }
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// Sound (SND)
//=============================================================================

void HBIOSDispatch::handleSND() {
  if (!cpu) return;

  uint8_t func = cpu->regs.BC.get_high();
  // C IS THE SOUND UNIT, NOT A CHANNEL.  Every function in this group takes it
  // and this dispatcher used it as a channel index for the three setters and
  // for play.  SystemGuide.md is unambiguous - "C: Sound Unit" on all eight
  // tables - and its worked example is the clearest statement of the model:
  //
  //     HBIOS B=51 C=00 L=80      ; Set volume to half level
  //     HBIOS B=53 C=00 HL=152    ; Select Middle C (C4)
  //     HBIOS B=54 C=00 D=01      ; Play note on Channel 1
  //
  // One unit, one pending volume and pitch, and the CHANNEL named at play time
  // in D.
  uint8_t result = HBR_SUCCESS;

  // THE UNIT NUMBER IN C IS CHECKED FIRST, and against a count.
  //
  // Every group in RomWBW reaches its driver through HB_DISPCALC, whose first
  // act is "LD A,C / CP (IY-1) / JR NC,HB_UNITERR" (hbios.asm:7448-7452) -
  // compare the unit against the count, and answer ERR_NOUNIT if it is not
  // there.  Nothing here checked it at all, so every unit 0-255 was served by
  // the one device this emulator has and told it had succeeded: writing to a
  // second serial port printed on the console, reading from one ate the user's
  // keystrokes, and a program enumerating units until ERR_NOUNIT never stopped.
  //
  // The count is the one BF_SYSGET reports for the group; see hbios_dispatch.h.
  if (cpu->regs.BC.get_low() >= SND_UNIT_COUNT) {
    setResult(HBR_NOUNIT);
    doRet();
    return;
  }

  switch (func) {
    case HBF_SNDRESET:
      // Silence what is sounding, not merely forget it.  Zeroing the arrays
      // left a tone already handed to the front end playing on, because
      // nothing told the front end anything had changed.
      for (int i = 0; i < 4; i++) {
        snd_volume[i] = 0;
        snd_period[i] = 0;
        emu_snd_emit_tone(i, 0, 0, 0);
      }
      snd_pending_volume = 0;
      snd_pending_period = 0;
      snd_duration = 0;
      break;

    // The three setters preset the UNIT's pending sound.  hbios.inc is explicit
    // about which register each reads - "BF_SNDVOL ... L CONTAINS VOLUME",
    // "BF_SNDPRD ... HL CONTAINS DRIVER SPECIFIC VALUE", "BF_SNDNOTE ...
    // CONTAINS NOTE" - and all three were reading E or DE before 2026-09.
    case HBF_SNDVOL:
      snd_pending_volume = cpu->regs.HL.get_low();
      break;

    case HBF_SNDPRD:
      snd_pending_period = cpu->regs.HL.get_pair16();
      break;

    case HBF_SNDNOTE: {
      // HL, not L: SystemGuide.md's table is "HL: Note" and its own note table
      // runs to 340 for B7, which does not fit in a byte.  hbios.inc's older
      // comment says L and is the looser of the two.
      //
      // The scale is eighth tones - 8 to a whole tone, 48 to an octave, so 4 to
      // a semitone - and ITS ZERO IS A#0/Bb0, which the table states outright
      // ("The value 0 corresponds to Bb/A# in octave 0") and which its columns
      // confirm: C4 = 152, A4 = 188.  This assumed A4 sat at step 9 of octave
      // 4 under a plain note/48 split, which put A4 at 246Hz and everything
      // else wrong by a growing margin.
      //
      // A4 = 188 is the anchor, so freq = 440 * 2^((note - 188) / 48).
      uint16_t note = cpu->regs.HL.get_pair16();
      double freq = 440.0 * pow(2.0, ((double)note - 188.0) / 48.0);
      // Stored as a period in microseconds, which is what BF_SNDPLAY converts
      // back to a frequency.  Clamped so a very low note cannot overflow it.
      double period_us = freq > 0 ? (1000000.0 / freq) : 0;
      snd_pending_period = period_us > 65535.0 ? 65535
                         : (uint16_t)period_us;
      break;
    }

    case HBF_SNDDUR:
      snd_duration = cpu->regs.HL.get_pair16();
      break;

    case HBF_SNDPLAY: {
      // D is the channel; C was the unit.  This read C and so every guest that
      // followed the documented sequence played on channel 0 whatever channel
      // it asked for.
      uint8_t ch = cpu->regs.DE.get_high();
      if (ch >= 4) {
        result = HBR_RANGE;
        break;
      }
      // Apply the pending pair to that channel, which is what "programming the
      // sound chip" means here, then hand it to the front end.
      snd_volume[ch] = snd_pending_volume;
      snd_period[ch] = snd_pending_period;

      int freq_hz = snd_period[ch] > 0 ? (int)(1000000 / snd_period[ch]) : 0;
      // Volume 0 is silence and is a real instruction, not a no-op: it is how a
      // guest stops a channel it started.
      emu_snd_emit_tone(ch, freq_hz, snd_volume[ch], snd_duration);
      break;
    }

    case HBF_SNDDEVICE:
      // The sound-side twin of BF_VDADEV, and it had no case at all - so it
      // reached the default arm and answered ERR_NOFUNC for a function every
      // RomWBW sound driver implements.
      //
      // C := attributes, D := device type, E := device number, H := unit mode,
      // L := base I/O address.  The same SNDDEV_BITMODE the SNDQ_DEV
      // subfunction reports, for the same reason: it is the one code in
      // hbios.inc:470-473 that does not name a chip this does not emulate.
      cpu->regs.BC.set_low(0x00);    // C := no attributes
      cpu->regs.DE.set_high(0x02);   // D := SNDDEV_BITMODE
      cpu->regs.DE.set_low(0x00);    // E := device number
      cpu->regs.HL.set_pair16(0);    // H := no modes, L := no I/O base
      break;

    case HBF_SNDBEEP:
      // RomWBW's beep is a real note: roughly 333ms of ~987Hz on channel 0,
      // then silence (SystemGuide.md 0x58 and the drivers behind it).  This was
      // emu_dsky_beep(100) - a front end's fixed beep, a third of the length.
      //
      // Through emu_snd_emit_tone, so a port that has installed a renderer gets
      // the pitch and the duration, and one that has not still gets its beep -
      // for 333ms now rather than 100.
      //
      // It does NOT block the guest.  Upstream's does, and nothing here can:
      // the web front end runs the Z80 on the browser's main loop, so sleeping
      // in a dispatch handler stops the page.
      emu_snd_emit_tone(0, 987, 255, 333);
      break;

    case HBF_SNDQUERY: {
      // The subfunction is in E and was ignored, so every query answered the
      // same way and left B and C holding whatever the caller passed in. On
      // RomWBW 3.6.0's device inventory that printed "85+0 CHANNELS" - 85 being
      // 0x55, BF_SNDQUERY itself, read back out of B as though it were data.
      uint8_t subfunc = cpu->regs.DE.get_low();
      switch (subfunc) {
        case SNDQ_CHCNT:
          // B := tone channels, C := noise channels
          // (Source/HBIOS/invntdev.asm:424-433 prints them as "B+C CHANNELS".)
          cpu->regs.BC.set_high(4);
          cpu->regs.BC.set_low(0);
          break;
        case SNDQ_VOLUME:
          // "L: Volume" - the UNIT's pending volume, the one a following
          // SNDPLAY would apply. Answered ERR_NOFUNC until 2026-09-18, because
          // the state was per-channel and there was no unit value to give.
          cpu->regs.HL.set_low(snd_pending_volume);
          break;

        case SNDQ_PERIOD:
          // "HL: Period", 16 bit. Same story as volume above.
          cpu->regs.HL.set_pair16(snd_pending_period);
          break;

        case SNDQ_DEV:
          // B := device type code, DE and HL := I/O ports.
          //
          // AN ERROR IS NOT AVAILABLE HERE, and that is worth knowing before
          // changing it. Source/HBIOS/invntdev.asm:412-418 calls this, then
          // does `LD A,B` and indexes a four-entry name table with it - it
          // never looks at A. Return a failure and B still holds BF_SNDQUERY
          // ($55), which the ROM prints as index 85 of a table with four
          // entries. That is the same shape as the "85+0 CHANNELS" bug fixed
          // earlier in this file, and it is why this answers a code at all.
          //
          // Of the four, $00 SN76489 and $01 AY38910 name real chips this does
          // not emulate, and $03 YM2612 likewise. $02 SNDDEV_BITMODE, "Bit-bang
          // Speaker", is the one that claims no chip - the host makes the
          // noise - so it is the least wrong valid answer. It answered $00
          // until 2026-09-18 on the belief that 0 meant "none"; hbios.inc:470
          // says $00 is SN76489.
          //
          // No I/O ports, because there are none to report.
          cpu->regs.BC.set_high(0x02);  // SNDDEV_BITMODE
          cpu->regs.DE.set_pair16(0);
          cpu->regs.HL.set_pair16(0);
          break;
        default:
          result = HBR_NOFUNC;
          break;
      }
      break;
    }

    default:
      // Report it. An unhandled function in this group used to fall through
      // with result still HBR_SUCCESS, handing the caller its own registers
      // back as though they were an answer - which is how the ROM device
      // inventory came to print a sound chip's name in the video row.
      if (debug_log) {
        emu_log("[HBIOS SND] Unhandled function 0x%02X\n", func);
      }
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// DSKY (Display/Keypad)
//=============================================================================

void HBIOSDispatch::handleDSKY() {
  if (!cpu) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t result = HBR_NOHW;  // No DSKY hardware present

  // All DSKY functions return HBR_NOHW since we don't emulate DSKY hardware
  // This matches the CLI behavior
  switch (func) {
    case HBF_DSKYRESET:
    case HBF_DSKYSTAT:
    case HBF_DSKYGETKEY:
    case HBF_DSKYSHOWHEX:
    case HBF_DSKYSHOWSEG:
    case HBF_DSKYKEYLEDS:
    case HBF_DSKYSTATLED:
    case HBF_DSKYBEEP:
    case HBF_DSKYDEVICE:
    case HBF_DSKYMESSAGE:
    case HBF_DSKYEVENT:
      // All return HBR_NOHW
      break;

    default:
      if (debug_log) {
        emu_log("[HBIOS DSKY] Unhandled function 0x%02X\n", func);
      }
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// Extension Functions (EXT)
//=============================================================================

void HBIOSDispatch::handleEXT() {
  if (!cpu || !memory) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t result = HBR_SUCCESS;

  switch (func) {
    case HBF_EXTSLICE: {
      // EXTSLICE - Get extended disk media information and slice offset
      // Input: B=0xE0, D=disk unit, E=slice number
      // Output: A=result, B=device attrs, C=media ID, DE:HL=LBA offset
      uint8_t disk_unit = cpu->regs.DE.get_high();  // D = disk unit
      uint8_t slice = cpu->regs.DE.get_low();       // E = slice number

      // B IS THE UNIT'S DEVICE ATTRIBUTES, the same byte BF_DIODEVICE reports.
      // EXT_SLICE opens by calling BF_DIODEVICE and storing the attribute byte
      // in SLICE_DEVATT (hbios.asm:5413-5414), and hands it back in B on every
      // exit - the success path and the ERR_RANGE path alike (5642, 5697).
      //
      // This returned 0 always, which is a claim: bit 4 clear says the unit is
      // not LBA capable, and CBIOS requires that bit before it will put a unit
      // in the drive map.  The value is set below, from the same expressions
      // HBF_DIODEVICE uses, once the unit is known.
      uint8_t dev_attrs = 0x00;
      uint8_t media_id = 0x04;   // MID_HD (default)
      uint32_t slice_lba = 0;

      // Map unit number to internal disk index using same mapping as DIO
      // First check if it's a memory disk
      bool is_memdisk = is_md_unit(disk_unit, md_disks);
      uint8_t hd_idx = map_hd_unit(disk_unit);

      if (is_memdisk) {
        // The attribute byte BF_DIODEVICE reports for this unit: md.asm's own
        // MD_AROM/MD_ARAM constants, bit 4 LBA capable, low nibble the media
        // class.
        uint8_t md_idx = map_md_unit(disk_unit);
        dev_attrs = (md_idx < 2 && md_disks[md_idx].is_rom) ? 0x14 : 0x15;

        // Memory disks have no slices. RomWBW answers a non-zero slice on one
        // with ERR_RANGE rather than quietly handing back slice 0's LBA, which
        // would give a guest the same data under two different names.
        slice_lba = 0;
        if (slice != 0) {
          result = HBR_RANGE;
        }
        // Ask the disk, exactly as HBF_DIOMEDIA does above - RomWBW's own
        // EXT_SLICE gets the media ID by calling BF_DIOMEDIA rather than
        // deriving it from the unit number (hbios.asm EXT_SLICE, "CALL
        // DIO_DISPATCH ; CALL DIO TO GET MEDIAID (RESULT IN E)").
        //
        // Deriving it here was both a duplicate and backwards: md_disks[0] is
        // the RAM disk and md_disks[1] the ROM disk, so CP/M gave each memory
        // disk the other's DPB and STAT reported 384K for the 256KB RAM disk.
        media_id = md_disks[map_md_unit(disk_unit)].is_rom ? MID_MDROM : MID_MDRAM;
        if (debug_log) debug_log("[HBIOS EXTSLICE] Memory disk unit 0x%02X, no slices\n", disk_unit);
      } else if (hd_idx != 0xFF && hd_idx < 16 && disks[hd_idx].is_open) {
        HBDisk& disk = disks[hd_idx];

        // %00110000, the same byte BF_DIODEVICE reports for a hard disk:
        // hdsk.asm:192 "LD C,%00110000 ; C := ATTRIBUTES, NON-REMOVABLE HARD
        // DISK".  Bit 4 is LBA capable and CBIOS requires it.
        dev_attrs = 0x30;

        // Probe MBR if not yet done
        if (!disk.partition_probed) {
          disk.partition_probed = true;
          disk.partition_base_lba = 0;
          disk.partition_sectors = 0;   // see closeDisk: the pair resets together
          disk.slice_size = 16640;  // Default: hd512 format
          disk.is_hd1k = false;

          bool detected_format = false;
          uint8_t mbr[512];
          bool mbr_valid = false;
          size_t disk_size = disk.size;

          // Read MBR - try file-backed first, then in-memory
          if (disk.file_backed && disk.handle) {
            // File-backed disk - read MBR via portable I/O
            size_t read = emu_disk_read((emu_disk_handle)disk.handle, 0, mbr, 512);
            mbr_valid = (read == 512);
            if (disk_size == 0) {
              disk_size = emu_disk_size((emu_disk_handle)disk.handle);
            }
          } else if (!disk.data.empty() && disk.data.size() >= 512) {
            // In-memory disk
            memcpy(mbr, disk.data.data(), 512);
            mbr_valid = true;
            if (disk_size == 0) {
              disk_size = disk.data.size();
            }
          }

          if (mbr_valid) {
            // Check for valid MBR signature
            if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
              // Check partition table for type 0x2E (RomWBW hd1k partition)
              for (int p = 0; p < 4; p++) {
                int offset = 0x1BE + (p * 16);
                uint8_t ptype = mbr[offset + 4];
                if (ptype == 0x2E) {
                  // Found RomWBW partition (hd1k format)
                  uint32_t part_lba = mbr[offset + 8] |
                                      (mbr[offset + 9] << 8) |
                                      (mbr[offset + 10] << 16) |
                                      (mbr[offset + 11] << 24);
                  // The entry's SIZE as well as its offset. RomWBW copies both
                  // in one go - EXT_SLICE3B does "LD BC,8 ; 8 BYTES - LBA
                  // OFFSET AND SIZE / LDIR" - and then bounds a slice against
                  // the partition. Reading only the offset is what left the
                  // bounds check below comparing against the whole medium.
                  uint32_t part_sectors = mbr[offset + 12] |
                                          (mbr[offset + 13] << 8) |
                                          (mbr[offset + 14] << 16) |
                                          (mbr[offset + 15] << 24);
                  disk.partition_base_lba = part_lba;
                  disk.partition_sectors = part_sectors;
                  disk.slice_size = 16384;  // hd1k: 8MB slices
                  disk.is_hd1k = true;
                  detected_format = true;
                  if (debug_log) debug_log("[HBIOS EXTSLICE] Detected hd1k format (0x2E partition), LBA %u\n", part_lba);
                  break;
                }
              }
            }

            // A SIZE HEURISTIC RomWBW DOES NOT HAVE, KEPT DELIBERATELY, and
            // measured before and after.
            //
            // Upstream reaches hd1k only through a $2E partition entry:
            // EXT_SLICE3B is the only path that sets SPS_HD1K, and the
            // no-partition fallback EXT_SLICE3C (hbios.asm:5565-5573) is
            // unconditional - "LD BC,SPS_HD512" - with nothing in between that
            // looks at the medium's size.  An audit on 2026-09-18 raised that
            // as a divergence, correctly.
            //
            // It was removed, and the published single-slice images stopped
            // booting: tools/boot_test.sh in romwbw_disks went red on Z3PLUS
            // for BOTH releases, because is_hd1k also chooses the MEDIA ID, and
            // MID_HD makes CBIOS read an hd1k filesystem - 1024 directory
            // entries - with hd512 parameters.
            //
            // The reason is how those images are built: romwbw_disks cuts them
            // with the wbw_hd1k diskdefs and gives them a type-06 partition
            // entry rather than a $2E one, so upstream's own rule would call
            // them hd512 too.  The heuristic is what makes them work here.  It
            // is wrong about RomWBW and right about the artifacts this emulator
            // exists to run, and the boot test is what says so.
            if (!detected_format && disk_size == 8388608) {
              disk.partition_base_lba = 0;
              disk.slice_size = 16384;
              disk.is_hd1k = true;
              detected_format = true;
              if (debug_log) debug_log("[HBIOS EXTSLICE] Detected hd1k format (8MB single slice)\n");
            }

            if (!detected_format) {
              if (debug_log) debug_log("[HBIOS EXTSLICE] Using hd512 format (size=%zu)\n", disk_size);
            }
          }
        }

        // Bound the slice's END, against the PARTITION where there is one.
        //
        // This compared the slice's START against the whole medium, which is
        // wrong twice over. RomWBW computes the upper sector - EXT_SLICE5A-5B
        // "ADD HL,BC ; ADD SPS, GET REQUIRED CAPCITY (UPPER SECTOR)" - and
        // compares it against the 0x2E partition's own size. Checking the start
        // admits a slice that begins inside the medium and runs off the end;
        // checking against the medium rather than the partition admits a slice
        // that runs into whatever follows the RomWBW partition, and CBIOS then
        // maps a drive letter onto a foreign filesystem.
        //
        // With no 0x2E entry there is no partition to bound against and the
        // medium is the limit, which is the fallback EXT_SLICE3C takes.
        uint32_t disk_sectors = disk.total_sectors();
        uint32_t slice_start_sector = disk.partition_base_lba + ((uint32_t)slice * disk.slice_size);
        uint64_t slice_end_sector = (uint64_t)slice_start_sector + disk.slice_size;

        uint64_t limit = disk_sectors;
        if (disk.partition_sectors != 0) {
          uint64_t part_end = (uint64_t)disk.partition_base_lba + disk.partition_sectors;
          if (part_end < limit) limit = part_end;
        }

        // SLICE 0 ON AN UNPARTITIONED MEDIUM IS NOT BOUND-CHECKED AT ALL.
        //
        // EXT_SLICE3C is the no-partition fallback, and for slice 0 it jumps
        // straight to EXT_SLICE5Z - "JR Z,EXT_SLICE5Z" - skipping the fit
        // check entirely and returning A=0, C=MID_HD, DE:HL=0 whatever the
        // medium's size.  That is deliberate upstream: slice 0 starts at sector
        // 0, so there is nothing for it to run off the end of, and a medium
        // smaller than one slice is still bootable from its first sector.
        //
        // Checking it here refused slice 0 on any bare image smaller than
        // 16640 sectors (8.3MB) - which is every small test image, and every
        // hd1k image read before the format is known.
        bool unbounded_slice0 = (slice == 0 && disk.partition_sectors == 0);

        if (slice_end_sector > limit && !unbounded_slice0) {
          // Slice runs past the end of its partition, or of the medium
          media_id = 0;  // MID_NONE - signals no valid media
          // ERR_RANGE, which is what EXT_SLICE returns for a slice past the
          // end. HBR_FAILED (0xFF) is a generic failure a caller cannot
          // distinguish from a missing unit.
          result = HBR_RANGE;
          if (debug_log) {
            debug_log("[HBIOS EXTSLICE] unit=0x%02X slice=%d rejected "
                      "(end %llu > limit %llu)\n", disk_unit, slice,
                      (unsigned long long)slice_end_sector,
                      (unsigned long long)limit);
          }
        } else {
          slice_lba = slice_start_sector;

          // Set media ID based on detected format
          if (disk.is_hd1k) {
            media_id = 0x0A;  // MID_HDNEW (hd1k format)
          }
          emu_log("[EXTSLICE] unit=0x%02X slice=%d -> media=0x%02X LBA=%u\n",
                  disk_unit, slice, media_id, slice_lba);
        }
      } else {
        // No disk on this unit. NOUNIT rather than a generic failure, and a
        // log rather than an error: a guest enumerating units will ask about
        // ones that are not there, and that is not the emulator going wrong.
        result = HBR_NOUNIT;
        media_id = 0;  // MID_NONE
        if (debug_log) {
          emu_log("[HBIOS EXTSLICE] unit=0x%02X slice=%d - no disk\n", disk_unit, slice);
        }
      }

      // Set return values
      cpu->regs.BC.set_high(dev_attrs);  // B = device attributes
      cpu->regs.BC.set_low(media_id);    // C = media ID
      cpu->regs.DE.set_pair16((slice_lba >> 16) & 0xFFFF);
      cpu->regs.HL.set_pair16(slice_lba & 0xFFFF);
      break;
    }

    case HBF_HOST_OPEN_R: {
      // Open host file for reading
      // Input: DE = address of null-terminated path string
      // Output: A = 0 success, 0xFF failure
      uint16_t path_addr = cpu->regs.DE.get_pair16();
      std::string path;
      if (!fetchGuestString(path_addr, &path)) {
        emu_error("[HOST] Open for read refused: no terminator in the first %d "
                  "bytes at 0x%04X\n", HOST_PATH_MAX, path_addr);
        result = HBR_FAILED;
        break;
      }

      if (emu_host_file_open_read(path.c_str())) {
        if (debug_log) debug_log("[HOST] Opened for read: %s\n", path.c_str());
        result = HBR_SUCCESS;
      } else {
        if (debug_log) debug_log("[HOST] Failed to open for read: %s\n", path.c_str());
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_HOST_OPEN_W: {
      // Open host file for writing
      // Input: DE = address of null-terminated path string
      // Output: A = 0 success, 0xFF failure
      uint16_t path_addr = cpu->regs.DE.get_pair16();
      std::string path;
      if (!fetchGuestString(path_addr, &path)) {
        // Refusing here matters more than on the read: a truncated write path
        // does not open the wrong file, it creates one.
        emu_error("[HOST] Open for write refused: no terminator in the first %d "
                  "bytes at 0x%04X\n", HOST_PATH_MAX, path_addr);
        result = HBR_FAILED;
        break;
      }

      if (emu_host_file_open_write(path.c_str())) {
        if (debug_log) debug_log("[HOST] Opened for write: %s\n", path.c_str());
        result = HBR_SUCCESS;
      } else {
        if (debug_log) debug_log("[HOST] Failed to open for write: %s\n", path.c_str());
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_HOST_READ: {
      // Read byte from host file
      // Output: A = 0 success (E = byte), A = 0xFF EOF or error
      if (!blocking_allowed && emu_host_file_get_state() == HOST_FILE_WAITING_READ) {
        // Browser: the file picker is still open. Rewind PC to re-execute the
        // OUT instruction (same trick as HBF_CIOIN) so the guest retries this
        // call once JS provides the file (state -> READING) or cancels
        // (state -> IDLE, read returns EOF). Without this the guest sees an
        // instant EOF and R8 imports a 0-byte file.
        uint16_t pc = cpu->regs.PC.get_pair16();
        cpu->regs.PC.set_pair16(pc - 2);
        waiting_for_host_file = true;
        return;  // Don't call setResult/doRet - will retry
      }
      waiting_for_host_file = false;
      int ch = emu_host_file_read_byte();
      if (ch < 0) {
        result = HBR_FAILED;  // EOF or error
      } else {
        cpu->regs.DE.set_low((uint8_t)ch);
        result = HBR_SUCCESS;
      }
      break;
    }

    case HBF_HOST_WRITE: {
      // Write byte to host file
      // Input: E = byte to write
      // Output: A = 0 success, 0xFF failure
      uint8_t byte = cpu->regs.DE.get_low();
      if (emu_host_file_write_byte(byte)) {
        result = HBR_SUCCESS;
      } else {
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_HOST_CLOSE: {
      // Close host file
      // Input: C = 0 for read file, C = 1 for write file
      // Output: A = 0 success
      uint8_t which = cpu->regs.BC.get_low();
      if (which == 0) {
        // Close read file
        emu_host_file_close_read();
        result = HBR_SUCCESS;
      } else {
        // Close write file (triggers download in web); a failed final flush
        // means the host file may be truncated (e.g. disk full)
        if (emu_host_file_close_write()) {
          result = HBR_SUCCESS;
        } else {
          if (debug_log) debug_log("[HOST] Close-write failed - file may be truncated\n");
          result = HBR_FAILED;
        }
      }
      break;
    }

    case HBF_HOST_MODE: {
      // Get/set transfer mode
      // Input: C = 0 get mode, C = 1 set mode; E = mode (0=auto, 1=text, 2=binary)
      // Output: E = current mode (for get), A = 0 success
      uint8_t subcmd = cpu->regs.BC.get_low();
      if (subcmd == 0) {
        // Get mode
        cpu->regs.DE.set_low(host_transfer_mode);
      } else {
        // Set mode
        host_transfer_mode = cpu->regs.DE.get_low();
      }
      result = HBR_SUCCESS;
      break;
    }

    case HBF_HOST_GETNAME: {
      // Where the open write file will actually land.
      // Input:  C = buffer size at DE, including room for the terminator
      //         DE = buffer address
      // Output: A = 0 and the buffer holds a NUL-terminated string
      //         A = 0xFF and the buffer is untouched
      //
      // W8 prints this instead of the path the user typed, because on most
      // front ends they are not the same string: the CLI lowercases the
      // basename and resolves the parent through whatever case the directory
      // really has, the browser reduces the whole path to a download name, and
      // a sandboxed app writes into its own Exports folder wherever the guest
      // pointed. Printing the typed path names a file that does not exist.
      const char* name = (emu_host_file_get_state() == HOST_FILE_WRITING)
                             ? emu_host_file_get_write_name()
                             : nullptr;
      if (!storeHostName(name, cpu->regs.BC.get_low(), cpu->regs.DE.get_pair16())) {
        result = HBR_FAILED;
        break;
      }
      if (debug_log) debug_log("[HOST] Effective write path: %s\n", name);
      result = HBR_SUCCESS;
      break;
    }

    case HBF_HOST_GETRNAME: {
      // Which file the open read is actually reading.  Same convention as
      // HBF_HOST_GETNAME above; see hbios_dispatch.h for why the two differ
      // only in which side of the transfer they describe.
      //
      // R8 prints this instead of the path the user typed.  Unlike the write
      // side the file has to exist for the open to have succeeded, so the two
      // strings are usually close - but "close" is not "the same": the CLI
      // retries a shouted path case-insensitively and answers with the
      // absolute path it settled on, and a front end with a file picker opens
      // whatever the user chose there.
      const char* name = (emu_host_file_get_state() == HOST_FILE_READING)
                             ? emu_host_file_get_read_name()
                             : nullptr;
      if (!storeHostName(name, cpu->regs.BC.get_low(), cpu->regs.DE.get_pair16())) {
        result = HBR_FAILED;
        break;
      }
      if (debug_log) debug_log("[HOST] Effective read path: %s\n", name);
      result = HBR_SUCCESS;
      break;
    }

    case HBF_HOST_CAPS: {
      // What this front end's backend guarantees. No inputs and no state, so it
      // is safe to call before anything is open - which is the whole point, see
      // the note in hbios_dispatch.h. The value comes from emu_host_path_caps(),
      // which each backend defines: the core deliberately does not, so a port
      // that has not been updated fails to link rather than assert a guarantee
      // its code does not make.
      cpu->regs.DE.set_low(emu_host_path_caps());
      cpu->regs.DE.set_high(0);   // reserved for a second byte of bits
      result = HBR_SUCCESS;
      break;
    }

    case HBF_HOST_GETARG: {
      // Get command line argument by index
      // Input: C = argument index (0 = first arg after command), DE = buffer address
      // Output: A = 0 success (buffer filled), A = 0xFF no such argument
      uint8_t arg_idx = cpu->regs.BC.get_low();
      uint16_t buf_addr = cpu->regs.DE.get_pair16();

      // Parse host_cmd_line to find the requested argument
      // Arguments are space-separated
      if (host_cmd_line.empty()) {
        result = HBR_FAILED;
        break;
      }

      const char* p = host_cmd_line.c_str();
      int current_arg = 0;

      while (*p) {
        // Skip leading spaces
        while (*p == ' ') p++;
        if (!*p) break;

        // Found start of an argument
        const char* arg_start = p;

        // Find end of argument
        while (*p && *p != ' ') p++;

        if (current_arg == arg_idx) {
          // Copy this argument to buffer. The terminator has to be clamped
          // along with the copy: an argument longer than 255 characters left
          // the guest's buffer unterminated AND dropped a zero byte past the
          // end of it, since this took the unclamped length. Unlike
          // HBF_HOST_GETNAME this call takes no size from the guest - C is the
          // index - so 256 bytes is the implied contract and 255 characters
          // plus a terminator is what fits.
          size_t len = p - arg_start;
          if (len > 255) len = 255;
          for (size_t i = 0; i < len; i++) {
            memory->store_mem((uint16_t)(buf_addr + i), arg_start[i]);
          }
          memory->store_mem((uint16_t)(buf_addr + len), 0);  // Null terminate
          result = HBR_SUCCESS;
          break;
        }
        current_arg++;
      }

      if (result != HBR_SUCCESS) {
        result = HBR_FAILED;  // Argument not found
      }
      break;
    }

    default:
      emu_log("[HBIOS EXT] Unhandled function 0x%02X\n", func);
      result = HBR_NOFUNC;
      break;
  }

  setResult(result);
  doRet();
}

//=============================================================================
// Boot Helper
//=============================================================================

bool HBIOSDispatch::bootFromDevice(const char* cmd_str) {
  if (!cpu || !memory) return false;

  boot_in_progress = true;  // Signal that boot has started (for debugging)

  // Skip leading whitespace
  while (*cmd_str == ' ') cmd_str++;

  // Check for ROM application boot (single letter)
  if (cmd_str[0] != '\0' && cmd_str[1] == '\0' && isalpha(cmd_str[0])) {
    int app_idx = findRomApp(cmd_str[0]);
    if (app_idx >= 0) {
      // Load and boot ROM application
      std::vector<uint8_t> app_data;
      if (!emu_file_load(rom_apps[app_idx].sys_path, app_data)) {
        emu_fatal("[SYSBOOT] Cannot load ROM app: %s\n", rom_apps[app_idx].sys_path.c_str());
      }

      if (app_data.size() < 0x600) {
        emu_fatal("[SYSBOOT] ROM app too small (size=%zu, need at least 0x600)\n", app_data.size());
      }

      // Read metadata from offset 0x5E0
      uint16_t load_addr = (uint16_t)(app_data[0x5EA] | (app_data[0x5EB] << 8));
      uint16_t end_addr = (uint16_t)(app_data[0x5EC] | (app_data[0x5ED] << 8));
      uint16_t entry_addr = (uint16_t)(app_data[0x5EE] | (app_data[0x5EF] << 8));

      if (debug_log) {
        emu_log("[SYSBOOT] ROM app load: 0x%04X-0x%04X entry: 0x%04X\n",
                load_addr, end_addr, entry_addr);
      }

      // Load sectors starting from sector 3 (offset 0x600)
      size_t load_size = end_addr - load_addr;
      size_t sectors = (load_size + 511) / 512;
      uint16_t addr = load_addr;

      for (size_t s = 0; s < sectors && addr < end_addr; s++) {
        size_t offset = 0x600 + s * 512;
        for (size_t i = 0; i < 512 && addr < end_addr && offset + i < app_data.size(); i++) {
          memory->store_mem(addr++, app_data[offset + i]);
        }
      }

      // Jump to entry point
      cpu->regs.PC.set_pair16(entry_addr);
      setResult(HBR_SUCCESS);
      return true;
    }
  }

  // Parse disk boot command: "HD0:0", "0", etc.
  int boot_unit = 0;
  int boot_slice = 0;

  if (emu_strncasecmp(cmd_str, "HD", 2) == 0 || emu_strncasecmp(cmd_str, "MD", 2) == 0) {
    // Parse HDn:s or MDn:s
    const char* p = cmd_str + 2;
    boot_unit = atoi(p);
    const char* colon = strchr(p, ':');
    if (colon) {
      boot_slice = atoi(colon + 1);
    }
  } else if (isdigit(cmd_str[0])) {
    // Just a number - use as unit
    boot_unit = atoi(cmd_str);
  }

  if (boot_unit < 0 || boot_unit >= 16 || !disks[boot_unit].is_open) {
    emu_fatal("[SYSBOOT] Invalid disk unit %d (is_open=%d)\n", boot_unit,
              (boot_unit >= 0 && boot_unit < 16) ? disks[boot_unit].is_open : -1);
  }

  if (debug_log) {
    emu_log("[SYSBOOT] Booting from disk %d slice %d\n", boot_unit, boot_slice);
  }

  // Save boot info for SYSGET_BOOTINFO
  saved_boot_unit = boot_unit;
  saved_boot_slice = boot_slice;

  // Update CB_BOOTVOL in HCB at 0x010D. CBIOS may read this from ROM bank 0
  // (which uses shadow RAM) or from RAM bank 0x80. Write via ROM bank 0 mode
  // to set shadow bits, ensuring reads from either path get the updated value.
  uint8_t saved_bank = memory->get_current_bank();
  memory->select_bank(0x00);  // ROM bank 0 - writes go to shadow RAM + set shadow bit
  memory->store_mem(0x010D, (uint8_t)boot_slice);  // CB_BOOTVOL low byte
  memory->store_mem(0x010E, (uint8_t)boot_unit);   // CB_BOOTVOL high byte
  memory->select_bank(saved_bank);  // Restore previous bank

  if (debug_log) {
    emu_log("[SYSBOOT] Set CB_BOOTVOL=0x%02X%02X (unit=%d, slice=%d)\n",
            boot_unit, boot_slice, boot_unit, boot_slice);
  }

  // Read metadata from offset 0x5E0 (same format as ROM apps)
  uint8_t meta_buf[32];
  size_t meta_read = 0;

  if (disks[boot_unit].file_backed && disks[boot_unit].handle) {
    meta_read = emu_disk_read((emu_disk_handle)disks[boot_unit].handle, 0x5E0, meta_buf, 32);
  } else if (!disks[boot_unit].data.empty() && disks[boot_unit].data.size() >= 0x600) {
    memcpy(meta_buf, &disks[boot_unit].data[0x5E0], 32);
    meta_read = 32;
  }

  if (meta_read < 32) {
    emu_fatal("[SYSBOOT] Cannot read disk metadata (read %zu, need 32)\n", meta_read);
  }

  // Parse metadata (little-endian)
  // Offset 26-27: PR_LOAD (load address)
  // Offset 28-29: PR_END (end address)
  // Offset 30-31: PR_ENTRY (entry point)
  uint16_t load_addr = (uint16_t)(meta_buf[26] | (meta_buf[27] << 8));
  uint16_t end_addr = (uint16_t)(meta_buf[28] | (meta_buf[29] << 8));
  uint16_t entry_addr = (uint16_t)(meta_buf[30] | (meta_buf[31] << 8));

  if (debug_log) {
    emu_log("[SYSBOOT] Load: 0x%04X-0x%04X Entry: 0x%04X\n",
            load_addr, end_addr, entry_addr);
  }

  // Load sectors starting from sector 3 (offset 0x600)
  size_t load_size = end_addr - load_addr;
  size_t sectors = (load_size + 511) / 512;
  uint16_t addr = load_addr;

  for (size_t s = 0; s < sectors && addr < end_addr; s++) {
    uint8_t sector_buf[512];
    size_t offset = 0x600 + s * 512;
    size_t read = 0;

    if (disks[boot_unit].file_backed && disks[boot_unit].handle) {
      read = emu_disk_read((emu_disk_handle)disks[boot_unit].handle, offset, sector_buf, 512);
    } else if (offset < disks[boot_unit].data.size()) {
      // offset can pass the end of a truncated image; subtracting first would
      // wrap the size_t and turn the memcpy into an out-of-bounds read.
      size_t avail = disks[boot_unit].data.size() - offset;
      read = (avail < 512) ? avail : 512;
      memcpy(sector_buf, &disks[boot_unit].data[offset], read);
    }

    for (size_t i = 0; i < read && addr < end_addr; i++) {
      memory->store_mem(addr++, sector_buf[i]);
    }
  }

  if (debug_log) {
    emu_log("[SYSBOOT] Loaded %d bytes, jumping to 0x%04X\n",
            (int)(addr - load_addr), entry_addr);
  }

  // Set up boot registers
  cpu->regs.DE.set_high((uint8_t)boot_unit);
  cpu->regs.DE.set_low(0);

  // Jump to entry point
  cpu->regs.PC.set_pair16(entry_addr);
  setResult(HBR_SUCCESS);
  return true;
}

//=============================================================================
// NVRAM Boot Configuration - String-based API
//=============================================================================

void HBIOSDispatch::setNvramSetting(const std::string& setting) {
  if (setting.empty()) {
    // Clear boot option - leave NVRAM uninitialized (shows menu)
    nvram_switches[0] = 0;  // Not initialized
    nvram_dirty = true;
    if (debug_log) {
      emu_log("[NVRAM] Cleared (uninitialized)\n");
    }
    return;
  }

  // Initialize NVRAM
  nvram_switches[0] = 'W';  // Fully initialized

  char first_char = setting[0];

  if (isalpha(first_char)) {
    // ROM app boot - use the letter as the app selection
    char app_char = (char)toupper(first_char);
    nvram_switches[1] = app_char;         // L = app character
    nvram_switches[2] = BOPTS_ROM;        // H = ROM boot flag (bit 7)
    nvram_switches[3] = ABOOT_AUTO;       // Enable autoboot, 0 timeout (immediate)

    if (debug_log) {
      emu_log("[NVRAM] Set ROM app '%c'\n", app_char);
    }
  } else if (isdigit(first_char)) {
    // Disk boot - parse unit and optional slice
    int unit = 0;
    int slice = 0;

    // Check for unit.slice format
    size_t dot_pos = setting.find('.');
    if (dot_pos != std::string::npos) {
      unit = atoi(setting.substr(0, dot_pos).c_str());
      slice = atoi(setting.substr(dot_pos + 1).c_str());
    } else {
      unit = atoi(setting.c_str());
    }

    // Clamp values to valid ranges
    if (unit < 0) unit = 0;
    if (unit > 127) unit = 127;
    if (slice < 0) slice = 0;
    if (slice > 255) slice = 255;

    nvram_switches[1] = (uint8_t)slice;            // L = slice number
    nvram_switches[2] = (uint8_t)(unit & BOPTS_UNIT);  // H = unit (bit 7 clear = disk)
    nvram_switches[3] = ABOOT_AUTO;                // Enable autoboot, 0 timeout

    if (debug_log) {
      emu_log("[NVRAM] Set disk unit=%d slice=%d\n", unit, slice);
    }
  } else {
    // Invalid format - treat as help request
    nvram_switches[1] = 'H';              // L = help
    nvram_switches[2] = BOPTS_ROM;        // H = ROM boot flag
    nvram_switches[3] = 0;                // Disable autoboot (show menu)

    if (debug_log) {
      emu_log("[NVRAM] Invalid setting '%s', defaulting to menu\n", setting.c_str());
    }
  }

  // Calculate checksum (also sets dirty flag)
  recalcNvramChecksum();
}

std::string HBIOSDispatch::getNvramSetting() {
  // Clear dirty flag - caller is reading the value
  nvram_dirty = false;

  // Check if NVRAM is initialized
  if (nvram_switches[0] != 'W') {
    return "";  // Uninitialized
  }

  bool is_rom = (nvram_switches[2] & BOPTS_ROM) != 0;

  if (is_rom) {
    // ROM app - return the app character
    char app_char = nvram_switches[1];
    if (app_char >= 'A' && app_char <= 'Z') {
      return std::string(1, app_char);
    }
    return "H";  // Default to help menu
  } else {
    // Disk boot - return "unit" or "unit.slice"
    int unit = nvram_switches[2] & BOPTS_UNIT;
    int slice = nvram_switches[1];

    if (slice == 0) {
      return std::to_string(unit);
    } else {
      return std::to_string(unit) + "." + std::to_string(slice);
    }
  }
}

bool HBIOSDispatch::hasNvramChange() {
  return nvram_dirty;
}
