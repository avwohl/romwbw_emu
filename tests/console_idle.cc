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
 * one within IDLE_POLL_MAX_GAP T-states, or repeats one of the last few polls
 * exactly - the same gap, the same stores to memory, the same registers -
 * which is what a wait loop does and a working program does not.  The second
 * half is for ZPM3, whose command prompt polls every 3605 T-states: under the
 * gap rule alone it never counted as waiting, so the CLI never ended a piped
 * run there, never released held input to it, and span at its prompt.  The
 * gaps below are the ones measured on the real guests (see notePoll() in
 * src/hbios_dispatch.cc).  A front end sleeps once per idle poll, via
 * takeIdlePoll(), not per instruction.
 *
 * The test links the real dispatcher against a stub console, drives HBIOS
 * through handlePortDispatch() as the proxy does, moves the CPU's cycle
 * counter by hand between calls, and makes the stores a guest would make
 * between polls: none, the same ones every time round (a wait loop), or new
 * ones every time (a program at work, moving its loop variable on).
 *
 * It also covers the hold on typed-ahead input that the CLI puts on a piped
 * stdin (holdInputUntilWanted()): RomWBW's boot loader used to eat a
 * script's first line, reading keys up to Enter during its autoboot
 * countdown and flushing them at its prompt.  And the hold's other end: a
 * bounded time after the loader hands over to what it booted, it lets go, so
 * a program that prints between polls before anything reads a key still gets
 * its input.  And the one key it lets through early: an Esc a script leads
 * with, which stops the autoboot countdown as it did before the hold.
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

