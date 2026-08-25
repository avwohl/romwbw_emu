/*
 * Emulator I/O Implementation - CLI (Unix Terminal)
 *
 * This implementation uses Unix terminal I/O (termios, select)
 * for running the emulator as a command-line application.
 */

#include "emu_io.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <strings.h>
#include <queue>
#include <random>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

//=============================================================================
// Terminal State Management
//=============================================================================

static struct termios original_termios;
static bool termios_saved = false;
static bool raw_mode_enabled = false;

// Input queue for async input
static std::queue<int> input_queue;

// EOF tracking
static bool stdin_eof = false;
static bool stdin_eof_consumed = false;  // guest read past EOF (see read_char)
static bool stdin_is_tty = false;  // set in emu_io_init()
static int peek_char = -1;

// Escape handling - the key the emulator reserves for the sim> debugger.
// Written only by emu_console_check_escape(), which the frontend stops
// calling when the escape is disabled, so the initial value has to be the
// disabled one (0) rather than ^E: otherwise a --escape=none run would go on
// latching ^E on the blocking read below, which is exactly the bug the switch
// exists to fix.
static char current_escape_char = 0;  // 0 = no key reserved
static bool escape_requested = false;

// Random number generator
static std::mt19937 rng(std::random_device{}());

//=============================================================================
// Platform Utilities Implementation
//=============================================================================

void emu_sleep_ms(int ms) {
  usleep(ms * 1000);
}

int emu_strcasecmp(const char* s1, const char* s2) {
  return strcasecmp(s1, s2);
}

int emu_strncasecmp(const char* s1, const char* s2, size_t n) {
  return strncasecmp(s1, s2, n);
}

bool emu_file_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

size_t emu_file_size(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  return st.st_size;
}

//=============================================================================
// Terminal State Management
//=============================================================================

static void restore_terminal() {
  if (termios_saved && raw_mode_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    raw_mode_enabled = false;
  }
}

//=============================================================================
// Console I/O Implementation
//=============================================================================

