// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Nextendo Network hooks of MainWindow, kept out of main_window.cpp so that file only carries
// one-line calls into here. Ported from Citron Nextendo's GMainWindow (src/citron/main.cpp).

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <fmt/format.h>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "common/nextendo_friends.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/olsc/nextendo_island.h"
#include "core/loader/loader.h"
#include "frontend_common/play_time_manager.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "ui_main.h"
#include "yuzu/game/game_list.h"
#include "yuzu/game/game_list_p.h"
#include "yuzu/main_window.h"
#include "yuzu/nextendo/nextendo_account_dialog.h"
#include "yuzu/nextendo/nextendo_bcat_titles.h"
#include "yuzu/nextendo/nextendo_controller.h"
#include "yuzu/nextendo/nextendo_game_invites.h"
#include "yuzu/nextendo/nextendo_package_bcat.h"
#include "yuzu/nextendo/nextendo_save_sync.h"
#include "yuzu/nextendo/nextendo_splatoon3_bcat.h"
#include "yuzu/nextendo/nextendo_toast.h"
#include "yuzu/nextendo/nextendo_zip.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#include "web_service/npln_patch_catalog.h"
#endif

namespace {

using namespace Nextendo::BcatTitles;
constexpr u64 kSplatoon2Eu = Splatoon2Eu;
constexpr u64 kSplatoon2Us = Splatoon2Us;
constexpr u64 kSplatoon2Jp = Splatoon2Jp;
constexpr u64 kSplatoon3 = Splatoon3;
static_assert(kSplatoon3 == Nextendo::Splatoon3Bcat::TitleId);
constexpr u64 kPokemonViolet = PokemonViolet;
constexpr u64 kPokemonScarlet = PokemonScarlet;

bool IsNextendoPackageBcatTitle(u64 title_id) {
    return IsPackage(title_id);
}

std::filesystem::path BcatDir(const std::string& title_id_hex) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
           fmt::format("system/save/bcat/{}", title_id_hex);
}

std::filesystem::path ByamlSkipPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nextendo_byaml_skip.txt";
}

[[maybe_unused]] std::filesystem::path NextendoByamlHashPath(const std::string& title_id_hex) {
    return BcatDir(title_id_hex) / ".nextendo_bcat_hash";
}

[[maybe_unused]] std::string NextendoByamlReadHash(const std::string& title_id_hex) {
    std::ifstream file{NextendoByamlHashPath(title_id_hex)};
    std::string value;
    std::getline(file, value);
    return value;
}

[[maybe_unused]] void NextendoByamlWriteHash(const std::string& title_id_hex, const std::string& value) {
    if (value.empty()) {
        return;
    }
    std::ofstream file{NextendoByamlHashPath(title_id_hex)};
    if (file) {
        file << value;
    }
}

// Visits every game of the game list, whether it is shown as a tree (games under folders) or
// flat (grid mode).
template <typename Fn>
void ForEachGame(QStandardItemModel* model, const Fn& fn) {
    const auto visit = [&fn](QStandardItem* item) {
        if (item && item->data(GameListItem::TypeRole).value<GameListItemType>() ==
                        GameListItemType::Game) {
            fn(item);
        }
    };
    for (int i = 0; i < model->rowCount(); ++i) {
        QStandardItem* top = model->item(i, 0);
        if (!top) {
            continue;
        }
        visit(top);
        for (int j = 0; j < top->rowCount(); ++j) {
            visit(top->child(j, 0));
        }
    }
}

} // Anonymous namespace

// ---------------------------------------------------------------------------------------------
// Setup

void MainWindow::NextendoInitialize() {
    nextendo_controller = new NextendoController(*QtCommon::system, this);
    // [Nextendo] Island uploads left pending (emulator closed during or before the upload).
    Service::OLSC::NextendoIsland::RunPendingInBackground();
    nextendo_toast = new NextendoToast(this);

    // Splatoon 2's schedule: the content provider isn't populated yet at this point of boot, so
    // just attempt the 3 known title IDs; a decline ("Don't ask again") makes this a no-op.
    // The download itself compares hashes, so an up-to-date copy costs one cheap request.
    for (const u64 title_id : {kSplatoon2Eu, kSplatoon2Us, kSplatoon2Jp}) {
        if (!NextendoByamlSkipped(title_id)) {
            SilentlyDownloadNextendoByaml(title_id);
        }
    }
}

