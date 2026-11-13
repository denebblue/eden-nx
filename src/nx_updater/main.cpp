// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone update helper. The emulator downloads and extracts a new release to a staging
// folder, copies this program next to it and starts it, then exits. This helper:
//   1. waits for the emulator process to exit,
//   2. moves every file of the previous version aside (listed in its nx-manifest.txt, or the
//      files the new version replaces when an older build has no manifest),
//   3. copies the new version in, rolling everything back if any copy fails,
//   4. deletes the previous version's files and restarts the emulator.
// The portable "user" folder is never touched, and neither is %APPDATA%.
//
// Usage: eden-nx-updater.exe --pid <pid> --source <staging dir> --target <install dir>
//                            --exe <executable name>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const wchar_t* ManifestName = L"nx-manifest.txt";
constexpr const wchar_t* OldSuffix = L".nx-old";

std::wofstream g_log;

void Log(const std::wstring& line) {
    if (g_log) {
        g_log << line << std::endl;
    }
}

void Fail(const std::wstring& message) {
    Log(L"ERROR: " + message);
    MessageBoxW(nullptr, message.c_str(), L"Update failed", MB_OK | MB_ICONERROR);
}

bool IsUserData(const fs::path& relative) {
    const auto first = relative.begin();
    if (first == relative.end()) {
        return false;
    }
    std::wstring head = first->wstring();
    for (auto& c : head) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return head == L"user";
}

// Relative paths of every regular file under root, except the portable user folder.
std::set<fs::path> ListFiles(const fs::path& root) {
    std::set<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto relative = fs::relative(it->path(), root, ec);
        if (ec) {
            break;
        }
        if (IsUserData(relative)) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file()) {
            files.insert(relative);
        }
    }
    return files;
}

std::set<fs::path> ReadManifest(const fs::path& path) {
    std::set<fs::path> files;
    std::wifstream in{path};
    std::wstring line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) {
            line.pop_back();
        }
        const fs::path relative{line};
        // Only plain relative paths inside the install folder.
        if (line.empty() || relative.is_absolute() || relative.has_root_name() ||
            line.find(L"..") != std::wstring::npos || IsUserData(relative)) {
            continue;
        }
        files.insert(relative);
    }
    return files;
}

// Retries: the old executable can stay locked for a moment after its process exits.
template <typename F>
bool Retry(F&& operation) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (operation()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD pid = 0;
    fs::path source;
    fs::path target;
    std::wstring exe_name = L"eden.exe";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring key = argv[i];
        const std::wstring value = argv[i + 1];
        if (key == L"--pid") {
            pid = static_cast<DWORD>(std::wcstoul(value.c_str(), nullptr, 10));
        } else if (key == L"--source") {
            source = value;
        } else if (key == L"--target") {
            target = value;
        } else if (key == L"--exe") {
            exe_name = value;
        }
    }
    LocalFree(argv);

    if (source.empty() || target.empty() || !fs::is_directory(source) ||
        !fs::is_directory(target) || !fs::exists(source / exe_name)) {
        MessageBoxW(nullptr, L"Invalid update arguments.", L"Update failed",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    g_log.open(source.parent_path() / L"update.log");
    Log(L"source: " + source.wstring());
    Log(L"target: " + target.wstring());

    // 1. Wait for the emulator to exit.
    if (pid != 0) {
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            if (WaitForSingleObject(process, 120000) == WAIT_TIMEOUT) {
                CloseHandle(process);
                Fail(L"The emulator did not close. Close it and run the update again.");
                return 1;
            }
            CloseHandle(process);
        }
    }

    const std::set<fs::path> new_files = ListFiles(source);
    std::set<fs::path> old_files = ReadManifest(target / ManifestName);
    // Builds without a manifest: only the files this version replaces are known to be ours.
    for (const auto& file : new_files) {
        if (fs::exists(target / file)) {
            old_files.insert(file);
        }
    }

    // 2. Move the previous version aside.
    std::vector<fs::path> moved;
    const auto rollback = [&] {
        std::error_code ec;
        for (const auto& file : new_files) {
            fs::remove(target / file, ec);
        }
        for (const auto& file : moved) {
            fs::path aside = target / file;
            aside += OldSuffix;
            fs::rename(aside, target / file, ec);
        }
    };
    for (const auto& file : old_files) {
        const fs::path live = target / file;
        if (!fs::exists(live)) {
            continue;
        }
        fs::path aside = live;
        aside += OldSuffix;
        const bool ok = Retry([&] {
            std::error_code ec;
            fs::remove(aside, ec);
            fs::rename(live, aside, ec);
            return !ec;
        });
        if (!ok) {
            Log(L"cannot move " + live.wstring());
            rollback();
            Fail(L"Could not replace " + file.wstring() +
                 L".\nThe previous version was kept. Make sure the emulator is closed and the "
                 L"folder is writable.");
            return 1;
        }
        moved.push_back(file);
    }

    // 3. Copy the new version in.
    for (const auto& file : new_files) {
        const fs::path destination = target / file;
        const bool ok = Retry([&] {
            std::error_code ec;
            fs::create_directories(destination.parent_path(), ec);
            fs::copy_file(source / file, destination, fs::copy_options::overwrite_existing, ec);
            return !ec;
        });
        if (!ok) {
            Log(L"cannot copy " + file.wstring());
            rollback();
            Fail(L"Could not install " + file.wstring() + L".\nThe previous version was kept.");
            return 1;
        }
    }

    // 4. Delete the previous version and start the new one.
    std::error_code ec;
    for (const auto& file : moved) {
        fs::path aside = target / file;
        aside += OldSuffix;
        fs::remove(aside, ec);
    }
    Log(L"installed " + std::to_wstring(new_files.size()) + L" files");

    std::wstring command = L"\"" + (target / exe_name).wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       target.wstring().c_str(), &startup, &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    } else {
        Fail(L"The update was installed, but the emulator could not be restarted. Open it "
             L"manually.");
    }

    fs::remove_all(source, ec);
    return 0;
}
