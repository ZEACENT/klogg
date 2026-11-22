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

#include "foldersearchresults.h"
#include "foldersearchengine.h"
#include "regularexpression.h"
#include "foldersearchtypes.h"
#include "reviewmarks.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCodec>

#include <atomic>
#include <thread>

namespace {
klogg::folder::MatchRecord match( uint64_t localLine, int64_t start, int64_t end,
                                  int expandedLen, int matchLen )
{
    klogg::folder::MatchRecord m;
    m.localLine = LineNumber( localLine );
    m.lineStartByte = OffsetInFile( start );
    m.lineEndByte = OffsetInFile( end );
    m.lineLength = LineLength( expandedLen );
    m.matchLen = LineLength( matchLen );
    return m;
}

// Writes the given lines (joined by '\n', with a trailing newline) to a temp
// file and returns its absolute path. Used to exercise the on-demand text fetch.
QString writeFile( const QTemporaryDir& dir, const QString& name, const QStringList& lines )
{
    const QString path = QDir( dir.path() ).absoluteFilePath( name );
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( lines.join( '\n' ).toUtf8() );
    f.write( "\n" );
    f.close();
    return path;
}
} // namespace

TEST_CASE( "Complete folder replacement prepares privately and publishes retained membership once",
           "[folder][folder-search-publication]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto oldPath = writeFile( dir, "old.log", { "MATCH old" } );
    const auto newPath = writeFile( dir, "new.log", { "MATCH new" } );
    const auto markedPath = writeFile( dir, "marked.log", { "old bookmark" } );
    FolderSearchEngine engine;
    engine.scanSynchronously( { oldPath, markedPath }, RegularExpressionPattern{ "MATCH" } );
    FolderSearchResults::MarksStore marks{ { markedPath, { 0 } } };
    FolderSearchResults results;
    results.setResults( engine.takeResults() );
    const auto oldSnapshot = results.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( oldSnapshot );
    const auto oldIdentity = oldSnapshot->row( 0_lnum ).identity;
    results.setMarksStore( &marks );
    // Legacy unrestricted marks would display this excluded path; the completed
    // search must restrict presentation without deleting the shared bookmark.
    marks[ oldPath ].insert( 0 );
    results.refreshForMarksChange();
    const auto originalMarks = marks;
    REQUIRE( results.getLineString( 3_lnum ) == "old bookmark" );
    writeFile( dir, "marked.log", { "retained bookmark", "later bookmark" } );
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    const auto captured = results.captureSearchReplacement();
    REQUIRE( captured );
    const auto generation = engine.scanSynchronously( { newPath, markedPath }, RegularExpressionPattern{ "MATCH" } );
    auto completed = engine.takeCompletedSearch( generation );
    REQUIRE( completed );
    REQUIRE( completed->status == FolderSearchEngine::CompletionStatus::Ready );
    const auto source = completed->groups.back().content;
    REQUIRE( source );
    const auto bytesRead = source->bytesReadForTesting();
    std::shared_ptr<FolderSearchResults::PreparedSearchReplacement> prepared;
    std::thread worker( [&] {
        prepared = FolderSearchResults::prepareSearchReplacement( captured, std::move( *completed ) );
    } );
    worker.join();
    REQUIRE( prepared );
    CHECK( results.containsRowIdentity( oldIdentity ) );
    CHECK( changed.count() == 0 );
    auto publication = results.lockSearchReplacement( prepared );
    REQUIRE( publication );
    CHECK( publication->install() );
    CHECK_FALSE( publication->install() );
    publication.reset();
    CHECK( changed.count() == 0 );
    CHECK( marks == originalMarks );
    CHECK_FALSE( results.containsRowIdentity( oldIdentity ) );
    CHECK( source->bytesReadForTesting() == bytesRead );
    CHECK_FALSE( results.lockSearchReplacement( prepared ) );
    bool observed = false;
    QObject::connect( &results, &FolderSearchResults::layoutChanged, &results, [&] {
        observed = results.groupCount() == 1 && results.getNbLine() == 4_lcount;
    } );
    results.layoutChanged();
    CHECK( observed );
    CHECK( changed.count() == 1 );
    REQUIRE( QFile::remove( oldPath ) );
    REQUIRE( QFile::remove( newPath ) );
    REQUIRE( QFile::remove( markedPath ) );
    CHECK( oldSnapshot->lineString( 0_lnum ) == "MATCH old" );
    CHECK( results.getLineString( 1_lnum ) == "MATCH new" );
    CHECK( results.getLineString( 3_lnum ) == "retained bookmark" );
    CHECK( results.containsRowIdentity( source->identity( 1_lnum ) ) );
    marks[ markedPath ].insert( 1 );
    results.refreshForMarksChange();
    CHECK( results.getLineString( 4_lnum ) == "later bookmark" );
}

TEST_CASE( "Complete folder publication rejects changed owners and discarded candidates stay private",
           "[folder][folder-search-publication]" )
{
    FolderSearchResults::MarksStore marks;
    FolderSearchResults::MarksStore otherMarks;
    auto results = std::make_unique<FolderSearchResults>();
    results->setMarksStore( &marks );
    const auto captured = results->captureSearchReplacement();
    REQUIRE( captured );
    FolderSearchEngine::CompletedSearch empty;
    empty.status = FolderSearchEngine::CompletionStatus::Ready;
    const auto prepared = FolderSearchResults::prepareSearchReplacement( captured, std::move( empty ) );
    REQUIRE( prepared );
    SECTION( "marks changed without refresh" ) { marks[ "absent.log" ].insert( 0 ); }
    SECTION( "marks rebound" ) { results->setMarksStore( &otherMarks ); }
    SECTION( "visibility changed" ) { results->setVisibility( FolderSearchResults::Visibility::Matches ); }
    SECTION( "new empty search supersedes capture" ) { results->beginSearch( {} ); }
    SECTION( "other owner" ) {
        FolderSearchResults other;
        CHECK_FALSE( other.lockSearchReplacement( prepared ) );
        return;
    }
    SECTION( "discarded guard and owner-independent preparation" ) {
        QSignalSpy changed( results.get(), &FolderSearchResults::layoutChanged );
        auto publication = results->lockSearchReplacement( prepared );
        REQUIRE( publication );
        publication.reset();
        CHECK( changed.count() == 0 );
        REQUIRE( results->lockSearchReplacement( prepared ) );
        results.reset();
        FolderSearchEngine::CompletedSearch afterClose;
        afterClose.status = FolderSearchEngine::CompletionStatus::Ready;
        std::shared_ptr<FolderSearchResults::PreparedSearchReplacement> detached;
        std::thread worker( [&] {
            detached = FolderSearchResults::prepareSearchReplacement( captured, std::move( afterClose ) );
        } );
        worker.join();
        CHECK( detached );
        return;
    }
    QSignalSpy changed( results.get(), &FolderSearchResults::layoutChanged );
    CHECK_FALSE( results->lockSearchReplacement( prepared ) );
    CHECK( changed.count() == 0 );
}

TEST_CASE( "Complete folder replacement preserves collapse by path and discards old streaming buffers",
           "[folder][folder-search-publication]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto a = writeFile( dir, "a.log", { "MATCH a" } );
    const auto b = writeFile( dir, "b.log", { "MATCH b" } );
    FolderSearchEngine engine;
    engine.scanSynchronously( { a, b }, RegularExpressionPattern{ "MATCH" } );
    auto oldGroups = engine.takeResults();
    REQUIRE( oldGroups.size() == 2 );
    FolderSearchResults results;
    results.setResults( oldGroups );
    results.setCollapsed( 0, true );
    SECTION( "existing groups" ) {}
    SECTION( "pending re-search with a missing predecessor" ) {
        results.beginSearch( { a, b } );
        results.addFileGroup( 1, oldGroups.back() );
    }
    const auto captured = results.captureSearchReplacement();
    REQUIRE( captured );
    const auto generation = engine.scanSynchronously( { b, a }, RegularExpressionPattern{ "MATCH" } );
    auto complete = engine.takeCompletedSearch( generation );
    REQUIRE( complete );
    const auto prepared = FolderSearchResults::prepareSearchReplacement( captured, std::move( *complete ) );
    REQUIRE( prepared );
    SECTION( "human collapse edit rejects the captured replacement" ) {
        results.expandAll();
        CHECK_FALSE( results.lockSearchReplacement( prepared ) );
        CHECK( results.getNbLine() == 4_lcount );
        return;
    }
    auto publication = results.lockSearchReplacement( prepared );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication.reset();
    CHECK_FALSE( results.isCollapsed( 0 ) );
    CHECK( results.isCollapsed( 1 ) );
    CHECK( results.getNbLine() == 3_lcount );
    CHECK( results.sourceForLine( 0_lnum ).filePath == b );
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    results.flushPending();
    results.addFileGroup( 0, oldGroups.front() );
    CHECK( changed.count() == 0 );
    CHECK( results.groupCount() == 2 );
    CHECK( results.getNbLine() == 3_lcount );
}

