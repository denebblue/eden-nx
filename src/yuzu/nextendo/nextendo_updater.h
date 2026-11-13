// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

class QWidget;

// Self-updater: checks this fork's GitHub releases, downloads the Windows package, and hands the
// file swap to eden-nx-updater.exe, which restarts the emulator. User data (the portable "user"
// folder or %APPDATA%) is never touched.
class NextendoUpdater : public QObject {
    Q_OBJECT

public:
    explicit NextendoUpdater(QWidget* parent);

    // Startup check: silent unless a newer release exists (and wasn't skipped).
    void CheckInBackground();
    // Menu check: always reports the result.
    void CheckInteractive();

    static const char* CurrentVersion();

signals:
    // Emitted when the helper has been started; the main window must close.
    void QuitForUpdate();

private:
    void Check(bool interactive);
    void Offer(const QString& tag, const QString& notes, bool interactive);
    void DownloadAndInstall(const QString& tag);

    QWidget* parent_widget;
    bool busy = false;
};
