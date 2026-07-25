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

#ifndef KLOGG_PLATFORM_CLIPBOARD_H
#define KLOGG_PLATFORM_CLIPBOARD_H

#include <QChar>

namespace klogg::platform {

// On Windows, Qt handles CRLF conversion natively for clipboard text.
// On other platforms, bare LF is the norm and we emit it directly.
// Returns the line-ending sequence to append when building clipboard text.
#ifdef Q_OS_WIN
constexpr auto clipboardLineEnding = QChar::LineFeed;
#else
constexpr auto clipboardNewlineBeforeLf = true;
#endif

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_CLIPBOARD_H
