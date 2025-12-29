/*
 * Emulator I/O Abstraction Layer
 *
 * This header defines the I/O interface used by the emulator core.
 * Different implementations can be provided for CLI (Unix terminal)
 * and WebAssembly (browser JavaScript callbacks).
 *
 * The emulator core should only use these functions for I/O,
 * never directly using stdio, termios, etc.
 */

#ifndef EMU_IO_H
#define EMU_IO_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

//=============================================================================
// Platform Utilities - portable replacements for platform-specific functions
//=============================================================================

// Sleep for specified milliseconds (portable replacement for usleep/Sleep)
void emu_sleep_ms(int ms);

// Case-insensitive string compare (portable replacement for strcasecmp/_stricmp)
int emu_strcasecmp(const char* s1, const char* s2);

// Case-insensitive string compare with length limit (portable strncasecmp/_strnicmp)
int emu_strncasecmp(const char* s1, const char* s2, size_t n);

//=============================================================================
// Console I/O - for emulated terminal
//=============================================================================

// Initialize the I/O system (call once at startup)
void emu_io_init();

// Cleanup (call at exit or when switching modes)
void emu_io_cleanup();

// Check if console input is available (non-blocking)
// Returns true if a character is waiting to be read
bool emu_console_has_input();

// Read a character from console (may block if no input)
// Returns the character, or -1 on EOF
// LF is converted to CR for CP/M compatibility
int emu_console_read_char();

// Queue a character for input (for async input sources)
void emu_console_queue_char(int ch);

// Clear the input queue (call on reset)
void emu_console_clear_queue();

// Write a character to console
void emu_console_write_char(uint8_t ch);

// Check for escape sequence (for entering debug console)
// escape_char: the escape character to look for
// Returns true if escape was detected and consumed
bool emu_console_check_escape(char escape_char);

// Check for repeated Ctrl+C exit condition
// ch: the character just read
// count: how many consecutive Ctrl+C required to exit
// Returns true if exit threshold reached
bool emu_console_check_ctrl_c_exit(int ch, int count);

//=============================================================================
// Auxiliary Device I/O - for printer, punch, reader
//=============================================================================

// Set printer (LST:) output file path (nullptr to close)
void emu_printer_set_file(const char* path);

// Printer output - writes character to printer device
void emu_printer_out(uint8_t ch);

// Printer status - returns true if printer is ready
bool emu_printer_ready();

// Set auxiliary input (RDR:) file path (nullptr to close)
void emu_aux_set_input_file(const char* path);

// Set auxiliary output (PUN:) file path (nullptr to close)
void emu_aux_set_output_file(const char* path);

// Auxiliary input - returns character or 0x1A (^Z) on EOF
int emu_aux_in();

// Auxiliary output - writes character to aux output device
void emu_aux_out(uint8_t ch);

//=============================================================================
// Debug/Log Output - for emulator status and debugging
//=============================================================================

// Enable/disable debug logging
void emu_set_debug(bool enable);

// Log a debug message (only when debug enabled)
// Uses printf-style formatting
void emu_log(const char* fmt, ...);

// Log an error message (always shown)
void emu_error(const char* fmt, ...);

// Log a FATAL error message and abort execution
// This function does NOT return - it calls abort()
//
// IMPORTANT: ALL errors in this codebase MUST use emu_fatal() by default.
// DO NOT change any emu_fatal() call to emu_error() or emu_log() without
// EXPLICIT APPROVAL from a human. Silent failures waste hours of debugging.
[[noreturn]] void emu_fatal(const char* fmt, ...);

// Log a status message (for user feedback)
void emu_status(const char* fmt, ...);

//=============================================================================
// File I/O - for loading ROMs and disk images
//=============================================================================

// Load a file into a buffer
// Returns true on success, fills data vector
// On failure, returns false and data is empty
bool emu_file_load(const std::string& path, std::vector<uint8_t>& data);

// Load a file into memory at a specific address
// Returns number of bytes loaded, or 0 on failure
size_t emu_file_load_to_mem(const std::string& path, uint8_t* mem,
                            size_t mem_size, size_t offset = 0);

// Save a buffer to a file
// Returns true on success
bool emu_file_save(const std::string& path, const std::vector<uint8_t>& data);

// Check if a file exists
bool emu_file_exists(const std::string& path);

// Get file size (returns 0 if file doesn't exist)
size_t emu_file_size(const std::string& path);

//=============================================================================
// Disk Image I/O - for emulated disk drives
//=============================================================================

// Opaque handle for disk images
typedef void* emu_disk_handle;

// Open a disk image file
// mode: "r" for read-only, "rw" for read-write, "rw+" for read-write create
// Returns handle, or nullptr on failure
emu_disk_handle emu_disk_open(const std::string& path, const char* mode);

