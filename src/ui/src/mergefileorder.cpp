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

#include "mergefileorder.h"

#include <QCollator>
#include <QFileInfo>
#include <algorithm>

std::vector<QString> sortedMergeFilePaths( const QStringList& filePaths )
{
    // Dictionary order: case-insensitive natural sort (numeric mode, so
    // "file2" < "file10"), keyed by the displayed file name with the full path
    // as a deterministic tiebreaker when two files share a name.
    QCollator collator;
    collator.setCaseSensitivity( Qt::CaseInsensitive );
    collator.setNumericMode( true );

    std::vector<QString> sortedPaths( filePaths.begin(), filePaths.end() );
    std::stable_sort( sortedPaths.begin(), sortedPaths.end(),
                      [ &collator ]( const QString& left, const QString& right ) {
                          const int cmp = collator.compare( QFileInfo( left ).fileName(),
                                                            QFileInfo( right ).fileName() );
                          return cmp != 0 ? cmp < 0 : left < right;
                      } );
    return sortedPaths;
}
