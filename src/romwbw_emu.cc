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
#include "emu_io.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// Forward declaration
class AltairEmulator;

// Extended Z80 CPU with port I/O support
// Overrides port_in/port_out virtual functions to route I/O through the emulator
class z80_with_io : public qkz80 {
public:
  AltairEmulator* emu;  // Set after emulator is created

  z80_with_io(qkz80_cpu_mem* memory) : qkz80(memory), emu(nullptr) {}

  // Override port I/O - routes through emulator's handle_in/handle_out
  void port_out(qkz80_uint8 port, qkz80_uint8 value) override;
  qkz80_uint8 port_in(qkz80_uint8 port) override;

  // Override halt and unimplemented opcode handlers
  void halt(void) override;
  void unimplemented_opcode(qkz80_uint8 opcode, qkz80_uint16 pc) override;
};

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

// Normalize line ending: LF -> CR for CP/M compatibility
static inline uint8_t normalize_eol(int ch) {
  return (ch == '\n') ? '\r' : ch;
}

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

// RomWBW HBIOS constants
constexpr uint16_t HBIOS_BASE = 0xFE00;      // Fake HBIOS entry point
constexpr uint16_t HBIOS_PROXY = 0xFFE0;     // Real HBIOS proxy location (if present)

// HBIOS function codes and result codes are now in hbios_dispatch.h
// Additional result codes used by CLI only:
constexpr uint8_t HBR_NOTREADY = 0xFE;  // Device not ready
constexpr uint8_t HBR_WRTPROT  = 0xFD;  // Write protected

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

// Check if input is available (not just EOF) by peeking
static bool stdin_has_data() {
  if (stdin_eof) return false;  // Already at EOF
  if (peek_char >= 0) return true;  // We have a peeked char

  // Check if select says readable
  fd_set readfds;
  struct timeval tv;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  int sel_result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
  if (sel_result <= 0) {
    return false;  // Nothing waiting
  }

  // Select says readable - use raw read to avoid stdio buffering issues
  int ch = raw_read_stdin();
  if (ch == -2) {  // EOF
    stdin_eof = true;
    return false;
  }
  if (ch < 0) {  // Error or would block
    return false;
  }

  // Check for escape character - intercept it here before emulated program sees it
  if (ch == console_escape_char && isatty(STDIN_FILENO)) {
    console_mode_requested = true;
    return false;  // Don't report this as available input
  }

  // Don't count ^C here - only count when actually consumed by a read
  // This prevents ^C from being triggered during boot menu console detection
  peek_char = ch;  // Save for later read
  return true;
}

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

// HBIOS bank copy state (shared between SETCPY and BNKCPY handlers)
static uint8_t g_bnkcpy_src_bank = 0x8E;
static uint8_t g_bnkcpy_dst_bank = 0x8E;
static uint16_t g_bnkcpy_count = 0;

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

class AltairEmulator {
private:
  qkz80* cpu;
  cpm_mem* memory;
  bool debug;
  bool romwbw_mode;
  bool strict_io_mode;  // Halt on unexpected I/O ports
  bool halted;          // Set when halted due to unexpected I/O

  // I/O port tracking
  std::map<uint8_t, int> port_in_counts;
  std::map<uint8_t, int> port_out_counts;
  std::set<uint8_t> unknown_ports;

  // Serial port state (88-2SIO)
  uint8_t sio_status_a;
  uint8_t sio_status_b;
  int input_char;  // Buffered input character (-1 = none)

  // Sense switches
  uint8_t sense_switches;

  // Memory configuration
  uint16_t ram_top;

  // Debug: trace instructions after CIOIN
  int cioin_trace_count = 0;

  // HBIOS disk state
  struct HBDiskState {
    uint32_t current_lba;  // Current LBA position
    FILE* image_file;      // Disk image file handle
    std::string image_path; // Path to disk image
    bool is_open;

    // Partition/slice info (detected from MBR)
    bool partition_probed;     // True if we've already parsed MBR
    uint32_t partition_base_lba;  // Starting LBA of RomWBW partition (2048 for hd1k, 0 for hd512)
    uint32_t slice_size;       // Sectors per slice (16384 for hd1k, 16640 for hd512)
    bool is_hd1k;              // True if hd1k format (use MID_HDNEW), false for hd512 (MID_HD)
    int max_slices;            // Max slices to expose (configurable, default 4)

    HBDiskState() : current_lba(0), image_file(nullptr), is_open(false),
                    partition_probed(false), partition_base_lba(0), slice_size(16640), is_hd1k(false), max_slices(4) {}

    ~HBDiskState() {
      if (image_file) fclose(image_file);
    }
  };

  HBDiskState hb_disks[16];  // Up to 16 HBIOS disk units (for file-backed HD)
  static constexpr size_t HBIOS_SECTOR_SIZE = 512;  // Standard sector size for HBIOS

  // Memory Disk (MD) state - MD0=RAM disk, MD1=ROM disk
  // These use bank memory directly, not file-backed images
  struct MemDiskState {
    uint32_t current_lba;     // Current LBA position
    uint8_t start_bank;       // First bank of this disk
    uint8_t num_banks;        // Number of 32KB banks
    bool is_rom;              // True if ROM disk (read-only)
    bool is_enabled;          // True if this MD unit exists

    MemDiskState() : current_lba(0), start_bank(0), num_banks(0), is_rom(false), is_enabled(false) {}

    // Calculate total sectors (512 bytes each)
    uint32_t total_sectors() const { return (uint32_t)num_banks * 32768 / 512; }
  };

  MemDiskState md_disks[2];  // MD0=RAM disk, MD1=ROM disk
  static constexpr uint8_t MD_UNIT_OFFSET = 0x80;  // MD units start at 0x80 in device numbering

  // ROM Application state (OS images like CP/M, ZSDOS that can be booted from "ROM")
  struct RomAppState {
    std::string name;         // Display name (e.g., "CP/M 2.2", "ZSDOS")
    std::string sys_path;     // Path to .sys file
    char key;                 // Boot key (e.g., 'C' for CP/M, 'Z' for ZSDOS)
    bool is_loaded;

    RomAppState() : key(0), is_loaded(false) {}
  };

  static constexpr int MAX_ROM_APPS = 16;
  RomAppState rom_apps[MAX_ROM_APPS];
  int num_rom_apps = 0;

  // Skip synthetic RET in handle_hbios_call (for I/O port dispatch via 0xEF)
  bool skip_synthetic_ret = false;

  // RomWBW romldr support - load real boot menu from RomWBW ROM
  std::string romldr_path;             // Path to romldr.bin or .rom file
  bool romldr_from_rom = false;        // True if loading from bank 1 of .rom file
  bool romldr_loaded = false;          // True if romldr was loaded successfully
  uint16_t next_pc = 0;                // Jump target for OUT redirect
  bool next_pc_valid = false;          // True if next_pc should be used

  // SIMH RTC state (port 0xFE)
  uint8_t simh_rtc_cmd = 0;        // Current RTC command
  uint8_t simh_rtc_buf[6];         // Time buffer: YY MM DD HH MM SS (BCD)
  int simh_rtc_idx = 0;            // Index into buffer for reads

  // SIMH HDSK state (port 0xFD)
  // Protocol: OTIR sends 7 bytes: CMD, DRV, SEC, TRK_LO, TRK_HI, DMA_LO, DMA_HI
  // After 7 bytes, execute command. IN returns result code.
  uint8_t hdsk_parm[7];            // Parameter block being received
  int hdsk_parm_idx = 0;           // Index into parameter block
  uint8_t hdsk_result = 0;         // Last command result (0=success)
  FILE* hdsk_files[8] = {nullptr}; // Up to 8 HDSK disk images
  std::string hdsk_paths[8];       // Paths to HDSK disk images

  // Host file transfer state (HBIOS extension 0xE1-0xE5)
  FILE* host_read_file = nullptr;   // Currently open host file for reading
  FILE* host_write_file = nullptr;  // Currently open host file for writing

  // Boot info - saved during SYSBOOT, returned by SYSGET_BOOTINFO
  int saved_boot_unit = 0;   // Boot disk unit
  int saved_boot_slice = 0;  // Boot slice

public:
  AltairEmulator(qkz80* acpu, cpm_mem* amem, bool adebug = false, bool aromwbw = false)
    : cpu(acpu), memory(amem), debug(adebug), romwbw_mode(aromwbw),
      strict_io_mode(false), halted(false),
      sio_status_a(0x02), sio_status_b(0x02),
      input_char(-1),
      sense_switches(0x00),
      ram_top(0xFFFF) {
  }

  void set_strict_io_mode(bool mode) { strict_io_mode = mode; }
  bool is_strict_io_mode() const { return strict_io_mode; }
  bool is_halted() const { return halted; }
  void set_halted(bool h) { halted = h; }

  void set_romwbw_mode(bool mode) { romwbw_mode = mode; }
  bool is_romwbw_mode() const { return romwbw_mode; }

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

