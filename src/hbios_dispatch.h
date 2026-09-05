/*
 * HBIOS Dispatch - Shared RomWBW HBIOS Handler
 *
 * This module provides HBIOS function handling that can be shared between
 * different platform implementations (CLI, WebAssembly, iOS).
 *
 * All I/O operations go through emu_io.h for platform independence.
 *
 * Function codes derived from RomWBW Source/HBIOS/hbios.inc
 */

#ifndef HBIOS_DISPATCH_H
#define HBIOS_DISPATCH_H

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <functional>

//=============================================================================
// HBIOS Function Codes (from RomWBW hbios.inc)
//=============================================================================

// HBIOS error/result codes (from hbios.inc ERR_* values)
enum HBiosResult {
  HBR_SUCCESS   = 0,     // ERR_NONE: Success
  HBR_UNDEF     = -1,    // ERR_UNDEF: Undefined error
  HBR_NOTIMPL   = -2,    // ERR_NOTIMPL: Function not implemented
  HBR_NOFUNC    = -3,    // ERR_NOFUNC: Invalid function
  HBR_NOUNIT    = -4,    // ERR_NOUNIT: Invalid unit number
  HBR_NOMEM     = -5,    // ERR_NOMEM: Out of memory
  HBR_RANGE     = -6,    // ERR_RANGE: Parameter out of range
  HBR_NOMEDIA   = -7,    // ERR_NOMEDIA: Media not present
  HBR_NOHW      = -8,    // ERR_NOHW: Hardware not present
  HBR_IO        = -9,    // ERR_IO: I/O error
  HBR_READONLY  = -10,   // ERR_READONLY: Write to read-only media
  HBR_TIMEOUT   = -11,   // ERR_TIMEOUT: Device timeout
  HBR_BADCFG    = -12,   // ERR_BADCFG: Invalid configuration
  HBR_INTERNAL  = -13,   // ERR_INTERNAL: Internal error
  // Legacy compatibility
  HBR_FAILED    = 0xFF,  // Generic failure (unsigned)
};

// HBIOS function codes (passed in B register)
// Derived from RomWBW hbios.inc BF_* definitions
enum HBiosFunc {
  // Character I/O (CIO) - 0x00-0x06
  HBF_CIO       = 0x00,
  HBF_CIOIN     = 0x00,  // Character input
  HBF_CIOOUT    = 0x01,  // Character output
  HBF_CIOIST    = 0x02,  // Character input status
  HBF_CIOOST    = 0x03,  // Character output status
  HBF_CIOINIT   = 0x04,  // Init/reset device/line config
  HBF_CIOQUERY  = 0x05,  // Report device/line config
  HBF_CIODEVICE = 0x06,  // Report device info

  // Disk I/O (DIO) - 0x10-0x1B
  HBF_DIO       = 0x10,
  HBF_DIOSTATUS = 0x10,  // Disk status
  HBF_DIORESET  = 0x11,  // Disk reset
  HBF_DIOSEEK   = 0x12,  // Disk seek
  HBF_DIOREAD   = 0x13,  // Disk read sectors
  HBF_DIOWRITE  = 0x14,  // Disk write sectors
  HBF_DIOVERIFY = 0x15,  // Disk verify sectors
  HBF_DIOFORMAT = 0x16,  // Disk format track
  HBF_DIODEVICE = 0x17,  // Disk device info report
  HBF_DIOMEDIA  = 0x18,  // Disk media report
  HBF_DIODEFMED = 0x19,  // Define disk media
  HBF_DIOCAP    = 0x1A,  // Disk capacity report
  HBF_DIOGEOM   = 0x1B,  // Disk geometry report

  // RTC (Real-Time Clock) - 0x20-0x28
  HBF_RTC       = 0x20,
  HBF_RTCGETTIM = 0x20,  // Get time
  HBF_RTCSETTIM = 0x21,  // Set time
  HBF_RTCGETBYT = 0x22,  // Get NVRAM byte by index
  HBF_RTCSETBYT = 0x23,  // Set NVRAM byte by index
  HBF_RTCGETBLK = 0x24,  // Get NVRAM data block
  HBF_RTCSETBLK = 0x25,  // Set NVRAM data block
  HBF_RTCGETALM = 0x26,  // Get alarm
  HBF_RTCSETALM = 0x27,  // Set alarm
  HBF_RTCDEVICE = 0x28,  // RTC device info report

