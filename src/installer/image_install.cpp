// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// installer/image_install.h. The disc image is read here with plain reads, not
// through the SDK's DiscImageDevice, which maps the whole image into memory: on
// the PS5 that never returned for a retail image (the first try, 2026-10-15:
// the title sat at the paths step until it was closed). Its layout is the
// device's (GDFX): a partition at one of a few offsets, its volume descriptor in
// sector 32, and directories as binary trees of entries. The title update is an
// STFS package, which the SDK reads with plain reads, so the desktop installer
// extracts that.

#include "installer/image_install.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "core/logging.h"
#include "installer/disc_install.h"

#if defined(__PROSPERO__)
extern "C" int sceKernelSendNotificationRequest(int, void *, size_t, int);
#endif

namespace eot::installer {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSector = 0x800;

// To the log and, on the PS5, to a notification: the install runs before the
// game draws anything
void Tell(const std::string &message) {
  EOT_INFO("[install] {}", message);
#if defined(__PROSPERO__)
  struct {
    char reserved[45];
    char message[3075];
  } request{};
  std::snprintf(request.message, sizeof(request.message), "%s", message.c_str());
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#endif
}

bool HasExtension(const fs::path &path, std::string_view extension) {
  std::string ext = path.extension().string();
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == extension;
}

bool SameName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

struct DiscFile {
  std::string path;
  uint64_t offset = 0;
  uint64_t size = 0;
};

class Disc {
public:
  explicit Disc(const fs::path &path) : fd_(open(path.c_str(), O_RDONLY)) {}
  ~Disc() {
    if (fd_ >= 0)
      close(fd_);
  }
  Disc(const Disc &) = delete;
  Disc &operator=(const Disc &) = delete;

  bool Read(uint64_t offset, void *out, size_t length) const {
    auto *p = static_cast<uint8_t *>(out);
    while (length) {
      const ssize_t n = pread(fd_, p, length, static_cast<off_t>(offset));
      if (n <= 0)
        return false;
      p += n;
      offset += uint64_t(n);
      length -= size_t(n);
    }
    return true;
  }

  // The partition and its root, as DiscImageDevice::Verify finds them
  bool Open(std::string &error) {
    if (fd_ < 0) {
      error = "cannot open it";
      return false;
    }
    static constexpr uint64_t kOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
    for (const uint64_t offset : kOffsets) {
      uint8_t volume[28];
      if (!Read(offset + 32 * kSector, volume, sizeof(volume)) ||
          std::memcmp(volume, "MICROSOFT*XBOX*MEDIA", 20) != 0)
        continue;
      game_offset_ = offset;
      std::memcpy(&root_sector_, volume + 20, 4);
      std::memcpy(&root_size_, volume + 24, 4);
      if (root_size_ < 13 || root_size_ > (32u << 20)) {
        error = "its root directory is damaged";
        return false;
      }
      return true;
    }
    error = "it is not an Xbox 360 disc image";
    return false;
  }

  bool List(std::vector<DiscFile> &files, std::string &error) const {
    return ListDirectory(root_sector_, root_size_, "", 0, files, error);
  }

private:
  bool ListDirectory(uint32_t sector, uint32_t size, const std::string &prefix, int depth,
                     std::vector<DiscFile> &files, std::string &error) const {
    if (depth > 32 || size > (32u << 20)) {
      error = "a directory is damaged";
      return false;
    }
    std::vector<uint8_t> table(size);
    if (!Read(game_offset_ + uint64_t(sector) * kSector, table.data(), table.size())) {
      error = "a directory could not be read";
      return false;
    }
    return ListEntry(table, 0, prefix, depth, files, error);
  }

  bool ListEntry(const std::vector<uint8_t> &table, uint16_t ordinal, const std::string &prefix, int depth,
                 std::vector<DiscFile> &files, std::string &error) const {
    const size_t at = size_t(ordinal) * 4;
    if (at + 14 > table.size()) {
      error = "a directory entry is out of bounds";
      return false;
    }
    const uint8_t *p = table.data() + at;
    uint16_t left, right;
    uint32_t sector, length;
    std::memcpy(&left, p + 0, 2);
    std::memcpy(&right, p + 2, 2);
    std::memcpy(&sector, p + 4, 4);
    std::memcpy(&length, p + 8, 4);
    const uint8_t attributes = p[12];
    const uint8_t name_length = p[13];
    if (at + 14 + name_length > table.size()) {
      error = "a directory entry is out of bounds";
      return false;
    }
    if (left && !ListEntry(table, left, prefix, depth, files, error))
      return false;
    const std::string path = prefix + std::string(reinterpret_cast<const char *>(p + 14), name_length);
    if (attributes & 0x10) {
      if (length && !ListDirectory(sector, length, path + "/", depth + 1, files, error))
        return false;
    } else {
      files.push_back({path, game_offset_ + uint64_t(sector) * kSector, length});
    }
    return !right || ListEntry(table, right, prefix, depth, files, error);
  }

