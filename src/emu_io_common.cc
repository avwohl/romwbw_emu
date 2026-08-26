/*
 * Emulator I/O - Common Implementation
 *
 * Functions that are identical across CLI and WASM platforms.
 * Platform-specific code remains in emu_io_cli.cc / emu_io_wasm.cc.
 */

#include "emu_io.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <string>

// For emu_rename below. NOMINMAX and WIN32_LEAN_AND_MEAN because this header
// otherwise defines min/max as macros and drags in half of COM, either of
// which can break code compiled after it - and this file is compiled by four
// ports with four different sets of surrounding headers.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

//=============================================================================
// File I/O Implementation
//=============================================================================

// See emu_io.h: plain rename() will not replace an existing target on Windows.
// The MSVC and mingw CRTs both fail it rather than replacing, which made the
// safe write path in emu_file_save() the broken one there.
int emu_rename(const char* from, const char* to) {
#ifdef _WIN32
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
  return rename(from, to);
#endif
}

// Upper bound for whole-file loads. Every caller loads a ROM, a ROM app
// image, or a disk image, and the biggest disk RomWBW can address is 256
// hd1k slices plus the 1MB prefix (~2GB). Rejecting anything larger keeps a
// user-supplied path from asking for an allocation that cannot succeed, and
// keeps the cast to size_t safe on 32-bit targets (wasm32).
static constexpr uint64_t EMU_MAX_LOAD_SIZE = 2147483648ULL;  // 2 GiB

// Measure an open stream in 64 bits, reporting failure instead of a bogus
// size. Plain `size_t size = ftell(f)` turns ftell's -1 into SIZE_MAX, and
// ftell does fail on real inputs: a path naming a pipe or socket (for
// example /dev/stdin when stdin is a pipe, or a document handed over by a
// mobile file picker) opens fine but is not seekable. The old code then fed
// SIZE_MAX to vector::resize, which throws length_error and, with no handler
// anywhere in the emulator, terminates the process.
static bool measure_stream(FILE* f, uint64_t* out_size) {
  if (emu_fseek(f, 0, SEEK_END) != 0) return false;
  emu_off_t end = emu_ftell(f);
  if (end < 0) return false;
  if (emu_fseek(f, 0, SEEK_SET) != 0) return false;
  *out_size = (uint64_t)end;
  return true;
}

bool emu_file_load(const std::string& path, std::vector<uint8_t>& data) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    data.clear();
    return false;
  }

  uint64_t size = 0;
  if (!measure_stream(f, &size) || size > EMU_MAX_LOAD_SIZE ||
      size > (uint64_t)SIZE_MAX) {
    fclose(f);
    data.clear();
    return false;
  }

  data.resize((size_t)size);
  size_t read = size > 0 ? fread(data.data(), 1, (size_t)size, f) : 0;
  fclose(f);

  if (read != (size_t)size) {
    data.clear();
    return false;
  }
  return true;
}

size_t emu_file_load_to_mem(const std::string& path, uint8_t* mem,
                            size_t mem_size, size_t offset) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return 0;

  // measure_stream rather than the bare fseek/ftell pair this used to have:
  // ftell fails on a non-seekable path (a fifo, /dev/stdin on a pipe, a
  // document handed over by a mobile file picker) and its -1 became a huge
  // size_t. The clamp below hid it - the read was capped at the buffer - so
  // this one was survivable where emu_file_load's was not, but it is the same
  // defect and the same fix, and it is the last site in this file still using
  // the 32-bit pair.
  uint64_t file_size = 0;
  if (!measure_stream(f, &file_size)) {
    fclose(f);
    return 0;
  }

  // Guard before subtracting: an offset past mem_size would underflow.
  if (offset >= mem_size) {
    fclose(f);
    return 0;
  }
  size_t avail = mem_size - offset;
  size_t to_read = (file_size < (uint64_t)avail) ? (size_t)file_size : avail;

  size_t read = fread(mem + offset, 1, to_read, f);
  fclose(f);
  return read;
}

bool emu_file_save(const std::string& path, const std::vector<uint8_t>& data) {
  // Write a temp file and rename it over the target. Callers use this to
  // persist a dirty in-memory disk image, and fopen("wb") truncates its
  // target before the first byte is written: a disk-full error or a kill
  // mid-write destroyed the previous image and left nothing in its place.
  // With the rename the old image survives any failure. fclose is checked
  // because stdio buffering can surface a write error only at the final
  // flush. (Imported from the z80cpmw migration audit, commit 79ddfc4.)
  std::string temp_path = path + ".tmp";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) return false;

  size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
  bool ok = (written == data.size());
  if (fclose(f) != 0) ok = false;

  if (!ok || emu_rename(temp_path.c_str(), path.c_str()) != 0) {
    remove(temp_path.c_str());
    return false;
  }
  return true;
}

