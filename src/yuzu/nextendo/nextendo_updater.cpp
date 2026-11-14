// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nextendo/nextendo_updater.h"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

#include <QApplication>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <QtConcurrent/QtConcurrent>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "qt_common/qt_common.h"
#include "web_service/github_releases.h"
#include "yuzu/nextendo/nextendo_zip.h"

#ifndef NX_VERSION
#define NX_VERSION "dev"
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* RepoOwner = "denebblue";
constexpr const char* RepoName = "eden-nx";
constexpr const char* PackageAsset = "eden-nextendo-windows-x64.zip";
constexpr const char* HelperName = "eden-nx-updater.exe";
constexpr const char* MainExe = "eden.exe";

// "v1.2.3" -> {1, 2, 3}; nullopt for anything else (dev builds never auto-update).
std::optional<std::array<int, 3>> ParseVersion(const std::string& text) {
    if (text.size() < 2 || (text[0] != 'v' && text[0] != 'V')) {
        return std::nullopt;
    }
    std::array<int, 3> parts{};
    std::size_t index = 0;
    std::size_t pos = 1;
    while (index < parts.size()) {
        std::size_t next = pos;
        while (next < text.size() && std::isdigit(static_cast<unsigned char>(text[next]))) {
            ++next;
        }
        if (next == pos) {
            return std::nullopt;
        }
        parts[index++] = std::stoi(text.substr(pos, next - pos));
        if (next >= text.size() || text[next] != '.') {
            break;
        }
        pos = next + 1;
    }
    return parts;
}

fs::path SkipFile() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nx_update_skip.txt";
}

std::string ReadSkippedTag() {
    std::ifstream in{SkipFile()};
    std::string tag;
    std::getline(in, tag);
    return tag;
}

void WriteSkippedTag(const std::string& tag) {
    std::ofstream out{SkipFile(), std::ios::trunc};
    out << tag;
}

bool IsWritable(const fs::path& dir) {
    const fs::path probe = dir / ".nx-write-test";
    {
        std::ofstream out{probe};
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

} // namespace

NextendoUpdater::NextendoUpdater(QWidget* parent) : QObject(parent), parent_widget(parent) {}

const char* NextendoUpdater::CurrentVersion() {
    return NX_VERSION;
}

void NextendoUpdater::CheckInBackground() {
#if defined(_WIN32) && defined(ENABLE_WEB_SERVICE)
    // Give the window and the game list time to settle first.
    QTimer::singleShot(std::chrono::seconds(5), this, [this] { Check(false); });
#endif
}

void NextendoUpdater::CheckInteractive() {
#if defined(_WIN32) && defined(ENABLE_WEB_SERVICE)
    Check(true);
#else
    QMessageBox::information(parent_widget, tr("Updates"),
                             tr("Automatic updates are only available on Windows."));
#endif
}

void NextendoUpdater::Check(bool interactive) {
#ifdef ENABLE_WEB_SERVICE
    if (busy) {
        return;
    }
    const auto current = ParseVersion(NX_VERSION);
    if (!current) {
        if (interactive) {
            QMessageBox::information(parent_widget, tr("Updates"),
                                     tr("This is a development build (%1); it does not update "
                                        "itself.")
                                         .arg(QString::fromLatin1(NX_VERSION)));
        }
        return;
    }

    busy = true;
    auto* watcher = new QFutureWatcher<std::pair<std::optional<WebService::GitHubReleases::Release>,
                                                 std::string>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, current, interactive] {
        watcher->deleteLater();
        busy = false;
        const auto [release, error] = watcher->result();
        if (!release) {
            LOG_WARNING(Frontend, "[Nextendo] Update check failed: {}", error);
            if (interactive) {
                QMessageBox::warning(parent_widget, tr("Updates"),
                                     tr("Could not check for updates:\n%1")
                                         .arg(QString::fromStdString(error)));
            }
            return;
        }
        const auto latest = ParseVersion(release->tag);
        if (!latest || *latest <= *current) {
            if (interactive) {
                QMessageBox::information(parent_widget, tr("Updates"),
                                         tr("You are on the latest version (%1).")
                                             .arg(QString::fromLatin1(NX_VERSION)));
            }
            return;
        }
        if (!interactive && release->tag == ReadSkippedTag()) {
            return;
        }
        Offer(QString::fromStdString(release->tag), QString::fromStdString(release->body),
              interactive);
    });
    watcher->setFuture(QtConcurrent::run([] {
        std::string error;
        auto release = WebService::GitHubReleases::GetLatest(RepoOwner, RepoName, &error);
        return std::make_pair(std::move(release), std::move(error));
    }));
#else
    (void)interactive;
#endif
}

