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

#ifndef DROPPATHCLASSIFICATION_H
#define DROPPATHCLASSIFICATION_H

#include <QStringList>

// Result of partitioning a set of dropped local paths: `dirs` are paths that
// resolve to a directory (and should be routed through the folder-open flow),
// `files` are everything else (treated as regular files / merge candidates).
// Each list is sorted via sortedMergeFilePaths so the behavior is deterministic
// regardless of the order the drop event delivered the URLs in.
//
// Note: QFileInfo::isDir() follows symlinks, so a dropped symlink-to-dir is
// classified as a directory. A non-existent path is NOT a directory, so it lands
// in `files` -- matching the previous behavior where such a path was handed to
// loadFile.
struct DroppedPathClassification {
    QStringList dirs;
    QStringList files;
};

// Partitions `localPaths` into directories and files. Non-local / empty URLs
// must already have been filtered out by the caller (dropEvent does this).
DroppedPathClassification classifyLocalPaths( const QStringList& localPaths );

#endif // DROPPATHCLASSIFICATION_H