  // Initialize memory disks (MD0=RAM, MD1=ROM) from HCB configuration
  // This reads the bank assignments from the HCB in bank 0 (ROM)
  void init_memory_disks() {
    banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
    if (!bmem) {
      fprintf(stderr, "[MD] Warning: banked memory not available, memory disks disabled\n");
      return;
    }

    // Read HCB configuration from ROM bank 0
    // HCB starts at 0x100, so actual addresses are HCB_BASE + offset
    // From emu_hbios.asm:
    // CB_BIDRAMD0 = HCB + 0xDC = 0x1DC (RAM disk start bank)
    // CB_RAMD_BNKS = HCB + 0xDD = 0x1DD (RAM disk bank count)
    // CB_BIDROMD0 = HCB + 0xDE = 0x1DE (ROM disk start bank)
    // CB_ROMD_BNKS = HCB + 0xDF = 0x1DF (ROM disk bank count)
    const uint16_t HCB_BASE = 0x100;
    uint8_t ramd_start = bmem->read_bank(0x00, HCB_BASE + 0xDC);  // 0x1DC
    uint8_t ramd_banks = bmem->read_bank(0x00, HCB_BASE + 0xDD);  // 0x1DD
    uint8_t romd_start = bmem->read_bank(0x00, HCB_BASE + 0xDE);  // 0x1DE
    uint8_t romd_banks = bmem->read_bank(0x00, HCB_BASE + 0xDF);  // 0x1DF

    // MD0 = RAM disk
    if (ramd_banks > 0) {
      md_disks[0].start_bank = ramd_start;
      md_disks[0].num_banks = ramd_banks;
      md_disks[0].is_rom = false;
      md_disks[0].is_enabled = true;
      md_disks[0].current_lba = 0;
      uint32_t size_kb = (uint32_t)ramd_banks * 32;
      fprintf(stderr, "[MD] MD0 (RAM disk): banks 0x%02X-0x%02X, %uKB, %u sectors\n",
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
      fprintf(stderr, "[MD] MD1 (ROM disk): banks 0x%02X-0x%02X, %uKB, %u sectors\n",
              romd_start, romd_start + romd_banks - 1, size_kb, md_disks[1].total_sectors());
    }

    // Dump the HCB drive map to understand what disk units the ROM expects
    // Drive map is at HCB+0x20 (CB_DRVMAP), 16 bytes for drives A:-P:
    fprintf(stderr, "[HCB] Drive map from ROM bank 0x00 at 0x%04X:\n", HCB_BASE + 0x20);
    for (int i = 0; i < 16; i += 8) {
      fprintf(stderr, "  %c-%c: ", 'A' + i, 'A' + i + 7);
      for (int j = 0; j < 8; j++) {
        uint8_t unit = bmem->read_bank(0x00, HCB_BASE + 0x20 + i + j);
        fprintf(stderr, "0x%02X ", unit);
      }
      fprintf(stderr, "\n");
    }
    // Also dump from RAM bank 0x80 (where CBIOS reads)
    fprintf(stderr, "[HCB] Drive map from RAM bank 0x80 at 0x%04X:\n", HCB_BASE + 0x20);
    for (int i = 0; i < 16; i += 8) {
      fprintf(stderr, "  %c-%c: ", 'A' + i, 'A' + i + 7);
      for (int j = 0; j < 8; j++) {
        uint8_t unit = bmem->read_bank(0x80, HCB_BASE + 0x20 + i + j);
        fprintf(stderr, "0x%02X ", unit);
      }
      fprintf(stderr, "\n");
    }

    // Also dump some other relevant HCB fields
    // CB_DEVCNT (device count) at HCB+0x0C
    // CB_INVDSK (invalid disk) at HCB+0x15
    // CB_BOOTDU (boot disk unit) at HCB+0x16
    uint8_t devcnt = bmem->read_bank(0x00, HCB_BASE + 0x0C);
    uint8_t invdsk = bmem->read_bank(0x00, HCB_BASE + 0x15);
    uint8_t bootdu = bmem->read_bank(0x00, HCB_BASE + 0x16);
    fprintf(stderr, "[HCB] devcnt=0x%02X invdsk=0x%02X bootdu=0x%02X\n", devcnt, invdsk, bootdu);

    // Dump disk unit table entries (if any) - CB_DISKUT at HCB+0x60, 16 entries of 4 bytes each
    fprintf(stderr, "[HCB] Disk unit table at 0x%04X:\n", HCB_BASE + 0x60);
    for (int i = 0; i < 16; i++) {
      uint16_t addr = HCB_BASE + 0x60 + (i * 4);
      uint8_t b0 = bmem->read_bank(0x00, addr);
      uint8_t b1 = bmem->read_bank(0x00, addr + 1);
      uint8_t b2 = bmem->read_bank(0x00, addr + 2);
      uint8_t b3 = bmem->read_bank(0x00, addr + 3);
      if (b0 != 0xFF) {  // Skip only 0xFF entries (show MD=0x00 entries too)
        fprintf(stderr, "  [%d] %02X %02X %02X %02X (ROM)\n", i, b0, b1, b2, b3);
      }
    }
    // Also dump from RAM bank 0x80 (where boot loader reads)
    fprintf(stderr, "[HCB] Disk unit table in RAM bank 0x80:\n");
    for (int i = 0; i < 16; i++) {
      uint16_t addr = HCB_BASE + 0x60 + (i * 4);
      uint8_t b0 = bmem->read_bank(0x80, addr);
      uint8_t b1 = bmem->read_bank(0x80, addr + 1);
      uint8_t b2 = bmem->read_bank(0x80, addr + 2);
      uint8_t b3 = bmem->read_bank(0x80, addr + 3);
      if (b0 != 0xFF) {  // Skip only 0xFF entries
        fprintf(stderr, "  [%d] %02X %02X %02X %02X (RAM)\n", i, b0, b1, b2, b3);
      }
    }
  }

  // Attach HBIOS disk image (file must already exist and be validated)
  bool attach_hbios_disk(int unit, const std::string& image_path) {
    if (unit < 0 || unit >= 16) return false;

    // Close any existing disk
    if (hb_disks[unit].image_file) {
      fclose(hb_disks[unit].image_file);
      hb_disks[unit].image_file = nullptr;
    }

    // Open the disk image for read/write - file must exist (validated earlier)
    hb_disks[unit].image_path = image_path;
    hb_disks[unit].image_file = fopen(image_path.c_str(), "r+b");
    if (!hb_disks[unit].image_file) {
      // Try read-only as fallback
      hb_disks[unit].image_file = fopen(image_path.c_str(), "rb");
      if (!hb_disks[unit].image_file) {
        fprintf(stderr, "[HBIOS] Failed to open disk image: %s\n", image_path.c_str());
        hb_disks[unit].is_open = false;
        return false;
      }
      fprintf(stderr, "[HBIOS] Attached disk unit %d (read-only): %s\n", unit, image_path.c_str());
    } else {
      fprintf(stderr, "[HBIOS] Attached disk unit %d: %s\n", unit, image_path.c_str());
    }

    hb_disks[unit].is_open = true;
    hb_disks[unit].current_lba = 0;
    return true;
  }

  // Set max slices for a disk (used by --disk0=file:N syntax)
  void set_disk_slice_count(int unit, int slices) {
    if (unit < 0 || unit >= 16) return;
    if (slices < 1) slices = 1;
    if (slices > 8) slices = 8;
    hb_disks[unit].max_slices = slices;
  }

  // Attach HDSK disk image (SIMH port 0xFD protocol)
  bool attach_hdsk_disk(int unit, const std::string& image_path) {
    if (unit < 0 || unit >= 8) return false;

    // Close any existing disk
    if (hdsk_files[unit]) {
      fclose(hdsk_files[unit]);
      hdsk_files[unit] = nullptr;
    }

    // Open the disk image
    hdsk_paths[unit] = image_path;
    hdsk_files[unit] = fopen(image_path.c_str(), "r+b");
    if (!hdsk_files[unit]) {
      // Try read-only
      hdsk_files[unit] = fopen(image_path.c_str(), "rb");
      if (!hdsk_files[unit]) {
        fprintf(stderr, "[HDSK] Failed to open disk image: %s\n", image_path.c_str());
        return false;
      }
      fprintf(stderr, "[HDSK] Attached disk unit %d (read-only): %s\n", unit, image_path.c_str());
    } else {
      fprintf(stderr, "[HDSK] Attached disk unit %d: %s\n", unit, image_path.c_str());
    }
    return true;
  }

  // Register a ROM application (OS that can be booted from "ROM")
  bool register_rom_app(char key, const std::string& name, const std::string& sys_path) {
    if (num_rom_apps >= MAX_ROM_APPS) {
      fprintf(stderr, "[ROMAPP] Error: Maximum ROM apps (%d) reached\n", MAX_ROM_APPS);
      return false;
    }

    // Check that the file exists
    FILE* f = fopen(sys_path.c_str(), "rb");
    if (!f) {
      fprintf(stderr, "[ROMAPP] Warning: Cannot open %s: %s\n", sys_path.c_str(), strerror(errno));
      return false;
    }
    fclose(f);

    rom_apps[num_rom_apps].key = toupper(key);
    rom_apps[num_rom_apps].name = name;
    rom_apps[num_rom_apps].sys_path = sys_path;
    rom_apps[num_rom_apps].is_loaded = true;
    fprintf(stderr, "[ROMAPP] Registered '%c' = %s (%s)\n", toupper(key), name.c_str(), sys_path.c_str());
    num_rom_apps++;
    return true;
  }

  // Find ROM app by key
  int find_rom_app(char key) const {
    char ukey = toupper(key);
    for (int i = 0; i < num_rom_apps; i++) {
      if (rom_apps[i].key == ukey && rom_apps[i].is_loaded) {
        return i;
      }
    }
    return -1;
  }

  void set_sense_switches(uint8_t val) {
    sense_switches = val;
    if (debug) fprintf(stderr, "Sense switches set to: 0x%02X\n", sense_switches);
  }

  // Handle IN instruction - returns value read from port
  uint8_t handle_in(uint8_t port) {
    port_in_counts[port]++;
    uint8_t value = 0xFF;  // Unknown ports return 0xFF (matches web emulator)

    // RomWBW UART ports (accent on 0x68-0x6F range for 16550-style UART)
    if (romwbw_mode) {
      switch (port) {
        case 0x68:  // UART data register
          if (input_char >= 0) {
            value = normalize_eol(input_char);
            input_char = -1;
          } else if (peek_char >= 0) {
            int ch = peek_char;
            peek_char = -1;
            if (check_ctrl_c_exit(ch)) {
              value = 0;  // ^C consumed, return nothing
            } else {
              value = normalize_eol(ch);
            }
          } else if (stdin_has_data()) {
            int ch = peek_char;
            peek_char = -1;
            if (ch == EOF) ch = 0;
            if (check_ctrl_c_exit(ch)) {
              value = 0;  // ^C consumed, return nothing
            } else {
              value = normalize_eol(ch);
            }
          }
          return value;

        case 0x6D:  // UART Line Status Register (LSR)
          value = 0x60;  // THRE + TEMT (transmitter empty)
          if (input_char >= 0 || peek_char >= 0 || stdin_has_data()) {
            value |= 0x01;  // Data ready
          }
          return value;

        case 0x69:  // UART IER
        case 0x6A:  // UART IIR
        case 0x6B:  // UART LCR
        case 0x6C:  // UART MCR
        case 0x6E:  // UART MSR
        case 0x6F:  // UART SCR
          return 0x00;  // Safe defaults

        case 0x70:  // RTC latch register (read: returns 0xFF = no RTC present)
          return 0xFF;

        case 0x78:  // Bank select (write-only, return current bank)
        case 0x7C:
          return memory->get_current_bank();

        case 0xFD:  // SIMH HDSK port - return result code
          return hdsk_result;

        case 0xFE:  // SIMH RTC port - return time data
          return simh_rtc_read();
      }
    }

    switch (port) {
      case 0x00:  // SIO status (alternate)
      case 0x10:  // SIO status (standard)
        value = 0x02;  // TDRE always set
        if (input_char >= 0 || stdin_has_data()) {
          value |= 0x01;  // RDRF set
        }
        break;

      case 0x01:  // SIO data (alternate)
      case 0x11:  // SIO data (standard)
        if (input_char >= 0) {
          // Check for console escape before returning
          if (input_char == console_escape_char && isatty(STDIN_FILENO)) {
            console_mode_requested = true;
            input_char = -1;
            value = 0;  // Don't pass escape to program
          } else {
            value = normalize_eol(input_char) & 0x7F;
            input_char = -1;
          }
        } else if (peek_char >= 0) {
          // Use character already peeked by stdin_has_data() or check_console_escape_async()
          int ch = peek_char;
          peek_char = -1;
          if (ch == console_escape_char && isatty(STDIN_FILENO)) {
            console_mode_requested = true;
            value = 0;  // Don't pass escape to program
          } else if (check_ctrl_c_exit(ch)) {
            value = 0;  // ^C consumed
          } else {
            value = normalize_eol(ch) & 0x7F;
          }
        } else if (stdin_has_data()) {
          // stdin_has_data() sets peek_char, so use it
          int ch = peek_char;
          peek_char = -1;
          if (ch == EOF) ch = 0;
          if (ch == console_escape_char && isatty(STDIN_FILENO)) {
            console_mode_requested = true;
            value = 0;  // Don't pass escape to program
          } else if (check_ctrl_c_exit(ch)) {
            value = 0;  // ^C consumed
          } else {
            value = normalize_eol(ch) & 0x7F;
          }
        }
        break;

      case 0xFF:
        value = sense_switches;
        break;

      default:
        if (unknown_ports.find(port) == unknown_ports.end()) {
          if (debug) fprintf(stderr, "[WARNING: Unknown IN port 0x%02X]\n", port);
          unknown_ports.insert(port);
        }
        if (strict_io_mode) {
          fprintf(stderr, "\n[STRICT I/O] Unexpected IN port 0x%02X at PC=0x%04X\n",
                  port, cpu->regs.PC.get_pair16());
          fprintf(stderr, "  Registers: A=0x%02X BC=0x%04X DE=0x%04X HL=0x%04X SP=0x%04X\n",
                  cpu->get_reg8(qkz80::reg_A),
                  cpu->regs.BC.get_pair16(),
                  cpu->regs.DE.get_pair16(),
                  cpu->regs.HL.get_pair16(),
                  cpu->regs.SP.get_pair16());
          halted = true;
        }
        break;
    }

    return value;
  }

  // Handle OUT instruction
  void handle_out(uint8_t port, uint8_t value) {
    port_out_counts[port]++;

    // RomWBW ports
    if (romwbw_mode) {
      switch (port) {
        case 0x68:  // UART data register - output character
          if (debug) fprintf(stderr, "[UART OUT] 0x%02X '%c'\n", value, (value >= 32 && value < 127) ? value : '.');
          emu_console_write_char(value);
          return;

        case 0x69:  // UART IER
        case 0x6A:  // UART FCR (FIFO control)
        case 0x6B:  // UART LCR
        case 0x6C:  // UART MCR
        case 0x6F:  // UART SCR
          return;  // Ignored

        case 0x70:  // RTC latch register (write: latch current time, ignored in emulation)
          return;

        case 0x78:  // RAM bank select
        case 0x7C:  // ROM bank select (same effect in MM_SBC)
          memory->select_bank(value);
          return;

        case 0xED:  // EMU BNKCALL port - bank call with IX=addr, A=bank
          handle_emu_bnkcall(value);
          return;

        case 0xEE:  // EMU HBIOS signal port
          handle_emu_signal(value);
          return;

        case 0xEF:  // HBIOS dispatch trigger port
          {
            // Trace HBIOS calls for debugging
            static int hbios_trace_count = 0;
            uint8_t func = cpu->regs.BC.get_high();
            if (hbios_trace_count < 200 && (func == 0x12 || func == 0x13)) {
              hbios_trace_count++;
              fprintf(stderr, "[PORT 0xEF #%d] PC=0x%04X func=0x%02X unit=%d\n",
                      hbios_trace_count, cpu->regs.PC.get_pair16(), func, cpu->regs.BC.get_low());
            }
          }
          handle_hbios_dispatch();
          return;

        case 0xFD:  // SIMH HDSK port - receive parameter byte
          hdsk_write(value);
          return;

        case 0xFE:  // SIMH RTC port - write command
          simh_rtc_write(value);
          return;
      }
    }

    switch (port) {
      case 0x00:  // SIO control (alternate)
      case 0x10:  // SIO control (standard)
        break;

      case 0x01:  // SIO data (alternate)
      case 0x11:  // SIO data (standard)
        if ((value & 0x7F) != 0x00) {
          emu_console_write_char(value);
        }
        break;

      case 0xFF:
        break;

      default:
        if (unknown_ports.find(port) == unknown_ports.end()) {
          if (debug) fprintf(stderr, "[WARNING: Unknown OUT port 0x%02X]\n", port);
          unknown_ports.insert(port);
        }
        if (strict_io_mode) {
          fprintf(stderr, "\n[STRICT I/O] Unexpected OUT port 0x%02X value 0x%02X at PC=0x%04X\n",
                  port, value, cpu->regs.PC.get_pair16());
          fprintf(stderr, "  Registers: A=0x%02X BC=0x%04X DE=0x%04X HL=0x%04X SP=0x%04X\n",
                  cpu->get_reg8(qkz80::reg_A),
                  cpu->regs.BC.get_pair16(),
                  cpu->regs.DE.get_pair16(),
                  cpu->regs.HL.get_pair16(),
                  cpu->regs.SP.get_pair16());
          halted = true;
        }
        break;
    }
  }

private:
  // Convert value to BCD
  uint8_t to_bcd(int val) {
    return ((val / 10) << 4) | (val % 10);
  }

  // SIMH RTC write handler (port 0xFE)
  // Commands: 0x07 = read clock, 0x08 = write clock
  void simh_rtc_write(uint8_t value) {
    simh_rtc_cmd = value;
    simh_rtc_idx = 0;

    if (value == 0x07) {  // Read clock command
      // Fill buffer with current time in BCD: YY MM DD HH MM SS
      time_t now = time(nullptr);
      struct tm* t = localtime(&now);
      simh_rtc_buf[0] = to_bcd(t->tm_year % 100);  // YY (00-99)
      simh_rtc_buf[1] = to_bcd(t->tm_mon + 1);     // MM (01-12)
      simh_rtc_buf[2] = to_bcd(t->tm_mday);        // DD (01-31)
      simh_rtc_buf[3] = to_bcd(t->tm_hour);        // HH (00-23)
      simh_rtc_buf[4] = to_bcd(t->tm_min);         // MM (00-59)
      simh_rtc_buf[5] = to_bcd(t->tm_sec);         // SS (00-59)
      if (debug) {
        fprintf(stderr, "[SIMH RTC] Read: %02X-%02X-%02X %02X:%02X:%02X\n",
                simh_rtc_buf[0], simh_rtc_buf[1], simh_rtc_buf[2],
                simh_rtc_buf[3], simh_rtc_buf[4], simh_rtc_buf[5]);
      }
    }
    // Other commands ignored for now
  }

  // SIMH RTC read handler (port 0xFE)
  uint8_t simh_rtc_read() {
    if (simh_rtc_cmd == 0x07 && simh_rtc_idx < 6) {
      return simh_rtc_buf[simh_rtc_idx++];
    }
    return 0;
  }

  // SIMH HDSK write handler (port 0xFD)
  // Protocol: OTIR sends 7 bytes: CMD, DRV, SEC, TRK_LO, TRK_HI, DMA_LO, DMA_HI
  // Commands: 1=RESET, 2=READ, 3=WRITE
  void hdsk_write(uint8_t value) {
    // Store byte in parameter buffer
    if (hdsk_parm_idx < 7) {
      hdsk_parm[hdsk_parm_idx++] = value;
    }

    // When we have 7 bytes, execute the command
    if (hdsk_parm_idx >= 7) {
      hdsk_execute_command();
      hdsk_parm_idx = 0;  // Reset for next command
    }
  }

  // Execute HDSK command after receiving all 7 parameter bytes
  void hdsk_execute_command() {
    uint8_t cmd = hdsk_parm[0];
    uint8_t drv = hdsk_parm[1];
    uint8_t sec = hdsk_parm[2];
    uint16_t trk = hdsk_parm[3] | (hdsk_parm[4] << 8);
    uint16_t dma = hdsk_parm[5] | (hdsk_parm[6] << 8);

    // Calculate LBA: (track * 256) + sector
    uint32_t lba = (uint32_t)trk * 256 + sec;
    uint32_t offset = lba * 512;

    if (debug) {
      fprintf(stderr, "[HDSK] cmd=%d drv=%d sec=%d trk=%d dma=0x%04X lba=%d offset=0x%X\n",
              cmd, drv, sec, trk, dma, lba, offset);
    }

    // Check drive number
    if (drv >= 8 || !hdsk_files[drv]) {
      if (debug) fprintf(stderr, "[HDSK] Drive %d not attached\n", drv);
      hdsk_result = 0xFF;  // Error
      return;
    }

    FILE* f = hdsk_files[drv];

    switch (cmd) {
      case 1:  // RESET
        hdsk_result = 0;  // Success
        break;

      case 2:  // READ - read 512 bytes from disk to memory at DMA
        {
          uint8_t buf[512];
          fseek(f, offset, SEEK_SET);
          size_t n = fread(buf, 1, 512, f);
          if (n < 512) {
            // Pad with zeros if reading past end of file
            memset(buf + n, 0, 512 - n);
          }
          // Write to Z80 memory at DMA address
          for (int i = 0; i < 512; i++) {
            memory->store_mem(dma + i, buf[i]);
          }
          hdsk_result = 0;  // Success
          if (debug) {
            fprintf(stderr, "[HDSK] READ lba=%d -> dma=0x%04X (read %zu bytes)\n", lba, dma, n);
          }
        }
        break;

      case 3:  // WRITE - write 512 bytes from memory at DMA to disk
        {
          uint8_t buf[512];
          // Read from Z80 memory at DMA address
          for (int i = 0; i < 512; i++) {
            buf[i] = memory->fetch_mem(dma + i);
          }
          fseek(f, offset, SEEK_SET);
          size_t n = fwrite(buf, 1, 512, f);
          if (n < 512) {
            hdsk_result = 0xFF;  // Write error
            fprintf(stderr, "[HDSK] WRITE error at lba=%d\n", lba);
          } else {
            hdsk_result = 0;  // Success
            if (debug) {
              fprintf(stderr, "[HDSK] WRITE lba=%d <- dma=0x%04X\n", lba, dma);
            }
          }
        }
        break;

      default:
        if (debug) fprintf(stderr, "[HDSK] Unknown command %d\n", cmd);
        hdsk_result = 0xFF;  // Error
        break;
    }
  }

  // Handle EMU BNKCALL port (port 0xED)
  // Called when HB_BNKCALL at 0xFFF9 executes: OUT (0xED), A
  // On entry: A = target bank, IX = call address in target bank
  // We intercept specific known vectors (like PRTSUM at 0x0406) and handle them
  void handle_emu_bnkcall(uint8_t bank) {
    uint16_t call_addr = cpu->regs.IX.get_pair16();

    if (debug) {
      fprintf(stderr, "[EMU BNKCALL] bank=0x%02X addr=0x%04X\n", bank, call_addr);
    }

    // Check for known HBIOS vectors that we need to handle
    switch (call_addr) {
      case 0x0406:  // PRTSUM - Print device summary (used by boot loader 'D' command)
        print_device_summary();
        break;

      default:
        // Unknown bank call - just log and return (caller will see no action)
        if (debug) {
          fprintf(stderr, "[EMU BNKCALL] Unknown vector 0x%04X in bank 0x%02X - ignoring\n",
                  call_addr, bank);
        }
        break;
    }

    // The proxy code at 0xFFF9 has OUT (0xED), A followed by RET
    // So we don't need to do anything else - Z80 will execute the RET
  }

  // Print device summary (implements PRTSUM vector 0x0406)
  // This is called by the boot loader 'D' command
  void print_device_summary() {
    // Print header
    const char* header = "\r\n  Unit Device\r\n  ---- ------\r\n";
    for (const char* p = header; *p; p++) {
      emu_console_write_char(*p);
    }

    int unit_num = 0;

    // First, list memory disks (MD0=RAM, MD1=ROM)
    for (int i = 0; i < 2; i++) {
      if (md_disks[i].is_enabled) {
        char line[80];
        const char* type_str = md_disks[i].is_rom ? "ROM Disk" : "RAM Disk";
        uint32_t size_kb = (uint32_t)md_disks[i].num_banks * 32;
        snprintf(line, sizeof(line), "    %d  MD%d: %s (%uKB)\r\n",
                 unit_num, i, type_str, size_kb);
        for (const char* p = line; *p; p++) {
          emu_console_write_char(*p);
        }
        unit_num++;
      }
    }

    // Then list hard disks
    for (int i = 0; i < 16; i++) {
      if (hb_disks[i].is_open) {
        char line[80];
        // Get file size
        long cur_pos = ftell(hb_disks[i].image_file);
        fseek(hb_disks[i].image_file, 0, SEEK_END);
        long file_size = ftell(hb_disks[i].image_file);
        fseek(hb_disks[i].image_file, cur_pos, SEEK_SET);
        uint64_t size_mb = (file_size + 512*1024) / (1024*1024);
        snprintf(line, sizeof(line), "    %d  HD%d: Hard Disk (%lluMB, %d slices)\r\n",
                 unit_num, i, (unsigned long long)size_mb, hb_disks[i].max_slices);
        for (const char* p = line; *p; p++) {
          emu_console_write_char(*p);
        }
        unit_num++;
      }
    }

    // End with blank line
    emu_console_write_char('\r');
    emu_console_write_char('\n');
  }

  // Handle EMU HBIOS signal port (port 0xEE)
  // Protocol:
  //   0x01 = HBIOS starting (boot code reached HBIOS entry)
  //   0xFE = PREINIT about to be called (test mode: halt here)
  //   0xFF = Init complete (HBIOS dispatch via port 0xEF is ready)
  void handle_emu_signal(uint8_t value) {
    if (debug) {
      fprintf(stderr, "[EMU signal: 0x%02X at PC=0x%04X]\n", value, cpu->regs.PC.get_pair16());
    }

    switch (value) {
      case 0x01:  // HBIOS starting
        if (debug) fprintf(stderr, "[EMU HBIOS: Boot code starting...]\n");
        break;

      case 0xFE:  // PREINIT about to be called - test mode halt point
        fprintf(stderr, "[EMU HBIOS: PREINIT point reached - halting for analysis]\n");
        fprintf(stderr, "  No unexpected I/O detected before this point.\n");
        fprintf(stderr, "  PC=0x%04X SP=0x%04X\n",
                cpu->regs.PC.get_pair16(), cpu->regs.SP.get_pair16());
        halted = true;
        break;

      case 0xFF:  // Init complete - HBIOS dispatch now via port 0xEF
        if (debug) fprintf(stderr, "[EMU HBIOS: Init complete, dispatch via port 0xEF]\n");

        // If romldr path is set, switch to ROM bank 1 and run romldr from ROM
        if (!romldr_path.empty()) {
          banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
          if (bmem && romldr_loaded) {
            // romldr is in ROM bank 1. Switch to it and jump to entry point.
            if (debug) fprintf(stderr, "[ROMLDR] Switching to ROM bank 1 (romldr), jumping to 0x0000\n");
            bmem->select_bank(0x01);  // ROM bank 1
            next_pc = 0x0000;  // Signal main loop to jump here instead of pc+2
            next_pc_valid = true;
          }
        }
        break;

      default:
        if (debug) fprintf(stderr, "[EMU HBIOS: Unknown signal 0x%02X]\n", value);
        break;
    }
  }

  // Handle HBIOS dispatch via I/O port 0xEF
  // Called when Z80 code executes: OUT (0xEF), A
  // The proxy code at 0xFFF0 should be:
  //   OUT (0xEF), A   ; Trigger dispatch (we handle HBIOS call here)
  //   RET             ; Return to caller (Z80 executes this normally)
  // We do NOT do a synthetic RET here - the Z80 proxy handles it.
  void handle_hbios_dispatch() {
    if (debug) {
      uint8_t func = cpu->regs.BC.get_high();
      uint8_t unit = cpu->regs.BC.get_low();
      fprintf(stderr, "[HBIOS dispatch via port 0xEF] func=0x%02X unit=0x%02X DE=0x%04X HL=0x%04X\n",
              func, unit, cpu->regs.DE.get_pair16(), cpu->regs.HL.get_pair16());
    }

    // Set flag to skip synthetic RET (Z80 proxy has its own RET instruction)
    skip_synthetic_ret = true;

    // Handle the HBIOS call
    handle_hbios_call(cpu->regs.PC.get_pair16());

    skip_synthetic_ret = false;
  }

public:
  // Debug: trace instruction if cioin_trace_count > 0
  void trace_after_cioin(uint16_t pc, uint8_t opcode) {
    if (cioin_trace_count > 0) {
      fprintf(stderr, "[TRACE] PC=0x%04X op=0x%02X A=0x%02X E=0x%02X F=0x%02X SP=0x%04X\n",
              pc, opcode, cpu->regs.AF.get_high(), cpu->regs.DE.get_low(),
              cpu->regs.AF.get_low(), cpu->regs.SP.get_pair16());
      cioin_trace_count--;
    }
  }

  // Handle PRTSUM - print device summary (for future use via port dispatch)
  bool handle_prtsum() {
    // Print header
    const char* header = "\r\nDisk Device Summary\r\n\r\n";
    for (const char* p = header; *p; p++) {
      emu_console_write_char(*p);
    }

    // Print header line
    const char* hdr_line = " Unit Dev       Type    Capacity\r\n";
    for (const char* p = hdr_line; *p; p++) {
      emu_console_write_char(*p);
    }
    const char* hdr_sep = " ---- --------- ------- --------\r\n";
    for (const char* p = hdr_sep; *p; p++) {
      emu_console_write_char(*p);
    }

    int unit_num = 0;
    char line[80];

    // Print memory disks
    for (int i = 0; i < 2; i++) {
      if (md_disks[i].is_enabled) {
        const char* type = md_disks[i].is_rom ? "ROM" : "RAM";
        uint32_t size_kb = md_disks[i].num_banks * 32;
        snprintf(line, sizeof(line), "   %2d MD%d       %-7s %4uKB\r\n",
                 unit_num, i, type, size_kb);
        for (const char* p = line; *p; p++) {
          emu_console_write_char(*p);
        }
        unit_num++;
      }
    }

    // Print hard disks
    for (int i = 0; i < 16; i++) {
      if (hb_disks[i].is_open) {
        // Get file size
        long size_mb = 0;
        if (hb_disks[i].image_file) {
          long cur = ftell(hb_disks[i].image_file);
          fseek(hb_disks[i].image_file, 0, SEEK_END);
          size_mb = ftell(hb_disks[i].image_file) / (1024 * 1024);
          fseek(hb_disks[i].image_file, cur, SEEK_SET);
        }
        snprintf(line, sizeof(line), "   %2d HDSK%d     Hard    %4ldMB\r\n",
                 unit_num, i, size_mb);
        for (const char* p = line; *p; p++) {
          emu_console_write_char(*p);
        }
        unit_num++;
      }
    }

    // Simulate RET instruction - pop return address from stack
    uint16_t sp = cpu->regs.SP.get_pair16();
    uint8_t low = memory->fetch_mem(sp);
    uint8_t high = memory->fetch_mem(sp + 1);
    cpu->regs.SP.set_pair16(sp + 2);
    cpu->regs.PC.set_pair16((high << 8) | low);

    return true;  // Handled
  }

  // Handle HBIOS call - called when PC is at an HBIOS dispatch address
  // Returns true if handled, false to continue normal execution
  bool handle_hbios_call(uint16_t pc) {
    // Check for escape character periodically (every HBIOS call) so Ctrl+E works in tight loops
    check_console_escape_async();

    uint8_t func = cpu->regs.BC.get_high();  // Function code in B register
    uint8_t unit = cpu->regs.BC.get_low();   // Unit number in C register
    uint8_t result = HBR_SUCCESS;
    uint16_t hl_before = cpu->regs.HL.get_pair16();  // Track HL for debugging

    if (debug) {
      fprintf(stderr, "[HBIOS call at 0x%04X, func=0x%02X, unit=0x%02X, HL=0x%04X]\n", pc, func, unit, hl_before);
    }


    switch (func) {
      case HBF_CIOIN: {  // Console input
        // Retry loop for ^C handling - if ^C is consumed, keep waiting for input
        for (;;) {
          // Wait for character from stdin
          while (input_char < 0 && peek_char < 0 && !stdin_eof) {
            // Quick check for stdin
            fd_set readfds;
            struct timeval tv;
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);
            tv.tv_sec = 0;
            tv.tv_usec = 1000;  // 1ms
            if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) > 0) {
              int ch = raw_read_stdin();
              if (ch == -2) {  // EOF
                stdin_eof = true;
                break;  // Exit loop on EOF
              } else if (ch >= 0) {
                // Check for console escape (^E)
                if (ch == console_escape_char && isatty(STDIN_FILENO)) {
                  console_mode_requested = true;
                  continue;  // Don't pass escape char to program
                }
                // Check for ^C exit - consume ^C chars, don't pass to program
                if (check_ctrl_c_exit(ch)) {
                  continue;  // ^C was counted for exit, don't pass to program
                }
                peek_char = ch;
                break;
              }
            }
          }
          uint8_t ch;
          if (input_char >= 0) {
            ch = input_char & 0xFF;
            input_char = -1;
          } else if (peek_char >= 0) {
            ch = peek_char & 0xFF;
            peek_char = -1;
            // Check for ^C exit when consuming peeked char
            if (check_ctrl_c_exit(ch)) {
              continue;  // ^C consumed - retry waiting for input
            }
          } else if (stdin_eof && !isatty(STDIN_FILENO)) {
            // EOF on pipe input - exit cleanly
            exit(0);
          } else {
            // No input available - return Ctrl-Z (EOF character) for CP/M
            ch = 0x1A;  // Ctrl-Z
          }
          // Got a valid character - translate LF and return
          if (ch == 0x0A) {
            ch = 0x0D;
          }
          cpu->regs.DE.set_low(ch);  // Return character in E
          break;  // Exit retry loop
        }  // end for(;;)
        break;
      }

