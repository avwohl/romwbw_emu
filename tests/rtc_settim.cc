/*
 * rtc_settim.cc - a guest can set the clock and read back what it set
 *
 * WHAT THIS IS FOR.  BF_RTCSETTIM was two lines - a comment saying "Set time -
 * ignored in emulator" and a break - so it reported HBR_SUCCESS and threw the
 * time away.  A guest running DATE SET watched it take and then read the host
 * clock straight back.  Answering success while doing nothing is the failure
 * mode this dispatcher has been bitten by before, and the reason is always the
 * same: a caller cannot defend against it.
 *
 * There is no chip to write, so what is stored is an OFFSET from the host
 * clock.  That keeps the clock RUNNING after it is set, which a frozen
 * timestamp would not, and it is the behaviour a guest expects of a real RTC.
 * It is deliberately not persisted: a real one is battery-backed and this is
 * not.
 *
 * Run: make -C src test_rtc_settim && ./src/test_rtc_settim
 *      make -C src test
 */

#include "hbios_dispatch.h"
#include "romwbw_mem.h"
#include "qkz80.h"
#include "emu_io.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

// See tests/vda_keyboard.cc: MSVC has no <strings.h>.
#ifdef _WIN32
#include <string.h>
#define EMU_TEST_STRNCASECMP _strnicmp
#else
#include <strings.h>
#define EMU_TEST_STRNCASECMP strncasecmp
#endif

//=============================================================================
// The two things this test watches
//=============================================================================

//=============================================================================
// The rest of emu_io: enough to link, never exercised here
//=============================================================================

void emu_dsky_beep(int) {}

bool emu_console_has_input() { return false; }
int emu_console_read_char() { return -1; }
void emu_console_write_char(uint8_t) {}
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
  return EMU_TEST_STRNCASECMP(a, b, n);
}

bool emu_file_exists(const std::string&) { return false; }

emu_host_file_state emu_host_file_get_state() { return HOST_FILE_IDLE; }
bool emu_host_file_open_read(const char*) { return false; }
bool emu_host_file_open_write(const char*) { return false; }
int emu_host_file_read_byte() { return -1; }
bool emu_host_file_write_byte(uint8_t) { return false; }
void emu_host_file_close_read() {}
bool emu_host_file_close_write() { return true; }
const char* emu_host_file_get_write_name() { return ""; }
const char* emu_host_file_get_read_name() { return ""; }
uint8_t emu_host_path_caps() { return 0; }

//=============================================================================
// Scaffolding
//=============================================================================

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
    // Without this every bank read answers 0xFF and every bank write is
    // dropped, so the copy would appear to do nothing for a reason that has
    // nothing to do with the copy.
    mem.enable_banking();
    hbios.setCPU(&cpu);
    hbios.setMemory(&mem);
    hbios.setBlockingAllowed(false);
  }


  // BF_RTCGETTIM and BF_RTCSETTIM both take a six-byte BCD buffer at HL:
  // YY MM DD HH MM SS.
  static const uint16_t BUF = 0x4000;

  void writeTime(int yy, int mm, int dd, int hh, int mi, int ss) {
    const int v[6] = {yy, mm, dd, hh, mi, ss};
    for (int i = 0; i < 6; i++) {
      mem.store_mem((uint16_t)(BUF + i), (uint8_t)(((v[i] / 10) << 4) | (v[i] % 10)));
    }
  }

  void writeRaw(int i, uint8_t byte) { mem.store_mem((uint16_t)(BUF + i), byte); }

  uint8_t rtc(uint8_t func) {
    cpu.regs.BC.set_high(func);
    cpu.regs.HL.set_pair16(BUF);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
    return cpu.regs.AF.get_high();
  }

  int readField(int i) {
    uint8_t b = mem.fetch_mem((uint16_t)(BUF + i));
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
  }


};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("BF_RTCSETTIM takes, and BF_RTCGETTIM reads back what was set\n");
  printf("------------------------------------------------------------\n");

  {
    Rig r;
    // A date the host clock will not be on.
    r.writeTime(84, 11, 23, 4, 56, 7);
    uint8_t a = r.rtc(HBF_RTCSETTIM);
    check(a == 0, "setting a valid time reports success");

    for (int i = 0; i < 6; i++) r.writeRaw(i, 0);
    r.rtc(HBF_RTCGETTIM);
    bool same = r.readField(0) == 84 && r.readField(1) == 11 && r.readField(2) == 23
             && r.readField(3) == 4 && r.readField(4) == 56;
    check(same, "and reading it straight back gives the same date and time");
    if (!same) {
      printf("      read back %02d-%02d-%02d %02d:%02d:%02d\n", r.readField(0),
             r.readField(1), r.readField(2), r.readField(3), r.readField(4),
             r.readField(5));
    }
    // Seconds are not compared: the host clock moves between the two calls,
    // which is the point - the guest's clock RUNS rather than freezing.
  }

  {
    // Two sets in a row: the second wins, rather than compounding.
    Rig r;
    r.writeTime(84, 11, 23, 4, 56, 7);
    r.rtc(HBF_RTCSETTIM);
    r.writeTime(99, 1, 2, 3, 4, 5);
    r.rtc(HBF_RTCSETTIM);
    for (int i = 0; i < 6; i++) r.writeRaw(i, 0);
    r.rtc(HBF_RTCGETTIM);
    check(r.readField(0) == 99 && r.readField(1) == 1 && r.readField(2) == 2
              && r.readField(3) == 3 && r.readField(4) == 4,
          "a second set replaces the first rather than adding to it");
  }

  {
    // Not BCD at all: every RomWBW driver returns non-zero when the write did
    // not take, and the worst answer is to accept it and report success.
    Rig r;
    r.writeTime(84, 11, 23, 4, 56, 7);
    r.rtc(HBF_RTCSETTIM);
    r.writeRaw(1, 0xAF);            // month nibble 0xF, not a digit
    uint8_t a = r.rtc(HBF_RTCSETTIM);
    check(a != 0, "a buffer that is not valid BCD is refused, not accepted");

    for (int i = 0; i < 6; i++) r.writeRaw(i, 0);
    r.rtc(HBF_RTCGETTIM);
    check(r.readField(0) == 84 && r.readField(1) == 11,
          "and the refused set leaves the previous time alone");
  }

  {
    // Valid BCD, impossible date.
    Rig r;
    r.writeTime(84, 13, 1, 0, 0, 0);   // month 13
    check(r.rtc(HBF_RTCSETTIM) != 0, "month 13 is refused");
    r.writeTime(84, 12, 1, 25, 0, 0);  // hour 25
    check(r.rtc(HBF_RTCSETTIM) != 0, "hour 25 is refused");
  }

  {
    // Rolling over a month boundary exercises the calendar arithmetic in both
    // directions - the offset is applied to a host reading, not to the stored
    // value, so a naive implementation gets this wrong.
    Rig r;
    r.writeTime(84, 2, 29, 23, 59, 58);   // 2084 is a leap year
    check(r.rtc(HBF_RTCSETTIM) == 0, "a leap day is accepted");
    for (int i = 0; i < 6; i++) r.writeRaw(i, 0);
    r.rtc(HBF_RTCGETTIM);
    check(r.readField(1) == 2 && r.readField(2) == 29,
          "and reads back as 29 February, not 1 March");
  }

  printf("------------------------------------------------------------\n");
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all checks passed (0 failures)\n");
  return 0;
}