void MainWindow::NextendoConnectMenu() {
    // Menu built in code (not main.ui) to keep the Nextendo port self-contained.
    nextendo_menu = new QMenu(tr("Nex&Tendo"), this);
    ui->menubar->insertMenu(ui->menu_Help->menuAction(), nextendo_menu);

    nextendo_action_open_account = nextendo_menu->addAction(tr("&Open Account Page"));
    nextendo_action_sign_in = nextendo_menu->addAction(tr("&Sign In"));
    nextendo_action_sign_out = nextendo_menu->addAction(tr("Sign &Out"));
    nextendo_menu->addSeparator();
    nextendo_action_redirection = nextendo_menu->addAction(tr("&Enable Network Redirection"));
    nextendo_action_redirection->setCheckable(true);
    nextendo_action_cloud_sync = nextendo_menu->addAction(tr("Automatic &Cloud Save Sync"));
    nextendo_action_cloud_sync->setCheckable(true);
    nextendo_action_notifications = nextendo_menu->addAction(tr("Show Friend &Notifications"));
    nextendo_action_notifications->setCheckable(true);

    const auto refresh_link_state = [this] {
        const bool linked = Common::NextendoAccount::IsLinked();
        nextendo_action_sign_in->setEnabled(!linked);
        nextendo_action_sign_out->setEnabled(linked);
    };
    refresh_link_state();
    // Refreshed every time the menu opens: values may have changed elsewhere (account dialog).
    connect(nextendo_menu, &QMenu::aboutToShow, this, [this, refresh_link_state] {
        refresh_link_state();
        nextendo_action_redirection->setChecked(Settings::values.enable_nextendo.GetValue());
        nextendo_action_cloud_sync->setChecked(
            Settings::values.nextendo_cloud_sync_enabled.GetValue());
        nextendo_action_notifications->setChecked(
            UISettings::values.nextendo_notifications_enabled.GetValue());
    });
    nextendo_action_redirection->setChecked(Settings::values.enable_nextendo.GetValue());
    nextendo_action_cloud_sync->setChecked(Settings::values.nextendo_cloud_sync_enabled.GetValue());
    nextendo_action_notifications->setChecked(
        UISettings::values.nextendo_notifications_enabled.GetValue());

    connect(nextendo_action_open_account, &QAction::triggered, this, [this] {
        if (!Common::NextendoAccount::IsLinked()) {
            nextendo_controller->SignIn();
            return;
        }
        NextendoOpenAccountPage(NextendoAccountDialog::kHomePage);
    });
    connect(nextendo_action_sign_in, &QAction::triggered, nextendo_controller,
            &NextendoController::SignIn);
    connect(nextendo_action_sign_out, &QAction::triggered, this, [this] {
        if (QMessageBox::question(this, tr("Nextendo Network"),
                                  tr("Sign out of Nextendo Network?")) == QMessageBox::Yes) {
            nextendo_controller->SignOut();
        }
    });
    // enable_nextendo is read by the core when a game starts: saved right away so a crash
    // doesn't lose it.
    connect(nextendo_action_redirection, &QAction::toggled, this, [this](bool checked) {
        Settings::values.enable_nextendo.SetValue(checked);
        OnSaveConfig();
        if (emulation_running) {
            statusBar()->showMessage(tr("Restart the game for the redirection change to apply."),
                                     8000);
        }
    });
    connect(nextendo_action_cloud_sync, &QAction::toggled, this, [this](bool checked) {
        Settings::values.nextendo_cloud_sync_enabled.SetValue(checked);
        OnSaveConfig();
    });
    connect(nextendo_action_notifications, &QAction::toggled, this, [this](bool checked) {
        UISettings::values.nextendo_notifications_enabled.SetValue(checked);
        OnSaveConfig();
    });

    connect(nextendo_controller, &NextendoController::AccountLinked, this, refresh_link_state);
    connect(nextendo_controller, &NextendoController::AccountUnlinked, this, refresh_link_state);

    // The OS may report success without a browser ever appearing: give a manual fallback.
    connect(nextendo_controller, &NextendoController::SignInUrlReady, this, [this](QString url) {
        if (!nextendo_signin_dialog) {
            nextendo_signin_dialog = new QDialog(this);
            nextendo_signin_dialog->setWindowTitle(tr("Sign in to Nextendo"));
            auto* layout = new QVBoxLayout(nextendo_signin_dialog);
            auto* label = new QLabel(tr("Finish signing in in your browser, then come back here.\n"
                                        "If nothing opened, copy this link into any browser:"));
            label->setWordWrap(true);
            auto* url_field = new QLineEdit;
            url_field->setReadOnly(true);
            url_field->setObjectName(QStringLiteral("nextendo_signin_url"));
            auto* button_row = new QHBoxLayout;
            auto* copy_button = new QPushButton(tr("Copy Link"));
            auto* open_button = new QPushButton(tr("Open in Browser"));
            auto* close_button = new QPushButton(tr("Close"));
            button_row->addWidget(copy_button);
            button_row->addWidget(open_button);
            button_row->addStretch(1);
            button_row->addWidget(close_button);
            layout->addWidget(label);
            layout->addWidget(url_field);
            layout->addLayout(button_row);
            connect(copy_button, &QPushButton::clicked, nextendo_signin_dialog, [url_field] {
                QGuiApplication::clipboard()->setText(url_field->text());
            });
            connect(open_button, &QPushButton::clicked, nextendo_signin_dialog, [url_field] {
                QDesktopServices::openUrl(QUrl(url_field->text()));
            });
            connect(close_button, &QPushButton::clicked, nextendo_signin_dialog, &QDialog::close);
            nextendo_signin_dialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(nextendo_signin_dialog, &QObject::destroyed, this,
                    [this] { nextendo_signin_dialog = nullptr; });
        }
        if (auto* url_field = nextendo_signin_dialog->findChild<QLineEdit*>(
                QStringLiteral("nextendo_signin_url"))) {
            url_field->setText(url);
            url_field->selectAll();
        }
        nextendo_signin_dialog->show();
        nextendo_signin_dialog->raise();
        nextendo_signin_dialog->activateWindow();
    });
    connect(nextendo_controller, &NextendoController::SignInFinished, this, [this] {
        if (nextendo_signin_dialog) {
            nextendo_signin_dialog->close();
        }
    });
    connect(nextendo_controller, &NextendoController::StatusChanged, this,
            [this](const QString& message) {
                if (message.isEmpty()) {
                    return;
                }
                statusBar()->showMessage(message, 8000);
                // The status bar is often hidden in Eden: also surface it as a notification,
                // unless the account page is open (it shows the message itself).
                if (!nextendo_account_dialog_instance) {
                    nextendo_toast->Show(tr("Nextendo Network"), message,
                                         NextendoToast::Kind::Info);
                }
            });

    // Notifications.
    connect(nextendo_controller, &NextendoController::FriendCameOnline, this,
            [this](u64, const QString& name, const QString& game_name, const QString&) {
                nextendo_toast->Show(name,
                                     game_name.isEmpty() ? tr("is now online")
                                                         : tr("is now playing %1").arg(game_name),
                                     NextendoToast::Kind::Online);
            });
    connect(nextendo_controller, &NextendoController::FriendWentOffline, this,
            [this](u64, const QString& name, const QString&) {
                nextendo_toast->Show(name, tr("is now offline"), NextendoToast::Kind::Offline);
            });
    connect(nextendo_controller, &NextendoController::FriendRequestReceived, this,
            [this](u64, const QString& name, const QString&) {
                nextendo_toast->Show(name, tr("sent you a friend request"),
                                     NextendoToast::Kind::Request);
                statusBar()->showMessage(tr("%1 sent you a friend request").arg(name), 8000);
            });
    connect(nextendo_controller, &NextendoController::FriendRequestSent, this,
            [this](const QString& friend_code) {
                nextendo_toast->Show(tr("Friend Request Sent!"), friend_code,
                                     NextendoToast::Kind::RequestSent);
            });
    connect(nextendo_toast, &NextendoToast::clicked, this, [this](NextendoToast::Kind kind) {
        if (Common::NextendoAccount::IsLinked() && kind == NextendoToast::Kind::Request) {
            NextendoOpenAccountPage(NextendoAccountDialog::kFriendsPage);
        }
    });

    // Game invitations: only those for the game running right now pop up.
    auto* game_invites = nextendo_controller->GameInvites();
    connect(game_invites, &NextendoGameInvites::Received, this,
            [this](const QString& id, const QString& name, u64) {
                const QString game =
                    nextendo_controller->ResolveGameName(nextendo_controller->GetLocalAppId());
                nextendo_toast->ShowInvite(name, tr("invited you to play %1").arg(game), id);
                statusBar()->showMessage(tr("%1 invited you to play %2").arg(name, game), 10000);
            });
    connect(nextendo_toast, &NextendoToast::inviteAnswered, this,
            [this, game_invites](const QString& id, bool accepted) {
                if (!accepted) {
                    game_invites->Decline(id);
                } else if (const QString error = game_invites->Accept(id); !error.isEmpty()) {
                    statusBar()->showMessage(error, 8000);
                    nextendo_toast->Show(tr("Nextendo Network"), error, NextendoToast::Kind::Info);
                }
            });

    // Quick Start (Recently Played page).
    connect(nextendo_controller, &NextendoController::QuickStartRequested, this,
            [this](u64 title_id) {
                const QString path = NextendoGamePath(title_id);
                if (path.isEmpty()) {
                    QMessageBox::information(this, tr("Quick Start"),
                                             tr("This game isn't in your game list."));
                    return;
                }
                if (emulation_running) {
                    QMessageBox::information(this, tr("Quick Start"),
                                             tr("Stop the running game first."));
                    return;
                }
                BootGameFromList(path, StartGameType::Normal);
            });

    // Presence: pushed every 5 s when it changed (or periodically to keep the server TTL alive).
    nextendo_presence_timer.setInterval(5000);
    connect(&nextendo_presence_timer, &QTimer::timeout, this, [this] {
#ifdef ENABLE_WEB_SERVICE
        if (!Common::NextendoAccount::IsLinked()) {
            return;
        }
        const std::string app_id =
            emulation_running ? fmt::format("{:016X}", nextendo_playtime_program_id)
                              : std::string{};
        const std::string app_name = emulation_running ? nextendo_game_name : std::string{};
        const bool app_id_changed = app_id != nextendo_last_pushed_app_id;

        s32 status = 0;
        std::string app_field;
        const bool have_update =
            Common::NextendoFriends::TakeLocalPresenceForPublish(status, app_field);
        if (!have_update && !app_id_changed) {
            return;
        }
        if (!have_update) {
            status = Common::NextendoFriends::GetLocalStatus();
            app_field = Common::NextendoFriends::GetLocalAppField();
        }

        nextendo_last_pushed_app_id = app_id;
        std::thread{[status, app_field, app_id, app_name] {
            WebService::NextendoApi::PushPresence(status, app_field, app_id, app_name);
        }}.detach();
#endif
    });
    nextendo_presence_timer.start();

    // BCAT: once the game list knows what is installed, fetch missing schedules/packages.
    connect(game_list, &GameList::PopulatingCompleted, this, [this] {
        if (nextendo_bcat_autodownload_done) {
            return;
        }
        nextendo_bcat_autodownload_done = true;
        ForEachGame(game_list->GetModel(), [this](QStandardItem* game) {
            const u64 title_id = game->data(GameListItemPath::ProgramIdRole).toULongLong();
            if (title_id != 0 && NextendoByamlRequired(title_id) &&
                !NextendoByamlInstalled(title_id) && !NextendoByamlSkipped(title_id)) {
                LOG_INFO(Frontend, "Nextendo BCAT: auto-downloading for {:016X}", title_id);
                SilentlyDownloadNextendoByaml(title_id);
            }
        });
    });
    connect(game_list, &GameList::DownloadNextendoScheduleRequested, this,
            &MainWindow::NextendoByamlDownloadFromMenu);
}

