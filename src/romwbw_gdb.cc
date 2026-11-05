/*
 * The GDB stub of romwbw_emu: a gdb::Target for banked memory.  See
 * romwbw_gdb.h and docs/GDB.md.
 */

#include "romwbw_gdb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4).  The debug file of the um80 tools names a program by
// its length and this hash, and "monitor wait-program sha256" compares them.
// ---------------------------------------------------------------------------

namespace {

const uint32_t K256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// The first word of s, which is cut off s.
std::string take_word(std::string& s) {
  s = trim(s);
  size_t e = s.find_first_of(" \t");
  std::string w = s.substr(0, e);
  s = e == std::string::npos ? "" : trim(s.substr(e));
  return w;
}

bool parse_hex_number(const std::string& w, unsigned& v) {
  if (w.empty() || w.size() > 8) return false;
  char* end = nullptr;
  unsigned long x = strtoul(w.c_str(), &end, 16);
  if (!end || *end) return false;
  v = (unsigned)x;
  return true;
}

int hexdigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string lower(std::string s) {
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
  return s;
}

}  // namespace

std::string romwbw_sha256_hex(const uint8_t* data, size_t len) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= len; i += 64) sha256_block(h, data + i);
  uint8_t tail[128];
  size_t rest = len - i;
  memcpy(tail, data + i, rest);
  tail[rest++] = 0x80;
  size_t total = rest <= 56 ? 64 : 128;
  memset(tail + rest, 0, total - rest);
  uint64_t bits = (uint64_t)len * 8;
  for (int k = 0; k < 8; k++) tail[total - 1 - (size_t)k] = (uint8_t)(bits >> (8 * k));
  sha256_block(h, tail);
  if (total == 128) sha256_block(h, tail + 64);
  char out[65];
  for (int k = 0; k < 8; k++) snprintf(out + 8 * k, 9, "%08x", h[k]);
  return out;
}

// ---------------------------------------------------------------------------
// The target
// ---------------------------------------------------------------------------

RomwbwTarget::RomwbwTarget(qkz80& cpu, banked_mem& mem, volatile bool* stop)
    : stub(nullptr), tracker(nullptr), cpu_(cpu), mem_(mem), stop_(stop), view_bank_(-1),
      break_bank_(-1), wait_armed_(false), wait_is_sha_(false), wait_len_(0) {}

uint32_t RomwbwTarget::read_reg(int index) {
  switch (index) {
    case gdb::REG_AF:  return cpu_.get_reg16(qkz80::regp_AF);
    case gdb::REG_BC:  return cpu_.regs.BC.get_pair16();
    case gdb::REG_DE:  return cpu_.regs.DE.get_pair16();
    case gdb::REG_HL:  return cpu_.regs.HL.get_pair16();
    case gdb::REG_SP:  return cpu_.regs.SP.get_pair16();
    case gdb::REG_PC:  return cpu_.regs.PC.get_pair16();
    case gdb::REG_IX:  return cpu_.regs.IX.get_pair16();
    case gdb::REG_IY:  return cpu_.regs.IY.get_pair16();
    case gdb::REG_AF2: return cpu_.regs.AF_.get_pair16();
    case gdb::REG_BC2: return cpu_.regs.BC_.get_pair16();
    case gdb::REG_DE2: return cpu_.regs.DE_.get_pair16();
    case gdb::REG_HL2: return cpu_.regs.HL_.get_pair16();
    case gdb::REG_IR:  return (uint32_t)((cpu_.regs.I << 8) | cpu_.regs.R);
  }
  return 0;
}

void RomwbwTarget::write_reg(int index, uint32_t v) {
  qkz80_uint16 w = (qkz80_uint16)(v & 0xFFFF);
  switch (index) {
    case gdb::REG_AF:  cpu_.set_reg16(w, qkz80::regp_AF); break;
    case gdb::REG_BC:  cpu_.regs.BC.set_pair16(w); break;
    case gdb::REG_DE:  cpu_.regs.DE.set_pair16(w); break;
    case gdb::REG_HL:  cpu_.regs.HL.set_pair16(w); break;
    case gdb::REG_SP:  cpu_.regs.SP.set_pair16(w); break;
    case gdb::REG_PC:  cpu_.regs.PC.set_pair16(w); break;
    case gdb::REG_IX:  cpu_.regs.IX.set_pair16(w); break;
    case gdb::REG_IY:  cpu_.regs.IY.set_pair16(w); break;
    case gdb::REG_AF2: cpu_.regs.AF_.set_pair16(w); break;
    case gdb::REG_BC2: cpu_.regs.BC_.set_pair16(w); break;
    case gdb::REG_DE2: cpu_.regs.DE_.set_pair16(w); break;
    case gdb::REG_HL2: cpu_.regs.HL_.set_pair16(w); break;
    case gdb::REG_IR:
      cpu_.regs.I = (qkz80_uint8)(w >> 8);
      cpu_.regs.R = (qkz80_uint8)(w & 0xFF);
      break;
  }
}

