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
//
// The copy: one thread reads the image in 8 MB pieces while this one writes the
// last piece out with write(2), each file as <name>.part, renamed once whole. The
// second try wrote through std::ofstream, the read and write in turn, and ran at
// 15 MB/s for the first GB, then near 1 MB/s.

#include "installer/image_install.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

#include <imgui.h>
#include <rex/logging/api.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "core/logging.h"
#include "embedded.h"
#include "installer/disc_install.h"
#include "ui/theme.h"

namespace eot::installer {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSector = 0x800;
constexpr size_t kPiece = 8 << 20;
constexpr size_t kPieces = 4;

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

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
bool ReadDisc(const fs::path &iso, std::vector<DiscFile> &files, std::string &problem) {
  EOT_INFO("[install] reading {}", iso.string());
  Disc disc(iso);
  std::string error;
  if (!disc.Open(error) || !disc.List(files, error)) {
    problem = iso.filename().string() + ": " + error + ".";
    EOT_WARN("[install] {}", problem);
    return false;
  }
  const auto has = [&](std::string_view name) {
    return std::any_of(files.begin(), files.end(), [&](const DiscFile &f) { return SameName(f.path, name); });
  };
  if (!has("Default.xex") || !has("Data/GameLogic.dll")) {
    problem = iso.filename().string() + " is not Spider-Man: Edge of Time's disc image.";
    EOT_WARN("[install] {} ({} files)", problem, files.size());
    return false;
  }
  std::erase_if(files, [](const DiscFile &f) { return f.path.starts_with("$SystemUpdate/"); });
  return true;
}

// A piece of the image on its way from the reading thread to the writer
struct Piece {
  size_t file = 0;
  uint64_t at = 0;
  size_t length = 0;
  std::vector<uint8_t> *buffer = nullptr;
  bool failed = false;
};

class PieceQueue {
public:
  void Push(Piece piece) {
    {
      std::lock_guard lock(mutex_);
      pieces_.push_back(piece);
    }
    changed_.notify_all();
  }
  Piece Pop() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return !pieces_.empty(); });
    Piece piece = pieces_.front();
    pieces_.pop_front();
    return piece;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Piece> pieces_;
};

bool WriteAll(int fd, const uint8_t *p, size_t length) {
  while (length) {
    const ssize_t n = write(fd, p, length);
    if (n <= 0)
      return false;
    p += n;
    length -= size_t(n);
  }
  return true;
}

