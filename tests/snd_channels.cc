/*
 * snd_channels.cc - BF_SNDPLAY sounds every channel the guest set up
 *
 * WHAT THIS IS FOR.  Two things, and the second is why the first was wrong for
 * so long.
 *
 * 1. BF_SNDPLAY used to look at channel 0 and call emu_dsky_beep(duration) - a
 *    fixed beep of that length - so a guest playing a four-voice tune got one
 *    beep, or on most front ends nothing.  It now renders every channel a guest
 *    plays, through the opt-in hook in emu_io.h, and falls back to exactly the
 *    old beep on a front end that has not installed a renderer.
 *
 * 2. THE REGISTER AND SCALE CONVENTIONS WERE WRONG, and the arrays being
 *    write-only is why nobody noticed.  From Source/Doc/SystemGuide.md:
 *
 *      - C is the Sound UNIT on every function in the group, never a channel.
 *        This dispatcher indexed its per-channel arrays with it.
 *      - BF_SNDPLAY takes the channel in **D**.  Its worked example is
 *        "HBIOS B=54 C=00 D=01 ; Play note on Channel 1".
 *      - Volume and pitch are ONE PENDING PAIR for the unit, preset by
 *        BF_SNDVOL / BF_SNDPRD / BF_SNDNOTE and applied to the channel named in
 *        D when BF_SNDPLAY is called.
 *      - BF_SNDNOTE takes the note in **HL**, not L: the guide's own table runs
 *        to 340 for B7, which does not fit in a byte.
 *      - The scale is eighth tones, 48 to an octave, AND ITS ZERO IS A#0/Bb0.
 *        The table gives C4 = 152 and A4 = 188.  The old code assumed A4 sat at
 *        step 9 of octave 4 under a plain note/48 split, which put A4 at 246Hz.
 *
 * So the anchor is freq = 440 * 2^((note - 188) / 48), and this file pins every
 * one of those against the published table rather than against itself.
 *
 * Run: make -C src test_snd_channels && ./src/test_snd_channels
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

struct Tone {
  int channel;
  int freq_hz;
  int volume;
  int duration_ms;
};

static std::vector<Tone> g_tones;   // what a renderer was handed
static int g_beeps = 0;             // how often the old fallback fired
static int g_beep_ms = 0;

static void recording_tone_handler(int channel, int freq_hz, int volume,
                                   int duration_ms) {
  Tone t;
  t.channel = channel;
  t.freq_hz = freq_hz;
  t.volume = volume;
  t.duration_ms = duration_ms;
  g_tones.push_back(t);
}

//=============================================================================
// The rest of emu_io: enough to link, never exercised here
//=============================================================================

void emu_dsky_beep(int duration_ms) { g_beeps++; g_beep_ms = duration_ms; }

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
    hbios.setCPU(&cpu);
    hbios.setMemory(&mem);
    hbios.setBlockingAllowed(false);
  }

  // One HBIOS call, entered exactly as the Z80 proxy enters it.  HL carries
  // the value for the three setters - hbios.inc is explicit that BF_SNDVOL
  // reads L, BF_SNDPRD reads HL and BF_SNDNOTE reads L - and DE carries the
  // duration for BF_SNDDUR.
  void call(uint8_t func, uint8_t channel = 0, uint16_t hl = 0) {
    cpu.regs.BC.set_high(func);
    cpu.regs.BC.set_low(channel);
    cpu.regs.HL.set_pair16(hl);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
  }

  // C is the unit on every one of these; it stays 0, there being one unit.
  void vol(uint8_t v)      { call(HBF_SNDVOL, 0, v); }
  void note(uint16_t n)    { call(HBF_SNDNOTE, 0, n); }
  void dur(uint16_t ms)    { call(HBF_SNDDUR, 0, ms); }
  void reset()             { call(HBF_SNDRESET, 0, 0); }

  // ...except play, which takes the CHANNEL in D.
  uint8_t play(uint8_t channel) {
    cpu.regs.BC.set_high(HBF_SNDPLAY);
    cpu.regs.BC.set_low(0);
    cpu.regs.DE.set_high(channel);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
    return cpu.regs.AF.get_high();
  }

  // SNDQUERY takes its subfunction in E.
  uint8_t query(uint8_t subfunc) {
    cpu.regs.BC.set_high(HBF_SNDQUERY);
    cpu.regs.BC.set_low(0);
    cpu.regs.DE.set_low(subfunc);
    cpu.regs.PC.set_pair16(AFTER);
    hbios.handlePortDispatch();
    return cpu.regs.AF.get_high();
  }
};

//=============================================================================
// Tests
//=============================================================================

// Straight out of SystemGuide.md's note table.
static const uint16_t NOTE_A0s = 0;    // the scale's zero
static const uint16_t NOTE_C4  = 152;
static const uint16_t NOTE_A4  = 188;
static const uint16_t NOTE_A3  = 140;
static const uint16_t NOTE_A5  = 236;
static const uint16_t NOTE_C7  = 296;  // does not fit in a byte

int main() {
  printf("BF_SNDPLAY renders the channel in D from the unit's pending sound\n");
  printf("------------------------------------------------------------\n");

  // --- the documented sequence, on four channels ---------------------------
  {
    Rig r;
    g_tones.clear();
    g_beeps = 0;
    emu_snd_set_tone_handler(recording_tone_handler);

    r.dur(250);
    // "HBIOS B=51 C=00 L=80 / B=53 C=00 HL=152 / B=54 C=00 D=01"
    const uint16_t notes[4] = {NOTE_A4, NOTE_A3, NOTE_C4, NOTE_A5};
    const uint8_t vols[4] = {200, 150, 100, 50};
    for (uint8_t ch = 0; ch < 4; ch++) {
      r.vol(vols[ch]);
      r.note(notes[ch]);
      r.play(ch);
    }

    check(g_tones.size() == 4, "four plays produce four tones");
    check(g_beeps == 0, "and the duration-only fallback does not fire with a renderer installed");

    bool channels_right = g_tones.size() == 4;
    for (size_t i = 0; i < g_tones.size(); i++) {
      if (g_tones[i].channel != (int)i) channels_right = false;
    }
    check(channels_right, "each tone names the channel from D, not the unit from C");

    bool vols_right = g_tones.size() == 4;
    for (size_t i = 0; i < g_tones.size(); i++) {
      if (g_tones[i].volume != vols[i]) vols_right = false;
    }
    check(vols_right, "each carries the volume pending when it was played");

    bool dur_right = g_tones.size() == 4;
    for (size_t i = 0; i < g_tones.size(); i++) {
      if (g_tones[i].duration_ms != 250) dur_right = false;
    }
    check(dur_right, "and the duration BF_SNDDURATION asked for");
  }

  // --- the scale, against the published table ------------------------------
  {
    struct { uint16_t note; int hz; const char* name; } cases[] = {
      { NOTE_A4,  440, "A4 = note 188 -> 440Hz" },
      { NOTE_A3,  220, "A3 = note 140 -> 220Hz, an octave below" },
      { NOTE_A5,  880, "A5 = note 236 -> 880Hz, an octave above" },
      { NOTE_C4,  262, "C4 = note 152 -> 262Hz (the guide's own example)" },
      { NOTE_A0s,  29, "note 0 is A#0, the scale's zero, at 29Hz" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Rig r;
      g_tones.clear();
      emu_snd_set_tone_handler(recording_tone_handler);
      r.dur(10);
      r.vol(255);
      r.note(cases[i].note);
      r.play(0);
      int got = g_tones.empty() ? -1 : g_tones[0].freq_hz;
      // Within 1%: the period is stored as whole microseconds.
      bool okc = got > 0 && abs(got - cases[i].hz) * 100 <= cases[i].hz;
      check(okc, cases[i].name);
      if (!okc) printf("      got %d Hz, wanted about %d\n", got, cases[i].hz);
    }
  }

  // --- the note is 16 bit --------------------------------------------------
  {
    Rig r;
    g_tones.clear();
    emu_snd_set_tone_handler(recording_tone_handler);
    r.dur(10);
    r.vol(255);
    r.note(NOTE_C7);          // 296: high byte 1, low byte 40
    r.play(0);
    // C7 is C4 three octaves up: 261.6 * 8 = 2093Hz.  Read as its low byte
    // (296 & 0xFF = 40) it would come out near 65Hz instead, which is the
    // whole point of the check.
    int got = g_tones.empty() ? -1 : g_tones[0].freq_hz;
    bool okc = got > 2050 && got < 2140;
    check(okc, "note 296 (C7) is read as 16 bits, not as its low byte");
    if (!okc) printf("      got %d Hz, wanted about 2093\n", got);
  }

  // --- a channel out of range is refused -----------------------------------
  {
    Rig r;
    g_tones.clear();
    emu_snd_set_tone_handler(recording_tone_handler);
    r.vol(255);
    r.note(NOTE_A4);
    uint8_t a = r.play(9);
    check(a != 0 && g_tones.empty(), "playing on channel 9 is refused, not folded into channel 1");
  }

  // --- volume 0 is an instruction, not a no-op -----------------------------
  {
    Rig r;
    g_tones.clear();
    emu_snd_set_tone_handler(recording_tone_handler);
    r.vol(200); r.note(NOTE_A4); r.play(0);
    r.vol(0);   r.play(0);
    check(g_tones.size() == 2 && g_tones[1].volume == 0,
          "a play at volume 0 reaches the front end, which is how a guest stops a channel");
  }

  // --- SNDRESET silences what is sounding ----------------------------------
  {
    Rig r;
    g_tones.clear();
    emu_snd_set_tone_handler(recording_tone_handler);
    r.vol(200); r.note(NOTE_A4); r.play(0); r.play(1);
    size_t before = g_tones.size();
    r.reset();
    size_t silenced = 0;
    for (size_t i = before; i < g_tones.size(); i++) {
      if (g_tones[i].volume == 0) silenced++;
    }
    check(silenced == 4,
          "BF_SNDRESET silences all four channels rather than only forgetting them");
  }

  // --- what a guest can read back ------------------------------------------
  {
    Rig r;
    emu_snd_set_tone_handler(nullptr);
    r.vol(123);
    r.note(NOTE_A4);
    check(r.query(SNDQ_VOLUME) == 0 && r.cpu.regs.HL.get_low() == 123,
          "SNDQ_VOLUME reads back the unit's pending volume in L");
    uint8_t a = r.query(SNDQ_PERIOD);
    check(a == 0 && r.cpu.regs.HL.get_pair16() > 2200 && r.cpu.regs.HL.get_pair16() < 2350,
          "SNDQ_PERIOD reads back the pending period in HL (A4 is about 2272us)");
    r.query(SNDQ_CHCNT);
    check(r.cpu.regs.BC.get_high() == 4 && r.cpu.regs.BC.get_low() == 0,
          "SNDQ_CHCNT still answers 4 tone channels and 0 noise");
    r.query(SNDQ_DEV);
    check(r.cpu.regs.BC.get_high() == 0x02,
          "SNDQ_DEV answers SNDDEV_BITMODE, not SN76489 - invntdev indexes a table with B and never reads A");
  }

  // --- with NO renderer, exactly the old behaviour -------------------------
  {
    Rig r;
    g_tones.clear();
    g_beeps = 0;
    g_beep_ms = 0;
    emu_snd_set_tone_handler(nullptr);
    r.dur(250);
    r.vol(200); r.note(NOTE_A4);
    r.play(0);
    r.play(1);
    r.play(2);
    check(g_tones.empty(), "with no renderer installed nothing is rendered");
    check(g_beeps == 1, "the duration-only beep fires for channel 0 alone, not once per play");
    check(g_beep_ms == 250, "and for the duration the guest asked for");
  }

  printf("------------------------------------------------------------\n");
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all checks passed (0 failures)\n");
  return 0;
}