// 8000H and up is always the common bank, whatever the view is.  Below that,
// the view is the bank that "monitor bank" chose, or else the bank the CPU has
// now.  peek_mem() reads what the CPU would read and sets no trace bit.
uint8_t RomwbwTarget::read_mem(uint16_t addr) {
  if (view_bank_ < 0 || addr >= banked_mem::BANK_BOUNDARY) return mem_.peek_mem(addr);
  return mem_.read_bank((uint8_t)view_bank_, addr);
}

void RomwbwTarget::write_mem(uint16_t addr, uint8_t v) {
  if (view_bank_ < 0 || addr >= banked_mem::BANK_BOUNDARY) {
    mem_.store_mem(addr, v);
  } else {
    mem_.write_bank((uint8_t)view_bank_, addr, v);   // a ROM bank ignores it
  }
}

uint8_t RomwbwTarget::read_code(uint16_t addr) { return mem_.peek_mem(addr); }

void RomwbwTarget::check_signals() {
  if (stop_ && *stop_) throw GdbRunEnd{false};
}

bool RomwbwTarget::program_matches() {
  if (!wait_armed_) return false;
  if (mem_.get_current_bank() != PROGRAM_BANK) return false;
  std::vector<uint8_t> got(wait_is_sha_ ? wait_len_ : wait_bytes_.size());
  for (size_t i = 0; i < got.size(); i++)
    got[i] = mem_.peek_mem((uint16_t)(PROGRAM_ADDR + i));
  if (wait_is_sha_) return romwbw_sha256_hex(got.data(), got.size()) == wait_sha_;
  return got == wait_bytes_;
}

bool RomwbwTarget::break_applies(uint16_t pc, bool rule) {
  if (rule) {
    if (!program_matches()) return false;
    // One shot.  The program is found, and its breakpoints belong to its bank.
    wait_armed_ = false;
    if (stub) stub->set_rule_break(PROGRAM_ADDR, false);
    break_bank_ = PROGRAM_BANK;
    cmd_bt_reset();
    fprintf(stderr, "[gdb: the program that wait-program named has started in bank %02X]\n",
            PROGRAM_BANK);
    return true;
  }
  if (break_bank_ < 0 || pc >= banked_mem::BANK_BOUNDARY) return true;
  return mem_.get_current_bank() == (uint8_t)break_bank_;
}

static std::string bank_text(int b) {
  char t[16];
  if (b < 0) return "the current bank";
  snprintf(t, sizeof t, "%02X", b);
  return t;
}

std::string RomwbwTarget::cmd_bank(const std::string& arg) {
  std::string a = lower(trim(arg));
  char t[96];
  if (a.empty()) {
    snprintf(t, sizeof t, "CPU bank %02X, memory view %s, 8000H and up is bank 8F\n",
             mem_.get_current_bank(), bank_text(view_bank_).c_str());
    return t;
  }
  if (a == "cur" || a == "current") {
    view_bank_ = -1;
    return "memory view: the bank that the CPU has\n";
  }
  unsigned b;
  if (!parse_hex_number(a, b) || !valid_bank(b))
    return "bank: give 00 to 0F (ROM), 80 to 8F (RAM) or cur\n";
  view_bank_ = (int)b;
  snprintf(t, sizeof t, "memory view: bank %02X below 8000H, bank 8F above\n", b);
  return t;
}

std::string RomwbwTarget::cmd_break_bank(const std::string& arg) {
  std::string a = lower(trim(arg));
  char t[96];
  if (a.empty()) {
    snprintf(t, sizeof t, "breakpoints below 8000H apply in %s\n",
             break_bank_ < 0 ? "every bank" : ("bank " + bank_text(break_bank_)).c_str());
    return t;
  }
  if (a == "off" || a == "any") {
    break_bank_ = -1;
    return "breakpoints below 8000H apply in every bank\n";
  }
  unsigned b;
  if (!parse_hex_number(a, b) || !valid_bank(b))
    return "break-bank: give 00 to 0F, 80 to 8F or off\n";
  break_bank_ = (int)b;
  snprintf(t, sizeof t, "breakpoints below 8000H apply in bank %02X only\n", b);
  return t;
}

