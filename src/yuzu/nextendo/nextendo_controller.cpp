// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include <QByteArray>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QProcess>
#include <QUrl>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/nextendo_compatible_titles.h"
#include "common/nextendo_friends.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/friend/friend.h"
#include "core/hle/service/olsc/nextendo_island.h"
#include "yuzu/nextendo/nextendo_controller.h"
#include "yuzu/nextendo/nextendo_game_invites.h"
#include "yuzu/nextendo/nextendo_save_sync.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/game_catalog.h"
#include "web_service/nextendo_api.h"
#include "web_service/npln_patch_catalog.h"
#endif

NextendoController::NextendoController(Core::System& system_, QObject* parent)
    : QObject(parent), system(system_), game_invites(new NextendoGameInvites(system_, this)) {
#ifdef ENABLE_WEB_SERVICE
    WebService::NplnPatchCatalog::StartTitleIndexCheck();
    WebService::GameCatalog::Start();
    Common::NextendoFriends::SetNameResolver(&WebService::NextendoApi::GetNameByPid);
#endif
    friend_poll_timer.setInterval(20000);
    connect(&friend_poll_timer, &QTimer::timeout, this, &NextendoController::PollFriends);
    friend_poll_timer.start();

    PollFriends();

    // Covers the "already linked, emulator just relaunched" case.
    if (Common::NextendoAccount::IsLinked()) {
        ApplyProfileName(Common::NextendoAccount::GetUsername());
        SyncProfileAvatar();
    }

    // The name saved at sign-in is only a cache: re-read it now, then every 5 minutes, so the
    // profile the games read (nn::account::GetNickname) follows the account pseudo.
    SyncProfileName();
    profile_name_timer.setInterval(5 * 60 * 1000);
    connect(&profile_name_timer, &QTimer::timeout, this, &NextendoController::SyncProfileName);
    profile_name_timer.start();
}

NextendoController::~NextendoController() = default;

bool NextendoController::IsLinked() const {
    return Common::NextendoAccount::IsLinked();
}

QString NextendoController::ResolveGameName(const std::string& app_id_hex,
                                            const std::string& hint_name) const {
    if (app_id_hex.empty()) {
        return {};
    }

    u64 program_id = 0;
    try {
        program_id = std::stoull(app_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (program_id == 0) {
        return {};
    }

    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto nacp = pm.GetControlMetadata().first;
    if (nacp) {
        const auto name = nacp->GetApplicationName();
        if (!name.empty()) {
            return QString::fromStdString(name);
        }
    }
    if (!hint_name.empty()) {
        return QString::fromStdString(hint_name);
    }
    return tr("a game");
}

QString NextendoController::ResolveGameIcon(const std::string& app_id_hex) const {
    if (app_id_hex.empty()) {
        return {};
    }

    u64 program_id = 0;
    try {
        program_id = std::stoull(app_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (program_id == 0) {
        return {};
    }

    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto icon_file = pm.GetControlMetadata().second;
    if (!icon_file) {
        return {};
    }
    const std::vector<u8> icon_bytes = icon_file->ReadAllBytes();
    if (icon_bytes.empty()) {
        return {};
    }
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(icon_bytes.data()),
                                          static_cast<int>(icon_bytes.size()))
                                   .toBase64());
}

std::string NextendoController::GetLocalAppId() const {
    if (!system.IsPoweredOn()) {
        return {};
    }
    return fmt::format("{:016X}", system.GetApplicationProcessProgramID());
}

void NextendoController::SignIn() {
#ifdef ENABLE_WEB_SERVICE
    emit StatusChanged(tr("Finish signing in in your browser, then come back here."));

    QPointer<NextendoController> self(this);
    std::thread{[this, self] {
        const auto open_url = [this, self](const std::string& url) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, url] {
#ifdef __linux__
                    // xdg-desktop-portal can report success without a browser ever appearing.
                    qint64 pid = -1;
                    const bool ok = QProcess::startDetached(
                        QStringLiteral("xdg-open"), {QString::fromStdString(url)}, QString(), &pid);
                    LOG_INFO(Frontend, "NextendoController::SignIn: xdg-open -> ok={} pid={}", ok,
                             pid);
#else
                    const bool ok = QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
                    LOG_INFO(Frontend,
                             "NextendoController::SignIn: QDesktopServices::openUrl -> {}", ok);
#endif
                    emit SignInUrlReady(QString::fromStdString(url));
                },
                Qt::QueuedConnection);
        };

        auto login_result = WebService::NextendoApi::SignInWithBrowser(open_url);

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, self, result = std::move(login_result)] {
                if (!self) {
                    return;
                }
                if (!result.ok) {
                    emit StatusChanged(QString::fromStdString(result.error));
                    emit SignInFinished();
                    return;
                }

                Common::NextendoAccount::Save(result.pid, result.username, result.friend_code,
                                              result.token);
                ApplyProfileName(result.username);
                SyncProfileAvatar();
                Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);
                first_poll = true;
                emit AccountLinked();
                emit SignInFinished();
                emit StatusChanged(tr("Signed in to Nextendo Network as %1.")
                                       .arg(QString::fromStdString(result.username)));
                RefreshFriendCache();
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    emit StatusChanged(tr("This build has no web services support."));
#endif
}

