// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nextendo/nextendo_ssbu_mods.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "web_service/github_releases.h"
#include "yuzu/nextendo/nextendo_zip.h"

namespace Nextendo::SsbuMods {

namespace fs = std::filesystem;

namespace {

constexpr const char* TitleDir = "atmosphere/contents/01006A800016E000";
constexpr const char* ExefsDir = "atmosphere/contents/01006A800016E000/exefs";
constexpr const char* SkylineDir = "atmosphere/contents/01006A800016E000/romfs/skyline";
constexpr const char* PluginsDir = "atmosphere/contents/01006A800016E000/romfs/skyline/plugins";
// Old standalone overclock sysmodule the Online Deluxe README asks to delete.
constexpr const char* OldOverclockDir = "atmosphere/contents/00FF0000A11CE0FF";

// One file to take from a repo's latest release. `source` is matched (case-insensitively)
// against a bare asset's name or a zip entry's file name.
struct FileRule {
    const char* source;
    const char* destination_dir; // relative to the SD card root
};

// Copies every zip entry whose path contains `prefix` to the same relative path on the SD card.
struct TreeRule {
    const char* prefix;
};

struct Component {
    const char* name;
    const char* owner;
    const char* repo;
    std::vector<FileRule> files;
    std::vector<TreeRule> trees;
};

// Emulator layout of saad-script/ssbu-online-deluxe scripts/create-sdcard-folder. Console-only
// parts (Horizon-OC, the ssbusync sysmodule, libssbu_oc_bridge) are deliberately left out.
const std::vector<Component>& Components() {
    static const std::vector<Component> components{
        {"ozone", "WuBoytH", "ozone",
         {{"main.npdm", ExefsDir}, {"subsdk9", ExefsDir}}, {}},
        {"ARCropolis", "raytwo", "arcropolis",
         {{"libarcropolis.nro", PluginsDir}}, {{"ultimate/arcropolis/"}}},
        {"nro-hook", "ultimate-research", "nro-hook-plugin",
         {{"libnro_hook.nro", PluginsDir}}, {}},
        {"Smashline", "HDR-Development", "smashline",
         {{"libsmashline_plugin.nro", PluginsDir}}, {}},
        {"imgui-smash", "Coolsonickirby", "imgui-smash",
         {{"libimgui_smash.nro", PluginsDir}}, {}},
        {"SSBU PIA manager", "project-ultelier", "ssbu-pia-interface",
         {{"libssbu_pia_manager.nro", PluginsDir}}, {}},
        {"ssbusync", "project-ultelier", "smash-ultelier",
         {{"libssbusync.nro", PluginsDir}}, {}},
        {"SSBU Online Deluxe", "saad-script", "ssbu-online-deluxe",
         {{"libssbu_online_deluxe.nro", PluginsDir}}, {}},
    };
    return components;
}

// Plugins that conflict with Online Deluxe (it replaces them) or only work on console.
constexpr std::array<const char*, 7> ConflictingPluginMarkers{
    "latency", "vsync", "less_lag", "lesslag", "nx_over", "oc_bridge", "less-lag",
};

std::string Lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string FileNameOf(const std::string& zip_path) {
    const auto slash = zip_path.find_last_of('/');
    return slash == std::string::npos ? zip_path : zip_path.substr(slash + 1);
}

bool WriteFileAtomically(const fs::path& path, const std::vector<u8>& data, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = fmt::format("cannot create {}: {}", path.parent_path().string(), ec.message());
        return false;
    }
    fs::path tmp = path;
    tmp += ".nxtmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
            error = fmt::format("cannot write {}", tmp.string());
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            error = fmt::format("write failed for {}", tmp.string());
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = fmt::format("cannot replace {}", path.string());
        return false;
    }
    return true;
}

