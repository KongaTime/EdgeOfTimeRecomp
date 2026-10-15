// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// installer/image_install.h: the game folder filled from the disc image and the
// title update package as they come, for a build with no installer wizard (the
// PS5 title). It finds them (FindImages), extracts what the folder lacks on a
// thread of its own (ImageInstall), and shows a progress screen meanwhile
// (ImageInstallDialog). A file already extracted whole is kept, so an install
// that was closed picks up where it stopped.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
class WindowedAppContext;
}  // namespace rex::ui

namespace eot::installer {

struct DiscFile {
  std::string path;
  uint64_t offset = 0;
  uint64_t size = 0;
};

struct ImagePlan {
  bool needed = false;     // the game folder lacks Default.xex or Default.xexp
  std::string problem;     // why it can't be filled, for the player; empty when it can
  std::filesystem::path iso;     // empty when the folder has the disc's files
  std::filesystem::path update;  // empty when it has the update's
  std::vector<DiscFile> files;   // the disc's
};

// Looks in each of <look_in> (not below) for the game's disc image (.iso) and its
// title update package, for what <game> lacks
ImagePlan FindImages(const std::filesystem::path &game, const std::vector<std::filesystem::path> &look_in);

struct ImageProgress {
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<uint64_t> bytes_kept{0};  // already there from an earlier try
  std::atomic<bool> complete{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> stop{false};  // asked to stop between pieces, to start again

  void SetCurrent(const std::string &file);
  std::string Current();
  void Fail(const std::string &error);
  std::string Error();

private:
  std::mutex mutex_;
  std::string current_;
  std::string error_;
};

// Extracts the plan into <game> on a thread; <progress> says how far it got
std::thread RunImageInstall(const ImagePlan &plan, const std::filesystem::path &game, ImageProgress &progress);

// The install's progress screen. With a plan that has a problem, it shows the
// problem and stays. Otherwise it runs the install, and once it is done calls
// <on_done> on the UI thread.
class ImageInstallDialog : public rex::ui::ImGuiDialog {
public:
  ImageInstallDialog(rex::ui::ImGuiDrawer *drawer, rex::ui::ImmediateDrawer *immediate_drawer,
                     rex::ui::WindowedAppContext &app_context, ImagePlan plan, std::filesystem::path game,
                     std::function<void()> on_done);
  ~ImageInstallDialog() override;

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  rex::ui::ImmediateDrawer *immediate_drawer_;
  rex::ui::WindowedAppContext &app_context_;
  ImagePlan plan_;
  std::filesystem::path game_;
  std::function<void()> on_done_;
  ImageProgress progress_;
  std::thread worker_;
  std::unique_ptr<rex::ui::ImmediateTexture> background_;
  bool background_tried_ = false;
  bool finished_ = false;
  // (time, bytes done) about once a second over the last 10 s: the rate shown
  // is the recent one, since the console's write cache makes the first GB or two
  // far faster than the rest (the third try: 240 MB/s, then 15)
  std::deque<std::pair<double, uint64_t>> samples_;
  double rate_ = 0, peak_rate_ = 0, slow_since_ = 0;
  bool restarting_ = false, restart_asked_ = false;
  std::string restart_note_;
  float done_seconds_ = 0;

  void WatchRate(uint64_t done, uint64_t total);
  void Restart();
};

}  // namespace eot::installer