void ExtractDisc(const ImagePlan &plan, const fs::path &game, ImageProgress &progress) {
  Disc disc(plan.iso);
  std::string error;
  if (!disc.Open(error)) {
    progress.Fail(plan.iso.filename().string() + ": " + error);
    return;
  }

  // What an earlier try extracted whole stays
  std::vector<size_t> todo;
  uint64_t kept = 0;
  std::error_code ec;
  for (size_t i = 0; i < plan.files.size(); ++i) {
    const DiscFile &file = plan.files[i];
    const fs::path dest = game / fs::path(file.path);
    if (fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) == file.size)
      kept += file.size;
    else
      todo.push_back(i);
  }
  progress.bytes_kept.store(kept);
  progress.bytes_done.store(kept);
  EOT_INFO("[install] {} of {} files to extract; {} MB already there", todo.size(), plan.files.size(), kept >> 20);

  std::vector<std::vector<uint8_t>> buffers(kPieces, std::vector<uint8_t>(kPiece));
  PieceQueue full, empty;
  for (auto &buffer : buffers)
    empty.Push({0, 0, 0, &buffer});
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> read_us{0};
  std::thread reader([&] {
    for (const size_t index : todo) {
      const DiscFile &file = plan.files[index];
      for (uint64_t at = 0; at < file.size || (at == 0 && file.size == 0);) {
        Piece piece = empty.Pop();
        if (stop.load())
          return;
        piece.file = index;
        piece.at = at;
        piece.length = size_t(std::min<uint64_t>(kPiece, file.size - at));
        const double t = Now();
        piece.failed = piece.length && !disc.Read(file.offset + at, piece.buffer->data(), piece.length);
        read_us.fetch_add(uint64_t((Now() - t) * 1e6));
        full.Push(piece);
        if (piece.failed)
          return;
        at += piece.length;
        if (file.size == 0)
          break;
      }
    }
  });

  double write_seconds = 0;
  const double started = Now();
  uint64_t logged = 0;
  int fd = -1;
  size_t open_file = SIZE_MAX;
  bool ok = true;
  const auto finish_file = [&]() {
    if (fd < 0)
      return true;
    const bool closed = close(fd) == 0;
    fd = -1;
    const fs::path dest = game / fs::path(plan.files[open_file].path);
    fs::path part = dest;
    part += ".part";
    std::error_code rename_ec;
    fs::rename(part, dest, rename_ec);
    if (!closed || rename_ec) {
      progress.Fail("could not write " + dest.string());
      return false;
    }
    return true;
  };
  for (size_t done_files = 0; ok && done_files < todo.size();) {
    Piece piece = full.Pop();
    const DiscFile &file = plan.files[piece.file];
    if (piece.failed) {
      progress.Fail("could not read " + file.path + " from the disc image");
      ok = false;
      break;
    }
    if (piece.file != open_file) {
      if (!finish_file()) {
        ok = false;
        break;
      }
      open_file = piece.file;
      progress.SetCurrent(file.path);
      const fs::path dest = game / fs::path(file.path);
      fs::create_directories(dest.parent_path(), ec);
      fs::path part = dest;
      part += ".part";
      fd = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd < 0) {
        progress.Fail("could not create " + part.string());
        ok = false;
        break;
      }
    }
    const double t = Now();
    if (!WriteAll(fd, piece.buffer->data(), piece.length)) {
      progress.Fail("could not write " + file.path + " (is the storage full?)");
      ok = false;
      break;
    }
    write_seconds += Now() - t;
    const uint64_t done = progress.bytes_done.fetch_add(piece.length) + piece.length;
    empty.Push(piece);
    if (piece.at + piece.length >= file.size)
      ++done_files;
    if (done - logged >= (512ull << 20)) {
      logged = done;
      const double copied = double(done - kept) / double(1 << 20);
      EOT_INFO("[install] {} of {} MB, {:.1f} MB/s (reading {:.1f} s, writing {:.1f} s)", done >> 20,
               progress.bytes_total.load() >> 20, copied / std::max(Now() - started, 0.001), read_us.load() / 1e6,
               write_seconds);
    }
  }
  if (ok)
    ok = finish_file();
  else if (fd >= 0)
    close(fd);
  stop.store(true);
  empty.Push({});  // wakes a reader that is waiting for a buffer
  reader.join();
  if (ok)
    EOT_INFO("[install] the disc's files are extracted: {:.0f} s (reading {:.1f} s, writing {:.1f} s)",
             Now() - started, read_us.load() / 1e6, write_seconds);
}

// The title update's Default.xexp and Data/GameLogic.dllp, by the desktop installer
void ExtractUpdate(const ImagePlan &plan, const fs::path &game, ImageProgress &progress) {
  progress.SetCurrent(plan.update.filename().string());
  InstallSources sources;
  sources.update = plan.update;
  InstallProgress update;
  std::thread worker = Installer::RunAsync(sources, game, false, update);
  worker.join();
  if (update.failed.load())
    progress.Fail("the title update could not be installed: " + update.GetError());
}

std::string Size(uint64_t bytes) {
  return bytes >= (1ull << 30) ? std::format("{:.2f} GB", double(bytes) / double(1ull << 30))
                               : std::format("{:.0f} MB", double(bytes) / double(1 << 20));
}

}  // namespace

void ImageProgress::SetCurrent(const std::string &file) {
  std::lock_guard lock(mutex_);
  current_ = file;
}

