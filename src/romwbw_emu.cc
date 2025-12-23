/*
 * RomWBW Emulator
 *
 * Emulates RomWBW with banked memory (512KB ROM + 512KB RAM).
 * HBIOS calls are handled by the emulator, allowing RomWBW to boot
 * CP/M, ZSDOS, and other operating systems.
 *
 * Console escape: Ctrl+E (configurable)
 */

// Version info
#define EMU_VERSION "2.0.0"
#define EMU_VERSION_DATE "2025-12-13"

#include "qkz80.h"
#include "romwbw_mem.h"
#include "hbios_dispatch.h"  // Shared HBIOS definitions
#include "hbios_cpu.h"       // Shared CPU with HBIOS port I/O
#include "emu_io.h"
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

// Global for signal handler to request stop
static volatile bool stop_requested = false;

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

// Deliver a maskable interrupt to the CPU
// Returns true if interrupt was delivered, false if blocked (interrupts disabled)
static bool deliver_maskable_interrupt(qkz80& cpu, const InterruptConfig& cfg) {
  // Check if interrupts are enabled (IFF1)
  if (cpu.regs.IFF1 == 0) {
    return false;  // Interrupts disabled
  }

  // Disable interrupts (like hardware would)
  cpu.regs.IFF1 = 0;
  cpu.regs.IFF2 = 0;

  // Push current PC
  cpu.push_word(cpu.regs.PC.get_pair16());

  // Jump to interrupt vector
  if (cfg.use_rst) {
    // RST n jumps to n * 8
    cpu.regs.PC.set_pair16(cfg.rst_num * 8);
  } else {
    // CALL addr
    cpu.regs.PC.set_pair16(cfg.call_addr);
  }

  return true;
}

// Deliver an NMI to the CPU
// NMI cannot be disabled, always jumps to 0x0066
static void deliver_nmi(qkz80& cpu) {
  // Copy IFF1 to IFF2 (so RETN can restore interrupt state)
  cpu.regs.IFF2 = cpu.regs.IFF1;
  // Disable interrupts
  cpu.regs.IFF1 = 0;

  // Push current PC
  cpu.push_word(cpu.regs.PC.get_pair16());

  // Jump to NMI vector
  cpu.regs.PC.set_pair16(0x0066);
}

// HBIOS function codes and result codes are now in hbios_dispatch.h

// Use banked_mem from romwbw_mem.h - provides both flat and banked memory modes
// For backward compatibility, alias it as cpm_mem
using cpm_mem = banked_mem;

// Terminal state management
static struct termios original_termios;
static bool termios_saved = false;

static void disable_raw_mode() {
  if (termios_saved) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    termios_saved = false;
  }
}

