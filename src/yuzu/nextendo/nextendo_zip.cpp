// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fstream>
#include <stdexcept>

#include <QBuffer>
#include <QByteArray>
#include <quazip.h>
#include <quazipfile.h>
#include <quazipfileinfo.h>
#include <quazipnewinfo.h>

#include "yuzu/nextendo/nextendo_zip.h"

namespace Nextendo::Zip {

namespace {

bool IsSafeRelativePath(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    const std::filesystem::path path{std::u8string(name.begin(), name.end())};
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        name.front() == '/' || name.front() == '\\') {
        return false;
    }
    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }
    return name.find(':') == std::string::npos;
}

} // Anonymous namespace

std::vector<Entry> ReadAll(std::span<const unsigned char> bytes, std::size_t max_total,
                           std::size_t max_entries) {
    if (bytes.empty()) {
        throw std::runtime_error("Empty ZIP");
    }
    QByteArray raw(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<qsizetype>(bytes.size()));
    QBuffer buffer(&raw);
    QuaZip zip(&buffer);
    if (!zip.open(QuaZip::mdUnzip)) {
        throw std::runtime_error("Cannot open ZIP");
    }

    std::vector<Entry> entries;
    std::size_t total = 0;
    for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
        if (entries.size() >= max_entries) {
            throw std::runtime_error("Too many ZIP entries");
        }
        QuaZipFileInfo64 info;
        if (!zip.getCurrentFileInfo(&info)) {
            throw std::runtime_error("Cannot read ZIP entry header");
        }
        Entry entry;
        entry.name = info.name.toStdString();
        entry.encrypted = (info.flags & 0x1) != 0;
        const quint32 unix_mode = info.externalAttr >> 16;
        entry.symlink = (unix_mode & 0170000) == 0120000;
        if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\')) {
            entry.directory = true;
            entry.name.pop_back();
            entries.push_back(std::move(entry));
            continue;
        }
        if (entry.encrypted || entry.symlink) {
            entries.push_back(std::move(entry));
            continue;
        }
        if (info.uncompressedSize > max_total - total) {
            throw std::runtime_error("ZIP expands beyond the size limit");
        }

        QuaZipFile file(&zip);
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error("Cannot open ZIP entry");
        }
        const QByteArray data = file.readAll();
        file.close(); // the CRC is checked here
        if (file.getZipError() != UNZ_OK) {
            throw std::runtime_error("Corrupt ZIP entry (CRC or inflate error)");
        }
        if (static_cast<std::size_t>(data.size()) > max_total - total ||
            static_cast<quint64>(data.size()) != info.uncompressedSize) {
            throw std::runtime_error("ZIP entry size mismatch");
        }
        total += static_cast<std::size_t>(data.size());
        entry.data.assign(data.begin(), data.end());
        entries.push_back(std::move(entry));
    }
    if (zip.getZipError() != UNZ_OK && zip.getZipError() != UNZ_END_OF_LIST_OF_FILE) {
        throw std::runtime_error("Invalid ZIP archive");
    }
    zip.close();
    return entries;
}

bool ExtractToDirectory(std::span<const unsigned char> bytes, const std::filesystem::path& dest) {
    std::vector<Entry> entries;
    try {
        entries = ReadAll(bytes, std::size_t{1} << 31, 1u << 16);
    } catch (const std::exception&) {
        return false;
    }
    for (const auto& entry : entries) {
        if (entry.symlink || entry.encrypted || !IsSafeRelativePath(entry.name)) {
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    for (const auto& entry : entries) {
        const auto path = dest / std::filesystem::path{
                                     std::u8string(entry.name.begin(), entry.name.end())};
        if (entry.directory) {
            std::filesystem::create_directories(path, ec);
            continue;
        }
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(entry.data.data()),
                  static_cast<std::streamsize>(entry.data.size()));
        if (!out) {
            return false;
        }
    }
    return true;
}

std::vector<unsigned char> Create(
    const std::vector<std::pair<std::string, std::vector<unsigned char>>>& files) {
    QByteArray raw;
    {
        QBuffer buffer(&raw);
        QuaZip zip(&buffer);
        if (!zip.open(QuaZip::mdCreate)) {
            return {};
        }
        for (const auto& [name, data] : files) {
            QuaZipFile out(&zip);
            if (!out.open(QIODevice::WriteOnly, QuaZipNewInfo(QString::fromStdString(name)))) {
                return {};
            }
            if (!data.empty() &&
                out.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) {
                return {};
            }
            out.close();
            if (out.getZipError() != ZIP_OK) {
                return {};
            }
        }
        zip.close();
        if (zip.getZipError() != ZIP_OK) {
            return {};
        }
    }
    return std::vector<unsigned char>(raw.begin(), raw.end());
}

} // namespace Nextendo::Zip
