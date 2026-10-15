// pkzprep.h: what pkzprep does, for the pkzprep tool and for a build that
// authors the port's packages where it runs (installer/port_packages.cpp).
//
//   Achievements(<achievements.toml>, <prefix>, <extra.txt>, <out.txt>)
//     rexglue's achievement dump -> <prefix><id>_NAME, _DESC, _LOCKED and _SCORE, then extra.txt's rows
//   AchievementIcons(<icons dir>, <plate.png>, <out.png>)
//     <id>.png icons on an 8x6 sheet of 64 px cells, the plate in the last cell
//   Strings(<english.txt>, <translation.txt>, <out.txt>)
//     every English row in its order, with the translation's text where it has one
//
// Include it in one source file: it holds stb_image, as static functions.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace pkzprep {
using Fields = std::vector<std::pair<std::string, std::string>>;

inline std::string Field(const Fields &fields, const char *key) {
  std::string found;
  for (const auto &[k, v] : fields)
    if (k == key)
      found = v;
  return found;
}

inline bool IsKey(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// key = value lines; a quoted value loses its quotes.
inline Fields ParseBlock(const std::string &block) {
  Fields fields;
  std::istringstream lines(block);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t k = line.find_first_not_of(" \t\r");
    if (k == std::string::npos)
      continue;
    size_t e = k;
    while (e < line.size() && IsKey(line[e]))
      ++e;
    if (e == k)
      continue;
    const size_t eq = line.find_first_not_of(" \t", e);
    if (eq == std::string::npos || line[eq] != '=')
      continue;
    const size_t v0 = line.find_first_not_of(" \t", eq + 1);
    const size_t v1 = line.find_last_not_of(" \t\r");
    if (v0 == std::string::npos || v1 < v0)
      continue;
    std::string value = line.substr(v0, v1 - v0 + 1);
    if (value[0] == '"')
      value = value.size() >= 2 ? value.substr(1, value.size() - 2) : std::string();
    fields.emplace_back(line.substr(k, e - k), value);
  }
  return fields;
}

inline bool ReadText(const std::string &path, std::string &out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "pkzprep: cannot open %s\n", path.c_str());
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

inline int Achievements(const std::string &in, const std::string &prefix, const std::string &extra, const std::string &out) {
  std::string text, more;
  if (!ReadText(in, text) || !ReadText(extra, more))
    return 1;
  const std::string marker = "[[achievements]]";
  std::vector<std::string> rows;
  size_t count = 0;
  for (size_t at = text.find(marker); at != std::string::npos;) {
    const size_t from = at + marker.size();
    const size_t next = text.find(marker, from);
    const Fields fields = ParseBlock(text.substr(from, next == std::string::npos ? std::string::npos : next - from));
    at = next;
    const std::string id = Field(fields, "id");
    if (id.empty())
      continue;
    const std::pair<const char *, const char *> kText[] = {
        {"NAME", "label"}, {"DESC", "description"}, {"LOCKED", "unachieved_description"}};
    for (const auto &[suffix, key] : kText)
      if (const std::string v = Field(fields, key); !v.empty())
        rows.push_back(prefix + id + "_" + suffix + "\t" + v);
    if (const std::string score = Field(fields, "gamerscore"); !score.empty())
      rows.push_back(prefix + id + "_SCORE\t" + score + " G");
    ++count;
  }
  if (!count) {
    std::fprintf(stderr, "pkzprep: no achievements in %s\n", in.c_str());
    return 1;
  }
  std::istringstream lines(more);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty() && line[0] != '#')
      rows.push_back(line);
  }
  std::ofstream o(out, std::ios::binary);
  o << "# NAME<TAB>text: the achievements of Default.xex, written by pkzprep -- do not edit.\n";
  for (const std::string &row : rows)
    o << row << '\n';
  if (!o) {
    std::fprintf(stderr, "pkzprep: cannot write %s\n", out.c_str());
    return 1;
  }
  std::printf("%s: %zu strings for %zu achievements\n", out.c_str(), rows.size(), count);
  return 0;
}

inline bool ReadRows(const std::string &path, Fields &rows) {
  std::string text;
  if (!ReadText(path, text))
    return false;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      std::fprintf(stderr, "pkzprep: %s: expected NAME<TAB>text: %s\n", path.c_str(), line.c_str());
      return false;
    }
    rows.emplace_back(line.substr(0, tab), line.substr(tab + 1));
  }
  return true;
}

