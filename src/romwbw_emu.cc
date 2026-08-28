/*
 * RomWBW Emulator
 *
 * Emulates RomWBW with banked memory (512KB ROM + 512KB RAM).
 * HBIOS calls are handled by the emulator, allowing RomWBW to boot
 * CP/M, ZSDOS, and other operating systems.
 *
 * Console escape: Ctrl+E (configurable)
 */

// Version info - normally injected from the top-level VERSION file by the
// makefile (-DEMU_VERSION=...); the fallback covers builds outside make
#ifndef EMU_VERSION
#define EMU_VERSION "dev"
#endif

#include "qkz80.h"
#include "romwbw_mem.h"
#include "hbios_dispatch.h"  // Shared HBIOS definitions
#include "hbios_cpu.h"       // Shared CPU with HBIOS port I/O
#include "emu_io.h"
#include "emu_init.h"        // Shared initialization functions
#include "emu_config.h"      // Optional JSON settings file
#include "romwbw_pin.h"      // Pinned RomWBW release
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <random>

// Build date from version.cc (recompiled on every relink)
extern const char* emu_build_date;

// Global for signal handler to request stop
static volatile bool stop_requested = false;

//=============================================================================
// NVRAM Persistence
//=============================================================================

// Per-user config directory, following the XDG base directory spec:
// $XDG_CONFIG_HOME/romwbw_emu if XDG_CONFIG_HOME is set (must be absolute),
// otherwise ~/.config/romwbw_emu. Falls back to "." if HOME is unset.
static std::string get_config_dir() {
  const char* xdg = getenv("XDG_CONFIG_HOME");
  std::string base;
  if (xdg && xdg[0] == '/') {
    base = xdg;
  } else {
    const char* home = getenv("HOME");
    if (!home) home = ".";
    base = std::string(home) + "/.config";
  }
  mkdir(base.c_str(), 0755);  // ensure parent exists (EEXIST is fine)
  std::string dir = base + "/romwbw_emu";
  mkdir(dir.c_str(), 0755);
  return dir;
}

// Get default NVRAM path
static std::string get_nvram_path() {
  return get_config_dir() + "/nvram";
}

// Legacy NVRAM location (pre-XDG_CONFIG_HOME support): always ~/.config.
// Used as a read fallback so an existing setting migrates on first save.
static std::string get_legacy_nvram_path() {
  const char* home = getenv("HOME");
  if (!home) home = ".";
  return std::string(home) + "/.config/romwbw_emu/nvram";
}

// Load NVRAM setting from file (just a printable string like "C" or "0.2")
static std::string load_nvram_setting(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return "";

  char buf[64];
  if (!fgets(buf, sizeof(buf), f)) {
    fclose(f);
    return "";
  }
  fclose(f);

  // Strip trailing whitespace
  size_t len = strlen(buf);
  while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) {
    buf[--len] = '\0';
  }
  return std::string(buf);
}

// Save NVRAM setting to file
static bool save_nvram_setting(const std::string& path, const std::string& setting) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return false;
  fprintf(f, "%s\n", setting.c_str());
  fclose(f);
  return true;
}

// Interrupt configuration for scheduled interrupts
struct InterruptConfig {
  bool enabled;
  unsigned int cycle_min;       // Minimum cycles between interrupts
  unsigned int cycle_max;       // Maximum cycles (random in range)
  bool use_rst;                 // true = RST n, false = CALL addr
  unsigned int rst_num;         // RST number (0-7) for RST mode
  unsigned int call_addr;       // Target address for CALL mode
  unsigned long long next_trigger;  // Next cycle count to trigger

  InterruptConfig() : enabled(false), cycle_min(0), cycle_max(0),
                      use_rst(true), rst_num(7), call_addr(0), next_trigger(0) {}
};

// Global interrupt configurations
static InterruptConfig maskable_int_config;
static InterruptConfig nmi_config;
static std::mt19937 interrupt_rng(std::random_device{}());


// Get next trigger cycle count (random in range)
static unsigned long long get_next_trigger(const InterruptConfig& cfg, unsigned long long current_cycles) {
  if (cfg.cycle_min == cfg.cycle_max) {
    return current_cycles + cfg.cycle_min;
  }
  std::uniform_int_distribution<unsigned int> dist(cfg.cycle_min, cfg.cycle_max);
  return current_cycles + dist(interrupt_rng);
}

// Track if we're waiting for a maskable interrupt to be delivered
// (used when IFF1=0 delays delivery)
static bool waiting_for_int_delivery = false;

// HBIOS function codes and result codes are now in hbios_dispatch.h

// Console mode escape character (default ^E like SIMH). 0 means NO key is
// reserved: nothing is taken from the guest and console mode is unreachable.
// NUL is the sentinel rather than a separate bool because the emu_io contract
// (emu_io.h, DOWNSTREAM.md) passes the escape as a bare char to every port's
// emu_console_check_escape(), and adding a required symbol there would be a
// link break in four ports. It is a sentinel, not a bindable key: every check
// site short-circuits before the value is ever compared against input, which
// matters because a terminal really can send 0x00 - xterm and the rest emit
// it for Ctrl+Space.
static char console_escape_char = 0x05;  // Ctrl+E
static bool console_mode_requested = false;

// Parse an escape-character spec: "none"/"off" to reserve no key at all,
// "^X" for a control char, or a literal character. Used by --escape and the
// config file's "escape" key.
static bool parse_escape_char(const char* esc, char* out) {
  // Every Ctrl-letter is live in the guest - the default ^E is cursor-up in
  // the WordStar diamond - so the key the emulator reserves has to be
  // surrenderable, the way z80cpmw's keyboard.ctrlRToCpm is. This test must
  // come first: the literal branch below would otherwise read "none" as the
  // letter 'n' and quietly reserve that instead.
  if (emu_strcasecmp(esc, "none") == 0 || emu_strcasecmp(esc, "off") == 0) {
    *out = 0;
    return true;
  }
  // Both branches insist on an exact length. Without that, the literal
  // branch swallowed the first byte of anything it did not recognise, so
  // --escape=non reserved 'n' and --escape=of reserved 'o' - near-misses of
  // the new keywords that used to be merely odd and are now silent, because
  // the reserved byte is no longer delivered to the guest at all. It also
  // rejects a multi-byte literal like --escape=e-acute, which truncated to
  // its UTF-8 lead byte and left the guest receiving the orphaned trail.
  if (esc[0] == '^' && esc[1] != '\0' && esc[2] == '\0') {
    char c = (char)toupper((unsigned char)esc[1]);
    if (c >= '@' && c <= '_') {
      *out = c - '@';
      return true;
    }
    return false;
  }
  // A literal must be printable ASCII: escape_key_name() spells the key back
  // as a bare byte, and a lone >= 0x80 byte is not valid UTF-8, so
  // --save-config would throw on it. DEL is not a bindable key either.
  if (esc[0] != '\0' && esc[1] == '\0' && (unsigned char)esc[0] < 0x7F) {
    *out = esc[0];
    return true;
  }
  return false;
}

// Check for escape character (non-blocking) - called periodically from main loop
static bool check_console_escape_async() {
  if (console_escape_char == 0) return false;  // --escape=none: no key reserved
  if (emu_console_check_escape(console_escape_char)) {
    console_mode_requested = true;
    return true;
  }
  return false;
}

// Symbol table for symbolic debugging
static std::map<std::string, uint16_t> symbols;        // name -> address
static std::map<uint16_t, std::string> addr_to_symbol; // address -> name

// Breakpoints
static std::set<uint16_t> breakpoints;

// Load symbol table from .sym file
// Format: Each line is "ADDRESS SYMBOL" where ADDRESS is 4 hex digits
static bool load_symbols(const char* filename) {
  FILE* f = fopen(filename, "r");
  if (!f) return false;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    // Skip empty lines and comments
    if (line[0] == '\0' || line[0] == '\n' || line[0] == ';' || line[0] == '#') continue;

    // Parse "ADDR SYMBOL" or "SYMBOL = ADDR" formats
    char sym[64];
    unsigned int addr;

    // Try "ADDR SYMBOL" format first
    if (sscanf(line, "%x %63s", &addr, sym) == 2) {
      symbols[sym] = (uint16_t)addr;
      addr_to_symbol[(uint16_t)addr] = sym;
    }
    // Try "SYMBOL = ADDR" format
    else if (sscanf(line, "%63s = %x", sym, &addr) == 2 ||
             sscanf(line, "%63s =%x", sym, &addr) == 2 ||
             sscanf(line, "%63s= %x", sym, &addr) == 2 ||
             sscanf(line, "%63s=%x", sym, &addr) == 2) {
      symbols[sym] = (uint16_t)addr;
      addr_to_symbol[(uint16_t)addr] = sym;
    }
    // Try "SYMBOL EQU ADDR" format (common in assembler listings)
    else if (sscanf(line, "%63s EQU %x", sym, &addr) == 2 ||
             sscanf(line, "%63s equ %x", sym, &addr) == 2) {
      symbols[sym] = (uint16_t)addr;
      addr_to_symbol[(uint16_t)addr] = sym;
    }
  }

  fclose(f);
  fprintf(stderr, "Loaded %zu symbols from %s\n", symbols.size(), filename);
  return true;
}