void emu_io_init() {
  stdin_is_tty = isatty(STDIN_FILENO);

  // Save original terminal settings if we're a TTY
  if (stdin_is_tty) {
    if (!termios_saved) {
      tcgetattr(STDIN_FILENO, &original_termios);
      termios_saved = true;
      atexit(restore_terminal);
    }

    // Enable raw mode for input (no canonical line editing, no echo, no
    // signal generation) but KEEP output post-processing (OPOST/ONLCR)
    // enabled. Every output path in this program emits a bare '\n' and
    // relies on the kernel translating it to "\r\n": emu_console_write_char()
    // strips the guest's '\r' (CP/M sends "\r\n"), and the log/debugger
    // messages use plain "\n". Disabling OPOST here broke that assumption and
    // produced "stair-stepped" output (LF with no CR) on terminals that don't
    // implicitly add a CR for a bare LF (e.g. the Linux console).
    // These are the clears cpmemu's enable_raw_mode() makes (its commit
    // 1584295), minus two on purpose - see the c_oflag and c_cflag notes
    // below. Every Ctrl-letter is a live key in CP/M, so the line discipline
    // must not consume any of them before the guest gets a chance.
    struct termios raw = original_termios;
    // IEXTEN as well as the obvious three: on Linux IEXTEN is inert once
    // ICANON is off, but BSD/XNU gates VLNEXT (^V) and VDISCARD (^O) on
    // IEXTEN alone, outside the ICANON block, so without this the macOS build
    // eats both - ^O being the whole WordStar onscreen-format prefix (^OL,
    // ^OC, ^OR). ECHONL is inert with ICANON off and is cleared only to keep
    // this mask diffable against cpmemu's.
    raw.c_lflag &= ~(ICANON | ECHO | ECHONL | IEXTEN | ISIG);
    // IXON: otherwise ^S and ^Q stay XON/XOFF flow control and never arrive.
    //   ^S is cursor-left in the WordStar diamond and ^Q prefixes the entire
    //   ^Qx family, so half the diamond is dead without this. IXOFF is
    //   deliberately NOT cleared: it only governs the kernel sending XOFF to
    //   throttle a fast sender, it never consumes a typed ^S, and on a real
    //   serial console it is the only thing preventing a queue overrun.
    // ICRNL/INLCR/IGNCR: otherwise a typed Enter arrives as 0x0A, which is
    //   what makes Ctrl+J indistinguishable from Enter. With ICRNL cleared
    //   the tty delivers Enter as CR natively, so the tty read path below
    //   must NOT rewrite LF - the two changes go together or Enter breaks.
    // ISTRIP: keeps the 8th bit. Nothing on the input path masks to 7 bits
    //   (hbios_dispatch.cc CIOIN/VDAKRD, hbios_cpu.cc port 0x01 all pass the
    //   byte straight through), and with ISTRIP set a 0x8D byte was delivered
    //   as 0x0D - a spurious Enter. Note the deliberate asymmetry this
    //   creates with emu_console_write_char(), which still masks output to
    //   0x7F: that mask is there because WordStar sets bit 7 on the last
    //   character of a word and expects a dumb terminal to strip it, so the
    //   guest can now receive a byte it cannot echo.
    // IGNBRK/BRKINT/PARMRK: BRKINT is not gated on ISIG, so without this a
    //   BREAK on a real serial console still flushes both queues and raises
    //   SIGINT even though ISIG is cleared above. PARMRK matters for two
    //   independent reasons. First, and the only one that bites on a normal
    //   tty: PARMRK doubles a *valid* \377 to "\377 \377" whenever ISTRIP is
    //   clear, and it does so regardless of INPCK - so now that ~ISTRIP makes
    //   the guest 8-bit clean, a typed 0xFF would otherwise arrive twice.
    //   Second, on a serial console with parity checking actually enabled it
    //   would inject the three-byte \377 \0 X marker on a parity error.
    //   INPCK and IGNPAR are left as inherited; both are clear on a normal
    //   tty, where the kernel reports no parity error at all and delivers the
    //   byte unchanged.
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    // c_oflag is deliberately NOT touched - see the OPOST comment above.
    // cpmemu clears it; we cannot, because every output path here emits a
    // bare '\n' and relies on ONLCR.
    // c_cflag is not touched either: CSIZE/PARENB/CS8 would reprogram a real
    // serial console's line parameters, and ~ISTRIP above is what actually
    // preserves the 8th bit.
    // VMIN=1, matching cpmemu, because every read() here already sits behind
    // a select() and so never actually waits - and because it is what makes
    // n == 0 from a tty mean "hangup" and nothing else. With VMIN=0 a byte
    // stolen between the select and the read returns 0, which this file
    // treats as EOF; that latched a permanent false EOF and, now that a tty
    // EOF also winds the emulator down, would end the run outright. The
    // residual risk of VMIN=1 is the same rare race blocking a status poll
    // until the next keystroke - recoverable, unlike the alternative.
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_enabled = true;

    // One-time check: CP/M full-screen programs assume at least 80x24.
    // emu_io_init() is re-invoked by the sim> debugger's cooked-mode toggle,
    // so warn only once per process.
    static bool size_warned = false;
    if (!size_warned && isatty(STDOUT_FILENO)) {
      size_warned = true;
      struct winsize ws;
      if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
          ws.ws_col > 0 && ws.ws_row > 0 &&
          (ws.ws_col < 80 || ws.ws_row < 24)) {
        fprintf(stderr, "[WARN] Terminal is %dx%d; CP/M full-screen programs expect at least 80x24. Output may wrap or garble.\n",
                ws.ws_col, ws.ws_row);
      }
    }
  }
}

// Forward declaration for aux cleanup
static void close_aux_files();

void emu_io_cleanup() {
  restore_terminal();
  close_aux_files();
}

bool emu_console_has_input() {
  // Check queued input first
  if (!input_queue.empty()) return true;

  // Check peeked char
  if (peek_char >= 0) return true;

  // Check if at EOF
  if (stdin_eof) return false;

  // Check with select - no wait, just poll
  fd_set readfds;
  struct timeval tv;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;  // No timeout - instant check

  int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
  if (result <= 0) {
    return false;
  }

  // Select says readable - peek to distinguish data from EOF
  // Use read() instead of getchar() to avoid stdio buffering issues
  char buf;
  ssize_t n = read(STDIN_FILENO, &buf, 1);
  if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
    return false;  // interrupted or not ready - NOT end of input
  }
  if (n <= 0) {
    stdin_eof = true;
    return false;
  }
  // The reserved key must be filtered HERE, not only on the blocking read.
  // peek_char is drained by three consumers, and two of them - VDAKRD
  // (hbios_dispatch.cc) and IN port 0x01 (hbios_cpu.cc) - call this function
  // and emu_console_read_char() inside a SINGLE executed instruction, so
  // poll_stdin() never gets its chance to run emu_console_check_escape()
  // in between and the escape byte would go straight to the guest.
  // stdin_is_tty is load-bearing: the pipe read path deliberately does not
  // reserve anything (emu_console_check_escape returns early for a non-tty),
  // and escape_requested is tested before that early-out, so latching here
  // for a pipe would drop a piped script into sim> on its first 0x05.
  if (stdin_is_tty && current_escape_char != 0 &&
      (unsigned char)buf == (unsigned char)current_escape_char) {
    escape_requested = true;
    return false;
  }
  peek_char = (unsigned char)buf;
  return true;
}

