/*
 * dio_units.cc - the disk unit number is a plain index, and nothing else
 *
 * WHAT THIS IS FOR.  The two unit mappers used to accept three encodings that
 * exist nowhere in HBIOS: 0x80-0x8F and 0xC0-0xCF as memory disks, 0x90-0x9F as
 * hard disks.  So unit 0x80 read MD0, 0xC3 read MD1 and 0x92 read hard disk 2,
 * every one of them answering A = 0.
 *
 * RomWBW has no such encoding.  C is an index into DIO_TBL and the dispatcher
 * bounds-checks it against the live entry count before any driver runs -
 * hbios.asm:7448-7452, HB_DISPCALC:
 *
 *     LD  A,C
 *     CP  (IY-1)          ; COMPARE TO COUNT
 *     JR  NC,HB_UNITERR   ; ERR_NOUNIT
 *
 * with DIO_MAX = 16.  Anything at or above the count is ERR_NOUNIT, full stop.
 *
 * The aliasing is the sharper half: 0x92 and unit 4 mapped to the SAME image,
 * so a write through the phantom unit landed on a real disk.  And a guest that
 * walks unit numbers past the count BF_SYSGET/DIOCNT reported - which is a
 * normal way to enumerate - found disks that answered successfully where real
 * hardware would have stopped it.
 *
 * Run: make -C src test_dio_units && ./src/test_dio_units
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
  std::vector<uint8_t> image;

  Rig() : cpu(&mem), image(512 * 64, 0) {
    mem.enable_banking();
    hbios.setCPU(&cpu);
    hbios.setMemory(&mem);
    hbios.setBlockingAllowed(false);

    // BOTH memory disks have to be ENABLED for this test to mean anything.
    // is_md_unit() tests is_enabled before it tests the unit number, so with no
    // memory disks configured every 0x80/0xC0 call is rejected for the wrong
    // reason and the assertions below pass whatever the mappers do.  That is
    // exactly how the first draft of this file was vacuous.
    //
    // initMemoryDisks() reads its configuration out of the HCB at bank 0:
    // CB_BIDRAMD0/CB_RAMD_BNKS at $1DC/$1DD and CB_BIDROMD0/CB_ROMD_BNKS at
    // $1DE/$1DF (hbios_dispatch.cc:380-383).
    mem.write_bank(0x00, 0x01DC, 0x10);  // RAM disk start bank
    mem.write_bank(0x00, 0x01DD, 0x08);  // ... 8 banks, so MD0 exists
    mem.write_bank(0x00, 0x01DE, 0x20);  // ROM disk start bank
    mem.write_bank(0x00, 0x01DF, 0x08);  // ... 8 banks, so MD1 exists
    hbios.initMemoryDisks();

    // One hard disk at unit 2 - the first hard disk, disk array index 0 - so
    // that the real unit this test compares against actually answers.
    hbios.loadDisk(0, image.data(), image.size());
  }

  // BF_DIODEVICE reports on a unit and is the cheapest call that must reject
  // one: no seek state, no transfer, just "what is at unit C".
  uint8_t device(uint8_t unit) {
    cpu.regs.BC.set_high(HBF_DIODEVICE);
    cpu.regs.BC.set_low(unit);
    cpu.regs.DE.set_pair16(0x1111);
    cpu.regs.HL.set_pair16(0x2222);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
    return cpu.regs.AF.get_high();
  }

  uint8_t D() { return cpu.regs.DE.get_high(); }
  uint8_t E() { return cpu.regs.DE.get_low(); }
};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("The disk unit number is a plain index, and nothing else\n");
  printf("------------------------------------------------------------\n");

  {
    Rig r;
    // The real unit space first: whatever the invented ranges do, these must
    // keep working, or the tests below prove nothing.
    check(r.device(0) == 0, "unit 0 is MD0 and answers");
    check(r.D() == 0x00, "and reports DIODEV_MD");
    check(r.device(1) == 0, "unit 1 is MD1 and answers");
    check(r.device(2) == 0, "unit 2 is the first hard disk and answers");
    check(r.D() == 0x09, "and reports DIODEV_HDSK");
    check(r.E() == 0x00, "as device 0 within its type");
  }

  {
    Rig r;
    // The three invented ranges.  Each used to answer A = 0.
    check(r.device(0x80) != 0, "unit 0x80 is not MD0 - it is no unit at all");
    check(r.device(0x8F) != 0, "nor is 0x8F");
    check(r.device(0xC0) != 0, "unit 0xC0 is not the ROM disk");
    check(r.device(0xC3) != 0, "nor is 0xC3");
    check(r.device(0x90) != 0, "unit 0x90 is not hard disk 0");
    check(r.device(0x92) != 0, "and 0x92 is not hard disk 2 - the aliasing case");
  }

  {
    Rig r;
    // DIO_MAX is 16, so 2..17 is the hard disk space and 18 is past it.  The
    // walk a guest does when it enumerates must terminate.
    bool any_success_past_the_end = false;
    for (int u = 18; u <= 255; u++) {
      if (r.device((uint8_t)u) == 0) { any_success_past_the_end = true; break; }
    }
    check(!any_success_past_the_end,
          "no unit from 18 to 255 answers success - an enumeration terminates");
  }

  {
    Rig r;
    // The aliasing itself, stated as the thing it was: two different unit
    // numbers naming one image.  0x92 mapped to index 2, which is unit 4.
    uint8_t via_alias = r.device(0x92);
    uint8_t alias_D = r.D(), alias_E = r.E();
    uint8_t via_real = r.device(4);
    check(!(via_alias == 0 && via_real == 0 && alias_D == r.D() && alias_E == r.E()),
          "0x92 and unit 4 no longer name the same disk");
  }

  printf("------------------------------------------------------------\n");
  printf("%s (%d failure(s))\n",
         failures ? "FAILURES" : "all checks passed", failures);
  return failures ? 1 : 0;
}