// Parse an address that might be numeric or symbolic
// Numeric: plain hex (ffa0) or with $ prefix ($ffa0) or 0x prefix (0xffa0)
// Symbolic: with . prefix (.BDOS, .ffa0)
// Returns -1 if invalid
static int parse_address(const char* str) {
  if (!str || !*str) return -1;

  // Symbolic lookup with . prefix
  if (str[0] == '.') {
    const char* sym = str + 1;
    auto it = symbols.find(sym);
    if (it != symbols.end()) {
      return it->second;
    }
    // Symbol not found
    fprintf(stderr, "Unknown symbol: %s\n", sym);
    return -1;
  }

  // Try numeric parsing
  char* endptr;
  unsigned long val;

  if (str[0] == '$') {
    // $hex format
    val = strtoul(str + 1, &endptr, 16);
  } else if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
    // 0x hex format
    val = strtoul(str + 2, &endptr, 16);
  } else {
    // Plain hex
    val = strtoul(str, &endptr, 16);
  }

  if (*endptr != '\0' && !isspace(*endptr)) {
    return -1;  // Invalid character in number
  }

  if (val > 0xFFFF) return -1;
  return (int)val;
}

// Format address with symbol if available
static std::string format_address(uint16_t addr) {
  char buf[64];
  auto it = addr_to_symbol.find(addr);
  if (it != addr_to_symbol.end()) {
    snprintf(buf, sizeof(buf), "%04X (%s)", addr, it->second.c_str());
  } else {
    snprintf(buf, sizeof(buf), "%04X", addr);
  }
  return std::string(buf);
}

// Console mode help text
static void print_console_help() {
  fprintf(stderr, "\nConsole mode commands:\n");
  fprintf(stderr, "  g, go, c, cont   Continue execution\n");
  fprintf(stderr, "  q, quit, exit    Exit emulator (writes trace if enabled)\n");
  fprintf(stderr, "  r, reg           Show registers\n");
  fprintf(stderr, "  e ADDR [COUNT]   Examine memory (e .LABEL or e ffa0)\n");
  fprintf(stderr, "  d ADDR VAL...    Deposit bytes to memory\n");
  fprintf(stderr, "  dm ADDR [COUNT]  Dump memory (16 bytes/line, with ASCII)\n");
  fprintf(stderr, "  bp ADDR          Set breakpoint (bp .LABEL or bp ffa0)\n");
  fprintf(stderr, "  bc ADDR          Clear breakpoint\n");
  fprintf(stderr, "  bl               List breakpoints\n");
  fprintf(stderr, "  ba               Clear all breakpoints\n");
  fprintf(stderr, "  s, step [N]      Step N instructions (default 1)\n");
  fprintf(stderr, "  sym [PATTERN]    List symbols matching pattern (or all)\n");
  fprintf(stderr, "  pc ADDR          Set PC to address\n");
  fprintf(stderr, "  ?, help          Show this help\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Address formats:\n");
  fprintf(stderr, "  ffa0             Plain hex\n");
  fprintf(stderr, "  $ffa0 or 0xffa0  Explicit hex\n");
  fprintf(stderr, "  .LABEL           Symbol lookup (. prefix)\n");
  fprintf(stderr, "\n");
}

// Read a line in console mode (with cooked terminal)
static bool read_console_line(char* buf, size_t buflen) {
  emu_io_cleanup();
  fprintf(stderr, "sim> ");
  fflush(stderr);

  if (!fgets(buf, (int)buflen, stdin)) {
    emu_io_init();
    return false;
  }

  // Strip newline
  size_t len = strlen(buf);
  if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';

  emu_io_init();
  return true;
}

// Console mode return values
enum ConsoleResult {
  CONSOLE_CONTINUE,   // Resume execution
  CONSOLE_QUIT,       // Exit emulator
  CONSOLE_STEP,       // Step N instructions then re-enter console
  CONSOLE_AGAIN       // Stay in console mode
};

// Step count for stepping
static int step_count = 0;

// Spell the reserved key the way a user types it. One helper because the key
// is named in four places (this banner, the console-mode banner, the usage
// text and --save-config) and they used to disagree: the console-mode banner
// said "^E" no matter what --escape was set to.
static std::string escape_key_name() {
  if (console_escape_char == 0) return "none";
  unsigned char c = (unsigned char)console_escape_char;
  if (c < 0x20) return std::string("^") + (char)('@' + c);
  return std::string(1, (char)c);
}