int emu_console_read_char() {
  // Check queued input first. emu_console_queue_char() has no CLI caller
  // (only the web frontend uses it), but emu_console_check_escape() parks a
  // byte here when it has to look past one, so on a tty this is reachable -
  // and the LF rewrite therefore has to be gated exactly like the paths
  // below, or a tty Ctrl+J would come back out of the queue as Enter.
  if (!input_queue.empty()) {
    int ch = input_queue.front();
    input_queue.pop();
    if (!stdin_is_tty && ch == '\n') ch = '\r';  // LF -> CR (pipes only)
    return ch;
  }

  // Check peeked char. peek_char is filled from a tty as well as from a pipe
  // - emu_console_has_input() has no isatty guard and runs for both, and
  // poll_stdin() drives emu_console_check_escape() after every executed
  // instruction - so on an interactive run this is the usual delivery route,
  // not an edge case. The LF rewrite therefore has to be gated exactly the
  // way the two read paths below are, or a typed Ctrl+J becomes Enter again
  // through here.
  if (peek_char >= 0) {
    int ch = peek_char;
    peek_char = -1;
    if (!stdin_is_tty && ch == '\n') ch = '\r';  // LF -> CR (pipes only)
    return ch;
  }

  // Check EOF. Only a guest *read* past EOF counts as "input exhausted" -
  // emu_console_has_input() can detect EOF early during status polls while
  // the guest is still mid-command, and stopping then would cut it off. The
  // main loop exits through end of main so NVRAM and the trace file still
  // get written (see emu_console_input_exhausted).
  if (stdin_eof) {
    // A guest read past a latched EOF means no further input can arrive,
    // tty or not. Without the tty case a hung-up terminal left the guest
    // taking 0x1A at full speed forever, because CIOIN resets the idle
    // counter on every call so the main loop's idle sleep never engages.
    // Safe only because EINTR no longer latches stdin_eof (see above and the
    // tty read below).
    stdin_eof_consumed = true;
    return -1;
  }

  // For non-TTY (pipe), do a single blocking read
  if (!stdin_is_tty) {
    char buf;
    ssize_t n = read(STDIN_FILENO, &buf, 1);
    if (n > 0) {
      int ch = (unsigned char)buf;
      // Kept here: c_iflag never applies to a pipe, so a piped script really
      // does terminate its lines with LF.
      if (ch == '\n') ch = '\r';
      return ch;
    }
    if (n < 0 && errno == EINTR) return -1;  // signal, not EOF
    // EOF on pipe - remember it and let the main loop wind down
    stdin_eof = true;
    stdin_eof_consumed = true;
    return -1;
  }

  // TTY: use blocking select() to wait for input
  // No timeout - block until input arrives for maximum throughput
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  int sel = select(STDIN_FILENO + 1, &readfds, NULL, NULL, NULL);
  if (sel > 0) {
    char buf;
    ssize_t n = read(STDIN_FILENO, &buf, 1);
    if (n > 0) {
      int ch = (unsigned char)buf;
      // The escape key is reserved by the emulator: latch it for
      // emu_console_check_escape() and tell the caller there is no byte for
      // the guest. Returning it as well fired the guest's own binding for
      // that key too - one press of the default ^E both moved the WordStar
      // cursor up (CP/M 2.2 reads it as physical end of line) and dropped
      // into sim>. The cast matters: ch is 0..255 while current_escape_char
      // is a plain (signed) char, so an escape >= 0x80 would never match.
      if (current_escape_char != 0 &&
          ch == (unsigned char)current_escape_char) {
        escape_requested = true;
        return EMU_CONSOLE_RETRY;
      }
      // No LF -> CR here: emu_io_init() clears ICRNL, so the tty delivers
      // Enter as CR already and a 0x0A on this path is a real Ctrl+J, a
      // distinct key the guest is entitled to see. If ICRNL is ever restored
      // above, this rewrite has to come back with it.
      return ch;
    }
    if (n < 0 && errno == EINTR) return -1;  // signal, not EOF
    stdin_eof = true;                        // n == 0: real hangup
    return -1;
  }
  if (sel < 0 && errno == EINTR) {
    // A caught signal must unblock the guest so the main loop can observe
    // stop_requested - the handlers in romwbw_emu.cc are installed with
    // sa_flags = 0 precisely so they interrupt this select. It is NOT EOF:
    // latching stdin_eof here would make emu_console_has_input() answer "no
    // input" for the rest of the run, and console input would never recover.
    return -1;
  }
  // Real error - treat as EOF
  stdin_eof = true;
  return -1;
}

