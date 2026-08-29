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

#ifndef FOLDERENUMERATION_H
#define FOLDERENUMERATION_H

#include <QString>
#include <functional>
#include <vector>

struct FolderEnumerationOptions {
    // When false (the default), symlinks are not listed (QDir::NoSymLinks) and
    // symlinked directories are not descended into. Matches grep's default.
    bool followSymlinks = false;
};

// Recursively lists every regular file under `folder`, natural-sorted by file
// name (reusing sortedMergeFilePaths so folder results appear in the same
// dictionary order as the Merge Files dialog). Returns an empty vector if
// `folder` is empty or not a directory. Binary files are NOT filtered here --
// the search engine skips them (grep -I) at scan time.
using FolderEnumerationStopPredicate = std::function<bool()>;

std::vector<QString> enumerateFolderFiles(
    const QString& folder, const FolderEnumerationOptions& options = {},
    FolderEnumerationStopPredicate shouldStop = {} );

#endif // FOLDERENUMERATION_H