// Handle console mode - returns action to take
static ConsoleResult handle_console_mode(qkz80* cpu, banked_mem* memory) {
  char line[256];
  char cmd[64];
  char arg1[64], arg2[64], arg3[64];

  fprintf(stderr, "\n[Console mode - %s to enter, 'help' for commands]\n",
          escape_key_name().c_str());
  fprintf(stderr, "PC=%s\n", format_address(cpu->regs.PC.get_pair16()).c_str());

  while (true) {
    if (!read_console_line(line, sizeof(line))) {
      return CONSOLE_QUIT;
    }

    // Parse command and args
    cmd[0] = arg1[0] = arg2[0] = arg3[0] = '\0';
    sscanf(line, "%63s %63s %63s %63s", cmd, arg1, arg2, arg3);

    // Empty line - repeat last command or just prompt again
    if (cmd[0] == '\0') continue;

    // Convert command to lowercase for comparison
    for (char* p = cmd; *p; p++) *p = (char)tolower(*p);

    // Continue/Go
    if (strcmp(cmd, "g") == 0 || strcmp(cmd, "go") == 0 ||
        strcmp(cmd, "c") == 0 || strcmp(cmd, "cont") == 0) {
      fprintf(stderr, "[Continuing...]\n");
      return CONSOLE_CONTINUE;
    }

    // Quit/Exit
    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 ||
        strcmp(cmd, "exit") == 0) {
      fprintf(stderr, "[Exiting...]\n");
      return CONSOLE_QUIT;
    }

    // Registers
    if (strcmp(cmd, "r") == 0 || strcmp(cmd, "reg") == 0 ||
        strcmp(cmd, "regs") == 0) {
      uint16_t af = cpu->regs.AF.get_pair16();
      uint16_t bc = cpu->regs.BC.get_pair16();
      uint16_t de = cpu->regs.DE.get_pair16();
      uint16_t hl = cpu->regs.HL.get_pair16();
      uint16_t sp = cpu->regs.SP.get_pair16();
      uint16_t pc = cpu->regs.PC.get_pair16();
      uint8_t flags = af & 0xFF;

      fprintf(stderr, "  A=%02X  BC=%04X  DE=%04X  HL=%04X  SP=%04X  PC=%s\n",
              af >> 8, bc, de, hl, sp, format_address(pc).c_str());
      fprintf(stderr, "  Flags: %c%c%c%c%c%c%c%c (S Z - H - P/V N C)\n",
              (flags & 0x80) ? 'S' : '-',
              (flags & 0x40) ? 'Z' : '-',
              (flags & 0x20) ? '1' : '0',
              (flags & 0x10) ? 'H' : '-',
              (flags & 0x08) ? '1' : '0',
              (flags & 0x04) ? 'P' : '-',
              (flags & 0x02) ? 'N' : '-',
              (flags & 0x01) ? 'C' : '-');
      continue;
    }

    // Examine memory
    if (strcmp(cmd, "e") == 0) {
      if (arg1[0] == '\0') {
        fprintf(stderr, "Usage: e ADDR [COUNT]\n");
        continue;
      }
      int addr = parse_address(arg1);
      if (addr < 0) {
        fprintf(stderr, "Invalid address: %s\n", arg1);
        continue;
      }
      int count = 1;
      if (arg2[0] != '\0') {
        count = parse_address(arg2);
        if (count < 1) count = 1;
        if (count > 256) count = 256;
      }
      uint8_t* mem = cpu->get_mem();
      for (int i = 0; i < count; i++) {
        uint16_t a = (addr + i) & 0xFFFF;
        fprintf(stderr, "  %s: %02X\n", format_address(a).c_str(),
                mem[a]);
      }
      continue;
    }

    // Dump memory (hex + ASCII)
    if (strcmp(cmd, "dm") == 0) {
      if (arg1[0] == '\0') {
        fprintf(stderr, "Usage: dm ADDR [COUNT]\n");
        continue;
      }
      int addr = parse_address(arg1);
      if (addr < 0) {
        fprintf(stderr, "Invalid address: %s\n", arg1);
        continue;
      }
      int count = 128;  // Default 8 lines
      if (arg2[0] != '\0') {
        count = parse_address(arg2);
        if (count < 1) count = 1;
        if (count > 4096) count = 4096;
      }
      uint8_t* mem = cpu->get_mem();
      for (int i = 0; i < count; i += 16) {
        uint16_t a = (addr + i) & 0xFFFF;
        fprintf(stderr, "  %04X: ", a);
        // Hex
        for (int j = 0; j < 16 && (i + j) < count; j++) {
          fprintf(stderr, "%02X ", (uint8_t)mem[(a + j) & 0xFFFF]);
        }
        // Pad if short line
        for (int j = count - i; j < 16; j++) {
          fprintf(stderr, "   ");
        }
        fprintf(stderr, " ");
        // ASCII
        for (int j = 0; j < 16 && (i + j) < count; j++) {
          uint8_t c = mem[(a + j) & 0xFFFF];
          fprintf(stderr, "%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        fprintf(stderr, "\n");
      }
      continue;
    }

    // Deposit to memory
    if (strcmp(cmd, "d") == 0) {
      if (arg1[0] == '\0' || arg2[0] == '\0') {
        fprintf(stderr, "Usage: d ADDR VAL [VAL...]\n");
        continue;
      }
      int addr = parse_address(arg1);
      if (addr < 0) {
        fprintf(stderr, "Invalid address: %s\n", arg1);
        continue;
      }
      // Parse remaining values from the line
      uint8_t* mem = cpu->get_mem();
      char* p = line;
      // Skip command
      while (*p && !isspace(*p)) p++;
      while (*p && isspace(*p)) p++;
      // Skip address
      while (*p && !isspace(*p)) p++;
      while (*p && isspace(*p)) p++;
      // Parse values
      int offset = 0;
      while (*p) {
        unsigned int val;
        if (sscanf(p, "%x", &val) != 1) break;
        mem[(addr + offset) & 0xFFFF] = val & 0xFF;
        offset++;
        // Skip this value
        while (*p && !isspace(*p)) p++;
        while (*p && isspace(*p)) p++;
      }
      fprintf(stderr, "  Deposited %d byte(s) at %04X\n", offset, addr);
      continue;
    }

    // Set breakpoint
    if (strcmp(cmd, "bp") == 0) {
      if (arg1[0] == '\0') {
        fprintf(stderr, "Usage: bp ADDR\n");
        continue;
      }
      int addr = parse_address(arg1);
      if (addr < 0) {
        fprintf(stderr, "Invalid address: %s\n", arg1);
        continue;
      }
      breakpoints.insert((uint16_t)addr);
      fprintf(stderr, "  Breakpoint set at %s\n", format_address((uint16_t)addr).c_str());
      continue;
    }

    // Clear breakpoint
    if (strcmp(cmd, "bc") == 0) {
      if (arg1[0] == '\0') {
        fprintf(stderr, "Usage: bc ADDR\n");
        continue;
      }
      int addr = parse_address(arg1);
      if (addr < 0) {
        fprintf(stderr, "Invalid address: %s\n", arg1);
        continue;
      }
      if (breakpoints.erase((uint16_t)addr)) {
        fprintf(stderr, "  Breakpoint cleared at %s\n", format_address((uint16_t)addr).c_str());
      } else {
        fprintf(stderr, "  No breakpoint at %04X\n", addr);
      }
      continue;
    }

    // List breakpoints
    if (strcmp(cmd, "bl") == 0) {
      if (breakpoints.empty()) {
        fprintf(stderr, "  No breakpoints set\n");
      } else {
        fprintf(stderr, "  Breakpoints:\n");
        for (uint16_t bp : breakpoints) {
          fprintf(stderr, "    %s\n", format_address(bp).c_str());
        }
      }
      continue;
    }

    // Clear all breakpoints
    if (strcmp(cmd, "ba") == 0) {
      size_t count = breakpoints.size();
      breakpoints.clear();
      fprintf(stderr, "  Cleared %zu breakpoint(s)\n", count);
      continue;
    }

    // Step
    if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0) {
      step_count = 1;
      if (arg1[0] != '\0') {
        int n = atoi(arg1);
        if (n > 0) step_count = n;
      }
      fprintf(stderr, "[Stepping %d instruction(s)...]\n", step_count);
      return CONSOLE_STEP;
    }

    // Set PC
    if (strcmp(cmd, "pc") == 0) {
      if (arg1[0] == '\0') {
        fprintf(stderr, "  PC=%s\n", format_address(cpu->regs.PC.get_pair16()).c_str());
        continue;
      }
      int addr = parse_address(arg1);
      if (addr < 0) {
        fprintf(stderr, "Invalid address: %s\n", arg1);
        continue;
      }
      cpu->regs.PC.set_pair16((uint16_t)addr);
      fprintf(stderr, "  PC set to %s\n", format_address((uint16_t)addr).c_str());
      continue;
    }

    // List symbols
    if (strcmp(cmd, "sym") == 0) {
      const char* pattern = arg1[0] ? arg1 : nullptr;
      int count = 0;
      for (auto& kv : symbols) {
        if (pattern == nullptr || strcasestr(kv.first.c_str(), pattern)) {
          fprintf(stderr, "  %04X %s\n", kv.second, kv.first.c_str());
          count++;
          if (count >= 50 && pattern == nullptr) {
            fprintf(stderr, "  ... (%zu total symbols, use 'sym PATTERN' to filter)\n",
                    symbols.size());
            break;
          }
        }
      }
      if (count == 0) {
        fprintf(stderr, "  No symbols%s\n", pattern ? " matching pattern" : " loaded");
      }
      continue;
    }

    // Help
    if (strcmp(cmd, "?") == 0 || strcmp(cmd, "help") == 0) {
      print_console_help();
      continue;
    }

    fprintf(stderr, "Unknown command: %s (try 'help')\n", cmd);
  }
}

class AltairEmulator : public HBIOSCPUDelegate {
private:
  hbios_cpu* cpu;
  banked_mem* memory;
  HBIOSDispatch hbios;  // Shared HBIOS dispatch
  bool debug;
  bool strict_io_mode;  // Halt on unexpected I/O ports
  bool halted;          // Set when halted due to unexpected I/O

  // Sense switches
  uint8_t sense_switches;

  // RomWBW romldr support - load real boot menu from RomWBW ROM
  std::string romldr_path;             // Path to romldr.bin or .rom file
  bool romldr_from_rom = false;        // True if loading from bank 1 of .rom file
  bool romldr_loaded = false;          // True if romldr was loaded successfully
  uint16_t next_pc = 0;                // Jump target for OUT redirect
  bool next_pc_valid = false;          // True if next_pc should be used

  // RAM bank initialization now uses HBIOSDispatch's shared bitmap
  // via hbios.getInitializedBanksBitmap()

public:
  AltairEmulator(hbios_cpu* acpu, banked_mem* amem, bool adebug = false)
    : cpu(acpu), memory(amem), debug(adebug),
      strict_io_mode(false), halted(false),
      sense_switches(0x00) {
    // Set ourselves as the delegate for port I/O callbacks
    cpu->delegate = this;

    // Initialize HBIOSDispatch
    hbios.setCPU(cpu);
    hbios.setMemory(memory);
    hbios.setBlockingAllowed(true);  // CLI can block on input
  }

  // HBIOSCPUDelegate interface implementation
  banked_mem* getMemory() override { return memory; }
  HBIOSDispatch* getHBIOS() override { return &hbios; }

  void initializeRamBankIfNeeded(uint8_t bank) override {
    initialize_ram_bank_if_needed(bank);
  }

  void onHalt() override {
    fprintf(stderr, "\n*** HALT instruction at PC=0x%04X ***\n",
            cpu->regs.PC.get_pair16());
    halted = true;
  }

  void onUnimplementedOpcode(uint8_t opcode, uint16_t pc) override {
    fprintf(stderr, "\n*** Unimplemented opcode 0x%02X at PC=0x%04X ***\n",
            opcode, pc);
    halted = true;
  }

  void logDebug(const char* fmt, ...) override {
    if (debug) {
      va_list args;
      va_start(args, fmt);
      vfprintf(stderr, fmt, args);
      va_end(args);
    }
  }

