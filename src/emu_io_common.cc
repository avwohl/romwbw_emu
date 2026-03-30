/*
 * Emulator I/O - Common Implementation
 *
 * Functions that are identical across CLI and WASM platforms.
 * Platform-specific code remains in emu_io_cli.cc / emu_io_wasm.cc.
 */

#include "emu_io.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>

//=============================================================================
// File I/O Implementation
//=============================================================================

bool emu_file_load(const std::string& path, std::vector<uint8_t>& data) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    data.clear();
    return false;
  }

  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);

  data.resize(size);
  size_t read = fread(data.data(), 1, size, f);
  fclose(f);

  if (read != size) {
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

  size_t to_read = file_size;
  if (offset + to_read > mem_size) {
    to_read = mem_size - offset;
  }

  size_t read = fread(mem + offset, 1, to_read, f);
  fclose(f);
  return read;
}

bool emu_file_save(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;

  size_t written = fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  return written == data.size();
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

emu_disk_handle emu_disk_open(const std::string& path, const char* mode) {
  const char* fmode;
  if (strcmp(mode, "r") == 0) {
    fmode = "rb";
  } else if (strcmp(mode, "rw") == 0) {
    fmode = "r+b";
  } else if (strcmp(mode, "rw+") == 0) {
    // Try to open existing, create if not exists
    fmode = "r+b";
    FILE* f = fopen(path.c_str(), fmode);
    if (!f) {
      f = fopen(path.c_str(), "w+b");
    }
    if (!f) return nullptr;

    disk_file* disk = new disk_file;
    disk->fp = f;
    fseek(f, 0, SEEK_END);
    disk->size = ftell(f);
    open_disk_handles.insert(disk);
    return disk;
  } else {
    return nullptr;
  }

  FILE* f = fopen(path.c_str(), fmode);
  if (!f) return nullptr;

  disk_file* disk = new disk_file;
  disk->fp = f;
  fseek(f, 0, SEEK_END);
  disk->size = ftell(f);
  open_disk_handles.insert(disk);
  return disk;
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

  fseek(disk->fp, offset, SEEK_SET);
  return fread(buffer, 1, count, disk->fp);
}

size_t emu_disk_write(emu_disk_handle handle, size_t offset,
                      const uint8_t* buffer, size_t count) {
  if (!handle) return 0;
  disk_file* disk = static_cast<disk_file*>(handle);
  if (!disk->fp) return 0;

  fseek(disk->fp, offset, SEEK_SET);
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
