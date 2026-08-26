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
#include <strings.h>
#include <queue>
#include <random>

//=============================================================================
// Platform Utilities
//=============================================================================

int emu_strcasecmp(const char* s1, const char* s2) {
  return strcasecmp(s1, s2);
}

int emu_strncasecmp(const char* s1, const char* s2, size_t n) {
  return strncasecmp(s1, s2, n);
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

// Debug logging enabled flag (volatile to prevent optimizer from removing checks)
static volatile bool emu_debug_enabled = false;

// Input queue for async keyboard input
static std::queue<int> input_queue;

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

void emu_sleep_ms(int ms) {
  // In WebAssembly, we can't really sleep - just yield
  // The blocking_allowed flag should be false for web anyway
  (void)ms;
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
  // Deliberately no LF -> CR rewrite. Nothing that reaches this queue in the
  // browser can be a stray 0x0A: xterm.js maps Enter to CR and normalises
  // pasted text with replace(/\r?\n/g, "\r") before term.onData sees it. The
  // only 0x0A that ever arrives is a deliberate Ctrl+J, which belongs to the
  // guest - it is line-feed in ED and cursor-down in the WordStar family.
  // (The CLI backend still rewrites on its pipe path, where a script really
  // does end its lines with LF. Do not unify the two.)
  return ch;
}

void emu_console_queue_char(int ch) {
  input_queue.push(ch);
}

void emu_console_write_char(uint8_t ch) {
  ch &= 0x7F;  // Strip high bit
  // Skip CR - browsers only need LF for line endings
  if (ch != '\r') {
    js_console_output(ch);
  }
}

bool emu_console_check_escape(char escape_char) {
  // Unreachable in this build, and deliberately a no-op rather than the
  // queue-pop it used to be. The only callers are in romwbw_emu.cc, which the
  // web target does not compile (see web/makefile), and there is no sim>
  // debugger in the browser to escape to - so popping would only have taken
  // a key away from the guest for nothing. The old comment claimed
  // JavaScript handled the escape; no page in this repo does.
  (void)escape_char;
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

void emu_set_debug(bool enable) {
  emu_debug_enabled = enable;
}

void emu_log(const char* fmt, ...) {
  if (!emu_debug_enabled) return;  // Only log when debug enabled
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

void emu_fatal(const char* fmt, ...) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "*** FATAL ERROR ***\n");
  js_error(buf);
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  js_error(buf);
  js_error("*** ABORTING ***\n");
  abort();
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

// Queue a key from JavaScript.
//
// EMSCRIPTEN_KEEPALIVE exports this even though it is not in the makefile's
// EXPORTED_FUNCTIONS list, so it is reachable from a page - but no page in
// this repo calls it, and it is NOT the key entry point. Use
// _romwbw_key_input (web/romwbw_web.cc): this one does not clear the
// waiting-for-input flag, so a key queued here never wakes a guest parked in
// CIOIN.
extern "C" EMSCRIPTEN_KEEPALIVE
void emu_queue_key(int ch) {
  emu_console_queue_char(ch);
}

//=============================================================================
// Host File Transfer Implementation
//=============================================================================

// JavaScript callbacks for host file transfer
EM_JS(void, js_host_file_request_read, (const char* filename), {
  if (Module.onHostFileRequestRead) Module.onHostFileRequestRead(UTF8ToString(filename));
});

EM_JS(void, js_host_file_download, (const char* filename, const uint8_t* data, int size), {
  if (Module.onHostFileDownload) {
    var arr = new Uint8Array(Module.HEAPU8.buffer, data, size);
    var blob = new Blob([arr], {type: 'application/octet-stream'});
    Module.onHostFileDownload(UTF8ToString(filename), blob);
  }
});

// State
static emu_host_file_state host_file_state = HOST_FILE_IDLE;
static std::vector<uint8_t> host_read_buffer;
static size_t host_read_pos = 0;
static std::vector<uint8_t> host_write_buffer;
static std::string host_write_filename;

void emu_console_clear_queue() {
  while (!input_queue.empty()) input_queue.pop();
}

bool emu_console_input_exhausted() {
  return false;  // browser input can always arrive later
}

bool emu_console_input_eof() {
  return false;
}

emu_host_file_state emu_host_file_get_state() {
  return host_file_state;
}

bool emu_host_file_open_read(const char* filename) {
  // Close any existing file
  host_read_buffer.clear();
  host_read_pos = 0;

  // Request file from JavaScript
  host_file_state = HOST_FILE_WAITING_READ;
  js_host_file_request_read(filename);
  return true;
}

// The browser download reduces any path to its last component
// (emu_host_path_basename, in emu_host_file_open_write below), so a guest path
// cannot escape to a directory - there is no directory. It confines, so it
// promises EMU_HOST_CAP_SAFE_PATHS and W8 will send a path, which becomes the
// suggested download name.
uint8_t emu_host_path_caps() {
  return EMU_HOST_CAP_SAFE_PATHS;
}

bool emu_host_file_open_write(const char* filename) {
  // Close any existing write buffer
  host_write_buffer.clear();
  // W8 can be given a host path (src/w8.asm), which means what arrives here may
  // contain directories. The browser has no filesystem to honour them with -
  // this becomes the suggested name on a download - and a name with separators
  // in it is not a usable download filename, so keep the basename only. A path
  // is not an error here, it just cannot mean what it means on the CLI, and W8
  // asks (HBF_HOST_GETNAME) for the name that comes out of this so it can tell
  // the user what the download will actually be called.
  host_write_filename =
      emu_host_path_basename(filename ? filename : "download.bin", "download.bin");
  // Lowercase it, for the same reason the CLI backend lowercases the name it
  // creates: the CCP shouted the whole command line, so the alternative is a
  // browser download called MYFILE.TXT. The typed case is gone before either
  // backend sees it, so this is a convention rather than a recovery - and the
  // two front ends have to pick the same one or the same W8 command produces
  // differently-named files.
  for (char& c : host_write_filename) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  host_file_state = HOST_FILE_WRITING;
  return true;
}

int emu_host_file_read_byte() {
  if (host_file_state != HOST_FILE_READING) return -1;
  if (host_read_pos >= host_read_buffer.size()) return -1;
  return host_read_buffer[host_read_pos++];
}

bool emu_host_file_write_byte(uint8_t byte) {
  if (host_file_state != HOST_FILE_WRITING) return false;
  host_write_buffer.push_back(byte);
  return true;
}

void emu_host_file_close_read() {
  host_read_buffer.clear();
  host_read_pos = 0;
  host_file_state = HOST_FILE_IDLE;
}

bool emu_host_file_close_write() {
  if (host_file_state == HOST_FILE_WRITING) {
    // Download even when the buffer is empty. An empty CP/M file is a real
    // file, the CLI and Windows backends both create it, and dropping it here
    // meant W8 printed "Done: 0 bytes" in the browser with nothing arriving -
    // indistinguishable from a browser that blocked the download.
    // data() on an empty vector may be null; the JS side takes a length too.
    js_host_file_download(host_write_filename.c_str(),
                          host_write_buffer.data(),
                          host_write_buffer.size());
  }
  host_write_buffer.clear();
  host_write_filename.clear();
  host_file_state = HOST_FILE_IDLE;
  return true;  // browser download cannot fail synchronously
}

void emu_host_file_provide_data(const uint8_t* data, size_t size) {
  host_read_buffer.assign(data, data + size);
  host_read_pos = 0;
  host_file_state = HOST_FILE_READING;
}

const uint8_t* emu_host_file_get_write_data() {
  return host_write_buffer.empty() ? nullptr : host_write_buffer.data();
}

size_t emu_host_file_get_write_size() {
  return host_write_buffer.size();
}

const char* emu_host_file_get_write_name() {
  return host_write_filename.c_str();
}

// The browser genuinely cannot say, so it says nothing, which is the documented
// "" answer in emu_io.h: HBF_HOST_GETRNAME reports no answer and R8 prints the
// path that was typed, exactly as it does today.
//
// Echoing the request would be worse here than anywhere else. A read in this
// front end opens a FILE PICKER (js_host_file_request_read above) and the
// guest's string is only a hint the user is free to ignore, so the file that
// arrives routinely has nothing to do with what was typed - the one case where
// printing the request is not merely uninformative but wrong.
//
// Answering properly needs the picked file's name to come back with its bytes,
// which is a change to emu_host_file_provide_data() and to the page's picker
// callback. Left undone deliberately: emcc is not a build dependency of this
// repository, so nothing here can compile or run the result. See todo.txt.
const char* emu_host_file_get_read_name() {
  return "";
}

// Exported function for JavaScript to provide file data after picker
extern "C" EMSCRIPTEN_KEEPALIVE
void emu_host_file_load(const uint8_t* data, int size) {
  emu_host_file_provide_data(data, size);
}

// Exported function for JavaScript to cancel file read
extern "C" EMSCRIPTEN_KEEPALIVE
void emu_host_file_cancel() {
  host_file_state = HOST_FILE_IDLE;
  host_read_buffer.clear();
  host_read_pos = 0;
}

#endif // __EMSCRIPTEN__