      case HBF_CIOOUT: {  // Console output
        uint8_t ch = cpu->regs.DE.get_low();
        emu_console_write_char(ch);
        break;
      }

      case HBF_CIOIST: {  // Console input status - return count in A
        uint8_t count = 0;
        // Check stdin for any unit that acts as console (0xFF, 0xD0, 0x00)
        bool is_console_unit = (unit == 0xFF || unit == 0xD0 || unit == 0x00);
        if (is_console_unit) {
          bool has_stdin = (input_char >= 0 || stdin_has_data());
          count = has_stdin ? 1 : 0;
        }
        result = count;
        break;
      }

      case HBF_CIOOST: {  // Console output status - return space available in A
        result = 255;  // Lots of space available in output buffer
        break;
      }

      case HBF_CIOINIT: {  // 0x04 - Init/reset device/line config
        // Just return success - we don't actually configure anything
        if (debug) {
          fprintf(stderr, "[HBIOS CIOINIT] Unit %d init\n", unit);
        }
        break;
      }

      case HBF_CIOQUERY: {  // 0x05 - Report device/line config
        // Returns DE = line characteristics (baud, parity, etc.)
        // We return a reasonable default: 9600 baud, 8N1
        cpu->regs.DE.set_pair16(0x0000);  // Default config
        if (debug) {
          fprintf(stderr, "[HBIOS CIOQUERY] Unit %d query\n", unit);
        }
        break;
      }

      case HBF_CIODEVICE: {  // 0x06 - Report device info
        // Returns: D = device type, E = physical unit number, C = device attributes
        // Device types from hbios.inc: CIODEV_*
        // 0x00 = UART, 0x10 = ASCI, 0x20 = TERM (terminal), 0x30 = PRPCON, etc.
        // We report as TERM (terminal emulator)
        cpu->regs.DE.set_high(0x20);   // D = CIODEV_TERM (terminal)
        cpu->regs.DE.set_low(0);       // E = physical unit 0
        cpu->regs.BC.set_low(0x03);    // C = attributes: input + output capable
        if (debug) {
          fprintf(stderr, "[HBIOS CIODEVICE] Unit %d: type=0x20 (TERM)\n", unit);
        }
        break;
      }

      case HBF_SYSVER: {  // Get version
        // Return version to match the ROM being used (SBC_simh_std = v3.5.1)
        // Format: D=major/minor (high/low nibble), E=update/patch (high/low nibble)
        // v3.5.1.0 = D=0x35, E=0x10
        cpu->regs.DE.set_pair16(0x3510);  // Version 3.5.1.0
        cpu->regs.HL.set_low(0x01);        // Platform ID = SBC (matches SBC_simh_std)
        break;
      }