TEST_CASE( "Finishing an empty folder search invalidates its pending collapse capture",
           "[folder][folder-search-publication][folder-collapse-capture]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = writeFile( dir, "a.log", { "MATCH" } );
    FolderSearchEngine engine;
    engine.scanSynchronously( { path }, RegularExpressionPattern{ "MATCH" } );
    FolderSearchResults results;
    results.setResults( engine.takeResults() );
    results.collapseAll();
    results.beginSearch( { path } );
    const auto captured = results.captureSearchReplacement();
    REQUIRE( captured );
    const auto generation = engine.scanSynchronously( { path }, RegularExpressionPattern{ "MATCH" } );
    auto completed = engine.takeCompletedSearch( generation );
    REQUIRE( completed );
    const auto prepared = FolderSearchResults::prepareSearchReplacement( captured, std::move( *completed ) );
    REQUIRE( prepared );
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    results.flushPending(); // Native completion discards pending collapse, without a layout change.
    REQUIRE( changed.count() == 0 );
    CHECK_FALSE( results.lockSearchReplacement( prepared ) );
    CHECK( results.getNbLine() == 0_lcount );
}

TEST_CASE( "Folder replacement distinguishes empty success from rejected or incomplete scans",
           "[folder][folder-search-publication]" )
{
    FolderSearchResults::MarksStore marks{ { "excluded.log", { 0 } } };
    FolderSearchResults results;
    results.setMarksStore( &marks );
    const auto before = results.getNbLine();
    REQUIRE( before != 0_lcount );
    const auto captured = results.captureSearchReplacement();
    REQUIRE( captured );
    FolderSearchEngine::CompletedSearch completed;
    bool accepted = false;
    SECTION( "empty success" ) { completed.status = FolderSearchEngine::CompletionStatus::Ready; accepted = true; }
    SECTION( "cancelled" ) { completed.status = FolderSearchEngine::CompletionStatus::Cancelled; }
    SECTION( "compile failure" ) { completed.status = FolderSearchEngine::CompletionStatus::CompileFailure; }
    SECTION( "enumeration failure" ) { completed.status = FolderSearchEngine::CompletionStatus::EnumerationFailure; }
    SECTION( "retention failure" ) { completed.status = FolderSearchEngine::CompletionStatus::RetentionFailure; }
    SECTION( "already streamed" ) { completed.status = FolderSearchEngine::CompletionStatus::ResultsConsumed; }
    SECTION( "missing member" ) {
        completed.status = FolderSearchEngine::CompletionStatus::Ready;
        completed.filePaths.push_back( "missing.log" );
    }
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    const auto prepared = FolderSearchResults::prepareSearchReplacement( captured, std::move( completed ) );
    CHECK( static_cast<bool>( prepared ) == accepted );
    CHECK( results.getNbLine() == before );
    if ( accepted ) {
        auto publication = results.lockSearchReplacement( prepared );
        REQUIRE( publication );
        REQUIRE( publication->install() );
        publication.reset();
        CHECK( results.getNbLine() == 0_lcount );
    }
    CHECK( marks.size() == 1 );
    CHECK( changed.count() == 0 );
}

TEST_CASE( "Folder prepared layouts install requested visibility without an intermediate rebuild",
           "[folder][folder-prepared-visibility]" )
{
    using Visibility = FolderSearchResults::Visibility;
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = writeFile( dir, "a.log", { "MATCH", "bookmark" } );
    FolderSearchEngine engine;
    engine.scanSynchronously( { path }, RegularExpressionPattern{ "MATCH" } );
    FolderSearchResults::MarksStore marks{ { path, { 1 } } };
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.setResults( engine.takeResults() );
    results.setVisibility( Visibility::Marks );
    REQUIRE( results.getNbLine() == 2_lcount );
    const auto originalMarks = marks;
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    SECTION( "existing results projection for a kept pane" ) {
        const auto captured = results.captureMarksLayout( marks, nullptr, Visibility::MarksAndMatches );
        REQUIRE( captured );
        const auto prepared = FolderSearchResults::prepareMarksLayout( captured );
        REQUIRE( prepared );
        CHECK( results.visibility() == Visibility::Marks );
        CHECK( results.getNbLine() == 2_lcount );
        auto publication = results.lockMarksLayout( prepared );
        REQUIRE( publication );
        REQUIRE( publication->install() );
        publication.reset();
    }
    SECTION( "complete search replacement" ) {
        const auto captured = results.captureSearchReplacement( Visibility::MarksAndMatches );
        REQUIRE( captured );
        const auto generation = engine.scanSynchronously( { path }, RegularExpressionPattern{ "MATCH" } );
        auto completed = engine.takeCompletedSearch( generation );
        REQUIRE( completed );
        const auto prepared = FolderSearchResults::prepareSearchReplacement( captured, std::move( *completed ) );
        REQUIRE( prepared );
        CHECK( results.visibility() == Visibility::Marks );
        CHECK( results.getNbLine() == 2_lcount );
        auto publication = results.lockSearchReplacement( prepared );
        REQUIRE( publication );
        REQUIRE( publication->install() );
        publication.reset();
    }
    CHECK( results.visibility() == Visibility::MarksAndMatches );
    CHECK( results.getNbLine() == 3_lcount );
    CHECK( marks == originalMarks );
    CHECK( changed.count() == 0 );
    CHECK( results.getLineString( 1_lnum ) == "MATCH" );
    CHECK( results.getLineString( 2_lnum ) == "bookmark" );
}

TEST_CASE( "Folder marks prepare privately and install once without intermediate notifications",
           "[folder][folder-mark-preparation]" )
{
    FolderSearchResults results;
    FolderSearchResults::MarksStore marks;
    results.setMarksStore( &marks );
    klogg::folder::FileGroup group;
    group.filePath = QStringLiteral( "/retained/a.log" );
    group.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    results.setResults( { group } );
    const FolderSearchResults::MarksStore candidate{ { group.filePath, { 2, 4 } } };
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    const auto captured = results.captureMarksLayout( candidate );
    REQUIRE( captured );
    std::shared_ptr<FolderSearchResults::PreparedMarksLayout> prepared;
    std::thread worker( [&] { prepared = FolderSearchResults::prepareMarksLayout( captured ); } );
    worker.join();
    REQUIRE( prepared );
    CHECK( results.getNbLine() == 2_lcount );
    CHECK( marks.isEmpty() );
    CHECK( changed.count() == 0 );
    auto publication = results.lockMarksLayout( prepared );
    REQUIRE( publication );
    marks = candidate; // The owning transaction installs every owner before notifications.
    CHECK( publication->install() );
    CHECK_FALSE( publication->install() );
    publication.reset(); // Readers can re-enter only after every publication guard is released.
    CHECK( changed.count() == 0 );
    CHECK( results.getNbLine() == 3_lcount );
    CHECK( results.sourceForLine( 1_lnum ).localLine == 2_lnum );
    CHECK( results.sourceForLine( 2_lnum ).localLine == 4_lnum );
    CHECK_FALSE( results.lockMarksLayout( prepared ) );
}

TEST_CASE( "Prepared folder marks reject changed context without overwriting human state",
           "[folder][folder-mark-preparation]" )
{
    FolderSearchResults results;
    FolderSearchResults::MarksStore marks;
    FolderSearchResults::MarksStore otherMarks;
    results.setMarksStore( &marks );
    klogg::folder::FileGroup group;
    group.filePath = QStringLiteral( "/retained/a.log" );
    group.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    results.setResults( { group } );
    const auto captured = results.captureMarksLayout( { { group.filePath, { 2 } } } );
    REQUIRE( captured );
    const auto prepared = FolderSearchResults::prepareMarksLayout( captured );
    REQUIRE( prepared );
    SECTION( "collapse" ) { results.collapseAll(); }
    SECTION( "visibility" ) { results.setVisibility( FolderSearchResults::Visibility::Matches ); }
    SECTION( "new search" ) { results.beginSearch( { group.filePath } ); }
    SECTION( "changed marks without layout refresh" ) { marks[ group.filePath ].insert( 7 ); }
    SECTION( "store rebound" ) { results.setMarksStore( &otherMarks ); }
    SECTION( "another owner" ) {
        FolderSearchResults other;
        other.setResults( { group } );
        other.setMarksStore( &marks );
        CHECK_FALSE( other.lockMarksLayout( prepared ) );
        return;
    }
    const auto count = results.getNbLine();
    QSignalSpy changed( &results, &FolderSearchResults::layoutChanged );
    CHECK_FALSE( results.lockMarksLayout( prepared ) );
    CHECK( results.getNbLine() == count );
    CHECK( changed.count() == 0 );
}

