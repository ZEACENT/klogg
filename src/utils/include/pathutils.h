/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
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

#ifndef KLOGG_PATH_UTILS_H
#define KLOGG_PATH_UTILS_H

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace klogg {

// Human-readable label for a document path: the final path component,
// robust to trailing/duplicate separators.
//
// QFileInfo::fileName() returns an empty string for paths that end in a
// separator (e.g. "/var/log/", "logs//", "C:/"). Folders opened via
// drag-drop (and some restore/drop paths) carry a trailing slash, so the
// naive QFileInfo(path).fileName() -- duplicated across the tab/widget/
// session label code -- produced blank tab and window titles. QDir::cleanPath
// normalises the separators first; if the result still has no base name
// (root "/", empty input, a bare drive "C:"), fall back to the original path
// so a non-empty input never yields an empty label.
inline QString displayNameForPath( const QString& path )
{
    const auto base = QFileInfo( QDir::cleanPath( path ) ).fileName();
    return base.isEmpty() ? path : base;
}

// Normalize characters in an automatically suggested filename stem, not an existing
// path or a user-selected destination. The caller owns the directory and extension.
QString suggestedFileNameStem( const QString& label, const QString& fallback );

} // namespace klogg

#endif
