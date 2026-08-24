/*
 * cli_console.cc - what the CLI console does with the bytes a user types
 *
 * The two cases todo.txt asked for, plus the ones the same commit turned on
 * and could not prove:
 *
 *   1. A piped "a\nb\n" must still deliver two CRs. A script feeding the guest
 *      through a pipe ends its lines with LF, and CP/M wants CR, so this path
 *      rewrites. c_iflag never applies to a pipe, so nothing else can do it.
 *
 *   2. On a tty, Enter must arrive as 0x0D and Ctrl+J as 0x0A. emu_io_init()
 *      clears ICRNL so the terminal delivers Enter as CR natively; the LF
 *      rewrite therefore has to be *absent* here, or the two keys collapse
 *      into one. That asymmetry - rewrite on a pipe, never on a tty - is the
 *      part that regresses silently, because either half alone still looks
 *      right in ordinary use.
 *
 *   3. The control keys the raw mode has to hand to the guest. IXON off or
 *      ^S/^Q are flow control and never arrive - half the WordStar diamond.
 *      IEXTEN off or BSD/XNU eats ^V (VLNEXT) and ^O (VDISCARD), which are
 *      gated on IEXTEN alone there, outside the ICANON block. todo.txt records
 *      that the IEXTEN clear "cannot be proven on linux" and wants someone
 *      with a mac to press ^V and ^O by hand: on a pty this is provable
 *      automatically, and on Linux it simply passes for the second reason.
 *
 *   4. The reserved escape key is consumed on a tty and not on a pipe. One
 *      press of ^E used to both move the WordStar cursor and open sim>.
 *
 * Each case runs in a forked child so the statics in emu_io_cli.cc start
 * clean, and because raw mode has to be established before the parent writes
 * anything - the line discipline processes input as it arrives, so bytes sent
 * ahead of tcsetattr would be judged by the old settings. The child raises a
 * ready flag when it is set up; the parent feeds it then.
 *
 * Build and run:  make -C src test
 */

#include "emu_io.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#else
#include <libutil.h>
#endif

//=============================================================================
// Reporting
//
// The checks run in a child, so they print as they go and the child's exit
// status carries the count back.
//=============================================================================

static int child_failures = 0;

static void check(bool ok, const char* what) {
  printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) child_failures++;
}

// Name a byte the way a person reading the output would say it.
static const char* spell(int b) {
  static char buf[32];
  switch (b) {
    case -1: return "EOF (-1)";
    case EMU_CONSOLE_RETRY: return "EMU_CONSOLE_RETRY (-2)";
    case 0x0D: return "CR (0x0D)";
    case 0x0A: return "LF (0x0A)";
    default: break;
  }
  if (b < 0x20) {
    snprintf(buf, sizeof(buf), "^%c (0x%02X)", '@' + b, b);
  } else if (b < 0x7F) {
    snprintf(buf, sizeof(buf), "'%c' (0x%02X)", b, b);
  } else {
    snprintf(buf, sizeof(buf), "0x%02X", b);
  }
  return buf;
}

// emu_console_has_input() polls with a zero timeout, so it can answer "not
// yet" while the parent's bytes are still in flight through the pty - the
// scenarios below are testing that a byte arrives through the peek path, not
// how quickly a pty moves it. Spin, bounded well inside the child's alarm.
static bool wait_for_input() {
  for (int i = 0; i < 5000; i++) {  // ~5s at 1ms a turn
    if (emu_console_has_input()) return true;
    usleep(1000);
  }
  return false;
}

// Read one byte and say whether it was the one expected.
static void expect(int want, const char* what) {
  int got = emu_console_read_char();
  char label[192];
  if (got == want) {
    snprintf(label, sizeof(label), "%s", what);
  } else {
    // spell() has one static buffer, so the two calls cannot be in one
    // snprintf argument list.
    char wantbuf[32];
    snprintf(wantbuf, sizeof(wantbuf), "%s", spell(want));
    snprintf(label, sizeof(label), "%s - wanted %s, got %s", what, wantbuf,
             spell(got));
  }
  check(got == want, label);
}

