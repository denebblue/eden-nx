// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu/nextendo/nextendo_save_sync.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/nextendo_compatible_titles.h"
#include "core/core.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/olsc/nextendo_island.h"
#include "yuzu/nextendo/nextendo_zip.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Nextendo::SaveSync {

namespace {

// Eden builds a SaveDataFactory per program instead of keeping a global one.
FileSys::VirtualDir TitleSaveDirectory(Core::System& system, u64 title_id) {
    if (!system.GetFilesystem()) {
        return nullptr;
    }
    const auto factory = system.GetFileSystemController().CreateSaveDataFactory(title_id);
    return factory ? factory->GetTitleSaveDirectory(title_id) : nullptr;
}

bool IsEligible(u64 title_id) {
    // [Nextendo] The island of Animal Crossing: New Horizons is never synced here: the game backs
    // it up itself through olsc:u (Service::OLSC::NextendoIsland), one island per account.
    return !Service::OLSC::NextendoIsland::IsStrict(title_id) &&
           Nextendo::CompatibleTitles::Table().count(title_id) != 0 &&
           Common::NextendoAccount::IsLinked();
}

[[maybe_unused]] bool HasLocalContent(const FileSys::VirtualDir& dir) {
    if (!dir) {
        return false;
    }
    if (!dir->GetFiles().empty()) {
        return true;
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub && HasLocalContent(sub)) {
            return true;
        }
    }
    return false;
}

void CollectFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
                  std::vector<std::pair<std::string, std::vector<unsigned char>>>& out) {
    for (const auto& file : dir->GetFiles()) {
        if (file) {
            const std::vector<u8> bytes = file->ReadAllBytes();
            out.emplace_back(prefix + file->GetName(),
                             std::vector<unsigned char>(bytes.begin(), bytes.end()));
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub) {
            CollectFiles(sub, prefix + sub->GetName() + "/", out);
        }
    }
}

} // namespace

void Pull(Core::System& system, u64 title_id, bool force) {
#ifdef ENABLE_WEB_SERVICE
    if (!IsEligible(title_id)) {
        return;
    }

    auto save_dir = TitleSaveDirectory(system, title_id);
    if (!save_dir) {
        return;
    }
    if (!force && HasLocalContent(save_dir)) {
        LOG_INFO(Frontend, "Nextendo save pull {:016X}: local save present -> kept (no overwrite)",
                 title_id);
        return;
    }

    const auto zip = WebService::NextendoApi::PullSave(fmt::format("{:016x}", title_id));
    if (!zip || zip->empty()) {
        return;
    }

    const std::string full_path = save_dir->GetFullPath();
    const std::filesystem::path dest{std::u8string(full_path.begin(), full_path.end())};
    if (Nextendo::Zip::ExtractToDirectory(*zip, dest)) {
        LOG_INFO(Frontend, "Nextendo save pull {:016X}: applied ({} B)", title_id, zip->size());
    } else {
        LOG_WARNING(Frontend, "Nextendo save pull {:016X}: could not extract the cloud save",
                    title_id);
    }
#else
    (void)system;
    (void)title_id;
    (void)force;
#endif
}

std::vector<u8> CaptureForPush(Core::System& system, u64 title_id) {
    if (!IsEligible(title_id)) {
        return {};
    }
    auto save_dir = TitleSaveDirectory(system, title_id);
    if (!save_dir) {
        return {};
    }
    std::vector<std::pair<std::string, std::vector<unsigned char>>> files;
    CollectFiles(save_dir, "", files);
    if (files.empty()) {
        return {};
    }
    auto zip = Nextendo::Zip::Create(files);
    return std::vector<u8>(zip.begin(), zip.end());
}

void UploadCaptured(u64 title_id, std::vector<u8> zip_bytes) {
#ifdef ENABLE_WEB_SERVICE
    if (zip_bytes.empty()) {
        return;
    }
    const std::string error =
        WebService::NextendoApi::PushSave(fmt::format("{:016x}", title_id), zip_bytes);
    if (!error.empty()) {
        LOG_WARNING(Frontend, "Nextendo save push {:016X} failed: {}", title_id, error);
    } else {
        LOG_INFO(Frontend, "Nextendo save push {:016X}: {} B", title_id, zip_bytes.size());
    }
#else
    (void)title_id;
    (void)zip_bytes;
#endif
}

} // namespace Nextendo::SaveSync