void MainWindow::NextendoOpenAccountPage(int page) {
    if (!Common::NextendoAccount::IsLinked()) {
        nextendo_controller->SignIn();
        return;
    }
    if (nextendo_account_dialog_instance) {
        nextendo_account_dialog_instance->raise();
        nextendo_account_dialog_instance->activateWindow();
        return;
    }
    NextendoAccountDialog dialog(nextendo_controller, *QtCommon::system, this, page);
    nextendo_account_dialog_instance = &dialog;
    dialog.exec();
    nextendo_account_dialog_instance = nullptr;
    OnSaveConfig(); // cloud sync / notification toggles live in the dialog too
}

QString MainWindow::NextendoGamePath(u64 title_id) const {
    QString path;
    ForEachGame(game_list->GetModel(), [&path, title_id](QStandardItem* game) {
        if (path.isEmpty() &&
            game->data(GameListItemPath::ProgramIdRole).toULongLong() == title_id) {
            path = game->data(GameListItemPath::FullPathRole).toString();
        }
    });
    return path;
}

// ---------------------------------------------------------------------------------------------
// Boot / stop hooks

void MainWindow::NextendoPrepareOnlinePatches(const QString& filename,
                                              const Service::AM::FrontendAppletParameters& params) {
#ifdef ENABLE_WEB_SERVICE
    // Prepare after the previous session has stopped and before any new NSO is loaded. The worker
    // finishes before loading the ROM, so every module sees the same snapshot.
    WebService::NplnPatchCatalog::EndGame();

    QProgressDialog progress(tr("Checking online patches..."), QString{}, 0, 0, this);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);

    QFutureWatcher<void> watcher;
    connect(&watcher, &QFutureWatcher<void>::finished, &progress, &QDialog::accept);
    const auto file = Core::GetGameFileFromPath(QtCommon::vfs, filename.toUtf8().constData());
    auto loader = Loader::GetLoader(*QtCommon::system, file, params.program_id,
                                    static_cast<std::size_t>(params.program_index));
    auto future = QtConcurrent::run([&loader, title_id = nextendo_title_id] {
        WebService::NplnPatchCatalog::WaitForTitleIndex();
        if (!WebService::NplnPatchCatalog::IsCompatibleTitle(title_id)) {
            return;
        }
        std::array<u8, 0x20> build_id{};
        if (!loader || loader->ReadMainBuildId(*QtCommon::system, build_id) !=
                           Loader::ResultStatus::Success) {
            LOG_WARNING(Frontend, "[Nextendo] Could not identify main build for online patch");
            return;
        }
        WebService::NplnPatchCatalog::PrepareForGame(title_id, Common::HexToString(build_id));
    });
    watcher.setFuture(future);
    if (!future.isFinished()) {
        progress.exec();
    }
    future.waitForFinished();