// Close a disk image
void emu_disk_close(emu_disk_handle disk);

// Read sectors from disk
// offset: byte offset into disk image
// buffer: destination buffer
// count: number of bytes to read
// Returns number of bytes actually read
size_t emu_disk_read(emu_disk_handle disk, size_t offset,
                     uint8_t* buffer, size_t count);

// Write sectors to disk
// offset: byte offset into disk image
// buffer: source buffer
// count: number of bytes to write
// Returns number of bytes actually written
size_t emu_disk_write(emu_disk_handle disk, size_t offset,
                      const uint8_t* buffer, size_t count);

// Flush disk writes to storage
void emu_disk_flush(emu_disk_handle disk);

// Get disk size
size_t emu_disk_size(emu_disk_handle disk);

//=============================================================================
// Time - for RTC emulation
//=============================================================================

// Get current time as broken-down components
struct emu_time {
  int year;    // Full year (e.g., 2025)
  int month;   // 1-12
  int day;     // 1-31
  int hour;    // 0-23
  int minute;  // 0-59
  int second;  // 0-59
  int weekday; // 0=Sunday, 1=Monday, ... 6=Saturday
};

void emu_get_time(emu_time* t);

//=============================================================================
// Random Numbers - for interrupt timing, etc.
//=============================================================================

// Get a random number in range [min, max]
unsigned int emu_random(unsigned int min, unsigned int max);

//=============================================================================
// Video/Display - for HBIOS VDA (Video Display Adapter) and DSKY
//=============================================================================

// Video capabilities (what the platform supports)
struct emu_video_caps {
  bool has_text_display;    // Can display text (rows x cols)
  bool has_pixel_display;   // Can display pixels
  bool has_dsky;            // Has DSKY-style display
  int text_rows;            // Number of text rows
  int text_cols;            // Number of text columns
  int pixel_width;          // Pixel display width
  int pixel_height;         // Pixel display height
};

// Get video capabilities
void emu_video_get_caps(emu_video_caps* caps);

// Text display operations (VDA)
void emu_video_clear();                           // Clear screen
void emu_video_set_cursor(int row, int col);      // Move cursor
void emu_video_get_cursor(int* row, int* col);    // Get cursor position
void emu_video_write_char(uint8_t ch);            // Write char at cursor
void emu_video_write_char_at(int row, int col, uint8_t ch);  // Write at position
void emu_video_scroll_up(int lines);              // Scroll up
void emu_video_set_attr(uint8_t attr);            // Set text attribute
uint8_t emu_video_get_attr();                     // Get current attribute

// DSKY (Display/Keyboard) operations
void emu_dsky_show_hex(uint8_t position, uint8_t value);  // Show hex digit
void emu_dsky_show_segments(uint8_t position, uint8_t segments);  // Raw segments
void emu_dsky_set_leds(uint8_t leds);             // Set status LEDs
void emu_dsky_beep(int duration_ms);              // Beep
int emu_dsky_get_key();                           // Get key (-1 if none)

//=============================================================================
// Host File Transfer - for R8/W8 utilities
//=============================================================================

// Host file state
enum emu_host_file_state {
  HOST_FILE_IDLE = 0,       // No operation pending
  HOST_FILE_WAITING_READ,   // Waiting for user to pick file to read
  HOST_FILE_READING,        // File loaded, ready to read bytes
  HOST_FILE_WRITING,        // Accumulating bytes to write
  HOST_FILE_WRITE_READY,    // Write buffer ready for UI to save
};

// Get current host file state
emu_host_file_state emu_host_file_get_state();

// Request to open host file for reading (triggers file picker in browser)
// filename: suggested filename (may be ignored by browser)
// Returns: true if request was initiated (wait for state change)
bool emu_host_file_open_read(const char* filename);

// Request to open host file for writing (creates buffer)
// filename: name to use when saving
// Returns: true if ready to write
bool emu_host_file_open_write(const char* filename);

// Read byte from host file
// Returns: byte value (0-255), or -1 on EOF/error
int emu_host_file_read_byte();

// Write byte to host file
// Returns: true on success
bool emu_host_file_write_byte(uint8_t byte);

// Close host file
// For write files, this triggers download in browser
void emu_host_file_close_read();
void emu_host_file_close_write();

// Load host file data (called by JavaScript after file picker)
// data: file contents
// size: number of bytes
void emu_host_file_provide_data(const uint8_t* data, size_t size);

// Get write buffer for download (returns nullptr if not writing)
const uint8_t* emu_host_file_get_write_data();
size_t emu_host_file_get_write_size();
const char* emu_host_file_get_write_name();

#endif // EMU_IO_H