TEST_CASE( "Folder mark preparation admits semantic no-ops and outlives the capture owner",
           "[folder][folder-mark-preparation]" )
{
    auto results = std::make_unique<FolderSearchResults>();
    FolderSearchResults::MarksStore marks;
    results->setMarksStore( &marks );
    const auto captured = results->captureMarksLayout( { { QStringLiteral( "/retained/a.log" ), { 2 } } } );
    REQUIRE( captured );
    SECTION( "no-op refresh is not a conflict" ) {
        results->refreshForMarksChange();
        results->setVisibility( results->visibility() );
        const auto prepared = FolderSearchResults::prepareMarksLayout( captured );
        REQUIRE( prepared );
        REQUIRE( results->lockMarksLayout( prepared ) );
    }
    SECTION( "worker needs no QObject lifetime" ) {
        results.reset();
        std::shared_ptr<FolderSearchResults::PreparedMarksLayout> prepared;
        std::thread worker( [&] { prepared = FolderSearchResults::prepareMarksLayout( captured ); } );
        worker.join();
        CHECK( prepared );
    }
}

TEST_CASE( "Folder mark preparation bounds candidate and projected metadata before admission",
           "[folder][folder-mark-preparation]" )
{
    FolderSearchResults results;
    FolderSearchResults::MarksStore marks;
    results.setMarksStore( &marks );
    FolderSearchResults::MarksStore candidate;
    SECTION( "mark count" ) {
        auto& rows = candidate[ QStringLiteral( "/retained/a.log" ) ];
        for ( size_t row = 0; row < klogg::ReviewMarksOwner::MaxMarks; ++row ) rows.insert( row );
        REQUIRE( results.captureMarksLayout( candidate ) );
        rows.insert( klogg::ReviewMarksOwner::MaxMarks );
    }
    SECTION( "member count" ) {
        for ( size_t index = 0; index < klogg::ReviewMarksOwner::MaxMembers; ++index )
            candidate.insert( QString::number( index ), { 0 } );
        REQUIRE( results.captureMarksLayout( candidate ) );
        candidate.insert( QStringLiteral( "overflow" ), { 0 } );
    }
    SECTION( "member text" ) {
        const QString path( static_cast<int>( klogg::ReviewMarksOwner::MaxTextUnits ), QLatin1Char( 'x' ) );
        candidate.insert( path, { 0 } );
        REQUIRE( results.captureMarksLayout( candidate ) );
        candidate.insert( QStringLiteral( "overflow" ), { 0 } );
    }
    SECTION( "projection upper bound includes headers" ) {
        klogg::folder::FileGroup group;
        group.filePath = QStringLiteral( "/retained/a.log" );
        group.matches.resize( FolderSearchResults::MaxPreparedLayoutRows - 1 );
        results.setResults( { group } );
        REQUIRE( results.captureMarksLayout( candidate ) );
        candidate.insert( group.filePath, { 1 } );
    }
    CHECK_FALSE( results.captureMarksLayout( candidate ) );
    CHECK_FALSE( FolderSearchResults::prepareMarksLayout( {} ) );
    CHECK_FALSE( results.lockMarksLayout( {} ) );
}

TEST_CASE( "Prepared folder marks keep immutable source leases without decoding during preparation",
           "[folder][folder-mark-preparation]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = writeFile( dir, "source.log", { "before", "hit", "after" } );
    RegularExpression expression{ RegularExpressionPattern{ QStringLiteral( "hit" ) } };
    auto matcher = expression.createMatcher();
    auto group = FolderSearchEngine::scanFile( path, *matcher );
    const auto source = group.content;
    REQUIRE( source );
    FolderSearchResults results;
    FolderSearchResults::MarksStore marks;
    results.setMarksStore( &marks );
    results.setResults( { group } );
    FolderSearchResults::MarksStore candidate{ { path, { 0, 2 } } };
    const auto oldSnapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    const auto captured = results.captureMarksLayout( candidate );
    REQUIRE( captured );
    candidate[ path ].clear(); // Detached candidate edits must not alter captured input.
    REQUIRE( QFile::remove( path ) );
    const auto bytesBefore = source->bytesReadForTesting();
    const auto prepared = FolderSearchResults::prepareMarksLayout( captured );
    REQUIRE( prepared );
    auto publication = results.lockMarksLayout( prepared );
    REQUIRE( publication );
    marks = FolderSearchResults::MarksStore{ { path, { 0, 2 } } };
    REQUIRE( publication->install() );
    publication.reset();
    CHECK( source->bytesReadForTesting() == bytesBefore );
    CHECK( oldSnapshot->lineCount() == 1_lcount );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot->lineCount() == 3_lcount );
    CHECK( snapshot->lineString( 0_lnum ) == QStringLiteral( "before" ) );
    CHECK( snapshot->lineString( 2_lnum ) == QStringLiteral( "after" ) );
}

TEST_CASE( "FolderSearchResults empty has 0 lines", "[folder]" )
{
    FolderSearchResults r;
    REQUIRE( r.getNbLine() == 0_lcount );
    REQUIRE( r.getMaxLength() == 0_length );
    REQUIRE( r.groupCount() == 0 );
}

TEST_CASE( "FolderSearchResults single file single match is header + match row", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/tmp/a.log";
    g.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    r.setResults( { g } );

    REQUIRE( r.groupCount() == 1 );
    REQUIRE( r.getNbLine() == 2_lcount ); // 1 header + 1 match
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );

    const auto src = r.sourceForLine( 1_lnum );
    REQUIRE( src.filePath == "/tmp/a.log" );
    REQUIRE( src.localLine == 4_lnum );
}

TEST_CASE( "FolderSearchResults header text shows path and total count", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/var/log/srv.log";
    g.matches.push_back( match( 1, 0, 10, 10, 3 ) );
    g.matches.push_back( match( 2, 0, 10, 10, 3 ) );
    r.setResults( { g } );

    const auto header = r.getLineString( 0_lnum );
    INFO( "header: " << header.toStdString() );
    REQUIRE( header.contains( "srv.log" ) );
    REQUIRE( header.contains( "2" ) ); // total matches
}

TEST_CASE( "FolderSearchResults files with no matches are hidden", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup empty;
    empty.filePath = "/empty.log"; // zero matches
    r.setResults( { a, empty } );

    REQUIRE( r.groupCount() == 1 ); // empty group dropped
    REQUIRE( r.getNbLine() == 2_lcount );
}

TEST_CASE( "FolderSearchResults multiple groups keep file/line order", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 3, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );

    // a: header(0) + 2 matches (1,2); b: header(3) + 1 match (4)
    REQUIRE( r.getNbLine() == 5_lcount );
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 1_lnum ).filePath == "/a.log" );
    REQUIRE( r.sourceForLine( 2_lnum ).filePath == "/a.log" );
    REQUIRE( r.lineKind( 3_lnum ) == LineKind::Header );
    REQUIRE( r.sourceForLine( 4_lnum ).filePath == "/b.log" );
}

TEST_CASE( "FolderSearchResults collapse hides a group's matches", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    b.matches.push_back( match( 2, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );
    REQUIRE( r.getNbLine() == 5_lcount ); // a(2) + b(3)

    r.toggleCollapse( 1 ); // collapse b (fileId == group index)
    REQUIRE( r.isCollapsed( 1 ) );
    REQUIRE( r.getNbLine() == 3_lcount ); // a(2) + b header only(1)

    // b's header is now at visible index 2 and still reports its total count.
    REQUIRE( r.lineKind( 2_lnum ) == LineKind::Header );
    const auto bHeader = r.getLineString( 2_lnum );
    INFO( "b header: " << bHeader.toStdString() );
    REQUIRE( bHeader.contains( "b.log" ) );
    REQUIRE( bHeader.contains( "2" ) ); // total matches still reported

    r.toggleCollapse( 1 ); // expand
    REQUIRE_FALSE( r.isCollapsed( 1 ) );
    REQUIRE( r.getNbLine() == 5_lcount );
}

TEST_CASE( "FolderSearchResults collapsed visible indices remap", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    r.setResults( { a, b } ); // a(header+2)=3, b(header+1)=2 -> total 5

    r.toggleCollapse( 0 ); // collapse a -> a header(1) + b(header+1)=2 -> total 3
    REQUIRE( r.getNbLine() == 3_lcount );
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header ); // a header
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Header ); // b header
    REQUIRE( r.lineKind( 2_lnum ) == LineKind::Data );   // b match
    REQUIRE( r.sourceForLine( 2_lnum ).filePath == "/b.log" );
}

