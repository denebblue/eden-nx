// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <QDialog>
#include <QTimer>

#include "common/common_types.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTabWidget;
class NextendoController;

namespace Core {
class System;
}

// Nextendo account page. Plain widgets, same functions as Citron's NextendoAccountDialog:
// profile (name/avatar), friends (add by code, accept/decline/cancel requests, remove, report),
// game invitations, players met online, recently played (Quick Start) and cloud saves.
class NextendoAccountDialog : public QDialog {
    Q_OBJECT

public:
    static constexpr int kHomePage = 0;
    static constexpr int kFriendsPage = 1;
    static constexpr int kPlayersPage = 2;
    static constexpr int kHistoryPage = 3;
    static constexpr int kCloudSavesPage = 4;

    explicit NextendoAccountDialog(NextendoController* controller, Core::System& system,
                                   QWidget* parent = nullptr, int initial_page = kHomePage);
    ~NextendoAccountDialog() override;

private:
    QWidget* BuildHomePage();
    QWidget* BuildFriendsPage();
    QWidget* BuildPlayersPage();
    QWidget* BuildHistoryPage();
    QWidget* BuildCloudSavesPage();

    void RefreshFriends();
    void RefreshInvites();
    void RefreshHistory();
    void RefreshPlayers();
    void RefreshCloudSaveTab();
    void ProbeCloudSaveAvailability(u64 title_id);
    void RefreshProfile();
    void RefreshPing();

    void RunAsync(std::function<std::string()> task, std::function<void()> on_success = nullptr);
    bool Confirm(const QString& title, const QString& message);

    void OnAdd();
    void OnAcceptRequest();
    void OnDeclineRequest();
    void OnCancelOutgoing();
    void OnRemoveFriend();
    void OnAcceptInvite();
    void OnDeclineInvite();
    void OnAddPlayer(QListWidget* view);
    void OnReport(QListWidget* view);
    void OpenReportDialog(u64 pid, const QString& name);
    void OnQuickStart();
    void OnChangeAvatar();
    void OnEditUsername();
    void ApplyFriendFilter(const QString& text);

    NextendoController* controller;
    Core::System& system;

    QTabWidget* tabs = nullptr;
    QLabel* status = nullptr;

    // Home
    QLabel* avatar_label = nullptr;
    QLabel* name_label = nullptr;
    QLabel* code_label = nullptr;
    QLabel* online_label = nullptr;
    QLabel* ping_label = nullptr;
    QCheckBox* notifications_checkbox = nullptr;

    // Friends
    QLineEdit* friend_search = nullptr;
    QListWidget* friends_list = nullptr;
    QListWidget* requests_list = nullptr;
    QListWidget* outgoing_list = nullptr;
    QListWidget* invites_list = nullptr;
    QLineEdit* friend_code_input = nullptr;
    QPushButton* add_button = nullptr;

    // Players
    QLabel* lobby_state_label = nullptr;
    QListWidget* lobby_list = nullptr;
    QListWidget* recent_players_list = nullptr;
    std::unordered_set<u64> known_player_pids;

    // History
    QListWidget* history_list = nullptr;

    // Cloud saves
    QLabel* cloud_save_status = nullptr;
    QComboBox* cloud_save_picker = nullptr;
    QPushButton* cloud_save_download_button = nullptr;
    QCheckBox* cloud_save_auto_sync_checkbox = nullptr;
    std::unordered_map<u64, bool> cloud_save_has_data;
    std::unordered_set<u64> cloud_save_probing;

    QTimer refresh_timer;
};