      case HBF_SYSBOOT: {  // EMU: Boot from disk or ROM app
        // HL register contains address of command string buffer from boot menu
        // Command string format: "HD0:0", "MD0:0", "0", or single letter for ROM app
        uint16_t cmd_addr = cpu->regs.HL.get_pair16();

        // Read command string from memory (null-terminated, max 64 chars)
        char cmd_str[64];
        int i = 0;
        for (; i < 63; i++) {
          char c = memory->fetch_mem(cmd_addr + i);
          if (c == 0 || c == '\r' || c == '\n') break;
          cmd_str[i] = c;
        }
        cmd_str[i] = '\0';

        fprintf(stderr, "[SYSBOOT] Command string: '%s'\n", cmd_str);

        // Skip leading whitespace
        char* p = cmd_str;
        while (*p == ' ') p++;

        // Check for ROM application boot (single letter command)
        if (*p != '\0' && p[1] == '\0' && isalpha(*p)) {
          int app_idx = find_rom_app(*p);
          if (app_idx >= 0) {
            // Boot from ROM application
            fprintf(stderr, "[SYSBOOT] Booting ROM app '%c' = %s\n", *p, rom_apps[app_idx].name.c_str());

            // Load .sys file from ROM app path (same format as disk boot)
            FILE* app_file = fopen(rom_apps[app_idx].sys_path.c_str(), "rb");
            if (!app_file) {
              fprintf(stderr, "[SYSBOOT] Error: Cannot open ROM app file: %s\n", rom_apps[app_idx].sys_path.c_str());
              result = HBR_FAILED;
              break;
            }

            // Read metadata from offset 0x5E0 (same as disk boot)
            uint8_t meta_buf[32];
            fseek(app_file, 0x5E0, SEEK_SET);
            size_t read_bytes = fread(meta_buf, 1, 32, app_file);
            if (read_bytes < 32) {
              fprintf(stderr, "[SYSBOOT] Error: Could not read ROM app metadata\n");
              fclose(app_file);
              result = HBR_FAILED;
              break;
            }

            // Parse metadata
            uint16_t load_addr = meta_buf[26] | (meta_buf[27] << 8);
            uint16_t end_addr = meta_buf[28] | (meta_buf[29] << 8);
            uint16_t entry_addr = meta_buf[30] | (meta_buf[31] << 8);

            fprintf(stderr, "[SYSBOOT] ROM App Load: 0x%04X - 0x%04X, Entry: 0x%04X\n",
                    load_addr, end_addr, entry_addr);

            // Calculate how many bytes to load
            size_t load_size = end_addr - load_addr;
            size_t sectors = (load_size + 511) / 512;

            // System image starts at offset 0x600
            fseek(app_file, 0x600, SEEK_SET);

            // Switch to user bank
            banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
            if (bmem) {
              // Copy page zero from ROM bank 0 to RAM bank 0x8E
              for (uint16_t addr = 0x0000; addr < 0x0100; addr++) {
                uint8_t byte = bmem->read_bank(0x00, addr);
                bmem->write_bank(0x8E, addr, byte);
              }
              // Copy HCB
              for (uint16_t addr = 0x0100; addr < 0x0200; addr++) {
                uint8_t byte = bmem->read_bank(0x00, addr);
                bmem->write_bank(0x8E, addr, byte);
              }
              // Patch APITYPE to HBIOS (0x00) instead of UNA (0xFF)
              bmem->write_bank(0x8E, 0x0112, 0x00);
              bmem->select_bank(0x8E);
            }

            // Read system image directly into memory
            uint16_t addr = load_addr;
            for (size_t sec = 0; sec < sectors && addr < end_addr; sec++) {
              uint8_t sector_buf[512];
              size_t rd = fread(sector_buf, 1, 512, app_file);
              if (rd == 0) break;
              for (size_t b = 0; b < rd && addr < end_addr; b++) {
                memory->store_mem(addr++, sector_buf[b]);
              }
            }
            fclose(app_file);

            fprintf(stderr, "[SYSBOOT] ROM app loaded, jumping to 0x%04X\n", entry_addr);
            cpu->regs.PC.set_pair16(entry_addr);
            break;
          }
        }

        // Parse the command string to determine disk unit
        // Unit numbering: 0-1 = MD (memory disks), 2+ = HD (hard disks)
        int boot_unit = -1;  // -1 = not found
        int boot_slice = 0;
        bool boot_is_memdisk = false;

        if (*p == '\0') {
          // Empty command - use first available disk (prefer MD, then HD)
          for (int u = 0; u < 2; u++) {
            if (md_disks[u].is_enabled) {
              boot_unit = u;
              boot_is_memdisk = true;
              break;
            }
          }
          if (boot_unit < 0) {
            for (int u = 0; u < 16; u++) {
              if (hb_disks[u].is_open) {
                boot_unit = u + 2;  // HD units start at 2
                boot_is_memdisk = false;
                break;
              }
            }
          }
        } else if (*p >= '0' && *p <= '9') {
          // Numeric input - treat as DIO unit number
          boot_unit = atoi(p);
          // Look for :slice
          while (*p && *p != ':') p++;
          if (*p == ':') {
            p++;
            boot_slice = atoi(p);
          }
          boot_is_memdisk = (boot_unit < 2 && md_disks[boot_unit].is_enabled);
        } else if ((p[0] == 'H' || p[0] == 'h') && (p[1] == 'D' || p[1] == 'd')) {
          // HDn: format (hard disk) - HD0 = DIO unit 2, etc.
          p += 2;
          int hd_num = atoi(p);
          boot_unit = hd_num + 2;  // HD units start at DIO unit 2
          boot_is_memdisk = false;
          // Look for :slice
          while (*p && *p != ':') p++;
          if (*p == ':') {
            p++;
            boot_slice = atoi(p);
          }
        } else if ((p[0] == 'M' || p[0] == 'm') && (p[1] == 'D' || p[1] == 'd')) {
          // MDn: format (memory disk) - MD0 = DIO unit 0, MD1 = DIO unit 1
          p += 2;
          boot_unit = atoi(p);  // MD0=0, MD1=1
          boot_is_memdisk = true;
          // Look for :slice
          while (*p && *p != ':') p++;
          if (*p == ':') {
            p++;
            boot_slice = atoi(p);
          }
        }

        // Validate the boot unit
        bool unit_valid = false;
        if (boot_unit >= 0 && boot_unit < 2) {
          // MD unit
          unit_valid = md_disks[boot_unit].is_enabled;
          boot_is_memdisk = true;
        } else if (boot_unit >= 2 && boot_unit < 18) {
          // HD unit
          unit_valid = hb_disks[boot_unit - 2].is_open;
          boot_is_memdisk = false;
        }

        fprintf(stderr, "[SYSBOOT] Loading system from %s %d, slice %d\n",
                boot_is_memdisk ? "MD" : "HD",
                boot_is_memdisk ? boot_unit : (boot_unit - 2),
                boot_slice);

        if (!unit_valid) {
          fprintf(stderr, "[SYSBOOT] Error: disk unit %d not available\n", boot_unit);
          result = HBR_FAILED;
          break;
        }

        // Use boot_unit for the rest of the function
        unit = boot_unit;

        // Save boot info for SYSGET_BOOTINFO
        saved_boot_unit = boot_unit;
        saved_boot_slice = boot_slice;

        // Helper lambda to read bytes from disk (either MD or HD)
        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
        auto read_disk_bytes = [&](uint32_t disk_offset, uint8_t* buf, size_t len) -> size_t {
          if (boot_is_memdisk) {
            // Read from memory disk bank memory
            MemDiskState& md = md_disks[unit];
            uint32_t sectors_per_bank = 32768 / HBIOS_SECTOR_SIZE;
            size_t bytes_read = 0;

            while (bytes_read < len) {
              uint32_t lba = disk_offset / HBIOS_SECTOR_SIZE;
              uint32_t offset_in_sector = disk_offset % HBIOS_SECTOR_SIZE;

              if (lba >= md.total_sectors()) break;

              uint32_t bank_offset = lba / sectors_per_bank;
              uint32_t sector_in_bank = lba % sectors_per_bank;
              uint8_t src_bank = md.start_bank + bank_offset;
              uint16_t src_offset = sector_in_bank * HBIOS_SECTOR_SIZE + offset_in_sector;

              uint8_t byte = bmem ? bmem->read_bank(src_bank, src_offset) : 0xE5;
              buf[bytes_read++] = byte;
              disk_offset++;
            }
            return bytes_read;
          } else {
            // Read from file-backed hard disk
            int hd_unit = unit - 2;
            fseek(hb_disks[hd_unit].image_file, disk_offset, SEEK_SET);
            return fread(buf, 1, len, hb_disks[hd_unit].image_file);
          }
        };

        // Read metadata from sector 2 (offset 0x5E0) to get load parameters
        // Metadata structure at offset 0x5E0:
        //   0x5E0: PR_WP (1 byte)
        //   0x5E1: PR_UPDSEQ (2 bytes)
        //   0x5E3: PR_VER (4 bytes)
        //   0x5E7: PR_LABEL (17 bytes)
        //   0x5F8: Reserved (2 bytes)
        //   0x5FA: PR_LDLOC (2 bytes) = offset 26 in buffer
        //   0x5FC: PR_LDEND (2 bytes) = offset 28 in buffer
        //   0x5FE: PR_ENTRY (2 bytes) = offset 30 in buffer
        uint8_t meta_buf[32];
        size_t read = read_disk_bytes(0x5E0, meta_buf, 32);
        if (read < 32) {
          fprintf(stderr, "[SYSBOOT] Error: could not read metadata (got %zu bytes)\n", read);
          result = HBR_FAILED;
          break;
        }

        // Parse metadata (little-endian)
        uint16_t load_addr = meta_buf[26] | (meta_buf[27] << 8);
        uint16_t end_addr = meta_buf[28] | (meta_buf[29] << 8);
        uint16_t entry_addr = meta_buf[30] | (meta_buf[31] << 8);

        fprintf(stderr, "[SYSBOOT] Load: 0x%04X - 0x%04X, Entry: 0x%04X\n",
                load_addr, end_addr, entry_addr);

        // Switch to user bank (0x8E) for loading
        if (bmem) {
          // Copy page zero (RST vectors) and HCB from ROM bank 0 to RAM bank 0x8E
          // This is crucial - CBIOS uses RST 08 to call HBIOS!
          fprintf(stderr, "[BANK] Copying page zero (RST vectors) and HCB from ROM bank 0 to RAM bank 0x8E\n");

          // Copy page zero (0x0000-0x0100) - contains RST 08 which jumps to 0xFFF0
          for (uint16_t addr = 0x0000; addr < 0x0100; addr++) {
            uint8_t byte = bmem->read_bank(0x00, addr);  // Read from ROM bank 0
            bmem->write_bank(0x8E, addr, byte);          // Write to RAM bank 0x8E
          }

          // Copy HCB (0x0100-0x0200) - system configuration
          for (uint16_t addr = 0x0100; addr < 0x0200; addr++) {
            uint8_t byte = bmem->read_bank(0x00, addr);  // Read from ROM bank 0
            bmem->write_bank(0x8E, addr, byte);          // Write to RAM bank 0x8E
          }
          // Patch APITYPE to HBIOS (0x00) instead of UNA (0xFF)
          bmem->write_bank(0x8E, 0x0112, 0x00);

          bmem->select_bank(0x8E);  // User bank

          // Verify RST 08 vector is correct
          fprintf(stderr, "[BANK] RST 08 (0x0008) in bank 0x8E: %02X %02X %02X\n",
                  bmem->read_bank(0x8E, 0x0008), bmem->read_bank(0x8E, 0x0009), bmem->read_bank(0x8E, 0x000A));
        }

        // Read system image directly into memory (starts at offset 0x600)
        uint16_t addr = load_addr;
        uint32_t disk_offset = 0x600;  // System image starts at sector 3
        while (addr < end_addr) {
          uint8_t byte;
          if (read_disk_bytes(disk_offset++, &byte, 1) == 0) break;
          memory->store_mem(addr++, byte);
        }

        fprintf(stderr, "[SYSBOOT] Loaded %d bytes to 0x%04X-0x%04X\n",
                (int)(addr - load_addr), load_addr, addr);

        // Verify load by dumping first bytes at entry point and at 0xE68B (cold boot)
        fprintf(stderr, "[SYSBOOT] Bytes at 0xE600: %02X %02X %02X %02X\n",
                memory->fetch_mem(0xE600), memory->fetch_mem(0xE601),
                memory->fetch_mem(0xE602), memory->fetch_mem(0xE603));
        fprintf(stderr, "[SYSBOOT] Bytes at 0xE68B (cold boot): %02X %02X %02X %02X %02X %02X\n",
                memory->fetch_mem(0xE68B), memory->fetch_mem(0xE68C),
                memory->fetch_mem(0xE68D), memory->fetch_mem(0xE68E),
                memory->fetch_mem(0xE68F), memory->fetch_mem(0xE690));
        fprintf(stderr, "[SYSBOOT] Bytes at 0xECE4 (CBIOS phase 2 src): %02X %02X %02X %02X\n",
                memory->fetch_mem(0xECE4), memory->fetch_mem(0xECE5),
                memory->fetch_mem(0xECE6), memory->fetch_mem(0xECE7));

        // Set up for system entry:
        // D = boot device unit
        // E = logical unit (0)
        cpu->regs.DE.set_high(unit);
        cpu->regs.DE.set_low(0);

        // Set PC to system entry point - this will NOT return to the Z80 code
        cpu->regs.PC.set_pair16(entry_addr);

        fprintf(stderr, "[SYSBOOT] Jumping to system at 0x%04X\n", entry_addr);

        // Return directly without incrementing PC (we set it to entry_addr)
        cpu->regs.AF.set_high(HBR_SUCCESS);
        return true;  // Return early - don't advance PC
      }

      case HBF_SYSBNKCPY: {  // Bank copy (0xF3) - execute copy using params from SETCPY
        // HBIOS BNKCPY interface (per RomWBW API):
        // Input: HL = Source address, DE = Destination address
        //        (Bank IDs and count from prior SETCPY call stored in globals)
        // Output: A = Result
        //
        // Uses globals set by SETCPY (0xF4):
        // g_bnkcpy_src_bank, g_bnkcpy_dst_bank, g_bnkcpy_count

        uint16_t src_addr = cpu->regs.HL.get_pair16();
        uint16_t dst_addr = cpu->regs.DE.get_pair16();
        uint16_t count = g_bnkcpy_count;
        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);

        // DEBUG: Trace BNKCPY to TPA (program loading) - disabled
        constexpr bool TRACE_BNKCPY_TPA = false;
        if (TRACE_BNKCPY_TPA && g_bnkcpy_dst_bank == 0x8E && dst_addr >= 0x0100 && dst_addr < 0x0200) {
          fprintf(stderr, "[BNKCPY->TPA] src=%02X:%04X dst=%02X:%04X len=%04X\n",
                  g_bnkcpy_src_bank, src_addr, g_bnkcpy_dst_bank, dst_addr, count);
          if (bmem) {
            fprintf(stderr, "  Source data: %02X %02X %02X %02X\n",
                    bmem->read_bank(g_bnkcpy_src_bank, src_addr),
                    bmem->read_bank(g_bnkcpy_src_bank, src_addr+1),
                    bmem->read_bank(g_bnkcpy_src_bank, src_addr+2),
                    bmem->read_bank(g_bnkcpy_src_bank, src_addr+3));
          }
        }

