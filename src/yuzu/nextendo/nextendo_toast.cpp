// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "yuzu/nextendo/nextendo_toast.h"

namespace {
constexpr int ToastWidth = 320;
constexpr int Margin = 12;
constexpr int NormalTimeoutMs = 6000;
constexpr int InviteTimeoutMs = 30000;
} // Anonymous namespace

NextendoToast::NextendoToast(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("NextendoToast"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setStyleSheet(QStringLiteral("#NextendoToast { background: #202225; border: 1px solid #5865f2;"
                                 " border-radius: 6px; } QLabel { color: white; }"));
    setFixedWidth(ToastWidth);
    setCursor(Qt::PointingHandCursor);

    title_label = new QLabel(this);
    QFont bold = title_label->font();
    bold.setBold(true);
    title_label->setFont(bold);
    title_label->setWordWrap(true);
    detail_label = new QLabel(this);
    detail_label->setWordWrap(true);

    accept_button = new QPushButton(tr("Accept"), this);
    decline_button = new QPushButton(tr("Decline"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(decline_button);
    buttons->addWidget(accept_button);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(title_label);
    layout->addWidget(detail_label);
    layout->addLayout(buttons);

    connect(accept_button, &QPushButton::clicked, this, [this] {
        const QString id = current.invite_id;
        Finish();
        emit inviteAnswered(id, true);
    });
    connect(decline_button, &QPushButton::clicked, this, [this] {
        const QString id = current.invite_id;
        Finish();
        emit inviteAnswered(id, false);
    });

    hide_timer.setSingleShot(true);
    connect(&hide_timer, &QTimer::timeout, this, &NextendoToast::Finish);

    parent->installEventFilter(this);
    hide();
}

NextendoToast::~NextendoToast() = default;

void NextendoToast::Show(const QString& title, const QString& detail, Kind kind) {
    // Status messages (sign-in result, cloud save...) are always shown.
    if (kind != Kind::Info && !UISettings::values.nextendo_notifications_enabled.GetValue()) {
        return;
    }
    Enqueue({title, detail, kind, {}});
}

void NextendoToast::ShowInvite(const QString& sender, const QString& detail,
                               const QString& invite_id) {
    // Invites are actionable, so they are shown even with notifications turned off.
    Enqueue({sender, detail, Kind::GameInvite, invite_id});
}

void NextendoToast::Enqueue(Entry entry) {
    queue.push_back(std::move(entry));
    if (!showing) {
        ShowNext();
    }
}

void NextendoToast::ShowNext() {
    if (queue.empty()) {
        showing = false;
        hide();
        return;
    }
    current = std::move(queue.front());
    queue.pop_front();
    showing = true;

    title_label->setText(current.title);
    detail_label->setText(current.detail);
    detail_label->setVisible(!current.detail.isEmpty());
    const bool is_invite = current.kind == Kind::GameInvite;
    accept_button->setVisible(is_invite);
    decline_button->setVisible(is_invite);

    adjustSize();
    Reposition();
    show();
    raise();
    hide_timer.start(is_invite ? InviteTimeoutMs : NormalTimeoutMs);
}

void NextendoToast::Finish() {
    hide_timer.stop();
    ShowNext();
}

void NextendoToast::Reposition() {
    if (auto* p = parentWidget()) {
        move(p->width() - width() - Margin, Margin + 24);
    }
}

void NextendoToast::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && current.kind != Kind::GameInvite) {
        const Kind kind = current.kind;
        Finish();
        emit clicked(kind);
        return;
    }
    QFrame::mousePressEvent(event);
}

bool NextendoToast::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        Reposition();
    }
    return QFrame::eventFilter(watched, event);
}
