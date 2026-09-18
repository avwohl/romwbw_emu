/*
 * sys_switch.cc - the NVRAM switches follow SWITCH_RES, table and all
 *
 * WHAT THIS IS FOR.  BF_SYSGET/SWITCH and BF_SYSSET/SWITCH ($C0) handled three
 * switch numbers - $FF, 1 and 3 - and answered every other one with HL = 0 and
 * SUCCESS on the get, or silence and SUCCESS on the set.  That is wrong in two
 * directions at once: 0 and 2 are real switches that read as zero, and 4 up are
 * ones RomWBW REFUSES that read as zero too, so a caller could not tell a switch
 * that is off from one that does not exist.
 *
 * The rule is a table.  hbios.asm:6490-6521:
 *
 *     SWITCH_TAB: .DB 0, 2, 0, 1, 0
 *     SWITCH_LEN  .EQU  $ - SWITCH_TAB - 2      ; = 3
 *
 * so switch numbers 0..3 are addressable, 4 (the checksum) and up are not, and
 * the table entry is the WIDTH: 2 means the value spans this byte and the next
 * (SYS_GETSWITCH's "if E > 1 then INC HL / LD B,(HL)").
 *
 * The set path has preconditions the emulator had none of
 * (hbios.asm:6457-6482): $FF resets, and anything else requires the block to
 * already read 'W'.  Both arms instead assigned the signature themselves, with
 * the comment "Ensure initialized" - so the first set of any switch quietly
 * initialised NVRAM, which is the one thing RomWBW will not do.
 *
 * Run: make -C src test_sys_switch && ./src/test_sys_switch
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


  // BF_SYSGET / BF_SYSSET subfunction $C0.  D is the switch number, HL the
  // value.  Both return their status in A.
  uint8_t getsw(uint8_t num) {
    cpu.regs.BC.set_high(HBF_SYSGET);
    cpu.regs.BC.set_low(SYSGET_SWITCH);
    cpu.regs.DE.set_high(num);
    cpu.regs.HL.set_pair16(0xDEAD);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
    return cpu.regs.AF.get_high();
  }

  uint8_t setsw(uint8_t num, uint16_t value) {
    cpu.regs.BC.set_high(HBF_SYSSET);
    cpu.regs.BC.set_low(SYSSET_SWITCH);
    cpu.regs.DE.set_high(num);
    cpu.regs.HL.set_pair16(value);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
    return cpu.regs.AF.get_high();
  }

  uint16_t HL() { return cpu.regs.HL.get_pair16(); }
};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("The NVRAM switches follow SWITCH_RES, table and all\n");
  printf("------------------------------------------------------------\n");

  {
    Rig r;
    // A fresh block: byte 0 is 0, "NVRAM present but not initialised".
    check(r.setsw(1, 0x1234) != 0,
          "setting a switch on an uninitialised block is refused");
    r.getsw(1);
    check(r.HL() != 0x1234, "and the refused set wrote nothing");
  }

  {
    Rig r;
    check(r.setsw(0xFF, 0) == 0, "switch $FF resets the block to defaults");
    check(r.setsw(1, 0x8041) == 0, "and a set is accepted once it reads 'W'");
    r.getsw(1);
    check(r.HL() == 0x8041,
          "switch 1 is two bytes wide: L from [1] and H from [2]");
  }

  {
    Rig r;
    r.setsw(0xFF, 0);
    r.setsw(1, 0x8041);   // [1] = 0x41, [2] = 0x80
    r.setsw(3, 0x0025);   // [3] = 0x25

    check(r.getsw(0) == 0 && (r.HL() & 0xFF) == 'W',
          "switch 0 reads the 'W' signature byte");
    check((r.HL() >> 8) == 0,
          "and it is one byte wide, so its high byte is 0 - not the byte after");

    r.getsw(2);
    check(r.HL() == 0x0080,
          "switch 2 is the second boot-options byte on its own, not zero");

    r.getsw(3);
    check(r.HL() == 0x0025, "switch 3 is the autoboot byte, one byte wide");
  }

  {
    Rig r;
    r.setsw(0xFF, 0);
    check(r.getsw(4) != 0, "switch 4 is the checksum and is refused, not read as 0");
    check(r.getsw(5) != 0, "and so is 5");
    check(r.getsw(254) != 0, "and 254");
    check(r.setsw(4, 1) != 0, "setting switch 4 is refused too");
  }

  {
    Rig r;
    check(r.getsw(0xFF) == 1,
          "switch $FF on an uninitialised block reports 1 - present, not configured");
    r.setsw(0xFF, 0);
    check(r.getsw(0xFF) == 'W',
          "and 'W' once reset, which is what the boot loader tests");
  }

  printf("------------------------------------------------------------\n");
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all checks passed (0 failures)\n");
  return 0;
}