TEST_CASE( "FolderSearchResults collapseAll and expandAll", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );
    REQUIRE( r.getNbLine() == 4_lcount ); // 2 headers + 2 matches

    r.collapseAll();
    REQUIRE( r.getNbLine() == 2_lcount ); // only the 2 headers
    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Header );

    r.expandAll();
    REQUIRE( r.getNbLine() == 4_lcount );
    REQUIRE_FALSE( r.isCollapsed( 0 ) );
    REQUIRE_FALSE( r.isCollapsed( 1 ) );
}

TEST_CASE( "FolderSearchResults reads match line text from the source file", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "alpha\n"  -> line 0, bytes [0,6)
    // "beta ERROR\n" -> line 1, bytes [6,17)
    // "gamma\n"  -> line 2, bytes [17,23)
    const QStringList lines{ "alpha", "beta ERROR", "gamma" };
    const QString path = writeFile( dir, "a.log", lines );

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.matches.push_back( match( 1, 6, 17, 10, 5 ) ); // "beta ERROR", match "ERROR"
    r.setResults( { g } );

    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );
    REQUIRE( r.getLineString( 1_lnum ) == QString( "beta ERROR" ) );
    REQUIRE( r.getLineLength( 1_lnum ) == 10_length );
}

TEST_CASE( "FolderSearchResults readMatchLine decodes with the file source codec", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // Write a UTF-16LE file (with BOM) holding:
    //   BOM(2) "foo\n"(8) -> line 0 = [0,10)
    //   "ERROR here\n"(22) -> line 1 = [10,32)
    QByteArray content;
    content.append( '\xff' );
    content.append( '\xfe' ); // BOM
    const std::string text = "foo\nERROR here\nbaz\n";
    for ( char c : text ) {
        content.append( c );
        content.append( '\0' ); // UTF-16LE low/high
    }
    const QString path = QDir( dir.path() ).absoluteFilePath( "utf16le.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( content );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
    g.matches.push_back( match( 1, 10, 32, 10, 5 ) ); // "ERROR here"
    r.setResults( { g } );

    REQUIRE( r.lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( r.lineKind( 1_lnum ) == LineKind::Data );
    // Decoded with the SOURCE codec, not the default UTF-8 display encoding:
    // the raw bytes [10,32) are "ERROR here\n" in UTF-16LE.
    const QString line = r.getLineString( 1_lnum );
    INFO( "decoded line: " << line.toStdString() );
    REQUIRE( line == QStringLiteral( "ERROR here" ) );
    // No BOM leaks into non-line-0 lines.
    REQUIRE_FALSE( line.startsWith( QChar( 0xFEFF ) ) );
}

TEST_CASE( "FolderSearchResults readMatchLine falls back to UTF-8 with no source codec", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QStringList lines{ "alpha", "beta ERROR", "gamma" };
    const QString path = writeFile( dir, "a.log", lines );

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    // sourceCodec left null -> readMatchLine must decode as UTF-8.
    g.matches.push_back( match( 1, 6, 17, 10, 5 ) ); // "beta ERROR"
    r.setResults( { g } );

    REQUIRE( r.getLineString( 1_lnum ) == QString( "beta ERROR" ) );
}

TEST_CASE( "FolderSearchResults a marked UTF-16 line under the cache cap decodes correctly",
           "[folder][marks]" )
{
    // The stateful-codec path (UTF-16) uses the whole-file decoded-line cache
    // (a byte scan cannot find line boundaries in UTF-16). UNDER the cap the
    // marked line must decode correctly and report Available.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = QDir( dir.path() ).absoluteFilePath( "utf16.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        // BOM + "alpha\nbeta\ngamma\n" in UTF-16LE.
        QByteArray bytes;
        bytes.append( "\xFF\xFE", 2 );
        for ( const QString& l : { QStringLiteral( "alpha" ), QStringLiteral( "beta" ),
                                   QStringLiteral( "gamma" ) } ) {
            for ( const QChar c : l ) {
                bytes.append( static_cast<char>( c.unicode() & 0xFF ) );
                bytes.append( static_cast<char>( ( c.unicode() >> 8 ) & 0xFF ) );
            }
            bytes.append( '\n' );
            bytes.append( '\0' );
        }
        f.write( bytes );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
    // One real match so the group exists; the mark is on a different line.
    g.matches.push_back( match( 0, 2, 12, 5, 5 ) ); // "alpha" (BOM at 0-1)
    r.setResults( { g } );

    // Mark line 2 ("gamma") -- not a match -> injected as a mark row.
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 2 );
    r.setMarksStore( &marks );

    // rows: [H0, D1(match line0), D2(mark row line2)].
    REQUIRE( r.getNbLine() == 3_lcount );
    const auto src = r.sourceForLine( 2_lnum );
    REQUIRE( src.localLine == 2_lnum );
    REQUIRE( r.markLineTextStatus( 2_lnum )
             == FolderSearchResults::MarkLineTextStatus::Available );
    REQUIRE( r.getLineString( 2_lnum ) == QStringLiteral( "gamma" ) );
}

TEST_CASE( "FolderSearchResults setResults keeps marks-only paths when membership is unspecified",
           "[folder][marks]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "marked-only.log", { "marked text" } );

    FolderSearchResults results;
    results.setResults( {} );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    results.setMarksStore( &marks );

    REQUIRE( results.getNbLine() == 2_lcount );
    REQUIRE( results.sourceForLine( 1_lnum ).filePath == path );
    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
}

TEST_CASE( "FolderSearchResults beginSearch invalidates the seek-based marked-line text cache",
           "[folder][marks][mark-cache-refresh]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "seek.log", { "old text", "ERROR" } );

    auto makeGroup = [ & ] {
        klogg::folder::FileGroup group;
        group.filePath = path;
        group.matches.push_back( match( 1, 9, 15, 5, 5 ) );
        return group;
    };

    FolderSearchResults results;
    results.setResults( { makeGroup() } );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    results.setMarksStore( &marks );

    // rows: [header, marked non-match line 0, match line 1]. Reading the mark
    // populates markTextCache_, the byte-newline-safe seek-path cache.
    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "old text" ) );

    REQUIRE( writeFile( dir, "seek.log", { "new text", "ERROR" } ) == path );
    results.beginSearch( QStringList{ path } );
    results.addFileGroup( 0, makeGroup() );

    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "new text" ) );
}

TEST_CASE( "FolderSearchResults beginSearch invalidates the UTF-16 marked-line decode cache",
           "[folder][marks][mark-cache-refresh]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = QDir( dir.path() ).absoluteFilePath( "utf16-refresh.log" );

    auto writeUtf16 = [ & ]( const QString& firstLine ) {
        QFile file( path );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray bytes;
        bytes.append( "\xFF\xFE", 2 );
        for ( const QString& line : { firstLine, QStringLiteral( "ERROR" ) } ) {
            for ( const QChar c : line ) {
                bytes.append( static_cast<char>( c.unicode() & 0xFF ) );
                bytes.append( static_cast<char>( ( c.unicode() >> 8 ) & 0xFF ) );
            }
            bytes.append( '\n' );
            bytes.append( '\0' );
        }
        REQUIRE( file.write( bytes ) == bytes.size() );
    };
    auto makeGroup = [ & ] {
        klogg::folder::FileGroup group;
        group.filePath = path;
        group.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
        group.matches.push_back( match( 1, 14, 26, 5, 5 ) );
        return group;
    };

    writeUtf16( QStringLiteral( "alpha" ) );
    FolderSearchResults results;
    results.setResults( { makeGroup() } );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    results.setMarksStore( &marks );

    // The stateful codec forces the whole-file markLineCache_ path.
    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "alpha" ) );

    writeUtf16( QStringLiteral( "omega" ) );
    results.beginSearch( QStringList{ path } );
    results.addFileGroup( 0, makeGroup() );

    REQUIRE( results.sourceForLine( 1_lnum ).localLine == 0_lnum );
    REQUIRE( results.getLineString( 1_lnum ) == QStringLiteral( "omega" ) );
}

