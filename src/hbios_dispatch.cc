/*
 * HBIOS Dispatch - Shared RomWBW HBIOS Handler Implementation
 *
 * Uses emu_io.h for all platform-independent I/O operations.
 */

#include "hbios_dispatch.h"
#include "emu_io.h"
#include "qkz80.h"
#include "romwbw_mem.h"
#include <cstring>
#include <cctype>
#include <cmath>

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
}

void HBIOSDispatch::reset() {
  trapping_enabled = false;
  waiting_for_input = false;
  main_entry = 0xFFF0;

  cio_dispatch = 0;
  dio_dispatch = 0;
  rtc_dispatch = 0;
  sys_dispatch = 0;
  vda_dispatch = 0;
  snd_dispatch = 0;

  signal_state = 0;
  signal_addr = 0;
  cur_bank = 0;

  vda_rows = 25;
  vda_cols = 80;
  vda_cursor_row = 0;
  vda_cursor_col = 0;
  vda_attr = 0x07;

  for (int i = 0; i < 4; i++) {
    snd_volume[i] = 0;
    snd_period[i] = 0;
  }
  snd_duration = 100;

  // Initialize NVRAM with valid RomWBW header if empty
  // First byte 'W' indicates NVRAM is initialized
  if (nvram[0] != 'W') {
    nvram[0] = 'W';
  }
}

//=============================================================================
// NVRAM Management
//=============================================================================

void HBIOSDispatch::loadNvram(const uint8_t* data, int size) {
  if (!data || size <= 0) {
    // Initialize with valid RomWBW NVRAM header if no data provided
    // First byte 'W' indicates NVRAM is initialized
    nvram[0] = 'W';
    if (debug) {
      emu_log("[HBIOS] NVRAM initialized with default header\n");
    }
    return;
  }
  int copy_size = (size < NVRAM_SIZE) ? size : NVRAM_SIZE;
  memcpy(nvram, data, copy_size);
  if (debug) {
    emu_log("[HBIOS] NVRAM loaded: %d bytes\n", copy_size);
  }
}

//=============================================================================
// Disk Management
//=============================================================================

bool HBIOSDispatch::loadDisk(int unit, const uint8_t* data, size_t size) {
  if (unit < 0 || unit >= 16) return false;

  closeDisk(unit);

  disks[unit].data.assign(data, data + size);
  disks[unit].size = size;
  disks[unit].is_open = true;
  disks[unit].file_backed = false;
  disks[unit].handle = nullptr;

  if (debug) {
    emu_log("[HBIOS] Loaded disk %d: %zu bytes (in-memory)\n", unit, size);
  }
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
      emu_error("[HBIOS] Cannot open disk file: %s\n", path.c_str());
      return false;
    }
  }

  disks[unit].handle = handle;
  disks[unit].path = path;
  disks[unit].size = emu_disk_size(handle);
  disks[unit].is_open = true;
  disks[unit].file_backed = true;

  if (debug) {
    emu_log("[HBIOS] Loaded disk %d: %s (%zu bytes)\n", unit, path.c_str(), disks[unit].size);
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
  char upper_key = std::toupper(key);
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
  // Protocol 1 (simple status, used by altair_emu):
  //   0x01 = HBIOS starting
  //   0xFE = PREINIT point
  //   0xFF = Init complete, enable trapping
  //
  // Protocol 2 (address registration, used by romwbw_web):
  //   State machine: 1/2=CIO, 3/4=DIO, 5/6=RTC, 7/8=SYS, 9/10=VDA, 11/12=SND
  //   First value sets state, then low byte, then high byte

  if (signal_state == 0) {
    // Check for special signals (Protocol 1)
    switch (value) {
      case 0x01:  // HBIOS starting
        if (debug) emu_log("[HBIOS] Boot code starting...\n");
        return;

      case 0xFE:  // PREINIT point (test mode)
        if (debug) emu_log("[HBIOS] PREINIT point reached\n");
        return;

      case 0xFF:  // Init complete - enable trapping
        trapping_enabled = true;
        if (debug) {
          emu_log("[HBIOS] Init complete, trapping enabled at 0x%04X\n", main_entry);
        }
        return;

      // Protocol 2: Start address registration (0x10-0x15 range)
      // These don't conflict with Protocol 1 signals
      case 0x10:  // Start CIO registration
      case 0x11:  // Start DIO registration
      case 0x12:  // Start RTC registration
      case 0x13:  // Start SYS registration
      case 0x14:  // Start VDA registration
      case 0x15:  // Start SND registration
        signal_state = value;
        signal_addr = 0;
        return;

      default:
        if (debug) emu_log("[HBIOS] Unknown signal: 0x%02X\n", value);
        return;
    }
  }

  // Protocol 2: Address registration state machine (0x10-0x15 + low/high bytes)
  // Each registration starts with 0x1X, then expects low byte, then high byte
  uint8_t handler_type = signal_state;
  bool is_low_byte = (signal_addr == 0);

  if (is_low_byte) {
    // Receiving low byte
    signal_addr = value;
  } else {
    // Receiving high byte - complete registration
    uint16_t addr = signal_addr | (value << 8);

    switch (handler_type) {
      case 0x10:
        cio_dispatch = addr;
        if (debug) emu_log("[HBIOS] CIO dispatch at 0x%04X\n", cio_dispatch);
        break;
      case 0x11:
        dio_dispatch = addr;
        if (debug) emu_log("[HBIOS] DIO dispatch at 0x%04X\n", dio_dispatch);
        break;
      case 0x12:
        rtc_dispatch = addr;
        if (debug) emu_log("[HBIOS] RTC dispatch at 0x%04X\n", rtc_dispatch);
        break;
      case 0x13:
        sys_dispatch = addr;
        if (debug) emu_log("[HBIOS] SYS dispatch at 0x%04X\n", sys_dispatch);
        break;
      case 0x14:
        vda_dispatch = addr;
        if (debug) emu_log("[HBIOS] VDA dispatch at 0x%04X\n", vda_dispatch);
        break;
      case 0x15:
        snd_dispatch = addr;
        if (debug) emu_log("[HBIOS] SND dispatch at 0x%04X\n", snd_dispatch);
        break;
    }
    signal_state = 0;
    signal_addr = 0;
  }
}

