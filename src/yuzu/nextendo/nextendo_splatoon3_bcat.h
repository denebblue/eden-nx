// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "yuzu/nextendo/nextendo_zip.h"

namespace Nextendo::Splatoon3Bcat {
inline constexpr unsigned long long TitleId = 0x0100C2500FC20000ULL;
inline constexpr std::array<std::string_view, 4> Regions{
    "ap-default", "eu-default", "jp-default", "us-default"};
inline constexpr std::array<std::string_view, 6> Files{
    "summary.yml", "base.pack.zs.enc", "bh.pack.zs.enc", "wa.pack.zs.enc",
    "wb.pack.zs.enc", "wc.pack.zs.enc"};
inline constexpr std::size_t MaxSize = 32 * 1024 * 1024;
inline std::mutex InstallMutex;

inline bool IsInstalled(const std::filesystem::path& root) {
    std::error_code ec;
    for (const auto region : Regions) {
        for (const auto file : Files) {
            const auto path = root / region / file;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) ||
                ec || std::filesystem::file_size(path, ec) == 0 || ec) {
                return false;
            }
        }
    }
    return true;
}

// Validate in memory before touching the installed package. No archive path is used
// on disk until it has passed the single-region/single-filename allowlist.
// Returns true when the installed package changed, false when it was already identical.
inline bool Install(std::span<const unsigned char> bytes, const std::filesystem::path& target) {
    std::lock_guard lock{InstallMutex};
    if (bytes.empty() || bytes.size() > MaxSize) {
        throw std::runtime_error("Invalid BCAT ZIP size");
    }
    const auto entries = Nextendo::Zip::ReadAll(bytes, MaxSize, 512);
    std::map<std::string, std::vector<unsigned char>> contents;
    for (const auto& entry : entries) {
        if (entry.symlink || entry.encrypted) {
            throw std::runtime_error("Invalid BCAT entry");
        }
        const std::string& name = entry.name;
        const auto slash = name.find('/');
        const auto region = name.substr(0, slash);
        if (std::find(Regions.begin(), Regions.end(), region) == Regions.end()) {
            throw std::runtime_error("Invalid BCAT region");
        }
        if (entry.directory && slash == std::string::npos) continue;
        if (entry.directory || slash == std::string::npos) {
            throw std::runtime_error("Invalid BCAT file");
        }
        const auto file = name.substr(slash + 1);
        if (file.empty() || file.size() > 31 || file == "." || file == ".." ||
            !std::all_of(file.begin(), file.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            })) {
            throw std::runtime_error("Unsafe BCAT filename");
        }
        std::string folded = name;
        for (auto& c : folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        for (const auto& [old_name, unused] : contents) {
            std::string old_folded = old_name;
            for (auto& c : old_folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (old_folded == folded) throw std::runtime_error("Duplicate BCAT file");
        }
        if (entry.data.empty()) {
            throw std::runtime_error("Invalid expanded BCAT size");
        }
        contents[name] = entry.data;
    }
    for (const auto region : Regions) for (const auto file : Files) {
        if (!contents.contains(std::string(region) + "/" + std::string(file))) {
            throw std::runtime_error("Incomplete BCAT package");
        }
    }
    auto stage = target;
    stage += ".nextendo-stage";
    auto backup = target;
    backup += ".nextendo-backup";
    // Recover an interrupted promotion before beginning another one.
    if (!std::filesystem::exists(target) && std::filesystem::exists(backup)) {
        std::filesystem::rename(backup, target);
    }
    bool identical = IsInstalled(target);
    std::size_t disk_files = 0;
    if (identical) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(target)) {
            if (item.is_symlink()) { identical = false; break; }
            if (item.is_regular_file()) ++disk_files;
        }
        identical = identical && disk_files == contents.size();
    }
    if (identical) for (const auto& [name, data] : contents) {
        std::ifstream in{target / name, std::ios::binary};
        const std::vector<unsigned char> existing{std::istreambuf_iterator<char>{in}, {}};
        if (!in || existing != data) { identical = false; break; }
    }
    if (identical) return false;
    std::filesystem::remove_all(stage);
    try {
        for (const auto& [name, data] : contents) {
            const auto path = stage / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out{path, std::ios::binary | std::ios::trunc};
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            out.close();
            if (!out) throw std::runtime_error("Cannot write BCAT staging file");
        }
        std::filesystem::remove_all(backup);
        const bool had_old = std::filesystem::exists(target);
        if (had_old) std::filesystem::rename(target, backup);
        try {
            std::filesystem::rename(stage, target);
        } catch (...) {
            if (had_old) std::filesystem::rename(backup, target);
            throw;
        }
        std::error_code ec;
        std::filesystem::remove_all(backup, ec);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(stage, ec);
        throw;
    }
    return true;
}
} // namespace Nextendo::Splatoon3Bcat
