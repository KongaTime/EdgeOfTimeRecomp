// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// installer/port_packages.h. The steps are the desktop build's (CMakeLists.txt):
// rexglue's achievement dump, pkzprep, then pkztool make over each manifest, with
// the game's Data folder as the reference packs (Main.pak there is Main.pkz,
// which PKZLib reads compressed).

#include "installer/port_packages.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

#include <tools/pkztool_make.h>

#include "core/build_info.h"
#include "core/logging.h"
#include "goliath/ui/menu_handles.h"
#include "pkzlib/tools/pkzprep.h"

namespace eot::installer {

namespace {

namespace fs = std::filesystem;

// Made from the port's art alone, at build time
constexpr const char *kPrebuilt[] = {"ReeotIcons.pkz", "ReeotSuits.pkz"};
// Made here, from the game's packs and Default.xex
constexpr const char *kAuthored[] = {"ReeotUI", "ReeotMenu", "ReeotAchievements"};
constexpr const char *kStampName = ".port-packages";

std::string BuildStamp() { return std::string(REEOT_GIT_COMMIT) + " " + REEOT_BUILD_TIMESTAMP; }

std::string ReadAll(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// As rexglue's achievement dump writes its strings
std::string TomlEscaped(const std::string &s) {
  std::string out;
  for (char c : s) {
    const unsigned char ch = static_cast<unsigned char>(c);
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\b')
      out += "\\b";
    else if (c == '\t')
      out += "\\t";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\f')
      out += "\\f";
    else if (c == '\r')
      out += "\\r";
    else if (ch < 0x20 || ch == 0x7F)
      out += std::format("\\u{:04X}", ch);
    else
      out += c;
  }
  return out;
}

// `rexglue init achievements`: achievements.toml and icons/<image id>.png
bool DumpAchievements(const fs::path &dir) {
  auto *kernel = rex::system::kernel_state();
  if (!kernel)
    return false;
  const rex::system::util::XdbfGameData db = kernel->title_xdbf();
  if (!db.is_valid())
    return false;
  const rex::system::XLanguage lang = db.GetExistingLanguage(rex::system::XLanguage::kEnglish);
  std::error_code ec;
  fs::create_directories(dir / "icons", ec);
  std::string toml;
  std::vector<uint32_t> images;
  for (const auto &entry : db.GetAchievements()) {
    const uint32_t image = entry.image_id;
    toml += "[[achievements]]\n";
    toml += std::format("id = {}\n", uint32_t(entry.id));
    toml += std::format("label = \"{}\"\n", TomlEscaped(db.GetStringTableEntry(lang, entry.label_id)));
    toml += std::format("description = \"{}\"\n", TomlEscaped(db.GetStringTableEntry(lang, entry.description_id)));
    toml += std::format("unachieved_description = \"{}\"\n",
                        TomlEscaped(db.GetStringTableEntry(lang, entry.unachieved_id)));
    toml += std::format("gamerscore = {}\n", uint32_t(entry.gamerscore));
    toml += std::format("image_id = {}\n\n", image);
    if (std::find(images.begin(), images.end(), image) != images.end())
      continue;
    images.push_back(image);
    const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, image);
    if (!block || !block.buffer || !block.size)
      continue;
    std::ofstream icon(dir / "icons" / std::format("{}.png", image), std::ios::binary);
    icon.write(reinterpret_cast<const char *>(block.buffer), static_cast<std::streamsize>(block.size));
  }
  std::ofstream out(dir / "achievements.toml", std::ios::binary);
  out << toml;
  return static_cast<bool>(out) && !images.empty();
}

// pkzprep's steps; each says what it wrote or why it stopped
bool Prepare(const fs::path &work) {
  const fs::path lib = work / "pkzlib";
  const fs::path generated = lib / "generated";
  std::error_code ec;
  fs::create_directories(generated, ec);
  for (const auto &it : fs::directory_iterator(lib / "strings", ec)) {
    const std::string name = it.path().filename().string();
    if (name.starts_with("menu.") && name != "menu.txt" &&
        pkzprep::Strings((lib / "strings" / "menu.txt").string(), it.path().string(), (generated / name).string()))
      return false;
  }
  const fs::path dump = work / "achievements";
  if (!DumpAchievements(dump)) {
    EOT_WARN("[install] Default.xex gave no achievements, so ReeotAchievements can't be made");
    return false;
  }
  return !pkzprep::Achievements((dump / "achievements.toml").string(), "REEOT_ACH_",
                                (lib / "strings" / "achievements.txt").string(),
                                (generated / "reeot_ach_strings.txt").string()) &&
         !pkzprep::AchievementIcons((dump / "icons").string(), (lib / "achievements" / "secret_plate.png").string(),
                                    (generated / "achievement_icons_64.png").string());
}

}  // namespace

void AuthorPortPackages(const fs::path &game, const fs::path &sources) {
  const fs::path custom = game / "Data" / eot::ui::kReeotPackageFolder;
  const fs::path stamp = custom / kStampName;
  const std::string build = BuildStamp();
  bool whole = ReadAll(stamp) == build;
  for (const char *name : kPrebuilt)
    whole = whole && fs::is_regular_file(custom / name);
  for (const char *name : kAuthored)
    whole = whole && fs::is_regular_file(custom / (std::string(name) + ".pkz"));
  if (whole)
    return;

  const auto started = std::chrono::steady_clock::now();
  EOT_INFO("[install] making the port's packages in {} from the game's files", custom.string());
  std::error_code ec;
  fs::create_directories(custom, ec);
  fs::remove(stamp, ec);
  bool ok = true;
  for (const char *name : kPrebuilt) {
    fs::copy_file(sources / "pkz" / name, custom / name, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      EOT_WARN("[install] could not copy {}: {}", name, ec.message());
      ec.clear();
      ok = false;
    }
  }

  // The manifests name their files relative to themselves, and the strings and
  // icons made here beside them: a copy to work in
  const fs::path work = custom / ".work";
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  for (const char *dir : {"pkzlib", "res"}) {
    fs::copy(sources / dir, work / dir, fs::copy_options::recursive, ec);
    if (ec) {
      EOT_WARN("[install] could not copy {}: {}", (sources / dir).string(), ec.message());
      ec.clear();
      ok = false;
    }
  }
  const bool prepared = ok && Prepare(work);
  for (const char *name : kAuthored) {
    if (!prepared)
      break;
    const fs::path out = custom / (std::string(name) + ".pkz");
    const fs::path part = custom / (std::string(name) + ".pkz.part");
    try {
      pkztool::Make((work / "pkzlib" / (std::string(name) + ".manifest")).string(), part.string(),
                    (game / "Data").string(), false);
      fs::rename(part, out, ec);
      if (ec)
        throw std::runtime_error("cannot rename " + part.string() + ": " + ec.message());
      EOT_INFO("[install] made {}", out.string());
    } catch (const std::exception &e) {
      EOT_WARN("[install] could not make {}: {}", name, e.what());
      fs::remove(part, ec);
      ok = false;
    }
  }
  ok = ok && prepared;
  fs::remove_all(work, ec);
  if (ok) {
    std::ofstream(stamp, std::ios::binary) << build;
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  if (ok)
    EOT_INFO("[install] the port's packages are made ({} ms)", ms.count());
  else
    EOT_WARN("[install] some of the port's packages are missing ({} ms); the lines above say why", ms.count());
}

}  // namespace eot::installer