//=============================================================================
// Disk Image I/O Implementation
//=============================================================================

struct disk_file {
  FILE* fp;
  size_t size;
};

// Track all open disk handles for emu_disk_flush_all()
static std::set<disk_file*> open_disk_handles;

// Wrap an opened image in a disk_file, sizing it with the checked 64-bit
// measurement. The recorded size drives format auto-detection and slice
// math, so an unmeasurable stream must fail the open rather than be handed
// on as SIZE_MAX.
static emu_disk_handle make_disk_handle(FILE* f) {
  uint64_t size = 0;
  if (!measure_stream(f, &size) || size > (uint64_t)SIZE_MAX) {
    fclose(f);
    return nullptr;
  }
  disk_file* disk = new disk_file;
  disk->fp = f;
  disk->size = (size_t)size;
  open_disk_handles.insert(disk);
  return disk;
}

emu_disk_handle emu_disk_open(const std::string& path, const char* mode) {
  const char* fmode;
  if (strcmp(mode, "r") == 0) {
    fmode = "rb";
  } else if (strcmp(mode, "rw") == 0) {
    fmode = "r+b";
  } else if (strcmp(mode, "rw+") == 0) {
    // Try to open existing, create if not exists
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) {
      f = fopen(path.c_str(), "w+b");
    }
    if (!f) return nullptr;
    return make_disk_handle(f);
  } else {
    return nullptr;
  }

  FILE* f = fopen(path.c_str(), fmode);
  if (!f) return nullptr;
  return make_disk_handle(f);
}

void emu_disk_close(emu_disk_handle handle) {
  if (!handle) return;
  disk_file* disk = static_cast<disk_file*>(handle);
  open_disk_handles.erase(disk);
  if (disk->fp) fclose(disk->fp);
  delete disk;
}

size_t emu_disk_read(emu_disk_handle handle, size_t offset,
                     uint8_t* buffer, size_t count) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (!disk->fp) return 0;

  // Seek in 64 bits and check it: an unchecked seek leaves the stream at its
  // previous position, so a failure would read some other part of the image
  // and report it as the requested block. DIOREAD treats a 0 return as
  // end-of-media (partial count), which is the safe reading.
  if (emu_fseek(disk->fp, (emu_off_t)offset, SEEK_SET) != 0) return 0;
  return fread(buffer, 1, count, disk->fp);
}

size_t emu_disk_write(emu_disk_handle handle, size_t offset,
                      const uint8_t* buffer, size_t count) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (!disk->fp) return 0;

  // Same 64-bit checked seek as the read path. Here a stale position would
  // be worse than a bad read: it would overwrite unrelated sectors (the MBR
  // of a combo image, say) while reporting the write as successful.
  // DIOWRITE surfaces the 0 return as HBR_IO.
  if (emu_fseek(disk->fp, (emu_off_t)offset, SEEK_SET) != 0) return 0;
  size_t written = fwrite(buffer, 1, count, disk->fp);

  // Update size if we wrote past the end
  size_t new_end = offset + written;
  if (new_end > disk->size) {
    disk->size = new_end;
  }

  return written;
}

void emu_disk_flush(emu_disk_handle handle) {
  if (!handle) return;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (disk->fp) fflush(disk->fp);
}

void emu_disk_flush_all() {
  for (disk_file* disk : open_disk_handles) {
    if (disk && disk->fp) {
      fflush(disk->fp);
    }
  }
}

size_t emu_disk_size(emu_disk_handle handle) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  return disk->size;
}

//=============================================================================
// Time Implementation
//=============================================================================

void emu_get_time(emu_time* t) {
  time_t now = time(nullptr);
  struct tm* tm = localtime(&now);

  t->year = tm->tm_year + 1900;
  t->month = tm->tm_mon + 1;
  t->day = tm->tm_mday;
  t->hour = tm->tm_hour;
  t->minute = tm->tm_min;
  t->second = tm->tm_sec;
  t->weekday = tm->tm_wday;
}

//=============================================================================
// Host File Path Helpers
//=============================================================================

