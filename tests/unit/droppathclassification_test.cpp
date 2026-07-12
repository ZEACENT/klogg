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
    // Independent verification of the documented contract: the returned lists
    // are sorted by fileName() in case-insensitive natural (numeric) order, NOT
    // by raw full-path string. We use controlled leaf names under one base
    // temp dir so the expected positions are known a priori and independent of
    // the random hex prefixes QTemporaryDir/QTemporaryFile produce.
    //
    // This deliberately does NOT call sortedMergeFilePaths (the helper
    // classifyLocalPaths delegates to internally) -- asserting against it
    // would be circular: a change that breaks the real contract but stays
    // self-consistent would still pass.
    QTemporaryDir base;
    REQUIRE( base.isValid() );
    const QDir root( base.path() );
    REQUIRE( root.mkpath( QStringLiteral( "adir" ) ) );
    REQUIRE( root.mkpath( QStringLiteral( "zdir" ) ) );
    writeFile( root.absoluteFilePath( QStringLiteral( "afile.log" ) ) );
    writeFile( root.absoluteFilePath( QStringLiteral( "zfile.log" ) ) );

    const QString aDir = root.absoluteFilePath( QStringLiteral( "adir" ) );
    const QString zDir = root.absoluteFilePath( QStringLiteral( "zdir" ) );
    const QString aFile = root.absoluteFilePath( QStringLiteral( "afile.log" ) );
    const QString zFile = root.absoluteFilePath( QStringLiteral( "zfile.log" ) );

    // Feed the input interleaved and z-before-a so a non-sorting implementation
    // would emit the input order verbatim and fail the positional checks below.
    const QStringList input{ zDir, zFile, aDir, aFile };
    const auto result = classifyLocalPaths( input );

    REQUIRE( result.dirs.size() == 2 );
    REQUIRE( result.files.size() == 2 );

    // Explicit positional assertions keyed on fileName() -- the real contract.
    REQUIRE( QFileInfo( result.dirs.at( 0 ) ).fileName() == QStringLiteral( "adir" ) );
    REQUIRE( QFileInfo( result.dirs.at( 1 ) ).fileName() == QStringLiteral( "zdir" ) );
    REQUIRE( QFileInfo( result.files.at( 0 ) ).fileName() == QStringLiteral( "afile.log" ) );
    REQUIRE( QFileInfo( result.files.at( 1 ) ).fileName() == QStringLiteral( "zfile.log" ) );

    // Determinism: the sort is stable regardless of input ordering.
    const auto resultRev = classifyLocalPaths( QStringList{ aFile, aDir, zFile, zDir } );
    REQUIRE( resultRev.dirs == result.dirs );
    REQUIRE( resultRev.files == result.files );
}

TEST_CASE( "isDirectoryPath returns true for a directory", "[drop]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    REQUIRE( isDirectoryPath( dir.path() ) );
}

TEST_CASE( "isDirectoryPath returns true for a directory with a trailing separator", "[drop]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    const QString withSlash = dir.path() + QDir::separator();
    REQUIRE( isDirectoryPath( withSlash ) );
}

TEST_CASE( "isDirectoryPath returns false for a file", "[drop]" )
{
    QTemporaryFile file;
    REQUIRE( file.open() );

    REQUIRE( !isDirectoryPath( file.fileName() ) );
}

TEST_CASE( "isDirectoryPath returns false for a non-existent path", "[drop]" )
{
    // QFileInfo::isDir() is false for a missing path, so the loadFile guard
    // will NOT route it to openFolderByPath -- matching classifyLocalPaths,
    // which also puts a missing path in `files`.
    const QString missing = "/no/such/path/here";
    REQUIRE( !isDirectoryPath( missing ) );
}

TEST_CASE( "isDirectoryPath follows a symlink to a directory", "[drop]" )
{
    QTemporaryDir target;
    REQUIRE( target.isValid() );
    // Create the symlink in a temp file path so it auto-cleans.
    QTemporaryFile linkHolder;
    REQUIRE( linkHolder.open() );
    const QString linkPath = linkHolder.fileName() + "_link";
    linkHolder.close();
    QFile::remove( linkPath );

    if ( QFile::link( target.path(), linkPath ) ) {
        // QFileInfo::isDir() follows symlinks, so a symlink-to-dir counts as a dir.
        REQUIRE( isDirectoryPath( linkPath ) );
        QFile::remove( linkPath );
    }
    else {
        // Some CI sandboxes forbid symlink creation; document the contract instead.
        INFO( "symlink creation not permitted in this environment; skipping" );
    }
}