//=============================================================================
// Trap Detection
//=============================================================================

bool HBIOSDispatch::checkTrap(uint16_t pc) const {
  if (!trapping_enabled) return false;

  // Main entry point (0xFFF0 by default)
  if (pc == main_entry) return true;

  // Per-handler dispatch addresses (optional)
  if (pc == cio_dispatch && cio_dispatch != 0) return true;
  if (pc == dio_dispatch && dio_dispatch != 0) return true;
  if (pc == rtc_dispatch && rtc_dispatch != 0) return true;
  if (pc == sys_dispatch && sys_dispatch != 0) return true;
  if (pc == vda_dispatch && vda_dispatch != 0) return true;
  if (pc == snd_dispatch && snd_dispatch != 0) return true;
  return false;
}

int HBIOSDispatch::getTrapType(uint16_t pc) const {
  // Main entry uses function code in B register
  if (pc == main_entry) return -2;  // Special: dispatch by B register

  if (pc == cio_dispatch && cio_dispatch != 0) return 0;
  if (pc == dio_dispatch && dio_dispatch != 0) return 1;
  if (pc == rtc_dispatch && rtc_dispatch != 0) return 2;
  if (pc == sys_dispatch && sys_dispatch != 0) return 3;
  if (pc == vda_dispatch && vda_dispatch != 0) return 4;
  if (pc == snd_dispatch && snd_dispatch != 0) return 5;
  return -1;
}

int HBIOSDispatch::getTrapTypeFromFunc(uint8_t func) {
  // Determine handler type from HBIOS function code in B register
  if (func <= 0x0F) return 0;        // CIO (0x00-0x0F)
  if (func <= 0x1F) return 1;        // DIO (0x10-0x1F)
  if (func <= 0x2F) return 2;        // RTC (0x20-0x2F)
  if (func <= 0x3F) return 6;        // DSKY (0x30-0x3F)
  if (func <= 0x4F) return 4;        // VDA (0x40-0x4F)
  if (func <= 0x5F) return 5;        // SND (0x50-0x5F)
  if (func >= 0xF0) return 3;        // SYS (0xF0-0xFF)
  return -1;
}

bool HBIOSDispatch::handleCall(int trap_type) {
  switch (trap_type) {
    case -2: return handleMainEntry();  // Dispatch by B register
    case 0: handleCIO(); break;
    case 1: handleDIO(); break;
    case 2: handleRTC(); break;
    case 3: handleSYS(); break;
    case 4: handleVDA(); break;
    case 5: handleSND(); break;
    case 6: handleDSKY(); break;
    default: return false;
  }
  return true;
}

