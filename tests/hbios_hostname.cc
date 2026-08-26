/*
 * hbios_hostname.cc - HBF_HOST_GETNAME (0xE8) tells the guest where its export
 *                     really went, and cannot be made to write outside the
 *                     buffer it was given
 *
 * W8 used to print the path the user typed.  On three of the five front ends
 * that named a file which does not exist: the CLI lowercases the basename and
 * resolves the parent through whatever case the directory really has, the
 * browser reduces the whole path to a download name, and a sandboxed app writes
 * into its own Exports folder wherever the guest pointed.  So W8 asks instead,
 * and this is the call it asks with.
 *
 * The call copies a host string into guest memory at an address and length the
 * guest chose, which is the part worth holding still: the length arrives in C
 * as a single byte, the copy is bounded by it, and the terminator has to fit
 * inside the same bound rather than one past it.
 *
 * It also has to be safe to call on an emulator that does not have it - a new
 * W8.COM runs on already-released front ends - so the "no such function" answer
 * is checked here too, against the neighbouring code the range was widened to.
 *
 * Build and run:  make -C src test
 */

#include "hbios_dispatch.h"
#include "romwbw_mem.h"
#include "qkz80.h"
#include "emu_io.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

//=============================================================================
// Host file stub - the one piece of emu_io the test steers
//=============================================================================

static emu_host_file_state g_state = HOST_FILE_IDLE;
static std::string g_write_name;
static std::string g_read_name;

emu_host_file_state emu_host_file_get_state() { return g_state; }
const char* emu_host_file_get_write_name() { return g_write_name.c_str(); }
const char* emu_host_file_get_read_name() { return g_read_name.c_str(); }
// HBF_HOST_CAPS forwards this. The test drives HBF_HOST_GETNAME, not CAPS, so
// the value is irrelevant here; a real backend returns EMU_HOST_CAP_SAFE_PATHS
// only if it confines guest paths.
uint8_t emu_host_path_caps() { return EMU_HOST_CAP_SAFE_PATHS; }

//=============================================================================
// The rest of emu_io: enough to link, never exercised here
//=============================================================================

bool emu_console_has_input() { return false; }
int emu_console_read_char() { return -1; }
void emu_console_write_char(uint8_t) {}
void emu_dsky_beep(int) {}
void emu_error(const char*, ...) {}
void emu_log(const char*, ...) {}
void emu_status(const char*, ...) {}
void emu_video_clear() {}
void emu_video_scroll_up(int) {}
void emu_video_set_attr(uint8_t) {}
void emu_video_set_cursor(int, int) {}
void emu_video_write_char(uint8_t) {}

void emu_fatal(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  abort();
}

int emu_strncasecmp(const char* a, const char* b, size_t n) {
  return strncasecmp(a, b, n);
}

bool emu_file_exists(const std::string&) { return false; }

// Recorded rather than stubbed away: the HBF_HOST_OPEN_R/W tests below turn on
// being able to tell "the dispatcher refused before the backend was reached"
// from "the backend said no", and both used to look like A = 0xFF.
static int g_open_read_calls = 0;
static int g_open_write_calls = 0;
static std::string g_last_open_path;

bool emu_host_file_open_read(const char* p) {
  g_open_read_calls++;
  g_last_open_path = p ? p : "";
  return true;
}
bool emu_host_file_open_write(const char* p) {
  g_open_write_calls++;
  g_last_open_path = p ? p : "";
  return true;
}
int emu_host_file_read_byte() { return -1; }
bool emu_host_file_write_byte(uint8_t) { return false; }
void emu_host_file_close_read() {}
bool emu_host_file_close_write() { return true; }

//=============================================================================
// Scaffolding
//=============================================================================

// The proxy's OUT (0xEF),A lives at 0xFFF0 and is two bytes, so by the time the
// dispatcher runs PC already points past it.
static const uint16_t ENTRY = 0xFFF0;
static const uint16_t AFTER = ENTRY + 2;

// Somewhere in the TPA to be handed a string, with a guard byte after it.
static const uint16_t BUF = 0x0400;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