void emu_console_queue_char(int ch) {
  input_queue.push(ch);
}

void emu_console_write_char(uint8_t ch) {
  ch &= 0x7F;  // Strip high bit
  // CP/M sends \r\n, but Unix terminals only need \n
  // Skip \r to avoid double-spacing issues
  if (ch != '\r') {
    putchar(ch);
    fflush(stdout);
  }
}

bool emu_console_check_escape(char escape_char) {
  // 0 means the frontend reserves no key at all (--escape=none). Return
  // before the store below: current_escape_char is written ONLY here, and it
  // is what the blocking read consults, so leaving a stale value behind would
  // keep stealing the old key on the one path an interactive user actually
  // hits.
  if (escape_char == 0) return false;

  // Save escape char for blocking read to check
  current_escape_char = escape_char;

  // Check if escape was detected by blocking read
  if (escape_requested) {
    escape_requested = false;
    return true;
  }

  // stdin_is_tty, not isatty(): this runs once per executed Z80 instruction
  // via poll_stdin(), and the syscall dominated the profile (measured at over
  // 80% of syscall time on a piped run, every call failing with ENOTTY).
  if (!stdin_is_tty) return false;

  int esc = (unsigned char)escape_char;  // ch below is 0..255

  // Check peeked char
  if (peek_char >= 0) {
    if (peek_char == esc) {
      peek_char = -1;  // Consume
      return true;
    }
    // A byte the guest has not collected yet must not block escape
    // detection. peek_char holds exactly one byte and only a guest read
    // drains it, so a guest sitting in a compute loop with no console I/O
    // would park a byte here and make the escape key undetectable for the
    // rest of the run. Move it into the FIFO - which emu_console_read_char()
    // drains before peek_char, so ordering is preserved - and carry on to
    // the select() below.
    input_queue.push(peek_char);
    peek_char = -1;
  }

  // Non-blocking check with select
  fd_set readfds;
  struct timeval tv;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
    return false;
  }

  char buf;
  ssize_t n = read(STDIN_FILENO, &buf, 1);
  if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
    return false;  // interrupted or not ready - NOT end of input
  }
  if (n <= 0) {
    stdin_eof = true;
    return false;
  }
  int ch = (unsigned char)buf;
  if (ch == esc) {
    return true;
  }
  // Not escape - save for later
  peek_char = ch;
  return false;
}

//=============================================================================
// Auxiliary Device I/O Implementation
//=============================================================================

static FILE* printer_file = nullptr;
static FILE* aux_in_file = nullptr;
static FILE* aux_out_file = nullptr;

void emu_printer_set_file(const char* path) {
  if (printer_file) {
    fclose(printer_file);
    printer_file = nullptr;
  }
  if (path && *path) {
    printer_file = fopen(path, "w");
    if (!printer_file) {
      emu_error("Warning: Cannot open printer file '%s'\n", path);
    }
  }
}

void emu_printer_out(uint8_t ch) {
  if (printer_file) {
    fputc(ch & 0x7F, printer_file);
    fflush(printer_file);
  } else {
    // No printer file - output to stdout with prefix
    fprintf(stdout, "[PRINTER] %c", ch & 0x7F);
    fflush(stdout);
  }
}

bool emu_printer_ready() {
  return true;  // Always ready
}

void emu_aux_set_input_file(const char* path) {
  if (aux_in_file) {
    fclose(aux_in_file);
    aux_in_file = nullptr;
  }
  if (path && *path) {
    aux_in_file = fopen(path, "r");
    if (!aux_in_file) {
      emu_error("Warning: Cannot open aux input file '%s'\n", path);
    }
  }
}

