/*
 * RomWBW Emulator - WebAssembly Version
 *
 * Compiles with Emscripten to run RomWBW in browser.
 * Uses shared hbios_cpu for port I/O and HBIOSDispatch for HBIOS emulation.
 * Console I/O via JavaScript callbacks through emu_io.h abstraction.
 */

#include "../src/hbios_cpu.h"  // Shared CPU with port I/O
#include "../src/emu_io.h"
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

// Version string - set by Makefile from VERSION file
#ifndef EMU_VERSION
#define EMU_VERSION "dev"
#endif

// Forward declaration
struct EmulatorState;
static EmulatorState* emu = nullptr;

//=============================================================================
// Emulator State - implements HBIOSCPUDelegate for callbacks
//=============================================================================

struct EmulatorState : public HBIOSCPUDelegate {
  banked_mem memory;
  hbios_cpu cpu;
  HBIOSDispatch hbios;

  bool running = false;
  bool debug = false;
  bool halted = false;

  // Counters
  long long instruction_count = 0;
  int batch_count = 0;

  EmulatorState() : cpu(&memory, this) {
    memory.enable_banking();
    hbios.setCPU(&cpu);
    hbios.setMemory(&memory);
    hbios.setBlockingAllowed(false);  // Web/WASM cannot block
  }

  // HBIOSCPUDelegate implementation
  banked_mem* getMemory() override { return &memory; }
  HBIOSDispatch* getHBIOS() override { return &hbios; }
  void initializeRamBankIfNeeded(uint8_t bank) override { (void)bank; }
  void onHalt() override {
    emu_log("[HALT] at PC=0x%04X\n", cpu.regs.PC.get_pair16());
    halted = true;
    running = false;
  }
  void onUnimplementedOpcode(uint8_t opcode, uint16_t pc) override {
    emu_log("[UNIMPLEMENTED] opcode 0x%02X at PC=0x%04X\n", opcode, pc);
    halted = true;
    running = false;
  }
  void logDebug(const char* fmt, ...) override {
    if (!debug) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    emu_log("%s", buf);
  }
};

// Ensure emulator exists
static void ensure_emu() {
  if (!emu) emu = new EmulatorState();
}

//=============================================================================
// Main Execution Loop
//=============================================================================

static void flush_output() {
  // Poll output buffer and send chars to display
  if (emu->hbios.hasOutputChars()) {
    std::vector<uint8_t> chars = emu->hbios.getOutputChars();
    for (uint8_t ch : chars) {
      emu_console_write_char(ch);
    }
  }
}

static void run_batch() {
  if (!emu->running || emu->hbios.isWaitingForInput()) return;

  emu->batch_count++;
  if (emu->debug && (emu->batch_count <= 5 || emu->batch_count % 100 == 0)) {
    emu_log("[BATCH] #%d starting, PC=0x%04X, instr=%lld\n",
            emu->batch_count, emu->cpu.regs.PC.get_pair16(), emu->instruction_count);
  }

  for (int i = 0; i < 50000 && emu->running && !emu->hbios.isWaitingForInput(); i++) {
    // Execute instruction - port I/O handled by hbios_cpu::port_in/port_out
    emu->cpu.execute();
    emu->instruction_count++;
  }

  // Flush any pending output characters to display
  flush_output();
}

static void main_loop() {
  if (emu) run_batch();
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
  // Clear waiting flag so run_batch continues
  if (emu) emu->hbios.clearWaitingForInput();
}

// Set boot string for auto-boot feature
// The string is fed to console input before user keyboard input
// A CR is automatically appended to submit the boot command
EMSCRIPTEN_KEEPALIVE
void romwbw_set_boot_string(const char* str) {
  if (!str) return;
  // Queue boot string to emu_console (poll_input will move to hbios)
  for (size_t i = 0; str[i]; i++) {
    int ch = str[i] & 0xFF;
    if (ch == '\n') ch = '\r';
    emu_console_queue_char(ch);
  }
  emu_console_queue_char('\r');  // Add CR to submit
}

