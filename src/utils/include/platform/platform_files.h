/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_PLATFORM_FILES_H
#define KLOGG_PLATFORM_FILES_H

#include <cstdint>
#include <optional>

#include <QFile>
#include <QFileDevice>
#include <QString>

namespace klogg::platform {

struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t file = 0;
};

bool operator==( const FileIdentity& left, const FileIdentity& right );
bool operator!=( const FileIdentity& left, const FileIdentity& right );
std::optional<FileIdentity> fileIdentity( const QFileDevice& file );

// Whether the platform uses exclusive file locks (Windows). When true, the
// "keep file closed" option is shown in preferences.
#ifdef Q_OS_WIN
constexpr bool hasExclusiveFileLocks = true;
#else
constexpr bool hasExclusiveFileLocks = false;
#endif

// Whether file-polling should be the default (true on Windows where
// filesystem notifications are less reliable).
#ifdef Q_OS_WIN
constexpr bool pollingEnabledDefault = true;
#else
constexpr bool pollingEnabledDefault = false;
#endif

// Secure filesystem objects for the current account without exposing platform
// ACL or mode-bit details to callers.
bool ensureOwnerOnlyDirectory( const QString& path );
bool restrictRegularFileToOwner( const QString& path );
bool ownerOnlyAccessIsEnforced( const QString& path );

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_FILES_H
