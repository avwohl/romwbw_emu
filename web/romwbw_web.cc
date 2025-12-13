/*
 * RomWBW Emulator - WebAssembly Version
 *
 * Compiles with Emscripten to run RomWBW in browser.
 * Uses shared HBIOSDispatch for HBIOS emulation.
 * Console I/O via JavaScript callbacks through emu_io.h abstraction.
 */

#include "qkz80.h"  // From cpmemu via -I flag
#include "../src/romwbw_mem.h"
#include "../src/hbios_dispatch.h"
#include "../src/emu_io.h"
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Use banked_mem as cpm_mem for RomWBW
using cpm_mem = banked_mem;

//=============================================================================
// Global State
//=============================================================================

static cpm_mem memory;
static qkz80 cpu(&memory);
static HBIOSDispatch hbios;
static bool running = false;
static bool waiting_for_input = false;
static bool debug = false;

// Boot string for auto-boot feature
static std::string boot_string;
static size_t boot_string_pos = 0;

// Instruction counter
static long long instruction_count = 0;

//=============================================================================
// Signal Port Handler (0xEE)
//=============================================================================

static void handle_emu_signal(uint8_t value) {
  hbios.handleSignalPort(value);
}

//=============================================================================
// I/O Port Handling
//=============================================================================

static int io_in_count = 0;

static uint8_t handle_in(uint8_t port) {
  uint8_t result = 0xFF;

  switch (port) {
    case 0x68:  // UART data
      if (emu_console_has_input()) {
        result = emu_console_read_char() & 0xFF;
      } else {
        result = 0;
      }
      break;

    case 0x6D:  // UART status (SSER)
      // Bit 0: RX ready, Bit 5: TX empty
      result = (emu_console_has_input() ? 0x01 : 0x00) | 0x20;
      break;

    case 0x78:  // Bank register
    case 0x7C:
      result = memory.get_current_bank();
      break;

    default:
      result = 0xFF;
      break;
  }

  if (debug && io_in_count < 50 && port != 0x6D) {  // Skip 0x6D (polled often)
    emu_log("[IN] port=0x%02X -> 0x%02X\n", port, result);
    io_in_count++;
  }

  return result;
}

static int io_out_count = 0;

static void handle_out(uint8_t port, uint8_t value) {
  if (debug && io_out_count < 100) {
    emu_log("[OUT] port=0x%02X value=0x%02X (%c)\n", port, value,
            (value >= 32 && value < 127) ? value : '.');
    io_out_count++;
  }
  switch (port) {
    case 0x68:  // UART data
      emu_console_write_char(value);
      break;

    case 0x78:  // RAM bank
    case 0x7C:  // ROM bank
      memory.select_bank(value);
      break;

    case 0xEE:  // EMU signal port
      handle_emu_signal(value);
      break;
  }
}

//=============================================================================
// Main Execution Loop
//=============================================================================

static void run_batch() {
  if (!running || waiting_for_input) return;

  for (int i = 0; i < 50000 && running && !waiting_for_input; i++) {
    uint16_t pc = cpu.regs.PC.get_pair16();
    uint8_t opcode = memory.fetch_mem(pc) & 0xFF;

    // Log when PC is near HBIOS entry
    static int near_fff0_count = 0;
    if (debug && pc >= 0xFFF0 && pc <= 0xFFFF && near_fff0_count < 10) {
      emu_log("[PC] at 0x%04X, opcode=0x%02X\n", pc, opcode);
      near_fff0_count++;
    }

    // Check for HBIOS trap
    if (hbios.checkTrap(pc)) {
      int trap_type = hbios.getTrapType(pc);
      if (!hbios.handleCall(trap_type)) {
        emu_error("[HBIOS] Failed to handle trap at 0x%04X\n", pc);
      }
      instruction_count++;
      continue;
    }

    // Handle HLT
    if (opcode == 0x76) {
      emu_status("HLT instruction - emulation stopped");
      running = false;
      break;
    }

    // Handle IN instruction (0xDB)
    if (opcode == 0xDB) {
      uint8_t port = memory.fetch_mem(pc + 1) & 0xFF;
      uint8_t value = handle_in(port);
      cpu.set_reg8(value, qkz80::reg_A);
      cpu.regs.PC.set_pair16(pc + 2);
      instruction_count++;
      continue;
    }

    // Handle OUT instruction (0xD3)
    if (opcode == 0xD3) {
      uint8_t port = memory.fetch_mem(pc + 1) & 0xFF;
      uint8_t value = cpu.get_reg8(qkz80::reg_A);
      handle_out(port, value);
      cpu.regs.PC.set_pair16(pc + 2);
      instruction_count++;
      continue;
    }

    // Execute normal instruction
    cpu.execute();
    instruction_count++;
  }
}