// Downloads one component and fills `files` (SD-relative path -> contents).
bool FetchComponent(const Component& component, std::map<std::string, std::vector<u8>>& files,
                    std::string& version, std::string& error) {
    std::string api_error;
    const auto release =
        WebService::GitHubReleases::GetLatest(component.owner, component.repo, &api_error);
    if (!release) {
        error = api_error;
        return false;
    }
    version = release->tag;

    std::vector<bool> matched(component.files.size(), false);
    std::size_t tree_files = 0;
    const auto all_matched = [&] {
        return std::ranges::all_of(matched, [](bool m) { return m; }) &&
               (component.trees.empty() || tree_files > 0);
    };

    // Bare assets first: they are exact matches and avoid downloading archives needlessly.
    for (const auto& asset : release->assets) {
        const std::string name = Lower(asset.name);
        for (std::size_t i = 0; i < component.files.size(); ++i) {
            if (matched[i] || name != Lower(component.files[i].source)) {
                continue;
            }
            std::vector<u8> data;
            if (!WebService::GitHubReleases::Download(asset, data, &api_error)) {
                error = fmt::format("{}: {}", asset.name, api_error);
                return false;
            }
            files[fmt::format("{}/{}", component.files[i].destination_dir,
                              component.files[i].source)] = std::move(data);
            matched[i] = true;
        }
    }

    for (const auto& asset : release->assets) {
        if (all_matched()) {
            break;
        }
        if (!Lower(asset.name).ends_with(".zip")) {
            continue;
        }
        std::vector<u8> zip_bytes;
        if (!WebService::GitHubReleases::Download(asset, zip_bytes, &api_error)) {
            error = fmt::format("{}: {}", asset.name, api_error);
            return false;
        }
        std::vector<Zip::Entry> entries;
        try {
            entries = Zip::ReadAll(zip_bytes, 256ULL << 20, 10000);
        } catch (const std::exception& e) {
            error = fmt::format("{} is not a valid archive ({})", asset.name, e.what());
            return false;
        }
        for (auto& entry : entries) {
            if (entry.directory || entry.symlink || entry.encrypted ||
                entry.name.find("..") != std::string::npos || entry.name.starts_with('/')) {
                continue;
            }
            const std::string entry_name = Lower(FileNameOf(entry.name));
            for (std::size_t i = 0; i < component.files.size(); ++i) {
                if (!matched[i] && entry_name == Lower(component.files[i].source)) {
                    files[fmt::format("{}/{}", component.files[i].destination_dir,
                                      component.files[i].source)] = entry.data;
                    matched[i] = true;
                }
            }
            for (const auto& tree : component.trees) {
                const auto pos = Lower(entry.name).find(tree.prefix);
                if (pos != std::string::npos) {
                    files[entry.name.substr(pos)] = entry.data;
                    ++tree_files;
                }
            }
        }
    }

    for (std::size_t i = 0; i < component.files.size(); ++i) {
        if (!matched[i]) {
            error = fmt::format("{} not found in release {} (upstream may have renamed it)",
                                component.files[i].source, release->tag);
            return false;
        }
        if (files[fmt::format("{}/{}", component.files[i].destination_dir,
                              component.files[i].source)]
                .empty()) {
            error = fmt::format("{} in release {} is empty", component.files[i].source,
                                release->tag);
            return false;
        }
    }
    return true;
}

// Copies `from` (file or directory) under `backup_root`, keeping its SD-relative path.
bool BackupPath(const fs::path& sd_root, const std::string& relative, const fs::path& backup_root,
                std::string& error) {
    std::error_code ec;
    const fs::path from = sd_root / relative;
    if (!fs::exists(from, ec)) {
        return true;
    }
    const fs::path to = backup_root / relative;
    fs::create_directories(to.parent_path(), ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = fmt::format("cannot back up {}: {}", from.string(), ec.message());
        return false;
    }
    return true;
}

void RestoreBackup(const fs::path& sd_root, const fs::path& backup_root) {
    std::error_code ec;
    for (const char* relative : {ExefsDir, SkylineDir, OldOverclockDir}) {
        const fs::path saved = backup_root / relative;
        const fs::path live = sd_root / relative;
        fs::remove_all(live, ec);
        if (fs::exists(saved, ec)) {
            fs::create_directories(live.parent_path(), ec);
            fs::copy(saved, live, fs::copy_options::recursive, ec);
        }
    }
}

} // namespace

