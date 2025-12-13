/*
 * Altair 8800 Emulator with CP/M 2.2 and RomWBW Support
 *
 * This emulator operates at the hardware level for BASIC, and at the
 * BIOS level for CP/M. Supports:
 * - 88-2SIO serial ports (ports 0x00-0x01 or 0x10-0x11)
 * - Sense switches (port 0xFF) for memory size configuration
 * - CP/M 2.2 with BIOS trapping at F600
 * - RomWBW with banked memory (512KB ROM + 512KB RAM)
 *
 * CP/M mode: Loads cpm22.sys at E000 and traps BIOS calls at F600-F630
 * RomWBW mode: Loads ROM image, enables banking, runs HBIOS natively
 */

// Version info
#define EMU_VERSION "1.1.0"
#define EMU_VERSION_DATE "2025-12-11"

#include "qkz80.h"
#include "romwbw_mem.h"
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

// Extended Z80 CPU with block I/O support
// Block I/O instructions (INI, INIR, OUTI, OUTIR, etc.) need to call back
// to the emulator for port I/O handling
class z80_with_io : public qkz80 {
public:
  AltairEmulator* emu;  // Set after emulator is created

  z80_with_io(qkz80_cpu_mem* memory) : qkz80(memory), emu(nullptr) {}

  // Execute one instruction, handling block I/O specially
  void execute_with_io();
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

// CP/M constants
constexpr uint16_t CPM_LOAD_ADDR = 0xE000;   // Where CP/M CCP/BDOS loads
constexpr uint16_t BIOS_BASE = 0xF600;       // BIOS entry points
constexpr uint16_t TPA_START = 0x0100;       // Transient Program Area
constexpr uint16_t DMA_DEFAULT = 0x0080;     // Default DMA address

// BIOS entry point offsets (relative to BIOS_BASE)
enum BiosEntry {
  BIOS_BOOT    = 0x00,  // Cold boot
  BIOS_WBOOT   = 0x03,  // Warm boot
  BIOS_CONST   = 0x06,  // Console status
  BIOS_CONIN   = 0x09,  // Console input
  BIOS_CONOUT  = 0x0C,  // Console output
  BIOS_LIST    = 0x0F,  // List (printer) output
  BIOS_PUNCH   = 0x12,  // Punch output
  BIOS_READER  = 0x15,  // Reader input
  BIOS_HOME    = 0x18,  // Home disk
  BIOS_SELDSK  = 0x1B,  // Select disk
  BIOS_SETTRK  = 0x1E,  // Set track
  BIOS_SETSEC  = 0x21,  // Set sector
  BIOS_SETDMA  = 0x24,  // Set DMA address
  BIOS_READ    = 0x27,  // Read sector
  BIOS_WRITE   = 0x2A,  // Write sector
  BIOS_PRSTAT  = 0x2D,  // Printer status
  BIOS_SECTRN  = 0x30,  // Sector translate
};

// RomWBW HBIOS constants
constexpr uint16_t HBIOS_BASE = 0xFE00;      // Fake HBIOS entry point
constexpr uint16_t HBIOS_PROXY = 0xFFE0;     // Real HBIOS proxy location (if present)

// HBIOS function codes (passed in B register)
enum HBiosFunc {
  // Serial I/O
  HBF_CIOIN    = 0x00,  // Console input
  HBF_CIOOUT   = 0x01,  // Console output
  HBF_CIOIST   = 0x02,  // Console input status
  HBF_CIOOST   = 0x03,  // Console output status
  HBF_CIOINIT  = 0x04,  // Console init
  HBF_CIOQUERY = 0x05,  // Console query
  HBF_CIODEVICE= 0x06,  // Console device info
  // Disk I/O
  HBF_DIOSTATUS= 0x10,  // Disk status
  HBF_DIORESET = 0x11,  // Disk reset
  HBF_DIOSEEK  = 0x12,  // Disk seek
  HBF_DIOREAD  = 0x13,  // Disk read
  HBF_DIOWRITE = 0x14,  // Disk write
  HBF_DIOVERIFY= 0x15,  // Disk verify
  HBF_DIOFORMAT= 0x16,  // Disk format
  HBF_DIODEVICE= 0x17,  // Disk device info
  HBF_DIOMEDIA = 0x18,  // Disk media info
  HBF_DIODEFMED= 0x19,  // Disk define media
  HBF_DIOCAP   = 0x1A,  // Disk capacity
  HBF_DIOGEOM  = 0x1B,  // Disk geometry
  // Extended Disk
  HBF_EXTSLICE = 0xE0,  // Calculate disk slice (for larger disks)
  // Management
  HBF_SYSRESET = 0xF0,  // System reset
  HBF_SYSVER   = 0xF1,  // Get version (standard HBIOS)
  HBF_SYSSETBNK= 0xF2,  // Set bank
  HBF_SYSGETBNK= 0xF3,  // Get current bank
  HBF_SYSSETCPY= 0xF4,  // Set copy params (source bank, dest bank, count)
  HBF_SYSBNKCPY= 0xF5,  // Bank copy (execute copy with params from SETCPY)
  HBF_SYSALLOC = 0xF6,  // Allocate memory from heap
  HBF_SYSFREE  = 0xF7,  // Free memory
  HBF_SYSGET   = 0xF8,  // Get system info
  HBF_SYSSET   = 0xF9,  // Set system info
  HBF_SYSPEEK  = 0xFA,  // Read byte from bank: D=bank, HL=addr, returns E=byte
  HBF_SYSPOKE  = 0xFB,  // Write byte to bank: D=bank, HL=addr, E=byte
  HBF_SYSINT   = 0xFC,  // Interrupt management
  HBF_SYSBOOT  = 0xFE,  // EMU: Boot from disk (our custom function, avoid conflicts)

  // VDA (Video Display Adapter) functions - 0x20-0x2F
  HBF_VDASTATUS  = 0x20,  // VDA status
  HBF_VDARESET   = 0x21,  // VDA reset
  HBF_VDAQUERY   = 0x22,  // VDA query
  HBF_VDADEVICE  = 0x23,  // VDA device info