std::string ImageProgress::Current() {
  std::lock_guard lock(mutex_);
  return current_;
}

void ImageProgress::Fail(const std::string &error) {
  {
    std::lock_guard lock(mutex_);
    if (error_.empty())
      error_ = error;
  }
  failed.store(true);
  EOT_ERROR("[install] {}", error);
}

std::string ImageProgress::Error() {
  std::lock_guard lock(mutex_);
  return error_;
}

void RunWriteTest(const fs::path &trigger, const std::vector<fs::path> &dirs) {
  std::error_code ec;
  if (!fs::exists(trigger, ec))
    return;
  fs::remove(trigger, ec);
  EOT_INFO("[write-test] start");
  constexpr size_t kChunk = 8 << 20;
  std::vector<uint8_t> data(kChunk);
  for (size_t i = 0; i < data.size(); ++i)
    data[i] = static_cast<uint8_t>(i * 2654435761u >> 24);
  const auto mb_s = [](uint64_t bytes, double seconds) { return double(bytes) / double(1 << 20) / std::max(seconds, 1e-3); };
  for (const fs::path &dir : dirs) {
    if (!fs::is_directory(dir, ec)) {
      EOT_INFO("[write-test] {}: not reachable from the title", dir.string());
      continue;
    }
    const fs::path file = dir / ".write-test";
    // A: 2 GB as the install writes it, then how long the system takes to put it on the drive
    int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
      EOT_INFO("[write-test] {}: cannot create a file there (errno {})", dir.string(), errno);
      continue;
    }
    double start = Now(), mark = start;
    uint64_t written = 0, marked = 0;
    bool ok = true;
    while (ok && written < (2048ull << 20)) {
      ok = WriteAll(fd, data.data(), data.size());
      written += data.size();
      if (written - marked >= (256ull << 20)) {
        const double now = Now();
        EOT_INFO("[write-test] {} A: {} MB written, {:.0f} MB/s over the last 256 MB", dir.string(), written >> 20,
                 mb_s(written - marked, now - mark));
        mark = now;
        marked = written;
      }
    }
    const double wrote = Now() - start;
    double t = Now();
    const bool synced = fsync(fd) == 0;
    const double flush = Now() - t;
    close(fd);
    EOT_INFO("[write-test] {} A: {} MB in {:.1f} s ({:.0f} MB/s), then fsync {:.1f} s{}; overall {:.0f} MB/s onto the "
             "drive",
             dir.string(), written >> 20, wrote, mb_s(written, wrote), flush, synced ? "" : " (failed)",
             mb_s(written, wrote + flush));
    fs::remove(file, ec);
    // B: 512 MB with an fsync every 128 MB: the drive's own rate on this path
    fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
      continue;
    for (int round = 0; round < 4 && ok; ++round) {
      t = Now();
      for (uint64_t n = 0; ok && n < (128ull << 20); n += data.size())
        ok = WriteAll(fd, data.data(), data.size());
      const double w = Now() - t;
      t = Now();
      fsync(fd);
      const double f = Now() - t;
      EOT_INFO("[write-test] {} B: 128 MB written in {:.2f} s, fsync {:.2f} s: {:.0f} MB/s onto the drive",
               dir.string(), w, f, mb_s(128ull << 20, w + f));
    }
    close(fd);
    fs::remove(file, ec);
    if (!ok)
      EOT_INFO("[write-test] {}: a write failed (errno {})", dir.string(), errno);
  }
  EOT_INFO("[write-test] done");
  rex::FlushLogging();
}

