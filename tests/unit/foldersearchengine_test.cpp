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

#include "foldersearchengine.h"
#include "foldersearchtypes.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {
QString writeFile( const QTemporaryDir& dir, const QString& name, const QByteArray& bytes )
{
    const QString path = QDir( dir.path() ).absoluteFilePath( name );
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( bytes );
    f.close();
    return path;
}

std::unique_ptr<PatternMatcher> matcherFor( const QString& pattern )
{
    RegularExpression re{ RegularExpressionPattern{ pattern } };
    REQUIRE( re.isValid() );
    return re.createMatcher();
}
} // namespace

TEST_CASE( "scanFile finds known matches with correct line numbers and offsets", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "foo\n"        -> line 0, bytes [0,4)
    // "bar ERROR\n"  -> line 1, "bar ERROR" is bytes [4,13), newline at 13
    // "baz\n"        -> line 2
    const QString path = writeFile( dir, "a.log", QByteArray( "foo\nbar ERROR\nbaz\n" ) );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.filePath == path );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 4 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 14 ) ); // past the newline
    REQUIRE( group.matches[ 0 ].lineLength == 9_length ); // "bar ERROR"
}

TEST_CASE( "scanFile matches across multiple lines", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\nnope\nMATCH again\n" ) );

    auto matcher = matcherFor( "MATCH" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.matches.size() == 2 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].localLine == 2_lnum );
}

TEST_CASE( "scanFile skips binary files (grep -I)", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    QByteArray content = QByteArray( "ERROR line\n" );
    content.append( '\0' ); // NUL within the first 32 KB => binary
    content.append( "more ERROR\n" );
    const QString path = writeFile( dir, "bin.log", content );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.matches.empty() );
}

TEST_CASE( "scanFile handles a line spanning multiple read blocks", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A single long line (>16 B block size) containing a match, then a newline.
    const QByteArray head( 20, 'x' );
    const QByteArray tail( 20, 'y' );
    const QByteArray content = head + " NEEDLE " + tail + "\n";
    const QString path = writeFile( dir, "long.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 16 ); // tiny block size

    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 0 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( content.size() ) );
}

TEST_CASE( "scanFile handles a final line without trailing newline", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "foo\nMATCH" ) ); // no trailing \n

    auto matcher = matcherFor( "MATCH" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 4 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 9 ) ); // EOF
}

TEST_CASE( "FolderSearchEngine aggregates multiple files by file then line", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString aPath = writeFile( dir, "a.log", QByteArray( "x MATCH\ny\nMATCH z\n" ) );
    const QString bPath = writeFile( dir, "b.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ aPath, bPath }, RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 2 );
    REQUIRE( results[ 0 ].filePath == aPath );
    REQUIRE( results[ 0 ].matches.size() == 2 );
    REQUIRE( results[ 0 ].matches[ 0 ].localLine == 0_lnum );
    REQUIRE( results[ 0 ].matches[ 1 ].localLine == 2_lnum );
    REQUIRE( results[ 1 ].filePath == bPath );
    REQUIRE( results[ 1 ].matches.size() == 1 );
    REQUIRE( engine.matchCount() == 3 );
}

TEST_CASE( "FolderSearchEngine drops files with no matches", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString aPath = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );
    const QString bPath = writeFile( dir, "b.log", QByteArray( "nothing here\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ aPath, bPath }, RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 1 );
    REQUIRE( results[ 0 ].filePath == aPath );
}

TEST_CASE( "FolderSearchEngine emits searchStarted and searchFinished", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    QSignalSpy startedSpy( &engine, &FolderSearchEngine::searchStarted );
    QSignalSpy finishedSpy( &engine, &FolderSearchEngine::searchFinished );
    const auto gen = engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern( "MATCH" ) );

    REQUIRE( startedSpy.count() == 1 );
    REQUIRE( finishedSpy.count() == 1 );
    REQUIRE( startedSpy.takeFirst()[ 0 ].toULongLong() == gen );
    REQUIRE( finishedSpy.takeFirst()[ 0 ].toULongLong() == gen );
}

TEST_CASE( "FolderSearchEngine stale generation produces no new results", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.takeResults().size() == 1 );

    engine.bumpGeneration(); // a newer search is now current
    engine.runSearch( engine.currentGeneration() - 1, QStringList{ path },
                      RegularExpressionPattern( "MATCH" ) ); // stale gen -> no-op
    REQUIRE( engine.takeResults().empty() );

    // A current-generation run DOES refill results.
    engine.runSearch( engine.currentGeneration(), QStringList{ path },
                      RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.takeResults().size() == 1 );
}

TEST_CASE( "FolderSearchEngine interrupt stops the scan", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.bumpGeneration();
    engine.interrupt();
    engine.runSearch( engine.currentGeneration(), QStringList{ path },
                      RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.takeResults().empty() );
}