  // DSKY (Display/Keypad) functions - 0x30-0x3A
  HBF_DSKYRESET  = 0x30,  // Reset DSKY
  HBF_DSKYSTATUS = 0x31,  // Input status
  HBF_DSKYGETKEY = 0x32,  // Get key
  HBF_DSKYSHOWHEX= 0x33,  // Show hex value
  HBF_DSKYSHOWSEG= 0x34,  // Show segment data
  HBF_DSKYKEYLEDS= 0x35,  // Set key LEDs
  HBF_DSKYSTATLED= 0x36,  // Set status LED
  HBF_DSKYBEEP   = 0x37,  // Beep
  HBF_DSKYDEVICE = 0x38,  // Device info
  HBF_DSKYMESSAGE= 0x39,  // Show message
  HBF_DSKYEVENT  = 0x3A,  // Event update
};

// HBIOS result codes
constexpr uint8_t HBR_SUCCESS  = 0x00;  // Success
constexpr uint8_t HBR_FAILED   = 0xFF;  // Generic failure
constexpr uint8_t HBR_NOTREADY = 0xFE;  // Device not ready
constexpr uint8_t HBR_WRTPROT  = 0xFD;  // Write protected
constexpr uint8_t HBR_NOHW     = 0xF8;  // Hardware not present (-8)

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

// Boot string queue - characters to feed before reading from stdin
static std::string boot_string;
static size_t boot_string_pos = 0;
static bool boot_prompt_seen = false;   // Set when "]: " pattern detected
static bool flush_completed = false;    // Set when flush timeout is reached
static bool boot_string_ready = false;  // Only true when both above are true
static int cioist_zero_count = 0;       // Count consecutive CIOIST returns of 0

// Track recent output to detect boot prompt "Boot [H=Help]: "
static char recent_output[20] = {0};
static int recent_output_pos = 0;

static void track_output_for_boot_prompt(char ch) {
  // Shift buffer and add new character
  if (recent_output_pos < 19) {
    recent_output[recent_output_pos++] = ch;
  } else {
    memmove(recent_output, recent_output + 1, 18);
    recent_output[18] = ch;
  }
  recent_output[recent_output_pos] = '\0';

  // Check if we see "]: " at the end (part of "[H=Help]: ")
  if (!boot_prompt_seen && recent_output_pos >= 3 &&
      recent_output[recent_output_pos-3] == ']' &&
      recent_output[recent_output_pos-2] == ':' &&
      recent_output[recent_output_pos-1] == ' ') {
    boot_prompt_seen = true;
  }
}

// Called when CIOIST returns 0 (no input available)
static void check_boot_string_activation() {
  if (!boot_string_ready && boot_prompt_seen) {
    cioist_zero_count++;
    // The flush routine at 0x826C needs multiple CIOIST=0 returns before it times out.
    // Looking at the timeout limit at (0x8F17)=0x01, it needs ~3 iterations with
    // units 0, 1, then timeout. Wait for a few more to be safe.
    // After the flush, the main menu loop at 0x8255 will call CIOIST with the
    // console unit - that's when we should activate.
    if (cioist_zero_count >= 5) {  // Wait for flush to fully timeout
      flush_completed = true;
      boot_string_ready = true;
    }
  }
}

// Forward declaration
static bool check_ctrl_c_exit(int ch);

// Check if input is available (not just EOF) by peeking
static bool stdin_has_data() {
  // Check boot string first (only if boot prompt has been shown)
  if (boot_string_ready && boot_string_pos < boot_string.size()) return true;

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
      char* mem = cpu->get_mem();
      for (int i = 0; i < count; i++) {
        uint16_t a = (addr + i) & 0xFFFF;
        fprintf(stderr, "  %s: %02X\n", format_address(a).c_str(),
                (uint8_t)mem[a]);
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
      char* mem = cpu->get_mem();
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
      char* mem = cpu->get_mem();
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

// CP/M Disk Parameter Block (DPB) for standard 8" SSSD
// 77 tracks, 26 sectors/track, 128 bytes/sector, 1024 bytes/block
// Reserved tracks: 2 (for system), directory entries: 64
// IMPORTANT: These must be placed AFTER the BIOS code (F633+) to not
// overwrite BDOS code which extends up to ~F5EC.
constexpr uint16_t DPB_ADDR = 0xF700;  // Location of DPB in memory
constexpr uint16_t DIRBUF_ADDR = 0xF720;  // 128-byte directory buffer
constexpr uint16_t ALV_ADDR = 0xF7A0;  // Allocation vector (32 bytes for 243 blocks)
constexpr uint16_t CSV_ADDR = 0xF7C0;  // Checksum vector (16 bytes)
constexpr uint16_t DPH_BASE = 0xF7E0;  // DPH for each disk (16 bytes each)

// CP/M Disk simulation using Linux filesystem
class CPMDisk {
public:
  std::string directory;  // Host directory for this drive
  bool selected = false;
  bool valid = false;

  // Disk geometry (standard 8" SSSD)
  static constexpr int SECTORS_PER_TRACK = 26;
  static constexpr int TRACKS = 77;
  static constexpr int SECTOR_SIZE = 128;
  static constexpr int BLOCK_SIZE = 1024;
  static constexpr int DIR_ENTRIES = 64;
  static constexpr int RESERVED_TRACKS = 2;  // System tracks

  // Disk Parameter Header (DPH) - returned by SELDSK
  // Points to: XLT, 0000, 0000, 0000, DIRBUF, DPB, CSV, ALV
  uint16_t dph_addr = 0;  // Address of DPH in CP/M memory

  CPMDisk() = default;

  bool open(const std::string& dir) {
    directory = dir;
    struct stat st;
    valid = (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    return valid;
  }

  // Initialize disk structures in CP/M memory
  static void init_disk_tables(char* mem) {
    // DPB - Disk Parameter Block (15 bytes)
    // Format: SPT, BSH, BLM, EXM, DSM, DRM, AL0, AL1, CKS, OFF
    uint16_t dpb = DPB_ADDR;
    mem[dpb + 0] = 26;      // SPT - sectors per track (low)
    mem[dpb + 1] = 0;       // SPT - sectors per track (high)
    mem[dpb + 2] = 3;       // BSH - block shift (1024 byte blocks = 2^(7+3) = 1024)
    mem[dpb + 3] = 7;       // BLM - block mask (2^3 - 1 = 7)
    mem[dpb + 4] = 0;       // EXM - extent mask (for 1K blocks = 0)
    mem[dpb + 5] = 242;     // DSM - max block number (low) ((77-2)*26*128/1024 - 1 = 242)
    mem[dpb + 6] = 0;       // DSM - max block number (high)
    mem[dpb + 7] = 63;      // DRM - max directory entry (low) (64 entries - 1)
    mem[dpb + 8] = 0;       // DRM - max directory entry (high)
    mem[dpb + 9] = 0xC0;    // AL0 - directory allocation bitmap
    mem[dpb + 10] = 0;      // AL1
    mem[dpb + 11] = 16;     // CKS - checksum vector size (low) (DRM/4 = 16)
    mem[dpb + 12] = 0;      // CKS - checksum vector size (high)
    mem[dpb + 13] = 2;      // OFF - reserved tracks (low)
    mem[dpb + 14] = 0;      // OFF - reserved tracks (high)

    // Initialize directory buffer to E5 (empty)
    for (int i = 0; i < 128; i++) {
      mem[DIRBUF_ADDR + i] = 0xE5;
    }

    // Initialize allocation vector to 0
    for (int i = 0; i < 32; i++) {
      mem[ALV_ADDR + i] = 0;
    }

    // Initialize checksum vector to 0
    for (int i = 0; i < 16; i++) {
      mem[CSV_ADDR + i] = 0;
    }

    // Initialize DPH for each potential drive (16 drives max)
    for (int d = 0; d < 16; d++) {
      uint16_t dph = DPH_BASE + (d * 16);
      // XLT - no translation table (sectors 1:1)
      mem[dph + 0] = 0;
      mem[dph + 1] = 0;
      // Scratch areas (3 x 16-bit = 6 bytes)
      mem[dph + 2] = 0; mem[dph + 3] = 0;
      mem[dph + 4] = 0; mem[dph + 5] = 0;
      mem[dph + 6] = 0; mem[dph + 7] = 0;
      // DIRBUF pointer
      mem[dph + 8] = DIRBUF_ADDR & 0xFF;
      mem[dph + 9] = (DIRBUF_ADDR >> 8) & 0xFF;
      // DPB pointer
      mem[dph + 10] = DPB_ADDR & 0xFF;
      mem[dph + 11] = (DPB_ADDR >> 8) & 0xFF;
      // CSV pointer (each drive could have separate, but we share for now)
      mem[dph + 12] = CSV_ADDR & 0xFF;
      mem[dph + 13] = (CSV_ADDR >> 8) & 0xFF;
      // ALV pointer (each drive should have separate, but we share for now)
      mem[dph + 14] = ALV_ADDR & 0xFF;
      mem[dph + 15] = (ALV_ADDR >> 8) & 0xFF;
    }
  }
};

class AltairEmulator {
private:
  qkz80* cpu;
  cpm_mem* memory;
  bool debug;
  bool cpm_mode;
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

  // CP/M BIOS state
  CPMDisk disks[16];      // A: through P:
  int current_disk = 0;   // Currently selected disk
  int current_track = 0;  // Current track
  int current_sector = 0; // Current sector
  uint16_t dma_addr = DMA_DEFAULT;  // DMA address

  // Directory buffer in CP/M memory (at DIRBUF location)
  uint16_t dirbuf_addr = 0x0080;  // Temporary, will be set properly

  // HBIOS disk state (for RomWBW mode)
  struct HBDiskState {
    uint32_t current_lba;  // Current LBA position
    FILE* image_file;      // Disk image file handle
    std::string image_path; // Path to disk image
    bool is_open;

    HBDiskState() : current_lba(0), image_file(nullptr), is_open(false) {}

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

  // EMU HBIOS signal port state (port 0xEE)
  int emu_signal_state = 0;        // State machine for signal protocol
  uint16_t cio_dispatch_addr = 0;  // CIO_DISPATCH trap address
  uint16_t dio_dispatch_addr = 0;  // DIO_DISPATCH trap address
  uint16_t rtc_dispatch_addr = 0;  // RTC_DISPATCH trap address
  uint16_t sys_dispatch_addr = 0;  // SYS_DISPATCH trap address
  bool hbios_trapping_enabled = false;  // True after init complete signal

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

public:
  AltairEmulator(qkz80* acpu, cpm_mem* amem, bool adebug = false, bool acpm = false, bool aromwbw = false)
    : cpu(acpu), memory(amem), debug(adebug), cpm_mode(acpm), romwbw_mode(aromwbw),
      strict_io_mode(false), halted(false),
      sio_status_a(0x02), sio_status_b(0x02),
      input_char(-1),
      sense_switches(0x00),
      ram_top(0xFFFF) {
  }

  void set_strict_io_mode(bool mode) { strict_io_mode = mode; }
  bool is_strict_io_mode() const { return strict_io_mode; }
  bool is_halted() const { return halted; }

  void set_cpm_mode(bool mode) { cpm_mode = mode; }
  bool is_cpm_mode() const { return cpm_mode; }

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
    fprintf(stderr, "[HCB] Drive map at 0x%04X:\n", HCB_BASE + 0x20);
    for (int i = 0; i < 16; i += 8) {
      fprintf(stderr, "  %c-%c: ", 'A' + i, 'A' + i + 7);
      for (int j = 0; j < 8; j++) {
        uint8_t unit = bmem->read_bank(0x00, HCB_BASE + 0x20 + i + j);
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
      if (b0 != 0xFF && b0 != 0x00) {  // Skip empty entries
        fprintf(stderr, "  [%d] %02X %02X %02X %02X\n", i, b0, b1, b2, b3);
      }
    }
  }

  // Attach HBIOS disk image
  bool attach_hbios_disk(int unit, const std::string& image_path) {
    if (unit < 0 || unit >= 16) return false;

    // Close any existing disk
    if (hb_disks[unit].image_file) {
      fclose(hb_disks[unit].image_file);
      hb_disks[unit].image_file = nullptr;
    }

    // Open the disk image
    hb_disks[unit].image_path = image_path;
    hb_disks[unit].image_file = fopen(image_path.c_str(), "r+b");
    if (!hb_disks[unit].image_file) {
      // Try creating it
      hb_disks[unit].image_file = fopen(image_path.c_str(), "w+b");
      if (!hb_disks[unit].image_file) {
        fprintf(stderr, "[HBIOS] Failed to open/create disk image: %s\n", image_path.c_str());
        hb_disks[unit].is_open = false;
        return false;
      }
    }

    hb_disks[unit].is_open = true;
    hb_disks[unit].current_lba = 0;
    fprintf(stderr, "[HBIOS] Attached disk unit %d: %s\n", unit, image_path.c_str());
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

  void set_disk_directory(int drive, const std::string& dir) {
    if (drive >= 0 && drive < 16) {
      if (disks[drive].open(dir)) {
        fprintf(stderr, "Drive %c: -> %s\n", 'A' + drive, dir.c_str());
      } else {
        fprintf(stderr, "Warning: Cannot open directory for drive %c: %s\n",
                'A' + drive, dir.c_str());
      }
    }
  }

  void set_sense_switches(uint8_t val) {
    sense_switches = val;
    if (debug) fprintf(stderr, "Sense switches set to: 0x%02X\n", sense_switches);
  }

  // Handle IN instruction - returns value read from port
  uint8_t handle_in(uint8_t port) {
    port_in_counts[port]++;
    uint8_t value = 0x00;

    // RomWBW UART ports (accent on 0x68-0x6F range for 16550-style UART)
    if (romwbw_mode) {
      switch (port) {
        case 0x68:  // UART data register
          if (input_char >= 0) {
            value = input_char & 0xFF;
            input_char = -1;
          } else if (peek_char >= 0) {
            int ch = peek_char;
            peek_char = -1;
            if (check_ctrl_c_exit(ch)) {
              
              value = 0;  // ^C consumed, return nothing
            } else {
              value = ch & 0xFF;
            }
          } else if (stdin_has_data()) {
            int ch = peek_char;
            peek_char = -1;
            if (ch == EOF) ch = 0;
            if (check_ctrl_c_exit(ch)) {
              
              value = 0;  // ^C consumed, return nothing
            } else {
              value = ch & 0xFF;
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
          value = input_char & 0x7F;
          input_char = -1;
        } else if (peek_char >= 0) {
          // Use character already peeked by stdin_has_data() or check_console_escape_async()
          int ch = peek_char;
          peek_char = -1;
          if (check_ctrl_c_exit(ch)) {
            
            value = 0;  // ^C consumed
          } else {
            if (ch == '\n') ch = '\r';
            value = ch & 0x7F;
          }
        } else if (stdin_has_data()) {
          // stdin_has_data() sets peek_char, so use it
          int ch = peek_char;
          peek_char = -1;
          if (ch == EOF) ch = 0;
          if (check_ctrl_c_exit(ch)) {
            
            value = 0;  // ^C consumed
          } else {
            if (ch == '\n') ch = '\r';
            value = ch & 0x7F;
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

        case 0xEE:  // EMU HBIOS signal port
          handle_emu_signal(value);
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

  // Handle BIOS call - called when PC is in BIOS range
  // Returns true if handled, false to continue normal execution
  bool handle_bios_call(uint16_t pc) {
    uint16_t offset = pc - BIOS_BASE;
    char* mem = cpu->get_mem();

    if (debug) {
      fprintf(stderr, "[BIOS call at 0x%04X, offset 0x%02X]\n", pc, offset);
    }

    switch (offset) {
      case BIOS_BOOT: {
        // Cold boot - initialize system
        if (debug) fprintf(stderr, "[BIOS BOOT - Cold boot]\n");

        // Initialize disk tables (DPH, DPB, buffers)
        CPMDisk::init_disk_tables(mem);

        // Set up page zero
        mem[0x0000] = 0xC3;  // JMP WBOOT
        mem[0x0001] = (BIOS_BASE + BIOS_WBOOT) & 0xFF;
        mem[0x0002] = (BIOS_BASE + BIOS_WBOOT) >> 8;
        mem[0x0003] = 0x00;  // IOBYTE
        mem[0x0004] = 0x00;  // Current drive/user
        mem[0x0005] = 0xC3;  // JMP BDOS
        // BDOS entry is at FBASE (E806 from sym file)
        mem[0x0006] = 0x06;
        mem[0x0007] = 0xE8;

        // CRITICAL: Set BDOS ACTIVE variable to 0xFF (invalid disk)
        // so that the first SETDSK call from CCP actually runs LOGINDRV
        // which calls SELDSK to initialize disk parameters.
        // ACTIVE is at EB42 (from symbol file)
        mem[0xEB42] = 0xFF;

        // Set default DMA
        dma_addr = DMA_DEFAULT;
        // Select disk A
        current_disk = 0;
        current_track = 0;
        current_sector = 1;
        // Print signon message
        fprintf(stderr, "\n[CP/M 2.2 Emulator - BIOS Cold Boot]\n");

        // CCP expects BC = (user << 4) | drive
        // Set BC = 0 for user 0, drive A
        cpu->regs.BC.set_pair16(0x0000);

        // Jump to CCP (E000) - not CBASE+3, that's for warm boot
        cpu->regs.PC.set_pair16(CPM_LOAD_ADDR);
        return true;
      }

      case BIOS_WBOOT: {
        // Warm boot - reload CCP/BDOS and jump to CCP
        if (debug) fprintf(stderr, "[BIOS WBOOT - Warm boot]\n");
        // Reload page zero (same as cold boot minus initialization)
        mem[0x0000] = 0xC3;  // JMP WBOOT
        mem[0x0001] = (BIOS_BASE + BIOS_WBOOT) & 0xFF;
        mem[0x0002] = (BIOS_BASE + BIOS_WBOOT) >> 8;
        // Preserve IOBYTE and current drive/user
        mem[0x0005] = 0xC3;  // JMP BDOS
        mem[0x0006] = 0x06;
        mem[0x0007] = 0xE8;
        // Set default DMA
        dma_addr = DMA_DEFAULT;
        // Jump to CCP + 3 (warm boot entry)
        cpu->regs.PC.set_pair16(CPM_LOAD_ADDR + 3);
        return true;
      }

      case BIOS_CONST: {
        // Console status - return A=0xFF if ready, A=0 if not
        bool ready = (input_char >= 0 || stdin_has_data());
        cpu->set_reg8(ready ? 0xFF : 0x00, qkz80::reg_A);
        // Return from BIOS call
        do_ret();
        return true;
      }

      case BIOS_CONIN: {
        // Console input - wait for character, return in A
        int ch;
        if (input_char >= 0) {
          ch = input_char;
          input_char = -1;
        } else if (peek_char >= 0) {
          // Use peeked character from stdin_has_data()
          ch = peek_char;
          peek_char = -1;
        } else {
          // Blocking wait for input
          while (!stdin_has_data()) {
            if (stdin_eof) {
              // EOF reached during wait - exit for pipe input
              if (!isatty(STDIN_FILENO)) {
                exit(0);
              }
              // For TTY, return ^Z
              ch = 0x1A;
              cpu->set_reg8(ch & 0x7F, qkz80::reg_A);
              do_ret();
              return true;
            }
            usleep(1000);  // 1ms delay
          }
          // stdin_has_data() returned true, so peek_char is set
          ch = peek_char;
          peek_char = -1;
          if (check_ctrl_c_exit(ch)) {
            
          }
          if (ch == '\n') ch = '\r';
        }
        cpu->set_reg8(ch & 0x7F, qkz80::reg_A);
        do_ret();
        return true;
      }

      case BIOS_CONOUT: {
        // Console output - print character in C
        uint8_t ch = cpu->get_reg8(qkz80::reg_C);
        emu_console_write_char(ch);
        do_ret();
        return true;
      }

      case BIOS_LIST: {
        // List output (printer) - print char in C
        uint8_t ch = cpu->get_reg8(qkz80::reg_C);
        emu_printer_out(ch);
        do_ret();
        return true;
      }

      case BIOS_PUNCH: {
        // Punch output - send to aux output
        uint8_t ch = cpu->get_reg8(qkz80::reg_C);
        emu_aux_out(ch);
        do_ret();
        return true;
      }

      case BIOS_READER: {
        // Reader input - get from aux input
        int ch = emu_aux_in();
        cpu->set_reg8(ch & 0x7F, qkz80::reg_A);
        do_ret();
        return true;
      }

      case BIOS_HOME: {
        // Home disk - seek to track 0
        current_track = 0;
        do_ret();
        return true;
      }

      case BIOS_SELDSK: {
        // Select disk - C=disk number (0=A, 1=B, etc.)
        // Return HL=DPH address or HL=0 if invalid
        uint8_t disk = cpu->get_reg8(qkz80::reg_C);
        if (debug) fprintf(stderr, "[BIOS SELDSK: disk %c]\n", 'A' + disk);

        if (disk < 16 && disks[disk].valid) {
          current_disk = disk;
          // Return pointer to DPH (properly initialized in BOOT)
          uint16_t dph = DPH_BASE + (disk * 16);
          cpu->regs.HL.set_pair16(dph);
        } else {
          // Invalid disk
          cpu->regs.HL.set_pair16(0x0000);
        }
        do_ret();
        return true;
      }

      case BIOS_SETTRK: {
        // Set track - BC=track number
        current_track = cpu->regs.BC.get_pair16();
        if (debug) fprintf(stderr, "[BIOS SETTRK: track %d]\n", current_track);
        do_ret();
        return true;
      }

      case BIOS_SETSEC: {
        // Set sector - BC=sector number
        current_sector = cpu->regs.BC.get_pair16();
        if (debug) fprintf(stderr, "[BIOS SETSEC: sector %d]\n", current_sector);
        do_ret();
        return true;
      }

      case BIOS_SETDMA: {
        // Set DMA address - BC=address
        dma_addr = cpu->regs.BC.get_pair16();
        if (debug) fprintf(stderr, "[BIOS SETDMA: 0x%04X]\n", dma_addr);
        do_ret();
        return true;
      }

      case BIOS_READ: {
        // Read sector - return A=0 if OK, A=1 if error
        if (debug) {
          fprintf(stderr, "[BIOS READ: disk %c, track %d, sector %d, DMA 0x%04X]\n",
                  'A' + current_disk, current_track, current_sector, dma_addr);
        }

        int result = do_disk_read();
        cpu->set_reg8(result, qkz80::reg_A);
        do_ret();
        return true;
      }

      case BIOS_WRITE: {
        // Write sector - C=write type, return A=0 if OK
        uint8_t write_type = cpu->get_reg8(qkz80::reg_C);
        if (debug) {
          fprintf(stderr, "[BIOS WRITE: disk %c, track %d, sector %d, DMA 0x%04X, type %d]\n",
                  'A' + current_disk, current_track, current_sector, dma_addr, write_type);
        }

        int result = do_disk_write();
        cpu->set_reg8(result, qkz80::reg_A);
        do_ret();
        return true;
      }

      case BIOS_PRSTAT: {
        // Printer status - return A=0 if not ready, A=0xFF if ready
        cpu->set_reg8(0xFF, qkz80::reg_A);  // Always ready
        do_ret();
        return true;
      }

      case BIOS_SECTRN: {
        // Sector translate - BC=logical sector (0-based), DE=translate table
        // Return HL=physical sector
        // For IBM 8" SSSD format, physical sectors are 1-based (1-26)
        // With no skew table (XLT=0), simply convert 0-based to 1-based
        uint16_t logical = cpu->regs.BC.get_pair16();
        uint16_t de = cpu->regs.DE.get_pair16();
        uint16_t physical;
        if (de == 0) {
          // No translation table - just add 1 to convert to 1-based physical
          physical = logical + 1;
        } else {
          // Use translation table - read physical sector from table
          char* mem = cpu->get_mem();
          physical = (uint8_t)mem[de + logical];
        }
        if (debug) fprintf(stderr, "[BIOS SECTRN: logical %d -> physical %d]\n", logical, physical);
        cpu->regs.HL.set_pair16(physical);
        do_ret();
        return true;
      }

      default:
        fprintf(stderr, "[ERROR: Unknown BIOS call at 0x%04X, offset 0x%02X]\n",
                pc, offset);
        return false;
    }
  }

private:
  // Execute RET instruction to return from BIOS call
  void do_ret() {
    char* mem = cpu->get_mem();
    uint16_t sp = cpu->regs.SP.get_pair16();
    uint16_t ret_addr = (uint8_t)mem[sp] | ((uint8_t)mem[sp + 1] << 8);
    cpu->regs.SP.set_pair16(sp + 2);
    cpu->regs.PC.set_pair16(ret_addr);
  }

  // Read a sector from the disk image
  int do_disk_read() {
    if (current_disk < 0 || current_disk >= 16 || !disks[current_disk].valid) {
      return 1;  // Error
    }

    // Calculate file offset: (track * 26 + (sector - 1)) * 128
    // Sector numbers are 1-based in CP/M
    long offset = ((long)current_track * CPMDisk::SECTORS_PER_TRACK +
                   (current_sector - 1)) * CPMDisk::SECTOR_SIZE;

    // Build filename: directory/diskimage.img or use raw sectors
    std::string img_path = disks[current_disk].directory + "/disk.img";
    FILE* f = fopen(img_path.c_str(), "rb");
    if (!f) {
      // No disk image - return zeros
      char* mem = cpu->get_mem();
      memset(&mem[dma_addr], 0xE5, CPMDisk::SECTOR_SIZE);
      return 0;
    }

    fseek(f, offset, SEEK_SET);
    char* mem = cpu->get_mem();
    size_t read = fread(&mem[dma_addr], 1, CPMDisk::SECTOR_SIZE, f);
    fclose(f);

    if (read < CPMDisk::SECTOR_SIZE) {
      // Pad with E5 (empty directory entry marker)
      memset(&mem[dma_addr + read], 0xE5, CPMDisk::SECTOR_SIZE - read);
    }

    return 0;  // Success
  }

  // Write a sector to the disk image
  int do_disk_write() {
    if (current_disk < 0 || current_disk >= 16 || !disks[current_disk].valid) {
      return 1;  // Error
    }

    long offset = ((long)current_track * CPMDisk::SECTORS_PER_TRACK +
                   (current_sector - 1)) * CPMDisk::SECTOR_SIZE;

    std::string img_path = disks[current_disk].directory + "/disk.img";
    FILE* f = fopen(img_path.c_str(), "r+b");
    if (!f) {
      // Create the file
      f = fopen(img_path.c_str(), "wb");
      if (!f) {
        return 1;  // Error
      }
    }

    fseek(f, offset, SEEK_SET);
    char* mem = cpu->get_mem();
    fwrite(&mem[dma_addr], 1, CPMDisk::SECTOR_SIZE, f);
    fclose(f);

    return 0;  // Success
  }

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

  // Handle EMU HBIOS signal port (port 0xEE)
  // Protocol:
  //   0x01 = HBIOS starting (boot code reached HBIOS entry)
  //   0xFE = PREINIT about to be called (test mode: halt here)
  //   0xFF = Init complete, enable PC-based trapping at 0xFFF0
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

      case 0xFF:  // Init complete - enable PC trapping at 0xFFF0
        hbios_trapping_enabled = true;
        if (debug) fprintf(stderr, "[EMU HBIOS: Init complete, PC trapping enabled at 0xFFF0]\n");

        // If romldr path is set, switch to ROM bank 1 and run romldr from ROM
        if (!romldr_path.empty()) {
          banked_mem* bmem = dynamic_cast<banked_mem*>(memory);
          if (bmem && romldr_loaded) {
            // romldr is in ROM bank 1. ROM bank 1 already has:
            // - RST 08 at 0x0008: JP 0xFFF0 (HBIOS entry, we trap this)
            // - Entry at 0x0000: JP 0x0100 (main romldr init)
            //
            // Switch to ROM bank 1 and jump to the entry point.
            // The code runs from ROM (0x0000-0x7FFF) with common RAM at 0x8000-0xFFFF.
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

  // Check if current PC matches an HBIOS dispatch address
  // Returns true if trapped and handled
  bool check_hbios_trap(uint16_t pc) {
    if (!hbios_trapping_enabled) return false;

    // Trap at 0xFFF0 - the fixed HBIOS entry point
    // This is where CBIOS and user code call HBIOS (RST 08 also jumps here via proxy)
    if (pc == 0xFFF0) {
      return handle_hbios_call(pc);
    }

    // Optional: trap at specific dispatch addresses if configured (non-zero)
    if (cio_dispatch_addr != 0 && pc == cio_dispatch_addr) {
      return handle_hbios_call(pc);
    }
    if (dio_dispatch_addr != 0 && pc == dio_dispatch_addr) {
      return handle_hbios_call(pc);
    }
    if (rtc_dispatch_addr != 0 && pc == rtc_dispatch_addr) {
      return handle_hbios_call(pc);
    }
    if (sys_dispatch_addr != 0 && pc == sys_dispatch_addr) {
      return handle_hbios_call(pc);
    }

    return false;
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
          // Check if boot_string is available (only on unit 0xFF)
          bool boot_string_available = (unit == 0xFF) && boot_string_ready &&
                                       boot_string_pos < boot_string.size();
          // Wait for character from stdin (any unit) or boot_string (unit 0xFF only)
          while (input_char < 0 && peek_char < 0 && !boot_string_available && !stdin_eof) {
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
            // Re-check boot_string availability
            boot_string_available = (unit == 0xFF) && boot_string_ready &&
                                    boot_string_pos < boot_string.size();
          }
          uint8_t ch;
          // Check boot string first (for auto-boot feature, only on unit 0xFF)
          if (boot_string_available) {
            ch = boot_string[boot_string_pos++] & 0xFF;
          } else if (input_char >= 0) {
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
        // Track output to detect boot prompt for --boot option
        if (!boot_string.empty()) {
          track_output_for_boot_prompt(ch);
        }
        break;
      }

      case HBF_CIOIST: {  // Console input status - return count in A
        uint8_t count = 0;
        // Check stdin for any unit that acts as console (0xFF, 0xD0, etc.)
        // Most units map to the same physical console in emulation
        bool check_stdin = (unit == 0xFF || unit == 0xD0 || unit == 0x00);
        if (check_stdin) {
          // Check real stdin input
          // Must call stdin_has_data() to actually poll stdin for new input
          bool has_stdin = (input_char >= 0 || stdin_has_data());
          // Check boot_string (only for unit 0xFF to avoid double-reading)
          bool has_boot_string = (unit == 0xFF) && boot_string_ready && boot_string_pos < boot_string.size();
          count = (has_stdin || has_boot_string) ? 1 : 0;
          // Check if we should activate boot_string
          if (count == 0 && !boot_string.empty() && unit == 0xFF) {
            check_boot_string_activation();
            // Re-check after activation
            if (boot_string_ready && boot_string_pos < boot_string.size()) {
              count = 1;
            }
          }
        }
        result = count;
        break;
      }

      case HBF_CIOOST: {  // Console output status - return space available in A
        result = 255;  // Lots of space available in output buffer
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
          boot_string_ready = false;
          boot_string_pos = 0;

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

              if (debug) {
                fprintf(stderr, "[HBIOS BCALL->DIOSEEK] Unit %d: LBA=%u\n", dio_unit, lba);
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
        } else {
          // Hard disk - also needs LBA mode
          dev_attrs = 0x00;    // LBA mode (bit 7 clear)
          media_id = 0x04;     // MID_HD
        }

        // Set return values
        cpu->regs.BC.set_high(dev_attrs);  // B = device attributes
        cpu->regs.BC.set_low(media_id);    // C = media ID
        // DE:HL = LBA offset (0 for slice 0, we don't support slicing)
        cpu->regs.DE.set_pair16(0);
        cpu->regs.HL.set_pair16(0);

        if (debug) {
          fprintf(stderr, "[HBIOS EXTSLICE] Unit %d (mapped=%d) slice %d: attrs=0x%02X media=0x%02X LBA=0\n",
                  disk_unit, mapped_unit, slice, dev_attrs, media_id);
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

        // DEBUG: Trace disk reads from ROM disk (unit 1) to CBIOS buffer
        // Set to > 0 to enable trace output
        static int dioread_trace_count = 0;
        constexpr int DIOREAD_TRACE_LIMIT = 0;  // Disabled
        if (DIOREAD_TRACE_LIMIT > 0 && unit == 1 && buffer_addr == 0x0A00 && dioread_trace_count < DIOREAD_TRACE_LIMIT) {
          dioread_trace_count++;
          MemDiskState& md = md_disks[unit];
          fprintf(stderr, "[DIOREAD u1 #%d] LBA=%u -> bank=0x%02X addr=0x%04X blocks=%d\n",
                  dioread_trace_count, md.current_lba, buffer_bank, buffer_addr, block_count);
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
        // Returns: D=device type, E=device number within type
        // Device types: 0x00=MD (memory disk), 0x03=IDE, 0x06=SD, 0x09=HDSK
        uint8_t dev_type = 0;
        uint8_t dev_num = 0;

        if (unit < 2 && md_disks[unit].is_enabled) {
          // Memory disk (MD0=RAM, MD1=ROM)
          dev_type = 0x00;  // DIODEV_MD
          dev_num = unit;   // 0 or 1
        } else if (unit >= 2 && unit < 18 && hb_disks[unit - 2].is_open) {
          // Hard disk
          dev_type = 0x09;  // DIODEV_HDSK
          dev_num = unit - 2;
        } else {
          result = HBR_NOTREADY;
        }

        cpu->regs.DE.set_high(dev_type);
        cpu->regs.DE.set_low(dev_num);
        if (debug) {
          fprintf(stderr, "[HBIOS DIODEVICE] Unit %d: type=0x%02X num=%d\n", unit, dev_type, dev_num);
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
                fprintf(stderr, "[HBIOS SYSGET DIOCNT] Returning %d disks (MD + HD)\n", disk_count);
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
            // D = boot device/unit, E = boot bank
            cpu->regs.DE.set_high(0);   // Boot device 0
            cpu->regs.DE.set_low(0x8E); // Boot bank
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
            // D = boot device/unit, E = boot bank, L = boot slice
            // Just acknowledge - we don't need to track this
            if (debug) {
              fprintf(stderr, "[SYSSET BOOTINFO] device=%d bank=0x%02X slice=%d\n",
                      cpu->regs.DE.get_high(), cpu->regs.DE.get_low(), cpu->regs.HL.get_low());
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

      // VDA functions - no video hardware present
      case HBF_VDASTATUS:
      case HBF_VDARESET:
      case HBF_VDAQUERY:
      case HBF_VDADEVICE:
        result = HBR_NOHW;  // No VDA hardware
        break;

      // DSKY functions - no hardware present
      case HBF_DSKYRESET:
      case HBF_DSKYSTATUS:
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
    // Since we only trap at dispatch addresses (via check_hbios_trap), the call chain was:
    //   RST 08 -> HB_INVOKE -> jp DISPATCH
    // RST 08 pushed the return address, so we pop it from the stack.
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

void print_usage(const char* prog) {
  fprintf(stderr, "Usage: %s [options] <binary.bin|cpm22.sys|romwbw.rom>\n", prog);
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --version, -v     Show version information\n");
  fprintf(stderr, "  --cpm             Enable CP/M mode (load at 0xE000, trap BIOS)\n");
  fprintf(stderr, "  --romwbw          Enable RomWBW mode (512KB ROM+RAM, Z80, bank switching)\n");
  fprintf(stderr, "  --strict-io       Halt on unexpected I/O ports (for RomWBW debugging)\n");
  fprintf(stderr, "  --debug           Enable debug output\n");
  fprintf(stderr, "  --load=0xNNNN     Load address (default: 0x0000, or 0xE000 for --cpm)\n");
  fprintf(stderr, "  --start=0xNNNN    Start address (default: load address)\n");
  fprintf(stderr, "  --disk-a=DIR      Set drive A: directory (CP/M mode)\n");
  fprintf(stderr, "  --disk-b=DIR      Set drive B: directory (CP/M mode)\n");
  fprintf(stderr, "  --hbdiskN=FILE    Attach disk image N (0-15) for RomWBW/HBIOS mode\n");
  fprintf(stderr, "  --romapp=K:FILE   Register ROM app with boot key K (C, Z, Q, etc.)\n");
  fprintf(stderr, "                    Example: --romapp=C:cpm_wbw.sys --romapp=Z:zsys_wbw.sys\n");
  fprintf(stderr, "  --romldr=FILE     Use RomWBW romldr boot menu (FILE can be .rom or .bin)\n");
  fprintf(stderr, "                    Example: --romldr=SBC_std.rom (uses bank 1 from ROM)\n");
  fprintf(stderr, "  --boot=STRING     Auto-boot string (e.g., 'HD0:0' or '0' or 'C')\n");
  fprintf(stderr, "                    Automatically entered at boot prompt (adds CR)\n");
  fprintf(stderr, "  --sense=0xNN      Set sense switch value\n");
  fprintf(stderr, "  --trace=FILE      Write execution trace to FILE (for ud80 disassembly)\n");
  fprintf(stderr, "  --symbols=FILE    Load symbol table from FILE (.sym)\n");
  fprintf(stderr, "  --escape=CHAR     Console escape char (default ^E, e.g. --escape=^]\\ )\n");
  fprintf(stderr, "  --mask-interrupt <range> <rst|call> <n>\n");
  fprintf(stderr, "                    Schedule maskable interrupts (respects DI/EI)\n");
  fprintf(stderr, "                    Range: min-max cycles (random in range)\n");
  fprintf(stderr, "                    Examples: --mask-interrupt 4000-4500 rst 7\n");
  fprintf(stderr, "                              --mask-interrupt 5000 call 0x0100\n");
  fprintf(stderr, "  --nmi <range>     Schedule NMI every range cycles (jumps to 0x0066)\n");
  fprintf(stderr, "                    Example: --nmi 10000-12000\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Console mode:\n");
  fprintf(stderr, "  Press the escape char (default Ctrl+E) to enter console mode.\n");
  fprintf(stderr, "  Type 'help' in console mode for available commands.\n");
  fprintf(stderr, "  Use 'quit' to exit and write trace file.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Examples:\n");
  fprintf(stderr, "  %s --cpm cpm22.sys --disk-a=./drivea\n", prog);
  fprintf(stderr, "  %s 4kbas40.bin --trace=trace.txt\n", prog);
  fprintf(stderr, "  %s 4kbas40.bin --symbols=4kbas.sym\n", prog);
  fprintf(stderr, "  %s --romwbw ~/esrc/RomWBW/Binary/SBC_std.rom\n", prog);
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
  bool load_addr_set = false;
  bool start_addr_set = false;
  bool debug = false;
  bool cpm_mode = false;
  bool romwbw_mode = false;
  bool strict_io_mode = false;
  int sense = -1;
  std::string disk_dirs[16];
  std::string hbios_disks[16];  // For RomWBW disk images
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
      fprintf(stderr, "altair_emu version %s (%s)\n", EMU_VERSION, EMU_VERSION_DATE);
      fprintf(stderr, "RomWBW HBIOS emulation with memory disk support\n");
      return 0;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--debug") == 0) {
      debug = true;
    } else if (strcmp(argv[i], "--cpm") == 0) {
      cpm_mode = true;
    } else if (strcmp(argv[i], "--romwbw") == 0) {
      romwbw_mode = true;
    } else if (strcmp(argv[i], "--strict-io") == 0) {
      strict_io_mode = true;
    } else if (strncmp(argv[i], "--sense=", 8) == 0) {
      sense = strtol(argv[i] + 8, nullptr, 0);
    } else if (strncmp(argv[i], "--load=", 7) == 0) {
      load_addr = strtol(argv[i] + 7, nullptr, 0);
      load_addr_set = true;
    } else if (strncmp(argv[i], "--start=", 8) == 0) {
      start_addr = strtol(argv[i] + 8, nullptr, 0);
      start_addr_set = true;
    } else if (strncmp(argv[i], "--disk-a=", 9) == 0) {
      disk_dirs[0] = argv[i] + 9;
    } else if (strncmp(argv[i], "--disk-b=", 9) == 0) {
      disk_dirs[1] = argv[i] + 9;
    } else if (strncmp(argv[i], "--disk-c=", 9) == 0) {
      disk_dirs[2] = argv[i] + 9;
    } else if (strncmp(argv[i], "--disk-d=", 9) == 0) {
      disk_dirs[3] = argv[i] + 9;
    } else if (strncmp(argv[i], "--hbdisk", 8) == 0) {
      // Parse --hbdisk0=file, --hbdisk1=file, etc.
      const char* opt = argv[i] + 8;
      int unit = -1;
      const char* path = nullptr;
      if (isdigit(opt[0]) && opt[1] == '=' && opt[2] != '\0') {
        unit = opt[0] - '0';
        path = opt + 2;
      } else if (isdigit(opt[0]) && isdigit(opt[1]) && opt[2] == '=' && opt[3] != '\0') {
        unit = (opt[0] - '0') * 10 + (opt[1] - '0');
        path = opt + 3;
      }
      if (unit >= 0 && unit < 16 && path) {
        hbios_disks[unit] = path;
      } else {
        fprintf(stderr, "Invalid --hbdisk option: %s\n", argv[i]);
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
    } else if (strncmp(argv[i], "--boot=", 7) == 0) {
      boot_string = argv[i] + 7;
      // Check if user provided a trailing CR/LF - if not, add CR
      if (boot_string.empty() || (boot_string.back() != '\r' && boot_string.back() != '\n')) {
        boot_string += '\r';  // Add CR to submit the boot command
      }
      fprintf(stderr, "Auto-boot string: '%s' (%zu chars)\n", boot_string.c_str(), boot_string.size());
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
  if (cpm_mode) {
    if (!load_addr_set) load_addr = CPM_LOAD_ADDR;
    if (!start_addr_set) start_addr = BIOS_BASE;  // Start at BIOS BOOT
  } else if (romwbw_mode) {
    // RomWBW starts at address 0x0000 in ROM bank 0
    if (!start_addr_set) start_addr = 0x0000;
  } else {
    if (!start_addr_set) start_addr = load_addr;
  }

  // Create memory and CPU
  cpm_mem memory;
  qkz80 cpu(&memory);

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

  // Create emulator
  AltairEmulator emu(&cpu, &memory, debug, cpm_mode, romwbw_mode);
  emu.set_strict_io_mode(strict_io_mode);

  // Set up disks
  for (int i = 0; i < 16; i++) {
    if (!disk_dirs[i].empty()) {
      emu.set_disk_directory(i, disk_dirs[i]);
    }
  }

  // Set up HBIOS disk images (for RomWBW mode)
  // NOTE: Memory disks are initialized later, after ROM is loaded
  if (romwbw_mode) {
    // Attach any file-backed hard disk images
    for (int i = 0; i < 16; i++) {
      if (!hbios_disks[i].empty()) {
        if (!emu.attach_hbios_disk(i, hbios_disks[i])) {
          fprintf(stderr, "Warning: Could not attach disk %d: %s\n", i, hbios_disks[i].c_str());
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

  // Set up BIOS trap range for CP/M mode
  if (cpm_mode) {
    memory.set_bios_range(BIOS_BASE, BIOS_BASE + 0x33);  // 17 * 3 = 51 bytes
    fprintf(stderr, "CP/M mode: BIOS traps at 0x%04X-0x%04X\n",
            BIOS_BASE, BIOS_BASE + 0x32);
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

    }

    // NOW initialize memory disks from HCB (after ROM is loaded)
    emu.init_memory_disks();

    // Enable page zero write tracing for debugging
    if (debug) {
      memory.set_trace_page_zero(true);
    }
  } else {
    // Load into flat 64KB memory
    char* mem = cpu.get_mem();
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
  if (cpm_mode) {
    cpu.regs.SP.set_pair16(CPM_LOAD_ADDR);  // E000 - stack grows into TPA
  } else if (romwbw_mode) {
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

    // Check for BIOS trap in CP/M mode
    if (cpm_mode && memory.is_bios_trap(pc)) {
      if (debug) fprintf(stderr, "[BIOS trap check: PC=0x%04X]\n", pc);
      if (emu.handle_bios_call(pc)) {
        instruction_count++;
        if (in_step_mode) step_count--;
        continue;
      }
      // If handle_bios_call returned false, we have an unhandled BIOS call
      fprintf(stderr, "[ERROR: Unhandled BIOS trap at PC=0x%04X, falling through!]\n", pc);
    }

    // Check for HBIOS trap in RomWBW mode
    // PC-based trapping when EMU HBIOS has signaled dispatch addresses (via port 0xEE)
    // NOTE: We only trap at dispatch addresses (CIO_DISPATCH, DIO_DISPATCH, etc.)
    // NOT at RST 08 opcodes. The RST 08 instruction pushes return address and jumps
    // to 0x0008, which then routes to the appropriate dispatch address. We must let
    // RST 08 execute normally so the stack is set up correctly.
    if (romwbw_mode) {
      if (emu.check_hbios_trap(pc)) {
        instruction_count++;
        if (in_step_mode) step_count--;
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

    // Handle IN instruction (0xDB)
    if (opcode == 0xDB) {
      // Mark both bytes as code for tracing
      memory.fetch_mem(pc, true);
      memory.fetch_mem(pc + 1, true);
      uint8_t port = memory.fetch_mem(pc + 1) & 0xFF;
      uint8_t value = emu.handle_in(port);
      // Check if strict I/O mode halted us
      if (emu.is_halted()) {
        fprintf(stderr, "\nEmulator halted (strict I/O mode) after %lld instructions\n",
                instruction_count);
        emu.print_port_stats();
        break;
      }
      // Check if the input was the console escape char
      if (value == console_escape_char && isatty(STDIN_FILENO)) {
        console_mode_requested = true;
        // Don't pass escape char to emulated program - re-read
        cpu.regs.PC.set_pair16(pc);  // Stay at IN instruction
        continue;
      }
      cpu.set_reg8(value, qkz80::reg_A);
      cpu.regs.PC.set_pair16(pc + 2);
      instruction_count++;
      if (in_step_mode) step_count--;
      continue;
    }

    // Handle OUT instruction (0xD3)
    if (opcode == 0xD3) {
      // Mark both bytes as code for tracing
      memory.fetch_mem(pc, true);
      memory.fetch_mem(pc + 1, true);
      uint8_t port = memory.fetch_mem(pc + 1) & 0xFF;
      uint8_t value = cpu.get_reg8(qkz80::reg_A);
      emu.handle_out(port, value);
      // Check if strict I/O mode halted us
      if (emu.is_halted()) {
        fprintf(stderr, "\nEmulator halted (strict I/O mode) after %lld instructions\n",
                instruction_count);
        emu.print_port_stats();
        break;
      }
      // Check if OUT handler wants to redirect PC
      if (emu.has_next_pc()) {
        cpu.regs.PC.set_pair16(emu.get_next_pc());
      } else {
        cpu.regs.PC.set_pair16(pc + 2);
      }
      instruction_count++;
      if (in_step_mode) step_count--;
      continue;
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
    // Debug: check for BIOS-area execution that doesn't get trapped
    if (debug && pc >= 0xF600 && pc < 0xF633) {
      fprintf(stderr, "[DEBUG BIOS AREA: PC=0x%04X, opcode=0x%02X, is_trap=%d]\n",
              pc, opcode, memory.is_bios_trap(pc));
    }

    // Debug: trace instructions after CIOIN
    emu.trace_after_cioin(pc, opcode);

    // Execute one instruction
    cpu.execute();
    instruction_count++;
    if (in_step_mode) step_count--;

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

  // Write trace file if tracing was enabled
  if (!trace_file.empty()) {
    memory.write_trace_script(trace_file.c_str(), load_addr);
  }

  return 0;
}