ImagePlan FindImages(const fs::path &game, const std::vector<fs::path> &look_in) {
  ImagePlan plan;
  std::error_code ec;
  const bool have_disc = fs::is_regular_file(game / "Default.xex", ec);
  const bool have_update = fs::is_regular_file(game / "Default.xexp", ec);
  if (have_disc && have_update)
    return plan;

  std::string disc_problem;
  for (const fs::path &dir : look_in) {
    EOT_INFO("[install] looking in {} for the disc image and the title update", dir.string());
    for (const auto &it : fs::directory_iterator(dir, ec)) {
      if (!it.is_regular_file(ec))
        continue;
      const fs::path &path = it.path();
      if (HasExtension(path, ".iso")) {
        if (!have_disc && plan.iso.empty() && ReadDisc(path, plan.files, disc_problem))
          plan.iso = path;
        continue;
      }
      if (have_update || !plan.update.empty() || HasExtension(path, ".bin") || HasExtension(path, ".zst") ||
          HasExtension(path, ".part"))
        continue;
      if (const PackageInfo info = InspectPackage(path); info.ok) {
        if (const std::string why = CheckUpdatePackage(info); why.empty())
          plan.update = path;
        else
          EOT_INFO("[install] {}: {}", path.string(), why);
      }
    }
  }
  if (plan.iso.empty() && plan.update.empty() && disc_problem.empty())
    return plan;  // nothing to install from: the caller says what is missing
  plan.needed = true;
  if (!have_disc && plan.iso.empty())
    plan.problem = !disc_problem.empty() ? disc_problem
                                         : "The title update is here, but not the disc image. Copy your .iso "
                                           "into the game's folder too.";
  // The recompiled code is the updated game's: it doesn't run without the update
  else if (!have_update && plan.update.empty())
    plan.problem = "The disc image is here, but not the title update. Copy the update package into the "
                   "game's folder too.";
  return plan;
}

std::thread RunImageInstall(const ImagePlan &plan, const fs::path &game, ImageProgress &progress) {
  uint64_t total = 0;
  for (const DiscFile &file : plan.files)
    total += file.size;
  progress.bytes_total.store(total);
  return std::thread([plan, game, &progress] {
    const double started = Now();
    std::error_code ec;
    fs::create_directories(game, ec);
    if (!plan.iso.empty())
      ExtractDisc(plan, game, progress);
    if (!progress.failed.load() && !plan.update.empty())
      ExtractUpdate(plan, game, progress);
    if (!progress.failed.load())
      EOT_INFO("[install] installed from {} and {} in {:.0f} s", plan.iso.string(), plan.update.string(),
               Now() - started);
    progress.complete.store(true);
  });
}

ImageInstallDialog::ImageInstallDialog(rex::ui::ImGuiDrawer *drawer, rex::ui::ImmediateDrawer *immediate_drawer,
                                       rex::ui::WindowedAppContext &app_context, ImagePlan plan, fs::path game,
                                       std::function<void()> on_done)
    : ImGuiDialog(drawer), immediate_drawer_(immediate_drawer), app_context_(app_context), plan_(std::move(plan)),
      game_(std::move(game)), on_done_(std::move(on_done)) {
  if (plan_.problem.empty()) {
    worker_ = RunImageInstall(plan_, game_, progress_);
  } else {
    EOT_WARN("[install] {}", plan_.problem);
  }
}

ImageInstallDialog::~ImageInstallDialog() {
  if (worker_.joinable())
    worker_.join();
}

