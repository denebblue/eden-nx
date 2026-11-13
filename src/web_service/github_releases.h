// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

// Minimal GitHub Releases client (public repos, unauthenticated). Shared by the SSBU mod
// installer and the self-updater.
namespace WebService::GitHubReleases {

struct Asset {
    std::string name;
    std::string download_url;
    std::size_t size = 0;
};

struct Release {
    std::string tag;
    std::string html_url;
    std::string body;
    std::vector<Asset> assets;
};

// GET /repos/{owner}/{repo}/releases/latest. On failure returns nullopt and fills *error.
std::optional<Release> GetLatest(const std::string& owner, const std::string& repo,
                                 std::string* error);

// Progress callback: (bytes received, total bytes). Returning false cancels the download.
using ProgressCallback = std::function<bool(std::size_t, std::size_t)>;

// Downloads an asset, following GitHub's CDN redirect, retrying transient failures and
// verifying the size GitHub reported. On failure returns false and fills *error.
bool Download(const Asset& asset, std::vector<u8>& out, std::string* error,
              const ProgressCallback& progress = {});

} // namespace WebService::GitHubReleases