  // DSKY (Display/Keypad) - 0x30-0x3A
  HBF_DSKY       = 0x30,
  HBF_DSKYRESET  = 0x30,  // Reset DSKY hardware
  HBF_DSKYSTAT   = 0x31,  // Get keypad status
  HBF_DSKYGETKEY = 0x32,  // Get key from keypad
  HBF_DSKYSHOWHEX= 0x33,  // Display binary value in hex
  HBF_DSKYSHOWSEG= 0x34,  // Display encoded segment string
  HBF_DSKYKEYLEDS= 0x35,  // Set/clear keypad LEDs
  HBF_DSKYSTATLED= 0x36,  // Set/clear status LEDs
  HBF_DSKYBEEP   = 0x37,  // Beep onboard DSKY speaker
  HBF_DSKYDEVICE = 0x38,  // DSKY device info report
  HBF_DSKYMESSAGE= 0x39,  // DSKY message handling
  HBF_DSKYEVENT  = 0x3A,  // DSKY event handling

  // Video Display Adapter (VDA) - 0x40-0x4F
  HBF_VDA       = 0x40,
  HBF_VDAINI    = 0x40,  // Initialize VDU
  HBF_VDAQRY    = 0x41,  // Query VDU status
  HBF_VDARES    = 0x42,  // Soft reset VDU
  HBF_VDADEV    = 0x43,  // Device info
  HBF_VDASCS    = 0x44,  // Set cursor style
  HBF_VDASCP    = 0x45,  // Set cursor position
  HBF_VDASAT    = 0x46,  // Set character attribute
  HBF_VDASCO    = 0x47,  // Set character color
  HBF_VDAWRC    = 0x48,  // Write character
  HBF_VDAFIL    = 0x49,  // Fill
  HBF_VDACPY    = 0x4A,  // Copy
  HBF_VDASCR    = 0x4B,  // Scroll
  HBF_VDAKST    = 0x4C,  // Get keyboard status
  HBF_VDAKFL    = 0x4D,  // Flush keyboard buffer
  HBF_VDAKRD    = 0x4E,  // Read keyboard
  HBF_VDARDC    = 0x4F,  // Read character

  // Sound (SND) - 0x50-0x58
  HBF_SND       = 0x50,
  HBF_SNDRESET  = 0x50,  // Reset sound system
  HBF_SNDVOL    = 0x51,  // Request sound volume
  HBF_SNDPRD    = 0x52,  // Request sound period
  HBF_SNDNOTE   = 0x53,  // Request note
  HBF_SNDPLAY   = 0x54,  // Initiate sound command
  HBF_SNDQUERY  = 0x55,  // Query sound capabilities
  HBF_SNDDUR    = 0x56,  // Request duration
  HBF_SNDDEVICE = 0x57,  // Sound device info request
  HBF_SNDBEEP   = 0x58,  // Play beep sound

  // Extension Functions - 0xE0-0xEF
  HBF_EXT       = 0xE0,
  HBF_EXTSLICE  = 0xE0,  // Slice calculation

  // Host File Transfer - 0xE1-0xE8 (EMU custom extension)
  // DE points at a NUL-terminated path in guest memory.  There is no length in
  // the call, so the dispatcher has to stop somewhere: HOST_PATH_MAX below.
  // Running off the end is a FAILED open, not a shorter path - see there.
  HBF_HOST_OPEN_R = 0xE1,  // Open host file for reading (DE=path addr)
  HBF_HOST_OPEN_W = 0xE2,  // Open host file for writing (DE=path addr)
  HBF_HOST_READ   = 0xE3,  // Read byte from host (returns E=byte, A=status)
  HBF_HOST_WRITE  = 0xE4,  // Write byte to host (E=byte)
  HBF_HOST_CLOSE  = 0xE5,  // Close host file (C=0 for read, C=1 for write)
  HBF_HOST_MODE   = 0xE6,  // Get/set mode (C=0 get, C=1 set; E=mode)
  HBF_HOST_GETARG = 0xE7,  // Get cmd arg by index (C=index, DE=buf addr)
  // Where the open write file will actually land, as text for the guest to
  // print.  C = size of the buffer at DE including the terminator, DE = buffer
  // address.  A = 0 and the buffer holds a NUL-terminated string; A = 0xFF and
  // the buffer is untouched when no write file is open or the backend cannot
  // say.  An emulator built before this existed answers 0xFF from the unknown-
  // function path, which is why W8 treats a failure as "print what was typed"
  // rather than as an error - a new W8.COM has to keep running on an already
  // released front end.
  HBF_HOST_GETNAME = 0xE8, // Get effective host write path (C=bufsize, DE=buf)
  // What this host-file implementation guarantees.  No inputs, no state, safe
  // to call at any time - which is the point: it is the ONLY way a guest
  // program can tell a new emulator from an old one BEFORE it does anything.
  // HBF_HOST_GETNAME cannot serve as the probe, because "no such function" and
  // "no write file open" are both 0xFF, so it can only be asked after a file is
  // already open - by which time the damage a probe exists to prevent may
  // already be arranged.
  // Returns A = 0 and E = HOST_CAP_* bits.  An emulator that predates this
  // answers A = 0xFF from the unknown-function path, which is the answer W8
  // acts on.
  HBF_HOST_CAPS = 0xE9,    // Get host-file capability bits (E = HOST_CAP_*)
  // The read twin of HBF_HOST_GETNAME, with the same C/DE calling convention
  // and the same "A = 0xFF and the buffer is untouched" answer.  R8 printed the
  // path the CCP shouted, which is not the file that was opened: the CLI
  // retries a failed open case-insensitively and reports the result as an
  // absolute path, and a browser or sandboxed port opens whatever the user
  // picked, which need not resemble the string the guest sent at all.
  //
  // One consequence smaller than the write side - a read creates nothing the
  // user then has to go and find - which is why it came later.
  HBF_HOST_GETRNAME = 0xEA, // Get effective host read path (C=bufsize, DE=buf)

