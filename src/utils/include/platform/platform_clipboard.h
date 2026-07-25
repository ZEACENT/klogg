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

// On Windows, Qt inserts CR before LF in clipboard text natively, so we do not
// prepend one ourselves. On other platforms we emit it explicitly.
#ifdef Q_OS_WIN
constexpr bool clipboardNewlineBeforeLf = false;
#else
constexpr bool clipboardNewlineBeforeLf = true;
#endif

} // namespace klogg::platform

#endif // KLOGG_PLATFORM_CLIPBOARD_H
