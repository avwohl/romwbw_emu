/*
 * console_idle.cc - a program that polls while it works is not idle
 *
 * Regression test for the console idle detector, which counted every console
 * status poll that found no key and called the guest idle after eight of them
 * with no output and no disk I/O in between.  Programs poll while they work -
 * MBASIC checks for ^C before every statement, PIP between records - so a busy
 * program read as idle, and the CLI did two things with that:
 *
 *   - slept 10 ms after every instruction for as long as it held, so MBASIC
 *     running `FOR I=1 TO 3000: X=X+1: NEXT` did not finish in 30 seconds;
 *   - with stdin a pipe at end of file, ended the run: `PIP DIR.HEX=DIR1.HEX,
 *     DIR2.HEX` was cut off with a zero-length DIR.$$$ on the disk, which is
 *     how tools/romwbw-batch found it.
 *
 * The rule now is that an empty poll counts only if it follows the previous
 * one within IDLE_POLL_MAX_GAP T-states.  The gaps below are the ones measured
 * on the real guests (see notePoll() in src/hbios_dispatch.cc).  A front end
 * sleeps once per idle poll, via takeIdlePoll(), not per instruction.
 *
 * The test links the real dispatcher against a stub console, drives HBIOS
 * through handlePortDispatch() as the proxy does, and moves the CPU's cycle
 * counter by hand between calls.
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
#ifdef _WIN32
#include <string.h>
#define EMU_TEST_STRNCASECMP _strnicmp
#else
#include <strings.h>
#define EMU_TEST_STRNCASECMP strncasecmp
#endif

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
    hbios.setCPU(&cpu);
    hbios.setMemory(&mem);
    hbios.setBlockingAllowed(true);  // the CLI
    cpu.cycles = 1000000;
  }

  void call(uint8_t func, uint8_t unit = 0, uint8_t e = 0) {
    cpu.regs.BC.set_high(func);
    cpu.regs.BC.set_low(unit);
    cpu.regs.DE.set_low(e);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
  }

  // `gap` T-states of guest work, then one console status poll.
  void poll_after(unsigned gap, uint8_t func = HBF_CIOIST) {
    cpu.cycles += gap;
    call(func);
  }

  // Polls with these gaps in turn, `n` of them.  Returns how many of them
  // takeIdlePoll() - the CLI's cue to sleep - answered yes to.
  int polls(const unsigned* gaps, int ngaps, int n, uint8_t func = HBF_CIOIST) {
    int sleeps = 0;
    for (int i = 0; i < n; i++) {
      poll_after(gaps[i % ngaps], func);
      if (hbios.takeIdlePoll()) sleeps++;
    }
    return sleeps;
  }
};

//=============================================================================
// Tests
//=============================================================================

int main() {
  printf("Console idle means waiting for a key, not working and polling\n");
  printf("------------------------------------------------------------\n");
  g_keys.clear();

  // --- the wait loops: still idle ---------------------------------------------
  {
    const unsigned prompt[] = {170};   // romldr "Boot [H=Help]:" loop
    Rig r;
    int sleeps = r.polls(prompt, 1, 20);
    check(r.hbios.isConsoleIdle(), "the boot loader's prompt loop is idle");
    check(sleeps == 13, "...and asks for a sleep on each poll from the eighth on");
  }
  {
    const unsigned fn11[] = {885};     // BDOS fn 11 loop on CP/M 3, the slowest
    Rig r;
    r.polls(fn11, 1, 20);
    check(r.hbios.isConsoleIdle(), "a BDOS fn 11 loop on CP/M 3 (885 T) is idle");
  }
  {
    const unsigned fn6[] = {370};
    Rig r;
    r.polls(fn6, 1, 20, HBF_VDAKST);
    check(r.hbios.isConsoleIdle(), "VDAKST counts the same way as CIOIST");
  }

  // --- programs at work: not idle ------------------------------------------
  {
    const unsigned pip[] = {46335};    // PIP concatenating two .HEX files
    Rig r;
    int sleeps = r.polls(pip, 1, 200);
    check(!r.hbios.isConsoleIdle(), "PIP polling between records is not idle");
    check(sleeps == 0, "...and never asks for a sleep");
  }
  {
    // MBASIC running FOR I=1 TO 3000: X=X+1: NEXT - the gaps as measured.
    const unsigned mbasic[] = {7380, 4470, 1865, 4180, 1905, 4220, 2010};
    Rig r;
    int sleeps = r.polls(mbasic, 7, 500);
    check(!r.hbios.isConsoleIdle(), "MBASIC running a loop is not idle");
    check(sleeps == 0, "...and never asks for a sleep");
  }
  {
    // Seven quick polls, then one after a long gap: the count starts over.
    const unsigned burst[] = {400, 400, 400, 400, 400, 400, 400, 50000,
                              400, 400, 400, 400, 400, 400};
    Rig r;
    r.polls(burst, 14, 14);
    check(!r.hbios.isConsoleIdle(),
          "a long gap restarts the count: 7 + 7 close polls are not 8 in a row");
  }

  // --- what resets it ------------------------------------------------------
  {
    const unsigned fast[] = {300};
    Rig r;
    r.polls(fast, 1, 10);
    check(r.hbios.isConsoleIdle(), "idle after ten close polls");
    r.call(HBF_CIOOUT, 0, 'x');
    check(!r.hbios.isConsoleIdle(), "output ends idle");
    r.polls(fast, 1, 10);
    g_keys.push_back('k');
    r.poll_after(300);
    check(!r.hbios.isConsoleIdle(), "a poll that finds a key ends idle");
    check(!r.hbios.takeIdlePoll(), "and asks for no sleep");
    g_keys.clear();
  }

  // --- takeIdlePoll is once per poll, not a level ----------------------------
  {
    const unsigned fast[] = {300};
    Rig r;
    r.polls(fast, 1, 10);
    check(r.hbios.isConsoleIdle(), "idle");
    check(!r.hbios.takeIdlePoll(),
          "with no new poll there is nothing to sleep for, idle or not");
  }

  printf("------------------------------------------------------------\n");
  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
