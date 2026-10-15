// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// installer/port_packages.h: the port's packages, made where the game runs, for a
// build that embeds none (the PS5 title). The desktop build makes them from the
// game's packs and embeds them; a build shipped to players can't carry them,
// since three of the five copy textures, fonts and text styles out of the game's
// own packs and the achievements out of its Default.xex. So the title makes those
// three from the player's game folder the first time it runs, and copies the
// other two, made from the port's own art, from its program folder.

#pragma once

#include <filesystem>

namespace eot::installer {

// Writes the five packages into <game>/Data/custom unless this build already
// wrote them there. <sources> holds pkz/ (the two packages made from the port's
// art), pkzlib/ (the manifests and string tables) and res/ (the art the three
// other manifests read). Call it once Default.xex is loaded (its achievements
// come from the runtime) and before the game reads its packages.
void AuthorPortPackages(const std::filesystem::path &game, const std::filesystem::path &sources);

}  // namespace eot::installer