// Back a cut position off any UTF-8 continuation byte, so a truncated name is
// still a valid byte sequence. Guest command lines are 8-bit and pass through
// the CCP untouched, so a name arriving here can genuinely be UTF-8 typed on a
// modern host; cutting mid-sequence produces a name that displays as a
// replacement character in every file manager that will accept it at all.
static size_t back_off_utf8(const std::string& s, size_t cut) {
  while (cut > 0 && (unsigned char)s[cut] >= 0x80 && (unsigned char)s[cut] < 0xC0) {
    cut--;
  }
  return cut;
}

// The same boundary from the other side: move a cut position FORWARD off a
// continuation byte, for a cut that keeps the TAIL. Backing up is right only
// when the cut keeps the head, where it shortens the answer; on a tail-keeping
// cut it LENGTHENS it, and a cap that can return more than the cap is not one.
// That is what it did: a mostly-suffix name whose 255-byte boundary landed
// inside a two-byte sequence came back 256 bytes long.
static size_t advance_off_utf8(const std::string& s, size_t cut) {
  while (cut < s.size() && (unsigned char)s[cut] >= 0x80 && (unsigned char)s[cut] < 0xC0) {
    cut++;
  }
  return cut;
}

// Cap one path component at EMU_HOST_NAME_MAX bytes, keeping the extension.
// See the note on EMU_HOST_NAME_MAX in emu_io.h for why the extension is the
// half worth keeping.
static std::string emu_host_path_cap_name(const std::string& base) {
  if (base.size() <= EMU_HOST_NAME_MAX) return base;

  // The extension is the last dot and what follows, and only if there is a
  // stem in front of it: ".bashrc" is a name, not an extension, and keeping
  // "" + ".bashrc" out of a 5000-character name would throw the whole name
  // away.
  size_t dot = base.rfind('.');
  std::string ext;
  if (dot != std::string::npos && dot > 0) ext = base.substr(dot);

  // No room for a stem beside it - a name that is nearly all "extension" -
  // so there is no extension worth preserving. Keep the END, which is where
  // whatever structure the name has runs out, and is the same choice
  // HBF_HOST_GETNAME makes when it has to cut a path.
  if (ext.size() >= EMU_HOST_NAME_MAX) {
    size_t start = advance_off_utf8(base, base.size() - EMU_HOST_NAME_MAX);
    // The whole window was continuation bytes, so there is no character
    // boundary to cut on: the tail is not valid UTF-8 whatever is done to it.
    // Take the last EMU_HOST_NAME_MAX bytes rather than the empty string,
    // which is what skipping to the end would produce - a name a caller then
    // has to invent, from a function whose whole job is to supply one.
    if (start >= base.size()) start = base.size() - EMU_HOST_NAME_MAX;
    return base.substr(start);
  }

  size_t keep = back_off_utf8(base, EMU_HOST_NAME_MAX - ext.size());
  return base.substr(0, keep) + ext;
}

// Reduce a guest-supplied path to its last component, for a backend with no
// filesystem to honour the directory part with. See emu_io.h for the contract.
//
// This lives here rather than in each backend because every sandboxed front
// end needs the same answer and they were arriving at different ones: the
// browser backend split on '/' only, so a Windows-shaped path typed into the
// web build became the whole string as one download name, and the iOS backend
// did not split at all. A path is not an error - W8 has to accept one, since
// the same disk image and the same W8.COM run on the CLI where it means
// something - it just cannot mean there what it means on a desktop.
std::string emu_host_path_basename(const std::string& path,
                                   const char* fallback) {
  const std::string fb = (fallback && *fallback) ? fallback : "download.bin";

  // Ignore trailing separators: "a/b/" names b, not "".
  size_t end = path.size();
  while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
  if (end == 0) return fb;

  size_t start = end;
  while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') start--;
  std::string base = path.substr(start, end - start);

  // A Windows path can name a file on another drive's current directory with
  // no separator at all ("C:OUT.TXT"), and the drive letter is not part of the
  // name. Strip exactly that prefix - a letter and a colon at the start - and
  // nothing else: a colon is a legal character in a POSIX filename, so cutting
  // at the last one turned "/tmp/my:file.txt" into "file.txt" in the browser
  // while the CLI wrote "my:file.txt", which is the divergence this helper was
  // written to prevent.
  if (base.size() >= 2 && base[1] == ':' &&
      ((base[0] >= 'A' && base[0] <= 'Z') || (base[0] >= 'a' && base[0] <= 'z'))) {
    base = base.substr(2);
  }

  // What is left has to be a name that cannot escape the directory it will be
  // joined to. "." and ".." are the two that can.
  if (base.empty() || base == "." || base == "..") return fb;

  return emu_host_path_cap_name(base);
}
