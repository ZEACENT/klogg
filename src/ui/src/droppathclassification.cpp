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

#include "droppathclassification.h"

#include "mergefileorder.h"

#include <QFileInfo>

DroppedPathClassification classifyLocalPaths( const QStringList& localPaths )
{
    DroppedPathClassification result;

    for ( const auto& path : localPaths ) {
        if ( QFileInfo( path ).isDir() ) {
            result.dirs.append( path );
        }
        else {
            result.files.append( path );
        }
    }

    const auto sortToList = []( const QStringList& paths ) -> QStringList {
        const auto sorted = sortedMergeFilePaths( paths );
        return QStringList{ sorted.begin(), sorted.end() };
    };
    result.dirs = sortToList( result.dirs );
    result.files = sortToList( result.files );

    return result;
}

bool isDirectoryPath( const QString& path )
{
    const QFileInfo info( path );
    if ( info.isDir() ) {
        return true;
    }
    // QFileInfo::isDir() does not resolve a symlink-to-directory on every
    // platform (notably Windows); resolve the symlink explicitly so a link to a
    // directory is still classified as a directory.
    const QString canonical = info.canonicalFilePath();
    return !canonical.isEmpty() && QFileInfo( canonical ).isDir();
}