void emu_aux_set_output_file(const char* path) {
  if (aux_out_file) {
    fclose(aux_out_file);
    aux_out_file = nullptr;
  }
  if (path && *path) {
    aux_out_file = fopen(path, "w");
    if (!aux_out_file) {
      emu_error("Warning: Cannot open aux output file '%s'\n", path);
    }
  }
}

int emu_aux_in() {
  if (aux_in_file) {
    int ch = fgetc(aux_in_file);
    if (ch == EOF) ch = 0x1A;  // ^Z on EOF
    return ch & 0x7F;
  }
  return 0x1A;  // ^Z if no file
}

void emu_aux_out(uint8_t ch) {
  if (aux_out_file) {
    fputc(ch & 0x7F, aux_out_file);
    fflush(aux_out_file);
  }
  // Silently ignore if no file
}

static void close_aux_files() {
  if (printer_file) {
    fclose(printer_file);
    printer_file = nullptr;
  }
  if (aux_in_file) {
    fclose(aux_in_file);
    aux_in_file = nullptr;
  }
  if (aux_out_file) {
    fclose(aux_out_file);
    aux_out_file = nullptr;
  }
}

//=============================================================================
// Debug/Log Output Implementation
//=============================================================================

static bool emu_debug_enabled = false;

void emu_set_debug(bool enable) {
  emu_debug_enabled = enable;
}