        if (bmem && count > 0) {
          // Copy byte by byte from source bank to dest bank
          for (uint16_t i = 0; i < count; i++) {
            // Handle common area (0x8000-0xFFFF) - always bank 0x8F
            uint8_t actual_src_bank = g_bnkcpy_src_bank;
            uint8_t actual_dst_bank = g_bnkcpy_dst_bank;
            uint16_t actual_src_addr = src_addr + i;
            uint16_t actual_dst_addr = dst_addr + i;

            // Common area addresses use bank 0x8F regardless of specified bank
            if (actual_src_addr >= 0x8000) {
              actual_src_bank = 0x8F;
              actual_src_addr -= 0x8000;  // Offset into common area
            }
            if (actual_dst_addr >= 0x8000) {
              actual_dst_bank = 0x8F;
              actual_dst_addr -= 0x8000;  // Offset into common area
            }

            uint8_t byte = bmem->read_bank(actual_src_bank, actual_src_addr);
            bmem->write_bank(actual_dst_bank, actual_dst_addr, byte);
          }
        }
        break;
      }

      case HBF_SYSSETBNK: {  // Set bank (0xF2)
        uint8_t new_bank = cpu->regs.BC.get_low();
        uint8_t prev_bank = memory->get_current_bank();
        if (debug) {
          fprintf(stderr, "[SYSSETBNK] Switching from bank 0x%02X to 0x%02X\n",
                  prev_bank, new_bank);
        }

        // When switching to a RAM bank for the first time, copy page zero and HCB
        // from ROM bank 0. This ensures romldr can read HCB values like CB_APP_BNKS.
        static uint16_t initialized_ram_banks = 0;  // Bitmap for banks 0x80-0x8F
        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
        if (bmem && (new_bank & 0x80) && !(new_bank & 0x70)) {  // RAM bank 0x80-0x8F
          uint8_t bank_idx = new_bank & 0x0F;
          if (!(initialized_ram_banks & (1 << bank_idx))) {
            // First time accessing this RAM bank - copy page zero and HCB
            if (debug) {
              fprintf(stderr, "[SYSSETBNK] Initializing RAM bank 0x%02X with page zero and HCB\n", new_bank);
            }
            // Copy page zero (0x0000-0x0100) - contains RST vectors
            for (uint16_t addr = 0x0000; addr < 0x0100; addr++) {
              uint8_t byte = bmem->read_bank(0x00, addr);
              bmem->write_bank(new_bank, addr, byte);
            }
            // Copy HCB (0x0100-0x0200) - system configuration
            for (uint16_t addr = 0x0100; addr < 0x0200; addr++) {
              uint8_t byte = bmem->read_bank(0x00, addr);
              bmem->write_bank(new_bank, addr, byte);
            }
            // Patch APITYPE to HBIOS (0x00) instead of UNA (0xFF)
            bmem->write_bank(new_bank, 0x0112, 0x00);
            initialized_ram_banks |= (1 << bank_idx);
          }
        }

        memory->select_bank(new_bank);
        cpu->regs.BC.set_low(prev_bank);  // Return previous bank in C
        break;
      }

      case HBF_SYSGETBNK: {  // Get current bank (0xF3)
        uint8_t cur_bank = memory->get_current_bank();
        cpu->regs.BC.set_low(cur_bank);  // Return current bank in C
        if (debug) {
          fprintf(stderr, "[SYSGETBNK] Current bank is 0x%02X\n", cur_bank);
        }
        break;
      }

      case HBF_SYSALLOC: {  // Allocate memory from HBIOS heap
        // Input: HL = size requested
        // Output: A = result, HL = address of allocated block (or 0 on failure)
        uint16_t size = cpu->regs.HL.get_pair16();

        // HBIOS heap is in bank 0x80 (HBIOS bank) at LOW addresses (< 0x8000)
        // This is important because addresses >= 0x8000 map to common area when
        // used with BNKCPY, which would overwrite INIT code running at 0x8000.
        // The heap starts after page zero/HCB data (0x0200) and stays within
        // the 32KB bank boundary.
        static uint16_t heap_ptr = 0x0200;  // Start of heap (after HCB at 0x0000)
        static uint16_t heap_end = 0x8000;  // End of heap - stay within bank's 32KB

        if (heap_ptr + size <= heap_end) {
          uint16_t addr = heap_ptr;
          heap_ptr += size;
          cpu->regs.HL.set_pair16(addr);
        } else {
          // Out of heap memory
          cpu->regs.HL.set_pair16(0);
          result = HBR_FAILED;
        }
        break;
      }

      case HBF_SYSFREE: {  // Free memory
        // Input: HL = address of block to free
        // We don't actually track allocations, so just succeed
        break;
      }

      case HBF_SYSRESET: {  // System reset (warm/cold boot)
        // C register: 0x01 = warm boot (restart boot loader), 0x02 = cold boot
        uint8_t reset_type = unit;  // unit is C register
        if (reset_type == 0x01 || reset_type == 0x02) {
          // Actual system reset requested (from REBOOT utility)
          fprintf(stderr, "\n[SYSRESET] %s boot requested - restarting system\n",
                  reset_type == 0x01 ? "Warm" : "Cold");

          // Switch to ROM bank 0
          banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
          if (bmem) {
            bmem->select_bank(0x00);
          }

          // Clear any pending console input to prevent boot menu getting garbage
          peek_char = -1;
          input_char = -1;

          // Set PC to 0 to restart from ROM - return from HBIOS handler will continue
          // at address 0 instead of returning to caller
          cpu->regs.PC.set_pair16(0x0000);
          return true;  // Return from handler, execution will continue at PC=0
        }
        // Other reset types (0x00 = query?) - just return success
        if (debug) {
          fprintf(stderr, "[HBIOS SYSRESET] Reset type 0x%02X (ignored)\n", reset_type);
        }
        break;
      }

      case HBF_SYSSETCPY: {  // SETCPY - Set Bank Copy Parameters (0xF4)
        // HBIOS SETCPY interface (per RomWBW API):
        // D = Destination Bank ID
        // E = Source Bank ID
        // HL = Count of bytes to copy
        // Returns: A = Result
        //
        // This function just STORES the parameters for later BNKCPY call.
        // No actual copying is done here.
        // Uses globals: g_bnkcpy_src_bank, g_bnkcpy_dst_bank, g_bnkcpy_count

        g_bnkcpy_dst_bank = cpu->regs.DE.get_high();  // D = dest bank
        g_bnkcpy_src_bank = cpu->regs.DE.get_low();   // E = source bank
        g_bnkcpy_count = cpu->regs.HL.get_pair16();   // HL = count
        break;
      }

      // NOTE: HBF_SYSBCALL was removed - there is no function code 0xF5 called "BCALL" in HBIOS!
      // Function 0xF5 is SYSBNKCPY (handled above at case HBF_SYSBNKCPY).
      // HB_BNKCALL at 0xFFF9 is a proxy entry point called directly, not via RST 08.
      // The following dead code is kept for reference but will never execute:
      if (false) {  // Dead code - was incorrectly labeled as SYSBCALL
        // HBIOS BCALL interface (per RomWBW API):
        // E = Bank to call into
        // DE = Address to call
        // HL, IX, IY are passed through to called routine
        // Returns: A = Result from called routine
        //
        // In real RomWBW, BCALL is used to call into HBIOS ROM routines.
        // Common patterns include SETCPY + BCALL(BNKCPY_routine).
        //
        // Since we don't have the actual HBIOS ROM, we intercept BCALL and
        // handle common patterns directly. When CBIOS calls SETCPY and then
        // BCALL to copy data, we perform the copy ourselves.

        uint8_t target_bank = cpu->regs.DE.get_low();   // E = target bank
        uint16_t call_addr = cpu->regs.DE.get_pair16(); // DE = address to call

        uint8_t bcall_func = cpu->regs.BC.get_high();  // B = function code
        uint8_t bcall_unit = cpu->regs.BC.get_low();   // C = unit
        fprintf(stderr, "[HBIOS BCALL] Call to bank %02X addr %04X (current bank %02X, DE=%04X, B=0x%02X, C=%d)\n",
                target_bank, call_addr, memory->get_current_bank(), cpu->regs.DE.get_pair16(), bcall_func, bcall_unit);

        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);

        // When SETCPY is followed by BCALL, CBIOS expects a bank-to-bank copy
        // to be performed. In real RomWBW, BCALL calls into the HBIOS ROM which
        // then executes the copy using the SETCPY parameters.
        //
        // Since we don't have the actual HBIOS ROM copy routines, we intercept
        // BCALL and perform the copy ourselves whenever SETCPY parameters are set.
        //
        // The pattern is: SETCPY (set params) -> BCALL (execute copy)
        // After the copy, CBIOS may then BCALL to the destination to execute code.

        if (bmem && g_bnkcpy_count > 0) {
          // This is likely a call to HBIOS BNKCPY routine in ROM.
          // SETCPY was called before this to set up the copy parameters.
          // Perform the bank copy directly using parameters from SETCPY.
          // HL = source address, we need to get from CPU
          // The HL register should contain the source address for the copy.

          uint16_t src_addr = cpu->regs.HL.get_pair16();
          // For BCALL->BNKCPY, the dest address might come from IX register
          // since DE is used for the BCALL target address itself.
          // Try IX first, fall back to same address if IX=0
          uint16_t dst_addr = cpu->regs.IX.get_pair16();
          if (dst_addr == 0) {
            dst_addr = src_addr;  // Fall back to same address (relocation pattern)
          }

          // Perform the copy - handle common area addresses properly
          // In RomWBW's memory model:
          //   - 0x0000-0x7FFF: Banked area (uses specified bank)
          //   - 0x8000-0xFFFF: Common area (always mapped to bank 0x8F, offset addr-0x8000)
          for (uint16_t i = 0; i < g_bnkcpy_count; i++) {
            uint16_t src_logical_addr = src_addr + i;
            uint16_t dst_logical_addr = dst_addr + i;

            // Map logical address to physical bank/offset
            uint8_t actual_src_bank;
            uint16_t actual_src_offset;
            if (src_logical_addr >= 0x8000) {
              // Common area - always in bank 0x8F
              actual_src_bank = 0x8F;
              actual_src_offset = src_logical_addr - 0x8000;
            } else {
              actual_src_bank = g_bnkcpy_src_bank;
              actual_src_offset = src_logical_addr;
            }

            uint8_t actual_dst_bank;
            uint16_t actual_dst_offset;
            if (dst_logical_addr >= 0x8000) {
              // Common area - always in bank 0x8F
              actual_dst_bank = 0x8F;
              actual_dst_offset = dst_logical_addr - 0x8000;
            } else {
              actual_dst_bank = g_bnkcpy_dst_bank;
              actual_dst_offset = dst_logical_addr;
            }

            uint8_t byte = bmem->read_bank(actual_src_bank, actual_src_offset);
            bmem->write_bank(actual_dst_bank, actual_dst_offset, byte);
          }

          // Clear the count so we don't repeat the copy
          g_bnkcpy_count = 0;

          // Return success
          result = HBR_SUCCESS;
          break;
        }

        // Check if this is a disk I/O call (function codes 0x10-0x19)
        // When CBIOS uses BCALL to call into HBIOS ROM for disk operations,
        // B contains the DIO function code. We intercept and handle these directly.
        if (bcall_func >= 0x10 && bcall_func <= 0x19) {
          // This is a DIO call via BCALL - handle it directly
          uint8_t dio_func = bcall_func;
          uint8_t dio_unit = bcall_unit;

          if (debug) {
            fprintf(stderr, "[HBIOS BCALL->DIO] Intercepting func=0x%02X unit=%d\n", dio_func, dio_unit);
          }

          // Dispatch to appropriate DIO handler
          // Note: MD units are 0-1, HD units are 2+
          bool is_memdisk = (dio_unit < 2 && md_disks[dio_unit].is_enabled);
          bool is_harddisk = (dio_unit >= 2 && dio_unit < 18 && hb_disks[dio_unit - 2].is_open);

          switch (dio_func) {
            case HBF_DIOSTATUS: {  // 0x10 - Disk status
              if (is_memdisk || is_harddisk) {
                result = HBR_SUCCESS;
                cpu->regs.DE.set_low(1);  // Sectors per track
                cpu->regs.HL.set_pair16(1);  // Heads
              } else {
                result = HBR_NOTREADY;
              }
              break;
            }

            case HBF_DIORESET: {  // 0x11 - Disk reset
              if (is_memdisk) {
                md_disks[dio_unit].current_lba = 0;
                result = HBR_SUCCESS;
              } else if (is_harddisk) {
                hb_disks[dio_unit - 2].current_lba = 0;
                result = HBR_SUCCESS;
              } else {
                result = HBR_NOTREADY;
              }
              break;
            }

            case HBF_DIOSEEK: {  // 0x12 - Disk seek
              uint16_t de_reg = cpu->regs.DE.get_pair16();
              uint16_t hl_reg = cpu->regs.HL.get_pair16();
              uint32_t lba = ((uint32_t)de_reg << 16) | hl_reg;
              if (hl_reg == 0 && de_reg < 0x100) {
                lba = cpu->regs.DE.get_low();
              }

              // DEBUG: Trace BNKCALL DIOSEEK for CP/M 3 boot investigation
              static int bnkcall_dioseek_trace = 0;
              if (bnkcall_dioseek_trace < 50 && is_harddisk) {
                bnkcall_dioseek_trace++;
                fprintf(stderr, "[BNKCALL DIOSEEK #%d] unit=%d LBA=%u\n", bnkcall_dioseek_trace, dio_unit, lba);
              }

              if (is_memdisk) {
                md_disks[dio_unit].current_lba = lba;
                result = HBR_SUCCESS;
              } else if (is_harddisk) {
                hb_disks[dio_unit - 2].current_lba = lba;
                result = HBR_SUCCESS;
              } else {
                result = HBR_NOTREADY;
              }
              break;
            }

            case HBF_DIOREAD: {  // 0x13 - Disk read
              uint16_t buffer_addr = cpu->regs.HL.get_pair16();
              uint8_t buffer_bank = cpu->regs.DE.get_high();
              uint8_t block_count = cpu->regs.DE.get_low();

              // DEBUG: Trace BNKCALL disk reads for CP/M 3 boot investigation
              static int bnkcall_dioread_trace = 0;
              if (bnkcall_dioread_trace < 100 && is_harddisk) {
                bnkcall_dioread_trace++;
                uint32_t lba = hb_disks[dio_unit - 2].current_lba;
                fprintf(stderr, "[BNKCALL DIOREAD #%d] unit=%d LBA=%u bank=0x%02X addr=0x%04X blocks=%d\n",
                        bnkcall_dioread_trace, dio_unit, lba, buffer_bank, buffer_addr, block_count);
              }

              if (!is_memdisk && !is_harddisk) {
                result = HBR_NOTREADY;
                cpu->regs.DE.set_low(0);
                break;
              }

              uint8_t blocks_read = 0;

              if (is_memdisk) {
                // Memory disk read
                MemDiskState& md = md_disks[dio_unit];
                uint32_t sectors_per_bank = 32768 / HBIOS_SECTOR_SIZE;

                for (uint8_t i = 0; i < block_count; i++) {
                  if (md.current_lba >= md.total_sectors()) break;

                  uint32_t bank_offset = md.current_lba / sectors_per_bank;
                  uint32_t sector_in_bank = md.current_lba % sectors_per_bank;
                  uint8_t src_bank = md.start_bank + bank_offset;
                  uint16_t src_offset = sector_in_bank * HBIOS_SECTOR_SIZE;

                  for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
                    uint8_t byte = bmem ? bmem->read_bank(src_bank, src_offset + j) : 0xE5;
                    if (bmem && (buffer_bank & 0x80)) {
                      uint16_t addr = buffer_addr + j;
                      if (addr >= 0x8000) {
                        bmem->write_bank(0x8F, addr - 0x8000, byte);
                      } else {
                        bmem->write_bank(buffer_bank, addr, byte);
                      }
                    } else {
                      memory->store_mem(buffer_addr + j, byte);
                    }
                  }

                  blocks_read++;
                  buffer_addr += HBIOS_SECTOR_SIZE;
                  md.current_lba++;
                }
              } else {
                // Hard disk read
                int hd_unit = dio_unit - 2;
                long offset = (long)hb_disks[hd_unit].current_lba * HBIOS_SECTOR_SIZE;
                fseek(hb_disks[hd_unit].image_file, offset, SEEK_SET);

                for (uint8_t i = 0; i < block_count; i++) {
                  uint8_t sector_buf[HBIOS_SECTOR_SIZE];
                  size_t read = fread(sector_buf, 1, HBIOS_SECTOR_SIZE, hb_disks[hd_unit].image_file);
                  if (read < HBIOS_SECTOR_SIZE) {
                    memset(sector_buf + read, 0xE5, HBIOS_SECTOR_SIZE - read);
                  }

                  for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
                    if (bmem && (buffer_bank & 0x80)) {
                      uint16_t addr = buffer_addr + j;
                      if (addr >= 0x8000) {
                        bmem->write_bank(0x8F, addr - 0x8000, sector_buf[j]);
                      } else {
                        bmem->write_bank(buffer_bank, addr, sector_buf[j]);
                      }
                    } else {
                      memory->store_mem(buffer_addr + j, sector_buf[j]);
                    }
                  }

                  blocks_read++;
                  buffer_addr += HBIOS_SECTOR_SIZE;
                  hb_disks[hd_unit].current_lba++;
                  if (read < HBIOS_SECTOR_SIZE) break;
                }
              }

              cpu->regs.DE.set_low(blocks_read);
              if (debug) {
                fprintf(stderr, "[HBIOS BCALL->DIOREAD] Unit %d: %d blocks read\n", dio_unit, blocks_read);
              }
              break;
            }

            case HBF_DIOWRITE: {  // 0x14 - Disk write
              uint16_t buffer_addr = cpu->regs.HL.get_pair16();
              uint8_t buffer_bank = cpu->regs.DE.get_high();
              uint8_t block_count = cpu->regs.DE.get_low();

              if (!is_memdisk && !is_harddisk) {
                result = HBR_NOTREADY;
                cpu->regs.DE.set_low(0);
                break;
              }

              uint8_t blocks_written = 0;

              if (is_memdisk) {
                // Memory disk write
                MemDiskState& md = md_disks[dio_unit];

                if (md.is_rom) {
                  result = HBR_WRTPROT;
                  cpu->regs.DE.set_low(0);
                  break;
                }

                uint32_t sectors_per_bank = 32768 / HBIOS_SECTOR_SIZE;

                for (uint8_t i = 0; i < block_count; i++) {
                  if (md.current_lba >= md.total_sectors()) break;

                  uint32_t bank_offset = md.current_lba / sectors_per_bank;
                  uint32_t sector_in_bank = md.current_lba % sectors_per_bank;
                  uint8_t dst_bank = md.start_bank + bank_offset;
                  uint16_t dst_offset = sector_in_bank * HBIOS_SECTOR_SIZE;

                  for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
                    uint8_t byte;
                    if (bmem && (buffer_bank & 0x80)) {
                      uint16_t addr = buffer_addr + j;
                      if (addr >= 0x8000) {
                        byte = bmem->read_bank(0x8F, addr - 0x8000);
                      } else {
                        byte = bmem->read_bank(buffer_bank, addr);
                      }
                    } else {
                      byte = memory->fetch_mem(buffer_addr + j, false);
                    }

                    if (bmem) {
                      bmem->write_bank(dst_bank, dst_offset + j, byte);
                    }
                  }

                  blocks_written++;
                  buffer_addr += HBIOS_SECTOR_SIZE;
                  md.current_lba++;
                }
              } else {
                // Hard disk write
                int hd_unit = dio_unit - 2;
                long offset = (long)hb_disks[hd_unit].current_lba * HBIOS_SECTOR_SIZE;
                fseek(hb_disks[hd_unit].image_file, offset, SEEK_SET);

                for (uint8_t i = 0; i < block_count; i++) {
                  uint8_t sector_buf[HBIOS_SECTOR_SIZE];

                  for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
                    if (bmem && (buffer_bank & 0x80)) {
                      uint16_t addr = buffer_addr + j;
                      if (addr >= 0x8000) {
                        sector_buf[j] = bmem->read_bank(0x8F, addr - 0x8000);
                      } else {
                        sector_buf[j] = bmem->read_bank(buffer_bank, addr);
                      }
                    } else {
                      sector_buf[j] = memory->fetch_mem(buffer_addr + j, false);
                    }
                  }

                  size_t written = fwrite(sector_buf, 1, HBIOS_SECTOR_SIZE, hb_disks[hd_unit].image_file);
                  if (written < HBIOS_SECTOR_SIZE) break;

                  blocks_written++;
                  buffer_addr += HBIOS_SECTOR_SIZE;
                  hb_disks[hd_unit].current_lba++;
                }

                fflush(hb_disks[hd_unit].image_file);
              }

              cpu->regs.DE.set_low(blocks_written);
              if (debug) {
                fprintf(stderr, "[HBIOS BCALL->DIOWRITE] Unit %d: %d blocks written\n", dio_unit, blocks_written);
              }
              break;
            }

            default:
              // HALT on unimplemented DIO functions
              fprintf(stderr, "\n*** FATAL: Unimplemented DIO function in BCALL ***\n");
              fprintf(stderr, "  DIO Function: 0x%02X\n", dio_func);
              fprintf(stderr, "  Unit: %d\n", dio_unit);
              fprintf(stderr, "  Call addr: 0x%04X\n", call_addr);
              exit(1);
          }
          break;  // Exit BCALL handler after DIO dispatch
        }

        // For common area addresses (>= 0x8000), try to execute the call
        if (bmem && call_addr >= 0x8000) {
          // Skip normal return handling for BCALL - set PC directly
          cpu->regs.PC.set_pair16(call_addr);
          return true;  // Exit handle_hbios_call without normal return handling
        }

        // For banked area or if not banked_mem, just return success
      }  // end if(false) dead code block

      case HBF_EXTSLICE: {  // Calculate disk slice (0xE0)
        // EXTSLICE - Get extended disk media information and slice offset
        // Entry: B=0xE0, D=disk unit, E=slice number
        // Exit:  A=status, B=device attributes, C=media ID, DE:HL=LBA offset
        //
        // Note: For EXTSLICE, unit is in D register (not C like most functions)
        uint8_t disk_unit = cpu->regs.DE.get_high();  // D = disk unit
        uint8_t slice = cpu->regs.DE.get_low();       // E = slice number

        // Translate unit number like we do for other disk functions
        uint8_t mapped_unit = disk_unit;
        if (disk_unit >= 0xC0 && disk_unit <= 0xCF) {
          mapped_unit = 1;  // Map to MD1 (ROM disk)
        } else if (disk_unit >= 0x90 && disk_unit <= 0x9F) {
          mapped_unit = 2 + (disk_unit & 0x0F);  // Map to HD
        } else if (disk_unit >= 0x80 && disk_unit <= 0x8F) {
          mapped_unit = (disk_unit & 0x0F) < 2 ? (disk_unit & 0x0F) : 1;
        }

        // Determine device attributes and media ID based on mapped unit
        uint8_t dev_attrs = 0;
        uint8_t media_id = 0;
        uint32_t slice_lba = 0;

        if (mapped_unit < 2) {
          // Memory disk (MD0=RAM, MD1=ROM)
          // IMPORTANT: Bit 7 of dev_attrs controls CHS vs LBA mode in CBIOS:
          //   - Bit 7 set: CBIOS uses CHS mode (divides track by 2 for head)
          //   - Bit 7 clear: CBIOS uses LBA mode (track*16 + sector)
          // Memory disks need LBA mode, so bit 7 must be CLEAR.
          if (mapped_unit == 0) {
            dev_attrs = 0x00;  // LBA mode (bit 7 clear)
            media_id = 0x02;   // MID_MDRAM
          } else {
            dev_attrs = 0x00;  // LBA mode (bit 7 clear)
            media_id = 0x01;   // MID_MDROM
          }
          // Memory disks don't have slices
          slice_lba = 0;
        } else {
          // Hard disk - detect partition format if not yet probed
          dev_attrs = 0x00;    // LBA mode (bit 7 clear)
          media_id = 0x04;     // MID_HD (default, will change to MID_HDNEW if hd1k)

          int hd_unit = mapped_unit - 2;
          if (hd_unit >= 0 && hd_unit < 16 && hb_disks[hd_unit].is_open) {
            // Probe MBR if not yet done
            if (!hb_disks[hd_unit].partition_probed) {
              hb_disks[hd_unit].partition_probed = true;
              hb_disks[hd_unit].partition_base_lba = 0;
              hb_disks[hd_unit].slice_size = 16640;  // Default: hd512 format
              hb_disks[hd_unit].is_hd1k = false;

              // Read MBR (sector 0)
              uint8_t mbr[512];
              long saved_pos = ftell(hb_disks[hd_unit].image_file);
              fseek(hb_disks[hd_unit].image_file, 0, SEEK_SET);
              size_t read = fread(mbr, 1, 512, hb_disks[hd_unit].image_file);

              // Also get file size to detect single-slice hd1k images
              fseek(hb_disks[hd_unit].image_file, 0, SEEK_END);
              long file_size = ftell(hb_disks[hd_unit].image_file);
              fseek(hb_disks[hd_unit].image_file, saved_pos, SEEK_SET);

              bool detected_format = false;
              if (read == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
                // Valid MBR signature - check partition table for type 0x2E (RomWBW)
                for (int p = 0; p < 4; p++) {
                  int offset = 0x1BE + (p * 16);
                  uint8_t ptype = mbr[offset + 4];
                  if (ptype == 0x2E) {
                    // Found RomWBW partition (hd1k format)
                    // LBA start is at offset 8-11 (little endian)
                    uint32_t part_lba = mbr[offset + 8] |
                                        (mbr[offset + 9] << 8) |
                                        (mbr[offset + 10] << 16) |
                                        (mbr[offset + 11] << 24);
                    hb_disks[hd_unit].partition_base_lba = part_lba;
                    hb_disks[hd_unit].slice_size = 16384;  // hd1k format
                    hb_disks[hd_unit].is_hd1k = true;
                    detected_format = true;
                    if (debug) {
                      fprintf(stderr, "[HBIOS EXTSLICE] Detected hd1k format (0x2E partition), LBA %u\n", part_lba);
                    }
                    break;
                  }
                }
              }

              // If no 0x2E partition found, check if single-slice hd1k image (exactly 8MB)
              if (!detected_format && file_size == 8388608) {
                // 8MB = exactly one hd1k slice - assume hd1k format
                hb_disks[hd_unit].partition_base_lba = 0;
                hb_disks[hd_unit].slice_size = 16384;
                hb_disks[hd_unit].is_hd1k = true;
                detected_format = true;
                if (debug) {
                  fprintf(stderr, "[HBIOS EXTSLICE] Detected hd1k format (8MB single slice)\n");
                }
              }

              if (!detected_format && debug) {
                fprintf(stderr, "[HBIOS EXTSLICE] Using hd512 format (file size=%ld)\n", file_size);
              }
            }

            // Calculate slice LBA offset
            slice_lba = hb_disks[hd_unit].partition_base_lba +
                        ((uint32_t)slice * hb_disks[hd_unit].slice_size);

            // Set media ID based on detected format
            if (hb_disks[hd_unit].is_hd1k) {
              media_id = 0x0A;  // MID_HDNEW (hd1k format with 1024 dir entries)
            }
          }
        }

        // Set return values
        cpu->regs.BC.set_high(dev_attrs);  // B = device attributes
        cpu->regs.BC.set_low(media_id);    // C = media ID
        // DE:HL = LBA offset (32-bit: DE=high16, HL=low16)
        cpu->regs.DE.set_pair16((slice_lba >> 16) & 0xFFFF);
        cpu->regs.HL.set_pair16(slice_lba & 0xFFFF);

        if (debug) {
          fprintf(stderr, "[HBIOS EXTSLICE] Unit %d (mapped=%d) slice %d: attrs=0x%02X media=0x%02X LBA=%u\n",
                  disk_unit, mapped_unit, slice, dev_attrs, media_id, slice_lba);
        }
        break;
      }

      case HBF_DIOSEEK: {  // Disk seek
        // Input: BC=Function/Unit, DE:HL=LBA (32-bit: DE=high16, HL=low16)
        // CBIOS sends DE:HL where bit 31 (high bit of D) is LBA mode flag.
        // Actual LBA is in lower 31 bits.
        uint16_t de_reg = cpu->regs.DE.get_pair16();
        uint16_t hl_reg = cpu->regs.HL.get_pair16();

        // Standard HBIOS convention: DE:HL = 32-bit value
        // Bit 31 (0x80 in high byte of DE) = LBA mode flag, mask it off
        uint32_t lba = (((uint32_t)(de_reg & 0x7FFF) << 16) | hl_reg);

        // DEBUG: Trace DIOSEEK calls
        static int dioseek_trace = 0;
        if (dioseek_trace < 50 && unit >= 2) {
          dioseek_trace++;
          fprintf(stderr, "[DIOSEEK HD #%d] unit=%d LBA=%u\n", dioseek_trace, unit, lba);
        }

        // Handle memory disks (units 0-1) vs hard disks (units 2+)
        if (unit < 2 && md_disks[unit].is_enabled) {
          md_disks[unit].current_lba = lba;
        } else if (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open) {
          hb_disks[unit - 2].current_lba = lba;
        } else {
          result = HBR_FAILED;
        }
        break;
      }

      case HBF_DIOREAD: {  // Disk read
        // Input: BC=Function/Unit, HL=Buffer Address, D=Buffer Bank (0x80=use current), E=Block Count
        // Output: A=Result, E=Blocks Read
        uint16_t buffer_addr = cpu->regs.HL.get_pair16();
        uint8_t buffer_bank = cpu->regs.DE.get_high();
        uint8_t block_count = cpu->regs.DE.get_low();

        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);

        // Check if this is a memory disk (units 0-1) or hard disk (units 2+)
        bool is_memdisk = (unit < 2 && md_disks[unit].is_enabled);
        bool is_harddisk = (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open);

        // DEBUG: Trace disk reads for CP/M 3 boot investigation
        static int dioread_trace_count = 0;
        constexpr int DIOREAD_TRACE_LIMIT = 100;  // Trace first 100 reads
        if (DIOREAD_TRACE_LIMIT > 0 && is_harddisk && dioread_trace_count < DIOREAD_TRACE_LIMIT) {
          dioread_trace_count++;
          uint32_t lba = hb_disks[unit - 2].current_lba;
          fprintf(stderr, "[DIOREAD HD #%d] unit=%d LBA=%u bank=0x%02X addr=0x%04X blocks=%d\n",
                  dioread_trace_count, unit, lba, buffer_bank, buffer_addr, block_count);
        }

        if (!is_memdisk && !is_harddisk) {
          result = HBR_FAILED;
          cpu->regs.DE.set_low(0);  // 0 blocks read
          break;
        }

        uint8_t blocks_read = 0;

        if (is_memdisk) {
          // Memory disk read - read from bank memory
          MemDiskState& md = md_disks[unit];
          uint32_t sectors_per_bank = 32768 / HBIOS_SECTOR_SIZE;  // 64 sectors per 32KB bank

          for (uint8_t i = 0; i < block_count; i++) {
            if (md.current_lba >= md.total_sectors()) {
              break;  // Past end of disk
            }

            // Calculate source bank and offset within bank
            uint32_t bank_offset = md.current_lba / sectors_per_bank;
            uint32_t sector_in_bank = md.current_lba % sectors_per_bank;
            uint8_t src_bank = md.start_bank + bank_offset;
            uint16_t src_offset = sector_in_bank * HBIOS_SECTOR_SIZE;

            // DEBUG: Trace ROM disk reads (first few bytes)
            if (DIOREAD_TRACE_LIMIT > 0 && unit == 1 && dioread_trace_count <= DIOREAD_TRACE_LIMIT && bmem) {
              fprintf(stderr, "  LBA %u -> src_bank=0x%02X offset=0x%04X  first bytes: %02X %02X %02X %02X\n",
                      md.current_lba, src_bank, src_offset,
                      bmem->read_bank(src_bank, src_offset),
                      bmem->read_bank(src_bank, src_offset+1),
                      bmem->read_bank(src_bank, src_offset+2),
                      bmem->read_bank(src_bank, src_offset+3));
            }

            // Read 512 bytes from source bank to destination
            for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
              uint8_t byte = bmem ? bmem->read_bank(src_bank, src_offset + j) : 0xE5;

              // Write to destination buffer
              if (bmem && (buffer_bank & 0x80)) {
                uint16_t addr = buffer_addr + j;
                if (addr >= 0x8000) {
                  bmem->write_bank(0x8F, addr - 0x8000, byte);
                } else {
                  bmem->write_bank(buffer_bank, addr, byte);
                }
              } else {
                memory->store_mem(buffer_addr + j, byte);
              }
            }

            blocks_read++;
            buffer_addr += HBIOS_SECTOR_SIZE;
            md.current_lba++;
          }

          if (debug) {
            fprintf(stderr, "[HBIOS MD READ] Unit %d: %d blocks from LBA 0x%08X\n",
                    unit, blocks_read, md.current_lba - blocks_read);
          }
        } else {
          // Hard disk read - read from file
          int hd_unit = unit - 2;

          // Seek to LBA position
          long offset = (long)hb_disks[hd_unit].current_lba * HBIOS_SECTOR_SIZE;
          fseek(hb_disks[hd_unit].image_file, offset, SEEK_SET);

          for (uint8_t i = 0; i < block_count; i++) {
            // Read sector into temporary buffer
            uint8_t sector_buf[HBIOS_SECTOR_SIZE];
            size_t read = fread(sector_buf, 1, HBIOS_SECTOR_SIZE, hb_disks[hd_unit].image_file);
            if (read < HBIOS_SECTOR_SIZE) {
              // Fill rest with E5 (CP/M empty marker)
              memset(sector_buf + read, 0xE5, HBIOS_SECTOR_SIZE - read);
            }

            // Write to memory
            for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
              if (bmem && (buffer_bank & 0x80)) {
                uint16_t addr = buffer_addr + j;
                if (addr >= 0x8000) {
                  bmem->write_bank(0x8F, addr - 0x8000, sector_buf[j]);
                } else {
                  bmem->write_bank(buffer_bank, addr, sector_buf[j]);
                }
              } else {
                memory->store_mem(buffer_addr + j, sector_buf[j]);
              }
            }

            blocks_read++;
            buffer_addr += HBIOS_SECTOR_SIZE;
            hb_disks[hd_unit].current_lba++;

            if (read < HBIOS_SECTOR_SIZE) break;  // EOF
          }

          if (debug) {
            fprintf(stderr, "[HBIOS HD READ] Unit %d: %d blocks from LBA 0x%08X\n",
                    unit, blocks_read, hb_disks[hd_unit].current_lba - blocks_read);
          }
        }

        cpu->regs.DE.set_low(blocks_read);
        break;
      }

      case HBF_DIOWRITE: {  // Disk write
        // Input: BC=Function/Unit, HL=Buffer Address, D=Buffer Bank (0x80=use current), E=Block Count
        // Output: A=Result, E=Blocks Written
        uint16_t buffer_addr = cpu->regs.HL.get_pair16();
        uint8_t buffer_bank = cpu->regs.DE.get_high();
        uint8_t block_count = cpu->regs.DE.get_low();

        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);

        // Check if this is a memory disk (units 0-1) or hard disk (units 2+)
        bool is_memdisk = (unit < 2 && md_disks[unit].is_enabled);
        bool is_harddisk = (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open);

        if (!is_memdisk && !is_harddisk) {
          result = HBR_FAILED;
          cpu->regs.DE.set_low(0);  // 0 blocks written
          break;
        }

        uint8_t blocks_written = 0;

        if (is_memdisk) {
          // Memory disk write - write to bank memory
          MemDiskState& md = md_disks[unit];

          // Check if ROM disk (read-only)
          if (md.is_rom) {
            result = HBR_WRTPROT;  // Write protected
            cpu->regs.DE.set_low(0);
            if (debug) {
              fprintf(stderr, "[HBIOS MD WRITE] Unit %d: ROM disk is write protected\n", unit);
            }
            break;
          }

          uint32_t sectors_per_bank = 32768 / HBIOS_SECTOR_SIZE;  // 64 sectors per 32KB bank

          for (uint8_t i = 0; i < block_count; i++) {
            if (md.current_lba >= md.total_sectors()) {
              break;  // Past end of disk
            }

            // Calculate destination bank and offset within bank
            uint32_t bank_offset = md.current_lba / sectors_per_bank;
            uint32_t sector_in_bank = md.current_lba % sectors_per_bank;
            uint8_t dst_bank = md.start_bank + bank_offset;
            uint16_t dst_offset = sector_in_bank * HBIOS_SECTOR_SIZE;

            // Write 512 bytes from source buffer to destination bank
            for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
              uint8_t byte;
              if (bmem && (buffer_bank & 0x80)) {
                uint16_t addr = buffer_addr + j;
                if (addr >= 0x8000) {
                  byte = bmem->read_bank(0x8F, addr - 0x8000);
                } else {
                  byte = bmem->read_bank(buffer_bank, addr);
                }
              } else {
                byte = memory->fetch_mem(buffer_addr + j);
              }

              if (bmem) {
                bmem->write_bank(dst_bank, dst_offset + j, byte);
              }
            }

            blocks_written++;
            buffer_addr += HBIOS_SECTOR_SIZE;
            md.current_lba++;
          }

          if (debug) {
            fprintf(stderr, "[HBIOS MD WRITE] Unit %d: %d blocks to LBA 0x%08X\n",
                    unit, blocks_written, md.current_lba - blocks_written);
          }
        } else {
          // Hard disk write - write to file
          int hd_unit = unit - 2;

          // Seek to LBA position
          long offset = (long)hb_disks[hd_unit].current_lba * HBIOS_SECTOR_SIZE;
          fseek(hb_disks[hd_unit].image_file, offset, SEEK_SET);

          for (uint8_t i = 0; i < block_count; i++) {
            // Read from memory into temporary buffer
            uint8_t sector_buf[HBIOS_SECTOR_SIZE];

            for (size_t j = 0; j < HBIOS_SECTOR_SIZE; j++) {
              if (bmem && (buffer_bank & 0x80)) {
                uint16_t addr = buffer_addr + j;
                if (addr >= 0x8000) {
                  sector_buf[j] = bmem->read_bank(0x8F, addr - 0x8000);
                } else {
                  sector_buf[j] = bmem->read_bank(buffer_bank, addr);
                }
              } else {
                sector_buf[j] = memory->fetch_mem(buffer_addr + j);
              }
            }

            // Write sector to disk
            size_t written = fwrite(sector_buf, 1, HBIOS_SECTOR_SIZE, hb_disks[hd_unit].image_file);
            if (written < HBIOS_SECTOR_SIZE) {
              result = HBR_FAILED;
              break;
            }

            blocks_written++;
            buffer_addr += HBIOS_SECTOR_SIZE;
            hb_disks[hd_unit].current_lba++;
          }

          fflush(hb_disks[hd_unit].image_file);

          if (debug) {
            fprintf(stderr, "[HBIOS HD WRITE] Unit %d: %d blocks to LBA 0x%08X\n",
                    unit, blocks_written, hb_disks[hd_unit].current_lba - blocks_written);
          }
        }

        cpu->regs.DE.set_low(blocks_written);
        break;
      }

      case HBF_DIOSTATUS: {
        // Return disk status - just return success if unit exists
        // For MD units: check md_disks, for HD: check hb_disks
        // Also handle high-numbered units from RomWBW platform configurations
        bool unit_exists = false;
        uint8_t mapped_unit = unit;

        // Translate RomWBW encoded unit numbers to our emulated units
        // Unit encoding: high nibble = device class, low nibble = device number
        // 0x00-0x0F: Memory disks (MD) - map to md_disks
        // 0x90-0x9F: HDSK (SIMH host disks) - map to hb_disks
        // 0xC0-0xCF: Unknown class (seen in some ROMs) - map to md_disks
        if (unit >= 0xC0 && unit <= 0xCF) {
          // Map 0xC? units to MD1 (ROM disk) - these seem to be boot-related
          mapped_unit = 1;  // MD1
          if (debug) {
            fprintf(stderr, "[HBIOS DIOSTATUS] Mapping unit 0x%02X to MD1\n", unit);
          }
        } else if (unit >= 0x90 && unit <= 0x9F) {
          // HDSK units - map to hb_disks (offset by 2 from unit 2+)
          mapped_unit = 2 + (unit & 0x0F);
          if (debug) {
            fprintf(stderr, "[HBIOS DIOSTATUS] Mapping unit 0x%02X to HD%d\n", unit, unit & 0x0F);
          }
        } else if (unit >= 0x80 && unit <= 0x8F) {
          // Possible MD units encoded differently
          mapped_unit = (unit & 0x0F) < 2 ? (unit & 0x0F) : 1;
          if (debug) {
            fprintf(stderr, "[HBIOS DIOSTATUS] Mapping unit 0x%02X to MD%d\n", unit, mapped_unit);
          }
        }

        if (mapped_unit < 2 && md_disks[mapped_unit].is_enabled) {
          unit_exists = true;
        } else if (mapped_unit >= 2 && mapped_unit < 18 && hb_disks[mapped_unit - 2].is_open) {
          unit_exists = true;
        }
        if (!unit_exists) {
          result = HBR_NOTREADY;
        }
        if (debug) {
          fprintf(stderr, "[HBIOS DIOSTATUS] Unit %d (mapped=%d): %s\n", unit, mapped_unit, unit_exists ? "OK" : "NOT READY");
        }
        break;
      }

      case HBF_DIORESET: {
        // Reset disk - reset LBA position
        if (unit < 2 && md_disks[unit].is_enabled) {
          md_disks[unit].current_lba = 0;
        } else if (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open) {
          hb_disks[unit - 2].current_lba = 0;
        }
        if (debug) {
          fprintf(stderr, "[HBIOS DIORESET] Unit %d\n", unit);
        }
        break;
      }

      case HBF_DIODEVICE: {
        // Get device info for unit
        // Returns: D=device type, E=device number within type, C=attributes
        // Device types: 0x00=MD (memory disk), 0x03=IDE, 0x06=SD, 0x09=HDSK
        // Attributes: bit 5=high capacity, bit 6=removable
        uint8_t dev_type = 0xFF;  // Default to "no device"
        uint8_t dev_num = 0xFF;
        uint8_t dev_attr = 0x00;  // Device attributes

        if (unit < 2 && md_disks[unit].is_enabled) {
          // Memory disk (MD0=RAM, MD1=ROM)
          dev_type = 0x00;  // DIODEV_MD
          dev_num = unit;   // 0 or 1
          dev_attr = 0x00;  // Not high capacity, not removable
        } else if (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open) {
          // Hard disk
          dev_type = 0x09;  // DIODEV_HDSK
          dev_num = unit - 2;
          dev_attr = 0x20;  // Bit 5 = high capacity (enables multiple slices)
        } else {
          result = HBR_NOTREADY;
        }

        cpu->regs.DE.set_high(dev_type);
        cpu->regs.DE.set_low(dev_num);
        cpu->regs.BC.set_low(dev_attr);  // C = device attributes
        if (debug) {
          fprintf(stderr, "[HBIOS DIODEVICE] Unit %d: type=0x%02X num=%d attr=0x%02X result=%d\n",
                  unit, dev_type, dev_num, dev_attr, result);
        }
        break;
      }

      case HBF_DIOMEDIA: {
        // Get media info - return media ID
        // For MD: 0x01=RAM, 0x02=ROM
        // For HD: 0x20=hard disk
        uint8_t media_id = 0;

        if (unit < 2 && md_disks[unit].is_enabled) {
          media_id = md_disks[unit].is_rom ? 0x02 : 0x01;  // ROM or RAM
        } else if (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open) {
          media_id = 0x20;  // Hard disk
        } else {
          result = HBR_NOTREADY;
        }

        cpu->regs.DE.set_low(media_id);
        if (debug) {
          fprintf(stderr, "[HBIOS DIOMEDIA] Unit %d: media=0x%02X\n", unit, media_id);
        }
        break;
      }

      case HBF_DIOCAP: {  // 0x1A - Get disk capacity
        // Returns: DE:HL = total sectors (32-bit)
        uint32_t total_sectors = 0;

        if (unit < 2 && md_disks[unit].is_enabled) {
          // Memory disk: sectors = banks * 64 (32KB/512 = 64 sectors per bank)
          total_sectors = md_disks[unit].num_banks * 64;
        } else if (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open) {
          // Hard disk: get file size
          FILE* f = hb_disks[unit - 2].image_file;
          if (f) {
            long cur_pos = ftell(f);
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, cur_pos, SEEK_SET);  // Restore position
            total_sectors = file_size / 512;
          }
        } else {
          result = HBR_NOTREADY;
        }

        cpu->regs.DE.set_pair16((total_sectors >> 16) & 0xFFFF);  // High word
        cpu->regs.HL.set_pair16(total_sectors & 0xFFFF);          // Low word
        if (debug) {
          fprintf(stderr, "[HBIOS DIOCAP] Unit %d: %u sectors (%u MB)\n",
                  unit, total_sectors, total_sectors / 2048);
        }
        break;
      }

      case HBF_DIOGEOM: {  // 0x1B - Get disk geometry
        // Returns: C=sectors/track, D=heads, E=tracks
        // For LBA-only disks, return simulated CHS geometry
        // hd1k format: 16 sectors/track, 16 heads, 64 cylinders per slice
        cpu->regs.BC.set_low(16);    // C = 16 sectors per track
        cpu->regs.DE.set_high(16);   // D = 16 heads
        cpu->regs.DE.set_low(64);    // E = 64 tracks (cylinders)
        if (debug) {
          fprintf(stderr, "[HBIOS DIOGEOM] Unit %d: 16/16/64 (s/h/t)\n", unit);
        }
        break;
      }

      case HBF_SYSGET: {  // Get system info (subfunctions in C register)
        // Subfunction in C register (which is 'unit' in our parsing)
        uint8_t subfunc = unit;

        switch (subfunc) {
          case 0x00:  // CIOCNT - Character device count
            cpu->regs.DE.set_low(1);  // 1 serial device
            break;
          case 0x10:  // DIOCNT - Disk device count
            // Count all disks: memory disks (MD) + hard disks (HD)
            {
              uint8_t disk_count = 0;
              // Count memory disks (MD0, MD1)
              for (int i = 0; i < 2; i++) {
                if (md_disks[i].is_enabled) disk_count++;
              }
              // Count hard disks
              for (int i = 0; i < 16; i++) {
                if (hb_disks[i].is_open) disk_count++;
              }
              cpu->regs.DE.set_low(disk_count);
              if (debug) {
                fprintf(stderr, "[HBIOS DIOCNT] Returning %d disks (MD:%d,%d HD:%d)\n",
                        disk_count,
                        md_disks[0].is_enabled ? 1 : 0,
                        md_disks[1].is_enabled ? 1 : 0,
                        hb_disks[0].is_open ? 1 : 0);
              }
            }
            break;
          case 0x20:  // RTCCNT - RTC device count
            cpu->regs.DE.set_low(0);  // No RTC devices
            break;
          case 0x30:  // EMUCNT - EMU device count (or reserved)
            cpu->regs.DE.set_low(0);  // No EMU devices
            break;
          case 0x40:  // VDACNT - Video device count
            cpu->regs.DE.set_low(0);  // No VDA devices
            break;
          case 0x50:  // SNDCNT - Sound device count
            cpu->regs.DE.set_low(0);  // No sound devices
            break;
          case 0xC0:  // SWITCH - Get front panel switches value
            // Return L = switch byte (0 = no switches set)
            cpu->regs.HL.set_low(0x00);  // No switches
            break;
          case 0xD0:  // TIMER - Get 32-bit timer tick value
            // Return DE:HL = tick count (just return 0 for now)
            cpu->regs.DE.set_pair16(0);
            cpu->regs.HL.set_pair16(0);
            break;
          case 0xE0:  // BOOTINFO - Get boot volume and bank info
            // D = boot device/unit, E = boot slice
            cpu->regs.DE.set_high(saved_boot_unit);
            cpu->regs.DE.set_low(saved_boot_slice);
            if (debug) {
              fprintf(stderr, "[SYSGET BOOTINFO] Returning D=%d (unit), E=%d (slice)\n",
                      saved_boot_unit, saved_boot_slice);
            }
            break;
          case 0xF0:  // CPUINFO - CPU info
            cpu->regs.DE.set_pair16(0x0004);  // Z80 @ 4MHz
            cpu->regs.HL.set_pair16(4000);    // 4000 KHz
            break;
          case 0xF1:  // MEMINFO - Memory bank info (NOT the same as SYSVER!)
            cpu->regs.DE.set_high(16);  // 16 ROM banks (512KB/32KB)
            cpu->regs.DE.set_low(16);   // 16 RAM banks (512KB/32KB)
            break;
          case 0xF2:  // BNKINFO - Bank info
            // Return bank IDs (per RomWBW HBIOS API)
            // D = BIOS bank ID
            // E = User/TPA bank ID
            cpu->regs.DE.set_high(0x80);  // BIOS bank (bank 0 of ROM)
            cpu->regs.DE.set_low(0x8E);   // User/TPA bank (RAM bank 14)
            if (debug) {
              fprintf(stderr, "[HBIOS BNKINFO] Returning D=0x80 (BIOS), E=0x8E (User)\n");
            }
            break;
          case 0xF3:  // CPUSPD - Get clock speed & wait states
            // H = wait states, L = CPU speed divisor
            cpu->regs.HL.set_high(0);  // No wait states
            cpu->regs.HL.set_low(1);   // Speed divisor 1 (full speed)
            break;
          case 0xF4:  // PANEL - Get front panel switches value
            // Returns L = panel switch byte (0 if no panel)
            cpu->regs.HL.set_low(0);  // No front panel
            break;
          case 0xF5:  // APPBNKS - Get app bank information
            // Returns: D = first app bank ID, E = app bank count
            // Read from HCB at 0x1E0-0x1E1 in bank 0x80
            {
              banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
              if (bmem) {
                uint8_t app_bank_start = bmem->read_bank(0x80, 0x1E0);  // CB_BIDAPP0
                uint8_t app_bank_count = bmem->read_bank(0x80, 0x1E1);  // CB_APP_BNKS
                cpu->regs.DE.set_high(app_bank_start);
                cpu->regs.DE.set_low(app_bank_count);
                if (debug) {
                  fprintf(stderr, "[HBIOS APPBNKS] first=0x%02X count=%d\n", app_bank_start, app_bank_count);
                }
              } else {
                cpu->regs.DE.set_pair16(0);
              }
            }
            break;
          case 0xFD:  // DEVLIST - List devices (custom for emulator boot menu)
            {
              // Print device list to console
              int unit_num = 0;

              // First, list memory disks (MD0=RAM, MD1=ROM)
              for (int i = 0; i < 2; i++) {
                if (md_disks[i].is_enabled) {
                  char line[64];
                  const char* type_str = md_disks[i].is_rom ? "ROM Disk" : "RAM Disk";
                  uint32_t size_kb = (uint32_t)md_disks[i].num_banks * 32;
                  snprintf(line, sizeof(line), " %2d    MD%d:     %s (%uKB)\r\n",
                           unit_num, i, type_str, size_kb);
                  for (const char* p = line; *p; p++) {
                    emu_console_write_char(*p);
                  }
                  unit_num++;
                }
              }

              // Then list hard disks
              for (int i = 0; i < 16; i++) {
                if (hb_disks[i].is_open) {
                  char line[64];
                  snprintf(line, sizeof(line), " %2d    HD%d:     Hard Disk\r\n", unit_num, i);
                  for (const char* p = line; *p; p++) {
                    emu_console_write_char(*p);
                  }
                  unit_num++;
                }
              }

              // Then list ROM applications
              if (num_rom_apps > 0) {
                const char* hdr = "\r\nROM Applications:\r\n";
                for (const char* p = hdr; *p; p++) emu_console_write_char(*p);
                for (int i = 0; i < num_rom_apps; i++) {
                  if (rom_apps[i].is_loaded) {
                    char line[64];
                    snprintf(line, sizeof(line), "  %c    %s\r\n", rom_apps[i].key, rom_apps[i].name.c_str());
                    for (const char* p = line; *p; p++) {
                      emu_console_write_char(*p);
                    }
                  }
                }
              }
            }
            break;
          default:
            fprintf(stderr, "\n*** FATAL: Unimplemented SYSGET subfunction ***\n");
            fprintf(stderr, "  Subfunction: 0x%02X\n", subfunc);
            fprintf(stderr, "  Registers: BC=%04X DE=%04X HL=%04X\n",
                    cpu->regs.BC.get_pair16(), cpu->regs.DE.get_pair16(), cpu->regs.HL.get_pair16());
            exit(1);
        }
        break;
      }

      case HBF_SYSSET: {  // Set system info (subfunctions in C register)
        uint8_t subfunc = unit;
        switch (subfunc) {
          case 0xC0:  // SETSWITCH - Set front panel switches
            // L = switch value to set - just ignore
            break;
          case 0xE0:  // SETBOOTINFO - Set boot volume and bank info
            // Per biosldr.z80: D = unit, E = slice, L = bank (always 0)
            // Save unit and slice for SYSGET BOOTINFO
            saved_boot_unit = cpu->regs.DE.get_high();
            saved_boot_slice = cpu->regs.DE.get_low();  // Slice is in E, not L!
            if (debug) {
              fprintf(stderr, "[SYSSET BOOTINFO] device=%d slice=%d bank=0x%02X (saved)\n",
                      saved_boot_unit, saved_boot_slice, cpu->regs.HL.get_low());
            }
            break;
          default:
            fprintf(stderr, "\n*** FATAL: Unimplemented SYSSET subfunction ***\n");
            fprintf(stderr, "  Subfunction: 0x%02X\n", subfunc);
            fprintf(stderr, "  Registers: BC=%04X DE=%04X HL=%04X\n",
                    cpu->regs.BC.get_pair16(), cpu->regs.DE.get_pair16(), cpu->regs.HL.get_pair16());
            exit(1);
        }
        break;
      }

      case HBF_SYSPEEK: {  // PEEK: Read byte from bank D at address HL, return in E
        uint8_t bank = cpu->regs.DE.get_high();
        uint16_t addr = cpu->regs.HL.get_pair16();
        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
        uint8_t byte = 0xFF;
        if (bmem) {
          // For addresses in lower 32KB, read from specified bank
          // For upper 32KB (common area), read directly
          if (addr < 0x8000) {
            byte = bmem->read_bank(bank, addr);
          } else {
            byte = memory->fetch_mem(addr);
          }
        }
        cpu->regs.DE.set_low(byte);  // Return byte in E
        result = HBR_SUCCESS;
        // Debug: verbose SYSPEEK logging
        if (debug) {
          fprintf(stderr, "[SYSPEEK] bank=0x%02X addr=0x%04X -> 0x%02X\n", bank, addr, byte);
        }
        break;
      }

      case HBF_SYSPOKE: {  // POKE: Write byte E to bank D at address HL
        uint8_t bank = cpu->regs.DE.get_high();
        uint8_t byte = cpu->regs.DE.get_low();
        uint16_t addr = cpu->regs.HL.get_pair16();
        banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
        if (bmem) {
          if (addr < 0x8000) {
            bmem->write_bank(bank, addr, byte);
          } else {
            memory->store_mem(addr, byte);
          }
        }
        result = HBR_SUCCESS;
        if (debug) {
          fprintf(stderr, "[HBIOS POKE] bank=0x%02X addr=0x%04X val=0x%02X\n", bank, addr, byte);
        }
        break;
      }

      case HBF_SYSINT: {  // INT: Interrupt management - just return success for now
        result = HBR_SUCCESS;
        break;
      }

      // RTC functions - no real-time clock hardware present
      case HBF_RTCGETTIM:
      case HBF_RTCSETTIM:
      case HBF_RTCGETBYT:
      case HBF_RTCSETBYT:
      case HBF_RTCGETBLK:
      case HBF_RTCSETBLK:
      case HBF_RTCGETALM:
      case HBF_RTCSETALM:
      case HBF_RTCDEVICE:
        result = HBR_NOHW;  // No RTC hardware
        break;

      // VDA functions - no video hardware present
      case HBF_VDAINI:
      case HBF_VDAQRY:
      case HBF_VDARES:
      case HBF_VDADEV:
        result = HBR_NOHW;  // No VDA hardware
        break;

      // DSKY functions - no hardware present
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
        result = HBR_NOHW;  // No DSKY hardware
        break;

      // Host file transfer functions (0xE1-0xE5)
      case HBF_HOST_OPEN_R: {
        // Open host file for reading
        // DE = address of null-terminated path string
        uint16_t path_addr = cpu->regs.DE.get_pair16();
        std::string path;
        for (int i = 0; i < 256; i++) {
          char c = memory->fetch_mem(path_addr + i);
          if (c == 0) break;
          path += c;
        }
        if (host_read_file) fclose(host_read_file);
        host_read_file = fopen(path.c_str(), "rb");
        result = host_read_file ? HBR_SUCCESS : HBR_FAILED;
        if (debug) fprintf(stderr, "[HBIOS] HOST_OPEN_R '%s' -> %s\n",
                          path.c_str(), host_read_file ? "OK" : "FAIL");
        break;
      }

      case HBF_HOST_OPEN_W: {
        // Open host file for writing
        // DE = address of null-terminated path string
        uint16_t path_addr = cpu->regs.DE.get_pair16();
        std::string path;
        for (int i = 0; i < 256; i++) {
          char c = memory->fetch_mem(path_addr + i);
          if (c == 0) break;
          path += c;
        }
        if (host_write_file) fclose(host_write_file);
        host_write_file = fopen(path.c_str(), "wb");
        result = host_write_file ? HBR_SUCCESS : HBR_FAILED;
        if (debug) fprintf(stderr, "[HBIOS] HOST_OPEN_W '%s' -> %s\n",
                          path.c_str(), host_write_file ? "OK" : "FAIL");
        break;
      }

      case HBF_HOST_READ: {
        // Read byte from host file
        // Returns: E = byte read, A = 0 success / 0xFF EOF
        if (host_read_file) {
          int c = fgetc(host_read_file);
          if (c != EOF) {
            cpu->regs.DE.set_low(c);
            result = HBR_SUCCESS;
          } else {
            result = HBR_FAILED;  // EOF
          }
        } else {
          result = HBR_FAILED;
        }
        break;
      }

      case HBF_HOST_WRITE: {
        // Write byte to host file
        // E = byte to write
        if (host_write_file) {
          fputc(cpu->regs.DE.get_low(), host_write_file);
          result = HBR_SUCCESS;
        } else {
          result = HBR_FAILED;
        }
        break;
      }

      case HBF_HOST_CLOSE: {
        // Close host file
        // C = 0 for read file, C = 1 for write file
        uint8_t which = cpu->regs.BC.get_low();
        if (which == 0 && host_read_file) {
          fclose(host_read_file);
          host_read_file = nullptr;
        } else if (which == 1 && host_write_file) {
          fclose(host_write_file);
          host_write_file = nullptr;
        }
        result = HBR_SUCCESS;
        break;
      }

      default:
        // HALT on unimplemented functions so we know exactly what's missing
        fprintf(stderr, "\n*** FATAL: Unimplemented HBIOS function ***\n");
        fprintf(stderr, "  Function: 0x%02X\n", func);
        fprintf(stderr, "  Unit: %d (0x%02X)\n", unit, unit);
        fprintf(stderr, "  Called from PC: 0x%04X\n", pc);
        fprintf(stderr, "  Registers: BC=%04X DE=%04X HL=%04X\n",
                cpu->regs.BC.get_pair16(), cpu->regs.DE.get_pair16(), cpu->regs.HL.get_pair16());
        exit(1);
    }

    // Set result in A register and set flags appropriately
    // HBIOS convention: A=result, Carry flag = error indicator (bit 7 set in A)
    // For status functions, A contains count/value, not error code
    // Error: A has bit 7 set (negative), Carry set
    // Success: A=value, Carry clear, Z reflects A==0
    cpu->regs.AF.set_high(result);

    // Set flags in F register (low byte of AF)
    // F register bits: S Z - H - P/V N C  (bit 7 to bit 0)
    // Z flag: set if A==0
    // C flag: set if A has bit 7 set (error)
    uint8_t flags = cpu->regs.AF.get_low();
    if (result == 0) {
      flags |= 0x40;   // Set Z flag (bit 6)
    } else {
      flags &= ~0x40;  // Clear Z flag (bit 6)
    }
    if (result & 0x80) {
      flags |= 0x01;   // Set C flag for error (bit 7 set in result)
    } else {
      flags &= ~0x01;  // Clear C flag
    }
    cpu->regs.AF.set_low(flags);

    // Check if HL was unexpectedly modified by a CIO call
    uint16_t hl_after = cpu->regs.HL.get_pair16();
    if (func <= 0x0F && hl_before != hl_after) {
      // CIO calls should NOT modify HL - this is a bug!
      fprintf(stderr, "*** BUG: CIO func 0x%02X corrupted HL: 0x%04X -> 0x%04X ***\n",
              func, hl_before, hl_after);
    }

    // Return to caller:
    // For PC-based trapping, RST 08 pushed the return address, so we pop it from the stack.
    // For I/O port dispatch, the Z80 proxy code has its own RET instruction, so skip this.
    if (!skip_synthetic_ret) {
      uint16_t sp = cpu->regs.SP.get_pair16();
      uint16_t ret_addr = memory->fetch_mem(sp) | (memory->fetch_mem(sp + 1) << 8);
      cpu->regs.SP.set_pair16(sp + 2);  // Pop return address
      cpu->regs.PC.set_pair16(ret_addr);

      // Debug: trace CIOIN return (only when debug is enabled)
      if (debug && func == HBF_CIOIN) {
        fprintf(stderr, "[CIOIN return] A=0x%02X E=0x%02X F=0x%02X ret=0x%04X SP=0x%04X\n",
                cpu->regs.AF.get_high(), cpu->regs.DE.get_low(),
                cpu->regs.AF.get_low(), ret_addr, cpu->regs.SP.get_pair16());
        // Show bytes at return address
        fprintf(stderr, "        Code at 0x%04X: %02X %02X %02X %02X %02X\n",
                ret_addr,
                memory->fetch_mem(ret_addr),
                memory->fetch_mem(ret_addr + 1),
                memory->fetch_mem(ret_addr + 2),
                memory->fetch_mem(ret_addr + 3),
                memory->fetch_mem(ret_addr + 4));
        cioin_trace_count = 20;  // Trace next 20 instructions
      }
    }

    return true;
  }


  void print_port_stats() {
    fprintf(stderr, "\n=== I/O Port Statistics ===\n");
    fprintf(stderr, "IN ports accessed:\n");
    for (auto& p : port_in_counts) {
      fprintf(stderr, "  Port 0x%02X: %d times\n", p.first, p.second);
    }
    fprintf(stderr, "OUT ports accessed:\n");
    for (auto& p : port_out_counts) {
      fprintf(stderr, "  Port 0x%02X: %d times\n", p.first, p.second);
    }
  }
};