  void set_strict_io_mode(bool mode) { strict_io_mode = mode; }
  bool is_strict_io_mode() const { return strict_io_mode; }
  bool is_halted() const { return halted; }
  void set_halted(bool h) { halted = h; }

  // Get/clear next_pc override
  bool has_next_pc() { return next_pc_valid; }
  uint16_t get_next_pc() { next_pc_valid = false; return next_pc; }

  // Set romldr path (for loading RomWBW boot menu instead of emu_hbios menu)
  void set_romldr_path(const std::string& path) {
    romldr_path = path;
    // If it ends in .rom, we load from bank 1 (offset 0x8000)
    romldr_from_rom = (path.size() > 4 && path.substr(path.size() - 4) == ".rom");
  }

  // Load romldr ROM file into ROM banks (call after loading emu_hbios into bank 0)
  bool load_romldr_rom() {
    if (romldr_path.empty()) return false;

    banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
    if (!bmem) return false;

    romldr_loaded = emu_load_romldr_rom(bmem, romldr_path.c_str());
    return romldr_loaded;
  }


  void set_sense_switches(uint8_t val) {
    sense_switches = val;
    if (debug) fprintf(stderr, "Sense switches set to: 0x%02X\n", sense_switches);
  }

  // Initialize a RAM bank if it hasn't been initialized yet
  // Uses shared emu_init_ram_bank() with HBIOSDispatch's bitmap for unified tracking
  void initialize_ram_bank_if_needed(uint8_t bank) {
    banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
    if (!bmem) return;

    uint16_t* bitmap = hbios.getInitializedBanksBitmap();
    if (debug && emu_init_ram_bank(bmem, bank, bitmap)) {
      fprintf(stderr, "[BANK INIT] Initialized RAM bank 0x%02X\n", bank);
    } else {
      emu_init_ram_bank(bmem, bank, bitmap);
    }
  }

  // Port I/O is now handled by hbios_cpu class
  // The following helper methods are called from hbios_cpu via HBIOSCPUDelegate

  // NOTE: handle_in/handle_out removed - now in hbios_cpu.cc

  // Provide access to HBIOSDispatch for output flushing
  void flush_output() {
    while (hbios.hasOutputChars()) {
      std::vector<uint8_t> chars = hbios.getOutputChars();
      for (uint8_t ch : chars) {
        emu_console_write_char(ch);
      }
    }
  }

  // Poll stdin for escape character only
  // Input is read directly by CIOIN/VDAKRD via emu_console_read_char
  void poll_stdin() {
    // Runs after every executed instruction, so the early-out matters twice
    // over: with --escape=none it keeps the guest's keys, and it removes the
    // per-instruction select() this used to cost.
    if (console_escape_char == 0) return;
    // Check for console escape first
    if (emu_console_check_escape(console_escape_char)) {
      console_mode_requested = true;
    }
  }
};

// Disk size constants and MBR checking are now in emu_init.h/emu_init.cc

// The RomWBW release this build emulates is part of the version identity:
// a client has to pair the binary with a ROM and disk images cut from the
// same release, so print it where the version is printed.
static void print_version_banner() {
  fprintf(stderr, "RomWBW Emulator v%s (built %s)\n", EMU_VERSION, emu_build_date);
  fprintf(stderr, "RomWBW compatibility: v%s (pinned)\n", ROMWBW_PIN_STR);
}

