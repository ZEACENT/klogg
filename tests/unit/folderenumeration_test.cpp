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

#include "folderenumeration.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {
void writeFile( const QString& path, const QByteArray& bytes = QByteArray( "log\n" ) )
{
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( bytes );
    f.close();
}
} // namespace

TEST_CASE( "enumerateFolderFiles lists nested files recursively", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QDir root( dir.path() );
    REQUIRE( root.mkpath( "sub/deep" ) );

    writeFile( root.absoluteFilePath( "c.log" ) );
    writeFile( root.absoluteFilePath( "sub/b.log" ) );
    writeFile( root.absoluteFilePath( "sub/deep/a.log" ) );

    const auto files = enumerateFolderFiles( dir.path() );
    REQUIRE( files.size() == 3 );
    // Natural sort by file name regardless of depth.
    REQUIRE( files[ 0 ].endsWith( "/a.log" ) );
    REQUIRE( files[ 1 ].endsWith( "/b.log" ) );
    REQUIRE( files[ 2 ].endsWith( "/c.log" ) );
}

TEST_CASE( "enumerateFolderFiles natural-sorts file2 before file10", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QDir root( dir.path() );
    writeFile( root.absoluteFilePath( "file10.log" ) );
    writeFile( root.absoluteFilePath( "file2.log" ) );
    writeFile( root.absoluteFilePath( "file1.log" ) );

    const auto files = enumerateFolderFiles( dir.path() );
    REQUIRE( files.size() == 3 );
    REQUIRE( files[ 0 ].endsWith( "/file1.log" ) );
    REQUIRE( files[ 1 ].endsWith( "/file2.log" ) );
    REQUIRE( files[ 2 ].endsWith( "/file10.log" ) );
}

TEST_CASE( "enumerateFolderFiles only returns regular files", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QDir root( dir.path() );
    REQUIRE( root.mkpath( "subdir" ) ); // a directory, must not appear
    writeFile( root.absoluteFilePath( "only.log" ) );

    const auto files = enumerateFolderFiles( dir.path() );
    REQUIRE( files.size() == 1 );
    REQUIRE( files[ 0 ].endsWith( "/only.log" ) );
    for ( const auto& f : files ) {
        REQUIRE( QFileInfo( f ).isFile() );
    }
}

TEST_CASE( "enumerateFolderFiles does not follow symlinks by default", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QDir root( dir.path() );
    writeFile( root.absoluteFilePath( "real.log" ) );
    // link.log -> real.log. Symlink creation needs privilege on some platforms
    // (notably Windows without developer mode); skip cleanly when unsupported.
    if ( !QFile::link( root.absoluteFilePath( "real.log" ),
                       root.absoluteFilePath( "link.log" ) )
         || !QFileInfo( root.absoluteFilePath( "link.log" ) ).isSymLink() ) {
        INFO( "symlink creation not permitted in this environment; skipping" );
        return;
    }

    const auto files = enumerateFolderFiles( dir.path() );
    REQUIRE( files.size() == 1 );
    REQUIRE( files[ 0 ].endsWith( "/real.log" ) );
}

TEST_CASE( "enumerateFolderFiles returns empty for a non-directory path", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    writeFile( dir.path() + QDir::separator() + "afile.log" );

    const auto files = enumerateFolderFiles( dir.path() + QDir::separator() + "afile.log" );
    REQUIRE( files.empty() );
    REQUIRE( enumerateFolderFiles( QString() ).empty() );
}