TEST_CASE( "FolderSearchResults a marked line over the cache cap with a stateful codec "
           "reports Unavailable instead of silently rendering empty",
           "[folder][marks][regression]" )
{
    // Regression guard for the 16 MiB whole-file mark-line cache cap: a marked
    // line in a file OVER the cap with a STATEFUL codec (UTF-16 -- the seek
    // path cannot apply) has no text. The model must expose that as an explicit
    // Unavailable state so the view can distinguish "text unavailable" from "the
    // line is empty" -- silently rendering blank was the original defect. The
    // fixture size derives from the SAME kMarkLineCacheCap constant the
    // production cap uses, so a cap change keeps this test on the over-cap side.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = QDir( dir.path() ).absoluteFilePath( "big-utf16.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        QByteArray bytes;
        bytes.append( "\xFF\xFE", 2 ); // BOM
        // Build UTF-16LE filler without QTextCodec to keep the fixture simple.
        QByteArray line;
        for ( const QChar c : QStringLiteral( "filler line to push past the cap\n" ) ) {
            line.append( static_cast<char>( c.unicode() & 0xFF ) );
            line.append( static_cast<char>( ( c.unicode() >> 8 ) & 0xFF ) );
        }
        while ( bytes.size() <= FolderSearchResults::kMarkLineCacheCap ) {
            bytes.append( line );
        }
        f.write( bytes );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    g.sourceCodec = QTextCodec::codecForName( "UTF-16LE" );
    g.matches.push_back( match( 0, 2, 12, 5, 5 ) );
    r.setResults( { g } );

    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 3 ); // a non-match filler line
    r.setMarksStore( &marks );

    REQUIRE( r.getNbLine() == 3_lcount ); // header + match + mark row
    const auto src = r.sourceForLine( 2_lnum );
    REQUIRE( src.localLine == 3_lnum );
    // RED before the contract: over-cap mark text silently rendered as empty
    // with no way to tell. Now it must report Unavailable.
    REQUIRE( r.markLineTextStatus( 2_lnum )
             == FolderSearchResults::MarkLineTextStatus::Unavailable );
    // And the results view must render an explicit placeholder for the
    // unavailable text, not a silent blank line (the user-facing symptom).
    REQUIRE( r.getLineString( 2_lnum )
             == FolderSearchResults::unavailableMarkLineText() );
    REQUIRE( r.getExpandedLineString( 2_lnum )
             == FolderSearchResults::unavailableMarkLineText() );
}

TEST_CASE( "FolderSearchResults matchLinesForFile returns ascending local lines", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 3, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 7, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 42, 0, 5, 5, 1 ) );
    klogg::folder::FileGroup b;
    b.filePath = "/b.log";
    b.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    r.setResults( { a, b } );

    const auto aLines = r.matchLinesForFile( "/a.log" );
    REQUIRE( aLines.size() == 3 );
    REQUIRE( aLines[ 0 ] == 3_lnum );
    REQUIRE( aLines[ 1 ] == 7_lnum );
    REQUIRE( aLines[ 2 ] == 42_lnum );

    const auto bLines = r.matchLinesForFile( "/b.log" );
    REQUIRE( bLines.size() == 1 );
    REQUIRE( bLines[ 0 ] == 1_lnum );
}

TEST_CASE( "FolderSearchResults matchLinesForFile is empty for an absent file", "[folder]" )
{
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    r.setResults( { a } );

    REQUIRE( r.matchLinesForFile( "/not/here.log" ).empty() );
    REQUIRE( r.matchLinesForFile( QString() ).empty() );
}

TEST_CASE( "FolderSearchResults matchLinesForFile ignores collapse state", "[folder]" )
{
    // matchLinesForFile reads groups_, not visibleRows_, so collapsing a group
    // must NOT change the per-file match list (the folder overview needs the full
    // set even when the results view is collapsed).
    FolderSearchResults r;
    klogg::folder::FileGroup a;
    a.filePath = "/a.log";
    a.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 1, 0, 5, 5, 1 ) );
    a.matches.push_back( match( 2, 0, 5, 5, 1 ) );
    r.setResults( { a } );
    REQUIRE( r.matchLinesForFile( "/a.log" ).size() == 3 );

    r.collapseAll();
    REQUIRE( r.getNbLine() == 1_lcount ); // only the header is visible now
    REQUIRE( r.matchLinesForFile( "/a.log" ).size() == 3 ); // ...but still 3 matches

    r.toggleCollapse( 0 );
    REQUIRE( r.matchLinesForFile( "/a.log" ).size() == 3 );
}

TEST_CASE( "FolderSearchResults getLineNumber maps rows to source lines", "[folder]" )
{
    // Single-file parity: copying with line numbers (or any getLineNumber
    // consumer) must see the SOURCE line numbers, not the result-row indices
    // (cluster E of the 2026-07-18 audit).
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/tmp/a.log";
    g.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    g.matches.push_back( match( 9, 0, 10, 10, 5 ) );
    r.setResults( { g } );

    // Row 0 is the group header; rows 1/2 map to source lines 4/9 (0-based).
    // getLineNumber is 1-based, so the source line numbers are 5 and 10.
    REQUIRE( r.getLineNumber( 1_lnum ) == 5_lnum );
    REQUIRE( r.getLineNumber( 2_lnum ) == 10_lnum );
}

TEST_CASE( "FolderSearchResults header rows are not copyable", "[folder]" )
{
    // Group headers are UI chrome, not source lines: the copy / search-
    // composition path skips them via the AbstractLogData copyable hook.
    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = "/tmp/a.log";
    g.matches.push_back( match( 4, 0, 10, 10, 5 ) );
    r.setResults( { g } );

    REQUIRE_FALSE( r.isLineCopyable( 0_lnum ) );
    REQUIRE( r.isLineCopyable( 1_lnum ) );
}

TEST_CASE( "FolderSearchResults decodes with the per-file encoding override", "[folder]" )
{
    // Parity with single-file setEncoding: overriding the display encoding of
    // a file must also apply to its RESULT rows (they otherwise decode with
    // the codec detected at scan time, and a misdetection stays mojibake).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "caf\xE9" (e-acute in Latin-1) -- invalid as UTF-8.
    const QString path = QDir( dir.path() ).absoluteFilePath( "latin1.log" );
    {
        QFile f( path );
        REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        f.write( QByteArray( "caf\xE9\n" ) );
        f.close();
    }

    FolderSearchResults r;
    klogg::folder::FileGroup g;
    g.filePath = path;
    // Simulate a scan-time misdetection as UTF-8.
    g.sourceCodec = QTextCodec::codecForName( "UTF-8" );
    g.matches.push_back( match( 0, 0, 4, 4, 3 ) );
    r.setResults( { g } );

    // Without an override the row decodes with the (wrong) detected codec.
    REQUIRE( r.getLineString( 1_lnum ) != QStringLiteral( "café" ) );

    // The override takes precedence over the detected codec, and notifies the
    // view (documented in the header: the view re-renders on layoutChanged).
    QSignalSpy layoutSpy( &r, &FolderSearchResults::layoutChanged );
    r.setEncodingOverrideForFile( path, "ISO 8859-1" );
    REQUIRE( r.getLineString( 1_lnum ) == QStringLiteral( "café" ) );
    REQUIRE( layoutSpy.count() == 1 );

    // Clearing the override returns to the detected codec (and notifies once,
    // only because an entry was actually removed).
    r.clearEncodingOverrideForFile( path );
    REQUIRE( r.getLineString( 1_lnum ) != QStringLiteral( "café" ) );
    REQUIRE( layoutSpy.count() == 2 );

    // No-op clear (nothing to remove) does not notify.
    r.clearEncodingOverrideForFile( path );
    REQUIRE( layoutSpy.count() == 2 );
}