  // System Functions - 0xF0-0xFC
  HBF_SYS       = 0xF0,
  HBF_SYSRESET  = 0xF0,  // Soft reset HBIOS
  HBF_SYSVER    = 0xF1,  // Get HBIOS version
  HBF_SYSSETBNK = 0xF2,  // Set current bank
  HBF_SYSGETBNK = 0xF3,  // Get current bank
  HBF_SYSSETCPY = 0xF4,  // Bank memory copy setup
  HBF_SYSBNKCPY = 0xF5,  // Bank memory copy
  HBF_SYSALLOC  = 0xF6,  // Alloc HBIOS heap memory
  HBF_SYSFREE   = 0xF7,  // Free HBIOS heap memory
  HBF_SYSGET    = 0xF8,  // Get HBIOS info
  HBF_SYSSET    = 0xF9,  // Set HBIOS parameters
  HBF_SYSPEEK   = 0xFA,  // Get byte from alt bank
  HBF_SYSPOKE   = 0xFB,  // Set byte in alt bank
  HBF_SYSINT    = 0xFC,  // Manage interrupt vectors

  // EMU custom extension (avoid conflict with standard codes)
  HBF_SYSBOOT   = 0xFE,  // EMU: Boot from device
};

// How far HBF_HOST_OPEN_R and HBF_HOST_OPEN_W will scan guest memory for the
// NUL that ends the path they were handed.  The guest passes an address and no
// length, so some limit is forced; what matters is what happens at it.  It used
// to be "use the first 256 bytes and say nothing", which is the one answer
// nobody can want: on a read that opens some other file, and on a write it
// CREATES one, at a path the guest never asked for and cannot see afterwards -
// HBF_HOST_GETNAME would report the truncation as if it were the destination.
// Reaching the limit is now a failed open (A = 0xFF), which every caller
// already handles, because a guest that cannot pass a path it can terminate
// has not asked for anything the emulator can honour.
//
// Nothing shipped can reach it: R8 and W8 both build the path in a 128-byte
// buffer and NUL-terminate inside it.  This is for the guest program that is
// not one of those two.
static constexpr int HOST_PATH_MAX = 256;

// HBF_HOST_CAPS bits are defined in emu_io.h (EMU_HOST_CAP_SAFE_PATHS), because
// the value is supplied by the backend function emu_host_path_caps() declared
// there, not by the core. The dispatcher just forwards it to the guest in E.
// See the note above that declaration for why it is a backend function: a
// front end that has not confined guest paths must fail to link rather than
// silently claim it has.

// SYSRESET subtypes (C register for SYSRESET)
enum HBiosSysResetType {
  SYSRES_INT    = 0x00,  // Reset HBIOS internal
  SYSRES_WARM   = 0x01,  // Warm start (restart boot loader)
  SYSRES_COLD   = 0x02,  // Cold start
  SYSRES_USER   = 0x03,  // User reset request
};

