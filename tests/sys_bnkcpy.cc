/*
 * sys_bnkcpy.cc - SYSBNKCPY advances HL and DE, so a run of them walks forward
 *
 * WHAT THIS IS FOR.  Source/Doc/SystemGuide.md, Function 0xF5, documents the
 * returned values as "HL: New Source Address" and "DE: New Destination
 * Address", and says in the paragraph above the table that a caller may make
 * repeated SYSBNKCPY calls after ONE SYSSETCPY as long as the banks and the
 * length do not change.  Those two facts only fit together if the addresses
 * advance: that is how a caller walks a region larger than one block.
 *
 * The emulator copied the bytes and left HL and DE exactly as passed, so the
 * second call in such a run copied the same block again, and the third, and the
 * region past the first `count` bytes was never written at all.  Nothing caught
 * it because nothing here had ever made two calls in a row.
 *
 * Run: make -C src test_sys_bnkcpy && ./src/test_sys_bnkcpy
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


  // SYSSETCPY takes the two bank ids and the byte count; SYSBNKCPY takes the
  // two addresses.  hbios.inc: "E: Source Bank, D: Destination Bank,
  // HL: Byte Count".
  void setcpy(uint8_t src_bank, uint8_t dst_bank, uint16_t count) {
    cpu.regs.BC.set_high(HBF_SYSSETCPY);
    cpu.regs.DE.set_high(dst_bank);
    cpu.regs.DE.set_low(src_bank);
    cpu.regs.HL.set_pair16(count);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
  }

  void bnkcpy(uint16_t src_addr, uint16_t dst_addr) {
    cpu.regs.BC.set_high(HBF_SYSBNKCPY);
    cpu.regs.HL.set_pair16(src_addr);
    cpu.regs.DE.set_pair16(dst_addr);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
  }

  uint16_t HL() { return cpu.regs.HL.get_pair16(); }
  uint16_t DE() { return cpu.regs.DE.get_pair16(); }
};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("SYSBNKCPY advances HL and DE, so a run of calls walks forward\n");
  printf("------------------------------------------------------------\n");

  const uint8_t SRC_BANK = 0x81;
  const uint8_t DST_BANK = 0x82;
  const uint16_t COUNT = 0x40;
  const uint16_t SRC = 0x1000;
  const uint16_t DST = 0x2000;

  {
    Rig r;
    // Fill three blocks of the source bank with a per-block marker.
    for (int blk = 0; blk < 3; blk++) {
      for (uint16_t i = 0; i < COUNT; i++) {
        r.mem.write_bank(SRC_BANK, SRC + blk * COUNT + i, (uint8_t)(0xA0 + blk));
      }
    }

    r.setcpy(SRC_BANK, DST_BANK, COUNT);

    r.bnkcpy(SRC, DST);
    check(r.HL() == SRC + COUNT, "after one copy HL is the new source address");
    check(r.DE() == DST + COUNT, "and DE is the new destination address");

    // The documented usage: keep calling without another SYSSETCPY.
    r.bnkcpy(r.HL(), r.DE());
    r.bnkcpy(r.HL(), r.DE());

    bool walked = true;
    for (int blk = 0; blk < 3; blk++) {
      for (uint16_t i = 0; i < COUNT; i++) {
        if (r.mem.read_bank(DST_BANK, DST + blk * COUNT + i) != (uint8_t)(0xA0 + blk)) {
          walked = false;
        }
      }
    }
    check(walked,
          "three calls after one SYSSETCPY copy three CONSECUTIVE blocks, not the first one three times");
    if (!walked) {
      printf("      dst block markers: %02X %02X %02X\n",
             r.mem.read_bank(DST_BANK, DST),
             r.mem.read_bank(DST_BANK, DST + COUNT),
             r.mem.read_bank(DST_BANK, DST + 2 * COUNT));
    }
  }

  {
    // A zero-length copy must still be well defined: nothing moves and the
    // addresses come back where they went in.
    Rig r;
    r.setcpy(SRC_BANK, DST_BANK, 0);
    r.bnkcpy(SRC, DST);
    check(r.HL() == SRC && r.DE() == DST,
          "a zero-byte copy leaves HL and DE exactly where they were");
  }

  printf("------------------------------------------------------------\n");
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all checks passed (0 failures)\n");
  return 0;
}
