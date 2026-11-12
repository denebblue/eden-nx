// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDateTime>
#include <QLocale>
#include "yuzu/applets/qt_error.h"
#include "yuzu/main_window.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace {

// [Nextendo] NEX carries no field for why a login was refused, so every account gate reaches the
// player as a bare 2306-XXXX. Ask the account server about our own account and say what it was.
// Runs on the emulation thread (the applet callers), never on the UI thread.
QString NextendoGateHint(Result error) {
#ifdef ENABLE_WEB_SERVICE
    constexpr u32 NexModule = 306;
    if (static_cast<u32>(error.GetModule()) != NexModule) {
        return {};
    }

    const auto status = WebService::NextendoApi::GetOnlineStatus();
    if (!status.queried || status.allow) {
        return {};
    }

    QString text = QString::fromStdString(status.message);
    if (status.reason == "suspended") {
        if (status.suspended_indefinitely) {
            text += QStringLiteral("\n") +
                    QtErrorDisplay::tr("Your online access is suspended indefinitely.");
        } else if (!status.suspended_until.empty()) {
            QString until = QString::fromStdString(status.suspended_until);
            bool numeric = false;
            const qint64 secs = until.toLongLong(&numeric);
            const QDateTime when = numeric ? QDateTime::fromSecsSinceEpoch(secs)
                                           : QDateTime::fromString(until, Qt::ISODate);
            if (when.isValid()) {
                until = QLocale().toString(when.toLocalTime(), QLocale::LongFormat);
            }
            text += QStringLiteral("\n") +
                    QtErrorDisplay::tr("Your online access is suspended until %1.").arg(until);
        }
    }
    if (text.trimmed().isEmpty()) {
        if (status.reason.empty()) {
            return {};
        }
        text = QtErrorDisplay::tr("Nextendo refused the connection (%1).")
                   .arg(QString::fromStdString(status.reason));
    }
    return QStringLiteral("\n\n%1").arg(text.trimmed());
#else
    (void)error;
    return {};
#endif
}

} // Anonymous namespace

QtErrorDisplay::QtErrorDisplay(MainWindow& parent) {
    connect(this, &QtErrorDisplay::MainWindowDisplayError, &parent,
            &MainWindow::ErrorDisplayDisplayError, Qt::QueuedConnection);
    connect(this, &QtErrorDisplay::MainWindowRequestExit, &parent,
            &MainWindow::ErrorDisplayRequestExit, Qt::QueuedConnection);
    connect(&parent, &MainWindow::ErrorDisplayFinished, this,
            &QtErrorDisplay::MainWindowFinishedError, Qt::DirectConnection);
}

QtErrorDisplay::~QtErrorDisplay() = default;

void QtErrorDisplay::Close() const {
    callback = {};
    emit MainWindowRequestExit();
}

void QtErrorDisplay::ShowError(Result error, FinishedCallback finished) const {
    callback = std::move(finished);
    emit MainWindowDisplayError(
        tr("Error Code: %1-%2 (0x%3)")
            .arg(static_cast<u32>(error.GetModule()) + 2000, 4, 10, QChar::fromLatin1('0'))
            .arg(error.GetDescription(), 4, 10, QChar::fromLatin1('0'))
            .arg(error.raw, 8, 16, QChar::fromLatin1('0')),
        tr("An error has occurred.\nPlease try again or contact the developer of the software.") +
            NextendoGateHint(error));
}

void QtErrorDisplay::ShowErrorWithTimestamp(Result error, std::chrono::seconds time,
                                            FinishedCallback finished) const {
    callback = std::move(finished);

    const QDateTime date_time = QDateTime::fromSecsSinceEpoch(time.count());
    emit MainWindowDisplayError(
        tr("Error Code: %1-%2 (0x%3)")
            .arg(static_cast<u32>(error.GetModule()) + 2000, 4, 10, QChar::fromLatin1('0'))
            .arg(error.GetDescription(), 4, 10, QChar::fromLatin1('0'))
            .arg(error.raw, 8, 16, QChar::fromLatin1('0')),
        tr("An error occurred on %1 at %2.\nPlease try again or contact the developer of the "
           "software.")
                .arg(date_time.toString(QStringLiteral("dddd, MMMM d, yyyy")))
                .arg(date_time.toString(QStringLiteral("h:mm:ss A"))) +
            NextendoGateHint(error));
}

void QtErrorDisplay::ShowCustomErrorText(Result error, std::string dialog_text,
                                         std::string fullscreen_text,
                                         FinishedCallback finished) const {
    callback = std::move(finished);
    // [Nextendo] ResultSuccess = informational notice, skip the error framing.
    if (error == ResultSuccess) {
        emit MainWindowDisplayError(QString::fromStdString(dialog_text),
                                    QString::fromStdString(fullscreen_text));
        return;
    }
    emit MainWindowDisplayError(
        tr("Error Code: %1-%2 (0x%3)")
            .arg(static_cast<u32>(error.GetModule()) + 2000, 4, 10, QChar::fromLatin1('0'))
            .arg(error.GetDescription(), 4, 10, QChar::fromLatin1('0'))
            .arg(error.raw, 8, 16, QChar::fromLatin1('0')),
        tr("An error has occurred.\n\n%1\n\n%2")
                .arg(QString::fromStdString(dialog_text))
                .arg(QString::fromStdString(fullscreen_text)) +
            NextendoGateHint(error));
}

void QtErrorDisplay::MainWindowFinishedError() {
    if (callback) {
        callback();
    }
}