// SYSGET subfunctions (C register for SYSGET)
enum HBiosSysGetFunc {
  SYSGET_CIOCNT   = 0x00,  // Get char unit count
  SYSGET_CIOFN    = 0x01,  // Get CIO unit fn/data adr
  SYSGET_DIOCNT   = 0x10,  // Get disk unit count
  SYSGET_DIOFN    = 0x11,  // Get DIO unit fn/data adr
  SYSGET_RTCCNT   = 0x20,  // Get RTC unit count
  SYSGET_DSKYCNT  = 0x30,  // Get DSKY unit count
  SYSGET_VDACNT   = 0x40,  // Get VDA unit count
  SYSGET_VDAFN    = 0x41,  // Get VDA unit fn/data adr
  SYSGET_SNDCNT   = 0x50,  // Get SND unit count
  SYSGET_SNDFN    = 0x51,  // Get SND unit fn/data adr
  SYSGET_SWITCH   = 0xC0,  // Get non-volatile switch value
  SYSGET_TIMER    = 0xD0,  // Get current timer value
  SYSGET_SECS     = 0xD1,  // Get current seconds value
  SYSGET_BOOTINFO = 0xE0,  // Get boot information
  SYSGET_CPUINFO  = 0xF0,  // Get CPU information
  SYSGET_MEMINFO  = 0xF1,  // Get memory capacity info
  SYSGET_BNKINFO  = 0xF2,  // Get bank assignment info
  SYSGET_CPUSPD   = 0xF3,  // Get clock speed & wait states
  SYSGET_PANEL    = 0xF4,  // Get front panel switches val
  SYSGET_APPBNKS  = 0xF5,  // Get app bank information
  // EMU custom extension
  SYSGET_DEVLIST  = 0xFD,  // EMU: List available devices
};

// SNDQUERY subfunctions (E register for BF_SNDQUERY).
// From RomWBW Source/HBIOS/hbios.inc: BF_SNDQ_STATUS is the base and the rest
// are offsets from it. Only CHCNT and DEV have callers in the 3.6.0 tree - the
// ROM device inventory asks for both - but the whole set is named so a future
// subfunction is added rather than silently answered wrong.
enum HBiosSndQueryFunc {
  SNDQ_STATUS = 0,  // Device status
  SNDQ_CHCNT  = 1,  // Count of channels: B := tone, C := noise
  SNDQ_VOLUME = 2,  // 8 bit number
  SNDQ_PERIOD = 3,  // 16 bit number
  SNDQ_DEV    = 4,  // Device type code in B, I/O ports in DE and HL
};

// SYSSET subfunctions (C register for SYSSET)
enum HBiosSysSetFunc {
  SYSSET_SWITCH   = 0xC0,  // Set non-volatile switch value
  SYSSET_TIMER    = 0xD0,  // Set timer value
  SYSSET_SECS     = 0xD1,  // Set seconds value
  SYSSET_BOOTINFO = 0xE0,  // Set boot information
  SYSSET_CPUSPD   = 0xF3,  // Set clock speed & wait states
  SYSSET_PANEL    = 0xF4,  // Set front panel LEDs
};

// NVRAM switch numbers (D register for SYSGET_SWITCH/SYSSET_SWITCH)
enum HBiosNVSwitchNum {
  NVSW_STATUS     = 0xFF,  // Get NVRAM status ('W' if initialized)
  NVSW_BOOTOPTS   = 1,     // Boot options: L=slice/app char, H=flags+unit
  NVSW_AUTOBOOT   = 3,     // Autoboot: L=flags+timeout
};

// NVRAM boot option flags (H register bits for NVSW_BOOTOPTS)
enum HBiosBootOptFlags {
  BOPTS_ROM       = 0x80,  // Bit 7: 1=ROM app, 0=disk
  BOPTS_UNIT      = 0x7F,  // Bits 0-6: disk unit number (when BOPTS_ROM=0)
};

// NVRAM autoboot flags (L register bits for NVSW_AUTOBOOT)
enum HBiosAutoBootFlags {
  ABOOT_AUTO      = 0x20,  // Bit 5: 1=auto boot enabled
  ABOOT_TIMEOUT   = 0x0F,  // Bits 0-3: timeout in seconds (0=immediate)
};

// Media ID values
enum HBiosMediaId {
  MID_NONE   = 0,
  MID_MDROM  = 1,
  MID_MDRAM  = 2,
  MID_RF     = 3,
  MID_HD     = 4,
  MID_FD720  = 5,
  MID_FD144  = 6,
  MID_FD360  = 7,
  MID_FD120  = 8,
  MID_FD111  = 9,
  MID_HDNEW  = 10,
};

//=============================================================================
// Emulator I/O State (for state machine operation)
//=============================================================================

enum HBIOSState {
  HBIOS_RUNNING = 0,      // Emulator is running normally
  HBIOS_NEEDS_INPUT,      // Waiting for character input (CIOIN)
  HBIOS_HALTED,           // Emulator halted (HLT instruction or error)
};

//=============================================================================
// Memory Disk (MD) State
//=============================================================================

