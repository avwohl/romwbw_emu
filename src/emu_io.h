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

// 64-bit seek/tell (portable replacement for fseeko/ftello).
//
// fseeko/ftello are POSIX and MSVC has neither, so the Windows port - which
// compiles this core in place rather than through a platform shim - failed to
// build the moment the core started measuring files in 64 bits. MSVC spells
// them _fseeki64/_ftelli64, and its off_t is a 32-bit long, so the offset type
// has to come from here too: an off_t offset would silently truncate past 2GB,
// which is inside the range a combo disk image can reach.
#ifdef _MSC_VER
#include <cstdio>
typedef __int64 emu_off_t;
#define emu_fseek _fseeki64
#define emu_ftell _ftelli64
#else
#include <sys/types.h>
#include <cstdio>
typedef off_t emu_off_t;
#define emu_fseek fseeko
#define emu_ftell ftello
#endif

// Rename `from` over `to`, replacing `to` if it exists. Returns 0 on success,
// non-zero on failure - the same shape as rename().
//
// ISO C leaves rename() undefined when the target exists, and both the MSVC
// CRT and mingw's msvcrt refuse it outright rather than replacing. That is not
// an edge case here: emu_file_save() writes a temp file and renames it over the
// image precisely so a failed write cannot destroy the previous one, so on
// Windows the safe path was the broken one. The Windows port had already
// worked around it locally with MoveFileExA(MOVEFILE_REPLACE_EXISTING) and
// asked for the shim, so the shared copy stops carrying a dormant bug that
// port does not have.
int emu_rename(const char* from, const char* to);

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

// The console read consumed a keystroke that belongs to the emulator rather
// than to the guest - today only the CLI's escape key. There is no byte for
// the guest; the caller must hand control back to the main loop (which will
// act on the escape) and re-run the read afterwards, not deliver a
// substitute character. A platform that reserves no key never returns it.
#define EMU_CONSOLE_RETRY (-2)

// Read a character from console (may block if no input)
// Returns the character, -1 on EOF, or EMU_CONSOLE_RETRY (see above).
// LF is converted to CR for CP/M compatibility on the paths where the host
// really does deliver LF for Enter (pipes); a tty in raw mode delivers CR
// natively, so Ctrl+J stays a distinct key there.
int emu_console_read_char();

// Queue a character for input (for async input sources)
void emu_console_queue_char(int ch);

// Clear the input queue (call on reset)
void emu_console_clear_queue();

// Returns true once the guest has read past a latched EOF on stdin - a
// hung-up tty as well as a drained pipe or file - so no further input can
// ever arrive. WASM returns false. Only the CLI main loop consults this (to
// exit through end of main so NVRAM/trace persistence still runs).
bool emu_console_input_exhausted();

// Weaker form: EOF has been *detected* on a non-interactive stdin (nothing
// queued), whether or not the guest has read past it. Combined with guest
// console-idle detection this catches guests that only poll input status
// (e.g. the romldr boot menu) and would otherwise spin forever on a closed
// pipe. Interactive and WASM platforms return false.
bool emu_console_input_eof();

// Write a character to console
void emu_console_write_char(uint8_t ch);

// Check for escape sequence (for entering debug console)
// escape_char: the escape character to look for, or 0 to reserve no key at
//   all - every Ctrl-letter is live in CP/M, so a host that claims one has to
//   be able to give it back (see "Ctrl-A..Ctrl-Z Belong to the Guest" in
//   DOWNSTREAM.md). With 0 the platform must not consume anything.
// Returns true if escape was detected and consumed
bool emu_console_check_escape(char escape_char);

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

// Flush all disk data to persistent storage (called on warm boot)
void emu_disk_flush_all();

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

// Request to open host file for reading.
// filename: host path exactly as typed by the guest (R8 command line).
//   Native backends: an absolute host path MUST be opened verbatim; a bare
//   (relative) name MAY be resolved against a platform default location
//   (CLI backend: the process working directory; a GUI port might use its
//   data folder). Never unconditionally prepend a directory to the name --
//   that turns absolute paths into invalid nested paths and breaks
//   "R8 /full/path/file.com" (this bug occurred in a downstream port).
//   Browser backends: triggers a file picker; filename is only a hint.
// Returns: true if request was initiated (wait for state change)
bool emu_host_file_open_read(const char* filename);

// Request to open host file for writing.
// filename: name/path for the output; same path contract as
//   emu_host_file_open_read for native backends. Browser backends use it
//   only as the suggested download filename (creates an in-memory buffer).
//
//   This really is a *path* and not a name: W8 takes an optional host path
//   (src/w8.asm) and passes it here verbatim. Two consequences every backend
//   has to handle rather than assume away:
//
//   - The CCP has uppercased it, and the typed case is gone before the
//     emulator ever sees the string. The convention across backends is to
//     resolve existing directory components case-insensitively and to
//     LOWERCASE the file name being created - what W8 has always done for the
//     name it derives from the FCB. A backend that picks a different
//     convention makes the same W8 command produce differently-named files on
//     different front ends.
//   - A backend with no filesystem to honour a directory with must reduce the
//     string with emu_host_path_basename() below rather than store it whole.
//     Storing it whole means a guest typing a path produces an export *name*
//     containing separators, and a UI layer that then joins that to its own
//     export directory can be walked out of it with "..".
//
// Returns: true if ready to write. A buffering backend that cannot know yet
//   (browser, mobile) returns true and reports the real outcome from
//   emu_host_file_close_write(); the guest is told either way.
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
// Returns false if the final flush/close failed (the written file may be
// truncated, e.g. disk full) - the last chance to detect buffered write errors
bool emu_host_file_close_write();

