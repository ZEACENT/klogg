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

#include <QFileInfo>
#include <algorithm>
#include <iterator>
#include <utility>

namespace {
// Portable case-insensitive NATURAL compare (digit runs compared by numeric
// value so "file2" < "file10", alpha runs compared case-insensitively). We do
// NOT use QCollator::setNumericMode here: it is silently ignored on some Linux
// Qt builds (no ICU), which made folder enumeration order platform-dependent
// and broke the natural-sort unit tests on Linux CI.
int naturalCompare( const QString& a, const QString& b )
{
    int i = 0, j = 0;
    const int na = static_cast<int>( a.size() ), nb = static_cast<int>( b.size() );
    while ( i < na && j < nb ) {
        const bool aDigit = a[ i ].isDigit();
        const bool bDigit = b[ j ].isDigit();
        if ( aDigit && bDigit ) {
            // Compare the two digit runs as numbers: strip leading zeros, then
            // compare by effective length (more digits = larger), then lexically.
            const int ai = i, bj = j;
            int zi = i, zj = j;
            while ( i < na && a[ i ].isDigit() ) ++i;
            while ( j < nb && b[ j ].isDigit() ) ++j;
            while ( zi < i - 1 && a[ zi ] == QLatin1Char( '0' ) ) ++zi;
            while ( zj < j - 1 && b[ zj ] == QLatin1Char( '0' ) ) ++zj;
            const int effA = i - zi, effB = j - zj;
            if ( effA != effB ) {
                return effA < effB ? -1 : 1;
            }
            for ( ; zi < i && zj < j; ++zi, ++zj ) {
                if ( a[ zi ] != b[ zj ] ) {
                    return a[ zi ].unicode() < b[ zj ].unicode() ? -1 : 1;
                }
            }
            (void)ai;
            (void)bj;
        }
        else {
            const int ca = a[ i ].toLower().unicode();
            const int cb = b[ j ].toLower().unicode();
            if ( ca != cb ) {
                return ca < cb ? -1 : 1;
            }
            ++i;
            ++j;
        }
    }
    if ( i < na ) {
        return 1; // b is a prefix of a -> a sorts after
    }
    if ( j < nb ) {
        return -1; // a is a prefix of b -> a sorts before
    }
    return 0;
}
} // namespace

std::vector<QString> sortedMergeFilePaths( const QStringList& filePaths )
{
    // QFileInfo construction is filesystem-aware and comparatively expensive;
    // derive each basename once rather than O(N log N) times in the comparator.
    struct SortEntry {
        QString path;
        QString fileName;
    };

    std::vector<SortEntry> entries;
    entries.reserve( static_cast<size_t>( filePaths.size() ) );
    std::transform( filePaths.cbegin(), filePaths.cend(), std::back_inserter( entries ),
                    []( const QString& path ) {
                        return SortEntry{ path, QFileInfo( path ).fileName() };
                    } );

    // Case-insensitive natural sort by file name (numeric awareness, so
    // "file2" < "file10"), with the full path as a deterministic tiebreaker when
    // two files share a name.
    std::stable_sort( entries.begin(), entries.end(),
                      []( const SortEntry& left, const SortEntry& right ) {
                          const int cmp = naturalCompare( left.fileName, right.fileName );
                          return cmp != 0 ? cmp < 0 : left.path < right.path;
                      } );

    std::vector<QString> sortedPaths;
    sortedPaths.reserve( entries.size() );
    std::transform( entries.begin(), entries.end(), std::back_inserter( sortedPaths ),
                    []( SortEntry& entry ) { return std::move( entry.path ); } );
    return sortedPaths;
}