static void enable_raw_mode() {
  if (!isatty(STDIN_FILENO)) {
    return;
  }

  if (!termios_saved) {
    tcgetattr(STDIN_FILENO, &original_termios);
    termios_saved = true;
    atexit(disable_raw_mode);
  }

  struct termios raw = original_termios;
  raw.c_lflag &= ~(ICANON | ECHO | ISIG);
  raw.c_cc[VMIN] = 0;  // Non-blocking
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Track if stdin has reached EOF
static bool stdin_eof = false;
static int peek_char = -1;  // Peeked character, -1 if none

// Raw read from stdin - bypasses stdio buffering which causes issues with select()
// Returns -1 if no data, -2 if EOF, or the character (0-255)
static int raw_read_stdin() {
  unsigned char ch;
  ssize_t n = read(STDIN_FILENO, &ch, 1);
  if (n == 0) return -2;  // EOF
  if (n < 0) return -1;   // Error or would block
  return ch;
}

// Console mode escape character (default ^E like SIMH)
static char console_escape_char = 0x05;  // Ctrl+E
static bool console_mode_requested = false;

// Forward declaration
static bool check_ctrl_c_exit(int ch);

// ^C exit handling - press Ctrl+C 4 times quickly to exit
static int consecutive_ctrl_c = 0;
static const int CTRL_C_EXIT_COUNT = 4;

static bool check_ctrl_c_exit(int ch) {
  if (ch == 0x03) {
    consecutive_ctrl_c++;
    if (consecutive_ctrl_c >= CTRL_C_EXIT_COUNT) {
      fprintf(stderr, "\n[Exiting: %d consecutive ^C received]\n", CTRL_C_EXIT_COUNT);
      disable_raw_mode();
      exit(0);
    }
    return true;  // Was ^C
  }
  // Don't reset counter on other characters - just count total ^C presses
  return false;
}

// Check if escape character is available in stdin (non-blocking)
// This is called periodically from the main loop for tight loops that don't do I/O
// IMPORTANT: Only consume the character if it IS the escape char, otherwise leave it
// Also handles ^C for exit even when emulated program is in a tight loop
static bool check_console_escape_async() {
  if (!isatty(STDIN_FILENO)) return false;
  if (peek_char >= 0) {
    // Already have a peeked char - check if it's escape or ^C
    if (peek_char == console_escape_char) {
      peek_char = -1;  // Consume it
      console_mode_requested = true;
      return true;
    }
    if (peek_char == 0x03) {  // ^C
      
      peek_char = -1;  // Consume it
      check_ctrl_c_exit(0x03);
      return false;
    }
    return false;  // Has data but not escape - don't consume
  }

  fd_set readfds;
  struct timeval tv;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;  // Non-blocking

  int sel_result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
  if (sel_result > 0) {
    int ch = raw_read_stdin();
    if (ch == -2) {  // EOF
      stdin_eof = true;
      return false;
    }
    if (ch < 0) {  // Error or would block
      return false;
    }
    if (ch == console_escape_char) {
      console_mode_requested = true;
      return true;
    }
    if (ch == 0x03) {  // ^C
      
      check_ctrl_c_exit(0x03);
      // Don't save ^C for emulated program when counting for exit
      return false;
    }
    // Not escape char - save it for the emulated program to read
    peek_char = ch;
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
      symbols[sym] = addr;
      addr_to_symbol[addr] = sym;
    }
    // Try "SYMBOL = ADDR" format
    else if (sscanf(line, "%63s = %x", sym, &addr) == 2 ||
             sscanf(line, "%63s =%x", sym, &addr) == 2 ||
             sscanf(line, "%63s= %x", sym, &addr) == 2 ||
             sscanf(line, "%63s=%x", sym, &addr) == 2) {
      symbols[sym] = addr;
      addr_to_symbol[addr] = sym;
    }
    // Try "SYMBOL EQU ADDR" format (common in assembler listings)
    else if (sscanf(line, "%63s EQU %x", sym, &addr) == 2 ||
             sscanf(line, "%63s equ %x", sym, &addr) == 2) {
      symbols[sym] = addr;
      addr_to_symbol[addr] = sym;
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
  disable_raw_mode();
  fprintf(stderr, "sim> ");
  fflush(stderr);

  if (!fgets(buf, buflen, stdin)) {
    enable_raw_mode();
    return false;
  }

  // Strip newline
  size_t len = strlen(buf);
  if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';

  enable_raw_mode();
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

// Handle console mode - returns action to take
static ConsoleResult handle_console_mode(qkz80* cpu, cpm_mem* memory) {
  char line[256];
  char cmd[64];
  char arg1[64], arg2[64], arg3[64];

  fprintf(stderr, "\n[Console mode - ^E to enter, 'help' for commands]\n");
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
    for (char* p = cmd; *p; p++) *p = tolower(*p);

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
      breakpoints.insert(addr);
      fprintf(stderr, "  Breakpoint set at %s\n", format_address(addr).c_str());
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
      if (breakpoints.erase(addr)) {
        fprintf(stderr, "  Breakpoint cleared at %s\n", format_address(addr).c_str());
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
      cpu->regs.PC.set_pair16(addr);
      fprintf(stderr, "  PC set to %s\n", format_address(addr).c_str());
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
  cpm_mem* memory;
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

  // RAM bank initialization tracking (bitmap for banks 0x80-0x8F)
  uint16_t initialized_ram_banks = 0;

public:
  AltairEmulator(hbios_cpu* acpu, cpm_mem* amem, bool adebug = false)
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

    FILE* f = fopen(romldr_path.c_str(), "rb");
    if (!f) {
      fprintf(stderr, "[ROMLDR] Cannot open %s\n", romldr_path.c_str());
      return false;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Load entire ROM file (up to 512KB)
    uint8_t* rom_data = bmem->get_rom();
    if (!rom_data) {
      fclose(f);
      return false;
    }

    // Save bank 0 (our emu_hbios) before loading
    uint8_t bank0_save[32768];
    memcpy(bank0_save, rom_data, 32768);

    // Load full ROM
    size_t bytes = fread(rom_data, 1, file_size, f);
    fclose(f);

    // Restore bank 0 with our emu_hbios code
    memcpy(rom_data, bank0_save, 32768);

    fprintf(stderr, "[ROMLDR] Loaded %zu bytes from %s (banks 1-15)\n", bytes, romldr_path.c_str());
    fprintf(stderr, "[ROMLDR] Bank 0 preserved (emu_hbios)\n");

    // Verify RST 08 vector in bank 1
    uint8_t* bank1 = rom_data + 32768;
    fprintf(stderr, "[ROMLDR] Bank 1 RST 08 vector (0x0008): %02X %02X %02X\n",
            bank1[0x08], bank1[0x09], bank1[0x0A]);

    romldr_loaded = true;
    return true;
  }


  void set_sense_switches(uint8_t val) {
    sense_switches = val;
    if (debug) fprintf(stderr, "Sense switches set to: 0x%02X\n", sense_switches);
  }

  // Initialize a RAM bank if it hasn't been initialized yet
  // Copies page zero and HCB from ROM bank 0 to ensure RST vectors and system config are present
  void initialize_ram_bank_if_needed(uint8_t bank) {
    banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
    if (!bmem) return;

    // Only initialize RAM banks 0x80-0x8F
    if (!(bank & 0x80) || (bank & 0x70)) return;

    uint8_t bank_idx = bank & 0x0F;
    if (initialized_ram_banks & (1 << bank_idx)) return;  // Already initialized

    if (debug) {
      fprintf(stderr, "[BANK INIT] Initializing RAM bank 0x%02X with page zero and HCB\n", bank);
    }

    // Copy page zero (0x0000-0x0100) - contains RST vectors
    for (uint16_t addr = 0x0000; addr < 0x0100; addr++) {
      uint8_t byte = bmem->read_bank(0x00, addr);
      bmem->write_bank(bank, addr, byte);
    }
    // Copy HCB (0x0100-0x0200) - system configuration
    for (uint16_t addr = 0x0100; addr < 0x0200; addr++) {
      uint8_t byte = bmem->read_bank(0x00, addr);
      bmem->write_bank(bank, addr, byte);
    }
    // Patch APITYPE to HBIOS (0x00) instead of UNA (0xFF)
    bmem->write_bank(bank, 0x0112, 0x00);

    initialized_ram_banks |= (1 << bank_idx);
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
    // Check for console escape first
    if (emu_console_check_escape(console_escape_char)) {
      console_mode_requested = true;
    }
  }
};

// Disk size constants
static constexpr size_t HD1K_SINGLE_SIZE = 8388608;      // 8 MB exactly
static constexpr size_t HD1K_PREFIX_SIZE = 1048576;      // 1 MB prefix
static constexpr size_t HD512_SINGLE_SIZE = 8519680;     // 8.32 MB

// Partition types
static constexpr uint8_t PART_TYPE_ROMWBW = 0x2E;  // RomWBW hd1k partition
static constexpr uint8_t PART_TYPE_FAT16 = 0x06;   // FAT16 (incompatible)
static constexpr uint8_t PART_TYPE_FAT32 = 0x0B;   // FAT32 (incompatible)

// Check if MBR has valid RomWBW partition or none at all
// Returns warning message or nullptr if OK
static const char* check_disk_mbr(const char* path, size_t size) {
  // Only check for 8MB single-slice images - these are the problematic ones
  if (size != HD1K_SINGLE_SIZE) {
    return nullptr;  // Only check single-slice images
  }

  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;

  uint8_t mbr[512];
  size_t read = fread(mbr, 1, 512, f);
  fclose(f);

  if (read != 512) return nullptr;

  // Check for MBR signature
  if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
    return nullptr;  // No MBR - probably raw hd1k slice, OK
  }

  // Has MBR signature - check partition types
  bool has_romwbw_partition = false;
  bool has_fat_partition = false;

  for (int p = 0; p < 4; p++) {
    int offset = 0x1BE + (p * 16);
    uint8_t ptype = mbr[offset + 4];
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
  if (mbr[0] == 0x18 || mbr[0] == 0xC3) {
    return nullptr;  // Looks like Z80 boot code - probably just has stale MBR signature
  }

  return "WARNING: disk has MBR but no RomWBW partition (0x2E) - format may be invalid";
}

// Validate disk image file - returns error message or nullptr if valid
static const char* validate_disk_image(const char* path, size_t* out_size = nullptr) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return "file does not exist";
  }

  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fclose(f);

  if (out_size) *out_size = size;

  // Check for valid hd1k sizes
  if (size == HD1K_SINGLE_SIZE) {
    // Check MBR for potential issues with single-slice images
    const char* mbr_warning = check_disk_mbr(path, size);
    if (mbr_warning) {
      fprintf(stderr, "[DISK] %s: %s\n", path, mbr_warning);
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

void print_usage(const char* prog) {
  fprintf(stderr, "RomWBW Emulator v%s (%s)\n", EMU_VERSION, EMU_VERSION_DATE);
  fprintf(stderr, "Usage: %s --romwbw=<rom.rom> [options]\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --version, -v     Show version information\n");
  fprintf(stderr, "  --romwbw=FILE     Enable RomWBW mode with ROM file (512KB ROM+RAM, Z80)\n");
  fprintf(stderr, "  --strict-io       Halt on unexpected I/O ports (for debugging)\n");
  fprintf(stderr, "  --debug           Enable debug output\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Disk options:\n");
  fprintf(stderr, "  --disk0=FILE[:N]  Attach disk image to slot 0 (default: 4 slices -> C:-F:)\n");
  fprintf(stderr, "  --disk1=FILE[:N]  Attach disk image to slot 1 (default: 4 slices -> G:-J:)\n");
  fprintf(stderr, "    N = number of slices (1-8), controls how many drive letters are used\n");
  fprintf(stderr, "    Example: --disk0=disk.img:1 uses only 1 slice (C: only)\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  --create-disk=FILE:FORMAT  Create a new formatted disk image and exit\n");
  fprintf(stderr, "    FORMAT: 8mb   - 8MB single slice hd1k (1024 dir entries)\n");
  fprintf(stderr, "            51mb  - 51MB combo disk (1MB MBR + 6x8MB slices)\n");
  fprintf(stderr, "    Example: --create-disk=mydisk.img:8mb\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  Supported disk formats (auto-detected):\n");
  fprintf(stderr, "    hd1k  - Modern RomWBW format, 8MB per slice, 1024 dir entries\n");
  fprintf(stderr, "    hd512 - Classic format, 8.32MB per slice, 512 dir entries\n");
  fprintf(stderr, "  Disk files must exist and have valid sizes (8MB or 8.32MB per slice).\n");
  fprintf(stderr, "  Combo disks with 1MB MBR prefix + multiple slices are supported.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Other options:\n");
  fprintf(stderr, "  --escape=CHAR     Console escape char (default ^E)\n");
  fprintf(stderr, "  --trace=FILE      Write execution trace to FILE\n");
  fprintf(stderr, "  --symbols=FILE    Load symbol table from FILE (.sym)\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Console mode:\n");
  fprintf(stderr, "  Press the escape char (default Ctrl+E) to enter console mode.\n");
  fprintf(stderr, "  Type 'help' in console mode for available commands.\n");
  fprintf(stderr, "  Use 'quit' to exit.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Examples:\n");
  fprintf(stderr, "  %s --romwbw=roms/emu_avw.rom\n", prog);
  fprintf(stderr, "  %s --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_combo.img\n", prog);
  fprintf(stderr, "  %s --romwbw=roms/emu_avw.rom --disk0=disks/hd1k_infocom.img:1\n", prog);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  // Parse arguments
  const char* binary = nullptr;
  uint16_t load_addr = 0x0000;
  uint16_t start_addr = 0x0000;
  bool start_addr_set = false;
  bool debug = false;
  bool strict_io_mode = false;
  int sense = -1;
  std::string hbios_disks[16];  // For RomWBW disk images (HBIOS dispatch)
  int hbios_disk_slices[16] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};  // Max slices per disk (default 4)
  std::string trace_file;
  std::string symbols_file;
  std::string romldr_path;  // RomWBW romldr boot menu

  // ROM application definitions: key=name:path
  struct RomAppDef {
    char key;
    std::string name;
    std::string path;
  };
  std::vector<RomAppDef> rom_app_defs;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
      fprintf(stderr, "RomWBW Emulator v%s (%s)\n", EMU_VERSION, EMU_VERSION_DATE);
      fprintf(stderr, "Emulates RomWBW with HBIOS, boots CP/M/ZSDOS from ROM disk\n");
      return 0;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--debug") == 0) {
      debug = true;
    } else if (strncmp(argv[i], "--romwbw=", 9) == 0) {
      binary = argv[i] + 9;
    } else if (strcmp(argv[i], "--strict-io") == 0) {
      strict_io_mode = true;
    } else if (strncmp(argv[i], "--create-disk=", 14) == 0) {
      // Parse --create-disk=FILE:FORMAT
      // FORMAT is 8mb or 51mb
      const char* opt = argv[i] + 14;
      const char* colon = strrchr(opt, ':');
      if (!colon || colon == opt || colon[1] == '\0') {
        fprintf(stderr, "Error: --create-disk requires FILE:FORMAT (e.g., mydisk.img:8mb)\n");
        return 1;
      }
      std::string path(opt, colon - opt);
      std::string format_str(colon + 1);

      emu_disk_format format;
      if (format_str == "8mb" || format_str == "8MB") {
        format = EMU_DISK_HD1K_SINGLE;
      } else if (format_str == "51mb" || format_str == "51MB") {
        format = EMU_DISK_HD1K_COMBO;
      } else {
        fprintf(stderr, "Error: Unknown disk format '%s' (use 8mb or 51mb)\n", format_str.c_str());
        return 1;
      }

      // Check if file already exists
      if (emu_file_exists(path)) {
        fprintf(stderr, "Error: File '%s' already exists. Remove it first or use a different name.\n", path.c_str());
        return 1;
      }

      // Create the disk
      size_t expected_size = (format == EMU_DISK_HD1K_SINGLE) ? EMU_HD1K_SINGLE_SIZE : EMU_HD1K_COMBO_SIZE;
      fprintf(stderr, "Creating %s disk image: %s (%zu bytes)...\n", format_str.c_str(), path.c_str(), expected_size);

      if (!emu_disk_create(path, format)) {
        fprintf(stderr, "Error: Failed to create disk image '%s'\n", path.c_str());
        return 1;
      }

      fprintf(stderr, "Successfully created %s\n", path.c_str());
      fprintf(stderr, "You can now use it with: --disk0=%s\n", path.c_str());
      return 0;  // Exit after creating disk
    } else if (strncmp(argv[i], "--sense=", 8) == 0) {
      sense = strtol(argv[i] + 8, nullptr, 0);
    } else if (strncmp(argv[i], "--load=", 7) == 0) {
      load_addr = strtol(argv[i] + 7, nullptr, 0);
    } else if (strncmp(argv[i], "--start=", 8) == 0) {
      start_addr = strtol(argv[i] + 8, nullptr, 0);
      start_addr_set = true;
    } else if (strncmp(argv[i], "--disk", 6) == 0) {
      // Parse --disk0=file[:slices], --disk1=file[:slices] (preferred form)
      // slices is optional, 1-8, defaults to 4
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
        // Check for :N slice count suffix (must be at end after the file path)
        std::string path_str(path_start);
        int slice_count = 4;  // Default
        size_t colon_pos = path_str.rfind(':');
        if (colon_pos != std::string::npos && colon_pos > 0) {
          // Check if what's after the colon is a single digit (slice count)
          std::string suffix = path_str.substr(colon_pos + 1);
          if (suffix.length() == 1 && isdigit(suffix[0])) {
            int n = suffix[0] - '0';
            if (n >= 1 && n <= 8) {
              slice_count = n;
              path_str = path_str.substr(0, colon_pos);
            }
          }
        }
        // Validate disk image exists and has valid size
        size_t disk_size = 0;
        const char* err = validate_disk_image(path_str.c_str(), &disk_size);
        if (err) {
          fprintf(stderr, "Error: --disk%d=%s: %s\n", unit, path_str.c_str(), err);
          return 1;
        }
        hbios_disks[unit] = path_str;
        hbios_disk_slices[unit] = slice_count;
        fprintf(stderr, "[DISK] Validated disk%d: %s (%zu bytes, %d slices)\n", unit, path_str.c_str(), disk_size, slice_count);
      } else {
        fprintf(stderr, "Invalid --disk option: %s (use --disk0=file[:slices] or --disk1=file[:slices])\n", argv[i]);
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
        def.key = toupper(opt[0]);
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
        def.key = toupper(opt[0]);
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
    } else if (strncmp(argv[i], "--trace=", 8) == 0) {
      trace_file = argv[i] + 8;
    } else if (strncmp(argv[i], "--symbols=", 10) == 0) {
      symbols_file = argv[i] + 10;
    } else if (strncmp(argv[i], "--escape=", 9) == 0) {
      // Parse escape character: ^X for control chars, or literal char
      const char* esc = argv[i] + 9;
      if (esc[0] == '^' && esc[1] != '\0') {
        // ^X format - convert to control character
        char c = toupper(esc[1]);
        if (c >= '@' && c <= '_') {
          console_escape_char = c - '@';
        } else {
          fprintf(stderr, "Invalid escape char: %s (use ^A through ^_)\n", esc);
          return 1;
        }
      } else if (esc[0] != '\0') {
        // Literal character
        console_escape_char = esc[0];
      }
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

  if (!binary) {
    fprintf(stderr, "Error: No binary file specified\n");
    return 1;
  }

  // Set defaults based on mode
  // RomWBW starts at address 0x0000 in ROM bank 0
  if (!start_addr_set) start_addr = 0x0000;

  // Create memory and CPU
  cpm_mem memory;
  hbios_cpu cpu(&memory, nullptr);  // delegate set below

  // Set Z80 mode and enable banking
  cpu.set_cpu_mode(qkz80::MODE_Z80);
  fprintf(stderr, "CPU mode: Z80\n");
  memory.enable_banking();
  memory.set_debug(debug);
  fprintf(stderr, "RomWBW mode: 512KB ROM + 512KB RAM, bank switching enabled\n");

  // Create emulator (sets cpu.delegate in constructor)
  AltairEmulator emu(&cpu, &memory, debug);
  emu.set_strict_io_mode(strict_io_mode);
  emu.getHBIOS()->setDebug(debug);  // Enable HBIOS debug output

  // Set up HBIOS disk images
  // NOTE: Memory disks are initialized later, after ROM is loaded
  // Attach any file-backed hard disk images (HBIOS dispatch protocol)
  for (int i = 0; i < 16; i++) {
    if (!hbios_disks[i].empty()) {
      if (!emu.getHBIOS()->loadDiskFromFile(i, hbios_disks[i])) {
        fprintf(stderr, "Warning: Could not attach disk %d: %s\n", i, hbios_disks[i].c_str());
      } else {
        // Set the slice count for this disk
        emu.getHBIOS()->setDiskSliceCount(i, hbios_disk_slices[i]);
      }
    }
  }

  // Register ROM applications with HBIOSDispatch
  for (const auto& def : rom_app_defs) {
    emu.getHBIOS()->addRomApp(def.name, def.path, def.key);
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

  // Print console escape char info
  if (console_escape_char < 0x20) {
    fprintf(stderr, "Console escape: ^%c (Ctrl+%c)\n",
            console_escape_char + '@', console_escape_char + '@');
  } else {
    fprintf(stderr, "Console escape: '%c'\n", console_escape_char);
  }

  // Enable raw terminal mode
  enable_raw_mode();

  // Load binary file
  FILE* fp = fopen(binary, "rb");
  if (!fp) {
    fprintf(stderr, "Cannot open %s: %s\n", binary, strerror(errno));
    return 1;
  }

  fseek(fp, 0, SEEK_END);
  size_t file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  {
    // Load ROM image into banked memory
    if (!memory.load_rom_file(binary)) {
      fprintf(stderr, "Failed to load ROM from %s\n", binary);
      fclose(fp);
      return 1;
    }
    fprintf(stderr, "Loaded %zu bytes ROM from %s\n", file_size, binary);
    fprintf(stderr, "Starting execution at 0x%04X in ROM bank 0\n", start_addr);
    fclose(fp);

    // If romldr path specified, load full RomWBW ROM (preserving bank 0)
    if (!romldr_path.empty()) {
      emu.load_romldr_rom();
    }

    // Copy HCB from ROM bank 0 to RAM bank 0x80 (HBIOS working bank)
    // The HCB at 0x100-0x1FF contains configuration that romldr reads via PEEK
    uint8_t* rom = memory.get_rom();
    uint8_t* ram = memory.get_ram();
    if (rom && ram) {
      // Patch ROM's HCB APITYPE field BEFORE copying to RAM
      // Set to 0x00 (HBIOS) instead of 0xFF (UNA) so REBOOT and other
      // utilities recognize this as an HBIOS system, not UNA
      // This ensures all subsequent copies from ROM will have correct value
      rom[0x0112] = 0x00;  // CB_APITYPE = HBIOS in ROM

      // Copy first 512 bytes (includes HCB at 0x100)
      memcpy(ram, rom, 512);

      fprintf(stderr, "Copied HCB from ROM bank 0 to RAM bank 0x80\n");

      // Set up HBIOS ident pointer at 0xFFFC in common area (bank 0x8F)
      // REBOOT and other utilities check for this signature
      // Format: pointer at 0xFFFC -> ident block with 'W', ~'W', ver_maj, ver_min
      // Common area 0x8000-0xFFFF maps to bank 0x8F (index 15 = 0x0F)
      // Physical offset in RAM = bank_index * 32KB + (addr - 0x8000)
      const uint32_t COMMON_BASE = 0x0F * 32768;  // Bank 0x8F = index 15

      // Create ident block at 0xFF00 in common area
      uint32_t ident_phys = COMMON_BASE + (0xFF00 - 0x8000);  // 0x7F00 offset
      ram[ident_phys + 0] = 'W';       // Signature byte 1
      ram[ident_phys + 1] = ~'W';      // Signature byte 2 (0xA8)
      ram[ident_phys + 2] = 0x35;      // Combined version: (major << 4) | minor = (3 << 4) | 5

      // Also create ident block at 0xFE00 (some REBOOT versions may look there)
      uint32_t ident_phys2 = COMMON_BASE + (0xFE00 - 0x8000);
      ram[ident_phys2 + 0] = 'W';
      ram[ident_phys2 + 1] = ~'W';
      ram[ident_phys2 + 2] = 0x35;

      // Store pointer to ident block at 0xFFFC (little-endian)
      uint32_t ptr_phys = COMMON_BASE + (0xFFFC - 0x8000);  // 0x7FFC offset
      ram[ptr_phys + 0] = 0x00;        // Low byte of 0xFF00
      ram[ptr_phys + 1] = 0xFF;        // High byte of 0xFF00

      // Populate disk unit table in HCB for boot loader device inventory
      // The disk unit table is at HCB+0x60 (address 0x160), 16 entries of 4 bytes each
      // Format per entry:
      //   Byte 0: Device type (DIODEV_*): 0x00=MD, 0x09=HDSK, 0x03=IDE, 0x06=SD, 0xFF=empty
      //   Byte 1: Unit number within device type
      //   Byte 2: Mode/attributes (0x80 = removable, etc.)
      //   Byte 3: Reserved/LU
      const uint16_t DISKUT_BASE = 0x160;  // HCB+0x60

      // First, mark all entries as empty (0xFF)
      for (int i = 0; i < 16; i++) {
        rom[DISKUT_BASE + i * 4 + 0] = 0xFF;
        rom[DISKUT_BASE + i * 4 + 1] = 0xFF;
        rom[DISKUT_BASE + i * 4 + 2] = 0xFF;
        rom[DISKUT_BASE + i * 4 + 3] = 0xFF;
      }

      int disk_idx = 0;

      // Add memory disks (MD0=RAM, MD1=ROM) - read from HCB config
      uint8_t ramd_banks = rom[0x1DD];  // CB_RAMD_BNKS
      uint8_t romd_banks = rom[0x1DF];  // CB_ROMD_BNKS
      if (ramd_banks > 0 && disk_idx < 16) {
        rom[DISKUT_BASE + disk_idx * 4 + 0] = 0x00;  // DIODEV_MD
        rom[DISKUT_BASE + disk_idx * 4 + 1] = 0x00;  // Unit 0 (RAM disk)
        rom[DISKUT_BASE + disk_idx * 4 + 2] = 0x00;  // No special attributes
        rom[DISKUT_BASE + disk_idx * 4 + 3] = 0x00;
        disk_idx++;
      }
      if (romd_banks > 0 && disk_idx < 16) {
        rom[DISKUT_BASE + disk_idx * 4 + 0] = 0x00;  // DIODEV_MD
        rom[DISKUT_BASE + disk_idx * 4 + 1] = 0x01;  // Unit 1 (ROM disk)
        rom[DISKUT_BASE + disk_idx * 4 + 2] = 0x00;  // No special attributes
        rom[DISKUT_BASE + disk_idx * 4 + 3] = 0x00;
        disk_idx++;
      }

      // Add hard disks from hbios_disks array
      for (int i = 0; i < 16 && disk_idx < 16; i++) {
        if (!hbios_disks[i].empty()) {
          rom[DISKUT_BASE + disk_idx * 4 + 0] = 0x09;  // DIODEV_HDSK
          rom[DISKUT_BASE + disk_idx * 4 + 1] = (uint8_t)i;  // HDSK unit number
          rom[DISKUT_BASE + disk_idx * 4 + 2] = 0x00;  // No special attributes
          rom[DISKUT_BASE + disk_idx * 4 + 3] = 0x00;
          disk_idx++;
        }
      }

      // Note: CB_DEVCNT will be updated after drive map is populated
      // to reflect the number of logical drives, not physical devices

      // Debug: show what we wrote to the disk table
      fprintf(stderr, "[HCB] Writing disk unit table:\n");
      for (int i = 0; i < disk_idx; i++) {
        fprintf(stderr, "  [%d] %02X %02X %02X %02X\n", i,
                rom[DISKUT_BASE + i * 4 + 0],
                rom[DISKUT_BASE + i * 4 + 1],
                rom[DISKUT_BASE + i * 4 + 2],
                rom[DISKUT_BASE + i * 4 + 3]);
      }

      // Populate drive map at HCB+0x20 (0x120)
      // Format: each byte = (slice << 4) | unit
      // Drive letters A-P map to bytes 0x120-0x12F
      // Value 0xFF = no drive assigned
      const uint16_t DRVMAP_BASE = 0x120;  // HCB+0x20
      int drive_letter = 0;  // 0=A, 1=B, etc.

      // First, mark all drive map entries as unused (0xFF)
      for (int i = 0; i < 16; i++) {
        rom[DRVMAP_BASE + i] = 0xFF;
      }

      // Then assign memory disks
      // A: = MD0 (RAM disk) if enabled
      if (ramd_banks > 0 && drive_letter < 16) {
        rom[DRVMAP_BASE + drive_letter] = 0x00;  // Unit 0, slice 0
        drive_letter++;
      }
      // B: = MD1 (ROM disk) if enabled
      if (romd_banks > 0 && drive_letter < 16) {
        rom[DRVMAP_BASE + drive_letter] = 0x01;  // Unit 1, slice 0
        drive_letter++;
      }

      // Then assign hard disk slices
      // Assign drive letters (slices) per hard disk, respecting per-disk slice limits
      for (int hd = 0; hd < 16 && drive_letter < 16; hd++) {
        if (!hbios_disks[hd].empty()) {
          // Unit number: HD0 = unit 2, HD1 = unit 3, etc.
          int unit = hd + 2;

          // Use per-disk slice count (configured via --disk0=file:N syntax, default 4)
          int num_slices = hbios_disk_slices[hd];

          // Assign each slice to a drive letter
          for (int slice = 0; slice < num_slices && drive_letter < 16; slice++) {
            rom[DRVMAP_BASE + drive_letter] = ((slice & 0x0F) << 4) | (unit & 0x0F);
            drive_letter++;
          }
        }
      }

      fprintf(stderr, "[HCB] Drive map: assigned %d drive letters\n", drive_letter);

      // Update device count in HCB to match number of logical drives
      // The CBIOS uses this to determine how many drives to configure
      rom[0x0C + 0x100] = (uint8_t)drive_letter;  // CB_DEVCNT at HCB+0x0C

      // Re-copy the updated HCB to RAM
      memcpy(ram, rom, 512);
      fprintf(stderr, "[HCB] Populated disk unit table with %d devices, %d logical drives\n",
              disk_idx, drive_letter);
    }

    // NOW initialize memory disks from HCB (after ROM is loaded)
    emu.getHBIOS()->initMemoryDisks();
  }

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
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

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

    // Flush output from HBIOSDispatch to stdout
    emu.flush_output();

    // Check if strict I/O mode halted us during port operations
    if (emu.is_halted()) {
      fprintf(stderr, "\nEmulator halted (strict I/O mode)\n");
      break;
    }

    // Check if OUT handler wants to redirect PC (e.g., for romldr bank switch)
    if (emu.has_next_pc()) {
      cpu.regs.PC.set_pair16(emu.get_next_pc());
    }

    // Check for scheduled interrupts
    if (nmi_config.enabled && cpu.cycles >= nmi_config.next_trigger) {
      deliver_nmi(cpu);
      nmi_config.next_trigger = get_next_trigger(nmi_config, cpu.cycles);
    }
    if (maskable_int_config.enabled && cpu.cycles >= maskable_int_config.next_trigger) {
      if (deliver_maskable_interrupt(cpu, maskable_int_config)) {
        // Interrupt delivered, schedule next one
        maskable_int_config.next_trigger = get_next_trigger(maskable_int_config, cpu.cycles);
      }
      // If interrupts disabled, keep trying until delivered
    }

    // Periodically check for console escape (every 10000 instructions)
    // This allows ^E to work even in tight loops that don't do I/O
    if (instruction_count % 10000 == 0) {
      check_console_escape_async();
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
            fprintf(stderr, "%02X ", memory.fetch_mem(0x4D50 + i));
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

  return 0;
}
