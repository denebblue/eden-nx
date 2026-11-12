// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <QFrame>
#include <QString>
#include <QTimer>

class QLabel;
class QPushButton;

// Minimal in-window notification: one card at a time in the top-right corner of the main window,
// queued, auto-hidden. Clicking the card emits clicked(); invites get Accept/Decline buttons.
class NextendoToast : public QFrame {
    Q_OBJECT

public:
    enum class Kind { Online, Offline, Request, RequestSent, GameInvite, Info };

    explicit NextendoToast(QWidget* parent);
    ~NextendoToast() override;

    void Show(const QString& title, const QString& detail, Kind kind);
    void ShowInvite(const QString& sender, const QString& detail, const QString& invite_id);

signals:
    void clicked(NextendoToast::Kind kind);
    void inviteAnswered(QString invite_id, bool accepted);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QString title;
        QString detail;
        Kind kind;
        QString invite_id;
    };

    void Enqueue(Entry entry);
    void ShowNext();
    void Finish();
    void Reposition();

    std::deque<Entry> queue;
    Entry current{};
    bool showing = false;
    QTimer hide_timer;
    QLabel* title_label;
    QLabel* detail_label;
    QPushButton* accept_button;
    QPushButton* decline_button;
};