void print_usage(const char* prog) {
  print_version_banner();
  fprintf(stderr, "Usage: %s --romwbw=<rom.rom> [options]\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --version, -v     Show version information\n");
  fprintf(stderr, "  --romwbw=FILE     Enable RomWBW mode with ROM file (512KB ROM+RAM, Z80)\n");
  fprintf(stderr, "  --strict-io       Halt on unexpected I/O ports (for debugging)\n");
  fprintf(stderr, "  --debug           Enable debug output\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Boot options:\n");
  fprintf(stderr, "  --boot=CMD        Auto-boot with command (e.g., C, 2, 2.3)\n");
  fprintf(stderr, "                    Overrides the persisted NVRAM setting for\n");
  fprintf(stderr, "                    THIS RUN ONLY - it is not written back, so a\n");
  fprintf(stderr, "                    script cannot change what you boot by default.\n");
  fprintf(stderr, "  --boot=none       Forget the persisted boot target and come up at\n");
  fprintf(stderr, "                    the menu ('off' is the same). This is the only\n");
  fprintf(stderr, "                    way to undo an earlier --boot or SYSCONF.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  NVRAM is persisted to $XDG_CONFIG_HOME/romwbw_emu/nvram (default ~/.config/romwbw_emu/nvram)\n");
  fprintf(stderr, "  Use 'W' at boot menu to configure via SYSCONF utility.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "File transfer (run inside CP/M):\n");
  fprintf(stderr, "  R8 <hostpath>     Import host file (path relative to emulator CWD, or absolute)\n");
  fprintf(stderr, "  W8 <cpmfile> [hostpath]\n");
  fprintf(stderr, "                    Export CP/M file.  With no hostpath it lands in the\n");
  fprintf(stderr, "                    emulator CWD under the CP/M name, lowercased.\n");
  fprintf(stderr, "  Both take the whole rest of the line as the path, so a directory name may\n");
  fprintf(stderr, "  contain spaces.  CP/M uppercases it, so the emulator matches the existing\n");
  fprintf(stderr, "  directories case-insensitively and lowercases the file name it creates;\n");
  fprintf(stderr, "  W8 prints the path it actually wrote.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Settings file (JSON; CLI flags always override):\n");
  fprintf(stderr, "  --config=FILE     Load settings from FILE (missing/malformed = error)\n");
  fprintf(stderr, "  --no-config       Ignore ./romwbw_emu.json and the config-dir file\n");
  fprintf(stderr, "  --save-config[=F] Write the effective settings to F (default ./romwbw_emu.json) and exit\n");
  fprintf(stderr, "  Searched automatically: ./romwbw_emu.json, then $XDG_CONFIG_HOME/romwbw_emu/config.json\n");
  fprintf(stderr, "  (default ~/.config/romwbw_emu/config.json). See docs/CONFIGURATION.md.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Disk options:\n");
  fprintf(stderr, "  --disk0=FILE      Attach disk image to slot 0\n");
  fprintf(stderr, "  --disk1=FILE      Attach disk image to slot 1\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  Supported disk formats (auto-detected):\n");
  fprintf(stderr, "    hd1k  - Modern RomWBW format, 8MB per slice, 1024 dir entries\n");
  fprintf(stderr, "    hd512 - Classic format, 8.32MB per slice, 512 dir entries\n");
  fprintf(stderr, "  Disk files must exist and have valid sizes (8MB or 8.32MB per slice).\n");
  fprintf(stderr, "  Combo disks with 1MB MBR prefix + multiple slices are supported.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Other options:\n");
  fprintf(stderr, "  --escape=CHAR     Key reserved for console mode: ^A..^_ or a literal\n");
  fprintf(stderr, "                    char (default ^E). The key is TAKEN AWAY from the\n");
  fprintf(stderr, "                    guest - ^E is cursor-up in the WordStar diamond.\n");
  fprintf(stderr, "  --escape=none     Reserve no key; every byte reaches CP/M ('off' is\n");
  fprintf(stderr, "                    the same). Console mode is then unreachable.\n");
  fprintf(stderr, "  --trace=FILE      Write execution trace to FILE\n");
  fprintf(stderr, "  --symbols=FILE    Load symbol table from FILE (.sym)\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Console mode:\n");
  fprintf(stderr, "  Press the escape char (default Ctrl+E) to enter console mode. That key\n");
  fprintf(stderr, "  never reaches CP/M; use --escape=none when the guest needs it (WordStar\n");
  fprintf(stderr, "  and its descendants use ^E for cursor-up).\n");
  fprintf(stderr, "  Type 'help' in console mode for available commands.\n");
  fprintf(stderr, "  Use 'quit' to exit.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Examples:\n");
  fprintf(stderr, "  %s --romwbw=roms/emu_avw.rom\n", prog);
  fprintf(stderr, "  %s --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img\n", prog);
  fprintf(stderr, "  %s --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_infocom.img:1\n", prog);
}

int main(int argc, char** argv) {
  // Parse arguments
  const char* binary = nullptr;
  uint16_t load_addr = 0x0000;
  uint16_t start_addr = 0x0000;
  bool start_addr_set = false;
  bool debug = false;
  bool strict_io_mode = false;
  int sense = -1;
  std::string hbios_disks[16];  // For RomWBW disk images (HBIOS dispatch)
  std::string trace_file;
  std::string symbols_file;
  std::string romldr_path;  // RomWBW romldr boot menu
  std::string boot_string;  // Auto-boot command (e.g., "C", "2", "2.3")
  // A --boot on the command line is a one-off override for this run, not a new
  // persisted setting.  Without this, every scripted boot rewrote whatever the
  // developer had configured, because the exit path saves whatever NVRAM holds.
  bool boot_from_cli = false;
  bool boot_clear = false;  // --boot=none: forget the persisted target too

  // ROM application definitions: key=name:path
  struct RomAppDef {
    char key;
    std::string name;
    std::string path;
  };
  std::vector<RomAppDef> rom_app_defs;

  // Settings file: pre-scan for the options that must act before the main
  // argv loop (the loop itself treats them as no-ops)
  std::string cli_config_path;   // from --config= only
  std::string config_path;       // whichever file actually loaded
  bool no_config = false;
  bool save_config = false;
  std::string save_config_path;
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--config=", 9) == 0) {
      cli_config_path = argv[i] + 9;
    } else if (strcmp(argv[i], "--no-config") == 0) {
      no_config = true;
    } else if (strcmp(argv[i], "--save-config") == 0) {
      save_config = true;
    } else if (strncmp(argv[i], "--save-config=", 14) == 0) {
      save_config = true;
      save_config_path = argv[i] + 14;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      // Informational flags must work even with a malformed config in cwd
      print_usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      print_version_banner();
      fprintf(stderr, "Emulates RomWBW with HBIOS, boots CP/M/ZSDOS from ROM disk\n");
      return 0;
    }
  }

  // Load the settings file. A malformed or missing explicit file is a hard
  // error - a silently-ignored config could boot the wrong disks.
  EmuConfig file_cfg;
  bool have_config = false;
  if (!cli_config_path.empty()) {
    std::string cfg_err;
    if (!emu_config_load(cli_config_path, file_cfg, cfg_err)) {
      fprintf(stderr, "Error: --config=%s: %s\n", cli_config_path.c_str(), cfg_err.c_str());
      return 1;
    }
    config_path = cli_config_path;
    have_config = true;
  } else if (!no_config) {
    std::string found = emu_config_discover(get_config_dir());
    if (!found.empty()) {
      std::string cfg_err;
      if (!emu_config_load(found, file_cfg, cfg_err)) {
        fprintf(stderr, "Error: config %s: %s\n", found.c_str(), cfg_err.c_str());
        return 1;
      }
      config_path = found;
      have_config = true;
    }
  }
  if (have_config) {
    fprintf(stderr, "[CONFIG] Loaded %s (--no-config to ignore)\n", config_path.c_str());
  }

  // No arguments and no config supplying a ROM: show usage (a config file
  // makes a bare "romwbw_emu" a valid, fully-described invocation)
  if (argc < 2 && file_cfg.rom.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  // Apply the file first; the argv loop below runs after and naturally
  // overrides (defaults < file < CLI). Config-sourced disks and escape are
  // validated only AFTER the merge - a stale config value must not be fatal
  // when the command line overrides it.
  bool disk_from_config[16] = {};
  bool cli_escape_set = false;
  if (have_config) {
    if (!file_cfg.rom.empty()) binary = file_cfg.rom.c_str();
    if (!file_cfg.boot.empty()) boot_string = file_cfg.boot;
    if (!file_cfg.symbols.empty()) symbols_file = file_cfg.symbols;
    if (!file_cfg.romldr.empty()) romldr_path = file_cfg.romldr;
    if (file_cfg.debug) debug = true;
    if (file_cfg.strictIo) strict_io_mode = true;
    for (int u = 0; u < 16; u++) {
      if (file_cfg.disks[u].empty()) continue;
      hbios_disks[u] = file_cfg.disks[u];
      disk_from_config[u] = true;
    }
    for (const EmuConfig::RomApp& a : file_cfg.romapps) {
      RomAppDef def;
      def.key = a.key;
      def.path = a.path;
      if (!a.name.empty()) {
        def.name = a.name;
      } else if (a.key == 'C') def.name = "CP/M 2.2";
      else if (a.key == 'Z') def.name = "ZSDOS";
      else if (a.key == 'Q') def.name = "QPM";
      else if (a.key == 'P') def.name = "CP/M 3";
      else def.name = std::string(1, a.key) + " Application";
      rom_app_defs.push_back(def);
    }
  }

  for (int i = 1; i < argc; i++) {
    // --version/-v and --help/-h are handled by the pre-scan above (they
    // must work even when a malformed config file sits in the cwd)
    if (strcmp(argv[i], "--debug") == 0) {
      debug = true;
    } else if (strncmp(argv[i], "--romwbw=", 9) == 0) {
      binary = argv[i] + 9;
    } else if (strcmp(argv[i], "--strict-io") == 0) {
      strict_io_mode = true;
    } else if (strncmp(argv[i], "--sense=", 8) == 0) {
      sense = (int)strtol(argv[i] + 8, nullptr, 0);
    } else if (strncmp(argv[i], "--load=", 7) == 0) {
      load_addr = (uint16_t)strtol(argv[i] + 7, nullptr, 0);
    } else if (strncmp(argv[i], "--start=", 8) == 0) {
      start_addr = (uint16_t)strtol(argv[i] + 8, nullptr, 0);
      start_addr_set = true;
    } else if (strncmp(argv[i], "--disk", 6) == 0) {
      // Parse --disk0=file, --disk1=file (preferred form)
      const char* opt = argv[i] + 6;
      int unit = -1;
      const char* path_start = nullptr;
      if (isdigit(opt[0]) && opt[1] == '=' && opt[2] != '\0') {
        unit = opt[0] - '0';
        path_start = opt + 2;
      } else if (isdigit(opt[0]) && isdigit(opt[1]) && opt[2] == '=' && opt[3] != '\0') {
        unit = (opt[0] - '0') * 10 + (opt[1] - '0');
        path_start = opt + 3;
      }
      if (unit >= 0 && unit < 16 && path_start) {
        std::string path_str(path_start);
        // Validate disk image exists and has valid size
        size_t disk_size = 0;
        const char* err = emu_validate_disk_image(path_str.c_str(), &disk_size);
        if (err) {
          fprintf(stderr, "Error: --disk%d=%s: %s\n", unit, path_str.c_str(), err);
          return 1;
        }
        hbios_disks[unit] = path_str;
        disk_from_config[unit] = false;  // CLI overrode any config value
        fprintf(stderr, "[DISK] Validated disk%d: %s (%zu bytes)\n", unit, path_str.c_str(), disk_size);
      } else {
        fprintf(stderr, "Invalid --disk option: %s (use --disk0=file or --disk1=file)\n", argv[i]);
        return 1;
      }
    } else if (strncmp(argv[i], "--romapp=", 9) == 0) {
      // Parse --romapp=K=Name:path  (K is the boot key, Name is display name, path is .sys file)
      // e.g., --romapp=C=CP/M 2.2:cpm_wbw.sys
      // For convenience, also accept: --romapp=C:cpm_wbw.sys (auto-names based on key)
      const char* opt = argv[i] + 9;
      RomAppDef def;
      def.key = 0;

      if (isalpha(opt[0]) && opt[1] == '=') {
        // Format: K=Name:path
        def.key = (char)toupper(opt[0]);
        const char* rest = opt + 2;
        const char* colon = strchr(rest, ':');
        if (colon && colon[1] != '\0') {
          def.name = std::string(rest, colon - rest);
          def.path = colon + 1;
        } else {
          fprintf(stderr, "Invalid --romapp format: %s (use K=Name:path)\n", argv[i]);
          return 1;
        }
      } else if (isalpha(opt[0]) && opt[1] == ':') {
        // Format: K:path (auto-name)
        def.key = (char)toupper(opt[0]);
        def.path = opt + 2;
        // Auto-generate name from key
        if (def.key == 'C') def.name = "CP/M 2.2";
        else if (def.key == 'Z') def.name = "ZSDOS";
        else if (def.key == 'Q') def.name = "QPM";
        else if (def.key == 'P') def.name = "CP/M 3";
        else def.name = std::string(1, def.key) + " Application";
      } else {
        fprintf(stderr, "Invalid --romapp format: %s (use K=Name:path or K:path)\n", argv[i]);
        return 1;
      }

      if (def.key && !def.path.empty()) {
        rom_app_defs.push_back(def);
      }
    } else if (strncmp(argv[i], "--romldr=", 9) == 0) {
      romldr_path = argv[i] + 9;
    } else if (strncmp(argv[i], "--boot=", 7) == 0) {
      const char* b = argv[i] + 7;
      // "none"/"off" are spelled the way --escape spells them.  They are the
      // only way back to the boot menu once a target is persisted: setting the
      // in-core NVRAM to "uninitialized" is not enough, because the setting
      // lives in a file that outlives the run.
      if (emu_strcasecmp(b, "none") == 0 || emu_strcasecmp(b, "off") == 0) {
        boot_clear = true;
        boot_string.clear();
      } else {
        boot_clear = false;
        boot_string = b;
      }
      boot_from_cli = true;
    } else if (strncmp(argv[i], "--trace=", 8) == 0) {
      trace_file = argv[i] + 8;
    } else if (strncmp(argv[i], "--symbols=", 10) == 0) {
      symbols_file = argv[i] + 10;
    } else if (strncmp(argv[i], "--escape=", 9) == 0) {
      // Parse escape character: ^X for control chars, or literal char
      const char* esc = argv[i] + 9;
      if (esc[0] != '\0') {
        if (!parse_escape_char(esc, &console_escape_char)) {
          fprintf(stderr, "Invalid escape char: %s (use ^A through ^_, a literal character, or none)\n", esc);
          return 1;
        }
        cli_escape_set = true;
      }
    } else if (strncmp(argv[i], "--config=", 9) == 0 ||
               strcmp(argv[i], "--no-config") == 0 ||
               strcmp(argv[i], "--save-config") == 0 ||
               strncmp(argv[i], "--save-config=", 14) == 0) {
      // Already handled by the pre-scan above
    } else if (strcmp(argv[i], "--mask-interrupt") == 0) {
      // Parse: --mask-interrupt 4000-4500 rst 7
      //    or: --mask-interrupt 5000-6000 call 0x0100
      if (i + 3 >= argc) {
        fprintf(stderr, "Error: --mask-interrupt requires: <min>-<max> <rst|call> <num|addr>\n");
        return 1;
      }
      i++;
      // Parse cycle range
      unsigned int cmin, cmax;
      if (sscanf(argv[i], "%u-%u", &cmin, &cmax) != 2) {
        // Try single value
        if (sscanf(argv[i], "%u", &cmin) == 1) {
          cmax = cmin;
        } else {
          fprintf(stderr, "Error: Invalid cycle range '%s'\n", argv[i]);
          return 1;
        }
      }
      i++;
      // Parse type (rst or call)
      const char* int_type = argv[i];
      i++;
      // Parse value
      unsigned int int_val;
      if (strncmp(argv[i], "0x", 2) == 0 || strncmp(argv[i], "0X", 2) == 0) {
        sscanf(argv[i] + 2, "%x", &int_val);
      } else {
        int_val = atoi(argv[i]);
      }

      maskable_int_config.enabled = true;
      maskable_int_config.cycle_min = cmin;
      maskable_int_config.cycle_max = cmax;
      if (strcmp(int_type, "rst") == 0 || strcmp(int_type, "RST") == 0) {
        maskable_int_config.use_rst = true;
        maskable_int_config.rst_num = int_val & 7;  // RST 0-7
      } else if (strcmp(int_type, "call") == 0 || strcmp(int_type, "CALL") == 0) {
        maskable_int_config.use_rst = false;
        maskable_int_config.call_addr = int_val & 0xFFFF;
      } else {
        fprintf(stderr, "Error: Unknown interrupt type '%s' (use 'rst' or 'call')\n", int_type);
        return 1;
      }
    } else if (strcmp(argv[i], "--nmi") == 0) {
      // Parse: --nmi 10000-12000
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: --nmi requires: <min>-<max>\n");
        return 1;
      }
      i++;
      // Parse cycle range
      unsigned int cmin, cmax;
      if (sscanf(argv[i], "%u-%u", &cmin, &cmax) != 2) {
        // Try single value
        if (sscanf(argv[i], "%u", &cmin) == 1) {
          cmax = cmin;
        } else {
          fprintf(stderr, "Error: Invalid cycle range '%s'\n", argv[i]);
          return 1;
        }
      }

      nmi_config.enabled = true;
      nmi_config.cycle_min = cmin;
      nmi_config.cycle_max = cmax;
      // NMI always jumps to 0x0066
      nmi_config.use_rst = false;
      nmi_config.call_addr = 0x0066;
    } else if (argv[i][0] != '-') {
      binary = argv[i];
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    }
  }

  // Post-merge validation of config-sourced values that the CLI did not
  // override (CLI values were validated inline as they were parsed)
  if (!cli_escape_set && have_config && !file_cfg.escape.empty() &&
      !parse_escape_char(file_cfg.escape.c_str(), &console_escape_char)) {
    fprintf(stderr, "Error: config %s: invalid \"escape\" value '%s' (use ^A through ^_, a literal character, or \"none\")\n",
            config_path.c_str(), file_cfg.escape.c_str());
    return 1;
  }
  for (int u = 0; u < 16; u++) {
    if (!disk_from_config[u] || hbios_disks[u].empty()) continue;
    size_t disk_size = 0;
    const char* err = emu_validate_disk_image(hbios_disks[u].c_str(), &disk_size);
    if (err) {
      fprintf(stderr, "Error: config disk%d: %s: %s\n", u, hbios_disks[u].c_str(), err);
      return 1;
    }
    fprintf(stderr, "[DISK] Validated disk%d: %s (%zu bytes)\n", u, hbios_disks[u].c_str(), disk_size);
  }

  // Merge romapps by key: config entries were added first, CLI entries after,
  // so on a duplicate key the latest (highest-index = CLI) definition wins.
  // Copy only the first match of the downward scan; later matches in the scan
  // are older definitions and are just dropped.
  for (size_t i = 0; i < rom_app_defs.size(); i++) {
    bool copied = false;
    for (size_t k = rom_app_defs.size(); k-- > i + 1; ) {
      if (rom_app_defs[k].key == rom_app_defs[i].key) {
        if (!copied) {
          rom_app_defs[i] = rom_app_defs[k];
          copied = true;
        }
        rom_app_defs.erase(rom_app_defs.begin() + k);
      }
    }
  }

  // --save-config: write the effective machine description and exit.
  // Everything is validated by this point (disks checked, escape parsed).
  if (save_config) {
    EmuConfig eff;
    if (binary) eff.rom = binary;
    eff.boot = boot_string;  // non-empty only if --boot/config set it (NVRAM loads later)
    // Round-trip a disabled escape as the word, not as "^@": the two parse
    // to the same value, but a saved file that says "^@" reads like a real
    // binding and 0x00 is a byte terminals genuinely send (Ctrl+Space).
    eff.escape = escape_key_name();
    eff.symbols = symbols_file;
    eff.romldr = romldr_path;
    eff.debug = debug;
    eff.strictIo = strict_io_mode;
    for (int u = 0; u < 16; u++) eff.disks[u] = hbios_disks[u];
    for (const RomAppDef& d : rom_app_defs) {
      EmuConfig::RomApp a;
      a.key = d.key;
      a.name = d.name;
      a.path = d.path;
      eff.romapps.push_back(a);
    }
    // Target: explicit --save-config=FILE, else the --config file, else
    // ./romwbw_emu.json - never a discovered XDG path
    std::string target = !save_config_path.empty() ? save_config_path
                       : !cli_config_path.empty()  ? cli_config_path
                       : "romwbw_emu.json";
    std::string save_err;
    if (!emu_config_save(target, eff, save_err)) {
      fprintf(stderr, "Error: --save-config: %s\n", save_err.c_str());
      return 1;
    }
    fprintf(stderr, "Saved config to %s\n", target.c_str());
    return 0;
  }

  if (!binary) {
    fprintf(stderr, "Error: No binary file specified\n");
    return 1;
  }

  // Set defaults based on mode
  // RomWBW starts at address 0x0000 in ROM bank 0
  if (!start_addr_set) start_addr = 0x0000;

  // Create memory and CPU
  banked_mem memory;
  hbios_cpu cpu(&memory, nullptr);  // delegate set below

  // Set Z80 mode and enable banking
  cpu.set_cpu_mode(qkz80::MODE_Z80);
  fprintf(stderr, "CPU mode: Z80\n");
  memory.enable_banking();
  memory.set_debug(debug);
  emu_set_debug(debug);  // gate emu_log() internal traces on --debug
  fprintf(stderr, "RomWBW mode: 512KB ROM + 512KB RAM, bank switching enabled\n");

  // Create emulator (sets cpu.delegate in constructor)
  AltairEmulator emu(&cpu, &memory, debug);
  emu.set_strict_io_mode(strict_io_mode);
  emu.getHBIOS()->setDebug(debug);  // Enable HBIOS debug output

  // Set up HBIOS disk images
  // NOTE: Memory disks are initialized later, after ROM is loaded
  // Attach any file-backed hard disk images (HBIOS dispatch protocol)
  int disk_count = 0;
  for (int i = 0; i < 16; i++) {
    if (!hbios_disks[i].empty()) {
      if (!emu.getHBIOS()->loadDiskFromFile(i, hbios_disks[i])) {
        fprintf(stderr, "Warning: Could not attach disk %d: %s\n", i, hbios_disks[i].c_str());
      } else {
        disk_count++;
      }
    }
  }

  // Calculate slice count for drive letter assignment based on disk count
  // (matching CBIOS logic): 1 disk = 8 slices, 2 disks = 4 each, 3+ = 2 each
  // Note: This only affects drive letter assignment, not slice access limits
  int auto_slices = (disk_count <= 1) ? 8 : (disk_count == 2) ? 4 : 2;

  // Apply slice counts for drive letter assignment
  for (int i = 0; i < 16; i++) {
    if (emu.getHBIOS()->isDiskLoaded(i)) {
      emu.getHBIOS()->setDiskSliceCount(i, auto_slices);
    }
  }

  // Register ROM applications with HBIOSDispatch
  for (const auto& def : rom_app_defs) {
    emu.getHBIOS()->addRomApp(def.name, def.path, def.key);
  }

  // Load persisted NVRAM from the config dir (XDG-aware; see get_config_dir)
  std::string nvram_path = get_nvram_path();
  std::string legacy_nvram_path = get_legacy_nvram_path();
  // The effective setting --boot asked for, as NVRAM spells it back.  Compared
  // at exit against what NVRAM then holds: equal means nothing but --boot ever
  // touched it, so there is nothing of the user's to save.
  std::string cli_boot_effective;

  if (boot_clear) {
    // --boot=none removes the file rather than writing an empty one.  An
    // absent file and a file holding "" already mean the same thing to the
    // loader below, and leaving a stub behind would make `ls` suggest a
    // setting is still configured.
    //
    // Only the file under the CURRENT config dir is removed.  The pre-XDG file
    // in ~/.config is named rather than unlinked - see below.
    bool removed = (remove(nvram_path.c_str()) == 0);
    emu.getHBIOS()->setNvramSetting("");
    if (removed) {
      fprintf(stderr, "--boot=none: cleared the persisted boot target (%s)\n",
              nvram_path.c_str());
    } else {
      // Not "no persisted boot target to clear": one path was looked at, and
      // the pre-XDG file tested below can still hold one.  Name the path that
      // was empty and let the next line name the other.
      fprintf(stderr, "--boot=none: nothing to clear at %s\n",
              nvram_path.c_str());
    }
    // A pre-XDG setting in ~/.config is still READ as a migration fallback, so
    // it would come back on the next run - but it sits outside the directory
    // XDG_CONFIG_HOME selected, and nothing here can tell a user who has
    // genuinely relocated their config from a scripted run pointing XDG at a
    // temp directory.  Deleting it guessed, and guessed wrong: a second
    // --boot=none under a temp XDG_CONFIG_HOME unlinked the developer's real
    // ~/.config/romwbw_emu/nvram, because the first one left the current path
    // empty and that was the whole of the guard.  Say the file is there
    // instead of reaching outside the config dir to remove it.
    if (legacy_nvram_path != nvram_path &&
        !load_nvram_setting(legacy_nvram_path).empty()) {
      fprintf(stderr,
              "--boot=none: a pre-XDG setting remains at %s and will be read "
              "again next run; remove that file by hand to clear it too\n",
              legacy_nvram_path.c_str());
    }
  } else {
    std::string nvram_loaded_from = nvram_path;
    std::string loaded_setting = load_nvram_setting(nvram_path);
    if (loaded_setting.empty()) {
      // Migration: a setting saved before XDG_CONFIG_HOME support lives in
      // ~/.config; read it here, saves go to the new path at exit
      if (legacy_nvram_path != nvram_path) {
        loaded_setting = load_nvram_setting(legacy_nvram_path);
        if (!loaded_setting.empty()) nvram_loaded_from = legacy_nvram_path;
      }
    }
    if (!loaded_setting.empty()) {
      emu.getHBIOS()->setNvramSetting(loaded_setting);
      fprintf(stderr, "Loaded NVRAM setting '%s' from %s%s\n", loaded_setting.c_str(),
              nvram_loaded_from.c_str(),
              nvram_loaded_from != nvram_path ? " (migrates to the new path on exit)" : "");
    }

    // Configure NVRAM boot option if specified (overrides persisted settings)
    if (!boot_string.empty()) {
      emu.getHBIOS()->setNvramSetting(boot_string);
      // Read it straight back: NVRAM normalises - "c" comes back as "C" and
      // "2.0" as "2" - so the string to compare at exit is this one, not the
      // one that was typed.
      cli_boot_effective = emu.getHBIOS()->getNvramSetting();
      fprintf(stderr, "Auto-boot: configured NVRAM for '%s'%s\n", boot_string.c_str(),
              boot_from_cli ? " (this run only)" : "");
    }
  }

  // Set romldr path if specified
  if (!romldr_path.empty()) {
    emu.set_romldr_path(romldr_path);
  }

  if (sense >= 0) {
    emu.set_sense_switches(sense & 0xFF);
  }

  // Load symbols if specified
  if (!symbols_file.empty()) {
    if (!load_symbols(symbols_file.c_str())) {
      fprintf(stderr, "Warning: Could not load symbols from %s\n", symbols_file.c_str());
    }
  }

  // Print console escape char info. Name the key AND say it is taken away:
  // the default ^E is cursor-up in the WordStar diamond, and a user who does
  // not know it is reserved concludes the editor is broken.
  if (console_escape_char == 0) {
    fprintf(stderr, "Console escape: none - every key goes to the guest (sim> unreachable)\n");
  } else if (!isatty(STDIN_FILENO)) {
    // The escape is a tty-only mechanism: emu_console_check_escape() returns
    // early for a pipe or a file, so on redirected stdin the key reaches the
    // guest and there is no way into sim>. Say so rather than claiming a
    // reservation that is not in force. isatty() directly, not the cached
    // stdin_is_tty - emu_io_init() has not run yet at this point.
    fprintf(stderr, "Console escape: %s - reserved on an interactive terminal only;"
                    " stdin is not a tty, so the key reaches the guest\n",
            escape_key_name().c_str());
  } else {
    fprintf(stderr, "Console escape: %s - reserved by the emulator, the guest never sees it"
                    " (--escape=none to disable)\n", escape_key_name().c_str());
  }

  // Enable raw terminal mode
  emu_io_init();

  {
    // Load ROM image into banked memory
    if (!emu_load_rom(&memory, binary)) {
      return 1;
    }
    fprintf(stderr, "Starting execution at 0x%04X in ROM bank 0\n", start_addr);

    // If romldr path specified, load full RomWBW ROM (preserving bank 0)
    if (!romldr_path.empty()) {
      emu.load_romldr_rom();
    }

    // Use shared initialization sequence:
    // 1. Patch APITYPE in ROM
    // 2. Copy HCB to RAM
    // 3. Set up HBIOS ident signatures
    // 4. Initialize memory disks and populate disk tables
    emu_complete_init(&memory, emu.getHBIOS(), nullptr);
  }

  // Register reset callback for SYSRESET (ROM reboot command 'R')
  emu_setup_reset_callback(&memory, &cpu, emu.getHBIOS());

  // Enable tracing if requested
  if (!trace_file.empty()) {
    memory.enable_tracing(true);
    fprintf(stderr, "Execution tracing enabled, will write to: %s\n", trace_file.c_str());
  }

  // Set PC to start address
  cpu.regs.PC.set_pair16(start_addr);

  // RomWBW ROM initializes SP itself, but start with safe value
  cpu.regs.SP.set_pair16(0x0000);  // Will be set by ROM

  // Signal handler for graceful stop
  auto signal_handler = [](int sig) {
    (void)sig;
    stop_requested = true;
  };
  // Use sigaction with sa_flags=0 (no SA_RESTART): a handler must interrupt
  // the blocking read/select on stdin so the main loop can observe
  // stop_requested - glibc signal() installs SA_RESTART, which silently
  // restarts the read and makes SIGHUP/SIGTERM unkillable on a piped stdin
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  // Terminal close: exit through end of main so NVRAM and the trace file are
  // saved; keep SIG_IGN if started under nohup
  struct sigaction old_hup;
  sigaction(SIGHUP, nullptr, &old_hup);
  if (old_hup.sa_handler != SIG_IGN) {
    sigaction(SIGHUP, &sa, nullptr);
  }

  // Initialize interrupt triggers
  if (maskable_int_config.enabled) {
    maskable_int_config.next_trigger = get_next_trigger(maskable_int_config, 0);
    fprintf(stderr, "Maskable interrupts enabled: %u-%u cycles, %s %u\n",
            maskable_int_config.cycle_min, maskable_int_config.cycle_max,
            maskable_int_config.use_rst ? "RST" : "CALL",
            maskable_int_config.use_rst ? maskable_int_config.rst_num : maskable_int_config.call_addr);
  }
  if (nmi_config.enabled) {
    nmi_config.next_trigger = get_next_trigger(nmi_config, 0);
    fprintf(stderr, "NMI enabled: %u-%u cycles, jump to 0x0066\n",
            nmi_config.cycle_min, nmi_config.cycle_max);
  }

  // Main execution loop
  long long instruction_count = 0;
  long long max_instructions = 10000000000LL;  // 10 billion max
  bool in_step_mode = false;  // True if stepping from console

  while (!stop_requested) {
    uint16_t pc = cpu.regs.PC.get_pair16();
    uint8_t opcode = memory.fetch_mem(pc, true) & 0xFF;

    // Check for breakpoint hit
    if (breakpoints.count(pc) && !in_step_mode) {
      fprintf(stderr, "\n[Breakpoint hit at %s]\n", format_address(pc).c_str());
      console_mode_requested = true;
    }

    // Check if stepping is complete
    if (in_step_mode && step_count <= 0) {
      console_mode_requested = true;
      in_step_mode = false;
    }

    // Note: console escape check is now done in emu.poll_stdin() which
    // calls emu_console_check_escape() and sets console_mode_requested

    // Handle console mode
    if (console_mode_requested) {
      console_mode_requested = false;
      ConsoleResult result = handle_console_mode(&cpu, &memory);
      switch (result) {
        case CONSOLE_QUIT:
          stop_requested = true;
          continue;
        case CONSOLE_CONTINUE:
          in_step_mode = false;
          continue;
        case CONSOLE_STEP:
          in_step_mode = true;
          // step_count is set by handle_console_mode
          break;
        case CONSOLE_AGAIN:
          continue;
      }
    }

    // Check for HLT instruction
    if (opcode == 0x76) {
      fprintf(stderr, "\nHLT instruction at 0x%04X\n", pc);
      break;
    }

    // Debug: track first 50000 instructions after boot to see where we go
    static long debug_count = 0;
    if (debug && debug_count < 50000 && instruction_count > 1) {
      debug_count++;
      if (debug_count % 1000 == 0 || (pc >= 0xF600 && pc < 0xF700) ||
          pc == 0xEB59 || pc == 0xEB5C || pc == 0xE806 || pc == 0xF483) {
        fprintf(stderr, "[TRACE %ld: PC=0x%04X, op=0x%02X]\n", debug_count, pc, opcode);
      }
    }

    // Debug: trace instructions after CIOIN (disabled for normal use)
    // emu.trace_after_cioin(pc, opcode);

    // Execute one instruction (I/O is handled via hbios_cpu port_in/port_out)
    cpu.execute();
    instruction_count++;
    if (in_step_mode) step_count--;

    // Poll stdin and queue input to HBIOSDispatch
    emu.poll_stdin();

    // Piped stdin at EOF: no input can ever arrive, so wind down through
    // end of main (NVRAM/trace still get written) instead of spinning in
    // the guest's console poll loop forever. Two triggers: the guest read
    // past EOF (blocking CIOIN), or the guest is only *polling* status
    // (e.g. the romldr boot menu) - console-idle plus EOF means it waits
    // for input that can never come. isConsoleIdle resets on every CIOOUT,
    // so a guest that is still producing output is never cut off.
    if (emu_console_input_exhausted() ||
        (emu_console_input_eof() && emu.getHBIOS()->isConsoleIdle())) {
      stop_requested = true;
    }

    // Flush output from HBIOSDispatch to stdout
    emu.flush_output();

    // Sleep when guest is idle (polling console with no input available)
    // to reduce CPU usage and power draw.
    if (emu.getHBIOS()->isConsoleIdle()) {
      usleep(10000);  // 10ms — imperceptible latency, significant power savings
    }

    // Check if strict I/O mode halted us during port operations
    if (emu.is_halted()) {
      fprintf(stderr, "\nEmulator halted (strict I/O mode)\n");
      break;
    }

    // Check if OUT handler wants to redirect PC (e.g., for romldr bank switch)
    if (emu.has_next_pc()) {
      cpu.regs.PC.set_pair16(emu.get_next_pc());
    }

    // Check for scheduled interrupts (using upstream qkz80 interrupt API)
    if (nmi_config.enabled && cpu.cycles >= nmi_config.next_trigger) {
      cpu.request_nmi();
      nmi_config.next_trigger = get_next_trigger(nmi_config, cpu.cycles);
    }
    if (maskable_int_config.enabled &&
        cpu.cycles >= maskable_int_config.next_trigger &&
        !waiting_for_int_delivery) {
      // Request interrupt using upstream API
      if (maskable_int_config.use_rst) {
        cpu.request_rst((uint8_t)maskable_int_config.rst_num);
      } else {
        // CALL mode: use IM0 with RST 38H vector (0xFF)
        cpu.request_int(0xFF);
      }
      waiting_for_int_delivery = true;
    }

    // Deliver pending interrupts
    cpu.check_interrupts();

    // If we were waiting for INT delivery and it completed, schedule next
    if (waiting_for_int_delivery && !cpu.int_pending) {
      maskable_int_config.next_trigger = get_next_trigger(maskable_int_config, cpu.cycles);
      waiting_for_int_delivery = false;
    }

    // Periodically check for console escape (every 10000 instructions)
    // This allows ^E to work even in tight loops that don't do I/O
    if (instruction_count % 10000 == 0) {
      check_console_escape_async();

      // Check periodic disk flush (flushes if writes pending and 20s elapsed)
      emu.getHBIOS()->checkPeriodicFlush();
    }

    // Debug: trace PC every 10M instructions to see where stuck
    if (debug) {
      static bool dumped_loop = false;
      if (instruction_count > 0 && instruction_count % 10000000 == 0) {
        uint16_t loop_pc = cpu.regs.PC.get_pair16();
        fprintf(stderr, "[%lldM] PC=0x%04X A=0x%02X BC=0x%04X HL=0x%04X\n",
                instruction_count / 1000000,
                loop_pc,
                cpu.get_reg8(qkz80::reg_A),
                cpu.regs.BC.get_pair16(),
                cpu.regs.HL.get_pair16());
        // Dump code around the loop once
        if (!dumped_loop && loop_pc >= 0x4D00 && loop_pc < 0x4E00) {
          dumped_loop = true;
          banked_mem* bmem = dynamic_cast<banked_mem*>(&memory);
          uint8_t cur_bank = bmem ? bmem->get_current_bank() : 0;
          fprintf(stderr, "Current bank: 0x%02X\n", cur_bank);
          fprintf(stderr, "Code dump 0x4D50-0x4D90:\n");
          for (int i = 0; i < 64; i++) {
            fprintf(stderr, "%02X ", memory.fetch_mem((uint16_t)(0x4D50 + i)));
            if ((i + 1) % 16 == 0) fprintf(stderr, "\n");
          }
        }
      }
    }

    if (instruction_count >= max_instructions) {
      fprintf(stderr, "\nReached instruction limit at PC=0x%04X\n",
              cpu.regs.PC.get_pair16());
      break;
    }
  }

  // Write trace file if tracing was enabled
  if (!trace_file.empty()) {
    memory.write_trace_script(trace_file.c_str(), load_addr);
  }

  // Save NVRAM if it was initialized (either by --boot, SYSCONF, or loaded from
  // file).  A --boot given on the command line is NOT saved: it is an override
  // for this run.  It used to be written back, which made every automated run
  // that boots the emulator - a test, a script, a CI job - silently replace the
  // developer's persisted boot target with whatever that run happened to pass.
  // A setting the GUEST changed during the run is a different thing and is
  // still saved, which is why this compares rather than just testing the flag:
  // SYSCONF leaves NVRAM holding something other than what --boot put there.
  // A --boot from the config file is left alone; that file IS a persisted
  // choice, so writing it through is at worst redundant.
  if (emu.getHBIOS()->isNvramInitialized()) {
    std::string setting = emu.getHBIOS()->getNvramSetting();
    if (boot_from_cli && !boot_clear && setting == cli_boot_effective) {
      fprintf(stderr, "--boot=%s applied to this run only; %s is unchanged\n",
              boot_string.c_str(), nvram_path.c_str());
    } else if (save_nvram_setting(nvram_path, setting)) {
      fprintf(stderr, "Saved NVRAM setting '%s' to %s\n", setting.c_str(), nvram_path.c_str());
    }
  }

  return 0;
}
