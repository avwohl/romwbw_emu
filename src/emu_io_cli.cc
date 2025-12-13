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
#include <ctime>
#include <queue>
#include <random>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>

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
static int peek_char = -1;

// Ctrl+C tracking
static int consecutive_ctrl_c = 0;

// Random number generator
static std::mt19937 rng(std::random_device{}());

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
  // Save original terminal settings if we're a TTY
  if (isatty(STDIN_FILENO)) {
    if (!termios_saved) {
      tcgetattr(STDIN_FILENO, &original_termios);
      termios_saved = true;
      atexit(restore_terminal);
    }

    // Enable raw mode
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 0;   // Non-blocking
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_enabled = true;
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

  // Check with select
  fd_set readfds;
  struct timeval tv;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
    return false;
  }

  // Select says readable - peek to distinguish data from EOF
  int ch = getchar();
  if (ch == EOF) {
    stdin_eof = true;
    return false;
  }
  peek_char = ch;
  return true;
}

int emu_console_read_char() {
  // Check queued input first
  if (!input_queue.empty()) {
    int ch = input_queue.front();
    input_queue.pop();
    if (ch == '\n') ch = '\r';  // LF -> CR for CP/M
    return ch;
  }

  // Check peeked char
  if (peek_char >= 0) {
    int ch = peek_char;
    peek_char = -1;
    if (ch == '\n') ch = '\r';
    return ch;
  }

  // Check EOF
  if (stdin_eof) return -1;

  // Read from stdin (blocking)
  int ch = getchar();
  if (ch == EOF) {
    stdin_eof = true;
    return -1;
  }
  if (ch == '\n') ch = '\r';
  return ch;
}

void emu_console_queue_char(int ch) {
  input_queue.push(ch);
}

void emu_console_write_char(uint8_t ch) {
  putchar(ch & 0x7F);
  fflush(stdout);
}

bool emu_console_check_escape(char escape_char) {
  if (!isatty(STDIN_FILENO)) return false;

  // Check peeked char
  if (peek_char >= 0) {
    if (peek_char == escape_char) {
      peek_char = -1;  // Consume
      return true;
    }
    return false;
  }

  // Check with select
  fd_set readfds;
  struct timeval tv;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
    return false;
  }

  int ch = getchar();
  if (ch == EOF) {
    stdin_eof = true;
    return false;
  }
  if (ch == escape_char) {
    return true;
  }
  // Not escape - save for later
  peek_char = ch;
  return false;
}

bool emu_console_check_ctrl_c_exit(int ch, int count) {
  if (ch == 0x03) {
    consecutive_ctrl_c++;
    if (consecutive_ctrl_c >= count) {
      emu_error("\n[Exiting: %d consecutive ^C received]\n", count);
      emu_io_cleanup();
      exit(0);
    }
    return false;
  } else {
    consecutive_ctrl_c = 0;
    return false;
  }
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

void emu_log(const char* fmt, ...) {
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

void emu_status(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

//=============================================================================
// File I/O Implementation
//=============================================================================

bool emu_file_load(const std::string& path, std::vector<uint8_t>& data) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    data.clear();
    return false;
  }

  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);

  data.resize(size);
  size_t read = fread(data.data(), 1, size, f);
  fclose(f);

  if (read != size) {
    data.clear();
    return false;
  }
  return true;
}

size_t emu_file_load_to_mem(const std::string& path, uint8_t* mem,
                            size_t mem_size, size_t offset) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0;

  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  size_t to_read = file_size;
  if (offset + to_read > mem_size) {
    to_read = mem_size - offset;
  }

  size_t read = fread(mem + offset, 1, to_read, f);
  fclose(f);
  return read;
}

bool emu_file_save(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;

  size_t written = fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  return written == data.size();
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
// Disk Image I/O Implementation
//=============================================================================

struct disk_file {
  FILE* fp;
  size_t size;
};

emu_disk_handle emu_disk_open(const std::string& path, const char* mode) {
  const char* fmode;
  if (strcmp(mode, "r") == 0) {
    fmode = "rb";
  } else if (strcmp(mode, "rw") == 0) {
    fmode = "r+b";
  } else if (strcmp(mode, "rw+") == 0) {
    // Try to open existing, create if not exists
    fmode = "r+b";
    FILE* f = fopen(path.c_str(), fmode);
    if (!f) {
      f = fopen(path.c_str(), "w+b");
    }
    if (!f) return nullptr;

    disk_file* disk = new disk_file;
    disk->fp = f;
    fseek(f, 0, SEEK_END);
    disk->size = ftell(f);
    return disk;
  } else {
    return nullptr;
  }

  FILE* f = fopen(path.c_str(), fmode);
  if (!f) return nullptr;

  disk_file* disk = new disk_file;
  disk->fp = f;
  fseek(f, 0, SEEK_END);
  disk->size = ftell(f);
  return disk;
}

void emu_disk_close(emu_disk_handle handle) {
  if (!handle) return;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (disk->fp) fclose(disk->fp);
  delete disk;
}

size_t emu_disk_read(emu_disk_handle handle, size_t offset,
                     uint8_t* buffer, size_t count) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (!disk->fp) return 0;

  fseek(disk->fp, offset, SEEK_SET);
  return fread(buffer, 1, count, disk->fp);
}

size_t emu_disk_write(emu_disk_handle handle, size_t offset,
                      const uint8_t* buffer, size_t count) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (!disk->fp) return 0;

  fseek(disk->fp, offset, SEEK_SET);
  size_t written = fwrite(buffer, 1, count, disk->fp);

  // Update size if we wrote past the end
  size_t new_end = offset + written;
  if (new_end > disk->size) {
    disk->size = new_end;
  }

  return written;
}

void emu_disk_flush(emu_disk_handle handle) {
  if (!handle) return;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (disk->fp) fflush(disk->fp);
}

size_t emu_disk_size(emu_disk_handle handle) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  return disk->size;
}

//=============================================================================
// Time Implementation
//=============================================================================

void emu_get_time(emu_time* t) {
  time_t now = time(nullptr);
  struct tm* tm = localtime(&now);

  t->year = tm->tm_year + 1900;
  t->month = tm->tm_mon + 1;
  t->day = tm->tm_mday;
  t->hour = tm->tm_hour;
  t->minute = tm->tm_min;
  t->second = tm->tm_sec;
  t->weekday = tm->tm_wday;
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
