/*
 * Emulator I/O Implementation - WebAssembly (Emscripten)
 *
 * This implementation uses Emscripten's EM_JS macros to call
 * JavaScript callbacks for all I/O operations.
 */

#ifdef __EMSCRIPTEN__

#include "emu_io.h"
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <queue>
#include <random>

//=============================================================================
// JavaScript Callbacks
//=============================================================================

// Console output - calls Module.onConsoleOutput(ch) in JavaScript
EM_JS(void, js_console_output, (int ch), {
  if (Module.onConsoleOutput) Module.onConsoleOutput(ch);
});

// Status message - calls Module.onStatus(msg) in JavaScript
EM_JS(void, js_status, (const char* msg), {
  if (Module.onStatus) Module.onStatus(UTF8ToString(msg));
});

// Log message - calls Module.onLog(msg) in JavaScript (optional)
EM_JS(void, js_log, (const char* msg), {
  if (Module.onLog) Module.onLog(UTF8ToString(msg));
  else console.log(UTF8ToString(msg));
});

// Error message - calls Module.onError(msg) in JavaScript
EM_JS(void, js_error, (const char* msg), {
  if (Module.onError) Module.onError(UTF8ToString(msg));
  else console.error(UTF8ToString(msg));
});

// Printer output - calls Module.onPrinterOutput(ch) in JavaScript (optional)
EM_JS(void, js_printer_output, (int ch), {
  if (Module.onPrinterOutput) Module.onPrinterOutput(ch);
});

// DSKY hex display - calls Module.onDskyHex(pos, value) in JavaScript (optional)
EM_JS(void, js_dsky_hex, (int pos, int value), {
  if (Module.onDskyHex) Module.onDskyHex(pos, value);
});

// DSKY segments - calls Module.onDskySegments(pos, segs) in JavaScript (optional)
EM_JS(void, js_dsky_segments, (int pos, int segs), {
  if (Module.onDskySegments) Module.onDskySegments(pos, segs);
});

// DSKY LEDs - calls Module.onDskyLeds(leds) in JavaScript (optional)
EM_JS(void, js_dsky_leds, (int leds), {
  if (Module.onDskyLeds) Module.onDskyLeds(leds);
});

// DSKY beep - calls Module.onDskyBeep(ms) in JavaScript (optional)
EM_JS(void, js_dsky_beep, (int ms), {
  if (Module.onDskyBeep) Module.onDskyBeep(ms);
});

// Video clear - calls Module.onVideoClear() in JavaScript (optional)
EM_JS(void, js_video_clear, (), {
  if (Module.onVideoClear) Module.onVideoClear();
});

// Video set cursor - calls Module.onVideoSetCursor(row, col) in JavaScript (optional)
EM_JS(void, js_video_set_cursor, (int row, int col), {
  if (Module.onVideoSetCursor) Module.onVideoSetCursor(row, col);
});

// Video write char - calls Module.onVideoWriteChar(ch) in JavaScript (optional)
EM_JS(void, js_video_write_char, (int ch), {
  if (Module.onVideoWriteChar) Module.onVideoWriteChar(ch);
});

//=============================================================================
// Internal State
//=============================================================================

// Input queue for async keyboard input
static std::queue<int> input_queue;

// Ctrl+C tracking
static int consecutive_ctrl_c = 0;

// Random number generator
static std::mt19937 rng(42);  // Fixed seed for reproducibility in WASM

// Video state
static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t text_attr = 0x07;

// Auxiliary device state (file-based, using Emscripten virtual filesystem)
static FILE* printer_file = nullptr;
static FILE* aux_in_file = nullptr;
static FILE* aux_out_file = nullptr;

//=============================================================================
// Console I/O Implementation
//=============================================================================

void emu_io_init() {
  // Nothing special needed for WebAssembly
}

void emu_io_cleanup() {
  // Close any open aux files
  if (printer_file) { fclose(printer_file); printer_file = nullptr; }
  if (aux_in_file) { fclose(aux_in_file); aux_in_file = nullptr; }
  if (aux_out_file) { fclose(aux_out_file); aux_out_file = nullptr; }
}

bool emu_console_has_input() {
  return !input_queue.empty();
}

int emu_console_read_char() {
  if (input_queue.empty()) {
    return -1;  // No input available
  }
  int ch = input_queue.front();
  input_queue.pop();
  if (ch == '\n') ch = '\r';  // LF -> CR for CP/M
  return ch;
}

void emu_console_queue_char(int ch) {
  input_queue.push(ch);
}

void emu_console_write_char(uint8_t ch) {
  js_console_output(ch & 0x7F);
}