#else
    (void)filename;
    (void)params;
#endif
}

void MainWindow::NextendoLoadFailed() {
#ifdef ENABLE_WEB_SERVICE
    WebService::NplnPatchCatalog::EndGame();
#endif
}

bool MainWindow::NextendoBeforeBoot(u64 title_id) {
    nextendo_title_id = title_id;

    // [Nextendo] Splatoon 3 refuses to boot with any mod OR cheat active on the production
    // server (same rule as Ryujinx-Nextendo): online play is against real people and the server
    // can't contradict a modified client. Updates/DLC are unaffected.
    const bool production_active =
        Settings::values.enable_nextendo.GetValue() &&
        Settings::values.nextendo_server_ip.GetValue() ==
            Settings::values.nextendo_server_ip.GetDefault();
    if (title_id == kSplatoon3 && production_active) {
        const FileSys::PatchManager pm{title_id, QtCommon::system->GetFileSystemController(),
                                       QtCommon::system->GetContentProvider()};
        QStringList active_mods;
        for (const auto& patch : pm.GetPatches()) {
            if (patch.enabled && patch.type == FileSys::PatchType::Mod) {
                active_mods.push_back(QString::fromStdString(patch.name));
            }
        }
        // Eden lists cheats as Mod patches (there is no separate GetCheats() like Citron's),
        // so the loop above already covers enabled cheat folders.
        if (!active_mods.isEmpty()) {
            LOG_CRITICAL(Frontend,
                         "[Nextendo] Refusing to boot Splatoon 3: {} mod(s)/cheat(s) enabled",
                         active_mods.size());
            QMessageBox::critical(
                this, tr("Splatoon 3: mods must be disabled"),
                tr("Splatoon 3 cannot be launched while any mod or cheat is enabled:\n\n%1\n\n"
                   "Disable them in the game's properties and try again. "
                   "Updates and DLC are not affected.")
                    .arg(active_mods.join(QStringLiteral("\n"))));
            return false;
        }
    }

    OfferNextendoByamlDownload(title_id);
    if (Settings::values.nextendo_cloud_sync_enabled.GetValue()) {
        // The save directory lookup goes through the system's filesystem.
        QtCommon::system->SetFilesystem(QtCommon::vfs);
        Nextendo::SaveSync::Pull(*QtCommon::system, title_id);
    }
    return true;
}

