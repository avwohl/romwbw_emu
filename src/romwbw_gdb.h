/*
 * The GDB stub of romwbw_emu: a gdb::Target for banked memory.
 *
 * gdbstub.cc and gdbstub.h are cpmemu's.  This build compiles them out of the
 * sibling cpmemu checkout, as it does qkz80 (docs/GDB.md, and
 * cpmemu/docs/debugger_plan.md decision 9).  Only romwbw_emu.cc, the CLI,
 * includes this file; no file that ioscpm, z80cpmw or cpmdroid compile does.
 */

#ifndef ROMWBW_GDB_H
#define ROMWBW_GDB_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gdbstub.h"
#include "qkz80.h"
#include "romwbw_mem.h"

// Thrown out of RomwbwTarget::step() when the run ends under the debugger: the
// instruction limit, the end of console input, a termination signal, or "quit"
// in sim>.  main() catches it and sends the W or X packet.
struct GdbRunEnd {
  bool limit;   // the instruction limit, not a plain end
};

class RomwbwTarget : public gdb::Target {
 public:
  // The bank that holds a CP/M 2.2 program's low 32K, and where CP/M loads it.
  static const uint8_t PROGRAM_BANK = 0x8E;
  static const uint16_t PROGRAM_ADDR = 0x0100;

  RomwbwTarget(qkz80& cpu, banked_mem& mem, volatile bool* stop);

  // Runs one turn of the run loop; set by main().
  std::function<void(gdb::StepEvents&)> step_fn;
  // The stub that serves this target, for the rule break.
  gdb::Stub* stub;
  // And its tracker, whose shadow stack starts again when the program starts.
  gdb::Tracker* tracker;

  uint32_t read_reg(int index) override;
  void write_reg(int index, uint32_t value) override;
  uint8_t read_mem(uint16_t addr) override;
  void write_mem(uint16_t addr, uint8_t value) override;
  uint8_t read_code(uint16_t addr) override;
  void step(gdb::StepEvents& ev) override { step_fn(ev); }
  bool is_z80() override { return cpu_.get_cpu_mode() == qkz80::MODE_Z80; }
  void check_signals() override;
  int bdos_function_at(uint16_t) override { return -1; }
  bool watch_set(int, uint16_t, uint16_t, bool) override { return false; }
  bool watch_hit(uint16_t&, int&) override { return false; }
  bool break_applies(uint16_t pc, bool rule) override;
  bool monitor_command(const std::string& cmd, std::string& reply) override;

  // The monitor commands, in a form that a test can call.
  std::string cmd_bank(const std::string& arg);
  std::string cmd_break_bank(const std::string& arg);
  std::string cmd_wait_program(const std::string& arg);
  std::string cmd_status();
  std::string cmd_bt_reset();

 private:
  qkz80& cpu_;
  banked_mem& mem_;
  volatile bool* stop_;

  int view_bank_;          // -1: the bank the CPU has now
  int break_bank_;         // -1: a breakpoint applies in every bank

  bool wait_armed_;
  bool wait_is_sha_;
  std::vector<uint8_t> wait_bytes_;
  size_t wait_len_;
  std::string wait_sha_;   // 64 lower-case hex digits

  bool program_matches();
  static bool valid_bank(unsigned b) {
    return b <= 0x0F || (b >= 0x80 && b <= 0x8F);
  }
};

// Lower-case hex SHA-256 of a byte string.  Exposed for the unit test.
std::string romwbw_sha256_hex(const uint8_t* data, size_t len);

// platform::termination_requested() answers from this.  The CLI's signal
// handler calls it with the signal number.
void romwbw_gdb_note_signal(int sig);

#endif  // ROMWBW_GDB_H