TEST_CASE( "FolderSearchResults byte-newline-safe codec gate is an allowlist", "[folder][marks]" )
{
    // codecIsByteNewlineSafe gates the seek-based mark-line read. It must be an
    // ALLOWLIST: only UTF-8 / known stateless single-byte codecs qualify, and
    // any stateful or unlisted codec returns false so it stays on the whole-file
    // decode path. A denylist would let an unlisted codec with a non-ASCII (or
    // multi-byte) newline encoding silently mis-seek.
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe( nullptr ) ); // UTF-8 default
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-8" ) ) );
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe(
        QTextCodec::codecForName( "ISO 8859-1" ) ) );
    REQUIRE( FolderSearchResults::codecIsByteNewlineSafe(
        QTextCodec::codecForName( "windows-1251" ) ) );

    // Stateful / multi-byte / unlisted codecs must NOT be treated as
    // byte-newline-safe.
    REQUIRE_FALSE(
        FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-16LE" ) ) );
    REQUIRE_FALSE(
        FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-16" ) ) );
    REQUIRE_FALSE(
        FolderSearchResults::codecIsByteNewlineSafe( QTextCodec::codecForName( "UTF-32" ) ) );
    if ( auto* sjis = QTextCodec::codecForName( "Shift-JIS" ) ) {
        REQUIRE_FALSE( FolderSearchResults::codecIsByteNewlineSafe( sjis ) );
    }
    if ( auto* gbk = QTextCodec::codecForName( "GBK" ) ) {
        REQUIRE_FALSE( FolderSearchResults::codecIsByteNewlineSafe( gbk ) );
    }
}

TEST_CASE( "FolderSearchResults getters are race-free against streaming and concurrent readers",
           "[folder]" )
{
    // QuickFind runs on a QtConcurrent worker reading the results model while
    // the main thread streams groups in, and the view's paints hit the same
    // getters (quickfind.cpp:211+). Before the model was internally
    // synchronized, concurrent reads tore the row/group containers and shared
    // per-group QFile cursors (garbage text, OOB access, crash).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    constexpr int fileCount = 8;
    constexpr int groupsPerFile = 8; // 64 groups -> 3264 visible rows
    constexpr int matchesPerGroup = 50;

    QStringList paths;
    std::vector<std::vector<int64_t>> lineOffsets;
    for ( int f = 0; f < fileCount; ++f ) {
        QStringList lines;
        std::vector<int64_t> offsets;
        int64_t running = 0;
        for ( int l = 0; l < 200; ++l ) {
            const auto line = QString( "file%1 line %2" ).arg( f ).arg( l );
            offsets.push_back( running );
            running += line.size() + 1;
            lines << line;
        }
        paths << writeFile( dir, QString( "f%1.log" ).arg( f ), lines );
        lineOffsets.push_back( std::move( offsets ) );
    }

    FolderSearchResults results;
    QStringList order;
    for ( int g = 0; g < fileCount * groupsPerFile; ++g ) {
        order << paths[ g % static_cast<int>( paths.size() ) ];
    }
    results.beginSearch( order );

    std::atomic<bool> stop{ false };
    std::atomic<int> failures{ 0 };

    auto reader = [ & ] {
        while ( !stop.load( std::memory_order_relaxed ) ) {
            try {
                const auto nb = results.getNbLine().get();
                for ( LinesCount::UnderlyingType i = 0; i < nb; ++i ) {
                    const auto row = LineNumber( i );
                    if ( results.lineKind( row ) != LineKind::Data ) {
                        continue;
                    }
                    const auto src = results.sourceForLine( row );
                    const auto text = results.getExpandedLineString( row );
                    if ( src.filePath.isEmpty() || text.isEmpty() ) {
                        continue;
                    }
                    // The row's text must come from ITS file; a torn read on a
                    // shared QFile handle returns another file's content.
                    const auto expectedPrefix = QLatin1String( "file" )
                                                + src.filePath.at( src.filePath.size() - 5 );
                    if ( !text.startsWith( expectedPrefix ) ) {
                        ++failures;
                    }
                }
            }
            catch ( ... ) {
                ++failures;
            }
        }
    };

    std::thread readerA( reader );
    std::thread readerB( reader );

    for ( int g = 0; g < fileCount * groupsPerFile; ++g ) {
        const int f = g % fileCount;
        klogg::folder::FileGroup group;
        group.filePath = paths[ f ];
        for ( int m = 0; m < matchesPerGroup; ++m ) {
            const auto start = lineOffsets[ static_cast<size_t>( f ) ][ static_cast<size_t>( m ) ];
            const auto lineText = QString( "file%1 line %2" ).arg( f ).arg( m );
            const auto textLen = static_cast<int64_t>( lineText.size() );
            group.matches.push_back( match( static_cast<uint64_t>( m ), start, start + textLen,
                                            static_cast<int>( textLen ), 4 ) );
        }
        results.addFileGroup( g, std::move( group ) );
    }

    stop.store( true );
    readerA.join();
    readerB.join();

    REQUIRE( failures.load() == 0 );
    REQUIRE( results.getNbLine().get()
             == static_cast<uint64_t>( fileCount * groupsPerFile * ( 1 + matchesPerGroup ) ) );
}

TEST_CASE( "Folder bounded snapshots reject large marked or matched rows before reading payload",
           "[folder][snapshot-bounded]" )
{
    const bool marked = GENERATE( false, true );
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto text = QStringLiteral( "ERROR " ) + QString( 1024, QLatin1Char( 'x' ) );
    const auto path = writeFile( dir, "bounded.log", { "prefix", text } );
    RegularExpression expression{ RegularExpressionPattern{ marked ? "absent" : "ERROR" } };
    auto matcher = expression.createMatcher();
    auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.content );
    const auto content = group.content;
    QHash<QString, std::set<uint64_t>> marks;
    if ( marked ) marks[ path ].insert( 1 );
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.setResults( { std::move( group ) } );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot );
    REQUIRE( snapshot->lineCount() == 1_lcount );
    const auto before = content->bytesReadForTesting();
    CHECK_THROWS_AS( snapshot->readLineBounded( 0_lnum, 1024, 2048 ), klogg::SnapshotLimitError );
    // Mark-only rows may inspect two fixed-size offset entries, never the body.
    CHECK( content->bytesReadForTesting() - before <= 2 * sizeof( qint64 ) );
    const auto bytes = static_cast<uint64_t>( text.toUtf8().size() ) + 1;
    CHECK( snapshot->sourceBytesForLine( 0_lnum ) == std::optional<uint64_t>( bytes ) );
    REQUIRE( QFile::remove( path ) );
    CHECK( snapshot->readLineBounded( 0_lnum, bytes, 2048 ).text == text );
}

TEST_CASE( "Folder snapshots retain canonical matches through source and layout replacement",
           "[folder][folder-snapshot]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = writeFile( dir, "snapshot.log", { "before", "ERROR old", "after" } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    auto group = FolderSearchEngine::scanFile( path, *matcher, 5, {}, { 1, 1 } );
    FolderSearchResults results;
    results.setResults( { std::move( group ) } );
    const auto presented = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( presented );
    REQUIRE( presented->lineCount() == 3_lcount );
    REQUIRE_FALSE( presented->row( 0_lnum ).match );
    REQUIRE( presented->row( 1_lnum ).sourceLineNumber == 2_lnum );
    const auto identity = presented->row( 1_lnum ).identity;
    REQUIRE( results.rowForIdentity( identity ) == OptionalLineNumber{ 2_lnum } );
    results.collapseAll();
    REQUIRE_FALSE( results.rowForIdentity( identity ) );
    results.setVisibility( FolderSearchResults::Visibility::Marks );
    const auto canonical = results.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( canonical );
    REQUIRE( canonical->lineCount() == 1_lcount );
    REQUIRE( canonical->row( 0_lnum ).identity == identity );
    REQUIRE( canonical->row( 0_lnum ).match );
    writeFile( dir, "snapshot.log", { "before", "ERROR new", "after" } );
    results.setResults( {} );
    REQUIRE_FALSE( results.rowForIdentity( identity ) );
    REQUIRE( QFile::remove( path ) );
    REQUIRE( canonical->lineString( 0_lnum ) == "ERROR old" );
    REQUIRE( presented->lineString( 0_lnum ) == "before" );
    REQUIRE( presented->lineString( 2_lnum ) == "after" );
}

TEST_CASE( "Folder snapshots retain zero-match scanned members for marks",
           "[folder][folder-snapshot][folder-batch-retention]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = writeFile( dir, "marks.log", { "first", "marked original" } );
    FolderSearchResults results;
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 1 );
    results.setMarksStore( &marks );
    SECTION( "streaming results retain the marked source" )
    {
        RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
        auto matcher = expression.createMatcher();
        results.beginSearch( { path } );
        results.addFileGroup( 0, FolderSearchEngine::scanFile( path, *matcher ) );
    }
    SECTION( "synchronous batch results retain the marked source" )
    {
        FolderSearchEngine engine;
        engine.scanSynchronously( { path }, RegularExpressionPattern{ "ERROR" } );
        results.setResults( engine.takeResults() );
        REQUIRE( engine.matchCount() == 0 );
    }
    REQUIRE( results.groupCount() == 0 );
    const auto matches = results.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    REQUIRE( matches->lineCount() == 0_lcount );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot );
    REQUIRE( snapshot->lineCount() == 1_lcount );
    REQUIRE_FALSE( snapshot->row( 0_lnum ).match );
    REQUIRE( snapshot->row( 0_lnum ).sourceLineNumber == 2_lnum );
    REQUIRE( QFile::remove( path ) );
    REQUIRE( snapshot->lineString( 0_lnum ) == "marked original" );
    REQUIRE( results.getLineString( 1_lnum ) == "marked original" );
}

TEST_CASE( "Folder snapshot refuses legacy records without retained source bytes",
           "[folder][folder-snapshot]" )
{
    FolderSearchResults results;
    klogg::folder::FileGroup group;
    group.filePath = "/missing/unversioned.log";
    group.matches.push_back( match( 0, 0, 5, 5, 1 ) );
    results.setResults( { group } );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( snapshot );
    REQUIRE_THROWS_AS( snapshot->lineString( 0_lnum ), klogg::SnapshotError );
}

TEST_CASE( "Folder snapshot freezes codec interpretation while current rows follow overrides",
           "[folder][folder-snapshot]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "codec.log", { QString::fromUtf8( "ERROR \xc3\xa9" ) } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    FolderSearchResults results;
    results.setResults( { FolderSearchEngine::scanFile( path, *matcher ) } );
    results.setEncodingOverrideForFile( path, "UTF-8" );
    const auto frozen = results.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( frozen );
    results.setEncodingOverrideForFile( path, "ISO-8859-1" );
    REQUIRE( frozen->lineString( 0_lnum ) == QString::fromUtf8( "ERROR \xc3\xa9" ) );
    REQUIRE( results.getLineString( 1_lnum ) != frozen->lineString( 0_lnum ) );
}