// z80_with_io port I/O implementations
// These override the qkz80 virtual functions and route I/O through the emulator
void z80_with_io::port_out(qkz80_uint8 port, qkz80_uint8 value) {
  if (emu) {
    emu->handle_out(port, value);
  }
}

qkz80_uint8 z80_with_io::port_in(qkz80_uint8 port) {
  if (emu) {
    return emu->handle_in(port);
  }
  return 0xFF;  // Floating bus
}

void z80_with_io::halt(void) {
  fprintf(stderr, "\n*** HALT instruction at PC=0x%04X ***\n",
          regs.PC.get_pair16());
  if (emu) {
    emu->set_halted(true);
  }
}

void z80_with_io::unimplemented_opcode(qkz80_uint8 opcode, qkz80_uint16 pc) {
  fprintf(stderr, "\n*** Unimplemented opcode 0x%02X at PC=0x%04X ***\n",
          opcode, pc);
  if (emu) {
    emu->set_halted(true);
  }
}

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
  bool romwbw_mode = false;
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
      romwbw_mode = true;
      binary = argv[i] + 9;
    } else if (strcmp(argv[i], "--strict-io") == 0) {
      strict_io_mode = true;
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
  if (romwbw_mode) {
    // RomWBW starts at address 0x0000 in ROM bank 0
    if (!start_addr_set) start_addr = 0x0000;
  } else {
    if (!start_addr_set) start_addr = load_addr;
  }

  // Create memory and CPU
  cpm_mem memory;
  z80_with_io cpu(&memory);

  // RomWBW requires Z80 mode, others use 8080
  if (romwbw_mode) {
    cpu.set_cpu_mode(qkz80::MODE_Z80);
    fprintf(stderr, "CPU mode: Z80\n");
    // Enable banking for RomWBW
    memory.enable_banking();
    memory.set_debug(debug);
    fprintf(stderr, "RomWBW mode: 512KB ROM + 512KB RAM, bank switching enabled\n");

    // Let ROM initialize first - don't install fake HBIOS yet
    // The ROM will set up its own HBIOS proxy in common RAM
    // We'll trap RST 08 (PC=0x0008) but let the ROM code run initially
    fprintf(stderr, "RomWBW will initialize HBIOS proxy, trapping at RST 08\n");
  } else {
    cpu.set_cpu_mode(qkz80::MODE_8080);
    fprintf(stderr, "CPU mode: 8080\n");
  }

  // Create emulator and link CPU to it for port I/O
  AltairEmulator emu(&cpu, &memory, debug, romwbw_mode);
  cpu.emu = &emu;  // Enable port_in/port_out routing through emulator
  emu.set_strict_io_mode(strict_io_mode);

  // Set up HBIOS disk images
  // NOTE: Memory disks are initialized later, after ROM is loaded
  if (romwbw_mode) {
    // Attach any file-backed hard disk images (HBIOS dispatch protocol)
    for (int i = 0; i < 16; i++) {
      if (!hbios_disks[i].empty()) {
        if (!emu.attach_hbios_disk(i, hbios_disks[i])) {
          fprintf(stderr, "Warning: Could not attach disk %d: %s\n", i, hbios_disks[i].c_str());
        } else {
          // Set the slice count for this disk
          emu.set_disk_slice_count(i, hbios_disk_slices[i]);
        }
      }
    }

    // Register ROM applications
    for (const auto& def : rom_app_defs) {
      emu.register_rom_app(def.key, def.name, def.path);
    }

    // Set romldr path if specified
    if (!romldr_path.empty()) {
      emu.set_romldr_path(romldr_path);
    }
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

  if (romwbw_mode) {
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
    emu.init_memory_disks();

    // Enable page zero write tracing for debugging
    if (debug) {
      memory.set_trace_page_zero(true);
    }
  } else {
    // Load into flat 64KB memory
    uint8_t* mem = cpu.get_mem();
    memset(mem, 0, 65536);

    size_t loaded = fread(&mem[load_addr], 1, file_size, fp);
    fclose(fp);

    fprintf(stderr, "Loaded %zu bytes from %s at 0x%04X\n", loaded, binary, load_addr);
    fprintf(stderr, "Starting execution at 0x%04X\n", start_addr);
  }

  // Enable tracing if requested
  if (!trace_file.empty()) {
    memory.enable_tracing(true);
    fprintf(stderr, "Execution tracing enabled, will write to: %s\n", trace_file.c_str());
  }

  // Set PC to start address
  cpu.regs.PC.set_pair16(start_addr);

  // Set SP based on mode
  if (romwbw_mode) {
    // RomWBW ROM initializes SP itself, but start with safe value
    cpu.regs.SP.set_pair16(0x0000);  // Will be set by ROM
  } else {
    cpu.regs.SP.set_pair16(0xFF00);
  }

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
    memory.set_last_pc(pc);  // For debug tracing
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

    // Check for console escape character in input buffer
    if (peek_char == console_escape_char) {
      peek_char = -1;  // Consume the escape char
      console_mode_requested = true;
    }

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
      fprintf(stderr, "\nHLT instruction at 0x%04X after %lld instructions\n",
              pc, instruction_count);
      emu.print_port_stats();
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
    // Debug: trace instructions after CIOIN
    emu.trace_after_cioin(pc, opcode);

    // Execute one instruction (I/O is handled via port_in/port_out virtual functions)
    cpu.execute();
    instruction_count++;
    if (in_step_mode) step_count--;

    // Check if strict I/O mode halted us during port operations
    if (emu.is_halted()) {
      fprintf(stderr, "\nEmulator halted (strict I/O mode) after %lld instructions\n",
              instruction_count);
      emu.print_port_stats();
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
      emu.print_port_stats();
      break;
    }
  }

  fprintf(stderr, "\nTotal instructions executed: %lld\n", instruction_count);
  emu.print_port_stats();

  // Write trace file if tracing was enabled
  if (!trace_file.empty()) {
    memory.write_trace_script(trace_file.c_str(), load_addr);
  }

  return 0;
}