struct MemDiskState {
  uint32_t current_lba = 0;     // Current LBA position
  uint8_t start_bank = 0;       // Starting bank number
  uint8_t num_banks = 0;        // Number of banks
  bool is_rom = false;          // True if ROM disk (read-only)
  bool is_enabled = false;      // True if this MD unit exists

  // Calculate total sectors (512 bytes per sector, 64 sectors per 32KB bank)
  uint32_t total_sectors() const {
    return (uint32_t)num_banks * 64;
  }
};

//=============================================================================
// Disk Structure
//=============================================================================

struct HBDisk {
  bool is_open = false;
  std::string path;
  std::vector<uint8_t> data;  // For in-memory disks
  void* handle = nullptr;     // For file-backed disks (emu_disk_handle)
  bool file_backed = false;
  size_t size = 0;
  uint32_t current_lba = 0;   // Current LBA position (set by DIOSEEK)

  // Total sectors in this image, in the 32-bit sector count HBIOS deals in
  // (DIOCAP hands it back split across DE and HL). Named to match
  // MemDiskState::total_sectors above so the two kinds of disk read alike at
  // the call sites.
  //
  // size is a size_t, wider than that interface on a 64-bit host, so the
  // narrowing happens here once and on purpose rather than implicitly at each
  // use. It clamps rather than wrapping: an image beyond 2 TiB cannot be
  // described in 32 bits at all, and reporting the largest capacity that can
  // be addressed is far better than reporting the remainder, which is what a
  // silent truncation would do - a 4 TiB image would otherwise report as
  // empty. The comparison is written against a size_t limit so it stays
  // meaningful on a 32-bit host, where size_t is no wider than the interface.
  uint32_t total_sectors() const {
    const size_t sectors = size / 512;
    const size_t limit = static_cast<size_t>(UINT32_MAX);
    return static_cast<uint32_t>(sectors < limit ? sectors : limit);
  }
  int max_slices = 8;         // Slices for drive letter assignment (not a limit on access)

  // Partition/slice info (detected from MBR on first EXTSLICE call)
  bool partition_probed = false;     // True if MBR has been parsed
  uint32_t partition_base_lba = 0;   // Start of RomWBW partition (2048 for hd1k, 0 for hd512)
  uint32_t slice_size = 16640;       // Sectors per slice (16384 for hd1k, 16640 for hd512)
  bool is_hd1k = false;              // True for hd1k format (MID_HDNEW=10), false for hd512 (MID_HD=4)

  // Manifest disk flag - set true for disks managed by app manifest (can be auto-updated)
  // UI should warn user before writing to manifest disks since changes may be lost
  bool is_manifest = false;

  // Warning suppression - set true if user checked "Don't warn about overwrites" in disk selector
  bool warning_suppressed = false;

  // Dirty flag - set true when disk data has been modified (for persistence)
  bool dirty = false;
};

//=============================================================================
// ROM Application Structure (for boot menu)
//=============================================================================

struct HBRomApp {
  std::string name;      // Display name
  std::string sys_path;  // Path to .sys file
  char key = 0;          // Key to press (e.g., 'B' for BASIC)
  bool is_loaded = false;
};

//=============================================================================
// HBIOS Dispatch Class
//=============================================================================

// Forward declarations for memory/CPU interfaces
class qkz80;
class banked_mem;

// Debug log function pointer type - set to enable debug logging
// When non-null, called with printf-style arguments for debug output
// Platforms provide their own implementation (emu_log, NSLog wrapper, etc.)
typedef void (*DebugLogFn)(const char* fmt, ...);

class HBIOSDispatch {
public:
  HBIOSDispatch();
  ~HBIOSDispatch();

  // Initialize/reset state
  void reset();

  // Clear input waiting state (used by SYSRESET to avoid stale state after reboot)
  void clearWaitingState();

  // Set CPU and memory references (must be called before use)
  void setCPU(qkz80* cpu) { this->cpu = cpu; }
  void setMemory(banked_mem* mem) { this->memory = mem; }

  // Debug output - set function pointer to enable, nullptr to disable
  // Example: hbios.setDebugLog(emu_log);  // use emu_log for debug output
  void setDebugLog(DebugLogFn fn) { debug_log = fn; }
  DebugLogFn getDebugLog() const { return debug_log; }

  // Legacy interface - calls setDebugLog with emu_log or nullptr
  void setDebug(bool enable);
  bool getDebug() const { return debug_log != nullptr; }
  bool getBootInProgress() const { return boot_in_progress; }

  // Get pointer to RAM bank initialization bitmap (for sharing with port I/O path)
  uint16_t* getInitializedBanksBitmap() { return &initialized_ram_banks; }

