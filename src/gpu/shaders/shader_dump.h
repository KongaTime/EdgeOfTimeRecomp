// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// The game's shader containers, gathered for XenosRecomp where the game is, as
// one file XenosRecomp reads as it reads the unpacked packs: the PS5 build has
// no PC to unpack them on, and the containers Default.xex holds are only whole
// once the runtime has loaded and patched it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace eot::gpu::shader_dump {

struct Totals {
  size_t packs = 0;    // .pkz and .pak files read
  size_t failed = 0;   // of them, the ones that could not be read whole
  size_t shaders = 0;  // distinct containers written
  size_t found = 0;    // containers found, repeats too
  uint64_t bytes = 0;  // written
};

// Writes to out_path every shader container in the .pkz and .pak files under
// game_folder, and in each memory range given (the loaded Default.xex's
// readable parts), each container once, 16 zero bytes after each. A container
// is what XenosRecomp's scan takes (its main.cpp): a header on a 4-byte step
// from the start of the unpacked file or range, flags 0x102A11xx, words 0x1C
// and 0x20 zero, its virtual and physical sizes inside what is left. note gets
// a line per file and a last one starting "dump: DONE". True when out_path was
// written and every pack read.
bool Run(const char *game_folder, const char *out_path,
         const std::vector<std::span<const uint8_t>> &memory, void (*note)(const char *line),
         Totals *totals);

}  // namespace eot::gpu::shader_dump
