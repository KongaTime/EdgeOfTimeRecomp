// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// gpu/shaders/shader_dump.h. Each .pkz is a BUCompress container (PKZLib's
// BUDecompress.h): its header and tables, then one zlib stream per sector, each
// a sector long in the file. It is inflated a sector at a time and scanned as it
// arrives, so a pack never has to fit in memory whole.

#include "gpu/shaders/shader_dump.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <BaseUtils/BUDecompress.h>

namespace eot::gpu::shader_dump {

namespace {

constexpr size_t kHeader = 36;  // sizeof(XenosRecomp's ShaderContainer)
// A real container is a few KiB; a larger "container" is pack data that
// happens to look like a header
constexpr size_t kLargest = 8u << 20;

void (*g_note)(const char *) = nullptr;

void Note(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Note(const char *format, ...) {
  if (!g_note)
    return;
  char line[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  g_note(line);
}

uint32_t ReadBE32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i)
    h = (h ^ p[i]) * 0x100000001B3ull;
  return h ^ n;
}

struct Writer {
  FILE *out = nullptr;
  std::unordered_set<uint64_t> seen;
  Totals totals;

  void Emit(const uint8_t *data, size_t size) {
    ++totals.found;
    if (!seen.insert(Fnv1a(data, size)).second)
      return;
    static const uint8_t kPad[16] = {};
    std::fwrite(data, 1, size, out);
    std::fwrite(kPad, 1, sizeof(kPad), out);
    ++totals.shaders;
    totals.bytes += size + sizeof(kPad);
  }
};

// XenosRecomp's scan over one unpacked file or memory range, fed in pieces
class Scanner {
 public:
  Scanner(Writer &writer, uint64_t total) : writer_(writer), total_(total) {}

  void Feed(const uint8_t *data, size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
    Scan();
    // Keep only what the scan has not passed
    const uint64_t drop = at_ - base_;
    if (drop) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + std::min<uint64_t>(drop, buffer_.size()));
      base_ += drop;
    }
  }

 private:
  void Scan() {
    const uint64_t end = base_ + buffer_.size();
    while (total_ > kHeader && at_ < total_ - kHeader - 1) {
      if (at_ + kHeader > end)
        return;
      const uint8_t *header = buffer_.data() + (at_ - base_);
      const uint32_t flags = ReadBE32(header);
      const uint64_t size = uint64_t(ReadBE32(header + 4)) + ReadBE32(header + 8);
      if ((flags & 0xFFFFFF00u) == 0x102A1100u && size <= total_ - at_ && ReadBE32(header + 0x1C) == 0 &&
          ReadBE32(header + 0x20) == 0 && size >= kHeader && size <= kLargest) {
        if (at_ + size > end)
          return;  // the rest of it comes with the next piece
        writer_.Emit(header, size_t(size));
        at_ += size;
      } else {
        at_ += 4;
      }
    }
  }

  Writer &writer_;
  uint64_t total_;
  std::vector<uint8_t> buffer_;
  uint64_t base_ = 0;  // the offset of buffer_[0]
  uint64_t at_ = 0;    // the scan's offset
};

bool ReadAt(int fd, uint64_t offset, uint8_t *data, size_t size) {
  while (size) {
    const ssize_t got = pread(fd, data, size, off_t(offset));
    if (got <= 0)
      return false;
    data += got;
    size -= size_t(got);
    offset += uint64_t(got);
  }
  return true;
}

bool ScanCompressed(Writer &writer, int fd, uint64_t file_size, const char *path) {
  std::vector<uint8_t> header(64);
  if (!ReadAt(fd, 0, header.data(), header.size()))
    return false;
  const bool big32 = ReadBE32(header.data()) == BUDecompress::kMagic;
  const uint32_t header_size = big32 ? ReadBE32(header.data() + 8)
                                     : (uint32_t(header[8]) | uint32_t(header[9]) << 8 |
                                        uint32_t(header[10]) << 16 | uint32_t(header[11]) << 24);
  if (header_size < 28 || header_size > file_size || header_size > (64u << 20)) {
    Note("dump: %s: bad header size %u", path, header_size);
    return false;
  }
  header.resize(header_size);
  if (!ReadAt(fd, 0, header.data(), header.size()))
    return false;
  BUDecompress d;
  try {
    d.ParseHeader(header.data(), header.size());
  } catch (const std::exception &e) {
    Note("dump: %s: %s", path, e.what());
    return false;
  }
  const uint32_t sector_size = d.mCompHeader.uiSectorSize;
  Scanner scanner(writer, d.mCompHeader.uiUncompFileSize);
  std::vector<uint8_t> in(sector_size);
  std::vector<uint8_t> out(std::max<uint32_t>(d.mCompHeader.uiBigChunkSize, sector_size * 8));
  for (uint32_t sector = 0; sector < d.GetNbSectors(); ++sector) {
    const uint64_t begin = uint64_t(header_size) + uint64_t(sector) * sector_size;
    const uint64_t want = d.SectorEnd(sector) - d.SectorStart(sector);
    if (begin >= file_size || want > out.size()) {
      Note("dump: %s: sector %u unreadable", path, sector);
      return false;
    }
    const size_t avail = size_t(std::min<uint64_t>(sector_size, file_size - begin));
    if (!ReadAt(fd, begin, in.data(), avail)) {
      Note("dump: %s: sector %u unreadable", path, sector);
      return false;
    }
    z_stream strm{};
    if (inflateInit(&strm) != Z_OK)
      return false;
    strm.next_in = in.data();
    strm.avail_in = uInt(avail);
    strm.next_out = out.data();
    strm.avail_out = uInt(out.size());
    int rc = Z_OK;
    while (rc == Z_OK && strm.avail_in && strm.avail_out)
      rc = inflate(&strm, Z_NO_FLUSH);
    const uint64_t produced = strm.total_out;
    inflateEnd(&strm);
    if (produced < want) {
      Note("dump: %s: sector %u inflated to %llu of %llu bytes (zlib %d)", path, sector,
           (unsigned long long)produced, (unsigned long long)want, rc);
      return false;
    }
    scanner.Feed(out.data(), size_t(want));
  }
  return true;
}