  // Disk management
  bool loadDisk(int unit, const uint8_t* data, size_t size);
  bool loadDiskFromFile(int unit, const std::string& path);
  void closeDisk(int unit);
  void closeAllDisks();  // Close all disks (call before reconfiguring)
  void flushAllDisks();  // Flush all disk writes to storage
  bool isDiskLoaded(int unit) const;
  const HBDisk& getDisk(int unit) const;
  void setDiskSliceCount(int unit, int slices);  // Set slices for drive letter assignment

  // Manifest disk write warning - for UI to warn about ephemeral changes
  // Call setDiskIsManifest(unit, true) when loading disks from app manifest
  // Call setDiskWarningSuppressed(unit, true) if user checked "Don't warn" checkbox
  // Poll pollManifestWriteWarning() to check if UI should show warning dialog
  void setDiskIsManifest(int unit, bool is_manifest);
  void setDiskWarningSuppressed(int unit, bool suppressed);
  bool pollManifestWriteWarning();  // Returns true once per session on first manifest disk write

  // Disk persistence - for saving modified disks
  bool isDiskDirty(int unit) const;           // Check if disk has been modified
  void clearDiskDirty(int unit);              // Clear dirty flag after saving
  const uint8_t* getDiskData(int unit) const; // Get pointer to disk data (in-memory only)
  size_t getDiskDataSize(int unit) const;     // Get size of disk data

  // Periodic disk flush - call from main loop or timer (every frame/tick is fine)
  // If any disk writes occurred and 20+ seconds have passed since last flush,
  // flushes all disks and returns true. Otherwise returns false.
  // Safe to call frequently - only flushes when both conditions are met.
  bool checkPeriodicFlush();

  // Memory disk initialization (call after ROM is loaded)
  void initMemoryDisks();

  // Populate disk unit table in HCB (call after disks are loaded)
  // This updates the HCB at 0x160 (HCB+0x60) with disk device info
  // so romldr and other tools can discover available disks
  void populateDiskUnitTable();

  // ROM application management
  void addRomApp(const std::string& name, const std::string& path, char key);
  void clearRomApps();

  // NVRAM boot option configuration - string-based API
  //
  // Set boot option from printable string:
  //   "C"   - Boot ROM app C (CP/M 2.2)
  //   "Z"   - Boot ROM app Z (ZSDOS)
  //   "2"   - Boot from disk unit 2, slice 0
  //   "2.3" - Boot from disk unit 2, slice 3
  //   "H"   - Show boot menu (help)
  //   ""    - Clear boot option (uninitialized, shows menu)
  void setNvramSetting(const std::string& setting);

  // Get current boot option as printable string
  // Returns same format as setNvramSetting accepts
  // Clears the dirty flag (call hasNvramChange first if you need to check)
  std::string getNvramSetting();

  // Check if NVRAM has been modified and needs to be persisted
  // Returns true once per change, cleared when getNvramSetting is called
  bool hasNvramChange();

  // Check if NVRAM is initialized (has a valid boot option set)
  bool isNvramInitialized() const { return nvram_switches[0] == 'W'; }

  // Host file transfer (EMU extension)
  void setHostCmdLine(const std::string& cmdline) { host_cmd_line = cmdline; }

  // Signal port handler (port 0xEE)
  // Supports two protocols:
  // 1. Simple status: 0x01=starting, 0xFE=preinit, 0xFF=init complete
  // 2. Address registration: state machine for per-handler dispatch addresses
  void handleSignalPort(uint8_t value);

  // Get handler type from function code in B register
  // Returns: 0=CIO, 1=DIO, 2=RTC, 3=SYS, 4=VDA, 5=SND, -1=unknown
  static int getTrapTypeFromFunc(uint8_t func);

  // Handle HBIOS call - dispatches based on function code in B register
  bool handleMainEntry();

  // Handle PRTSUM - print device summary (called by boot loader 'D' command)
  void handlePRTSUM();

  // Handle HBIOS dispatch triggered by OUT to port 0xEF
  // This is the unified entry point for all platforms (CLI, web, iOS, Mac)
  // Sets skip_ret=true since Z80 proxy has its own RET instruction
  void handlePortDispatch();

  // Check if trapping is enabled
  bool isTrappingEnabled() const { return trapping_enabled; }
  void setTrappingEnabled(bool enable) { trapping_enabled = enable; }

  // Check if waiting for console input (CIOIN/VDAKRD called with no data) or
  // for the browser file picker (HBF_HOST_READ while HOST_FILE_WAITING_READ).
  // Defined in the .cc because it consults emu_host_file_get_state().
  bool isWaitingForInput() const;
  void clearWaitingForInput() { waiting_for_input = false; }