void NextendoController::SignOut() {
    Common::NextendoAccount::Clear();
    Common::NextendoFriends::Set({});
    last_known_status.clear();
    offline_streak.clear();
    last_known_requests.clear();
    emit AccountUnlinked();
    emit StatusChanged(tr("Signed out of Nextendo Network."));
}

void NextendoController::ManualSaveDownload(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    // Writing into the save directory while a live session has it mounted crashes it.
    if (system.IsPoweredOn()) {
        emit StatusChanged(tr("Stop the running game before downloading a cloud save."));
        return;
    }
    if (Service::OLSC::NextendoIsland::IsStrict(title_id)) {
        emit StatusChanged(tr("Your island is restored from the game: press - on the title "
                              "screen and open the island backup settings."));
        return;
    }
    if (!Nextendo::CompatibleTitles::Table().count(title_id)) {
        emit StatusChanged(tr("This game doesn't support cloud saves."));
        return;
    }

    emit StatusChanged(tr("Downloading save from the cloud..."));
    QPointer<NextendoController> self(this);
    std::thread{[this, self, title_id] {
        Nextendo::SaveSync::Pull(system, title_id, /*force=*/true);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, self] {
                if (!self) {
                    return;
                }
                emit StatusChanged(tr("Cloud save applied."));
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    emit StatusChanged(tr("This build has no web services support."));
#endif
}

void NextendoController::QuickStart(u64 title_id) {
    emit QuickStartRequested(title_id);
}

void NextendoController::ApplyProfileName(const std::string& name) {
    if (name.empty()) {
        return;
    }

    // Must be the live ProfileManager every game reads from, not a throwaway copy.
    auto& profile_manager = system.GetProfileManager();
    const auto uuid = profile_manager.GetLastOpenedUser();
    if (uuid.IsInvalid()) {
        return;
    }

    Service::Account::ProfileBase profile{};
    if (!profile_manager.GetProfileBase(uuid, profile)) {
        return;
    }

    const std::string trimmed = name.substr(0, profile.username.size() - 1);
    const std::string current(reinterpret_cast<const char*>(profile.username.data()),
                              strnlen(reinterpret_cast<const char*>(profile.username.data()),
                                      profile.username.size()));
    if (current == trimmed) {
        return; // already up to date: no save file rewrite every 5 minutes
    }
    std::fill(profile.username.begin(), profile.username.end(), '\0');
    std::copy(trimmed.begin(), trimmed.end(), profile.username.begin());

    profile_manager.SetProfileBase(uuid, profile);
    profile_manager.WriteUserSaveFile();
    LOG_INFO(Frontend, "[Nextendo] Renamed the active profile to the account nickname");
}

void NextendoController::SyncProfileName() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::NextendoAccount::IsLinked()) {
        return;
    }
    const u64 pid = Common::NextendoAccount::GetPid();
    std::thread{[this, pid, guard = QPointer<NextendoController>(this)] {
        const std::string name = WebService::NextendoApi::GetNameByPid(pid);
        if (name.empty() || !guard) {
            return; // out of reach: keep the profile as it is
        }
        QMetaObject::invokeMethod(
            this,
            [this, guard, pid, name] {
                if (!guard || !Common::NextendoAccount::IsLinked() ||
                    Common::NextendoAccount::GetPid() != pid) {
                    return; // signed out or switched account while the request was in flight
                }
                if (name != Common::NextendoAccount::GetUsername()) {
                    Common::NextendoAccount::Save(pid, name,
                                                  Common::NextendoAccount::GetFriendCode(),
                                                  Common::NextendoAccount::GetToken());
                    LOG_INFO(Frontend, "[Nextendo] Account pseudo changed on the server");
                }
                ApplyProfileName(name);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoController::SyncProfileAvatar() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::NextendoAccount::IsLinked()) {
        return;
    }
    auto& profile_manager = system.GetProfileManager();
    const auto uuid = profile_manager.GetLastOpenedUser();
    if (uuid.IsInvalid()) {
        return;
    }
    const u64 pid = Common::NextendoAccount::GetPid();
    std::thread{[this, uuid, pid, guard = QPointer<NextendoController>(this)] {
        const std::string b64 = WebService::NextendoApi::GetAvatarByPid(pid);
        if (b64.empty() || !guard) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, guard, uuid, b64] {
                if (!guard) {
                    return;
                }
                WriteProfileAvatar(uuid, b64);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoController::WriteProfileAvatar(const Common::UUID& uuid,
                                            const std::string& avatar_b64) {
    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(avatar_b64)))) {
        return;
    }
    if (image.width() != 256 || image.height() != 256) {
        image = image.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

    const auto image_path = QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
        fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uuid.FormattedString())));

    QDir{}.mkpath(QFileInfo(image_path).absolutePath());
    if (image.save(image_path, "JPEG")) {
        LOG_INFO(Frontend, "[Nextendo] Synced the active profile's avatar from the account");
    }
}

void NextendoController::RefreshFriendCache() {
    PollFriends();
}

void NextendoController::NotifyFriendRequestSent(const QString& friend_code) {
    emit FriendRequestSent(friend_code);
}

void NextendoController::PollFriends() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::NextendoAccount::IsLinked()) {
        return;
    }

    QPointer<NextendoController> self(this);
    std::thread{[this, self] {
        auto fetched = WebService::NextendoApi::GetFriends();
        if (!fetched.ok || !self) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this, self, list = std::move(fetched)] {
                if (!self) {
                    return;
                }
                std::vector<Common::NextendoFriends::Entry> cache;
                cache.reserve(list.friends.size());
                for (const auto& entry : list.friends) {
                    const auto decoded_image =
                        QByteArray::fromBase64(QByteArray::fromStdString(entry.image_base64));
                    u64 account_id = 0;
                    if (!entry.account_hex.empty()) {
                        account_id = std::strtoull(entry.account_hex.c_str(), nullptr, 16);
                    }
                    cache.push_back({entry.pid, entry.name, entry.presence_status,
                                     entry.app_field,
                                     std::vector<u8>(decoded_image.begin(), decoded_image.end()),
                                     account_id, entry.is_console,
                                     std::strtoull(entry.app_id.c_str(), nullptr, 16)});
                }
                Common::NextendoFriends::Set(std::move(cache));
                // The guest's INotificationService only signals once at construction; tell any
                // Friends viewer already on screen that real data showed up.
                Service::Friend::NotifyFriendsListUpdated();

                const bool suppress = first_poll;
                first_poll = false;

                std::map<u64, s32> current_status;
                for (const auto& entry : list.friends) {
                    const auto it = last_known_status.find(entry.pid);
                    const bool was_online = it != last_known_status.end() && it->second != 0;

                    // A single 0 can be a mid-transition blip on the server: require it twice
                    // in a row (~20 s) before treating the friend as offline.
                    if (entry.presence_status == 0 && was_online) {
                        if (++offline_streak[entry.pid] < 2) {
                            current_status[entry.pid] = it->second;
                            continue;
                        }
                        if (!suppress) {
                            emit FriendWentOffline(entry.pid, QString::fromStdString(entry.name),
                                                   QString::fromStdString(entry.image_base64));
                        }
                    } else {
                        offline_streak.erase(entry.pid);
                        const bool was_offline =
                            it == last_known_status.end() || it->second == 0;
                        if (!suppress && was_offline && entry.presence_status != 0) {
                            emit FriendCameOnline(entry.pid, QString::fromStdString(entry.name),
                                                  ResolveGameName(entry.app_id, entry.app_name),
                                                  QString::fromStdString(entry.image_base64));
                        }
                    }
                    current_status[entry.pid] = entry.presence_status;
                }
                last_known_status = std::move(current_status);

                std::set<u64> current_requests;
                for (const auto& entry : list.requests) {
                    current_requests.insert(entry.pid);
                    if (!suppress && !last_known_requests.contains(entry.pid)) {
                        emit FriendRequestReceived(entry.pid, QString::fromStdString(entry.name),
                                                   QString::fromStdString(entry.image_base64));
                    }
                }
                last_known_requests = std::move(current_requests);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}