  int fd_;
  uint64_t game_offset_ = 0;
  uint32_t root_sector_ = 0;
  uint32_t root_size_ = 0;
};

// Lists the game's disc image, or says why it isn't one
bool ReadDisc(const fs::path &iso, std::vector<DiscFile> &files) {
  EOT_INFO("[install] reading {}", iso.string());
  Disc disc(iso);
  std::string error;
  if (!disc.Open(error) || !disc.List(files, error)) {
    EOT_WARN("[install] {}: {}", iso.string(), error);
    return false;
  }
  const auto has = [&](std::string_view name) {
    return std::any_of(files.begin(), files.end(), [&](const DiscFile &f) { return SameName(f.path, name); });
  };
  if (!has("Default.xex") || !has("Data/GameLogic.dll")) {
    EOT_WARN("[install] {} is not Edge of Time's disc ({} files, no Default.xex and Data/GameLogic.dll)",
             iso.string(), files.size());
    return false;
  }
  return true;
}

bool ExtractDisc(const fs::path &iso, const std::vector<DiscFile> &files, const fs::path &game) {
  Disc disc(iso);
  std::string error;
  if (!disc.Open(error)) {
    Tell("Edge of Time: the install failed: " + iso.filename().string() + ": " + error);
    return false;
  }
  uint64_t total = 0, done = 0;
  for (const DiscFile &file : files)
    total += file.size;
  EOT_INFO("[install] {} files, {} MB from {}", files.size(), total >> 20, iso.string());
  std::vector<uint8_t> buffer(8 << 20);
  uint64_t shown = 0;
  std::error_code ec;
  for (const DiscFile &file : files) {
    if (file.path.starts_with("$SystemUpdate/"))
      continue;
    const fs::path dest = game / fs::path(file.path);
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    for (uint64_t at = 0; at < file.size && out;) {
      const size_t n = size_t(std::min<uint64_t>(buffer.size(), file.size - at));
      if (!disc.Read(file.offset + at, buffer.data(), n)) {
        Tell("Edge of Time: the install failed: could not read " + file.path + " from the disc image");
        return false;
      }
      out.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(n));
      at += n;
      done += n;
      const uint64_t percent = total ? done * 100 / total : 100;
      if (percent >= shown + 20 && percent < 100) {
        shown = percent / 20 * 20;
        Tell(std::format("Edge of Time: installing, {}%", shown));
      }
    }
    if (!out) {
      Tell("Edge of Time: the install failed: could not write " + dest.string());
      return false;
    }
  }
  return true;
}

// The title update's Default.xexp and Data/GameLogic.dllp, by the desktop installer
bool ExtractUpdate(const fs::path &package, const fs::path &game) {
  InstallSources sources;
  sources.update = package;
  InstallProgress progress;
  std::thread worker = Installer::RunAsync(sources, game, false, progress);
  worker.join();
  if (progress.failed.load()) {
    Tell("Edge of Time: the title update could not be installed: " + progress.GetError());
    return false;
  }
  return true;
}

}  // namespace

bool InstallFromImages(const fs::path &game, const std::vector<fs::path> &look_in) {
  std::error_code ec;
  const bool have_disc = fs::is_regular_file(game / "Default.xex", ec);
  const bool have_update = fs::is_regular_file(game / "Default.xexp", ec);
  if (have_disc && have_update)
    return true;

  fs::path iso, update;
  std::vector<DiscFile> files;
  for (const fs::path &dir : look_in) {
    EOT_INFO("[install] looking in {} for the disc image and the title update", dir.string());
    for (const auto &it : fs::directory_iterator(dir, ec)) {
      if (!it.is_regular_file(ec))
        continue;
      const fs::path &path = it.path();
      if (HasExtension(path, ".iso")) {
        if (!have_disc && iso.empty() && ReadDisc(path, files))
          iso = path;
        continue;
      }
      if (have_update || !update.empty() || HasExtension(path, ".bin") || HasExtension(path, ".zst"))
        continue;
      if (const PackageInfo info = InspectPackage(path); info.ok) {
        if (const std::string why = CheckUpdatePackage(info); why.empty())
          update = path;
        else
          EOT_INFO("[install] {}: {}", path.string(), why);
      }
    }
  }
  if (iso.empty() && update.empty())
    return true;
  if (!have_disc && iso.empty()) {
    Tell("Edge of Time: found the title update but not the disc image. Copy your .iso into the gamedata folder.");
    return false;
  }
  // The recompiled code is the updated game's: it doesn't run without the update
  if (!have_update && update.empty()) {
    Tell("Edge of Time: found the disc image but not the title update. Copy the update package into the "
         "gamedata folder too.");
    return false;
  }

  Tell("Edge of Time: installing from your " + std::string(!iso.empty() ? "disc image" : "title update") +
       ". This takes a few minutes.");
  const auto started = std::chrono::steady_clock::now();
  if (!iso.empty() && !ExtractDisc(iso, files, game))
    return false;
  if (!update.empty() && !ExtractUpdate(update, game))
    return false;
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
  EOT_INFO("[install] installed from {} and {} in {} s", iso.string(), update.string(), seconds);
  std::string leftovers;
  for (const fs::path &source : {iso, update})
    if (!source.empty())
      leftovers += (leftovers.empty() ? "" : " and ") + source.filename().string();
  Tell("Edge of Time is installed. You can delete " + leftovers + " to free up space.");
  return true;
}

}  // namespace eot::installer