void emu_log(const char* fmt, ...) {
  if (!emu_debug_enabled) return;  // internal trace - only with --debug
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

void emu_error(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

void emu_fatal(const char* fmt, ...) {
  fprintf(stderr, "\n*** FATAL ERROR ***\n");
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n*** ABORTING ***\n");
  fflush(stderr);
  emu_io_cleanup();  // Restore terminal
  abort();
}

void emu_status(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

//=============================================================================
// Random Numbers Implementation
//=============================================================================

unsigned int emu_random(unsigned int min, unsigned int max) {
  if (min >= max) return min;
  std::uniform_int_distribution<unsigned int> dist(min, max);
  return dist(rng);
}

//=============================================================================
// Video/Display Implementation (CLI - minimal/no-op)
//=============================================================================

// CLI has no graphical display, but we can track cursor for basic support
static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t text_attr = 0x07;  // Default: white on black

void emu_video_get_caps(emu_video_caps* caps) {
  caps->has_text_display = false;  // No real text display in CLI
  caps->has_pixel_display = false;
  caps->has_dsky = false;
  caps->text_rows = 25;
  caps->text_cols = 80;
  caps->pixel_width = 0;
  caps->pixel_height = 0;
}

void emu_video_clear() {
  // Could send ANSI escape codes, but keep it simple
  cursor_row = 0;
  cursor_col = 0;
}

void emu_video_set_cursor(int row, int col) {
  cursor_row = row;
  cursor_col = col;
}

void emu_video_get_cursor(int* row, int* col) {
  *row = cursor_row;
  *col = cursor_col;
}

void emu_video_write_char(uint8_t ch) {
  // In CLI mode, video writes go to console
  emu_console_write_char(ch);
  cursor_col++;
}

void emu_video_write_char_at(int row, int col, uint8_t ch) {
  // No real positioning in CLI
  (void)row;
  (void)col;
  emu_console_write_char(ch);
}

void emu_video_scroll_up(int lines) {
  (void)lines;
  // No-op in CLI
}

void emu_video_set_attr(uint8_t attr) {
  text_attr = attr;
}

uint8_t emu_video_get_attr() {
  return text_attr;
}

// DSKY operations - no-op in CLI
void emu_dsky_show_hex(uint8_t position, uint8_t value) {
  (void)position;
  (void)value;
}

void emu_dsky_show_segments(uint8_t position, uint8_t segments) {
  (void)position;
  (void)segments;
}

void emu_dsky_set_leds(uint8_t leds) {
  (void)leds;
}

void emu_dsky_beep(int duration_ms) {
  (void)duration_ms;
  // Could output BEL character: putchar('\a');
}

int emu_dsky_get_key() {
  return -1;  // No DSKY keys in CLI
}

//=============================================================================
// Host File Transfer Implementation (CLI - uses direct file I/O)
//=============================================================================

static FILE* cli_host_read_file = nullptr;
static FILE* cli_host_write_file = nullptr;
static std::string cli_host_write_filename;
static emu_host_file_state cli_host_state = HOST_FILE_IDLE;

// Resolve a path by matching each component case-insensitively against the
// directory entries on disk. The CP/M CCP uppercases the command tail before
// R8 copies it (see src/r8.asm), so "R8 /tmp/lower/file.txt" arrives here as
// "/TMP/LOWER/FILE.TXT" and an exact fopen fails. Exact-case components are
// preferred (checked first); among multiple case-variants the first readdir
// hit wins. Returns "" if any component has no match.
static std::string resolve_path_case_insensitive(const char* path) {
  std::string dir;                 // parent resolved so far ("" = cwd)
  std::string resolved;            // resolved path being built
  const char* p = path;
  if (*p == '/') {
    dir = "/";
    resolved = "/";
    while (*p == '/') p++;
  }
  while (*p) {
    const char* start = p;
    while (*p && *p != '/') p++;
    std::string comp(start, p - start);
    while (*p == '/') p++;
    if (comp.empty()) continue;
    std::string sep = (resolved.empty() || resolved == "/") ? "" : "/";
    if (comp == "." || comp == "..") {
      resolved += sep + comp;
      dir = resolved;
      continue;
    }
    // Exact match first
    std::string candidate = resolved + sep + comp;
    struct stat st;
    if (stat(candidate.c_str(), &st) == 0) {
      resolved = candidate;
      dir = resolved;
      continue;
    }
    // Case-insensitive scan of the parent directory
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d) return "";
    std::string match;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      if (emu_strcasecmp(ent->d_name, comp.c_str()) == 0) {
        match = ent->d_name;
        break;
      }
    }
    closedir(d);
    if (match.empty()) return "";
    resolved += sep + match;
    dir = resolved;
  }
  return resolved;
}

void emu_console_clear_queue() {
  // Clear any queued input - not much to do in CLI
}

bool emu_console_input_exhausted() {
  // True once the guest has read past a latched EOF - a hung-up tty as well
  // as a drained pipe/file: no further input can ever arrive and the guest
  // has drained everything queued, so the main loop should wind down
  // cleanly. The tty case is what stops a closed terminal leaving the guest
  // taking 0x1A at full speed forever.
  return stdin_eof_consumed;
}

bool emu_console_input_eof() {
  return stdin_eof && !stdin_is_tty && input_queue.empty() && peek_char < 0;
}

emu_host_file_state emu_host_file_get_state() {
  return cli_host_state;
}

// fopen("rb") succeeds on a directory on Linux and macOS - only the first read
// fails - so without this R8 opened /tmp/somedir, reported success, and the
// guest then went on to F_DELETE and F_MAKE before discovering an instant EOF.
// A CP/M file of the same name was destroyed and replaced with an empty one,
// and "Done: 0 bytes" is indistinguishable from importing a genuinely empty
// file. The guest cannot tell the two apart, so the refusal has to be here;
// failing the open is what makes R8's existing host_open_error fire, which it
// does before the delete.
static bool is_directory(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool emu_host_file_open_read(const char* filename) {
  if (cli_host_read_file) {
    fclose(cli_host_read_file);
    cli_host_read_file = nullptr;
  }
  if (is_directory(filename)) {
    cli_host_state = HOST_FILE_IDLE;
    return false;
  }
  cli_host_read_file = fopen(filename, "rb");
  if (!cli_host_read_file) {
    // The guest's CCP uppercased the path; retry case-insensitively
    std::string alt = resolve_path_case_insensitive(filename);
    if (!alt.empty() && alt != filename && !is_directory(alt.c_str())) {
      cli_host_read_file = fopen(alt.c_str(), "rb");
      if (cli_host_read_file) {
        emu_log("[HOST] Resolved '%s' as '%s'\n", filename, alt.c_str());
      }
    }
  }
  if (cli_host_read_file) {
    cli_host_state = HOST_FILE_READING;
    return true;
  }
  cli_host_state = HOST_FILE_IDLE;
  return false;
}

// Put the case back into a path the CCP uppercased, for a file that does not
// exist yet.
//
// resolve_path_case_insensitive() cannot be used as-is here: it requires every
// component to exist, and the last one is the file we are about to create. So
// resolve only the parent - which does have to exist - and lowercase the
// basename, which is the same convention W8 has always used for the name it
// derives from the FCB. CP/M destroys the typed case before we ever see it, so
// lowercase is a choice, not a recovery; it is documented as such in README.
static std::string resolve_write_path(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  for (char& c : base) c = (char)tolower((unsigned char)c);
  if (slash == std::string::npos) return base;          // no directory part
  std::string dir = path.substr(0, slash);
  if (dir.empty()) return "/" + base;                   // path was "/name"
  std::string resolved_dir = resolve_path_case_insensitive(dir.c_str());
  if (resolved_dir.empty()) resolved_dir = dir;         // no match: use as typed
  // Canonicalise the parent. W8 prints this back to the user (HBF_HOST_GETNAME)
  // and the shouted directory is not always corrected by the scan above: on a
  // case-insensitive volume the exact-match branch accepts "/TMP/W8T" as it
  // stands, so without this the answer would be a path that happens to open the
  // right file while looking nothing like where the user should go looking.
  // Only the parent - the basename names a file that does not exist yet.
  // realpath(p, NULL) rather than a fixed buffer: POSIX requires a caller-
  // supplied buffer to be at least PATH_MAX, which is a compile-time constant
  // that does not bound a real path on every filesystem, and the NULL form
  // allocates exactly what is needed. Failure is not an error here - the
  // directory may be unreadable while still being writable through - so keep
  // the case-insensitive resolution as the answer.
  char* real = realpath(resolved_dir.c_str(), nullptr);
  if (real) {
    resolved_dir = real;
    free(real);
  }
  return resolved_dir + "/" + base;
}

// Make a resolved path absolute, so what W8 prints back names a place rather
// than a name. "w8.com" answers "where did it go" only for someone who also
// knows which directory the emulator was started from, and the guest cannot
// see that; "/home/me/build/w8.com" needs no such context. Purely a matter of
// which string we keep - the two open the same file.
static std::string absolutise(const std::string& path) {
  if (!path.empty() && path[0] == '/') return path;
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) return path;   // ENAMETOOLONG etc: say less
  std::string dir(cwd);
  if (dir.empty() || dir[dir.size() - 1] != '/') dir += "/";
  return dir + path;
}

bool emu_host_file_open_write(const char* filename) {
  if (cli_host_write_file) {
    fclose(cli_host_write_file);
    cli_host_write_file = nullptr;
  }
  cli_host_write_filename = filename ? filename : "output.bin";
  cli_host_write_filename = absolutise(resolve_write_path(cli_host_write_filename));
  cli_host_write_file = fopen(cli_host_write_filename.c_str(), "wb");
  if (cli_host_write_file) {
    cli_host_state = HOST_FILE_WRITING;
    return true;
  }
  // A failed open leaves no destination to report. Clearing it is what stops
  // emu_host_file_get_write_name() naming a file that was never created.
  cli_host_write_filename.clear();
  cli_host_state = HOST_FILE_IDLE;
  return false;
}

int emu_host_file_read_byte() {
  if (!cli_host_read_file) return -1;
  int ch = fgetc(cli_host_read_file);
  return (ch == EOF) ? -1 : ch;
}

bool emu_host_file_write_byte(uint8_t byte) {
  if (!cli_host_write_file) return false;
  return fputc(byte, cli_host_write_file) != EOF;
}

void emu_host_file_close_read() {
  if (cli_host_read_file) {
    fclose(cli_host_read_file);
    cli_host_read_file = nullptr;
  }
  cli_host_state = HOST_FILE_IDLE;
}

bool emu_host_file_close_write() {
  bool ok = true;
  if (cli_host_write_file) {
    // fputc buffers; ENOSPC etc. can surface only at the final flush in fclose
    ok = (fclose(cli_host_write_file) == 0);
    cli_host_write_file = nullptr;
  }
  cli_host_state = HOST_FILE_IDLE;
  return ok;
}

void emu_host_file_provide_data(const uint8_t* data, size_t size) {
  // Not used in CLI - files are read directly
  (void)data;
  (void)size;
}

const uint8_t* emu_host_file_get_write_data() {
  return nullptr;  // Not used in CLI
}

size_t emu_host_file_get_write_size() {
  return 0;  // Not used in CLI
}

// Only meaningful while a write file is open - see emu_io.h. Without the state
// test this served the previous transfer's path to anyone who asked between
// two exports, which is exactly the wrong answer to give a guest that is about
// to print it as "the file went here".
const char* emu_host_file_get_write_name() {
  if (cli_host_state != HOST_FILE_WRITING) return "";
  return cli_host_write_filename.c_str();
}