// What the CLI gives holdInputUntilWanted(): the next key, left queued.
static int peek_key() { return g_keys.empty() ? -1 : g_keys.front(); }

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

  uint8_t A() { return cpu.regs.AF.get_high(); }
  uint8_t E() { return cpu.regs.DE.get_low(); }

  // What the guest stores between two polls.  A wait loop stores the same
  // bytes every time round - return addresses, a status byte, and in ZPM3's
  // case the time it has just read from the RTC - and a program at work
  // stores something new: its loop variable, a pointer, a count.
  enum Work { NOTHING, SAME, COUNTING };
  Work work = NOTHING;
  unsigned counter = 0;

  void guest_stores() {
    switch (work) {
      case NOTHING:
        break;
      case SAME:
        mem.store_mem(0x9000, 0x17);
        mem.store_mem(0x9001, 0x52);
        break;
      case COUNTING:
        counter++;
        mem.store_mem(0x9000, (uint8_t)(counter & 0xFF));
        mem.store_mem(0x9001, (uint8_t)((counter >> 8) & 0xFF));
        break;
    }
  }

  // `gap` T-states of guest work, then one console status poll.
  void poll_after(unsigned gap, uint8_t func = HBF_CIOIST) {
    cpu.cycles += gap;
    guest_stores();
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

  // --- slow wait loops: idle because they repeat ----------------------------
  {
    // ZPM3's command prompt reads the RTC between polls, which puts them 3605
    // T-states apart - slower than MBASIC at work - but every time round it
    // stores the same time and arrives with the same registers.
    const unsigned zpm3[] = {3605};
    Rig r;
    r.work = Rig::SAME;
    int sleeps = r.polls(zpm3, 1, 20);
    check(r.hbios.isConsoleIdle(), "ZPM3's prompt (3605 T, repeating) is idle");
    // The first poll follows whatever ran before the loop and the second is
    // the first time round it, so the third is the first repeat, and the
    // eighth repeat is the ninth poll.
    check(sleeps == 12, "...and asks for a sleep on each poll from the ninth on");
    // Once a second the time it reads changes: that poll is new, and the
    // loop is idle again eight polls later.
    r.work = Rig::COUNTING;
    r.poll_after(3605);
    check(!r.hbios.isConsoleIdle(), "...a poll that stores something new ends it");
    r.work = Rig::SAME;
    r.polls(zpm3, 1, 8);
    check(r.hbios.isConsoleIdle(), "...and it is idle again once it repeats");
  }
  {
    // MBASIC waiting in `10 IF INKEY$="" THEN 10`: two polls each time round,
    // the ^C check before the statement and INKEY$ itself.
    const unsigned inkey[] = {805, 2230};
    Rig r;
    r.polls(inkey, 2, 20);
    check(r.hbios.isConsoleIdle(),
          "MBASIC in an INKEY$ loop (805, 2230, repeating) is idle");
  }

  // --- programs at work: not idle ------------------------------------------
  {
    const unsigned pip[] = {46335};    // PIP concatenating two .HEX files
    Rig r;
    r.work = Rig::COUNTING;
    int sleeps = r.polls(pip, 1, 200);
    check(!r.hbios.isConsoleIdle(), "PIP polling between records is not idle");
    check(sleeps == 0, "...and never asks for a sleep");
  }
  {
    // MBASIC running FOR I=1 TO 3000: X=X+1: NEXT - the gaps as measured.
    const unsigned mbasic[] = {7380, 4470, 1865, 4180, 1905, 4220, 2010};
    Rig r;
    r.work = Rig::COUNTING;
    int sleeps = r.polls(mbasic, 7, 500);
    check(!r.hbios.isConsoleIdle(), "MBASIC running a loop is not idle");
    check(sleeps == 0, "...and never asks for a sleep");
  }
  {
    // MBASIC running FOR I=1 TO 3000: NEXT polls every 2075 T-states for a
    // thousand polls on end - as regular as any wait loop.  Only I moves.
    const unsigned mbasic[] = {2075};
    Rig r;
    r.work = Rig::COUNTING;
    int sleeps = r.polls(mbasic, 1, 500);
    check(!r.hbios.isConsoleIdle() && sleeps == 0,
          "a loop polling at a steady gap but storing a new value each time is "
          "not idle");
  }
  {
    // The same, with the count kept in a register rather than memory.
    Rig r;
    int idle = 0;
    for (int i = 0; i < 50; i++) {
      r.cpu.regs.HL.set_pair16((uint16_t)i);
      r.poll_after(3605);
      if (r.hbios.isConsoleIdle()) idle++;
    }
    check(idle == 0, "...nor one whose registers differ each time round");
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

  // --- piped input is held until the guest wants it -----------------------------
  //
  // RomWBW's boot loader ate a script's first line: during the autoboot
  // countdown it reads keys up to Enter, looking for Esc.  With the input
  // held, a status poll that is not part of a wait loop sees no key.
  {
    // romldr's countdown polls between 1/64 s delays and decrements its
    // sub-second counter every time round (acmd_to_64), so no poll repeats.
    const unsigned countdown[] = {50000};
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back('S');
    r.hbios.holdInputUntilWanted();
    r.poll_after(countdown[0]);
    check(r.A() == 0, "held: the countdown's status poll sees no key");
    r.polls(countdown, 1, 50);
    check(r.A() == 0 && r.hbios.isInputHeld(),
          "held: fifty countdown polls still see none - it is counting, not "
          "waiting");
    r.call(HBF_CIOIN);
    check(r.E() == 'S', "held: a CIOIN gets the key at once - it is wanted");
    check(!r.hbios.isInputHeld(), "...and releases the rest of the input");
    g_keys.push_back('T');
    r.poll_after(50000);
    check(r.A() != 0, "released: a status poll sees the next key");
    g_keys.clear();
  }
  {
    // The boot loader's prompt: it flushes what is waiting, then polls in a
    // loop.  The flush must find nothing and the loop must get the key.
    const unsigned prompt[] = {170};
    Rig r;
    g_keys.clear();
    g_keys.push_back('2');
    r.hbios.holdInputUntilWanted();
    r.poll_after(170);
    check(r.A() == 0, "held: the prompt's flush finds nothing to throw away");
    int seen = 0;
    for (int i = 0; i < 20 && !seen; i++) {
      r.poll_after(prompt[0]);
      if (r.A() != 0) seen = i + 2;
    }
    check(seen == 9, "held: the prompt's poll loop gets the key once it counts "
                     "as waiting, on its ninth poll");
    g_keys.clear();
  }
  {
    // ZPM3 reads a key only after a poll reports one, so a hold released by
    // nothing but a CIOIN or a tight loop kept piped input from it for ever.
    const unsigned zpm3[] = {3605};
    Rig r;
    r.work = Rig::SAME;
    g_keys.clear();
    g_keys.push_back('D');
    r.hbios.holdInputUntilWanted();
    int seen = 0;
    for (int i = 0; i < 40 && !seen; i++) {
      r.poll_after(zpm3[0]);
      if (r.A() != 0) seen = i + 1;
    }
    check(seen == 10, "held: ZPM3's prompt (3605 T apart) gets the key once it "
                      "counts as waiting, on its tenth poll");
    g_keys.clear();
  }
  {
    // A program that runs before anything reads a key - a game loop, a
    // display started at boot - and prints between polls.  Output resets the
    // idle count, so it never waits, and it never reads: under a hold
    // released only by a read or a wait loop it saw none of its input, where
    // without the hold it saw all of it.  So the boot loader handing over to
    // what it booted - SYSSET BOOTINFO, its last act before the jump - starts
    // a clock, and the input goes 120M T-states later.  Not at once: the OS's
    // own start polls too, and CP/M 3's CPMLDR, NZCOM's loader and Z3PLUS
    // took the first key of a script that way.  The slowest of them, NZCOM,
    // waits at its prompt 16.4M T-states after the handover.
    const unsigned long long BOUND = 120000000ull;
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back('K');
    r.hbios.holdInputUntilWanted();
    bool seen = false;
    for (int i = 0; i < 200 && !seen; i++) {
      r.call(HBF_CIOOUT, 0, '.');
      r.poll_after(2000);
      seen = r.A() != 0;
    }
    check(!seen && r.hbios.isInputHeld(),
          "held: a loop that prints between polls sees no key in 200 turns - "
          "it neither reads nor waits");
    r.cpu.regs.DE.set_high(2);             // D = boot unit
    r.cpu.regs.HL.set_low(0x8E);           // L = boot bank
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);   // C = subfunction; E = slice 0
    unsigned long long handover = r.cpu.cycles;
    r.call(HBF_CIOOUT, 0, '.');
    r.poll_after(2000);
    check(r.A() == 0 && r.hbios.isInputHeld(),
          "held: just after the boot loader hands over (SYSSET BOOTINFO), a "
          "poll still sees no key - the OS is starting");
    r.cpu.cycles = handover + BOUND - 3000;
    r.call(HBF_CIOOUT, 0, '.');
    r.poll_after(2000);
    check(r.A() == 0, "held: ...nor one 1000 T-states short of the bound");
    r.call(HBF_CIOOUT, 0, '.');
    r.poll_after(2000);
    check(r.A() != 0 && !r.hbios.isInputHeld(),
          "released: the first poll 120M T-states after the handover sees the "
          "key");
    g_keys.clear();
  }
  {
    // An OS that waits for a key before the bound gets the input then, as it
    // always did.
    Rig r;
    g_keys.clear();
    g_keys.push_back('D');
    r.hbios.holdInputUntilWanted();
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);
    r.cpu.cycles += 4500000;               // CP/M 3's start
    r.call(HBF_CIOIN);
    check(r.E() == 'D', "held: an OS reading a key before the bound gets it");
    g_keys.clear();
  }
  {
    // A reset reboots into the boot loader, whose countdown must not get
    // the input because an OS started a while ago.
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back('Z');
    r.hbios.holdInputUntilWanted();
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);
    r.cpu.cycles += 1000000;
    r.call(HBF_SYSRESET, 0x01);            // warm boot
    r.cpu.cycles += 200000000ull;
    r.poll_after(50000);
    check(r.A() == 0 && r.hbios.isInputHeld(),
          "held: after a reset the loader's countdown sees no key, however "
          "long ago an OS started");
    g_keys.clear();
  }
  {
    // The same for HBIOSDispatch::reset(), which a front end calls to
    // restart the machine - ioscpm and z80cpmw do.  It did not forget the
    // handover, which only SYSRESET did, so the clock ran on from the last
    // one and the restarted loader's countdown got the input.
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back('Z');
    r.hbios.holdInputUntilWanted();
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);
    r.cpu.cycles += 1000000;
    r.hbios.reset();
    r.cpu.cycles += 200000000ull;
    r.poll_after(50000);
    check(r.A() == 0 && r.hbios.isInputHeld(),
          "held: after HBIOSDispatch::reset() the loader's countdown sees no "
          "key either");
    g_keys.clear();
  }
  // --- ...but a script's leading Esc still stops the autoboot countdown -------
  //
  // romldr's countdown takes Esc to mean "go to my prompt", and 0448175 let a
  // pipe press it: `(printf '\033'; sleep 1.5; printf 'D\r') | romwbw_emu
  // --boot=2` stopped at the prompt and listed the devices.  Held, the
  // countdown never saw it, CP/M booted and its CCP got `^[D`.  The CLI now
  // gives the hold a way to see the next byte, and an Esc at the front gets
  // through until the loader hands over.
  {
    const unsigned countdown[] = {50000};
    const unsigned prompt[] = {170};
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back(0x1B);
    g_keys.push_back('D');
    g_keys.push_back('\r');
    r.hbios.holdInputUntilWanted(true, peek_key);
    r.poll_after(countdown[0]);
    check(r.A() != 0, "held: a leading Esc shows to the countdown's status "
                      "poll");
    r.call(HBF_CIOIN);
    check(r.E() == 0x1B && r.hbios.isInputHeld(),
          "held: the countdown's CIOIN gets the Esc, and the rest stays held");
    r.work = Rig::NOTHING;
    r.poll_after(prompt[0]);
    check(r.A() == 0, "held: the prompt's flush then finds nothing to throw "
                      "away");
    int seen = 0;
    for (int i = 0; i < 20 && !seen; i++) {
      r.poll_after(prompt[0]);
      if (r.A() != 0) seen = i + 2;
    }
    check(seen == 9, "held: the prompt's poll loop gets the line after the "
                     "Esc once it counts as waiting");
    r.call(HBF_CIOIN);
    check(r.E() == 'D' && !r.hbios.isInputHeld(),
          "released: ...and reads it, which lets go of the rest");
    g_keys.clear();
  }
  {
    // Every other front end holds, if at all, without a peek, and the Esc
    // is held with the rest.
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back(0x1B);
    r.hbios.holdInputUntilWanted();
    r.poll_after(50000);
    check(r.A() == 0, "held without a peek: a leading Esc is held like any "
                      "key");
    g_keys.clear();
  }
  {
    // Only a leading Esc: one behind the first line is that line's business.
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back('D');
    g_keys.push_back(0x1B);
    r.hbios.holdInputUntilWanted(true, peek_key);
    r.poll_after(50000);
    check(r.A() == 0, "held: an Esc behind another key does not show");
    g_keys.clear();
  }
  {
    // Once the loader has handed over, an OS is starting, and CP/M 3's
    // CPMLDR and NZCOM's loader take what a poll shows them.  The Esc was
    // for the loader.
    Rig r;
    r.work = Rig::COUNTING;
    g_keys.clear();
    g_keys.push_back(0x1B);
    r.hbios.holdInputUntilWanted(true, peek_key);
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);
    r.poll_after(50000);
    check(r.A() == 0 && r.hbios.isInputHeld(),
          "held: after the handover a leading Esc is held like any key");
    r.call(HBF_SYSRESET, 0x01);            // warm boot: the loader again
    r.poll_after(50000);
    check(r.A() != 0, "held: after a reset the loader's countdown sees it "
                      "again");
    g_keys.clear();
  }
  {
    Rig r;
    g_keys.clear();
    g_keys.push_back('X');
    r.call(HBF_VDAKST);
    check(r.A() != 0, "not held (a terminal, or any other front end): a queued "
                      "key shows at once");
    g_keys.clear();
  }
  {
    // VDAKFL flushes the keyboard.  Held input is not in the guest's keyboard
    // yet, so a flush must not throw it away.
    Rig r;
    g_keys.clear();
    g_keys.push_back('Q');
    r.hbios.holdInputUntilWanted();
    r.call(HBF_VDAKFL);
    check(g_keys.size() == 1, "held: VDAKFL leaves held input alone");
    r.call(HBF_CIOIN);
    g_keys.push_back('R');
    r.call(HBF_VDAKFL);
    check(g_keys.empty(), "released: VDAKFL flushes what is waiting");
    g_keys.clear();
  }

  // --- the boot loader's autoboot countdown takes real time ----------------------
  //
  // romldr's countdown polls once per 1/64 s turn, timed by vdelay: 976 outer
  // turns of 9 instructions at the 4 MHz CPUINFO reports, and qkz80 counts 5
  // cycles for every instruction - 43,920 cycles, plus the poll's own few
  // dozen instructions, 150 or so.  Run at host speed a SYSCONF-set `S AB
  // E,3` was over in 0.4 s.  takeLoaderPoll() tells a front end how long each
  // turn was meant to take, and the CLI waits that long.  But only the
  // loader's: an OS, a ROM application and a loader command run as fast as
  // before, and the idle rule above is untouched.
  {
    Rig r;
    r.call(HBF_SYSGET, SYSGET_CPUINFO);
    check(r.cpu.regs.HL.get_low() == HBIOSDispatch::CPU_KHZ / 1000 &&
          r.cpu.regs.DE.get_pair16() == HBIOSDispatch::CPU_KHZ,
          "countdown: CPUINFO reports CPU_KHZ, the clock pacing goes by");
  }
  {
    const unsigned turn = 43920 + 150;
    Rig r;
    r.work = Rig::COUNTING;                // acmd_to_64, every turn
    unsigned long long us = 1;
    r.poll_after(turn);
    bool first = r.hbios.takeLoaderPoll(&us);
    check(first && us == 0, "countdown: the loader's first empty poll starts a "
                            "run, and asks for no wait");
    check(!r.hbios.takeLoaderPoll(&us), "countdown: ...and is taken once");
    unsigned long long total = 0;
    int reported = 0, sleeps = 0;
    for (int i = 0; i < 64; i++) {
      r.poll_after(turn);
      if (r.hbios.takeLoaderPoll(&us)) {
        reported++;
        total += us;
      }
      if (r.hbios.takeIdlePoll()) sleeps++;
    }
    check(reported == 64 && us >= 15600 && us <= 15700,
          "countdown: each turn after it is 1/64 s - 15.6 ms - to wait for");
    check(total >= 990000 && total <= 1010000,
          "countdown: 64 turns come to one second");
    check(!r.hbios.isConsoleIdle() && sleeps == 0,
          "countdown: ...and it is still not idle - it counts, it does not wait");
  }
  {
    // MBASIC after the handover, as fast as 1.48 made it: nothing to wait for.
    const unsigned mbasic[] = {7380, 4470, 1865, 4180, 1905, 4220, 2010};
    Rig r;
    r.work = Rig::COUNTING;
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);
    int reported = 0;
    for (int i = 0; i < 500; i++) {
      r.poll_after(mbasic[i % 7]);
      if (r.hbios.takeLoaderPoll(nullptr)) reported++;
    }
    check(reported == 0, "countdown: after the handover no poll is paced - "
                         "MBASIC runs at full speed");
    // A ROM application is handed over to the same way (romldr's appload),
    // and a reset puts the loader back.
    r.call(HBF_SYSRESET, 0x02);            // cold boot
    unsigned long long us = 1;
    r.poll_after(44070);
    check(r.hbios.takeLoaderPoll(&us) && us == 0,
          "countdown: after a SYSRESET the loader's polls are paced again, "
          "from a new run");
    r.poll_after(44070);
    check(r.hbios.takeLoaderPoll(&us) && us > 15000,
          "countdown: ...the next one a turn later");
    r.call(HBF_SYSSET, SYSSET_BOOTINFO);
    r.hbios.reset();
    r.poll_after(44070);
    check(r.hbios.takeLoaderPoll(&us) && us == 0,
          "countdown: and after HBIOSDispatch::reset() too");
  }
  {
    // What breaks a run: a key read (a loader command starts with one, and is
    // never paced), a poll that finds a key, disk I/O, and a gap no turn has.
    Rig r;
    r.work = Rig::COUNTING;
    unsigned long long us = 1;
    r.poll_after(44070);
    r.poll_after(44070);
    check(r.hbios.takeLoaderPoll(&us) && us > 15000, "countdown: a run");
    g_keys.clear();
    g_keys.push_back('D');
    r.poll_after(44070);
    check(!r.hbios.takeLoaderPoll(&us),
          "countdown: a poll that finds a key is not paced");
    r.call(HBF_CIOIN);
    r.poll_after(44070);
    check(r.hbios.takeLoaderPoll(&us) && us == 0,
          "countdown: after a key is read the next poll starts a new run");
    r.poll_after(44070);
    r.call(HBF_DIOSTATUS, 2);
    r.poll_after(44070);
    check(r.hbios.takeLoaderPoll(&us) && us == 0,
          "countdown: disk I/O starts a new run - a boot under way");
    r.poll_after(3000000);                 // over a second at 45 cycles/16 us
    check(r.hbios.takeLoaderPoll(&us) && us == 0,
          "countdown: a gap of more than a second is no turn, and is not "
          "waited for");
    r.poll_after(170);                     // the prompt's poll loop
    check(r.hbios.takeLoaderPoll(&us) && us < 100,
          "countdown: the prompt's poll loop asks for next to nothing");
    g_keys.clear();
  }

  printf("------------------------------------------------------------\n");
  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
