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

// End-to-end integration tests for the folder-search UI: drive a real
// FolderCrawlerWidget in the Qt event loop, run an async folder search, and
// assert the grouped-results display, the main-view-opens-on-select flow, and
// collapse/expand. Runs in klogg_itests (QApplication + product-like regex
// engine, registered LineNumber/LinesCount metatypes for queued signals).

#include <catch2/catch.hpp>

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

#include "foldercrawlerwidget.h"
#include "foldersearchresults.h"
#include "linetypes.h"

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

// Pump the event loop in small steps until `predicate` is true or `timeoutMs`
// elapses. Needed because the folder search runs on a worker thread and
// delivers its results via a queued signal processed by the event loop.
bool waitFor( const std::function<bool()>& predicate, int timeoutMs = 5000 )
{
    QElapsedTimer t;
    t.start();
    while ( !predicate() ) {
        if ( t.elapsed() > timeoutMs ) {
            return false;
        }
        QTest::qWait( 50 );
    }
    return true;
}
} // namespace

TEST_CASE( "FolderCrawlerWidget search populates grouped results", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );

    REQUIRE( waitFor( [ & ]() { return widget.folderResults()->getNbLine().get() > 0; } ) );

    // a: header + 2 matches = 3 rows; b: header + 1 match = 2 rows; total 5.
    REQUIRE( widget.folderResults()->getNbLine() == 5_lcount );
    REQUIRE( widget.folderResults()->groupCount() == 2 );
    REQUIRE( widget.folderResults()->lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Data );
    // Main view is empty until a row is selected.
    REQUIRE( widget.currentMainFilePath().isEmpty() );
}

TEST_CASE( "FolderCrawlerWidget selecting a result opens the source file", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );

    REQUIRE( waitFor( [ & ]() { return widget.folderResults()->getNbLine().get() > 0; } ) );
    // Visible rows: [header(0), match(1)].
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Data );

    widget.selectResultRow( 1_lnum ); // opens a.log at the matched line

    REQUIRE( waitFor( [ & ]() { return !widget.currentMainFilePath().isEmpty(); } ) );
    REQUIRE( widget.currentMainFilePath() == a );
}

TEST_CASE( "FolderCrawlerWidget reselecting the same file reuses it without reload churn",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return widget.folderResults()->getNbLine().get() > 0; } ) );

    widget.selectResultRow( 1_lnum ); // first select -> async load + swap
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );

    // Selecting another row in the SAME file must keep the same file loaded.
    widget.selectResultRow( 2_lnum );
    QTest::qWait( 100 );
    REQUIRE( widget.currentMainFilePath() == a );
}

TEST_CASE( "FolderCrawlerWidget collapse-all and expand-all change visible rows", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return widget.folderResults()->getNbLine().get() > 0; } ) );

    const auto expanded = widget.folderResults()->getNbLine(); // 2 headers + 3 matches = 5
    REQUIRE( expanded == 5_lcount );

    widget.collapseAll();
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == 2_lcount ); // only the 2 headers
    REQUIRE( widget.folderResults()->lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Header );

    widget.expandAll();
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == expanded );
}

TEST_CASE( "FolderCrawlerWidget header-click toggles a single group", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return widget.folderResults()->getNbLine().get() > 0; } ) );
    REQUIRE_FALSE( widget.folderResults()->isCollapsed( 0 ) );

    widget.clickHeaderRow( 0_lnum ); // toggle group a
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->isCollapsed( 0 ) );
    // a collapsed: a-header(1) + b(header+1) = 1 + 2 = 3 visible rows.
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );
    // The header text still reports the group's total match count.
    const auto headerText = widget.folderResults()->getLineString( 0_lnum );
    REQUIRE( headerText.contains( "2" ) );

    widget.clickHeaderRow( 0_lnum ); // expand again
    QTest::qWait( 100 );
    REQUIRE_FALSE( widget.folderResults()->isCollapsed( 0 ) );
}

TEST_CASE( "FolderCrawlerWidget search with no matches leaves results empty", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "nothing here\nstill nothing\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );

    // searchFinished fires (with 0 matches) but getNbLine stays 0.
    QTest::qWait( 200 );
    REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );
    REQUIRE( widget.currentMainFilePath().isEmpty() );
}