void MainWindow::NextendoApplyPerGameWorkarounds(u64 title_id) {
    // TEMPORARY (same as Citron Nextendo): resolution scale above 1x doubles Nextendo online
    // points in Splatoon 2 and 3. Clamp to 1x until the scaling bug is fixed.
    if (IsSplatoon2(title_id) || title_id == kSplatoon3) {
        LOG_INFO(Frontend, "[Nextendo] Clamping resolution to 1x for Splatoon (online points)");
        Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
    }
}

void MainWindow::NextendoOnGameBooted(const std::string& title_name) {
    nextendo_game_name = title_name;
    nextendo_game_icon_base64.clear();
    std::vector<u8> icon_bytes;
    if (QtCommon::system->GetAppLoader().ReadIcon(icon_bytes) == Loader::ResultStatus::Success &&
        !icon_bytes.empty()) {
        nextendo_game_icon_base64 = QByteArray(reinterpret_cast<const char*>(icon_bytes.data()),
                                               static_cast<int>(icon_bytes.size()))
                                        .toBase64()
                                        .toStdString();
    }
    // [Nextendo] The island upload of Animal Crossing: New Horizons never runs while it plays.
    Service::OLSC::NextendoIsland::SetRunningTitle(nextendo_title_id);
}

void MainWindow::NextendoOnStartGame() {
    nextendo_playtime_program_id = QtCommon::system->GetApplicationProcessProgramID();
    Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnlinePlay);
}

