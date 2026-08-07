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

//=============================================================================
// File I/O Implementation
//=============================================================================

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

  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  // Guard before subtracting: offset past mem_size would underflow, and a
  // failed ftell makes file_size (size_t)-1, so clamp to the space available.
  if (offset >= mem_size) {
    fclose(f);
    return 0;
  }
  size_t avail = mem_size - offset;
  size_t to_read = (file_size < avail) ? file_size : avail;

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

  if (!ok || rename(temp_path.c_str(), path.c_str()) != 0) {
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