void NextendoUpdater::Offer(const QString& tag, const QString& notes, bool interactive) {
    if (QtCommon::system->IsPoweredOn()) {
        if (interactive) {
            QMessageBox::information(parent_widget, tr("Updates"),
                                     tr("Version %1 is available. Close the running game to "
                                        "update.")
                                         .arg(tag));
        }
        return; // the startup check runs again next launch
    }

    QMessageBox box(QMessageBox::Information, tr("Update available"),
                    tr("Version %1 is available (you have %2).\n\nThe emulator will download it, "
                       "close, install it and start again. Your games, saves and settings are "
                       "kept.")
                        .arg(tag, QString::fromLatin1(NX_VERSION)),
                    QMessageBox::NoButton, parent_widget);
    if (!notes.trimmed().isEmpty()) {
        box.setDetailedText(notes);
    }
    QPushButton* update = box.addButton(tr("Update now"), QMessageBox::AcceptRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    QPushButton* skip = box.addButton(tr("Skip this version"), QMessageBox::DestructiveRole);
    box.setDefaultButton(update);
    box.exec();

    if (box.clickedButton() == skip) {
        WriteSkippedTag(tag.toStdString());
    } else if (box.clickedButton() == update) {
        DownloadAndInstall(tag);
    }
}

void NextendoUpdater::DownloadAndInstall(const QString& tag) {
#ifdef ENABLE_WEB_SERVICE
    const fs::path install_dir = QCoreApplication::applicationDirPath().toStdWString();
    if (!IsWritable(install_dir)) {
        QMessageBox::warning(parent_widget, tr("Update failed"),
                             tr("The emulator folder is not writable:\n%1\n\nMove the emulator "
                                "to a folder you own (for example Documents) and try again.")
                                 .arg(QString::fromStdWString(install_dir.wstring())));
        return;
    }

    busy = true;
    std::atomic<std::size_t> received{0};
    std::atomic<std::size_t> total{0};
    std::atomic<bool> cancel{false};

    QProgressDialog progress(tr("Downloading %1...").arg(tag), tr("Cancel"), 0, 100,
                             parent_widget);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();

    const std::string tag_std = tag.toStdString();
    auto future = QtConcurrent::run([&]() -> std::pair<fs::path, std::string> {
        std::string error;
        const auto release = WebService::GitHubReleases::GetLatest(RepoOwner, RepoName, &error);
        if (!release || release->tag != tag_std) {
            return {{}, error.empty() ? "the release changed, try again" : error};
        }
        const WebService::GitHubReleases::Asset* package = nullptr;
        for (const auto& asset : release->assets) {
            if (asset.name == PackageAsset) {
                package = &asset;
            }
        }
        if (!package) {
            return {{}, std::string{"the release has no "} + PackageAsset};
        }
        std::vector<u8> bytes;
        if (!WebService::GitHubReleases::Download(
                *package, bytes, &error, [&](std::size_t current, std::size_t size) {
                    received = current;
                    total = size;
                    return !cancel.load();
                })) {
            return {{}, error};
        }

        std::error_code ec;
        const fs::path work = fs::temp_directory_path(ec) / "eden-nx-update" / tag_std;
        fs::remove_all(work, ec);
        const fs::path staging = work / "package";
        if (!Nextendo::Zip::ExtractToDirectory(bytes, staging)) {
            return {{}, "the downloaded package could not be extracted"};
        }
        if (!fs::exists(staging / MainExe)) {
            return {{}, "the downloaded package is incomplete"};
        }
        return {staging, {}};
    });

    while (!future.isFinished()) {
        if (progress.wasCanceled()) {
            cancel = true;
        }
        const std::size_t size = total.load();
        if (size > 0) {
            progress.setValue(static_cast<int>(received.load() * 100 / size));
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    progress.close();
    busy = false;

    const auto [staging, error] = future.result();
    if (staging.empty()) {
        if (!cancel) {
            QMessageBox::warning(parent_widget, tr("Update failed"),
                                 tr("The update could not be downloaded:\n%1")
                                     .arg(QString::fromStdString(error)));
        }
        return;
    }

    // Run the helper from outside the install folder so it can replace itself. Prefer the copy
    // shipped with the new version.
    std::error_code ec;
    // Since v1.2.2 the helper lives in tools/ so only eden.exe and eden-cli.exe sit at the top.
    fs::path helper_source;
    for (const fs::path& candidate :
         {staging / "tools" / HelperName, staging / HelperName, install_dir / "tools" / HelperName,
          install_dir / HelperName}) {
        if (fs::exists(candidate, ec)) {
            helper_source = candidate;
            break;
        }
    }
    const fs::path helper = staging.parent_path() / HelperName;
    fs::copy_file(helper_source, helper, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        QMessageBox::warning(parent_widget, tr("Update failed"),
                             tr("The update helper (%1) is missing. Download the new version "
                                "manually from the releases page.")
                                 .arg(QString::fromLatin1(HelperName)));
        return;
    }

    const QStringList arguments{
        QStringLiteral("--pid"),    QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--source"), QString::fromStdWString(staging.wstring()),
        QStringLiteral("--target"), QString::fromStdWString(install_dir.wstring()),
        QStringLiteral("--exe"),    QString::fromLatin1(MainExe),
    };
    if (!QProcess::startDetached(QString::fromStdWString(helper.wstring()), arguments)) {
        QMessageBox::warning(parent_widget, tr("Update failed"),
                             tr("The update helper could not be started."));
        return;
    }
    LOG_INFO(Frontend, "[Nextendo] Update {} staged, restarting through the helper", tag_std);
    emit QuitForUpdate();
#else
    (void)tag;
#endif
}