//=============================================================================
// Running one case with a chosen stdin
//=============================================================================

typedef void (*body_fn)(void);
typedef void (*prepare_fn)(int slave_fd);

static int failures = 0;
static int cases = 0;

static void heading(const char* title) {
  printf("\n%s\n", title);
  for (int i = 0; i < 68; i++) putchar('-');
  putchar('\n');
  fflush(stdout);
}

// Collect a child's verdict.
//
// A child killed by SIGALRM ran out its guard waiting for a byte, which is the
// shape a swallowed key takes: the line discipline consumed it, so the read
// never returns rather than returning something wrong. That is how a restored
// IEXTEN shows up - VLNEXT eats the ^V and then waits for the character it is
// supposed to quote - so it is worth naming rather than printing a bare signal
// number.
static void reap(pid_t pid, const char* title) {
  int wstatus = 0;
  waitpid(pid, &wstatus, 0);
  cases++;
  if (WIFEXITED(wstatus)) {
    failures += WEXITSTATUS(wstatus);
  } else if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGALRM) {
    printf("FAIL: %s - timed out waiting for a byte that never arrived; the "
           "terminal line discipline probably swallowed it\n", title);
    failures++;
  } else {
    printf("FAIL: %s did not finish (signal %d)\n", title,
           WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0);
    failures++;
  }
}

// Feed `data` down a pipe and run `body` with that pipe as stdin. The pipe is
// filled and closed before the fork, so there is nothing to synchronise.
static void run_on_pipe(const char* title, body_fn body, const char* data,
                        size_t len) {
  heading(title);
  int fds[2];
  if (pipe(fds) != 0) {
    printf("FAIL: %s - pipe() failed: %s\n", title, strerror(errno));
    failures++;
    cases++;
    return;
  }
  if (len && write(fds[1], data, len) != (ssize_t)len) {
    printf("FAIL: %s - short write to pipe\n", title);
    failures++;
    cases++;
    return;
  }
  close(fds[1]);  // the guest must see a real EOF at the end

  pid_t pid = fork();
  if (pid == 0) {
    alarm(15);  // a blocked read must fail the test, not hang it
    dup2(fds[0], STDIN_FILENO);
    close(fds[0]);
    emu_io_init();
    body();
    emu_io_cleanup();
    fflush(stdout);
    _exit(child_failures > 120 ? 120 : child_failures);
  }
  close(fds[0]);
  reap(pid, title);
}

// Run `body` with a pty as stdin, feeding `data` only once the child has raw
// mode on: the line discipline judges input as it arrives.
//
// `prepare`, if given, runs on the slave before the fork, so a case can hand
// the child a terminal that arrives in a particular state. That matters for
// any flag a fresh pty leaves off: emu_io_init() only ever *clears* bits, so
// clearing one the pty never set proves nothing unless the test sets it first.
static void run_on_pty(const char* title, body_fn body,
                       const unsigned char* data, size_t len,
                       prepare_fn prepare = NULL) {
  heading(title);
  int master = -1, slave = -1;
  if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
    printf("FAIL: %s - openpty() failed: %s\n", title, strerror(errno));
    failures++;
    cases++;
    return;
  }
  int ready[2];
  if (pipe(ready) != 0) {
    printf("FAIL: %s - pipe() failed: %s\n", title, strerror(errno));
    failures++;
    cases++;
    return;
  }

  if (prepare) prepare(slave);

  pid_t pid = fork();
  if (pid == 0) {
    alarm(15);
    close(master);
    close(ready[0]);
    dup2(slave, STDIN_FILENO);
    close(slave);
    emu_io_init();
    // Raw mode is on: it is safe for the parent to type now.
    ssize_t ignored = write(ready[1], "r", 1);
    (void)ignored;
    close(ready[1]);
    body();
    emu_io_cleanup();
    fflush(stdout);
    _exit(child_failures > 120 ? 120 : child_failures);
  }
  close(slave);
  close(ready[1]);
  char flag;
  if (read(ready[0], &flag, 1) == 1 && len) {
    ssize_t written = write(master, data, len);
    (void)written;
  }
  close(ready[0]);
  reap(pid, title);
  close(master);
}