static void main_loop() {
  run_batch();
}

//=============================================================================
// Exported Functions for JavaScript
//=============================================================================

extern "C" {

// Send keyboard input
EMSCRIPTEN_KEEPALIVE
void romwbw_key_input(int ch) {
  if (ch == '\n') ch = '\r';  // LF -> CR for CP/M
  emu_console_queue_char(ch);
  waiting_for_input = false;
}

// Set boot string for auto-boot feature
// The string is fed to console input before user keyboard input
// A CR is automatically appended to submit the boot command
EMSCRIPTEN_KEEPALIVE
void romwbw_set_boot_string(const char* str) {
  if (str) {
    boot_string = str;
    // Queue boot string to console input
    for (size_t i = 0; i < boot_string.size(); i++) {
      emu_console_queue_char(boot_string[i] & 0xFF);
    }
    emu_console_queue_char('\r');  // Add CR to submit
  }
}

// Helper: Set up HBIOS ident signatures in RAM common area
// This is required for REBOOT and other utilities to recognize the system
static void setup_hbios_ident() {
  uint8_t* ram = memory.get_ram();
  if (!ram) return;

  // Common area 0x8000-0xFFFF maps to bank 0x8F (index 15 = 0x0F)
  // Physical offset in RAM = bank_index * 32KB + (addr - 0x8000)
  const uint32_t COMMON_BASE = 0x0F * 32768;  // Bank 0x8F = index 15

  // Create ident block at 0xFF00 in common area
  uint32_t ident_phys = COMMON_BASE + (0xFF00 - 0x8000);
  ram[ident_phys + 0] = 'W';       // Signature byte 1
  ram[ident_phys + 1] = ~'W';      // Signature byte 2 (0xA8)
  ram[ident_phys + 2] = 0x35;      // Combined version: (major << 4) | minor = (3 << 4) | 5

  // Also create ident block at 0xFE00 (some utilities may look there)
  uint32_t ident_phys2 = COMMON_BASE + (0xFE00 - 0x8000);
  ram[ident_phys2 + 0] = 'W';
  ram[ident_phys2 + 1] = ~'W';
  ram[ident_phys2 + 2] = 0x35;

  // Store pointer to ident block at 0xFFFC (little-endian)
  uint32_t ptr_phys = COMMON_BASE + (0xFFFC - 0x8000);
  ram[ptr_phys + 0] = 0x00;        // Low byte of 0xFF00
  ram[ptr_phys + 1] = 0xFF;        // High byte of 0xFF00
}

// Load ROM image
EMSCRIPTEN_KEEPALIVE
int romwbw_load_rom(const uint8_t* data, int size) {
  uint8_t* rom = memory.get_rom();
  if (!rom) return -1;

  int copy_size = (size < 512 * 1024) ? size : 512 * 1024;
  memcpy(rom, data, copy_size);

  // Patch APITYPE at 0x0112 to 0x00 (HBIOS) instead of 0xFF (UNA)
  // This is required for REBOOT and other utilities to work
  rom[0x0112] = 0x00;  // CB_APITYPE = HBIOS

  // Copy HCB to RAM bank 0x80
  uint8_t* ram = memory.get_ram();
  if (ram) {
    memcpy(ram, rom, 512);
  }

  // Set up HBIOS identification signatures
  setup_hbios_ident();

  char msg[64];
  snprintf(msg, sizeof(msg), "ROM loaded: %d bytes", copy_size);
  emu_status(msg);
  return 0;
}

// Load disk image to unit
EMSCRIPTEN_KEEPALIVE
int romwbw_load_disk(int unit, const uint8_t* data, int size) {
  if (unit < 0 || unit >= 16) return -1;

  if (!hbios.loadDisk(unit, data, size)) {
    return -1;
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "Disk %d loaded: %d bytes", unit, size);
  emu_status(msg);
  return 0;
}

// Get disk data for saving
EMSCRIPTEN_KEEPALIVE
const uint8_t* romwbw_get_disk_data(int unit) {
  if (unit < 0 || unit >= 16 || !hbios.isDiskLoaded(unit)) return nullptr;
  return hbios.getDisk(unit).data.data();
}

EMSCRIPTEN_KEEPALIVE
int romwbw_get_disk_size(int unit) {
  if (unit < 0 || unit >= 16 || !hbios.isDiskLoaded(unit)) return 0;
  return hbios.getDisk(unit).data.size();
}

// Reset callback for SYSRESET
static void handle_sysreset(uint8_t reset_type) {
  if (debug) {
    emu_log("[SYSRESET] %s boot - restarting\n",
            reset_type == 0x01 ? "Warm" : "Cold");
  }
  // Switch to ROM bank 0
  memory.select_bank(0x00);
  // Clear any pending input
  // (emu_io_wasm clears automatically on next queue)
  // Set PC to 0 to restart from ROM
  cpu.regs.PC.set_pair16(0x0000);
}

// Start emulation
EMSCRIPTEN_KEEPALIVE
void romwbw_start() {
  // Set Z80 mode
  cpu.set_cpu_mode(qkz80::MODE_Z80);

  // Enable banking
  memory.enable_banking();

  // Set up HBIOS dispatch with CPU and memory
  hbios.setCPU(&cpu);
  hbios.setMemory(&memory);
  hbios.setDebug(debug);

  // Register reset callback for SYSRESET (REBOOT command)
  hbios.setResetCallback(handle_sysreset);

  // Reset CPU
  cpu.regs.AF.set_pair16(0);
  cpu.regs.BC.set_pair16(0);
  cpu.regs.DE.set_pair16(0);
  cpu.regs.HL.set_pair16(0);
  cpu.regs.PC.set_pair16(0x0000);  // Start at ROM address 0
  cpu.regs.SP.set_pair16(0x0000);

  // Select ROM bank 0
  memory.select_bank(0);

  running = true;
  waiting_for_input = false;
  instruction_count = 0;

  emu_status("RomWBW starting...");
}

// Stop emulation
EMSCRIPTEN_KEEPALIVE
void romwbw_stop() {
  running = false;
}

// Check if running
EMSCRIPTEN_KEEPALIVE
int romwbw_is_running() {
  return running ? 1 : 0;
}

// Get instruction count
EMSCRIPTEN_KEEPALIVE
double romwbw_get_instruction_count() {
  return (double)instruction_count;
}

// Get current PC
EMSCRIPTEN_KEEPALIVE
int romwbw_get_pc() {
  return cpu.regs.PC.get_pair16();
}

// Set debug mode
EMSCRIPTEN_KEEPALIVE
void romwbw_set_debug(int enable) {
  debug = (enable != 0);
  memory.set_debug(debug);
  hbios.setDebug(debug);
}

// Auto-start with preloaded files
EMSCRIPTEN_KEEPALIVE
int romwbw_autostart() {
  // Load ROM from virtual filesystem
  FILE* f = fopen("/romwbw.rom", "rb");
  if (!f) {
    emu_status("Error: romwbw.rom not found");
    return -1;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint8_t* rom = memory.get_rom();
  if (!rom) {
    fclose(f);
    return -1;
  }

  fread(rom, 1, size, f);
  fclose(f);

  // Patch APITYPE at 0x0112 to 0x00 (HBIOS) instead of 0xFF (UNA)
  rom[0x0112] = 0x00;  // CB_APITYPE = HBIOS

  // Copy HCB to RAM
  uint8_t* ram = memory.get_ram();
  if (ram) {
    memcpy(ram, rom, 512);
  }

  // Set up HBIOS identification signatures
  setup_hbios_ident();

  // Load disk if present
  f = fopen("/hd0.img", "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> disk_data(size);
    fread(disk_data.data(), 1, size, f);
    fclose(f);
    hbios.loadDisk(0, disk_data.data(), disk_data.size());
  }

  romwbw_start();
  return 0;
}

}  // extern "C"

int main() {
  // Initialize I/O layer
  emu_io_init();

  // Initialize memory
  memory.enable_banking();

  // Initialize HBIOS dispatch
  hbios.setCPU(&cpu);
  hbios.setMemory(&memory);
  hbios.reset();

  emu_status("RomWBW Emulator ready");
  emscripten_set_main_loop(main_loop, 0, 0);
  return 0;
}
