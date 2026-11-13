// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "web_service/github_releases.h"

#include <chrono>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"

namespace WebService::GitHubReleases {

namespace {

// The release-asset CDN rejects non-browser user agents with a fixed-size 403 page.
constexpr const char* BrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";

// "https://host/a/b" -> {"https://host", "/a/b"}
std::pair<std::string, std::string> SplitUrl(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {"", url};
    }
    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return {url, "/"};
    }
    return {url.substr(0, path_start), url.substr(path_start)};
}

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

std::optional<Release> GetLatest(const std::string& owner, const std::string& repo,
                                 std::string* error) {
    httplib::Client api{"https://api.github.com"};
    api.set_connection_timeout(15);
    api.set_read_timeout(15);
    api.set_follow_location(true);

    const std::string path = "/repos/" + owner + "/" + repo + "/releases/latest";
    const auto result = api.Get(path, httplib::Headers{{"User-Agent", BrowserUserAgent},
                                                       {"Accept", "application/vnd.github+json"}});
    if (!result) {
        SetError(error, "network error contacting GitHub (" +
                            httplib::to_string(result.error()) + ")");
        LOG_ERROR(WebService, "GitHub {}/{}: request failed: {}", owner, repo,
                  httplib::to_string(result.error()));
        return std::nullopt;
    }
    if (result->status != 200) {
        const auto remaining = result->headers.find("X-RateLimit-Remaining");
        if (result->status == 403 && remaining != result->headers.end() &&
            remaining->second == "0") {
            SetError(error, "GitHub rate limit reached, try again in an hour");
        } else {
            SetError(error, "GitHub returned HTTP " + std::to_string(result->status));
        }
        LOG_ERROR(WebService, "GitHub {}/{}: HTTP {}", owner, repo, result->status);
        return std::nullopt;
    }

    try {
        const auto json = nlohmann::json::parse(result->body);
        Release release;
        release.tag = json.value("tag_name", "");
        release.html_url = json.value("html_url", "");
        release.body = json.value("body", "");
        for (const auto& asset : json.value("assets", nlohmann::json::array())) {
            release.assets.push_back(Asset{
                .name = asset.value("name", ""),
                .download_url = asset.value("browser_download_url", ""),
                .size = asset.value("size", std::size_t{0}),
            });
        }
        return release;
    } catch (const nlohmann::json::exception& e) {
        SetError(error, std::string{"invalid GitHub response: "} + e.what());
        return std::nullopt;
    }
}

bool Download(const Asset& asset, std::vector<u8>& out, std::string* error,
              const ProgressCallback& progress) {
    const auto [host, path] = SplitUrl(asset.download_url);
    if (host.empty()) {
        SetError(error, "malformed download URL");
        return false;
    }

    constexpr int MaxAttempts = 5;
    for (int attempt = 0; attempt < MaxAttempts; ++attempt) {
        std::string body;
        bool cancelled = false;
        httplib::Client client{host};
        client.set_connection_timeout(30);
        client.set_read_timeout(60);
        client.set_follow_location(true);

        const auto result = client.Get(
            path, httplib::Headers{{"User-Agent", BrowserUserAgent}, {"Accept", "*/*"}},
            [&](const char* data, std::size_t length) {
                body.append(data, length);
                return true;
            },
            [&](u64 current, u64 total) {
                if (progress && !progress(static_cast<std::size_t>(current),
                                          static_cast<std::size_t>(total ? total : asset.size))) {
                    cancelled = true;
                    return false;
                }
                return true;
            });

        if (cancelled) {
            SetError(error, "cancelled");
            return false;
        }
        const bool size_ok = asset.size == 0 || body.size() == asset.size;
        if (result && result->status == 200 && size_ok) {
            out.assign(body.begin(), body.end());
            return true;
        }

        LOG_WARNING(WebService, "Download {} attempt {} failed (HTTP {}, {} of {} bytes)",
                    asset.name, attempt + 1, result ? result->status : 0, body.size(),
                    asset.size);
        if (attempt + 1 == MaxAttempts) {
            if (result && result->status == 200 && !size_ok) {
                SetError(error, "download was truncated (" + std::to_string(body.size()) +
                                    " of " + std::to_string(asset.size) + " bytes)");
            } else {
                SetError(error, "download failed (HTTP " +
                                    std::to_string(result ? result->status : 0) + ")");
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 << attempt));
    }
    return false;
}

} // namespace WebService::GitHubReleases