//=============================================================================
// The cases
//=============================================================================

// 1. A pipe ends its lines with LF and the guest has to see CR.
static void body_pipe_line_endings() {
  expect('a', "a piped 'a' arrives as itself");
  expect(0x0D, "the LF after it arrives as CR");
  expect('b', "a piped 'b' arrives as itself");
  expect(0x0D, "the second LF arrives as CR too");
  expect(-1, "and then the pipe is at EOF");
  check(emu_console_input_exhausted(),
        "a read past EOF reports the input exhausted, so the run can wind down");
}

// The same rewrite must not fire twice, and must not touch anything else.
static void body_pipe_leaves_other_bytes_alone() {
  expect(0x0D, "a CR already in the pipe stays CR - not doubled, not dropped");
  expect(0x09, "TAB survives a pipe");
  expect(0x1A, "^Z survives a pipe as a byte, rather than ending it");
  expect(0xFF, "the 8th bit survives a pipe");
}

// A piped script must not be able to trip the emulator's escape key: it
// reserves nothing, whatever the frontend armed.
static void body_pipe_reserves_nothing() {
  check(!emu_console_check_escape(0x05),
        "arming ^E on a pipe reports no escape pending");
  expect(0x05, "and a piped ^E reaches the guest instead of opening sim>");
  check(!emu_console_check_escape(0x05),
        "reading it did not latch an escape either");
  expect('x', "the byte after it is undisturbed");
}

// 2. The case the whole asymmetry exists for.
static void body_tty_enter_vs_ctrl_j() {
  expect(0x0D, "Enter on a tty arrives as CR - ICRNL is cleared, so the "
               "terminal already sent CR");
  expect(0x0A, "Ctrl+J on a tty arrives as LF - no rewrite here, or the two "
               "keys would be one");
}

// The same two bytes, delivered through the peek path instead. That path has
// its own copy of the tty test, and a bug in it turns a typed Ctrl+J back into
// Enter - invisibly, because the direct read above still passes.
static void body_tty_enter_vs_ctrl_j_peeked() {
  check(wait_for_input(), "has_input() sees the typed Enter");
  expect(0x0D, "Enter delivered through the peek path is still CR");
  check(wait_for_input(), "has_input() sees the typed Ctrl+J");
  expect(0x0A, "Ctrl+J delivered through the peek path is still LF");
}

// 3. The control keys raw mode has to preserve.
static void body_tty_flow_control_keys() {
  expect(0x13, "^S reaches the guest - IXON is cleared, so it is not XOFF");
  expect(0x11, "^Q reaches the guest - not XON");
}

static void body_tty_extended_keys() {
  // On BSD/XNU these two are gated on IEXTEN alone, outside ICANON, so this
  // is the check todo.txt wanted a Mac for. On Linux they sit inside the
  // ICANON block and are already off, so it passes there for a second reason.
  expect(0x16, "^V reaches the guest - IEXTEN is cleared, so it is not VLNEXT");
  expect(0x0F, "^O reaches the guest - not VDISCARD");
}

static void body_tty_signal_keys() {
  expect(0x03, "^C reaches the guest as a byte - ISIG is cleared, so it is "
               "not SIGINT");
  expect(0x1A, "^Z reaches the guest - not SIGTSTP");
  expect(0x1C, "^\\ reaches the guest - not SIGQUIT");
}

// A terminal handed to us with ISTRIP already set. A fresh pty has it off, so
// without this the "8th bit survives" check would pass whether or not the code
// cleared anything.
static void prepare_istrip(int slave_fd) {
  struct termios t;
  if (tcgetattr(slave_fd, &t) != 0) return;
  t.c_iflag |= ISTRIP;
  tcsetattr(slave_fd, TCSANOW, &t);
}

