// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"

// One-click installer for the Super Smash Bros. Ultimate online mod stack (SSBU Online Deluxe and
// its Skyline plugins), following the emulator layout of the upstream create-sdcard-folder script.
//
// Differences from Citron's installer, each fixing a failure users hit there:
//  - ozone provides exefs/ (not ARCropolis's bundled Skyline): Online Deluxe requires ozone, and
//    mixing the two is what made ARCropolis report a wrong game version and crash at boot.
//  - Asset names are matched inside zips by file name, so upstream repackaging (Online Deluxe
//    moving from a zip to a bare .nro, ssbusync shipping a zip) no longer breaks the install.
//  - All downloads finish and validate before anything on disk changes; a failed write restores
//    the backup. A half-installed plugin set is never left behind.
//  - ARCropolis's sdmc:/ultimate/arcropolis resources are installed too.
//  - Plugins known to conflict (old latency slider, vsync, less-lag, overclock mods) are removed.
//  - The installed game version is checked first.
namespace Nextendo::SsbuMods {

constexpr u64 TitleId = 0x01006A800016E000ULL;
// Minimum game version the current plugin stack supports.
constexpr const char* RequiredVersion = "13.0.5";

// True when the version string (e.g. "13.0.5") is at least RequiredVersion.
bool IsSupportedVersion(const std::string& display_version);

// True when a Skyline-compatible loader is installed for SSBU on the emulated SD card.
bool IsLoaderInstalled();

struct Report {
    bool success = false;
    std::vector<std::string> installed;             // "component vX.Y.Z"
    std::vector<std::string> errors;                // fatal problems (nothing was changed)
    std::vector<std::string> removed_conflicts;     // plugin files removed
    std::vector<std::string> warnings;              // non-fatal notes for the user
    std::filesystem::path backup_dir;               // empty if nothing needed backing up
};

// Blocking: downloads, validates and installs. Call from a worker thread.
Report Install();

} // namespace Nextendo::SsbuMods
