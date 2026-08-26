/*
 * vda_keyboard.cc - VDAKST / VDAKRD behave like their CIO twins
 *
 * Regression test for the two VDA keyboard bugs found in the ^R input-path
 * audit and fixed afterwards:
 *
 *   - VDAKST set only E and left A at HBR_SUCCESS (0), so a guest polling
 *     the video keyboard read "no key" however much was queued.
 *   - VDAKRD, with no key pending, set waiting_for_input and returned without
 *     rewinding PC.  Dispatch is a two-byte OUT (0xEF),A followed by the Z80
 *     proxy's own RET, so skipping the rewind let that RET fire with E holding
 *     whatever the previous call left there: the guest took a stale byte for a
 *     keystroke and never came back for the real one.  CIOIN has always
 *     rewound; the two paths differ only in which device the guest asked for.
 *
 * Both are reachable from every port - SYSGET_VDACNT reports one VDA to all of
 * them - and the rewind half only bites a non-blocking front end, which is the
 * web/WASM build and iOS/macOS.
 *
 * The test links the real dispatcher against a stub console it can control, and
 * drives HBIOS through handlePortDispatch(), the same entry the proxy uses.
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
#include <deque>
#include <string>
#include <strings.h>

//=============================================================================
// Console stub - the one piece of emu_io the test steers
//=============================================================================

static std::deque<int> g_keys;

bool emu_console_has_input() { return !g_keys.empty(); }

int emu_console_read_char() {
  if (g_keys.empty()) return -1;
  int ch = g_keys.front();
  g_keys.pop_front();
  return ch;
}

void emu_console_write_char(uint8_t) {}

//=============================================================================
// The rest of emu_io: enough to link, never exercised here
//=============================================================================

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

// Declared in emu_io.h but implemented per backend rather than in
// emu_io_common.cc, so the test has to supply it.
bool emu_file_exists(const std::string&) { return false; }

emu_host_file_state emu_host_file_get_state() { return HOST_FILE_IDLE; }
bool emu_host_file_open_read(const char*) { return false; }
bool emu_host_file_open_write(const char*) { return false; }
int emu_host_file_read_byte() { return -1; }
bool emu_host_file_write_byte(uint8_t) { return false; }
void emu_host_file_close_read() {}
bool emu_host_file_close_write() { return true; }
// HBF_HOST_GETNAME reaches this from handleEXT; the dispatcher asks for the
// state first, and this stub is never WRITING, so the name is never read.
const char* emu_host_file_get_write_name() { return ""; }
const char* emu_host_file_get_read_name() { return ""; }
// HBF_HOST_CAPS reaches handleEXT; the dispatcher forwards this. The stub
// promises nothing, which is the safe default for a test that never exports.
uint8_t emu_host_path_caps() { return 0; }

//=============================================================================
// Scaffolding
//=============================================================================

// The proxy's OUT (0xEF),A lives at 0xFFF0 and is two bytes, so by the time
// the dispatcher runs PC already points past it.
static const uint16_t ENTRY = 0xFFF0;
static const uint16_t AFTER = ENTRY + 2;

static int failures = 0;

static void check(bool ok, const char* what) {
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
    // Non-blocking is the web/WASM and iOS/macOS setting, and the only one
    // where the missing PC rewind is reachable.
    hbios.setBlockingAllowed(false);
  }

  // One HBIOS call, entered exactly as the Z80 proxy enters it.
  void call(uint8_t func, uint8_t unit = 0) {
    cpu.regs.BC.set_high(func);
    cpu.regs.BC.set_low(unit);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
  }

  uint8_t A() { return cpu.regs.AF.get_high(); }
  uint8_t E() { return cpu.regs.DE.get_low(); }
  uint16_t PC() { return cpu.regs.PC.get_pair16(); }
};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("VDA keyboard functions answer the way their CIO twins do\n");
  printf("------------------------------------------------------------\n");

  // --- VDAKST reports the pending key in A, not only in E -----------------
  {
    Rig r;
    g_keys.clear();
    g_keys.push_back('X');
    r.call(HBF_VDAKST);
    check(r.A() != 0, "VDAKST with a key pending returns non-zero status in A");
    check(r.E() != 0, "VDAKST with a key pending returns a count in E");
  }
  {
    Rig r;
    g_keys.clear();
    r.call(HBF_VDAKST);
    check(r.A() == 0, "VDAKST with nothing pending returns zero status in A");
    check(r.E() == 0, "VDAKST with nothing pending returns zero count in E");
  }
  {
    Rig r;
    g_keys.clear();
    g_keys.push_back('X');
    r.call(HBF_CIOIST);
    check(r.A() != 0, "CIOIST agrees: non-zero status in A");
  }

  // --- VDAKRD rewinds PC when no key is pending ---------------------------
  {
    Rig r;
    g_keys.clear();
    r.cpu.regs.DE.set_low(0x5A);  // sentinel: a stale E must not pass for a key
    r.call(HBF_VDAKRD);
    check(r.PC() == ENTRY,
          "VDAKRD with no key rewinds PC over the OUT so the call retries");
    check(r.hbios.isWaitingForInput(),
          "VDAKRD with no key marks the emulator as waiting");
    check(r.E() == 0x5A, "VDAKRD with no key leaves E untouched");

    // ...and the retry delivers the key once one arrives.
    g_keys.push_back('Q');
    r.cpu.regs.PC.set_pair16(AFTER);  // the OUT re-executes
    r.hbios.handlePortDispatch();
    check(r.E() == 'Q', "the retried VDAKRD delivers the key in E");
    check(r.PC() == AFTER, "the retried VDAKRD does not rewind again");
    check(!r.hbios.isWaitingForInput(),
          "the retried VDAKRD clears the waiting flag");
  }
  {
    Rig r;
    g_keys.clear();
    r.call(HBF_CIOIN);
    check(r.PC() == ENTRY, "CIOIN agrees: rewinds PC with no key pending");
  }

  // --- VDAKRD with a key pending is unchanged -----------------------------
  {
    Rig r;
    g_keys.clear();
    g_keys.push_back('A');
    r.call(HBF_VDAKRD);
    check(r.E() == 'A', "VDAKRD with a key pending returns it in E");
    check(r.A() == 0, "VDAKRD with a key pending reports success in A");
    check(r.PC() == AFTER, "VDAKRD with a key pending leaves PC past the OUT");
  }

  printf("------------------------------------------------------------\n");
  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
