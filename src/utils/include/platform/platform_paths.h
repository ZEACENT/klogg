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

#ifndef KLOGG_PLATFORM_PATHS_H
#define KLOGG_PLATFORM_PATHS_H

#include <QSettings>

namespace klogg::platform {

// The preferred QSettings format for each platform.
// Windows: IniFormat (registry not used for application settings).
// macOS: NativeFormat (plist, HIG-conformant).
// Linux: NativeFormat (ini/conf).
#ifdef Q_OS_WIN
constexpr auto settingsFormat = QSettings::IniFormat;
#else
constexpr auto settingsFormat = QSettings::NativeFormat;
#endif

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_PATHS_H
