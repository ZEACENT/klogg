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

#include <QFile>

namespace klogg::platform {

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

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_FILES_H