bool emu_console_check_escape(char escape_char) {
  // In WebAssembly, escape is typically handled by JavaScript
  // Check if escape char is at front of queue
  if (!input_queue.empty() && input_queue.front() == escape_char) {
    input_queue.pop();
    return true;
  }
  return false;
}

bool emu_console_check_ctrl_c_exit(int ch, int count) {
  if (ch == 0x03) {
    consecutive_ctrl_c++;
    if (consecutive_ctrl_c >= count) {
      js_error("[Exiting: consecutive ^C received]");
      // In WASM, we can't really exit - just signal the error
      return true;
    }
  } else {
    consecutive_ctrl_c = 0;
  }
  return false;
}

//=============================================================================
// Auxiliary Device I/O Implementation
//=============================================================================

void emu_printer_set_file(const char* path) {
  if (printer_file) {
    fclose(printer_file);
    printer_file = nullptr;
  }
  if (path && *path) {
    printer_file = fopen(path, "w");
  }
}

void emu_printer_out(uint8_t ch) {
  if (printer_file) {
    fputc(ch & 0x7F, printer_file);
    fflush(printer_file);
  } else {
    js_printer_output(ch & 0x7F);
  }
}

bool emu_printer_ready() {
  return true;
}

void emu_aux_set_input_file(const char* path) {
  if (aux_in_file) {
    fclose(aux_in_file);
    aux_in_file = nullptr;
  }
  if (path && *path) {
    aux_in_file = fopen(path, "r");
  }
}

void emu_aux_set_output_file(const char* path) {
  if (aux_out_file) {
    fclose(aux_out_file);
    aux_out_file = nullptr;
  }
  if (path && *path) {
    aux_out_file = fopen(path, "w");
  }
}

int emu_aux_in() {
  if (aux_in_file) {
    int ch = fgetc(aux_in_file);
    if (ch == EOF) return 0x1A;  // ^Z on EOF
    return ch & 0x7F;
  }
  return 0x1A;  // ^Z if no file
}

void emu_aux_out(uint8_t ch) {
  if (aux_out_file) {
    fputc(ch & 0x7F, aux_out_file);
    fflush(aux_out_file);
  }
}

//=============================================================================
// Debug/Log Output Implementation
//=============================================================================

void emu_log(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  js_log(buf);
}

void emu_error(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  js_error(buf);
}

void emu_status(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  js_status(buf);
}

//=============================================================================
// File I/O Implementation (uses Emscripten virtual filesystem)
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
  FILE* f = fopen(path.c_str(), "rb");
  if (f) {
    fclose(f);
    return true;
  }
  return false;
}

size_t emu_file_size(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fclose(f);
  return size;
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
// Video/Display Implementation
//=============================================================================

void emu_video_get_caps(emu_video_caps* caps) {
  // WebAssembly can support various displays via JavaScript
  caps->has_text_display = true;
  caps->has_pixel_display = false;
  caps->has_dsky = true;  // DSKY support via callbacks
  caps->text_rows = 25;
  caps->text_cols = 80;
  caps->pixel_width = 0;
  caps->pixel_height = 0;
}

void emu_video_clear() {
  cursor_row = 0;
  cursor_col = 0;
  js_video_clear();
}

void emu_video_set_cursor(int row, int col) {
  cursor_row = row;
  cursor_col = col;
  js_video_set_cursor(row, col);
}

void emu_video_get_cursor(int* row, int* col) {
  *row = cursor_row;
  *col = cursor_col;
}

void emu_video_write_char(uint8_t ch) {
  js_video_write_char(ch);
  cursor_col++;
}

void emu_video_write_char_at(int row, int col, uint8_t ch) {
  js_video_set_cursor(row, col);
  js_video_write_char(ch);
}

void emu_video_scroll_up(int lines) {
  (void)lines;
  // Scroll would need JavaScript implementation
}

void emu_video_set_attr(uint8_t attr) {
  text_attr = attr;
}

uint8_t emu_video_get_attr() {
  return text_attr;
}

// DSKY operations
void emu_dsky_show_hex(uint8_t position, uint8_t value) {
  js_dsky_hex(position, value);
}

void emu_dsky_show_segments(uint8_t position, uint8_t segments) {
  js_dsky_segments(position, segments);
}

void emu_dsky_set_leds(uint8_t leds) {
  js_dsky_leds(leds);
}

void emu_dsky_beep(int duration_ms) {
  js_dsky_beep(duration_ms);
}

int emu_dsky_get_key() {
  // DSKY key input would come through the input queue
  return -1;
}

//=============================================================================
// Exported Functions for JavaScript
//=============================================================================

// Queue a key from JavaScript
extern "C" EMSCRIPTEN_KEEPALIVE
void emu_queue_key(int ch) {
  emu_console_queue_char(ch);
}

#endif // __EMSCRIPTEN__
