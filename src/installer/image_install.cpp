// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// installer/image_install.h

#include "installer/image_install.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include <rex/filesystem/devices/disc_image_device.h>

#include "core/logging.h"
#include "installer/disc_install.h"

#if defined(__PROSPERO__)
extern "C" int sceKernelSendNotificationRequest(int, void *, size_t, int);
#endif

namespace eot::installer {

namespace {

namespace fs = std::filesystem;

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

}  // namespace

bool InstallFromImages(const fs::path &game, const std::vector<fs::path> &look_in) {
  std::error_code ec;
  const bool have_disc = fs::is_regular_file(game / "Default.xex", ec);
  const bool have_update = fs::is_regular_file(game / "Default.xexp", ec);
  if (have_disc && have_update)
    return true;

  InstallSources sources;
  for (const fs::path &dir : look_in) {
    for (const auto &it : fs::directory_iterator(dir, ec)) {
      if (!it.is_regular_file(ec))
        continue;
      const fs::path &path = it.path();
      if (HasExtension(path, ".iso")) {
        if (have_disc || !sources.disc.empty())
          continue;
        if (auto disc = OpenDiscImage(path); disc && ValidateDisc(*disc))
          sources.disc = path;
        else
          EOT_WARN("[install] could not read {} as Edge of Time's disc image", path.string());
        continue;
      }
      if (have_update || !sources.update.empty())
        continue;
      if (const PackageInfo info = InspectPackage(path); info.ok && CheckUpdatePackage(info).empty())
        sources.update = path;
    }
  }
  if (sources.disc.empty() && sources.update.empty())
    return true;
  if (!have_disc && sources.disc.empty()) {
    Tell("Edge of Time: found the title update but not the disc image. Copy your .iso into the gamedata folder.");
    return false;
  }
  // The recompiled code is the updated game's: it doesn't run without the update
  if (!have_update && sources.update.empty()) {
    Tell("Edge of Time: found the disc image but not the title update. Copy the update package into the "
         "gamedata folder too.");
    return false;
  }

  Tell("Edge of Time: installing from your " +
       std::string(!sources.disc.empty() ? "disc image" : "title update") + ". This takes a few minutes.");
  const auto started = std::chrono::steady_clock::now();
  InstallProgress progress;
  std::thread worker = Installer::RunAsync(sources, game, false, progress);
  size_t shown = 0;
  while (!progress.complete.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const size_t total = progress.bytes_total.load();
    const size_t percent = total ? progress.bytes_done.load() * 100 / total : 0;
    if (percent >= shown + 20 && percent < 100) {
      shown = percent / 20 * 20;
      Tell(std::format("Edge of Time: installing, {}%", shown));
    }
  }
  worker.join();
  if (progress.failed.load()) {
    Tell("Edge of Time: the install failed: " + progress.GetError());
    return false;
  }
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
  EOT_INFO("[install] installed from {} and {} in {} s", sources.disc.string(), sources.update.string(), seconds);
  std::string leftovers;
  for (const fs::path &source : {sources.disc, sources.update})
    if (!source.empty())
      leftovers += (leftovers.empty() ? "" : " and ") + source.filename().string();
  Tell("Edge of Time is installed. You can delete " + leftovers + " to free up space.");
  return true;
}

}  // namespace eot::installer