void ImageInstallDialog::OnDraw(ImGuiIO &io) {
  if (!background_ && !background_tried_ && immediate_drawer_) {
    background_tried_ = true;
    int w = 0, h = 0, channels = 0;
    constexpr auto kImage = eot::Embedded("installer/installer.png");
    if (uint8_t *rgba = stbi_load_from_memory(kImage.data, static_cast<int>(kImage.size), &w, &h, &channels, 4)) {
      background_ = immediate_drawer_->CreateTexture(uint32_t(w), uint32_t(h),
                                                     rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
      stbi_image_free(rgba);
    }
  }

  const bool failed = !plan_.problem.empty() || progress_.failed.load();
  const bool complete = plan_.problem.empty() && progress_.complete.load() && !failed;
  if (complete && !finished_) {
    done_seconds_ += io.DeltaTime;
    // A moment at 100% before the game starts
    if (done_seconds_ > 1.0f) {
      finished_ = true;
      if (worker_.joinable())
        worker_.join();
      app_context_.CallInUIThreadDeferred(on_done_);
    }
  }

  auto *vp = ImGui::GetMainViewport();
  const float scale = std::max(1.0f, vp->Size.y / 720.0f);
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::SetNextWindowBgAlpha(1.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##image_install", nullptr, flags)) {
    if (background_)
      ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(background_.get()), vp->Pos,
                                           ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
    const float width = std::min(vp->Size.x - 64.0f * scale, 760.0f * scale);
    ImGui::SetCursorPos(ImVec2((vp->Size.x - width) * 0.5f, vp->Size.y * 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::Theme::kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 20 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 10 * scale));
    if (ImGui::BeginChild("##image_install_panel", ImVec2(width, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::SetWindowFontScale(scale * 1.25f);
      ImGui::TextUnformatted(failed ? "The game could not be installed"
                                    : complete ? "Installed" : "Installing Spider-Man: Edge of Time");
      ImGui::SetWindowFontScale(scale);
      if (failed) {
        const std::string why = !plan_.problem.empty() ? plan_.problem : progress_.Error();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
        ImGui::TextWrapped("%s", why.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped("Close the game with the PS button, fix that, and start it again. Files already "
                           "installed are kept.");
      } else {
        const uint64_t total = std::max<uint64_t>(progress_.bytes_total.load(), 1);
        const uint64_t done = std::min(progress_.bytes_done.load(), total);
        const float fraction = complete ? 1.0f : float(double(done) / double(total));
        const std::string label = std::format("{}%", int(fraction * 100.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.15f, 0.15f, 1.0f));
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 34.0f * scale), label.c_str());
        ImGui::PopStyleColor();
        const double now = Now();
        if (samples_.empty() || now - samples_.back().first >= 1.0)
          samples_.emplace_back(now, done);
        while (samples_.size() > 2 && now - samples_.front().first > 10.0)
          samples_.pop_front();
        const auto &[then, done_then] = samples_.front();
        const double rate = now - then >= 2.0 ? double(done - std::min(done, done_then)) / (now - then) : 0.0;
        if (now - then >= 5.0) {
          peak_rate_ = std::max(peak_rate_, rate);
          // The console takes about 1.5 GB at 200 MB/s or more, then writes at about
          // 25 MB/s until its cache has emptied; closing the app gives it the time
          // to, and the next start copies fast again (2026-10-15, on the console)
          if (peak_rate_ >= 60.0 * (1 << 20) && rate < peak_rate_ / 3.0 && total - done > (512ull << 20))
            slowed_ = true;
        }
        std::string line = std::format("{} of {}", Size(done), Size(total));
        if (rate > 0 && !complete) {
          const double left = double(total - done) / rate;
          line += std::format("   |   {:.0f} MB/s   |   about {}", rate / double(1 << 20),
                              left >= 90 ? std::format("{:.0f} min left", left / 60.0)
                                         : std::format("{:.0f} s left", std::max(left, 1.0)));
        }
        ImGui::TextUnformatted(line.c_str());
        const std::string current = complete ? std::string("Starting the game...") : progress_.Current();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Theme::White(0.6f));
        ImGui::TextUnformatted(current.c_str());
        if (!complete && !slowed_)
          ImGui::TextWrapped("You can close the game and come back: the files already installed are kept.");
        ImGui::PopStyleColor();
        if (!complete && slowed_) {
          ImGui::Spacing();
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
          ImGui::TextWrapped("The PS5 has slowed the install down. To speed it up again, close the game with the "
                             "PS button and open it again: it picks up where it left off. A full install usually "
                             "takes 2 or 3 restarts. Or just leave it running: it gets there either way.");
          ImGui::PopStyleColor();
        }
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
  }
  ImGui::End();
}

}  // namespace eot::installer
