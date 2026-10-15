// Copyright (C) 2026 KongaTime
// SPDX-License-Identifier: MIT
//
// installer/image_install.h: the game folder filled from the disc image and the
// title update package as they come, for a build with no installer window (the
// PS5 title). The extraction is the desktop installer's (installer/disc_install.h);
// what it is doing goes to the log and, on the PS5, to the console's notifications.

#pragma once

#include <filesystem>
#include <vector>

namespace eot::installer {

// When <game> lacks Default.xex or Default.xexp, looks in each of <look_in> (not
// below) for the game's disc image (.iso) and its title update package, and
// extracts what <game> lacks into it. Returns false when it found something to
// install but could not install it; true when the folder was whole already, when
// there was nothing to install from, or when the install finished.
bool InstallFromImages(const std::filesystem::path &game, const std::vector<std::filesystem::path> &look_in);

}  // namespace eot::installer
