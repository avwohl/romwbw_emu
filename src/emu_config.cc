/*
 * emu_config.cc - JSON settings file implementation
 *
 * The only translation unit that includes the vendored nlohmann/json header
 * (24k lines - keep it contained). See emu_config.h for the schema.
 */

#include "emu_config.h"
#include "include/nlohmann/json.hpp"
#include <cstdio>
#include <sys/stat.h>

using nlohmann::json;

bool emu_config_load(const std::string& path, EmuConfig& out, std::string& err) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    err = "cannot open file";
    return false;
  }
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    text.append(buf, n);
  }
  fclose(f);

  json j;
  try {
    j = json::parse(text);
  } catch (const json::parse_error& e) {
    err = e.what();
    return false;
  }
  if (!j.is_object()) {
    err = "top level must be a JSON object";
    return false;
  }

  try {
    int version = j.value("version", 1);
    if (version > 1) {
      err = "config version " + std::to_string(version) +
            " is newer than supported version 1";
      return false;
    }

    out.rom = j.value("rom", out.rom);
    out.boot = j.value("boot", out.boot);
    out.escape = j.value("escape", out.escape);
    out.symbols = j.value("symbols", out.symbols);
    out.romldr = j.value("romldr", out.romldr);
    out.debug = j.value("debug", out.debug);
    out.strictIo = j.value("strictIo", out.strictIo);

    if (j.contains("disks")) {
      const json& d = j.at("disks");
      if (!d.is_array()) {
        err = "\"disks\" must be an array (index = disk unit)";
        return false;
      }
      if (d.size() > 16) {
        err = "\"disks\" has more than 16 entries";
        return false;
      }
      for (size_t i = 0; i < d.size(); i++) {
        if (d[i].is_null()) continue;
        if (!d[i].is_string()) {
          err = "\"disks\" entries must be strings or null";
          return false;
        }
        out.disks[i] = d[i].get<std::string>();
      }
    }

    if (j.contains("romapps")) {
      const json& apps = j.at("romapps");
      if (!apps.is_array()) {
        err = "\"romapps\" must be an array of {key,name,path} objects";
        return false;
      }
      for (const json& a : apps) {
        if (!a.is_object()) {
          err = "\"romapps\" entries must be objects";
          return false;
        }
        std::string key = a.value("key", std::string());
        std::string app_path = a.value("path", std::string());
        if (key.size() != 1 || !isalpha((unsigned char)key[0]) || app_path.empty()) {
          err = "\"romapps\" entries need \"key\" (one letter) and \"path\"";
          return false;
        }
        EmuConfig::RomApp app;
        app.key = toupper(key[0]);
        app.name = a.value("name", std::string());
        app.path = app_path;
        out.romapps.push_back(app);
      }
    }
  } catch (const json::exception& e) {
    // j.value() throws type_error when a key is present with the wrong type
    err = e.what();
    return false;
  }
  return true;
}

std::string emu_config_discover(const std::string& config_dir) {
  struct stat st;
  if (stat("romwbw_emu.json", &st) == 0) {
    return "romwbw_emu.json";
  }
  if (!config_dir.empty()) {
    std::string p = config_dir + "/config.json";
    if (stat(p.c_str(), &st) == 0) {
      return p;
    }
  }
  return "";
}

bool emu_config_save(const std::string& path, const EmuConfig& cfg, std::string& err) {
  json j;
  j["version"] = 1;
  if (!cfg.rom.empty()) j["rom"] = cfg.rom;
  if (!cfg.boot.empty()) j["boot"] = cfg.boot;
  if (!cfg.escape.empty()) j["escape"] = cfg.escape;
  if (!cfg.symbols.empty()) j["symbols"] = cfg.symbols;
  if (!cfg.romldr.empty()) j["romldr"] = cfg.romldr;
  j["debug"] = cfg.debug;
  j["strictIo"] = cfg.strictIo;

  // Size the array to the highest attached unit (all-null tail adds noise)
  int max_unit = -1;
  for (int i = 0; i < 16; i++) {
    if (!cfg.disks[i].empty()) max_unit = i;
  }
  if (max_unit >= 0) {
    json d = json::array();
    for (int i = 0; i <= max_unit; i++) {
      if (cfg.disks[i].empty()) {
        d.push_back(nullptr);
      } else {
        d.push_back(cfg.disks[i]);
      }
    }
    j["disks"] = d;
  }

  if (!cfg.romapps.empty()) {
    json apps = json::array();
    for (const EmuConfig::RomApp& a : cfg.romapps) {
      json o;
      o["key"] = std::string(1, a.key);
      if (!a.name.empty()) o["name"] = a.name;
      o["path"] = a.path;
      apps.push_back(o);
    }
    j["romapps"] = apps;
  }

  // Serialize before touching the filesystem - dump() throws type_error 316
  // on invalid UTF-8 (e.g. a Latin-1 byte in a path), and an abort here
  // would be an unhandled exception
  std::string text;
  try {
    text = j.dump(2) + "\n";
  } catch (const json::exception& e) {
    err = e.what();
    return false;
  }

  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) {
    err = "cannot create " + tmp;
    return false;
  }
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    err = "write failed for " + tmp;
    remove(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    err = "rename to " + path + " failed";
    remove(tmp.c_str());
    return false;
  }
  return true;
}