TEST_CASE( "Folder snapshot identity survives streaming FileId shifts without aliasing",
           "[folder][folder-snapshot]" )
{
    QTemporaryDir dir;
    const auto first = writeFile( dir, "a.log", { "ERROR first" } );
    const auto last = writeFile( dir, "z.log", { "marked last" } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    QHash<QString, std::set<uint64_t>> marks;
    marks[ last ].insert( 0 );
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.beginSearch( { last, first } );
    results.addFileGroup( 0, FolderSearchEngine::scanFile( last, *matcher ) );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot );
    const auto identity = snapshot->row( 0_lnum ).identity;
    REQUIRE( results.rowForIdentity( identity ) == OptionalLineNumber{ 1_lnum } );
    results.addFileGroup( 1, FolderSearchEngine::scanFile( first, *matcher ) );
    REQUIRE( results.rowForIdentity( identity ) == OptionalLineNumber{ 3_lnum } );
    results.setVisibility( FolderSearchResults::Visibility::Matches );
    REQUIRE_FALSE( results.rowForIdentity( identity ) );
    REQUIRE( snapshot->lineString( 0_lnum ) == "marked last" );
}

TEST_CASE( "Folder stale marks fail snapshot reads without throwing through presentation",
           "[folder][folder-snapshot][folder-stale-mark]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "short.log", { "only line" } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 40 );
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.beginSearch( { path } );
    results.addFileGroup( 0, FolderSearchEngine::scanFile( path, *matcher ) );
    REQUIRE_NOTHROW( results.getLineString( 1_lnum ) );
    REQUIRE_NOTHROW( results.getLineLength( 1_lnum ) );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot );
    REQUIRE_THROWS_AS( snapshot->lineString( 0_lnum ), klogg::SnapshotError );
}

TEST_CASE( "Folder retention exhaustion rejects snapshots instead of omitting failed members",
           "[folder][folder-snapshot][folder-retention-failure]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "source.log", { "no match" } );
    IndexedContentBuilder::Options options;
    const bool boundaryFailure = GENERATE( false, true );
    options.byteMemoryBudget = boundaryFailure ? 1024U : 0U;
    options.boundaryMemoryBudget = boundaryFailure ? 0U : 1024U;
    options.temporaryDirectory = dir.path() + "/missing-directory";
    auto arena = std::make_shared<klogg::folder::ScanArena>( options );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    FolderSearchResults results;
    results.beginSearch( { path } );
    results.addFileGroup( 0, FolderSearchEngine::scanFile( path, *matcher, 7, {}, {}, arena ) );
    REQUIRE_THROWS_AS( results.createReadSnapshot( klogg::SnapshotScope::Matches ),
                       klogg::SnapshotError );
}

TEST_CASE( "Folder snapshot inverse separates canonical ordinals from frozen view geometry",
           "[folder][folder-snapshot][folder-inverse]" )
{
    QTemporaryDir dir;
    const auto first = writeFile( dir, "first.log", { "skip", "ERROR first", "skip", "ERROR last" } );
    const auto second = writeFile( dir, "second.log", { "ERROR other" } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    FolderSearchResults results;
    results.setResults( { FolderSearchEngine::scanFile( first, *matcher ),
                          FolderSearchEngine::scanFile( second, *matcher ) } );
    const auto canonical = results.createReadSnapshot( klogg::SnapshotScope::Matches );
    const auto presented = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    const auto firstId = canonical->row( 0_lnum ).identity;
    const auto lastId = canonical->row( 1_lnum ).identity;
    const auto otherId = canonical->row( 2_lnum ).identity;
    REQUIRE( canonical->rowForIdentity( firstId ) == OptionalLineNumber{ 0_lnum } );
    REQUIRE( canonical->rowForIdentity( lastId ) == OptionalLineNumber{ 1_lnum } );
    REQUIRE( presented->rowForIdentity( otherId ) == OptionalLineNumber{ 2_lnum } );
    REQUIRE( presented->presentedRowForIdentity( otherId ) == OptionalLineNumber{ 4_lnum } );
    auto nonMatch = firstId;
    nonMatch.row = 2;
    REQUIRE_FALSE( canonical->rowForIdentity( nonMatch ) );
    REQUIRE_FALSE( results.rowForIdentity( nonMatch ) );
    results.setCollapsed( 0, true );
    const auto collapsed = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE_FALSE( collapsed->rowForIdentity( firstId ) );
    REQUIRE_FALSE( collapsed->presentedRowForIdentity( firstId ) );
    REQUIRE( collapsed->presentedRowForIdentity( otherId ) == OptionalLineNumber{ 2_lnum } );
    results.setResults( {} );
    REQUIRE( presented->presentedRowForIdentity( otherId ) == OptionalLineNumber{ 4_lnum } );
    REQUIRE( canonical->rowForIdentity( lastId ) == OptionalLineNumber{ 1_lnum } );
}

TEST_CASE( "Folder retained mark text follows codec and source revisions without changing snapshots",
           "[folder][folder-snapshot]" )
{
    QTemporaryDir dir;
    const auto original = QString::fromUtf8( "marked \xc3\xa9" );
    const auto replacement = QString::fromUtf8( "marked \xc3\xb6" );
    const auto path = writeFile( dir, "cached.log", { original } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 0 );
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.setResults( { FolderSearchEngine::scanFile( path, *matcher ) } );
    results.setEncodingOverrideForFile( path, "UTF-8" );
    const auto frozen = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( results.getLineString( 1_lnum ) == original );
    REQUIRE( results.getLineLength( 1_lnum ) == LineLength( original.size() ) );
    results.setEncodingOverrideForFile( path, "ISO-8859-1" );
    REQUIRE( results.getLineString( 1_lnum ) != original );
    results.setEncodingOverrideForFile( path, "UTF-8" );
    REQUIRE( results.getLineString( 1_lnum ) == original );
    writeFile( dir, "cached.log", { replacement } );
    results.setResults( { FolderSearchEngine::scanFile( path, *matcher ) } );
    REQUIRE( results.getLineString( 1_lnum ) == replacement );
    REQUIRE( frozen->lineString( 0_lnum ) == original );
}

TEST_CASE( "Folder quiet zero-match commits retain bytes for later bookmarks",
           "[folder][folder-snapshot]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "later-mark.log", { "retained for later mark" } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    QHash<QString, std::set<uint64_t>> marks;
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.beginSearch( { path } );
    results.addFileGroup( 0, FolderSearchEngine::scanFile( path, *matcher ) );
    REQUIRE( results.getNbLine() == 0_lcount );
    const auto beforeMarks = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( beforeMarks->lineCount() == 0_lcount );
    REQUIRE( QFile::remove( path ) );
    marks[ path ].insert( 0 );
    results.refreshForMarksChange();
    REQUIRE( results.getLineString( 1_lnum ) == "retained for later mark" );
    const auto marked = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( marked->lineString( 0_lnum ) == "retained for later mark" );
    REQUIRE( beforeMarks->lineCount() == 0_lcount );
}

TEST_CASE( "Folder source identity existence is independent of presentation membership",
           "[folder][folder-snapshot][folder-identity-existence]" )
{
    QTemporaryDir dir;
    const auto matchedPath = writeFile( dir, "matched.log", { "plain", "ERROR match" } );
    const auto marksPath = writeFile( dir, "marks-only.log", { "marked" } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    auto matched = FolderSearchEngine::scanFile( matchedPath, *matcher );
    auto marksOnly = FolderSearchEngine::scanFile( marksPath, *matcher );
    REQUIRE( matched.content );
    REQUIRE( marksOnly.content );
    const auto matchId = matched.content->identity( 1_lnum );
    const auto nonmatchId = matched.content->identity( 0_lnum );
    const auto marksOnlyId = marksOnly.content->identity( 0_lnum );
    QHash<QString, std::set<uint64_t>> marks;
    marks[ matchedPath ].insert( 0 );
    marks[ marksPath ].insert( 0 );
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.beginSearch( { matchedPath, marksPath } );
    results.addFileGroup( 0, std::move( matched ) );
    results.addFileGroup( 1, std::move( marksOnly ) );
    REQUIRE( results.containsRowIdentity( matchId ) );
    results.setVisibility( FolderSearchResults::Visibility::Matches );
    REQUIRE_FALSE( results.rowForIdentity( nonmatchId ) );
    REQUIRE_FALSE( results.rowForIdentity( marksOnlyId ) );
    REQUIRE( results.containsRowIdentity( nonmatchId ) );
    REQUIRE( results.containsRowIdentity( marksOnlyId ) );
    results.collapseAll();
    REQUIRE_FALSE( results.rowForIdentity( matchId ) );
    REQUIRE( results.containsRowIdentity( matchId ) );
    marks.clear();
    results.refreshForMarksChange();
    REQUIRE( results.containsRowIdentity( marksOnlyId ) );
    auto nonexistent = marksOnlyId;
    nonexistent.row = 1;
    REQUIRE_FALSE( results.containsRowIdentity( nonexistent ) );
    results.setResults( { FolderSearchEngine::scanFile( matchedPath, *matcher ) } );
    REQUIRE_FALSE( results.containsRowIdentity( matchId ) );
    REQUIRE_FALSE( results.containsRowIdentity( marksOnlyId ) );
}

TEST_CASE( "Folder marked and matched snapshot rows share incomplete UTF8 replacement",
           "[folder][folder-normalization]" )
{
    QTemporaryDir dir;
    const QString path = QDir( dir.path() ).filePath( "incomplete.log" );
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( "ascii prefix\nBAD \xc3" ) == 18 );
    file.close();
    RegularExpression matchedExpression{ RegularExpressionPattern{ "BAD" } };
    RegularExpression absentExpression{ RegularExpressionPattern{ "absent" } };
    auto matchedMatcher = matchedExpression.createMatcher();
    auto absentMatcher = absentExpression.createMatcher();
    // Context forces streaming; the first ASCII-only block establishes UTF8.
    auto matchedGroup = FolderSearchEngine::scanFile( path, *matchedMatcher, 13, {}, { 1, 0 } );
    auto marksGroup = FolderSearchEngine::scanFile( path, *absentMatcher, 13, {}, { 1, 0 } );
    FolderSearchResults matchedResults;
    matchedResults.setResults( { std::move( matchedGroup ) } );
    FolderSearchResults markedResults;
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 1 );
    markedResults.setMarksStore( &marks );
    markedResults.setResults( { std::move( marksGroup ) } );
    const auto matched = matchedResults.createReadSnapshot( klogg::SnapshotScope::Matches );
    const auto marked = markedResults.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( matched->lineCount() == 1_lcount );
    REQUIRE( marked->lineCount() == 1_lcount );
    const auto expected = QStringLiteral( "BAD " ) + QChar( QChar::ReplacementCharacter );
    REQUIRE( matched->lineString( 0_lnum ) == expected );
    REQUIRE( marked->lineString( 0_lnum ) == expected );
}