  // Console idle detection for power management.
  // Returns true when guest is polling console status with no input available
  // (consecutive CIOIST/VDAKST calls returning "no key").
  // Host run loop should sleep longer when idle (e.g., 10ms instead of 0.1ms).
  bool isConsoleIdle() const { return idle_poll_count >= IDLE_POLL_THRESHOLD; }

  //==========================================================================
  // State Machine I/O Interface
  // The emulator is a pure state machine. Instead of calling external functions,
  // it buffers I/O and signals what it needs. The outer loop handles actual I/O.
  //==========================================================================

  // Get current emulator state
  HBIOSState getState() const { return emu_state; }

  // Character Output: Get buffered output chars (clears buffer after call)
  // Caller should display these characters
  std::vector<uint8_t> getOutputChars();

  // Character Output: Check if there are pending output chars
  bool hasOutputChars() const { return !output_buffer.empty(); }

  // Character Output: Queue a single output char (for direct UART output)
  void queueOutputChar(uint8_t ch) { output_buffer.push_back(ch); }

  // Character Input: Provide a character (in response to NEEDS_INPUT state)
  void provideInputChar(int ch);

  // Character Input: Queue multiple characters
  void queueInputChars(const uint8_t* data, size_t len);
  void queueInputChar(int ch);

  // Character Input: Check if input is available
  bool hasInputChar() const { return !input_buffer.empty(); }

  // Character Input: Read and consume one char (returns -1 if empty)
  int readInputChar();

  // Character Input: Clear input buffer
  void clearInputBuffer();

  // Set whether blocking I/O is allowed (false for web/WASM)
  void setBlockingAllowed(bool allowed) { blocking_allowed = allowed; }
  bool isBlockingAllowed() const { return blocking_allowed; }

  // Control whether handlers do a synthetic RET
  // Set to true for I/O port dispatch (Z80 proxy has its own RET)
  // Set to false for PC-based trapping (we need to do the RET)
  void setSkipRet(bool skip) { skip_ret = skip; }
  bool getSkipRet() const { return skip_ret; }

  // Set reset callback for SYSRESET function
  // The callback should perform: switch to ROM bank 0, clear input, set PC to 0
  using ResetCallback = std::function<void(uint8_t reset_type)>;
  void setResetCallback(ResetCallback cb) { reset_callback = cb; }

  // Main entry point address (default 0xFFF0)
  void setMainEntry(uint16_t addr) { main_entry = addr; }
  uint16_t getMainEntry() const { return main_entry; }

  // Individual function handlers
  void handleCIO();   // Character I/O
  void handleDIO();   // Disk I/O
  void handleRTC();   // Real-time clock
  void handleSYS();   // System functions
  void handleVDA();   // Video display
  void handleSND();   // Sound
  void handleDSKY();  // Display/Keypad
  void handleEXT();   // Extension functions (slice calc)

private:
  // CPU and memory references (not owned)
  qkz80* cpu = nullptr;
  banked_mem* memory = nullptr;
  DebugLogFn debug_log = nullptr;  // Debug function pointer (null = disabled)

  // State machine
  HBIOSState emu_state = HBIOS_RUNNING;
  std::vector<uint8_t> output_buffer;  // Buffered output chars (for CIOOUT)
  std::vector<int> input_buffer;       // Buffered input chars (for CIOIN)

  // Dispatch control
  bool trapping_enabled = false;
  bool waiting_for_input = false;  // Set when CIOIN/VDAKRD needs input
  bool waiting_for_host_file = false;  // Set when HBF_HOST_READ awaits the file picker
  bool skip_ret = false;           // Skip synthetic RET (for I/O port dispatch)
  bool blocking_allowed = true;    // Can we block for I/O? (false for web/WASM)
  uint16_t main_entry = 0xFFF0;    // Main HBIOS entry point

  // Console idle detection: counts consecutive CIOIST/VDAKST polls returning
  // "no input". Reset when input is consumed, output is written, or disk I/O
  // occurs. When count >= threshold, isConsoleIdle() returns true.
  int idle_poll_count = 0;
  static constexpr int IDLE_POLL_THRESHOLD = 8;

  // Manifest disk write warning - set true on first write to manifest disk (non-suppressed)
  // Cleared after pollManifestWriteWarning() returns true
  bool manifest_write_pending = false;
  static bool manifest_warning_shown;  // Static: survives object recreation within session

