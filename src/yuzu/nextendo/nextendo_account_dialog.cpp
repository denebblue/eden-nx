// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <thread>
#include <unordered_map>

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "common/nextendo_account.h"
#include "common/nextendo_compatible_titles.h"
#include "common/nextendo_outgoing_requests.h"
#include "common/settings.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "qt_common/config/uisettings.h"
#include "yuzu/nextendo/nextendo_account_dialog.h"
#include "yuzu/nextendo/nextendo_controller.h"
#include "yuzu/nextendo/nextendo_game_invites.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace {

constexpr int PidRole = Qt::UserRole + 1;
constexpr int CodeRole = Qt::UserRole + 2;
constexpr int NameRole = Qt::UserRole + 3;
constexpr int InviteIdRole = Qt::UserRole + 4;
constexpr int TitleIdRole = Qt::UserRole + 5;

QIcon IconFromBase64(const QString& b64, int size = 32) {
    if (b64.isEmpty()) {
        return {};
    }
    QPixmap pixmap;
    if (!pixmap.loadFromData(QByteArray::fromBase64(b64.toLatin1()))) {
        return {};
    }
    return QIcon(pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

[[maybe_unused]] QString PresenceText(s32 presence, const QString& game) {
    switch (presence) {
    case 2:
        return game.isEmpty() ? NextendoAccountDialog::tr("Playing")
                              : NextendoAccountDialog::tr("Playing %1").arg(game);
    case 1:
        return NextendoAccountDialog::tr("Online");
    default:
        return NextendoAccountDialog::tr("Offline");
    }
}

[[maybe_unused]] QString FormatSeconds(u64 seconds) {
    const u64 hours = seconds / 3600;
    const u64 minutes = (seconds % 3600) / 60;
    if (hours > 0) {
        return NextendoAccountDialog::tr("%1 h %2 min").arg(hours).arg(minutes);
    }
    return NextendoAccountDialog::tr("%1 min").arg(std::max<u64>(minutes, 1));
}

QListWidget* MakeList(QWidget* parent) {
    auto* list = new QListWidget(parent);
    list->setIconSize(QSize(32, 32));
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    return list;
}

u64 SelectedPid(const QListWidget* list) {
    const auto* item = list->currentItem();
    return item ? item->data(PidRole).toULongLong() : 0;
}

#ifdef ENABLE_WEB_SERVICE
QString PlayerDisplayName(const WebService::NextendoApi::LobbyPlayer& player) {
    if (!player.name.empty()) {
        return QString::fromStdString(player.name);
    }
    return QStringLiteral("#%1").arg(player.pid);
}

QString LobbyStateLine(const WebService::NextendoApi::Lobby& lobby) {
    const QString state = lobby.state_code == "searching"
                              ? NextendoAccountDialog::tr("Looking for players")
                          : lobby.state_code == "matched" ? NextendoAccountDialog::tr("In a match")
                                                          : QString();
    if (state.isEmpty()) {
        return NextendoAccountDialog::tr("%1 / %2 players").arg(lobby.count).arg(lobby.max);
    }
    return NextendoAccountDialog::tr("%1 / %2 players - %3")
        .arg(lobby.count)
        .arg(lobby.max)
        .arg(state);
}
#endif

} // Anonymous namespace

NextendoAccountDialog::NextendoAccountDialog(NextendoController* controller_,
                                             Core::System& system_, QWidget* parent,
                                             int initial_page)
    : QDialog(parent), controller(controller_), system(system_) {
    setWindowTitle(tr("Nextendo Account"));
    resize(720, 560);

    // Otherwise the same physical controller drives this dialog and a running game at once.
    system.HIDCore().SetGuestInputSuspended(true);

    tabs = new QTabWidget(this);
    tabs->addTab(BuildHomePage(), tr("Account"));
    tabs->addTab(BuildFriendsPage(), tr("Friends"));
    tabs->addTab(BuildPlayersPage(), tr("Players"));
    tabs->addTab(BuildHistoryPage(), tr("Recently Played"));
    tabs->addTab(BuildCloudSavesPage(), tr("Cloud Saves"));

    status = new QLabel(this);
    status->setWordWrap(true);

    auto* close_buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(status, 1);
    bottom->addWidget(close_buttons);
    layout->addLayout(bottom);

    tabs->setCurrentIndex(std::clamp(initial_page, 0, tabs->count() - 1));

    RefreshProfile();
    RefreshPing();
    RefreshFriends();
    RefreshHistory();
    RefreshPlayers();
    RefreshCloudSaveTab();

    connect(controller, &NextendoController::StatusChanged, this, [this](const QString& message) {
        if (!message.isEmpty()) {
            status->setText(message);
        }
    });

    refresh_timer.setInterval(15000);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshFriends);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshPlayers);
    connect(&refresh_timer, &QTimer::timeout, this, &NextendoAccountDialog::RefreshCloudSaveTab);
    if (auto* invites = controller->GameInvites()) {
        connect(invites, &NextendoGameInvites::Changed, this,
                &NextendoAccountDialog::RefreshInvites);
        connect(&refresh_timer, &QTimer::timeout, invites, &NextendoGameInvites::Refresh);
        invites->Refresh();
    }
    RefreshInvites();
    refresh_timer.start();

    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == kHistoryPage) {
            RefreshHistory();
        } else if (index == kCloudSavesPage) {
            RefreshCloudSaveTab();
        }
    });
}