// wait-program bytes HEX...           the first bytes of the .COM file
// wait-program sha256 LENGTH HEX64    the length and the SHA-256 of the program
// wait-program off
// wait-program                        say what is armed
std::string RomwbwTarget::cmd_wait_program(const std::string& arg) {
  std::string rest = trim(arg);
  std::string kind = lower(take_word(rest));
  char t[160];
  if (kind.empty()) {
    if (!wait_armed_) return "wait-program: not armed\n";
    if (wait_is_sha_)
      snprintf(t, sizeof t, "wait-program: armed, %zu bytes with SHA-256 %s\n", wait_len_,
               wait_sha_.c_str());
    else
      snprintf(t, sizeof t, "wait-program: armed, %zu bytes\n", wait_bytes_.size());
    return t;
  }
  if (kind == "off") {
    wait_armed_ = false;
    if (stub) stub->set_rule_break(PROGRAM_ADDR, false);
    return "wait-program: off\n";
  }
  if (kind == "bytes") {
    std::string hex;
    while (!rest.empty()) hex += take_word(rest);
    if (hex.empty() || hex.size() % 2 != 0 || hex.size() > 2 * 4096)
      return "wait-program bytes: give 1 to 4096 bytes as hex digits\n";
    std::vector<uint8_t> v;
    for (size_t i = 0; i < hex.size(); i += 2) {
      int hi = hexdigit(hex[i]), lo = hexdigit(hex[i + 1]);
      if (hi < 0 || lo < 0) return "wait-program bytes: not a hex digit\n";
      v.push_back((uint8_t)((hi << 4) | lo));
    }
    wait_bytes_ = v;
    wait_is_sha_ = false;
  } else if (kind == "sha256") {
    std::string len = take_word(rest);
    std::string sha = lower(take_word(rest));
    char* end = nullptr;
    unsigned long n = strtoul(len.c_str(), &end, 10);
    bool ok = !len.empty() && end && !*end && n >= 1 && n <= 0xFE00 - PROGRAM_ADDR && sha.size() == 64;
    for (size_t i = 0; ok && i < sha.size(); i++) ok = hexdigit(sha[i]) >= 0;
    if (!ok)
      return "wait-program sha256: give the length in decimal (1 to 65024) and 64 hex digits\n";
    wait_len_ = n;
    wait_sha_ = sha;
    wait_is_sha_ = true;
  } else {
    return "wait-program: bytes HEX..., sha256 LENGTH HEX64, or off\n";
  }
  wait_armed_ = true;
  if (stub) stub->set_rule_break(PROGRAM_ADDR, true);
  snprintf(t, sizeof t, "wait-program: armed; the next start at %04XH in bank %02X that matches stops\n",
           PROGRAM_ADDR, PROGRAM_BANK);
  return t;
}

// The calls of the boot and of the CCP are not the program's, and the boot
// leaves the shadow stack out of step (the ROM changes banks by RET).  Start
// the stack again with one frame: the word at SP, which is where a RET goes.
std::string RomwbwTarget::cmd_bt_reset() {
  if (!tracker) return "bt-reset: no shadow stack\n";
  uint16_t sp = cpu_.regs.SP.get_pair16();
  uint16_t ret = (uint16_t)(mem_.peek_mem(sp) | (mem_.peek_mem((uint16_t)(sp + 1)) << 8));
  tracker->reset();
  tracker->seed(ret, sp);
  char t[80];
  snprintf(t, sizeof t, "shadow stack: restarted with the return address %04X from SP %04X\n",
           ret, sp);
  return t;
}

std::string RomwbwTarget::cmd_status() {
  char t[200];
  snprintf(t, sizeof t, "pc %04X, CPU bank %02X\n",
           (unsigned)cpu_.regs.PC.get_pair16(), mem_.get_current_bank());
  return std::string(t) + cmd_bank("") + cmd_break_bank("") + cmd_wait_program("");
}

bool RomwbwTarget::monitor_command(const std::string& cmd, std::string& reply) {
  std::string rest = trim(cmd);
  std::string word = lower(take_word(rest));
  if (word == "bank") reply = cmd_bank(rest);
  else if (word == "break-bank") reply = cmd_break_bank(rest);
  else if (word == "wait-program") reply = cmd_wait_program(rest);
  else if (word == "bt-reset") reply = cmd_bt_reset();
  else if (word == "status") reply = cmd_status();
  else
    reply = "monitor commands: bt, bank [XX|cur], break-bank [XX|off], "
            "wait-program [bytes HEX...|sha256 LENGTH HEX64|off], bt-reset, status\n";
  return true;
}
