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

#include "folderenumeration.h"

#include "mergefileorder.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStringList>

std::vector<QString> enumerateFolderFiles( const QString& folder,
                                            const FolderEnumerationOptions& options )
{
    if ( folder.isEmpty() ) {
        return {};
    }
    if ( !QFileInfo( folder ).isDir() ) {
        return {};
    }

    QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot | QDir::Readable;
    if ( !options.followSymlinks ) {
        filters |= QDir::NoSymLinks;
    }

    QStringList collected;
    collected.reserve( 64 );
    QDirIterator iterator( folder, filters, QDirIterator::Subdirectories );
    while ( iterator.hasNext() ) {
        const QString path = iterator.next();
        // QDir::NoSymLinks is best-effort and is not honoured consistently on
        // Windows; skip symlinks explicitly so enumeration never follows a link
        // when the caller did not ask to.
        if ( !options.followSymlinks && QFileInfo( path ).isSymLink() ) {
            continue;
        }
        collected << path;
    }

    return sortedMergeFilePaths( collected );
}