// Load host file data (called by JavaScript after file picker)
// data: file contents
// size: number of bytes
void emu_host_file_provide_data(const uint8_t* data, size_t size);

// Get write buffer for download (returns nullptr if not writing)
const uint8_t* emu_host_file_get_write_data();
size_t emu_host_file_get_write_size();

// Where the bytes handed to emu_host_file_write_byte() will actually land,
// as a string fit to show the person who typed the W8 command.
//
// This is NOT an echo of the name passed to emu_host_file_open_write(). It is
// the *effective* destination after the backend has done whatever it does to a
// requested path: the CLI's case-insensitive parent resolution and lowercased
// basename, a browser's reduction to a bare download name, a mobile port's
// Exports folder, a packaged Windows build's redirected LocalCache. Those
// transformations are exactly why the guest cannot compute the answer itself,
// and why W8 asks for it (HBF_HOST_GETNAME, 0xE8) instead of printing what the
// user typed - which on three of the five front ends names a file that does
// not exist.
//
// Valid between a successful emu_host_file_open_write() and the matching
// emu_host_file_close_write(). Outside that window the return value is "" or
// nullptr; callers must tolerate both.
const char* emu_host_file_get_write_name();

// Which file emu_host_file_read_byte() is actually reading, as a string fit to
// show the person who typed the R8 command. The read twin of
// emu_host_file_get_write_name() above, and the same contract: not an echo of
// what was passed to emu_host_file_open_read(), but the effective source after
// the backend has done whatever it does with a requested path.
//
// The difference is smaller here than on the write side - the file has to
// exist for the open to have succeeded, so it is usually the same file under a
// different spelling - but it is not nothing: the CLI retries a path the CCP
// uppercased case-insensitively and answers with the absolute path it settled
// on, and a front end that opens a file picker returns whatever the user
// chose, which need not resemble the guest's string at all.
//
// Valid between a successful emu_host_file_open_read() and the matching
// emu_host_file_close_read(). Outside that window the return value is "" or
// nullptr; callers must tolerate both.
//
// NEW REQUIRED BACKEND FUNCTION: a port that syncs this core must define it or
// fail to link. Returning "" is a correct answer for a backend that genuinely
// cannot say - HBF_HOST_GETRNAME then reports "no answer" and R8 falls back to
// printing what was asked for.
const char* emu_host_file_get_read_name();

// What this front end's host-file backend guarantees about a guest-supplied
// path, as a bitmask. The core's HBF_HOST_CAPS (0xE9) hands the low byte to the
// guest, and W8.COM refuses to send a host path to a backend that does not set
// EMU_HOST_CAP_SAFE_PATHS.
//
// This is DELIBERATELY declared here and NOT defined in the shared core. Every
// backend must define it, so the assertion it makes is written by the code the
// assertion is about. The core returning a constant - which is what this
// replaced - meant a port that had not thought about guest paths asserted it
// had, just by compiling. A port that has not been updated for this now fails
// to LINK instead, which is the whole point: the guarantee and the code that
// makes it true arrive together, or not at all.
//
// If you reached this comment from a linker error ("undefined symbol
// emu_host_path_caps"), that error is the intended signal that your port has a
// core sync to absorb. Do NOT just add a stub to make it compile: read
// docs/DOWNSTREAM_2026-08-25.md first - defining this function is one of
// several steps, and the rest (a disk-image refresh, an R8 fix, a release
// ordering) will not announce themselves the way this one did.
//
// EMU_HOST_CAP_SAFE_PATHS means: this backend will not use a guest-supplied
// path DESTRUCTIVELY. It writes the one file the path names and nothing else -
// no delete of anything the path resolves near, no silent substitution of a
// different file, no traversal used to reach outside an intended area and
// remove it. That is the property W8 needs before it will hand over a path.
//
// It is NOT "the path is confined to one directory". A backend that opens an
// absolute path exactly as given (the CLI, the Windows port) still SETS the
// bit, because writing where you were told is not the destructive behaviour
// this guards against - the bug it exists for was iOS building a URL from the
// path and calling removeItem on it, deleting the user's whole Documents
// folder. Set the bit if your open-write path is a plain "create/replace the
// named file"; clear it only if you cannot yet promise even that.
enum {
  EMU_HOST_CAP_SAFE_PATHS = 0x01,  // a guest path is never used destructively
};
uint8_t emu_host_path_caps();

// Last path component of `path`, for a backend that cannot honour a directory
// at all (a browser download, a sandboxed app's own Exports folder). Accepts
// both separators, because the string comes from a guest command line that may
// have been typed on any host: C:\USERS\ME\OUT.TXT reduces to "OUT.TXT"
// exactly as "/home/me/out.txt" reduces to "out.txt".
//
// Never returns something that would escape the directory it is joined to: a
// result of "", ".", ".." or a bare drive letter is replaced by `fallback`.
// Trailing separators are ignored, so "a/b/" gives "b" rather than "".
//
// The result is at most EMU_HOST_NAME_MAX bytes. Every caller uses it as a
// file name - a browser download, an entry in a sandboxed app's Exports folder
// - and no filesystem in this family takes a longer component, so a 5000-
// character last component was a name that could only be refused. What is kept
// is the EXTENSION and the front of the stem, not the front of the whole
// string: a name cut to "aaaa...aaa" with the ".txt" thrown away opens in
// nothing, while "aaa....txt" still opens in the right application. A cut
// never lands in the middle of a UTF-8 sequence.
static const size_t EMU_HOST_NAME_MAX = 255;  // one component, every FS here
std::string emu_host_path_basename(const std::string& path,
                                   const char* fallback = "download.bin");

#endif // EMU_IO_H
