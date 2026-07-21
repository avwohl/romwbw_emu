/*
 * emu_config.h - Optional JSON settings file for the CLI emulator
 *
 * Lets a machine description live in a file instead of a long command line
 * (idea imported from the z80cpmw Windows port's z80cpmw.json). The file
 * holds only what describes the machine; per-run debug switches (--trace,
 * --load, --start, --sense, --mask-interrupt, --nmi) stay CLI-only.
 *
 * Discovery (see emu_config_discover and main()):
 *   1. --config=FILE          explicit file; missing/malformed is a hard error
 *   2. ./romwbw_emu.json      current directory
 *   3. <config dir>/config.json   ($XDG_CONFIG_HOME or ~/.config)/romwbw_emu
 * --no-config skips 2 and 3. CLI flags always override file values.
 *
 * Only emu_config.cc includes the vendored nlohmann/json header - keep it
 * out of this interface so no other translation unit pays for it.
 */

#ifndef EMU_CONFIG_H
#define EMU_CONFIG_H

#include <string>
#include <vector>

struct EmuConfig {
  std::string rom;        // "rom"      - like --romwbw=FILE
  std::string boot;       // "boot"     - like --boot=CMD (overrides NVRAM every run; omit to let NVRAM win)
  std::string escape;     // "escape"   - like --escape (^E syntax or literal char)
  std::string symbols;    // "symbols"  - like --symbols=FILE
  std::string romldr;     // "romldr"   - like --romldr=FILE
  bool debug = false;     // "debug"    - like --debug
  bool strictIo = false;  // "strictIo" - like --strict-io
  std::string disks[16];  // "disks"    - array, index = unit; null/absent = none

  struct RomApp {         // "romapps"  - like --romapp=K=Name:path
    char key = 0;
    std::string name;
    std::string path;
  };
  std::vector<RomApp> romapps;
};

// Load cfg from a JSON file into out (only keys present in the file are
// applied). Returns false with a human-readable parser/validation message in
// err. A "version" newer than supported is refused.
bool emu_config_load(const std::string& path, EmuConfig& out, std::string& err);

// Return the first existing config file per the discovery order (steps 2-3),
// or "" if none. config_dir is the per-user config directory (XDG-aware).
std::string emu_config_discover(const std::string& config_dir);

// Serialize cfg to path (atomic: .tmp + rename). Returns false with a
// message in err on failure.
bool emu_config_save(const std::string& path, const EmuConfig& cfg, std::string& err);

#endif // EMU_CONFIG_H