// Every language's table lists the English rows in the same order: the windows find a string by its place.
inline int Strings(const std::string &english_path, const std::string &translation_path, const std::string &out) {
  Fields english, translation;
  if (!ReadRows(english_path, english) || !ReadRows(translation_path, translation))
    return 1;
  std::map<std::string, std::string> own(translation.begin(), translation.end());
  for (const auto &[key, text] : translation)
    if (std::none_of(english.begin(), english.end(), [&](const auto &row) { return row.first == key; })) {
      std::fprintf(stderr, "pkzprep: %s has %s, which %s does not\n", translation_path.c_str(), key.c_str(),
                   english_path.c_str());
      return 1;
    }
  std::ofstream o(out, std::ios::binary);
  o << "# NAME<TAB>text: the translation over the English rows, written by pkzprep -- do not edit.\n";
  for (const auto &[key, text] : english) {
    const auto it = own.find(key);
    o << key << '\t' << (it != own.end() ? it->second : text) << '\n';
  }
  if (!o) {
    std::fprintf(stderr, "pkzprep: cannot write %s\n", out.c_str());
    return 1;
  }
  std::printf("%s: %zu of %zu strings translated, the rest English\n", out.c_str(), own.size(), english.size());
  return 0;
}

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

inline bool LoadPng(const std::string &path, Image &img) {
  int channels = 0;
  stbi_uc *data = stbi_load(path.c_str(), &img.width, &img.height, &channels, 4);
  if (!data)
    return false;
  img.rgba.assign(data, data + size_t(img.width) * img.height * 4);
  stbi_image_free(data);
  return true;
}

inline uint32_t Crc32(const uint8_t *p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i)
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

inline void Put32(std::vector<uint8_t> &v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8)
    v.push_back(static_cast<uint8_t>(x >> s));
}

inline void Chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &body) {
  Put32(out, static_cast<uint32_t>(body.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), body.begin(), body.end());
  Put32(out, Crc32(out.data() + start, out.size() - start));
}

// RGBA8 PNG with stored deflate blocks: pkztool decodes it, nobody keeps it.
inline bool SavePng(const std::string &path, const Image &img) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const uint8_t *row = img.rgba.data() + size_t(y) * img.width * 4;
    raw.insert(raw.end(), row, row + size_t(img.width) * 4);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  for (size_t at = 0; at < raw.size() || raw.empty();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (raw.empty())
      break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  Put32(z, (b << 16) | a);
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, static_cast<uint32_t>(img.width));
  Put32(ihdr, static_cast<uint32_t>(img.height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  std::ofstream o(path, std::ios::binary);
  o.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(o);
}

inline int AchievementIcons(const std::string &dir, const std::string &plate_path, const std::string &out) {
  constexpr int kColumns = 8, kRows = 6, kCell = 64;
  Image sheet;
  sheet.width = kColumns * kCell;
  sheet.height = kRows * kCell;
  sheet.rgba.assign(size_t(sheet.width) * sheet.height * 4, 0);
  auto paste = [&](int slot, const Image &img) {
    const int ox = (slot % kColumns) * kCell, oy = (slot / kColumns) * kCell;
    for (int y = 0; y < kCell; ++y)
      for (int x = 0; x < kCell; ++x) {
        const size_t from = (size_t(y * img.height / kCell) * img.width + x * img.width / kCell) * 4;
        std::copy_n(img.rgba.data() + from, 4, sheet.rgba.data() + (size_t(oy + y) * sheet.width + ox + x) * 4);
      }
  };
  int icons = 0;
  for (int id = 1; id < kColumns * kRows; ++id) {
    Image img;
    if (!LoadPng(dir + "/" + std::to_string(id) + ".png", img))
      break;
    paste(id - 1, img);
    ++icons;
  }
  if (!icons) {
    std::fprintf(stderr, "pkzprep: no <image_id>.png icons in %s\n", dir.c_str());
    return 1;
  }
  Image plate;
  if (!LoadPng(plate_path, plate)) {
    std::fprintf(stderr, "pkzprep: cannot read %s\n", plate_path.c_str());
    return 1;
  }
  paste(kColumns * kRows - 1, plate);
  if (!SavePng(out, sheet)) {
    std::fprintf(stderr, "pkzprep: cannot write %s\n", out.c_str());
    return 1;
  }
  std::printf("%s: %dx%d, %d icons and the secret plate\n", out.c_str(), sheet.width, sheet.height, icons);
  return 0;
}

} // namespace pkzprep
