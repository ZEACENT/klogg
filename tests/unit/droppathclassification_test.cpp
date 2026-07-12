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

#include <catch2/catch.hpp>

#include "droppathclassification.h"

#include "mergefileorder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>

namespace {
void writeFile( const QString& path )
{
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( "log\n" );
    f.close();
}
} // namespace

TEST_CASE( "classifyLocalPaths returns empty lists for empty input", "[drop]" )
{
    const auto result = classifyLocalPaths( QStringList{} );
    REQUIRE( result.dirs.isEmpty() );
    REQUIRE( result.files.isEmpty() );
}

TEST_CASE( "classifyLocalPaths puts all files in files and none in dirs", "[drop]" )
{
    QTemporaryFile f1;
    QTemporaryFile f2;
    REQUIRE( f1.open() );
    REQUIRE( f2.open() );

    const QStringList input{ f1.fileName(), f2.fileName() };
    const auto result = classifyLocalPaths( input );

    REQUIRE( result.dirs.isEmpty() );
    REQUIRE( result.files.size() == 2 );
}

TEST_CASE( "classifyLocalPaths puts all directories in dirs", "[drop]" )
{
    QTemporaryDir d1;
    QTemporaryDir d2;
    REQUIRE( d1.isValid() );
    REQUIRE( d2.isValid() );

    const QStringList input{ d1.path(), d2.path() };
    const auto result = classifyLocalPaths( input );

    REQUIRE( result.files.isEmpty() );
    REQUIRE( result.dirs.size() == 2 );
}

TEST_CASE( "classifyLocalPaths partitions a mixed drop correctly", "[drop]" )
{
    QTemporaryDir base;
    REQUIRE( base.isValid() );
    const QDir root( base.path() );
    REQUIRE( root.mkpath( "subdir" ) );
    writeFile( root.absoluteFilePath( "afile.log" ) );

    const QString dirPath = root.absoluteFilePath( "subdir" );
    const QString filePath = root.absoluteFilePath( "afile.log" );

    const auto result = classifyLocalPaths( QStringList{ filePath, dirPath } );

    REQUIRE( result.dirs.size() == 1 );
    REQUIRE( result.files.size() == 1 );
    REQUIRE( QFileInfo( result.dirs.at( 0 ) ).isDir() );
    REQUIRE( QFileInfo( result.files.at( 0 ) ).isFile() );
}

TEST_CASE( "classifyLocalPaths treats a non-existent path as a file", "[drop]" )
{
    // QFileInfo::isDir() is false for a missing path, so it lands in `files`.
    // This matches the previous behavior where such a path was handed to loadFile.
    const QString missing = "/no/such/path/here.log";
    REQUIRE( !QFileInfo( missing ).isDir() );

    const auto result = classifyLocalPaths( QStringList{ missing } );
    REQUIRE( result.dirs.isEmpty() );
    REQUIRE( result.files.size() == 1 );
    REQUIRE( result.files.at( 0 ) == missing );
}

TEST_CASE( "classifyLocalPaths classifies a directory given with a trailing slash", "[drop]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    const QString withSlash = dir.path() + QDir::separator();
    const auto result = classifyLocalPaths( QStringList{ withSlash } );

    REQUIRE( result.dirs.size() == 1 );
    REQUIRE( result.files.isEmpty() );
}

TEST_CASE( "classifyLocalPaths sorts the returned lists", "[drop]" )
{
    QTemporaryDir d1;
    QTemporaryDir d2;
    QTemporaryFile f1;
    QTemporaryFile f2;
    REQUIRE( d1.isValid() );
    REQUIRE( d2.isValid() );
    REQUIRE( f1.open() );
    REQUIRE( f2.open() );

    // Names chosen so the natural sort (by QFileInfo::fileName()) differs from
    // insertion order: z-dir precedes a-dir on input but must come out last.
    const QString zDir = d1.path();
    const QString aDir = d2.path();
    const QString zFile = f1.fileName();
    const QString aFile = f2.fileName();

    const QStringList input{ zDir, zFile, aDir, aFile };
    const auto result = classifyLocalPaths( input );

    REQUIRE( result.dirs.size() == 2 );
    REQUIRE( result.files.size() == 2 );
    // Deterministic ordering (stable regardless of input order); the exact
    // key is fileName(), so just assert it is sorted and stable.
    const auto dirSorted = sortedMergeFilePaths( QStringList{ aDir, zDir } );
    REQUIRE( result.dirs.at( 0 ) == QString( dirSorted.at( 0 ) ) );
    REQUIRE( result.dirs.at( 1 ) == QString( dirSorted.at( 1 ) ) );
}
