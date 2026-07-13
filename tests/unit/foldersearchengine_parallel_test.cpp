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

// Unit tests for the parallel (bounded std::thread pool) scan path in
// FolderSearchEngine. Verifies that scanSynchronously returns groups in
// enumeration order via takeResults regardless of which worker finishes first,
// that match counts are exact under concurrency, and that interrupt halts the
// pool promptly.

#include <catch2/catch.hpp>

#include "foldersearchengine.h"
#include "foldersearchtypes.h"
#include "regularexpressionpattern.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
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
} // namespace

TEST_CASE( "FolderSearchEngine parallel scan preserves enumeration order", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    // a.log is large so the workers handling the tiny z*.log files finish well
    // before a.log. Regardless of completion order, takeResults() must return
    // every group strictly in enumeration (file-list) order.
    QByteArray big;
    big.reserve( 256 * 1024 );
    for ( int i = 0; i < 8000; ++i ) {
        big += "ERROR line " + QByteArray::number( i ) + "\n";
    }
    const QString aPath = writeFile( dir, "a.log", big );

    QStringList allPaths;
    allPaths << aPath;
    const int tinyCount = 8;
    for ( int i = 0; i < tinyCount; ++i ) {
        allPaths << writeFile( dir, QString( "z%1.log" ).arg( i ), QByteArray( "ERROR\n" ) );
    }

    const quint64 expectedMatches = 8000u + static_cast<quint64>( tinyCount );

    // Run several times to confirm run-to-run determinism (the commit order is
    // driven by the index cursor, never by completion timing).
    for ( int run = 0; run < 5; ++run ) {
        FolderSearchEngine engine;
        engine.scanSynchronously( allPaths, RegularExpressionPattern( "ERROR" ) );
        const auto results = engine.takeResults();

        REQUIRE( results.size() == static_cast<size_t>( allPaths.size() ) );
        for ( int i = 0; i < static_cast<int>( allPaths.size() ); ++i ) {
            INFO( "run " << run << " index " << i );
            REQUIRE( results[ static_cast<size_t>( i ) ].filePath == allPaths[ i ] );
        }
        // Exact match count: no double-count, no drops under concurrency.
        REQUIRE( engine.matchCount() == expectedMatches );
    }
}

TEST_CASE( "FolderSearchEngine interrupt stops the parallel pool", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    // Many files so the bounded pool is actually used.
    QStringList paths;
    for ( int i = 0; i < 50; ++i ) {
        paths << writeFile( dir, QString( "f%1.log" ).arg( i ), QByteArray( "ERROR\n" ) );
    }

    FolderSearchEngine engine;
    engine.bumpGeneration();
    engine.interrupt(); // workers see shouldStop() true and return promptly
    engine.runSearch( engine.currentGeneration(), paths, RegularExpressionPattern( "ERROR" ) );

    // Interrupted scan -> no committed results.
    REQUIRE( engine.takeResults().empty() );
}