bool HBIOSDispatch::handleMainEntry() {
  if (!cpu) return false;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t unit = cpu->regs.BC.get_low();
  int trap_type = getTrapTypeFromFunc(func);

  if (debug) {
    uint16_t de = cpu->regs.DE.get_pair16();
    uint16_t hl = cpu->regs.HL.get_pair16();
    emu_log("[HBIOS] func=0x%02X unit=0x%02X DE=0x%04X HL=0x%04X type=%d\n",
            func, unit, de, hl, trap_type);
  }

  switch (trap_type) {
    case 0: handleCIO(); return true;
    case 1: handleDIO(); return true;
    case 2: handleRTC(); return true;
    case 3: handleSYS(); return true;
    case 4: handleVDA(); return true;
    case 5: handleSND(); return true;
    case 6: handleDSKY(); return true;
    default:
      // Unknown function - return error and RET
      if (debug) {
        emu_log("[HBIOS] Unknown function 0x%02X (trap_type=%d)\n", func, trap_type);
      }
      cpu->regs.AF.set_high(HBR_FAILED);
      doRet();
      return true;
  }
}

//=============================================================================
// Helper Functions
//=============================================================================

void HBIOSDispatch::doRet() {
  if (!cpu || !memory) return;

  uint16_t sp = cpu->regs.SP.get_pair16();
  uint8_t lo = memory->fetch_mem(sp);
  uint8_t hi = memory->fetch_mem(sp + 1);
  cpu->regs.SP.set_pair16(sp + 2);
  cpu->regs.PC.set_pair16(lo | (hi << 8));
}

void HBIOSDispatch::writeConsoleString(const char* str) {
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

  switch (func) {
    case HBF_CIOIN: {
      // Read character - if none available, wait
      if (!emu_console_has_input()) {
        // No input - set waiting flag and DON'T call doRet()
        // This keeps PC at the trap address so it will retry
        waiting_for_input = true;
        return;  // Don't fall through to doRet()
      }
      int ch = emu_console_read_char();
      cpu->regs.DE.set_low(ch & 0xFF);
      break;
    }

    case HBF_CIOOUT: {
      // Write character
      uint8_t ch = cpu->regs.DE.get_low();
      emu_console_write_char(ch);
      break;
    }

    case HBF_CIOIST: {
      // Input status
      cpu->regs.DE.set_low(emu_console_has_input() ? 0xFF : 0x00);
      break;
    }

    case HBF_CIOOST: {
      // Output status - always ready
      cpu->regs.DE.set_low(0xFF);
      break;
    }

    case HBF_CIOINIT:
      // Initialize - nothing to do
      break;

    case HBF_CIOQUERY: {
      // Query - return capabilities
      // D = device type (0 = UART)
      // E = device number
      cpu->regs.DE.set_high(0x00);  // UART
      cpu->regs.DE.set_low(unit);
      break;
    }

    case HBF_CIODEVICE: {
      // Device info
      // DE = device attributes
      cpu->regs.DE.set_pair16(0x0000);
      break;
    }

    default:
      if (debug) {
        emu_log("[HBIOS CIO] Unhandled function 0x%02X\n", func);
      }
      result = HBR_FAILED;
      break;
  }

  cpu->regs.AF.set_high(result);
  doRet();
}

//=============================================================================
// Disk I/O (DIO)
//=============================================================================