void MainWindow::NextendoOnEmulationStopped() {
    // Every shutdown path lands in OnEmulationStopped; flush the play time first.
    play_time_manager->Stop();
    SyncNextendoHistory();
    Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);

#ifdef ENABLE_WEB_SERVICE
    WebService::NplnPatchCatalog::EndGame();

    // emu_thread has fully exited here: no more concurrent guest access to the save data.
    if (Settings::values.nextendo_cloud_sync_enabled.GetValue() && nextendo_title_id != 0) {
        auto save_zip = Nextendo::SaveSync::CaptureForPush(*QtCommon::system, nextendo_title_id);
        if (!save_zip.empty()) {
            std::thread{[title_id = nextendo_title_id, zip = std::move(save_zip)]() mutable {
                Nextendo::SaveSync::UploadCaptured(title_id, std::move(zip));
            }}.detach();
        }
    }
#endif

    // [Nextendo] Island of Animal Crossing: New Horizons: the backup enabled in the game is sent
    // after the game, without any window. When the window is closing it is only marked pending
    // and the next launch sends it.
    Service::OLSC::NextendoIsland::SetRunningTitle(0);
    Service::OLSC::NextendoIsland::AfterGame(nextendo_title_id, !nextendo_main_window_closing);

    nextendo_playtime_program_id = 0;
    nextendo_game_name.clear();
    nextendo_game_icon_base64.clear();
}

void MainWindow::NextendoOnClose() {
    nextendo_main_window_closing = true;
}

void MainWindow::NextendoAfterShutdownOnClose() {
    // [Nextendo] An island upload already running ends first (60 s at most), like Ryujinx.
    Service::OLSC::NextendoIsland::WaitIdle(60000);
}

void MainWindow::SyncNextendoHistory() {
#ifdef ENABLE_WEB_SERVICE
    const bool linked = Common::NextendoAccount::IsLinked();
    const u64 program_id = nextendo_playtime_program_id;
    if (!linked || program_id == 0) {
        return;
    }
    const u64 seconds = play_time_manager->GetPlayTime(program_id);
    LOG_INFO(Frontend, "Nextendo history: title={:016X} seconds={}", program_id, seconds);
    if (seconds == 0) {
        return;
    }

    WebService::NextendoApi::HistoryEntry entry;
    entry.title_id = fmt::format("{:016X}", program_id);
    entry.name = nextendo_game_name;
    entry.seconds = seconds;
    entry.last_played = QDateTime::currentDateTimeUtc()
                            .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"))
                            .toStdString();
    entry.icon_base64 = nextendo_game_icon_base64;

    // Detached: shutdown must not block on the network.
    std::thread{[entry] { WebService::NextendoApi::SyncHistory({entry}); }}.detach();
#endif
}