// Helper: Set up HBIOS ident signatures in RAM common area
// This is required for REBOOT and other utilities to recognize the system
static void setup_hbios_ident() {
  if (!emu) return;
  uint8_t* ram = emu->memory.get_ram();
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

// Load ROM image - creates fresh emulator state
EMSCRIPTEN_KEEPALIVE
int romwbw_load_rom(const uint8_t* data, int size) {
  // Create fresh emulator state when loading new ROM
  delete emu;
  emu = new EmulatorState();

  uint8_t* rom = emu->memory.get_rom();
  if (!rom) return -1;

  int copy_size = (size < 512 * 1024) ? size : 512 * 1024;
  memcpy(rom, data, copy_size);

  // Patch APITYPE at 0x0112 to 0x00 (HBIOS) instead of 0xFF (UNA)
  // This is required for REBOOT and other utilities to work
  rom[0x0112] = 0x00;  // CB_APITYPE = HBIOS

  // Copy HCB to RAM bank 0x80
  uint8_t* ram = emu->memory.get_ram();
  if (ram) {
    memcpy(ram, rom, 512);
  }

  // Set up HBIOS identification signatures
  setup_hbios_ident();

  // Initialize memory disks from HCB configuration
  emu->hbios.initMemoryDisks();

  char msg[64];
  snprintf(msg, sizeof(msg), "ROM loaded: %d bytes", copy_size);
  emu_status(msg);
  return 0;
}

// Load disk image to unit
EMSCRIPTEN_KEEPALIVE
int romwbw_load_disk(int unit, const uint8_t* data, int size) {
  ensure_emu();
  emu_log("[WASM] romwbw_load_disk called: unit=%d size=%d\n", unit, size);
  if (unit < 0 || unit >= 16) return -1;

  if (!emu->hbios.loadDisk(unit, data, size)) {
    return -1;
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "Disk %d loaded: %d bytes", unit, size);
  emu_status(msg);
  emu_log("[WASM] Disk loaded into slot %d successfully\n", unit);
  return 0;
}

// Get disk data for saving
EMSCRIPTEN_KEEPALIVE
const uint8_t* romwbw_get_disk_data(int unit) {
  if (!emu || unit < 0 || unit >= 16 || !emu->hbios.isDiskLoaded(unit)) return nullptr;
  return emu->hbios.getDisk(unit).data.data();
}

EMSCRIPTEN_KEEPALIVE
int romwbw_get_disk_size(int unit) {
  if (!emu || unit < 0 || unit >= 16 || !emu->hbios.isDiskLoaded(unit)) return 0;
  return emu->hbios.getDisk(unit).data.size();
}

// Create a new formatted disk image and load it into a unit
// format: 0 = 8MB single slice, 1 = 51MB combo (6 slices)
// unit: disk unit to load into (0-15)
// Returns: 0 on success, -1 on error
EMSCRIPTEN_KEEPALIVE
int romwbw_create_disk(int unit, int format) {
  ensure_emu();
  if (unit < 0 || unit >= 16) return -1;
  if (format < 0 || format > 1) return -1;

  emu_disk_format disk_format = (format == 0) ? EMU_DISK_HD1K_SINGLE : EMU_DISK_HD1K_COMBO;
  std::vector<uint8_t> data = emu_disk_create_memory(disk_format);
  if (data.empty()) {
    emu_error("Failed to create disk image");
    return -1;
  }

  if (!emu->hbios.loadDisk(unit, data.data(), data.size())) {
    emu_error("Failed to load disk into unit %d", unit);
    return -1;
  }

  const char* format_name = (format == 0) ? "8MB single" : "51MB combo";
  emu_status("Created %s disk in unit %d", format_name, unit);
  return 0;
}

// Create a new formatted disk image for download (doesn't load into emulator)
// format: 0 = 8MB single slice, 1 = 51MB combo (6 slices)
// Returns: pointer to allocated data (caller must free with _free), or 0 on error
// Size is returned via romwbw_get_disk_format_size()
static std::vector<uint8_t> created_disk_data;

EMSCRIPTEN_KEEPALIVE
uint8_t* romwbw_create_disk_for_download(int format) {
  if (format < 0 || format > 1) return nullptr;

  emu_disk_format disk_format = (format == 0) ? EMU_DISK_HD1K_SINGLE : EMU_DISK_HD1K_COMBO;
  created_disk_data = emu_disk_create_memory(disk_format);
  if (created_disk_data.empty()) {
    return nullptr;
  }

  return created_disk_data.data();
}

EMSCRIPTEN_KEEPALIVE
int romwbw_get_created_disk_size() {
  return created_disk_data.size();
}

// Get disk format sizes (for JavaScript to know expected sizes)
EMSCRIPTEN_KEEPALIVE
int romwbw_get_disk_format_size(int format) {
  switch (format) {
    case 0: return EMU_HD1K_SINGLE_SIZE;  // 8MB
    case 1: return EMU_HD1K_COMBO_SIZE;   // 51MB
    default: return 0;
  }
}

// Reset callback for SYSRESET
static void handle_sysreset(uint8_t reset_type) {
  if (!emu) return;
  if (emu->debug) {
    emu_log("[SYSRESET] %s boot - restarting\n",
            reset_type == 0x01 ? "Warm" : "Cold");
  }
  // Switch to ROM bank 0
  emu->memory.select_bank(0x00);
  // Set PC to 0 to restart from ROM
  emu->cpu.regs.PC.set_pair16(0x0000);
}

// Start emulation
EMSCRIPTEN_KEEPALIVE
void romwbw_start() {
  ensure_emu();

  emu_log("[WASM] RomWBW Emulator v" EMU_VERSION " built " __DATE__ " " __TIME__ " starting\n");

  // Set Z80 mode
  emu->cpu.set_cpu_mode(qkz80::MODE_Z80);

  // Register reset callback for SYSRESET (REBOOT command)
  emu->hbios.setResetCallback(handle_sysreset);

  // Populate disk unit table in HCB so romldr can discover disks
  emu->hbios.populateDiskUnitTable();

  // Reset CPU and start at ROM address 0
  emu->cpu.regs.AF.set_pair16(0);
  emu->cpu.regs.BC.set_pair16(0);
  emu->cpu.regs.DE.set_pair16(0);
  emu->cpu.regs.HL.set_pair16(0);
  emu->cpu.regs.PC.set_pair16(0x0000);
  emu->cpu.regs.SP.set_pair16(0x0000);
  emu->memory.select_bank(0);

  // Reset counters
  emu->instruction_count = 0;
  emu->batch_count = 0;

  emu->running = true;
  emu->hbios.clearWaitingForInput();

  emu_status("RomWBW starting...");
}

// Stop emulation
EMSCRIPTEN_KEEPALIVE
void romwbw_stop() {
  if (emu) emu->running = false;
}

// Check if running
EMSCRIPTEN_KEEPALIVE
int romwbw_is_running() {
  return (emu && emu->running) ? 1 : 0;
}

// Check if waiting for input
EMSCRIPTEN_KEEPALIVE
int romwbw_is_waiting() {
  return (emu && emu->hbios.isWaitingForInput()) ? 1 : 0;
}

// Get instruction count
EMSCRIPTEN_KEEPALIVE
double romwbw_get_instruction_count() {
  return emu ? (double)emu->instruction_count : 0;
}

// Get current PC
EMSCRIPTEN_KEEPALIVE
int romwbw_get_pc() {
  return emu ? emu->cpu.regs.PC.get_pair16() : 0;
}

// Set debug mode
EMSCRIPTEN_KEEPALIVE
void romwbw_set_debug(int enable) {
  if (emu) {
    emu->debug = (enable != 0);
    emu->memory.set_debug(emu->debug);
    emu->hbios.setDebug(emu->debug);
  }
}

// Run a single batch of instructions (for testing)
EMSCRIPTEN_KEEPALIVE
int romwbw_run_batch() {
  if (!emu || !emu->running) return 0;
  run_batch();
  return emu->running ? 1 : 0;
}

// Auto-start with preloaded files
EMSCRIPTEN_KEEPALIVE
int romwbw_autostart() {
  // Create fresh emulator state
  delete emu;
  emu = new EmulatorState();

  // Load ROM from virtual filesystem
  FILE* f = fopen("/romwbw.rom", "rb");
  if (!f) {
    emu_status("Error: romwbw.rom not found");
    return -1;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  uint8_t* rom = emu->memory.get_rom();
  if (!rom) {
    fclose(f);
    return -1;
  }

  fread(rom, 1, size, f);
  fclose(f);

  // Patch APITYPE at 0x0112 to 0x00 (HBIOS) instead of 0xFF (UNA)
  rom[0x0112] = 0x00;  // CB_APITYPE = HBIOS

  // Copy HCB to RAM
  uint8_t* ram = emu->memory.get_ram();
  if (ram) {
    memcpy(ram, rom, 512);
  }

  // Set up HBIOS identification signatures
  setup_hbios_ident();

  // Initialize memory disks from HCB configuration
  emu->hbios.initMemoryDisks();

  // Load disk if present
  f = fopen("/hd0.img", "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> disk_data(size);
    fread(disk_data.data(), 1, size, f);
    fclose(f);
    emu->hbios.loadDisk(0, disk_data.data(), disk_data.size());
  }

  romwbw_start();
  return 0;
}

}  // extern "C"

int main() {
  // Initialize I/O layer
  emu_io_init();

  emu_status("RomWBW Emulator ready");
  emscripten_set_main_loop(main_loop, 0, 0);
  return 0;
}