TEST_CASE( "Folder marked and matched snapshots share row BOM and framing CR policy",
           "[folder][folder-normalization]" )
{
    QTemporaryDir dir;
    const QString path = QDir( dir.path() ).filePath( "framing.log" );
    const QByteArray bytes = QByteArray( "prefix\n\xef\xbb\xbf" ) + "ERROR\r\r\n";
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( bytes ) == bytes.size() );
    file.close();
    RegularExpression matchedExpression{ RegularExpressionPattern{ "ERROR" } };
    RegularExpression absentExpression{ RegularExpressionPattern{ "absent" } };
    auto matchedMatcher = matchedExpression.createMatcher();
    auto absentMatcher = absentExpression.createMatcher();
    FolderSearchResults matchedResults;
    matchedResults.setResults( {
        FolderSearchEngine::scanFile( path, *matchedMatcher, 7, {}, { 1, 0 } ) } );
    FolderSearchResults markedResults;
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 1 );
    markedResults.setMarksStore( &marks );
    markedResults.setResults( {
        FolderSearchEngine::scanFile( path, *absentMatcher, 7, {}, { 1, 0 } ) } );
    const auto matched = matchedResults.createReadSnapshot( klogg::SnapshotScope::Matches );
    const auto marked = markedResults.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( matched->lineString( 0_lnum ) == "ERROR\r" );
    REQUIRE( marked->lineString( 0_lnum ) == "ERROR\r" );
}

TEST_CASE( "Folder encoding overrides notify interpretation changes after releasing model locks",
           "[folder][folder-interpretation]" )
{
    QTemporaryDir dir;
    const auto original = QString::fromUtf8( "ERROR \xc3\xa9" );
    const auto path = writeFile( dir, "encoding.log", { original } );
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    FolderSearchResults results;
    results.setResults( { FolderSearchEngine::scanFile( path, *matcher ) } );
    QSignalSpy changed( &results, &AbstractLogData::interpretationChanged );
    std::vector<QString> observed;
    QObject::connect( &results, &AbstractLogData::interpretationChanged, &results,
                      [&]() { observed.push_back( results.getLineString( 1_lnum ) ); } );
    results.setEncodingOverrideForFile( path, "ISO-8859-1" );
    REQUIRE( changed.count() == 1 );
    REQUIRE( observed.size() == 1 );
    REQUIRE( observed.front() != original );
    results.setEncodingOverrideForFile( path, "ISO-8859-1" );
    REQUIRE( changed.count() == 1 );
    results.clearEncodingOverrideForFile( path );
    REQUIRE( changed.count() == 2 );
    REQUIRE( observed.back() == original );
    results.clearEncodingOverrideForFile( path );
    REQUIRE( changed.count() == 2 );
}

TEST_CASE( "Folder marked and matched framing remains bound to the scanned codec",
           "[folder][folder-snapshot][folder-framing]" )
{
    QTemporaryDir dir;
    const auto path = QDir( dir.path() ).filePath( "utf16.log" );
    QByteArray bytes( "\xff\xfe", 2 );
    for ( const char character : QByteArray( "prefix\nERROR target\ntail\n" ) ) {
        bytes.append( character );
        bytes.append( '\0' );
    }
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( bytes ) == bytes.size() );
    file.close();
    RegularExpression expression{ RegularExpressionPattern{ "ERROR" } };
    auto matcher = expression.createMatcher();
    auto group = FolderSearchEngine::scanFile( path, *matcher, 19 );
    REQUIRE( group.matches.size() == 1 );
    const auto record = group.matches.front();
    REQUIRE( record.localLine == 1_lnum );
    const auto rawContent = group.content->readBytes(
        record.lineStartByte.get(), record.lineEndByte.get() - record.lineStartByte.get() - 2 );
    const auto expected = QString::fromLatin1( rawContent.constData(), rawContent.size() );
    FolderSearchResults matchedResults;
    matchedResults.setResults( { group } );
    group.matches.clear();
    QHash<QString, std::set<uint64_t>> marks;
    marks[ path ].insert( 1 );
    FolderSearchResults markedResults;
    markedResults.setMarksStore( &marks );
    markedResults.setResults( { std::move( group ) } );
    matchedResults.setEncodingOverrideForFile( path, "ISO-8859-1" );
    markedResults.setEncodingOverrideForFile( path, "ISO-8859-1" );
    REQUIRE( QFile::remove( path ) );
    const auto matched = matchedResults.createReadSnapshot( klogg::SnapshotScope::Matches );
    const auto marked = markedResults.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    const auto matchedText = matched->lineString( 0_lnum );
    const auto markedText = marked->lineString( 0_lnum );
    CHECK( matchedText == expected );
    CHECK( markedText == expected );
    CHECK( markedText == matchedText );
}

TEST_CASE( "Folder snapshot marked-row lookup reads requested rows rather than file prefixes",
           "[folder][folder-snapshot][folder-row-locator]" )
{
    QTemporaryDir dir;
    const auto path = QDir( dir.path() ).filePath( "many-rows.log" );
    constexpr uint64_t totalRows = 4096;
    constexpr uint64_t markedRows = 32;
    const QByteArray record = QByteArray( 127, 'x' ) + '\n';
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    const auto bytes = record.repeated( static_cast<int>( totalRows ) );
    REQUIRE( file.write( bytes ) == bytes.size() );
    file.close();
    RegularExpression expression{ RegularExpressionPattern{ "not-present" } };
    auto matcher = expression.createMatcher();
    auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.content );
    const auto source = group.content;
    QHash<QString, std::set<uint64_t>> marks;
    for ( uint64_t row = totalRows - markedRows; row < totalRows; ++row ) {
        marks[ path ].insert( row );
    }
    FolderSearchResults results;
    results.setMarksStore( &marks );
    results.setResults( { std::move( group ) } );
    const auto snapshot = results.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot->lineCount() == LinesCount( markedRows ) );
    REQUIRE( QFile::remove( path ) );
    const auto beforeRead = source->bytesReadForTesting();
    for ( uint64_t row = 0; row < markedRows; ++row ) {
        REQUIRE( snapshot->lineString( LineNumber( row ) ) == QString( 127, QLatin1Char( 'x' ) ) );
    }
    const auto readWork = source->bytesReadForTesting() - beforeRead;
    const auto maximumReadWork = markedRows
        * ( static_cast<uint64_t>( record.size() ) + 2U * sizeof( qint64 ) );
    REQUIRE( readWork > 0 );
    REQUIRE( readWork <= maximumReadWork );
}