  // Periodic disk flush tracking (20-second interval)
  bool disk_writes_pending = false;     // Set true on any disk write
  time_t last_periodic_flush = 0;       // Time of last periodic flush
  static constexpr int PERIODIC_FLUSH_INTERVAL = 20;  // Seconds between flushes

  // Signal port state machine
  uint8_t signal_state = 0;
  uint16_t signal_addr = 0;

  // Bank for PEEK/POKE
  uint8_t cur_bank = 0;

  // Bank copy state (SYSSETCPY/SYSBNKCPY)
  uint8_t bnkcpy_src_bank = 0x8E;
  uint8_t bnkcpy_dst_bank = 0x8E;
  uint16_t bnkcpy_count = 0;

  // HBIOS heap state (SYSALLOC)
  // Heap is in bank 0x80 starting after HCB (0x0200) up to 0x8000
  uint16_t heap_ptr = 0x0200;
  static constexpr uint16_t heap_end = 0x8000;

  // Bitmap tracking which RAM banks (0x80-0x8F) have been initialized
  // Exposed via getter so port I/O and SYSSETBNK paths share one bitmap
  uint16_t initialized_ram_banks = 0;

private:

  // VDA state
  int vda_rows = 25;
  int vda_cols = 80;
  int vda_cursor_row = 0;
  int vda_cursor_col = 0;
  uint8_t vda_attr = 0x07;

  // Sound state
  uint8_t snd_volume[4] = {0};
  uint16_t snd_period[4] = {0};
  uint16_t snd_duration = 100;

  // Host file transfer state (EMU extension 0xE1-0xE7)
  // File handles are now managed by emu_io abstraction
  uint8_t host_transfer_mode = 0;   // 0=auto, 1=text, 2=binary
  std::string host_cmd_line;        // Original command line for GETARG

  // Reset callback for SYSRESET
  ResetCallback reset_callback = nullptr;

  // Boot info (saved during SYSBOOT, returned by SYSGET_BOOTINFO)
  int saved_boot_unit = 0;
  int saved_boot_slice = 0;
  bool boot_in_progress = false;  // Set when boot starts, for debugging

  // NVRAM switches for boot configuration (emulates RTC NVRAM)
  // This emulates the RTC NVRAM layout used by RomWBW for boot configuration.
  // The ROM's SYSCONF utility (W command) reads/writes this via RTCGETBYT/RTCSETBYT.
  // The boot loader reads this via SYSGET_SWITCH.
  //
  // NVRAM Layout (5 bytes):
  // [0] = Header signature 'W' (0x57)
  // [1] = Boot char (L) - app char for ROM boot, slice number for disk boot
  // [2] = Boot options (H) - BOPTS_ROM (0x80) | unit number (0-127)
  // [3] = Autoboot flags - ABOOT_AUTO (0x20) | timeout (0x0F)
  // [4] = Checksum - XOR of bytes 0-3 XOR with version bytes
  //
  // See RomWBW Source/Doc/SystemGuide.md for full documentation.
  static constexpr int NVRAM_SIZE = 5;
  uint8_t nvram_switches[NVRAM_SIZE] = {0, 'H', BOPTS_ROM, 0, 0};
  bool nvram_dirty = false;  // Set when NVRAM modified, cleared by getNvramSetting()

  // Helper to recalculate NVRAM checksum (byte 4) and set dirty flag
  void recalcNvramChecksum();

  // Disks
  HBDisk disks[16];

  // Memory disks (MD0=RAM, MD1=ROM)
  MemDiskState md_disks[2];

  // ROM applications
  std::vector<HBRomApp> rom_apps;

  // Helper: set result code and Z flag for HBIOS return
  void setResult(uint8_t result);

  // Helper: perform RET instruction (pop PC from stack)
  void doRet();

  // Helper: write string to console
  void writeConsoleString(const char* str);

  // Helper: find ROM app by key
  int findRomApp(char key) const;

  // Helper: fetch a NUL-terminated guest string for the host-file calls.
  // False when no terminator appears within HOST_PATH_MAX bytes - see the note
  // on that constant.
  bool fetchGuestString(uint16_t addr, std::string* out) const;

  // Helper: deliver a host path into a guest buffer of `bufsize` bytes
  // (terminator included), shared by HBF_HOST_GETNAME and HBF_HOST_GETRNAME.
  // False - and the buffer untouched - when there is nothing to report or no
  // room for a character and a NUL.
  bool storeHostName(const char* name, uint8_t bufsize, uint16_t buf_addr);

  // Helper: boot from disk or ROM app
  bool bootFromDevice(const char* cmd_str);
};

#endif // HBIOS_DISPATCH_H