struct Rig {
  banked_mem mem;
  qkz80 cpu;
  HBIOSDispatch hbios;

  Rig() : cpu(&mem) {
    hbios.setCPU(&cpu);
    hbios.setMemory(&mem);
  }

  // Fill the buffer area with a byte no answer would ever contain, so an
  // untouched cell is distinguishable from a written one.
  void poison(uint16_t at, int n, uint8_t with = 0xEE) {
    for (int i = 0; i < n; i++) mem.store_mem(at + i, with);
  }

  void call(uint8_t func, uint8_t c, uint16_t de) {
    cpu.regs.BC.set_high(func);
    cpu.regs.BC.set_low(c);
    cpu.regs.DE.set_pair16(de);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
  }

  uint8_t A() { return cpu.regs.AF.get_high(); }
  uint8_t at(uint16_t a) { return mem.fetch_mem(a); }

  // The NUL-terminated string the guest can now see at `a`, capped so a missing
  // terminator cannot run away with the test.
  std::string str(uint16_t a, int cap = 512) {
    std::string s;
    for (int i = 0; i < cap; i++) {
      uint8_t ch = mem.fetch_mem(a + i);
      if (ch == 0) break;
      s += (char)ch;
    }
    return s;
  }
};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("HBF_HOST_GETNAME: where the export really went\n");
  printf("------------------------------------------------------------\n");

  // --- the ordinary answer -------------------------------------------------
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "/home/me/build/out.com";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 64, BUF);
    check(r.A() == 0, "a write file open reports success");
    check(r.str(BUF) == g_write_name, "and the buffer holds the effective path");
    check(r.at(BUF + (uint16_t)g_write_name.size()) == 0,
          "terminated exactly after the last character");
    check(r.at(BUF + (uint16_t)g_write_name.size() + 1) == 0xEE,
          "and nothing beyond the terminator was touched");
  }

  // --- nothing open --------------------------------------------------------
  {
    Rig r;
    g_state = HOST_FILE_IDLE;
    g_write_name = "/home/me/stale.com";   // a name from a previous transfer
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 64, BUF);
    check(r.A() != 0, "no write file open reports failure");
    check(r.at(BUF) == 0xEE,
          "and the guest's buffer is left alone - it still holds what it asked "
          "for, which is what W8 falls back to printing");
  }

  // --- a backend with no answer -------------------------------------------
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 64, BUF);
    check(r.A() != 0, "an empty name reports failure rather than an empty string");
    check(r.at(BUF) == 0xEE, "and writes nothing");
  }

  // --- the buffer bound ----------------------------------------------------
  //
  // C is one byte, so a path longer than 254 characters cannot be delivered
  // whole - and a bare name gets the whole working directory prepended, so
  // that is reachable rather than exotic. What must NOT happen is handing back
  // a path chopped at the front and calling it the destination: W8 prints this
  // as fact, and a head-truncated path can name a real directory, or even a
  // different real file. Keep the end, where the file name is, and mark the
  // cut so the answer reads as a fragment.
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "/aaaa/bbbb/cccc/out.txt";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 12, BUF);     // room for 11 characters and a NUL
    check(r.A() == 0, "a short buffer still succeeds");
    check(r.at(BUF + 11) == 0, "the terminator is inside the buffer, not past it");
    check(r.at(BUF + 12) == 0xEE, "the byte after the buffer is untouched");
    const std::string got = r.str(BUF);
    check(got.size() == 11, "exactly filling the room the guest offered");
    check(got == ".../out.txt",
          "marked as a fragment, holding the END of the path - the file name "
          "survives, which is the part the user has to go looking for");
  }
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "abc";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 2, BUF);      // the smallest buffer with any room
    check(r.A() == 0, "a two-byte buffer succeeds");
    check(r.str(BUF) == "c",
          "with no room for the marker, the tail alone - a bare \"...\" would "
          "say nothing at all");
    check(r.at(BUF + 2) == 0xEE, "and nothing past it");
  }
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "short.txt";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 64, BUF);
    check(r.str(BUF) == "short.txt",
          "a path that fits is never marked - the cut marker means something");
  }
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "abc";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETNAME, 1, BUF);      // room for a NUL and nothing else
    check(r.A() != 0, "a one-byte buffer fails rather than storing a bare NUL");
    check(r.at(BUF) == 0xEE, "and does not write the terminator either");
    r.call(HBF_HOST_GETNAME, 0, BUF);
    check(r.A() != 0, "a zero-length buffer fails");
    check(r.at(BUF) == 0xEE, "and writes nothing");
  }

  // --- the largest buffer C can name --------------------------------------
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = std::string(300, 'x');  // longer than any 8-bit length
    r.poison(BUF, 300);
    r.call(HBF_HOST_GETNAME, 255, BUF);
    check(r.A() == 0, "C=255 succeeds");
    check(r.str(BUF).size() == 254, "254 characters fit beside the terminator");
    check(r.str(BUF).compare(0, 3, "...") == 0, "and the cut is marked");
    check(r.at(BUF + 254) == 0, "which is terminated");
    check(r.at(BUF + 255) == 0xEE, "with byte 255 of the buffer untouched");
  }

  // --- 0xE8 reaches the extension handler at all ---------------------------
  //
  // The host-file range was 0xE0-0xE7 and getTrapTypeFromFunc() returned -1
  // above it, which is the "unknown function" path: an answer, but the same
  // answer for a function that exists and one that does not. Widening the range
  // is what makes the two distinguishable, so check both ends of it.
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_write_name = "/x";
    r.poison(BUF, 16);
    r.call(HBF_HOST_GETNAME, 16, BUF);
    check(r.str(BUF) == "/x",
          "0xE8 is routed to the extension handler, not to unknown-function");
  }
  {
    Rig r;
    r.poison(BUF, 16);
    r.call(0xEF, 16, BUF);   // still inside the widened range, still no function
    check(r.A() != 0, "an unused code in the widened range reports failure");
    check(r.at(BUF) == 0xEE, "and writes nothing into the guest's buffer");
  }

  // --- HBF_HOST_CAPS: the interlock W8 uses before sending a host path -----
  //
  // The point of this call is that a guest can ask it BEFORE opening anything.
  // HBF_HOST_GETNAME cannot serve: "no such function" and "no write file open"
  // are both 0xFF, so it can only be asked once a file is already open.
  {
    Rig r;
    g_state = HOST_FILE_IDLE;          // nothing open - must not matter
    g_write_name = "";
    r.cpu.regs.DE.set_pair16(0xA5A5);
    r.call(HBF_HOST_CAPS, 0, 0);
    check(r.A() == 0, "answers with nothing open - it takes no state");
    check((r.cpu.regs.DE.get_low() & EMU_HOST_CAP_SAFE_PATHS) != 0,
          "and reports that a guest path cannot escape where the front end writes");
    check(r.cpu.regs.DE.get_high() == 0, "the reserved high byte is zero");
  }
  {
    Rig r;
    g_state = HOST_FILE_WRITING;       // ...and equally must not matter
    g_write_name = "/x/y.txt";
    r.call(HBF_HOST_CAPS, 0, 0);
    check(r.A() == 0, "and the same answer mid-transfer");
  }
  {
    // The distinction the interlock rests on. An emulator predating the call
    // answers from the unknown-function path; this asserts the two answers a
    // guest can see are both non-zero, so "or a / jp nz" is the right test
    // whichever it gets - and that a code still inside the widened range but
    // unimplemented does not accidentally look like success.
    Rig r;
    r.call(0xEE, 0, 0);
    check(r.A() != 0, "an unimplemented code in the range does not look like a capability");
  }

  // --- HBF_HOST_OPEN_R / _W: a path the guest never terminated ------------
  //
  // The call carries an address and no length, so the dispatcher stops after
  // HOST_PATH_MAX bytes.  It used to stop and USE what it had, which turns a
  // guest bug into an emulator action: the read opens some other file, and the
  // write CREATES one, at a path nobody asked for.  Neither shipped utility can
  // get here - R8 and W8 both terminate inside 128 bytes - so what these check
  // is the behaviour at the edge, for the guest program that is not one of
  // those two.
  printf("\n");
  printf("HBF_HOST_OPEN_R / _W: an unterminated path is refused, not trimmed\n");
  printf("------------------------------------------------------------\n");

  // Fill guest memory from `at` with `n` non-zero bytes and NO terminator.
  auto fill = [](Rig& r, uint16_t at, int n) {
    for (int i = 0; i < n; i++) r.mem.store_mem((uint16_t)(at + i), 'x');
  };

  {
    Rig r;
    g_open_read_calls = g_open_write_calls = 0;
    g_last_open_path.clear();
    const std::string path = "/tmp/" + std::string(200, 'a') + ".dat";
    for (size_t i = 0; i < path.size(); i++) r.mem.store_mem(BUF + (uint16_t)i, path[i]);
    r.mem.store_mem(BUF + (uint16_t)path.size(), 0);
    r.call(HBF_HOST_OPEN_R, 0, BUF);
    check(r.A() == 0, "a long but terminated path still opens");
    check(g_open_read_calls == 1 && g_last_open_path == path,
          "and reaches the backend whole, all 209 characters of it");
  }
  {
    // The exact boundary: HOST_PATH_MAX bytes scanned, the last of them the
    // terminator.  255 characters is the longest path that can be delivered.
    Rig r;
    g_open_read_calls = 0;
    fill(r, BUF, 255);
    r.mem.store_mem(BUF + 255, 0);
    r.call(HBF_HOST_OPEN_R, 0, BUF);
    check(r.A() == 0, "255 characters and a terminator is the longest that fits");
    check(g_last_open_path.size() == 255, "and arrives at that length");
  }
  {
    // One byte further and there is no terminator inside the bound.
    Rig r;
    g_open_read_calls = 0;
    g_last_open_path = "sentinel";
    fill(r, BUF, 256);
    r.mem.store_mem(BUF + 256, 0);
    r.call(HBF_HOST_OPEN_R, 0, BUF);
    check(r.A() != 0, "256 characters with the terminator just past the bound fails");
    check(g_open_read_calls == 0,
          "and the backend is never asked - no file is opened under a name the "
          "guest did not write");
    check(g_last_open_path == "sentinel", "so nothing was handed to it");
  }
  {
    // The write side is the one that matters: a truncated write path does not
    // open the wrong file, it creates one.
    Rig r;
    g_open_write_calls = 0;
    g_last_open_path = "sentinel";
    fill(r, BUF, 300);
    r.call(HBF_HOST_OPEN_W, 0, BUF);
    check(r.A() != 0, "an unterminated write path fails");
    check(g_open_write_calls == 0, "and creates nothing");
    check(g_last_open_path == "sentinel", "having passed nothing to the backend");
  }
  {
    // An empty path - the terminator is the first byte - is a path the guest
    // really did write, so it goes through and the backend decides.  This is
    // not the truncation case and must not be swept up with it.
    Rig r;
    g_open_read_calls = 0;
    r.mem.store_mem(BUF, 0);
    r.call(HBF_HOST_OPEN_R, 0, BUF);
    check(g_open_read_calls == 1 && g_last_open_path.empty(),
          "an empty path is passed on, not refused - the backend answers it");
  }

  // --- HBF_HOST_GETRNAME: the read twin ------------------------------------
  //
  // R8 printed the path the CCP shouted rather than the file that was opened.
  // The two are usually the same file, since it has to exist for the open to
  // succeed - but not the same string, and on a front end whose read is a file
  // PICKER they need not even be the same file. This is 0xE8's mirror, so what
  // is worth checking is that it mirrors: same buffer bound, same "no answer"
  // rule, and that the two do not answer for each other's side of the transfer.
  printf("\n");
  printf("HBF_HOST_GETRNAME: which file R8 is really reading\n");
  printf("------------------------------------------------------------\n");
  {
    Rig r;
    g_state = HOST_FILE_READING;
    g_read_name = "/home/me/Notes/report.txt";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETRNAME, 64, BUF);
    check(r.A() == 0, "a read file open reports success");
    check(r.str(BUF) == g_read_name, "and the buffer holds the effective source");
    check(r.at(BUF + (uint16_t)g_read_name.size()) == 0, "terminated in place");
    check(r.at(BUF + (uint16_t)g_read_name.size() + 1) == 0xEE,
          "and nothing beyond the terminator was touched");
  }
  {
    Rig r;
    g_state = HOST_FILE_IDLE;
    g_read_name = "/home/me/stale.txt";   // a name from a previous transfer
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETRNAME, 64, BUF);
    check(r.A() != 0, "nothing open reports failure");
    check(r.at(BUF) == 0xEE,
          "and leaves the guest's buffer alone - it still holds what R8 asked "
          "for, which is what R8 falls back to printing");
  }
  {
    // The browser answers "" here on purpose: its read is a file picker, so the
    // guest's string is a hint the user is free to ignore and echoing it would
    // be wrong rather than merely unhelpful.
    Rig r;
    g_state = HOST_FILE_READING;
    g_read_name = "";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETRNAME, 64, BUF);
    check(r.A() != 0, "a backend with no answer reports failure, not \"\"");
    check(r.at(BUF) == 0xEE, "and writes nothing");
  }
  {
    // Same cut rule as 0xE8, and for the same reason: a head-truncated path can
    // name a real directory or a different real file, and R8 prints this as
    // fact.
    Rig r;
    g_state = HOST_FILE_READING;
    g_read_name = "/aaaa/bbbb/cccc/in.txt";
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETRNAME, 11, BUF);    // room for 10 characters and a NUL
    check(r.A() == 0, "a short buffer still succeeds");
    check(r.str(BUF) == ".../in.txt",
          "keeping the END and marking the cut, exactly as 0xE8 does");
    check(r.at(BUF + 11) == 0xEE, "the byte after the buffer is untouched");
    r.poison(BUF, 64);
    r.call(HBF_HOST_GETRNAME, 1, BUF);
    check(r.A() != 0, "a one-byte buffer fails rather than storing a bare NUL");
    check(r.at(BUF) == 0xEE, "and writes nothing");
  }
  {
    // The two calls describe opposite ends of the transfer and must not answer
    // for each other: a read in progress has no destination and a write in
    // progress has no source.
    Rig r;
    g_state = HOST_FILE_READING;
    g_read_name = "/in.txt";
    g_write_name = "/out.txt";
    r.poison(BUF, 32);
    r.call(HBF_HOST_GETNAME, 32, BUF);
    check(r.A() != 0, "0xE8 has no answer while a READ is what is open");
    check(r.at(BUF) == 0xEE, "and writes nothing");
    r.poison(BUF, 32);
    r.call(HBF_HOST_GETRNAME, 32, BUF);
    check(r.A() == 0 && r.str(BUF) == "/in.txt", "0xEA does");
  }
  {
    Rig r;
    g_state = HOST_FILE_WRITING;
    g_read_name = "/in.txt";
    g_write_name = "/out.txt";
    r.poison(BUF, 32);
    r.call(HBF_HOST_GETRNAME, 32, BUF);
    check(r.A() != 0, "0xEA has no answer while a WRITE is what is open");
    check(r.at(BUF) == 0xEE, "and writes nothing");
    r.poison(BUF, 32);
    r.call(HBF_HOST_GETNAME, 32, BUF);
    check(r.A() == 0 && r.str(BUF) == "/out.txt", "0xE8 does");
  }
  {
    // 0xEA is inside the widened extension range and reaches the handler, the
    // same thing that had to be checked when 0xE8 was added.
    Rig r;
    g_state = HOST_FILE_READING;
    g_read_name = "/y";
    r.poison(BUF, 16);
    r.call(HBF_HOST_GETRNAME, 16, BUF);
    check(r.str(BUF) == "/y",
          "0xEA is routed to the extension handler, not to unknown-function");
  }

  printf("------------------------------------------------------------\n");
  printf("%d passed, %d failed\n", checks - failures, failures);
  return failures ? 1 : 0;
}