// ---------------------------------------------------------------------------------------------
// BCAT (online schedules / delivery packages)

bool MainWindow::NextendoByamlRequired(u64 title_id) const {
    return IsBcatTitle(title_id);
}

bool MainWindow::NextendoByamlInstalled(u64 title_id) const {
    const auto bcat_dir = BcatDir(fmt::format("{:016X}", title_id));
    if (title_id == kSplatoon3) {
        return Nextendo::Splatoon3Bcat::IsInstalled(bcat_dir);
    }
    if (IsNextendoPackageBcatTitle(title_id)) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(bcat_dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().filename() != ".nextendo_bcat_hash") {
                return true;
            }
        }
        return false;
    }
    return std::filesystem::exists(bcat_dir / "vsdata/VSSetting_0.byaml");
}

bool MainWindow::NextendoByamlSkipped(u64 title_id) const {
    std::ifstream file{ByamlSkipPath()};
    if (!file) {
        return false;
    }
    const auto needle = fmt::format("{:016X}", title_id);
    std::string line;
    while (std::getline(file, line)) {
        if (line == needle) {
            return true;
        }
    }
    return false;
}

void MainWindow::NextendoByamlMarkSkipped(u64 title_id) const {
    std::ofstream file{ByamlSkipPath(), std::ios::app};
    if (file) {
        file << fmt::format("{:016X}", title_id) << "\n";
    }
}

// Runs on worker threads: touches only the filesystem and the network, never the UI.
bool MainWindow::NextendoByamlDownload(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    const auto title_id_hex = fmt::format("{:016X}", title_id);

    // Only this title ID's server-side Splatoon 2 content is kept current; fetch it for all.
    constexpr u64 kCanonicalByamlTitleId = kSplatoon2Eu;
    const bool package = IsNextendoPackageBcatTitle(title_id);
    const auto fetch_title_id_hex = fmt::format(
        "{:016X}", (package || title_id == kSplatoon3) ? title_id : kCanonicalByamlTitleId);
    const auto dest_path = BcatDir(title_id_hex);
    if (package) {
        // Put back a cache left aside by an interrupted swap, even if the server is unreachable.
        Nextendo::PackageBcat::RecoverInterrupted(dest_path);
    }

    // Always fetch the full seed and compare its hash locally (the server's 304 handling isn't
    // reliable enough to be the only freshness signal).
    auto zip_bytes = WebService::NextendoApi::DownloadBcatSeed(fetch_title_id_hex);
    if (zip_bytes.empty() && title_id == kPokemonScarlet) {
        // The current event is shared by both games but served under Violet's title ID.
        zip_bytes =
            WebService::NextendoApi::DownloadBcatSeed(fmt::format("{:016X}", kPokemonViolet));
    }
    if (zip_bytes.empty()) {
        return false;
    }

    if (title_id == kSplatoon3) {
        try {
            const bool changed = Nextendo::Splatoon3Bcat::Install(zip_bytes, dest_path);
            LOG_INFO(Frontend, "Nextendo BCAT: Splatoon 3 package {}",
                     changed ? "installed" : "already current");
            return true;
        } catch (const std::exception& ex) {
            LOG_ERROR(Frontend, "Nextendo BCAT: Splatoon 3 update failed; keeping cache: {}",
                      ex.what());
            return false;
        }
    }

    const auto server_hash = WebService::NextendoApi::HashBcatSeedHex(zip_bytes);
    if (server_hash == NextendoByamlReadHash(title_id_hex) && NextendoByamlInstalled(title_id)) {
        return true; // already current
    }

    if (package) {
        // The package is the title's whole delivery cache, validated in memory then swapped in
        // place of the previous folder; a failure keeps the previous cache.
        try {
            Nextendo::PackageBcat::Install(zip_bytes, dest_path, server_hash);
            LOG_INFO(Frontend, "Nextendo BCAT: package for {} installed ({})", title_id_hex,
                     server_hash);
            return true;
        } catch (const std::exception& ex) {
            LOG_ERROR(Frontend, "Nextendo BCAT: package for {} rejected; keeping cache: {}",
                      title_id_hex, ex.what());
            return false;
        }
    }

    // Splatoon 2 schedule seed (vsdata/coopdata/fesdata): replace the folder wholesale so files
    // the server no longer serves don't linger. ExtractToDirectory rejects unsafe paths.
    std::error_code ec;
    std::filesystem::remove_all(dest_path, ec);
    const bool ok = Nextendo::Zip::ExtractToDirectory(zip_bytes, dest_path);
    if (ok) {
        NextendoByamlWriteHash(title_id_hex, server_hash);
    } else {
        LOG_ERROR(Frontend, "Nextendo BCAT: could not extract the schedule for {}", title_id_hex);
    }
    return ok;
#else
    (void)title_id;
    return false;
#endif
}