bool IsSupportedVersion(const std::string& display_version) {
    const auto parse = [](const std::string& text) {
        std::array<int, 3> parts{};
        std::istringstream stream{text};
        std::string part;
        for (std::size_t i = 0; i < parts.size() && std::getline(stream, part, '.'); ++i) {
            try {
                parts[i] = std::stoi(part);
            } catch (...) {
                return std::optional<std::array<int, 3>>{};
            }
        }
        return std::optional<std::array<int, 3>>{parts};
    };
    const auto have = parse(display_version);
    const auto need = parse(RequiredVersion);
    return have && need && *have >= *need;
}

bool IsLoaderInstalled() {
    std::error_code ec;
    const auto sd_root = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    return fs::exists(sd_root / ExefsDir / "subsdk9", ec);
}

Report Install() {
    Report report;
    const fs::path sd_root = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);

    // 1. Download and validate everything before touching the SD card.
    std::map<std::string, std::vector<u8>> files;
    std::vector<std::string> versions;
    for (const auto& component : Components()) {
        std::string version;
        std::string error;
        if (!FetchComponent(component, files, version, error)) {
            report.errors.push_back(fmt::format("{}: {}", component.name, error));
            continue;
        }
        versions.push_back(fmt::format("{} {}", component.name, version));
    }
    if (!report.errors.empty()) {
        LOG_ERROR(Frontend, "SSBU mods: download failed, nothing was changed");
        return report;
    }

    // 2. Back up what is about to change.
    const fs::path backup_root =
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "ssbu-mod-backups" /
        fmt::format("{}", static_cast<s64>(std::time(nullptr)));
    std::error_code ec;
    bool has_existing = false;
    for (const char* relative : {ExefsDir, SkylineDir, OldOverclockDir}) {
        has_existing |= fs::exists(sd_root / relative, ec);
    }
    if (has_existing) {
        for (const char* relative : {ExefsDir, SkylineDir, OldOverclockDir}) {
            std::string error;
            if (!BackupPath(sd_root, relative, backup_root, error)) {
                report.errors.push_back(error);
                return report;
            }
        }
        report.backup_dir = backup_root;
    }

    // 3. Clean the loader and conflicting plugins. exefs/ is replaced wholesale so an older
    //    Skyline (e.g. the one ARCropolis bundles) can't linger next to ozone.
    fs::remove_all(sd_root / ExefsDir, ec);
    fs::remove_all(sd_root / OldOverclockDir, ec);
    if (fs::is_directory(sd_root / PluginsDir, ec)) {
        for (const auto& entry : fs::directory_iterator(sd_root / PluginsDir, ec)) {
            const std::string name = Lower(entry.path().filename().string());
            const bool conflicting = std::ranges::any_of(
                ConflictingPluginMarkers,
                [&](const char* marker) { return name.find(marker) != std::string::npos; });
            if (conflicting) {
                report.removed_conflicts.push_back(entry.path().filename().string());
                fs::remove(entry.path(), ec);
            }
        }
    }

    // 4. Write. Any failure rolls back to the backup.
    for (const auto& [relative, data] : files) {
        std::string error;
        if (!WriteFileAtomically(sd_root / relative, data, error)) {
            report.errors.push_back(error);
            if (has_existing) {
                RestoreBackup(sd_root, backup_root);
            } else {
                fs::remove_all(sd_root / ExefsDir, ec);
                fs::remove_all(sd_root / SkylineDir, ec);
            }
            report.removed_conflicts.clear();
            return report;
        }
    }

    // 5. A mod under load/<title id>/ that ships its own exefs would override ozone.
    const fs::path load_dir =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "01006A800016E000";
    if (fs::is_directory(load_dir, ec)) {
        for (const auto& mod : fs::directory_iterator(load_dir, ec)) {
            if (fs::exists(mod.path() / "exefs" / "subsdk9", ec) ||
                fs::exists(mod.path() / "exefs" / "main.npdm", ec)) {
                report.warnings.push_back(fmt::format(
                    "The mod \"{}\" in the load folder replaces the game loader and will break "
                    "these mods. Disable it in the game's Add-Ons tab.",
                    mod.path().filename().string()));
            }
        }
    }

    report.installed = std::move(versions);
    report.success = true;
    LOG_INFO(Frontend, "SSBU mods installed ({} files)", files.size());
    return report;
}

} // namespace Nextendo::SsbuMods