bool ScanRaw(Writer &writer, int fd, uint64_t file_size) {
  Scanner scanner(writer, file_size);
  std::vector<uint8_t> chunk(1u << 20);
  for (uint64_t at = 0; at < file_size;) {
    const size_t n = size_t(std::min<uint64_t>(chunk.size(), file_size - at));
    if (!ReadAt(fd, at, chunk.data(), n))
      return false;
    scanner.Feed(chunk.data(), n);
    at += n;
  }
  return true;
}

bool EndsWith(const std::string &s, const char *suffix) {
  const size_t n = std::strlen(suffix);
  if (s.size() < n)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
      return false;
  return true;
}

void ListPacks(const std::string &folder, std::vector<std::string> &packs) {
  DIR *dir = opendir(folder.c_str());
  if (!dir)
    return;
  while (dirent *entry = readdir(dir)) {
    if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
      continue;
    const std::string path = folder + "/" + entry->d_name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      ListPacks(path, packs);
    else if (EndsWith(path, ".pkz") || EndsWith(path, ".pak"))
      packs.push_back(path);
  }
  closedir(dir);
}

}  // namespace

bool Run(const char *game_folder, const char *out_path,
         const std::vector<std::span<const uint8_t>> &memory, void (*note)(const char *line),
         Totals *totals) {
  g_note = note;
  Writer writer;
  writer.out = std::fopen(out_path, "wb");
  if (!writer.out) {
    Note("dump: FAILED: cannot write %s", out_path);
    return false;
  }

  for (size_t i = 0; i < memory.size(); ++i) {
    const size_t before = writer.totals.shaders;
    Scanner scanner(writer, memory[i].size());
    scanner.Feed(memory[i].data(), memory[i].size());
    Note("dump: memory range %zu (%zu KB): %zu new shaders, %zu in all", i + 1, memory[i].size() >> 10,
         writer.totals.shaders - before, writer.totals.shaders);
  }

  Note("dump: looking for the game's packs in %s", game_folder);
  std::vector<std::string> packs;
  ListPacks(game_folder, packs);
  std::sort(packs.begin(), packs.end());
  if (packs.empty())
    Note("dump: FAILED: no .pkz or .pak files in %s", game_folder);
  const size_t prefix = std::strlen(game_folder) + 1;
  for (size_t i = 0; i < packs.size(); ++i) {
    const std::string &path = packs[i];
    ++writer.totals.packs;
    const int fd = open(path.c_str(), O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
      Note("dump: %s: cannot open", path.c_str());
      if (fd >= 0)
        close(fd);
      ++writer.totals.failed;
      continue;
    }
    const uint64_t size = uint64_t(st.st_size);
    uint8_t magic[64] = {};
    const size_t head = size_t(std::min<uint64_t>(sizeof(magic), size));
    const bool read = ReadAt(fd, 0, magic, head);
    const size_t before = writer.totals.shaders;
    bool ok = false;
    if (read && BUDecompress::IsCompressed(magic, head))
      ok = ScanCompressed(writer, fd, size, path.c_str());
    else if (read)
      ok = ScanRaw(writer, fd, size);
    close(fd);
    if (!ok)
      ++writer.totals.failed;
    Note("dump: [%zu/%zu] %s (%llu MB): %zu new shaders, %zu in all%s", i + 1, packs.size(),
         path.size() > prefix ? path.c_str() + prefix : path.c_str(), (unsigned long long)(size >> 20),
         writer.totals.shaders - before, writer.totals.shaders, ok ? "" : " - FAILED");
    std::fflush(writer.out);
  }
  std::fclose(writer.out);
  chmod(out_path, 0666);
  Note("dump: DONE: %zu shaders (%zu found with repeats), %llu KB in %s; %zu pack(s) failed",
       writer.totals.shaders, writer.totals.found, (unsigned long long)(writer.totals.bytes >> 10), out_path,
       writer.totals.failed);
  if (totals)
    *totals = writer.totals;
  return !packs.empty() && !writer.totals.failed;
}

}  // namespace eot::gpu::shader_dump