void HBIOSDispatch::handleDIO() {
  if (!cpu || !memory) return;

  uint8_t func = cpu->regs.BC.get_high();  // B = function
  uint8_t unit = cpu->regs.BC.get_low();   // C = unit
  uint8_t result = HBR_SUCCESS;

  switch (func) {
    case HBF_DIOSTATUS: {
      // Get status
      if (unit < 16 && disks[unit].is_open) {
        cpu->regs.DE.set_low(0x00);  // Ready
      } else {
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_DIORESET:
      // Reset - nothing to do
      disks[unit].current_lba = 0;
      break;

    case HBF_DIOSEEK: {
      // Seek to LBA
      // Input: BC=Function/Unit, DE:HL=LBA (32-bit: DE=high16, HL=low16)
      // Bit 31 (0x80 in high byte of DE) = LBA mode flag, mask it off
      uint16_t de_reg = cpu->regs.DE.get_pair16();
      uint16_t hl_reg = cpu->regs.HL.get_pair16();
      uint32_t lba = (((uint32_t)(de_reg & 0x7FFF) << 16) | hl_reg);

      if (unit < 16 && disks[unit].is_open) {
        disks[unit].current_lba = lba;
        if (debug) {
          emu_log("[HBIOS DIO SEEK] unit=%d lba=%u\n", unit, lba);
        }
      } else {
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_DIOREAD: {
      // Read sectors using current_lba (set by DIOSEEK)
      // Input: BC=Function/Unit, HL=Buffer Address, D=Buffer Bank (0x80=use current), E=Block Count
      // Output: A=Result, E=Blocks Read
      // LBA comes from current_lba set by prior DIOSEEK call

      if (unit >= 16 || !disks[unit].is_open) {
        result = HBR_FAILED;
        cpu->regs.DE.set_low(0);
        break;
      }

      uint16_t buffer = cpu->regs.HL.get_pair16();
      uint8_t buffer_bank = cpu->regs.DE.get_high();
      uint8_t count = cpu->regs.DE.get_low();
      uint32_t lba = disks[unit].current_lba;

      if (debug) {
        emu_log("[HBIOS DIO READ] unit=%d lba=%u count=%d buf=0x%04X bank=0x%02X\n",
                unit, lba, count, buffer, buffer_bank);
      }

      uint8_t blocks_read = 0;

      if (disks[unit].file_backed && disks[unit].handle) {
        // Read from file
        uint8_t sector_buf[512];
        for (int s = 0; s < count; s++) {
          size_t offset = (lba + s) * 512;
          size_t read = emu_disk_read((emu_disk_handle)disks[unit].handle,
                                      offset, sector_buf, 512);
          if (read == 0) {
            break;
          }
          // Copy to Z80 memory
          for (size_t i = 0; i < 512; i++) {
            memory->store_mem(buffer + s * 512 + i, sector_buf[i]);
          }
          blocks_read++;
        }
      } else if (!disks[unit].data.empty()) {
        // Read from memory buffer
        for (int s = 0; s < count; s++) {
          size_t offset = (lba + s) * 512;
          if (offset + 512 > disks[unit].data.size()) {
            break;
          }
          for (size_t i = 0; i < 512; i++) {
            memory->store_mem(buffer + s * 512 + i, disks[unit].data[offset + i]);
          }
          blocks_read++;
        }
      } else {
        result = HBR_FAILED;
      }

      // Update current_lba for next sequential access
      disks[unit].current_lba += blocks_read;
      cpu->regs.DE.set_low(blocks_read);
      break;
    }

    case HBF_DIOWRITE: {
      // Write sectors using current_lba (set by DIOSEEK)
      // Input: BC=Function/Unit, HL=Buffer Address, D=Buffer Bank (0x80=use current), E=Block Count
      // Output: A=Result, E=Blocks Written
      // LBA comes from current_lba set by prior DIOSEEK call

      if (unit >= 16 || !disks[unit].is_open) {
        result = HBR_FAILED;
        cpu->regs.DE.set_low(0);
        break;
      }

      uint16_t buffer = cpu->regs.HL.get_pair16();
      uint8_t buffer_bank = cpu->regs.DE.get_high();
      uint8_t count = cpu->regs.DE.get_low();
      uint32_t lba = disks[unit].current_lba;

      if (debug) {
        emu_log("[HBIOS DIO WRITE] unit=%d lba=%u count=%d buf=0x%04X bank=0x%02X\n",
                unit, lba, count, buffer, buffer_bank);
      }

      uint8_t blocks_written = 0;

      if (disks[unit].file_backed && disks[unit].handle) {
        uint8_t sector_buf[512];
        for (int s = 0; s < count; s++) {
          size_t offset = (lba + s) * 512;
          for (size_t i = 0; i < 512; i++) {
            sector_buf[i] = memory->fetch_mem(buffer + s * 512 + i);
          }
          emu_disk_write((emu_disk_handle)disks[unit].handle, offset, sector_buf, 512);
          blocks_written++;
        }
        emu_disk_flush((emu_disk_handle)disks[unit].handle);
      } else if (!disks[unit].data.empty()) {
        for (int s = 0; s < count; s++) {
          size_t offset = (lba + s) * 512;
          if (offset + 512 > disks[unit].data.size()) {
            disks[unit].data.resize(offset + 512);
          }
          for (size_t i = 0; i < 512; i++) {
            disks[unit].data[offset + i] = memory->fetch_mem(buffer + s * 512 + i);
          }
          blocks_written++;
        }
      } else {
        result = HBR_FAILED;
      }

      // Update current_lba for next sequential access
      disks[unit].current_lba += blocks_written;
      cpu->regs.DE.set_low(blocks_written);
      break;
    }

    case HBF_DIOFORMAT:
      // Format track - not supported in emulator
      result = HBR_NOTIMPL;
      break;

    case HBF_DIODEVICE: {
      // Disk device info report
      // Returns device type and attributes
      if (unit < 16 && disks[unit].is_open) {
        cpu->regs.DE.set_high(0x03);  // DIODEV_IDE (hard disk type)
        cpu->regs.DE.set_low(0x00);   // Subtype
      } else {
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_DIOMEDIA: {
      // Disk media report - return media type
      if (unit < 16 && disks[unit].is_open) {
        cpu->regs.DE.set_low(MID_HD);  // Hard disk media
      } else {
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_DIODEFMED:
      // Define disk media - not supported in emulator
      result = HBR_NOTIMPL;
      break;

    case HBF_DIOCAP: {
      // Get capacity
      if (unit < 16 && disks[unit].is_open) {
        uint32_t sectors = disks[unit].size / 512;
        cpu->regs.DE.set_pair16(sectors & 0xFFFF);
        cpu->regs.HL.set_pair16((sectors >> 16) & 0xFFFF);
      } else {
        result = HBR_FAILED;
      }
      break;
    }

    case HBF_DIOGEOM: {
      // Get geometry
      // Returns: C=sectors/track, D=heads, E=tracks (for CHS addressing)
      // For LBA-only, return dummy values
      cpu->regs.BC.set_low(63);   // 63 sectors/track
      cpu->regs.DE.set_high(16);  // 16 heads
      cpu->regs.DE.set_low(255);  // 255 tracks
      break;
    }

    default:
      if (debug) {
        emu_log("[HBIOS DIO] Unhandled function 0x%02X\n", func);
      }
      result = HBR_FAILED;
      break;
  }

  cpu->regs.AF.set_high(result);
  doRet();
}

//=============================================================================
// Real-Time Clock (RTC)
//=============================================================================

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

      // RomWBW format: YY MM DD HH MM SS (BCD)
      auto to_bcd = [](int v) -> uint8_t {
        return ((v / 10) << 4) | (v % 10);
      };

      memory->store_mem(buffer + 0, to_bcd(t.year % 100));
      memory->store_mem(buffer + 1, to_bcd(t.month));
      memory->store_mem(buffer + 2, to_bcd(t.day));
      memory->store_mem(buffer + 3, to_bcd(t.hour));
      memory->store_mem(buffer + 4, to_bcd(t.minute));
      memory->store_mem(buffer + 5, to_bcd(t.second));
      break;
    }

    case HBF_RTCSETTIM:
      // Set time - ignored in emulator
      break;

    case HBF_RTCGETBYT: {
      // Get NVRAM byte by index
      // C = index, returns E = value
      uint8_t index = cpu->regs.BC.get_low();
      if (index < NVRAM_SIZE) {
        cpu->regs.DE.set_low(nvram[index]);
        if (debug) {
          emu_log("[HBIOS RTC] GETBYT index=%d value=0x%02X\n", index, nvram[index]);
        }
      } else {
        cpu->regs.DE.set_low(0);
        result = HBR_RANGE;
      }
      break;
    }

    case HBF_RTCSETBYT: {
      // Set NVRAM byte by index
      // C = index, E = value
      uint8_t index = cpu->regs.BC.get_low();
      uint8_t value = cpu->regs.DE.get_low();
      if (index < NVRAM_SIZE) {
        nvram[index] = value;
        if (debug) {
          emu_log("[HBIOS RTC] SETBYT index=%d value=0x%02X\n", index, value);
        }
        // Notify save callback
        if (nvram_save_callback) {
          nvram_save_callback(nvram, NVRAM_SIZE);
        }
      } else {
        result = HBR_RANGE;
      }
      break;
    }

    case HBF_RTCGETBLK: {
      // Get NVRAM data block
      // HL = buffer address, returns buffer filled with NVRAM contents
      uint16_t buffer = cpu->regs.HL.get_pair16();
      for (int i = 0; i < NVRAM_SIZE && memory; i++) {
        memory->store_mem(buffer + i, nvram[i]);
      }
      cpu->regs.DE.set_low(NVRAM_SIZE);  // Return size in E
      if (debug) {
        emu_log("[HBIOS RTC] GETBLK buffer=0x%04X size=%d\n", buffer, NVRAM_SIZE);
      }
      break;
    }

    case HBF_RTCSETBLK: {
      // Set NVRAM data block
      // HL = buffer address, C = count
      uint16_t buffer = cpu->regs.HL.get_pair16();
      uint8_t count = cpu->regs.BC.get_low();
      if (count > NVRAM_SIZE) count = NVRAM_SIZE;
      for (int i = 0; i < count && memory; i++) {
        nvram[i] = memory->fetch_mem(buffer + i);
      }
      if (debug) {
        emu_log("[HBIOS RTC] SETBLK buffer=0x%04X count=%d\n", buffer, count);
      }
      // Notify save callback
      if (nvram_save_callback) {
        nvram_save_callback(nvram, NVRAM_SIZE);
      }
      break;
    }

    case HBF_RTCDEVICE: {
      // RTC device info
      // Return device type in D, attributes in E
      cpu->regs.DE.set_high(0x01);  // Device type: DS1302/generic RTC
      cpu->regs.DE.set_low(0x01);   // Attributes: has NVRAM
      break;
    }

    default:
      if (debug) {
        emu_log("[HBIOS RTC] Unhandled function 0x%02X\n", func);
      }
      result = HBR_FAILED;
      break;
  }

  cpu->regs.AF.set_high(result);
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
      emu_log("[HBIOS SYSRESET] reset_type=0x%02X\n", reset_type);
      if (reset_type == 0x01 || reset_type == 0x02) {
        // Call the reset callback if set
        if (reset_callback) {
          reset_callback(reset_type);
          // Don't call doRet() - the callback sets PC directly
          return;
        }
      }
      // Other reset types or no callback - just return success
      break;
    }

    case HBF_SYSVER: {
      // Get HBIOS version
      // DE = version (e.g., 0x362B = 3.6.0.43)
      // L = platform ID
      cpu->regs.DE.set_pair16(0x362B);
      cpu->regs.HL.set_low(0x00);  // Platform = EMU
      break;
    }

    case HBF_SYSSETBNK: {
      // Set current bank
      // Input: L = bank ID to set
      uint8_t bank = cpu->regs.HL.get_low();
      if (memory) {
        memory->select_bank(bank);
      }
      cur_bank = bank;
      if (debug) {
        emu_log("[HBIOS] SYSSETBNK bank=0x%02X\n", bank);
      }
      break;
    }

    case HBF_SYSGETBNK: {
      // Get current bank
      // Output: L = current bank ID
      uint8_t bank = cur_bank;
      if (memory) {
        bank = memory->get_current_bank();
      }
      cpu->regs.HL.set_low(bank);
      break;
    }

    case HBF_SYSGET: {
      // Get system info - subfunc in C
      switch (subfunc) {
        case SYSGET_CIOCNT:
          // Number of CIO devices
          cpu->regs.DE.set_low(1);  // 1 console
          break;

        case SYSGET_DIOCNT: {
          // Number of DIO devices
          int count = 0;
          for (int i = 0; i < 16; i++) {
            if (disks[i].is_open) count++;
          }
          cpu->regs.DE.set_low(count);
          break;
        }

        case SYSGET_VDACNT:
          cpu->regs.DE.set_low(1);  // 1 VDA
          break;

        case SYSGET_SNDCNT:
          cpu->regs.DE.set_low(1);  // 1 sound device
          break;

        case SYSGET_BOOTINFO:
          // Boot info - return boot device (unit 0)
          cpu->regs.DE.set_low(0);
          break;

        case SYSGET_BNKINFO:
          // Bank info: D = BIOS bank, E = user bank
          cpu->regs.DE.set_high(0x80);  // BIOS in bank 0
          cpu->regs.DE.set_low(0x8E);   // User in bank 14
          break;

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

        default:
          if (debug) {
            emu_log("[HBIOS SYSGET] Unhandled subfunction 0x%02X\n", subfunc);
          }
          break;
      }
      break;
    }

    case HBF_SYSPEEK: {
      // Peek byte from bank
      uint8_t bank = cpu->regs.DE.get_high();
      uint16_t addr = cpu->regs.HL.get_pair16();
      uint8_t byte = 0xFF;

      if (memory) {
        if (addr < 0x8000) {
          byte = memory->read_bank(bank, addr);
        } else {
          byte = memory->fetch_mem(addr);
        }
      }
      cpu->regs.DE.set_low(byte);
      if (debug) {
        emu_log("[SYSPEEK] bank=0x%02X addr=0x%04X -> 0x%02X\n", bank, addr, byte);
      }
      break;
    }

    case HBF_SYSPOKE: {
      // Poke byte to bank
      uint8_t bank = cpu->regs.DE.get_high();
      uint8_t byte = cpu->regs.DE.get_low();
      uint16_t addr = cpu->regs.HL.get_pair16();

      if (memory) {
        if (addr < 0x8000) {
          memory->write_bank(bank, addr, byte);
        } else {
          memory->store_mem(addr, byte);
        }
      }
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
        uint8_t c = memory->fetch_mem(cmd_addr + i);
        if (c == 0 || c == '\r' || c == '\n') break;
        cmd_str[i++] = c;
      }
      cmd_str[i] = '\0';

      if (debug) {
        emu_log("[SYSBOOT] Command string: '%s'\n", cmd_str);
      }

      // Skip leading whitespace
      char* p = cmd_str;
      while (*p == ' ') p++;

      // Try to boot
      if (!bootFromDevice(p)) {
        result = HBR_FAILED;
      }
      break;
    }

    default:
      if (debug) {
        emu_log("[HBIOS SYS] Unhandled function 0x%02X\n", func);
      }
      result = HBR_FAILED;
      break;
  }

  cpu->regs.AF.set_high(result);
  doRet();
}

//=============================================================================
// Video Display Adapter (VDA)
//=============================================================================

void HBIOSDispatch::handleVDA() {
  if (!cpu) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t result = HBR_SUCCESS;

  switch (func) {
    case HBF_VDAINI:
    case HBF_VDARES:
      vda_cursor_row = 0;
      vda_cursor_col = 0;
      vda_attr = 0x07;
      emu_video_clear();
      break;

    case HBF_VDAQRY: {
      // Query - return rows/cols
      cpu->regs.DE.set_high(vda_cols);
      cpu->regs.DE.set_low(vda_rows);
      break;
    }

    case HBF_VDASCP: {
      // Set cursor position
      vda_cursor_row = cpu->regs.DE.get_high();
      vda_cursor_col = cpu->regs.DE.get_low();
      emu_video_set_cursor(vda_cursor_row, vda_cursor_col);
      break;
    }

    case HBF_VDASAT: {
      // Set attribute
      vda_attr = cpu->regs.DE.get_low();
      emu_video_set_attr(vda_attr);
      break;
    }

    case HBF_VDASCO: {
      // Set color
      // D = foreground, E = background (CGA 16-color)
      uint8_t fg = cpu->regs.DE.get_high();
      uint8_t bg = cpu->regs.DE.get_low();
      vda_attr = (bg << 4) | (fg & 0x0F);
      emu_video_set_attr(vda_attr);
      break;
    }

    case HBF_VDAWRC: {
      // Write character at cursor
      uint8_t ch = cpu->regs.DE.get_low();
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
      // Scroll
      int lines = cpu->regs.DE.get_low();
      emu_video_scroll_up(lines);
      break;
    }

    case HBF_VDAKST: {
      // Keyboard status
      cpu->regs.DE.set_low(emu_console_has_input() ? 0xFF : 0x00);
      break;
    }

    case HBF_VDAKRD: {
      // Keyboard read - if none available, wait
      if (!emu_console_has_input()) {
        // No input - set waiting flag and DON'T call doRet()
        waiting_for_input = true;
        return;  // Don't fall through to doRet()
      }
      int ch = emu_console_read_char();
      cpu->regs.DE.set_low(ch & 0xFF);
      break;
    }

    case HBF_VDARDC: {
      // Read character at cursor - return space (not implemented)
      cpu->regs.DE.set_low(' ');
      break;
    }

    default:
      if (debug) {
        emu_log("[HBIOS VDA] Unhandled function 0x%02X\n", func);
      }
      break;
  }

  cpu->regs.AF.set_high(result);
  doRet();
}

//=============================================================================
// Sound (SND)
//=============================================================================

void HBIOSDispatch::handleSND() {
  if (!cpu) return;

  uint8_t func = cpu->regs.BC.get_high();
  uint8_t channel = cpu->regs.BC.get_low();
  uint8_t result = HBR_SUCCESS;

  switch (func) {
    case HBF_SNDRESET:
      for (int i = 0; i < 4; i++) {
        snd_volume[i] = 0;
        snd_period[i] = 0;
      }
      snd_duration = 100;
      break;

    case HBF_SNDVOL:
      if (channel < 4) {
        snd_volume[channel] = cpu->regs.DE.get_low();
      }
      break;

    case HBF_SNDPRD:
      if (channel < 4) {
        snd_period[channel] = cpu->regs.DE.get_pair16();
      }
      break;

    case HBF_SNDNOTE: {
      // Set note (MIDI note number)
      // Convert to period: period = 1000000 / frequency
      // MIDI note to freq: freq = 440 * 2^((note-69)/12)
      uint8_t note = cpu->regs.DE.get_low();
      if (channel < 4 && note > 0) {
        double freq = 440.0 * pow(2.0, (note - 69) / 12.0);
        snd_period[channel] = (uint16_t)(1000000.0 / freq);
      }
      break;
    }

    case HBF_SNDDUR:
      snd_duration = cpu->regs.DE.get_pair16();
      break;

    case HBF_SNDPLAY:
      // Play sound - use channel 0's period and duration
      if (snd_period[0] > 0 && snd_volume[0] > 0) {
        int duration_ms = snd_duration;
        emu_dsky_beep(duration_ms);
      }
      break;

    case HBF_SNDBEEP:
      // Simple beep
      emu_dsky_beep(100);
      break;

    case HBF_SNDQUERY:
      // Query sound capabilities
      cpu->regs.DE.set_pair16(0x0001);  // 1 channel supported
      break;

    default:
      if (debug) {
        emu_log("[HBIOS SND] Unhandled function 0x%02X\n", func);
      }
      break;
  }

  cpu->regs.AF.set_high(result);
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
      if (debug) {
        emu_log("[HBIOS DSKY] Unhandled function 0x%02X\n", func);
      }
      break;
  }

  cpu->regs.AF.set_high(result);
  doRet();
}

//=============================================================================
// Boot Helper
//=============================================================================

bool HBIOSDispatch::bootFromDevice(const char* cmd_str) {
  if (!cpu || !memory) return false;

  // Skip leading whitespace
  while (*cmd_str == ' ') cmd_str++;

  // Check for ROM application boot (single letter)
  if (cmd_str[0] != '\0' && cmd_str[1] == '\0' && isalpha(cmd_str[0])) {
    int app_idx = findRomApp(cmd_str[0]);
    if (app_idx >= 0) {
      // Load and boot ROM application
      std::vector<uint8_t> app_data;
      if (!emu_file_load(rom_apps[app_idx].sys_path, app_data)) {
        emu_error("[SYSBOOT] Cannot load ROM app: %s\n", rom_apps[app_idx].sys_path.c_str());
        return false;
      }

      if (app_data.size() < 0x600) {
        emu_error("[SYSBOOT] ROM app too small\n");
        return false;
      }

      // Read metadata from offset 0x5E0
      uint16_t load_addr = app_data[0x5EA] | (app_data[0x5EB] << 8);
      uint16_t end_addr = app_data[0x5EC] | (app_data[0x5ED] << 8);
      uint16_t entry_addr = app_data[0x5EE] | (app_data[0x5EF] << 8);

      if (debug) {
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
      cpu->regs.AF.set_high(HBR_SUCCESS);
      return true;
    }
  }

  // Parse disk boot command: "HD0:0", "0", etc.
  int boot_unit = 0;
  int boot_slice = 0;

  if (strncasecmp(cmd_str, "HD", 2) == 0 || strncasecmp(cmd_str, "MD", 2) == 0) {
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
    emu_error("[SYSBOOT] Invalid disk unit %d\n", boot_unit);
    return false;
  }

  if (debug) {
    emu_log("[SYSBOOT] Booting from disk %d slice %d\n", boot_unit, boot_slice);
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
    emu_error("[SYSBOOT] Cannot read disk metadata\n");
    return false;
  }

  // Parse metadata (little-endian)
  // Offset 26-27: PR_LOAD (load address)
  // Offset 28-29: PR_END (end address)
  // Offset 30-31: PR_ENTRY (entry point)
  uint16_t load_addr = meta_buf[26] | (meta_buf[27] << 8);
  uint16_t end_addr = meta_buf[28] | (meta_buf[29] << 8);
  uint16_t entry_addr = meta_buf[30] | (meta_buf[31] << 8);

  if (debug) {
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
    } else if (!disks[boot_unit].data.empty()) {
      size_t avail = disks[boot_unit].data.size() - offset;
      read = (avail < 512) ? avail : 512;
      if (read > 0) {
        memcpy(sector_buf, &disks[boot_unit].data[offset], read);
      }
    }

    for (size_t i = 0; i < read && addr < end_addr; i++) {
      memory->store_mem(addr++, sector_buf[i]);
    }
  }

  if (debug) {
    emu_log("[SYSBOOT] Loaded %d bytes, jumping to 0x%04X\n",
            (int)(addr - load_addr), entry_addr);
  }

  // Set up boot registers
  cpu->regs.DE.set_high(boot_unit);
  cpu->regs.DE.set_low(0);

  // Jump to entry point
  cpu->regs.PC.set_pair16(entry_addr);
  cpu->regs.AF.set_high(HBR_SUCCESS);
  return true;
}