static void body_tty_eight_bit() {
  expect(0xE1,
         "a byte with the 8th bit set survives a terminal that arrived with "
         "ISTRIP set - the clear is what preserves it");
}

static void body_tty_diamond() {
  // The keys a WordStar user is pressing while all of the above is going on.
  expect(0x01, "^A survives");
  expect(0x04, "^D survives");
  expect(0x05, "^E survives when no key is reserved");
  expect(0x06, "^F survives");
  expect(0x12, "^R survives");
  expect(0x18, "^X survives");
  expect(0x0B, "^K survives");
  expect(0x10, "^P survives");
  expect(0x19, "^Y survives");
}

// 4. The reserved key is the emulator's, and only on a tty.
static void body_tty_escape_is_consumed() {
  check(!emu_console_check_escape(0x05),
        "arming ^E reports nothing pending yet");
  expect(EMU_CONSOLE_RETRY,
         "a typed ^E is withheld from the guest rather than delivered twice");
  check(emu_console_check_escape(0x05),
        "and is reported to the frontend as an escape");
  check(!emu_console_check_escape(0x05),
        "the escape is reported once, not on every poll");
  expect('z', "the byte after the escape reaches the guest normally");
}

// escape_char 0 means the frontend reserves no key: --escape=none, and the
// contract v1.36 added. Nothing may be consumed.
static void body_tty_escape_none_consumes_nothing() {
  check(!emu_console_check_escape(0),
        "an escape_char of 0 reserves nothing and reports nothing");
  expect(0x05, "so ^E reaches the guest untouched");
}

//=============================================================================

int main() {
  printf("The CLI console, driven through a real pipe and a real pty\n");

  run_on_pipe("A piped script ends its lines with LF; the guest needs CR",
              body_pipe_line_endings, "a\nb\n", 4);

  {
    const char data[] = {0x0D, 0x09, 0x1A, (char)0xFF};
    run_on_pipe("The pipe rewrite touches LF and nothing else",
                body_pipe_leaves_other_bytes_alone, data, sizeof(data));
  }

  {
    const char data[] = {0x05, 'x'};
    run_on_pipe("A pipe reserves no key for the emulator",
                body_pipe_reserves_nothing, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0x0D, 0x0A};
    run_on_pty("Enter and Ctrl+J are different keys on a tty",
               body_tty_enter_vs_ctrl_j, data, sizeof(data));
    run_on_pty("...and still are when delivered through the peek path",
               body_tty_enter_vs_ctrl_j_peeked, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0x13, 0x11};
    run_on_pty("^S and ^Q are WordStar keys, not flow control",
               body_tty_flow_control_keys, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0x16, 0x0F};
    run_on_pty("^V and ^O are WordStar keys, not line-editing verbs",
               body_tty_extended_keys, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0x03, 0x1A, 0x1C};
    run_on_pty("^C, ^Z and ^\\ are bytes for the guest, not signals",
               body_tty_signal_keys, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0xE1};
    run_on_pty("ISTRIP arrives set, and the 8th bit still survives",
               body_tty_eight_bit, data, sizeof(data), prepare_istrip);
  }

  {
    const unsigned char data[] = {0x01, 0x04, 0x05, 0x06,
                                  0x12, 0x18, 0x0B, 0x10, 0x19};
    run_on_pty("The rest of the WordStar diamond",
               body_tty_diamond, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0x05, 'z'};
    run_on_pty("The reserved key is consumed once, on a tty",
               body_tty_escape_is_consumed, data, sizeof(data));
  }

  {
    const unsigned char data[] = {0x05};
    run_on_pty("--escape=none reserves nothing",
               body_tty_escape_none_consumes_nothing, data, sizeof(data));
  }

  printf("\n");
  for (int i = 0; i < 68; i++) putchar('=');
  printf("\n%d case%s run, %d failure%s\n", cases, cases == 1 ? "" : "s",
         failures, failures == 1 ? "" : "s");
  printf("%s\n", failures ? "TESTS FAILED" : "all checks passed");
  return failures ? 1 : 0;
}