void MainWindow::RunNextendoByamlDownloadWithProgress(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    QProgressDialog progress(tr("Downloading online schedule..."), QString{}, 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.show();

    auto future = QtConcurrent::run([this, title_id] { return NextendoByamlDownload(title_id); });
    while (!future.isFinished()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    progress.close();

    if (future.result()) {
        QMessageBox::information(this, tr("Nextendo Network"), tr("Online data installed."));
    } else {
        QMessageBox::warning(this, tr("Nextendo Network"),
                             tr("Failed to download the online data. You can try again later "
                                "via right-click on the game."));
    }
#else
    (void)title_id;
#endif
}

void MainWindow::NextendoByamlDownloadFromMenu(u64 title_id) {
    if (title_id == kSplatoon3 && QtCommon::system->IsPoweredOn()) {
        QMessageBox::information(this, tr("Nextendo BCAT"),
                                 tr("Close the running game before updating Splatoon 3 BCAT."));
        return;
    }
    RunNextendoByamlDownloadWithProgress(title_id);
}

void MainWindow::SilentlyDownloadNextendoByaml(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    // Splatoon 3 is refreshed synchronously before boot; a detached install could otherwise
    // replace its cache after the game has already opened it.
    if (title_id == kSplatoon3) {
        return;
    }
    std::thread{[this, title_id] {
        if (NextendoByamlDownload(title_id)) {
            LOG_INFO(Frontend, "Nextendo BCAT: auto-download succeeded for {:016X}", title_id);
        } else {
            LOG_ERROR(Frontend, "Nextendo BCAT: auto-download failed for {:016X}", title_id);
        }
    }}.detach();
#else
    (void)title_id;
#endif
}

void MainWindow::OfferNextendoByamlDownload(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    if (title_id == kSplatoon3 && QtCommon::system->IsPoweredOn()) {
        LOG_WARNING(Frontend,
                    "Nextendo BCAT: close the running game to refresh Splatoon 3 safely");
        return;
    }
    // Packages and Splatoon 3 are refreshed every boot, before the game can open the files.
    if (IsNextendoPackageBcatTitle(title_id) || title_id == kSplatoon3) {
        auto future =
            QtConcurrent::run([this, title_id] { return NextendoByamlDownload(title_id); });
        while (!future.isFinished()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return;
    }
    if (!NextendoByamlRequired(title_id) || NextendoByamlInstalled(title_id) ||
        NextendoByamlSkipped(title_id)) {
        return;
    }

    QMessageBox ask(this);
    ask.setWindowTitle(tr("Nextendo Network"));
    ask.setText(tr("This game needs the online schedule (Nextendo Network)."));
    ask.setInformativeText(
        tr("This file (stage/mode/festival schedules) is required to play online and is NOT "
           "included with the emulator. It can be downloaded from Nextendo Network servers and "
           "installed automatically.\n\nWithout it, the game stays stuck \"offline\". "
           "(Re-downloadable later via right-click on the game.)"));
    QPushButton* yes_button = ask.addButton(tr("Yes, download"), QMessageBox::AcceptRole);
    ask.addButton(tr("No"), QMessageBox::RejectRole);
    QPushButton* skip_button = ask.addButton(tr("Don't ask again"), QMessageBox::DestructiveRole);
    ask.setDefaultButton(yes_button);
    ask.exec();

    if (ask.clickedButton() == skip_button) {
        NextendoByamlMarkSkipped(title_id);
        return;
    }
    if (ask.clickedButton() != yes_button) {
        return;
    }
    RunNextendoByamlDownloadWithProgress(title_id);
#else
    (void)title_id;
#endif
}
