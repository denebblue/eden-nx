// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// In-memory ZIP helpers for the Nextendo frontend (BCAT packages, cloud saves), built on QuaZip,
// which Eden already bundles on every platform. Citron used libarchive (and PowerShell on
// Windows) for the same job.
namespace Nextendo::Zip {

struct Entry {
    std::string name;  // path inside the archive, '/' separated, no trailing '/' for directories
    bool directory = false;
    bool symlink = false;
    bool encrypted = false;
    std::vector<unsigned char> data;
};

// Reads every entry of an in-memory ZIP. Throws std::runtime_error when the archive can't be
// opened, an entry fails its CRC/inflate, or the limits (entry count, total expanded size) are
// exceeded. Directories, symlinks and encrypted entries are reported, not rejected: the caller
// decides.
std::vector<Entry> ReadAll(std::span<const unsigned char> bytes, std::size_t max_total,
                           std::size_t max_entries);

// Extracts a ZIP into dest. Refuses (returns false, writes nothing) archives with absolute paths,
// "..", symlinks or encrypted entries.
bool ExtractToDirectory(std::span<const unsigned char> bytes, const std::filesystem::path& dest);

// Builds a ZIP from (archive path, contents) pairs. Empty on failure.
std::vector<unsigned char> Create(
    const std::vector<std::pair<std::string, std::vector<unsigned char>>>& files);

} // namespace Nextendo::Zip