NextendoAccountDialog::~NextendoAccountDialog() {
    system.HIDCore().SetGuestInputSuspended(false);
}

// ---------------------------------------------------------------------------------------------
// Page construction

QWidget* NextendoAccountDialog::BuildHomePage() {
    auto* page = new QWidget;
    avatar_label = new QLabel(page);
    avatar_label->setFixedSize(96, 96);
    avatar_label->setScaledContents(true);
    avatar_label->setFrameShape(QFrame::Box);

    name_label = new QLabel(QString::fromStdString(Common::NextendoAccount::GetUsername()), page);
    QFont name_font = name_label->font();
    name_font.setPointSize(name_font.pointSize() + 4);
    name_font.setBold(true);
    name_label->setFont(name_font);
    code_label = new QLabel(
        tr("Friend code: %1").arg(QString::fromStdString(Common::NextendoAccount::GetFriendCode())),
        page);
    code_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    online_label = new QLabel(page);
    if (Settings::values.enable_nextendo.GetValue()) {
        online_label->setText(tr("Network redirection: enabled"));
    } else {
        online_label->setText(
            tr("Network redirection: DISABLED. Enable it in the Nextendo menu to play online."));
    }
    online_label->setWordWrap(true);
    ping_label = new QLabel(tr("Server: checking..."), page);

    auto* change_avatar = new QPushButton(tr("Change Profile Picture..."), page);
    auto* change_name = new QPushButton(tr("Change Username..."), page);
    auto* recheck = new QPushButton(tr("Check Connection"), page);
    connect(change_avatar, &QPushButton::clicked, this, &NextendoAccountDialog::OnChangeAvatar);
    connect(change_name, &QPushButton::clicked, this, &NextendoAccountDialog::OnEditUsername);
    connect(recheck, &QPushButton::clicked, this, &NextendoAccountDialog::RefreshPing);

    notifications_checkbox = new QCheckBox(tr("Show friend notifications"), page);
    notifications_checkbox->setChecked(
        UISettings::values.nextendo_notifications_enabled.GetValue());
    connect(notifications_checkbox, &QCheckBox::toggled, this, [](bool checked) {
        UISettings::values.nextendo_notifications_enabled.SetValue(checked);
    });

    auto* info = new QVBoxLayout;
    info->addWidget(name_label);
    info->addWidget(code_label);
    info->addWidget(online_label);
    info->addWidget(ping_label);
    info->addStretch(1);

    auto* top = new QHBoxLayout;
    top->addWidget(avatar_label, 0, Qt::AlignTop);
    top->addLayout(info, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(change_avatar);
    buttons->addWidget(change_name);
    buttons->addWidget(recheck);
    buttons->addStretch(1);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addLayout(buttons);
    layout->addWidget(notifications_checkbox);
    layout->addStretch(1);
    return page;
}

QWidget* NextendoAccountDialog::BuildFriendsPage() {
    auto* page = new QWidget;

    // Left column: friends.
    friend_search = new QLineEdit(page);
    friend_search->setPlaceholderText(tr("Search friends"));
    friend_search->setClearButtonEnabled(true);
    connect(friend_search, &QLineEdit::textChanged, this,
            &NextendoAccountDialog::ApplyFriendFilter);
    friends_list = MakeList(page);
    auto* remove_button = new QPushButton(tr("Remove Friend"), page);
    auto* report_button = new QPushButton(tr("Report..."), page);
    connect(remove_button, &QPushButton::clicked, this, &NextendoAccountDialog::OnRemoveFriend);
    connect(report_button, &QPushButton::clicked, this,
            [this] { OnReport(friends_list); });

    friend_code_input = new QLineEdit(page);
    friend_code_input->setPlaceholderText(tr("Friend code"));
    add_button = new QPushButton(tr("Add Friend"), page);
    connect(add_button, &QPushButton::clicked, this, &NextendoAccountDialog::OnAdd);
    connect(friend_code_input, &QLineEdit::returnPressed, this, &NextendoAccountDialog::OnAdd);

    auto* add_row = new QHBoxLayout;
    add_row->addWidget(friend_code_input, 1);
    add_row->addWidget(add_button);
    auto* friend_buttons = new QHBoxLayout;
    friend_buttons->addWidget(remove_button);
    friend_buttons->addWidget(report_button);
    friend_buttons->addStretch(1);

    auto* left = new QVBoxLayout;
    left->addWidget(new QLabel(tr("Friends"), page));
    left->addWidget(friend_search);
    left->addWidget(friends_list, 1);
    left->addLayout(friend_buttons);
    left->addLayout(add_row);

    // Right column: invitations and requests.
    invites_list = MakeList(page);
    auto* invite_accept = new QPushButton(tr("Join"), page);
    auto* invite_decline = new QPushButton(tr("Decline"), page);
    connect(invite_accept, &QPushButton::clicked, this, &NextendoAccountDialog::OnAcceptInvite);
    connect(invite_decline, &QPushButton::clicked, this, &NextendoAccountDialog::OnDeclineInvite);
    connect(invites_list, &QListWidget::itemDoubleClicked, this,
            &NextendoAccountDialog::OnAcceptInvite);

    requests_list = MakeList(page);
    auto* request_accept = new QPushButton(tr("Accept"), page);
    auto* request_decline = new QPushButton(tr("Decline"), page);
    connect(request_accept, &QPushButton::clicked, this, &NextendoAccountDialog::OnAcceptRequest);
    connect(request_decline, &QPushButton::clicked, this,
            &NextendoAccountDialog::OnDeclineRequest);

    outgoing_list = MakeList(page);
    auto* outgoing_cancel = new QPushButton(tr("Cancel Request"), page);
    connect(outgoing_cancel, &QPushButton::clicked, this,
            &NextendoAccountDialog::OnCancelOutgoing);

    const auto make_row = [](QPushButton* a, QPushButton* b) {
        auto* row = new QHBoxLayout;
        row->addWidget(a);
        if (b) {
            row->addWidget(b);
        }
        row->addStretch(1);
        return row;
    };

    auto* right = new QVBoxLayout;
    right->addWidget(new QLabel(tr("Game invitations"), page));
    right->addWidget(invites_list, 1);
    right->addLayout(make_row(invite_accept, invite_decline));
    right->addWidget(new QLabel(tr("Incoming friend requests"), page));
    right->addWidget(requests_list, 1);
    right->addLayout(make_row(request_accept, request_decline));
    right->addWidget(new QLabel(tr("Sent friend requests"), page));
    right->addWidget(outgoing_list, 1);
    right->addLayout(make_row(outgoing_cancel, nullptr));

    auto* layout = new QHBoxLayout(page);
    layout->addLayout(left, 3);
    layout->addLayout(right, 2);
    return page;
}

QWidget* NextendoAccountDialog::BuildPlayersPage() {
    auto* page = new QWidget;
    lobby_state_label = new QLabel(tr("Not in a lobby."), page);
    lobby_list = MakeList(page);
    recent_players_list = MakeList(page);

    const auto make_buttons = [this, page](QListWidget* list) {
        auto* add = new QPushButton(tr("Add Friend"), page);
        auto* report = new QPushButton(tr("Report..."), page);
        connect(add, &QPushButton::clicked, this, [this, list] { OnAddPlayer(list); });
        connect(report, &QPushButton::clicked, this, [this, list] { OnReport(list); });
        auto* row = new QHBoxLayout;
        row->addWidget(add);
        row->addWidget(report);
        row->addStretch(1);
        return row;
    };

    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Current lobby"), page));
    layout->addWidget(lobby_state_label);
    layout->addWidget(lobby_list, 1);
    layout->addLayout(make_buttons(lobby_list));
    layout->addWidget(new QLabel(tr("Recently met players"), page));
    layout->addWidget(recent_players_list, 1);
    layout->addLayout(make_buttons(recent_players_list));
    return page;
}

QWidget* NextendoAccountDialog::BuildHistoryPage() {
    auto* page = new QWidget;
    history_list = MakeList(page);
    history_list->setIconSize(QSize(48, 48));
    auto* quick_start = new QPushButton(tr("Quick Start"), page);
    connect(quick_start, &QPushButton::clicked, this, &NextendoAccountDialog::OnQuickStart);
    connect(history_list, &QListWidget::itemDoubleClicked, this,
            &NextendoAccountDialog::OnQuickStart);

    auto* row = new QHBoxLayout;
    row->addWidget(quick_start);
    row->addStretch(1);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(history_list, 1);
    layout->addLayout(row);
    return page;
}

QWidget* NextendoAccountDialog::BuildCloudSavesPage() {
    auto* page = new QWidget;
    cloud_save_status = new QLabel(page);
    cloud_save_status->setWordWrap(true);
    cloud_save_picker = new QComboBox(page);
    cloud_save_picker->setIconSize(QSize(32, 32));
    cloud_save_download_button = new QPushButton(tr("Download Save from Cloud"), page);
    connect(cloud_save_download_button, &QPushButton::clicked, this, [this] {
        const u64 title_id = cloud_save_picker->currentData().toULongLong();
        if (title_id == 0) {
            return;
        }
        if (!Confirm(tr("Cloud Save"),
                     tr("Replace the local save of %1 with the cloud save?")
                         .arg(cloud_save_picker->currentText()))) {
            return;
        }
        controller->ManualSaveDownload(title_id);
    });

    cloud_save_auto_sync_checkbox =
        new QCheckBox(tr("Sync saves automatically (download on boot, upload on stop)"), page);
    cloud_save_auto_sync_checkbox->setChecked(
        Settings::values.nextendo_cloud_sync_enabled.GetValue());
    connect(cloud_save_auto_sync_checkbox, &QCheckBox::toggled, this, [](bool checked) {
        Settings::values.nextendo_cloud_sync_enabled.SetValue(checked);
    });

    auto* row = new QHBoxLayout;
    row->addWidget(cloud_save_picker, 1);
    row->addWidget(cloud_save_download_button);

    auto* layout = new QVBoxLayout(page);
    layout->addWidget(cloud_save_status);
    layout->addLayout(row);
    layout->addWidget(cloud_save_auto_sync_checkbox);
    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------------------------
// Refreshers

void NextendoAccountDialog::RefreshProfile() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto profile = WebService::NextendoApi::GetProfile();
        if (!profile.ok || !guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, image = profile.image_base64, name = profile.name] {
                if (!guard) {
                    return;
                }
                if (!name.empty()) {
                    name_label->setText(QString::fromStdString(name));
                }
                QPixmap pixmap;
                if (!image.empty() && pixmap.loadFromData(QByteArray::fromBase64(
                                          QByteArray::fromStdString(image)))) {
                    avatar_label->setPixmap(pixmap);
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::RefreshPing() {
#ifdef ENABLE_WEB_SERVICE
    ping_label->setText(tr("Server: checking..."));
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        const auto ms = WebService::NextendoApi::PingBackend();
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, ms] {
                if (!guard) {
                    return;
                }
                ping_label->setText(ms ? tr("Server: reachable (%1 ms)").arg(*ms)
                                       : tr("Server: unreachable"));
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::RefreshFriends() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto fetched = WebService::NextendoApi::GetFriends();
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, list = std::move(fetched)]() mutable {
                if (!guard) {
                    return;
                }
                if (!list.ok) {
                    status->setText(QString::fromStdString(list.error));
                    return;
                }

                const u64 prev_friend = SelectedPid(friends_list);
                const u64 prev_request = SelectedPid(requests_list);

                // Same order as Citron: playing my game, playing, online, offline.
                const std::string local_app_id = controller->GetLocalAppId();
                const auto rank = [&](const WebService::NextendoApi::Friend& f) {
                    if (f.presence_status == 0) {
                        return 3;
                    }
                    if (f.app_id.empty()) {
                        return 2;
                    }
                    return (!local_app_id.empty() && f.app_id == local_app_id) ? 0 : 1;
                };
                std::stable_sort(list.friends.begin(), list.friends.end(),
                                 [&](const auto& a, const auto& b) {
                                     const int ra = rank(a);
                                     const int rb = rank(b);
                                     return ra != rb ? ra < rb : a.name < b.name;
                                 });

                friends_list->clear();
                for (const auto& entry : list.friends) {
                    const QString game = controller->ResolveGameName(entry.app_id, entry.app_name);
                    auto* item = new QListWidgetItem(
                        IconFromBase64(QString::fromStdString(entry.image_base64)),
                        QStringLiteral("%1  -  %2")
                            .arg(QString::fromStdString(entry.name),
                                 PresenceText(entry.presence_status, game)),
                        friends_list);
                    item->setData(PidRole, QVariant::fromValue<qulonglong>(entry.pid));
                    item->setData(CodeRole, QString::fromStdString(entry.friend_code));
                    item->setData(NameRole, QString::fromStdString(entry.name));
                    if (entry.pid == prev_friend) {
                        friends_list->setCurrentItem(item);
                    }
                }

                requests_list->clear();
                for (const auto& entry : list.requests) {
                    auto* item = new QListWidgetItem(
                        IconFromBase64(QString::fromStdString(entry.image_base64)),
                        QString::fromStdString(entry.name), requests_list);
                    item->setData(PidRole, QVariant::fromValue<qulonglong>(entry.pid));
                    item->setData(NameRole, QString::fromStdString(entry.name));
                    if (entry.pid == prev_request) {
                        requests_list->setCurrentItem(item);
                    }
                }

                std::vector<std::string> accepted_codes;
                accepted_codes.reserve(list.friends.size());
                for (const auto& entry : list.friends) {
                    accepted_codes.push_back(entry.friend_code);
                }
                Common::NextendoOutgoingRequests::PruneAccepted(accepted_codes);

                outgoing_list->clear();
                for (const auto& entry : Common::NextendoOutgoingRequests::Get()) {
                    auto* item = new QListWidgetItem(QString::fromStdString(entry.friend_code),
                                                     outgoing_list);
                    item->setData(CodeRole, QString::fromStdString(entry.friend_code));
                }

                ApplyFriendFilter(friend_search->text());
                tabs->setTabText(kFriendsPage, list.requests.empty()
                                                   ? tr("Friends")
                                                   : tr("Friends (%1)").arg(list.requests.size()));
                status->setText(tr("%1 friend(s), %2 request(s).")
                                    .arg(list.friends.size())
                                    .arg(list.requests.size()));
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    status->setText(tr("This build has no web services support."));
#endif
}

void NextendoAccountDialog::RefreshInvites() {
    auto* invites = controller->GameInvites();
    const QString prev_id =
        invites_list->currentItem() ? invites_list->currentItem()->data(InviteIdRole).toString()
                                    : QString{};
    invites_list->clear();
    if (!invites) {
        return;
    }
    const s64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto& invite : invites->Pending()) {
        const QString game = controller->ResolveGameName(fmt::format("{:016X}", invite.title_id));
        const s64 minutes = std::max<s64>(1, (invite.expires_at - now + 59) / 60);
        auto* item = new QListWidgetItem(
            IconFromBase64(invites->Avatar(invite.sender_pid)),
            tr("%1 - %2 (%3 min left)")
                .arg(QString::fromStdString(invite.sender_name), game)
                .arg(minutes),
            invites_list);
        const QString id = QString::fromStdString(invite.id);
        item->setData(InviteIdRole, id);
        if (id == prev_id) {
            invites_list->setCurrentItem(item);
        }
    }
}

void NextendoAccountDialog::RefreshHistory() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto fetched = WebService::NextendoApi::GetHistory();
        if (!fetched.ok || !guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, list = std::move(fetched)] {
                if (!guard) {
                    return;
                }
                const QString prev = history_list->currentItem()
                                         ? history_list->currentItem()->data(TitleIdRole).toString()
                                         : QString{};
                history_list->clear();
                for (const auto& entry : list.entries) {
                    const QString local_name = controller->ResolveGameName(entry.title_id);
                    const QString local_icon = controller->ResolveGameIcon(entry.title_id);
                    const QString name = (!local_name.isEmpty() && local_name != tr("a game"))
                                             ? local_name
                                             : QString::fromStdString(entry.name);
                    if (name.contains(QStringLiteral(".nca"), Qt::CaseInsensitive)) {
                        continue;
                    }
                    const QString icon = !local_icon.isEmpty()
                                             ? local_icon
                                             : QString::fromStdString(entry.icon_base64);
                    QString text = QStringLiteral("%1\n%2").arg(name, FormatSeconds(entry.seconds));
                    const auto last = QDateTime::fromString(
                        QString::fromStdString(entry.last_played), Qt::ISODate);
                    if (last.isValid()) {
                        text += tr(" - last played %1")
                                    .arg(QLocale().toString(last.toLocalTime(),
                                                            QLocale::ShortFormat));
                    }
                    auto* item = new QListWidgetItem(IconFromBase64(icon, 48), text, history_list);
                    const QString title_id = QString::fromStdString(entry.title_id);
                    item->setData(TitleIdRole, title_id);
                    item->setData(NameRole, name);
                    if (title_id == prev) {
                        history_list->setCurrentItem(item);
                    }
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::RefreshPlayers() {
#ifdef ENABLE_WEB_SERVICE
    std::thread{[this, guard = QPointer<NextendoAccountDialog>(this)] {
        auto lobby = WebService::NextendoApi::GetMyLobby();
        auto recent = WebService::NextendoApi::GetRecentPlayers();

        std::unordered_map<u64, std::string> avatars;
        const auto fetch_avatar = [&](u64 pid) {
            if (pid != 0 && !avatars.contains(pid)) {
                avatars[pid] = WebService::NextendoApi::GetAvatarByPid(pid);
            }
        };
        for (const auto& player : lobby.players) {
            fetch_avatar(player.pid);
        }
        for (const auto& player : recent) {
            fetch_avatar(player.pid);
        }
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, lobby = std::move(lobby), recent = std::move(recent),
             avatars = std::move(avatars)]() mutable {
                if (!guard) {
                    return;
                }
                known_player_pids.clear();
                lobby_list->clear();
                recent_players_list->clear();
                lobby_state_label->setText(lobby.in_lobby ? LobbyStateLine(lobby)
                                                          : tr("Not in a lobby."));

                const auto add = [&](QListWidget* list, const auto& player, bool in_lobby) {
                    if (player.known) {
                        known_player_pids.insert(player.pid);
                    }
                    QString text = PlayerDisplayName(player);
                    if (player.is_me) {
                        text += tr(" (you)");
                    } else if (player.host) {
                        text += tr(" (host)");
                    }
                    const QString game = controller->ResolveGameName(player.title_id);
                    if (!game.isEmpty() && !in_lobby) {
                        text += QStringLiteral("  -  %1").arg(game);
                    }
                    auto* item = new QListWidgetItem(
                        IconFromBase64(QString::fromStdString(avatars[player.pid])), text, list);
                    item->setData(PidRole, QVariant::fromValue<qulonglong>(player.pid));
                    item->setData(CodeRole, QString::fromStdString(player.friend_code));
                    item->setData(NameRole, PlayerDisplayName(player));
                };
                for (const auto& player : lobby.players) {
                    add(lobby_list, player, true);
                }
                for (const auto& player : recent) {
                    add(recent_players_list, player, false);
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::RefreshCloudSaveTab() {
    const std::string app_id_hex = controller->GetLocalAppId();
    u64 running_title_id = 0;
    if (!app_id_hex.empty()) {
        try {
            running_title_id = std::stoull(app_id_hex, nullptr, 16);
        } catch (const std::exception&) {
        }
    }

    if (running_title_id != 0) {
        cloud_save_picker->setEnabled(false);
        cloud_save_download_button->setEnabled(false);
        if (Nextendo::CompatibleTitles::Table().count(running_title_id)) {
            cloud_save_status->setText(
                tr("%1: the save uploads automatically when you stop this game.")
                    .arg(controller->ResolveGameName(app_id_hex)));
        } else {
            cloud_save_status->setText(tr("Stop the running game to download a cloud save."));
        }
        return;
    }

    const u64 previous = cloud_save_picker->currentData().toULongLong();
    cloud_save_picker->clear();
    for (const auto& [program_id, version] : Nextendo::CompatibleTitles::Table()) {
        const std::string title_id_hex = fmt::format("{:016X}", program_id);
        const QString icon_b64 = controller->ResolveGameIcon(title_id_hex);
        if (icon_b64.isEmpty()) {
            continue; // Not installed locally.
        }
        const auto it = cloud_save_has_data.find(program_id);
        if (it == cloud_save_has_data.end()) {
            ProbeCloudSaveAvailability(program_id);
            continue;
        }
        if (!it->second) {
            continue;
        }
        cloud_save_picker->addItem(IconFromBase64(icon_b64),
                                   controller->ResolveGameName(title_id_hex),
                                   QVariant::fromValue<qulonglong>(program_id));
        if (program_id == previous) {
            cloud_save_picker->setCurrentIndex(cloud_save_picker->count() - 1);
        }
    }

    const bool any = cloud_save_picker->count() > 0;
    cloud_save_picker->setEnabled(any);
    cloud_save_download_button->setEnabled(any);
    if (!cloud_save_probing.empty()) {
        cloud_save_status->setText(tr("Checking for cloud saves..."));
    } else if (any) {
        cloud_save_status->setText(tr("Pick a title to download its cloud save."));
    } else {
        cloud_save_status->setText(tr("No cloud saves found for your installed games."));
    }
}

void NextendoAccountDialog::ProbeCloudSaveAvailability(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    if (cloud_save_probing.count(title_id)) {
        return;
    }
    cloud_save_probing.insert(title_id);
    const std::string title_id_hex = fmt::format("{:016x}", title_id);
    std::thread{[this, title_id, title_id_hex, guard = QPointer<NextendoAccountDialog>(this)] {
        const auto save = WebService::NextendoApi::PullSave(title_id_hex);
        const bool has_data = save.has_value() && !save->empty();
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, title_id, has_data] {
                if (!guard) {
                    return;
                }
                cloud_save_probing.erase(title_id);
                cloud_save_has_data[title_id] = has_data;
                RefreshCloudSaveTab();
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

// ---------------------------------------------------------------------------------------------
// Actions

void NextendoAccountDialog::RunAsync(std::function<std::string()> task,
                                     std::function<void()> on_success) {
#ifdef ENABLE_WEB_SERVICE
    add_button->setEnabled(false);
    status->setText(tr("Working..."));
    std::thread{[this, work = std::move(task), success_cb = std::move(on_success),
                 guard = QPointer<NextendoAccountDialog>(this)] {
        const std::string result = work();
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error = result, success_cb] {
                if (!guard) {
                    return;
                }
                add_button->setEnabled(true);
                if (error.empty()) {
                    status->setText(tr("Done."));
                    RefreshFriends();
                    controller->RefreshFriendCache();
                    if (success_cb) {
                        success_cb();
                    }
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    (void)task;
    (void)on_success;
#endif
}

bool NextendoAccountDialog::Confirm(const QString& title, const QString& message) {
    return QMessageBox::question(this, title, message, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::Yes) == QMessageBox::Yes;
}

void NextendoAccountDialog::OnAdd() {
#ifdef ENABLE_WEB_SERVICE
    const std::string code = friend_code_input->text().trimmed().toStdString();
    if (code.empty()) {
        status->setText(tr("Enter a friend code first."));
        return;
    }
    friend_code_input->clear();
    RunAsync([code] { return WebService::NextendoApi::AddFriendByCode(code); },
             [this, code] {
                 Common::NextendoOutgoingRequests::Add(code);
                 controller->NotifyFriendRequestSent(QString::fromStdString(code));
             });
#endif
}

void NextendoAccountDialog::OnAcceptRequest() {
#ifdef ENABLE_WEB_SERVICE
    const u64 pid = SelectedPid(requests_list);
    if (pid == 0) {
        return;
    }
    const QString name = requests_list->currentItem()->data(NameRole).toString();
    if (!Confirm(tr("Friend Request"), tr("Accept %1's friend request?").arg(name))) {
        return;
    }
    RunAsync([pid] { return WebService::NextendoApi::AcceptFriend(pid); });
#endif
}

void NextendoAccountDialog::OnDeclineRequest() {
#ifdef ENABLE_WEB_SERVICE
    const u64 pid = SelectedPid(requests_list);
    if (pid == 0) {
        return;
    }
    const QString name = requests_list->currentItem()->data(NameRole).toString();
    if (!Confirm(tr("Friend Request"), tr("Decline %1's friend request?").arg(name))) {
        return;
    }
    RunAsync([pid] { return WebService::NextendoApi::DeclineFriend(pid); });
#endif
}

void NextendoAccountDialog::OnCancelOutgoing() {
    auto* item = outgoing_list->currentItem();
    if (!item) {
        return;
    }
    // The server has no "cancel" endpoint: this only drops the local record of the request.
    Common::NextendoOutgoingRequests::Remove(item->data(CodeRole).toString().toStdString());
    delete outgoing_list->takeItem(outgoing_list->row(item));
}

void NextendoAccountDialog::OnRemoveFriend() {
#ifdef ENABLE_WEB_SERVICE
    const u64 pid = SelectedPid(friends_list);
    if (pid == 0) {
        return;
    }
    const QString name = friends_list->currentItem()->data(NameRole).toString();
    if (!Confirm(tr("Remove Friend"), tr("Remove %1 from your friends list?").arg(name))) {
        return;
    }
    RunAsync([pid] { return WebService::NextendoApi::RemoveFriend(pid); });
#endif
}

void NextendoAccountDialog::OnAcceptInvite() {
    auto* invites = controller->GameInvites();
    auto* item = invites_list->currentItem();
    if (!invites || !item) {
        return;
    }
    const QString error = invites->Accept(item->data(InviteIdRole).toString());
    if (!error.isEmpty()) {
        status->setText(error);
        return;
    }
    accept(); // Get out of the game's way while it joins.
}

void NextendoAccountDialog::OnDeclineInvite() {
    auto* invites = controller->GameInvites();
    auto* item = invites_list->currentItem();
    if (!invites || !item) {
        return;
    }
    invites->Decline(item->data(InviteIdRole).toString());
}

void NextendoAccountDialog::OnAddPlayer(QListWidget* view) {
#ifdef ENABLE_WEB_SERVICE
    auto* item = view->currentItem();
    const u64 pid = SelectedPid(view);
    if (!item || pid == 0 || pid == Common::NextendoAccount::GetPid()) {
        return;
    }
    if (!known_player_pids.contains(pid)) {
        status->setText(tr("This player isn't a known Nextendo account."));
        return;
    }
    const std::string friend_code = item->data(CodeRole).toString().toStdString();
    if (friend_code.empty()) {
        status->setText(tr("This player doesn't have a friend code available."));
        return;
    }
    if (!Confirm(tr("Add Friend"),
                 tr("Send %1 a friend request?").arg(item->data(NameRole).toString()))) {
        return;
    }
    RunAsync([friend_code] { return WebService::NextendoApi::AddFriendByCode(friend_code); },
             [this, friend_code] {
                 Common::NextendoOutgoingRequests::Add(friend_code);
                 controller->NotifyFriendRequestSent(QString::fromStdString(friend_code));
             });
#else
    (void)view;
#endif
}

void NextendoAccountDialog::OnReport(QListWidget* view) {
    auto* item = view->currentItem();
    const u64 pid = SelectedPid(view);
    if (!item || pid == 0 || pid == Common::NextendoAccount::GetPid()) {
        return;
    }
    if (view != friends_list && !known_player_pids.contains(pid)) {
        status->setText(tr("This player isn't a known Nextendo account."));
        return;
    }
    OpenReportDialog(pid, item->data(NameRole).toString());
}

void NextendoAccountDialog::OpenReportDialog(u64 pid, const QString& name) {
#ifdef ENABLE_WEB_SERVICE
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Report %1").arg(name));

    auto* reason_combo = new QComboBox(&dialog);
    reason_combo->addItem(tr("Cheating"), QStringLiteral("cheating"));
    reason_combo->addItem(tr("Inappropriate name"), QStringLiteral("name"));
    reason_combo->addItem(tr("Impersonating someone else's in-game name"),
                          QStringLiteral("name_mismatch"));
    reason_combo->addItem(tr("Inappropriate avatar"), QStringLiteral("avatar"));
    reason_combo->addItem(tr("Harassment"), QStringLiteral("harassment"));
    reason_combo->addItem(tr("Griefing"), QStringLiteral("griefing"));
    reason_combo->addItem(tr("Impersonating a Nextendo account"),
                          QStringLiteral("impersonation"));
    reason_combo->addItem(tr("Other"), QStringLiteral("other"));

    auto* comment_edit = new QLineEdit(&dialog);
    comment_edit->setPlaceholderText(tr("What happened? (optional)"));

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Send Report"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Reason:"), &dialog));
    layout->addWidget(reason_combo);
    layout->addWidget(comment_edit);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const std::string reason = reason_combo->currentData().toString().toStdString();
    const std::string comment = comment_edit->text().toStdString();
    RunAsync(
        [pid, reason, comment] {
            const std::string error = WebService::NextendoApi::ReportPlayer(pid, reason, comment);
            if (error == "not_encountered") {
                return std::string("You haven't shared a lobby with this player recently.");
            }
            if (error == "quota") {
                return std::string("You've sent too many reports recently. Try again later.");
            }
            return error;
        },
        [this] { status->setText(tr("Report sent.")); });
#else
    (void)pid;
    (void)name;
#endif
}

void NextendoAccountDialog::OnQuickStart() {
    auto* item = history_list->currentItem();
    if (!item) {
        return;
    }
    bool ok = false;
    const u64 title_id = item->data(TitleIdRole).toString().toULongLong(&ok, 16);
    if (!ok || title_id == 0) {
        return;
    }
    if (!Confirm(tr("Quick Start"), tr("Start %1?").arg(item->data(NameRole).toString()))) {
        return;
    }
    controller->QuickStart(title_id);
    accept();
}

void NextendoAccountDialog::OnChangeAvatar() {
#ifdef ENABLE_WEB_SERVICE
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a profile picture"), QString{}, tr("Images (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }
    QImage image(path);
    if (image.isNull()) {
        status->setText(tr("Couldn't read that image."));
        return;
    }
    const int side = std::min(image.width(), image.height());
    const QRect crop((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    const QImage square =
        image.copy(crop).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray jpeg_bytes;
    QBuffer buffer(&jpeg_bytes);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "JPG", 85);
    const std::string image_base64 = jpeg_bytes.toBase64().toStdString();

    avatar_label->setPixmap(QPixmap::fromImage(square));
    status->setText(tr("Uploading profile picture..."));

    std::thread{[this, image_base64, guard = QPointer<NextendoAccountDialog>(this)] {
        const std::string error = WebService::NextendoApi::PushProfilePicture(image_base64);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error] {
                if (!guard) {
                    return;
                }
                status->setText(error.empty() ? tr("Profile picture updated.")
                                              : QString::fromStdString(error));
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::OnEditUsername() {
#ifdef ENABLE_WEB_SERVICE
    static const QRegularExpression valid_name(QStringLiteral("^[A-Za-z0-9_-]{3,16}$"));

    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Change Username"),
                              tr("3-16 characters: letters, digits, '_' or '-'"),
                              QLineEdit::Normal, name_label->text(), &ok)
            .trimmed();
    if (!ok || name.isEmpty() || name == name_label->text()) {
        return;
    }
    if (!valid_name.match(name).hasMatch()) {
        status->setText(tr("Invalid username."));
        return;
    }

    status->setText(tr("Updating username..."));
    const std::string new_name = name.toStdString();
    std::thread{[this, new_name, guard = QPointer<NextendoAccountDialog>(this)] {
        const std::string error = WebService::NextendoApi::SetUsername(new_name);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [this, guard, error, new_name] {
                if (!guard) {
                    return;
                }
                if (error.empty()) {
                    Common::NextendoAccount::Save(Common::NextendoAccount::GetPid(), new_name,
                                                  Common::NextendoAccount::GetFriendCode(),
                                                  Common::NextendoAccount::GetToken());
                    name_label->setText(QString::fromStdString(new_name));
                    status->setText(tr("Username updated."));
                } else {
                    status->setText(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void NextendoAccountDialog::ApplyFriendFilter(const QString& text) {
    for (int i = 0; i < friends_list->count(); ++i) {
        auto* item = friends_list->item(i);
        item->setHidden(!text.isEmpty() &&
                        !item->data(NameRole).toString().contains(text, Qt::CaseInsensitive));
    }
}
