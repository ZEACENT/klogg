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

#ifndef MERGEFILEORDER_H
#define MERGEFILEORDER_H

#include <QString>
#include <QStringList>
#include <vector>

// Returns the given file paths sorted in dictionary order for display in the
// "Merge Files" dialog. Sorting is case-insensitive and natural (numeric mode,
// so "file2" sorts before "file10"), keyed by the displayed file name with the
// full path used as a deterministic tiebreaker when two files share a name.
std::vector<QString> sortedMergeFilePaths( const QStringList& filePaths );

#endif // MERGEFILEORDER_H
