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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg. If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <memory>
#include <set>
#include <vector>

#include "abstractlogdata.h"
#include "containers.h"
#include "highlighterset.h"
#include "logsnapshot.h"
#include "quickfind.h"
#include "quickfindpattern.h"
#include "regularexpressionpattern.h"
#include "test_utils.h"

namespace {
struct ReadGate {
    std::atomic<bool> firstRead{ true };
    QSemaphore entered;
    QSemaphore resume;

    void pause()
    {
        if ( firstRead.exchange( false ) ) {
            entered.release();
            resume.acquire();
        }
    }
};

struct ReadObservations {
    std::atomic<int> acquisitions{ 0 };
    std::atomic<int> mutableReads{ 0 };
    std::atomic<int> snapshotReads{ 0 };
    std::atomic<bool> acquiredOnOwnerThread{ false };
    std::atomic<bool> presentedScope{ false };
    std::shared_ptr<ReadGate> gate;
};

class NavigationData final : public AbstractLogData {
public:
    QStringList lines{ QStringLiteral( "accepted" ) };
    std::set<uint64_t> headers;
    uint64_t incarnation = 71;
    bool supportsSnapshots = true;
    bool failAcquisition = false;
    bool failRead = false;
    std::shared_ptr<ReadObservations> observations = std::make_shared<ReadObservations>();

    class Snapshot final : public klogg::LogSnapshot {
    public:
        struct Row {
            QString text;
            LineNumber presented;
            klogg::SnapshotRow metadata;
        };
        std::vector<Row> rows;
        bool failRead = false;
        std::shared_ptr<ReadObservations> observations;

        LinesCount lineCount() const override { return LinesCount{ rows.size() }; }
        klogg::SnapshotRow row( LineNumber index ) const override
        {
            return rows.at( static_cast<size_t>( index.get() ) ).metadata;
        }
        QString lineString( LineNumber index ) const override
        {
            if ( observations->gate ) observations->gate->pause();
            ++observations->snapshotReads;
            if ( failRead ) throw klogg::SnapshotError{ "Injected snapshot read failure" };
            return rows.at( static_cast<size_t>( index.get() ) ).text;
        }
        OptionalLineNumber presentedRowForIdentity( const klogg::LogRowId& identity ) const override
        {
            for ( const auto& entry : rows ) {
                if ( entry.metadata.identity == identity ) return entry.presented;
            }
            return {};
        }
    };

    klogg::LogSnapshot::Ptr createReadSnapshot( klogg::SnapshotScope scope ) const override
    {
        ++observations->acquisitions;
        observations->acquiredOnOwnerThread = QThread::currentThread() == thread();
        observations->presentedScope = scope == klogg::SnapshotScope::PresentedRows;
        if ( failAcquisition ) throw klogg::SnapshotError{ "Injected acquisition failure" };
        if ( !supportsSnapshots ) return {};
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->observations = observations;
        snapshot->failRead = failRead;
        for ( klogg::ContainerIndex index = 0; index < lines.size(); ++index ) {
            const auto presented = static_cast<uint64_t>( index );
            if ( headers.count( presented ) != 0 ) continue;
            snapshot->rows.push_back( {
                lines.at( index ), LineNumber{ presented },
                { { klogg::LogRowId::Origin::Memory, incarnation, 3, 1000 + presented },
                  LineNumber{ 100 + presented }, true, false } } );
        }
        return snapshot;
    }

protected:
    QString doGetLineString( LineNumber line ) const override
    {
        // Both old mutable reads and future snapshot reads stop at the same
        // deterministic boundary. A mutation happens before this read resumes.
        if ( observations->gate ) observations->gate->pause();
        ++observations->mutableReads;
        return lines.at( static_cast<klogg::ContainerIndex>( line.get() ) );
    }
    QString doGetExpandedLineString( LineNumber line ) const override
    {
        return untabify( doGetLineString( line ) );
    }
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount count ) const override
    {
        klogg::vector<QString> result;
        for ( auto line = first; line < first + count; ++line ) {
            result.push_back( doGetLineString( line ) );
        }
        return result;
    }
    klogg::vector<QString> doGetExpandedLines( LineNumber first, LinesCount count ) const override
    {
        auto result = doGetLines( first, count );
        for ( auto& line : result ) line = untabify( std::move( line ) );
        return result;
    }
    LineNumber doGetLineNumber( LineNumber line ) const override
    {
        ++observations->mutableReads;
        return LineNumber{ 100 + line.get() };
    }
    bool doIsLineCopyable( LineNumber line ) const override
    {
        ++observations->mutableReads;
        return headers.count( line.get() ) == 0;
    }
    LinesCount doGetNbLine() const override
    {
        ++observations->mutableReads;
        return LinesCount{ static_cast<uint64_t>( lines.size() ) };
    }
    LineLength doGetMaxLength() const override { return 100_length; }
    LineLength doGetLineLength( LineNumber line ) const override
    {
        return LineLength{ doGetExpandedLineString( line ).size() };
    }
    void doSetDisplayEncoding( const char* ) override {}
    QTextCodec* doGetDisplayEncoding() const override { return QTextCodec::codecForName( "UTF-8" ); }
    void doAttachReader() const override {}
    void doDetachReader() const override {}
};

// Unblock before joining even when a REQUIRE fails. The gate is test-owned and
// shared by source/snapshot, never a timing switch in the production worker.
struct SearchCleanup {
    QuickFind& finder;
    std::shared_ptr<ReadGate> gate;
    ~SearchCleanup()
    {
        if ( gate ) gate->resume.release();
        finder.stopSearch();
    }
};

QuickFindResult takeResult( SafeQSignalSpy& completed )
{
    REQUIRE( completed.safeWait( 5000 ) );
    REQUIRE( completed.count() == 1 );
    return qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) );
}

void startNavigation( QuickFind& finder, const QuickFindPattern& pattern,
                      const Selection& cursor, bool backward, bool incremental )
{
    if ( incremental ) {
        if ( backward ) finder.incrementallySearchBackward( cursor, pattern.getMatcher() );
        else finder.incrementallySearchForward( cursor, pattern.getMatcher() );
    }
    else {
        if ( backward ) finder.searchBackward( cursor, pattern.getMatcher() );
        else finder.searchForward( cursor, pattern.getMatcher() );
    }
}
void requireQueryParity( const QuickFindPattern& quickFind,
                         const RegularExpressionPattern& filter, const Highlighter& highlighter,
                         const QString& text )
{
    const auto filterExpression = static_cast<QRegularExpression>( filter );
    const bool expected = filterExpression.match( text ).hasMatch();
    const auto matcher = quickFind.getMatcher();
    // Check actual navigation and painting before inspecting construction flags.
    CHECK( matcher.isLineMatching( text ) == expected );
    klogg::vector<HighlightedMatch> quickMatches;
    klogg::vector<HighlightedMatch> highlighterMatches;
    CHECK( quickFind.matchLine( text, quickMatches ) == expected );
    CHECK( highlighter.matchLine( text, highlighterMatches ) == expected );
    CHECK( quickFind.isCaseSensitive() == filter.isCaseSensitive );
    REQUIRE( quickMatches.size() == highlighterMatches.size() );
    for ( size_t index = 0; index < quickMatches.size(); ++index ) {
        CHECK( quickMatches[ index ].startColumn() == highlighterMatches[ index ].startColumn() );
        CHECK( quickMatches[ index ].size() == highlighterMatches[ index ].size() );
    }
}
void requireCaptureReferenceParity( const QString& query,
                                    const std::vector<std::pair<QString, bool>>& examples )
{
    CAPTURE( query.toStdString() );
    const QRegularExpression reference{ query, QRegularExpression::UseUnicodePropertiesOption };
    REQUIRE( reference.isValid() );
    const RegularExpressionPattern filter{ query, true, false, false, false };
    const auto predicate = static_cast<QRegularExpression>( filter );
    const auto extraction = filter.toRegularExpression();
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( query, false, true, false );
    const auto navigator = quickFind.getMatcher();
    // A conditional can match even when its optional capture does not
    // participate. Whole-line highlighters preserve this predicate behavior;
    // an empty projected fragment list is not the same as a failed occurrence.
    Highlighter highlighter{ query, false, false, Qt::black, Qt::yellow };
    CHECK( predicate.isValid() );
    REQUIRE( extraction.isValid() );
    REQUIRE( quickFind.regularExpression().isValid() );
    for ( const auto& example : examples ) {
        const auto& text = example.first;
        const bool expected = example.second;
        CAPTURE( text.toStdString(), expected );
        const auto referenceMatch = reference.match( text );
        REQUIRE( referenceMatch.hasMatch() == expected );
        const bool predicateMatched = predicate.isValid() && predicate.match( text ).hasMatch();
        CHECK( predicateMatched == expected );
        const auto extracted = extraction.match( text );
        CHECK( extracted.hasMatch() == expected );
        CHECK( navigator.isLineMatching( text ) == expected );
        klogg::vector<HighlightedMatch> highlights;
        CHECK( highlighter.matchLine( text, highlights ) == expected );
        if ( expected ) {
            CHECK( extracted.capturedTexts() == referenceMatch.capturedTexts() );
            CHECK( navigator.lastOccurrence().capturedTexts() == referenceMatch.capturedTexts() );
        }
    }
}
} // namespace

TEST_CASE( "Quick Find navigation reads the accepted source version after mutation",
           "[quickfind-snapshot]" )
{
    for ( const bool backward : { false, true } ) {
        for ( const bool incremental : { false, true } ) {
            CAPTURE( backward, incremental );
            NavigationData source;
            const auto observations = source.observations;
            observations->gate = std::make_shared<ReadGate>();
            QuickFindPattern pattern;
            pattern.changeSearchPattern( QStringLiteral( "accepted" ) );
            QuickFind finder{ source };
            SearchCleanup cleanup{ finder, observations->gate };
            SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
            Selection cursor;
            cursor.setNavigationBounds( FilePosition{ 0_lnum, 100_lcol },
                                        FilePosition{ 0_lnum, 0_lcol } );
            startNavigation( finder, pattern, cursor, backward, incremental );
            REQUIRE( observations->gate->entered.tryAcquire( 1, 5000 ) );
            source.lines[ 0 ] = QStringLiteral( "replaced" );
            ++source.incarnation;
            observations->gate->resume.release();
            auto result = takeResult( completed );
            REQUIRE( result.found );
            REQUIRE( result.requestId == finder.currentRequestId() );
            REQUIRE( result.rowIdentity == std::optional<klogg::LogRowId>{
                { klogg::LogRowId::Origin::Memory, 71, 3, 1000 } } );
            REQUIRE( result.selection.getSelectedText( nullptr ) == QStringLiteral( "accepted" ) );
            REQUIRE( observations->acquisitions.load() == 1 );
            REQUIRE( observations->acquiredOnOwnerThread.load() );
            REQUIRE( observations->presentedScope.load() );
            REQUIRE( observations->mutableReads.load() == 0 );
            REQUIRE( observations->snapshotReads.load() == 1 );
            std::vector<klogg::LogRowId> identities;
            result.selection.remapCapturedRows( [ & ]( const klogg::LogRowId& identity ) {
                identities.push_back( identity );
                return OptionalLineNumber{ 7_lnum };
            } );
            REQUIRE( identities == std::vector<klogg::LogRowId>{
                { klogg::LogRowId::Origin::Memory, 71, 3, 1000 } } );
            REQUIRE( result.selection.getLines() == klogg::vector<LineNumber>{ 7_lnum } );
        }
    }
}

TEST_CASE( "Quick Find accepted navigation does not retain a mutable source owner",
           "[quickfind-snapshot]" )
{
    auto source = std::make_unique<NavigationData>();
    const auto observations = source->observations;
    observations->gate = std::make_shared<ReadGate>();
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "accepted" ) );
    QuickFind finder{ *source };
    SearchCleanup cleanup{ finder, observations->gate };
    SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
    finder.searchForward( Selection{}, pattern.getMatcher() );
    // Fail safely on the old implementation before deleting an owner that its
    // worker still references. This is also the synchronous acquisition contract.
    REQUIRE( observations->acquisitions.load() == 1 );
    REQUIRE( observations->gate->entered.tryAcquire( 1, 5000 ) );
    source.reset();
    observations->gate->resume.release();
    const auto result = takeResult( completed );
    REQUIRE( result.found );
    REQUIRE( result.selection.getSelectedText( nullptr ) == QStringLiteral( "accepted" ) );
    REQUIRE( observations->mutableReads.load() == 0 );
}

TEST_CASE( "Quick Find snapshot ordinals preserve header gaps in navigation coordinates",
           "[quickfind-snapshot]" )
{
    for ( const bool backward : { false, true } ) {
        for ( const bool incremental : { false, true } ) {
            CAPTURE( backward, incremental );
            NavigationData source;
            source.lines = QStringList{ QStringLiteral( "preTOKENpost" ),
                                        QStringLiteral( "\tpreTOKENpost" ),
                                        QStringLiteral( "preTOKENpost" ),
                                        QStringLiteral( "preTOKENpost" ),
                                        QStringLiteral( "preTOKENpost" ),
                                        QStringLiteral( "preTOKENpost" ) };
            source.headers = { 0, 2, 3, 5 };
            QuickFindPattern pattern;
            pattern.changeSearchPattern( QStringLiteral( "(?:pre)(TOKEN)post" ), false, true, false );
            QuickFind finder{ source };
            SearchCleanup cleanup{ finder, {} };
            SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
            Selection cursor;
            cursor.setNavigationBounds( FilePosition{ 3_lnum, 0_lcol },
                                        FilePosition{ 3_lnum, 0_lcol } );
            startNavigation( finder, pattern, cursor, backward, incremental );
            auto result = takeResult( completed );
            const auto expectedLine = backward ? 1_lnum : 4_lnum;
            const auto expectedStart = backward ? 8_lcol : 0_lcol;
            REQUIRE( result.found );
            REQUIRE( result.line == expectedLine );
            REQUIRE( result.selection.getSelectedText( nullptr ) == QStringLiteral( "TOKEN" ) );
            REQUIRE( result.selection.getLines() == klogg::vector<LineNumber>{ expectedLine } );
            REQUIRE( result.selection.getPreviousPosition()
                     == FilePosition{ expectedLine, expectedStart } );
            REQUIRE( result.selection.getNextPosition()
                     == FilePosition{ expectedLine, expectedStart + 12_length } );
            const auto portions = result.selection.getPortionsForLine( expectedLine );
            REQUIRE( portions.size() == 1 );
            REQUIRE( portions.front().startColumn() == expectedStart + 3_length );
            std::vector<klogg::LogRowId> identities;
            result.selection.remapCapturedRows( [ & ]( const klogg::LogRowId& identity ) {
                identities.push_back( identity );
                return OptionalLineNumber{ expectedLine };
            } );
            REQUIRE( identities == std::vector<klogg::LogRowId>{
                { klogg::LogRowId::Origin::Memory, 71, 3, 1000 + expectedLine.get() } } );
            REQUIRE( source.observations->mutableReads.load() == 0 );
        }
    }
}

TEST_CASE( "Quick Find zero-width snapshot occurrences carry stable identity",
           "[quickfind-snapshot]" )
{
    NavigationData source;
    source.lines = QStringList{ QStringLiteral( "header" ), QStringLiteral( "value" ) };
    source.headers = { 0 };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "$" ), false, true, false );
    QuickFind finder{ source };
    SearchCleanup cleanup{ finder, {} };
    SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
    finder.searchForward( Selection{}, pattern.getMatcher() );
    const auto result = takeResult( completed );
    REQUIRE( result.found );
    REQUIRE( result.line == 1_lnum );
    REQUIRE( result.selection.isEmpty() );
    REQUIRE( result.rowIdentity == std::optional<klogg::LogRowId>{
        { klogg::LogRowId::Origin::Memory, 71, 3, 1001 } } );
    REQUIRE( result.selection.getPreviousPosition() == FilePosition{ 1_lnum, 5_lcol } );
    REQUIRE( result.selection.getNextPosition() == FilePosition{ 2_lnum, 0_lcol } );
    REQUIRE( result.requestId == finder.currentRequestId() );
}

TEST_CASE( "Quick Find queued completions retain their accepted request identity",
           "[quickfind-snapshot][quickfind-request]" )
{
    NavigationData source;
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "accepted" ) );
    QuickFind finder{ source };
    SearchCleanup cleanup{ finder, {} };
    SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
    QObject receiver;
    std::vector<std::pair<quint64, quint64>> delivered;
    QObject::connect( &finder, &QuickFind::searchDone, &receiver,
                      [ & ]( const QuickFindResult& result ) {
                          delivered.emplace_back( result.requestId, finder.currentRequestId() );
                      }, Qt::QueuedConnection );
    bool secondRequested = false;
    QObject::connect( &finder, &QuickFind::searchDone, &finder,
                      [ & ]( const QuickFindResult& ) {
                          if ( !secondRequested ) {
                              secondRequested = true;
                              // Before the queued receiver can consume the first
                              // completion, a new real request supersedes it.
                              finder.searchForward( Selection{}, pattern.getMatcher() );
                          }
                      }, Qt::DirectConnection );
    finder.searchForward( Selection{}, pattern.getMatcher() );
    const auto firstRequest = finder.currentRequestId();
    for ( int index = 0; index < 2; ++index ) {
        REQUIRE( completed.safeWait( 5000 ) );
        const auto result = qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) );
        REQUIRE( result.found );
        REQUIRE( result.requestId == ( index == 0 ? firstRequest : finder.currentRequestId() ) );
    }
    QCoreApplication::sendPostedEvents( &receiver, QEvent::MetaCall );
    REQUIRE( delivered.size() == 2 );
    REQUIRE( delivered[ 0 ].first == firstRequest );
    REQUIRE( delivered[ 0 ].first < delivered[ 0 ].second );
    REQUIRE( delivered[ 1 ].first == delivered[ 1 ].second );
    const auto lastRequest = finder.currentRequestId();
    finder.stopSearch();
    REQUIRE( finder.currentRequestId() > lastRequest );
}

TEST_CASE( "Numeric backreferences have identical Filter extraction and Quick Find predicates",
           "[query-options][query-capture-references]" )
{
    requireCaptureReferenceParity( QStringLiteral( R"(\A(a)\1\z)" ),
        { { QStringLiteral( "aa" ), true }, { QStringLiteral( "ab" ), false } } );
}

TEST_CASE( "Unnamed and named groups retain the same numeric reference numbering",
           "[query-options][query-capture-references]" )
{
    requireCaptureReferenceParity( QStringLiteral( R"(\A(a)(?<x>b)\1\z)" ),
        { { QStringLiteral( "aba" ), true }, { QStringLiteral( "abb" ), false } } );
}

TEST_CASE( "Capture conditionals retain identical branch semantics across query consumers",
           "[query-options][query-capture-references]" )
{
    SECTION( "optional unnamed capture controls the branch" )
    {
        requireCaptureReferenceParity( QStringLiteral( R"(\A(a)?(?(1)b|c)\z)" ),
            { { QStringLiteral( "ab" ), true }, { QStringLiteral( "c" ), true },
              { QStringLiteral( "b" ), false }, { QStringLiteral( "ac" ), false } } );
    }
    SECTION( "a named group must not take the unnamed capture's number" )
    {
        requireCaptureReferenceParity( QStringLiteral( R"(\A(a)?(?<x>b)(?(1)c|d)\z)" ),
            { { QStringLiteral( "abc" ), true }, { QStringLiteral( "bd" ), true },
              { QStringLiteral( "bc" ), false }, { QStringLiteral( "abd" ), false } } );
    }
}

TEST_CASE( "Fresh Quick Find regex queries use the same Unicode word semantics as Filter",
           "[query-options][query-options-regression]" )
{
    const auto query = QStringLiteral( R"(^\w+$)" );
    const auto text = QString{ QChar{ 0x00e9 } };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( query, true, false );
    RegularExpressionPattern filter{ query, quickFind.isCaseSensitive(), false, false, false };
    Highlighter highlighter{ query, !filter.isCaseSensitive, true, Qt::black, Qt::yellow };
    REQUIRE( static_cast<QRegularExpression>( filter ).match( text ).hasMatch() );
    requireQueryParity( quickFind, filter, highlighter, text );
}

TEST_CASE( "Fresh Quick Find case flags agree with its advertised default",
           "[query-options][query-options-regression]" )
{
    const auto query = QStringLiteral( "error" );
    QuickFindPattern quickFind;
    REQUIRE_FALSE( quickFind.isCaseSensitive() );
    quickFind.changeSearchPattern( query );
    RegularExpressionPattern filter{ query, quickFind.isCaseSensitive(), false, false, true };
    Highlighter highlighter{ query, !filter.isCaseSensitive, true, Qt::black, Qt::yellow };
    highlighter.setUseRegex( false );
    requireQueryParity( quickFind, filter, highlighter, QStringLiteral( "ERROR" ) );
}

TEST_CASE( "Explicit Quick Find case policy survives three-argument pattern replacement",
           "[query-options][query-options-characterization]" )
{
    for ( const bool ignoreCase : { false, true } ) {
        CAPTURE( ignoreCase );
        QuickFindPattern quickFind;
        quickFind.changeSearchPattern( QStringLiteral( "prefix(\\w+)" ), ignoreCase, true, false );
        RegularExpressionPattern filter{ QStringLiteral( "prefix(\\w+)" ), !ignoreCase,
                                         false, false, false };
        Highlighter highlighter{ filter.pattern, ignoreCase, true, Qt::black, Qt::yellow };
        requireQueryParity( quickFind, filter, highlighter,
                            QStringLiteral( "PREFIX" ) + QChar{ 0x00e9 } );
        quickFind.changeSearchPattern( QStringLiteral( "other(\\w+)" ), true, false );
        filter.pattern = QStringLiteral( "other(\\w+)" );
        Highlighter replacement{ filter.pattern, ignoreCase, true, Qt::black, Qt::yellow };
        requireQueryParity( quickFind, filter, replacement,
                            QStringLiteral( "OTHER" ) + QChar{ 0x00f1 } );
    }
}

TEST_CASE( "Shared query construction preserves projected captures and literal escaping",
           "[query-options]" )
{
    RegularExpressionPattern query{ QStringLiteral( "(prefix)(\\w+)" ), false,
                                    false, false, false };
    const auto text = QStringLiteral( "PREFIX" ) + QChar{ 0x00e9 };
    const auto predicate = static_cast<QRegularExpression>( query );
    const auto capturing = query.toRegularExpression();
    REQUIRE( predicate.match( text ).hasMatch() );
    const auto occurrence = capturing.match( text );
    REQUIRE( occurrence.hasMatch() );
    REQUIRE( capturing.captureCount() == 2 );
    REQUIRE( occurrence.captured( 1 ) == QStringLiteral( "PREFIX" ) );
    REQUIRE( occurrence.captured( 2 ) == QString{ QChar{ 0x00e9 } } );
    query.isPlainText = true;
    const auto literal = query.toRegularExpression();
    REQUIRE_FALSE( literal.match( text ).hasMatch() );
    REQUIRE( literal.match( query.pattern ).hasMatch() );
    REQUIRE( literal.captureCount() == 0 );
}

TEST_CASE( "Shared query options preserve literal escaping and whole-word boundaries",
           "[query-options][query-options-characterization]" )
{
    const auto literal = QStringLiteral( "a+b" );
    const auto text = QStringLiteral( "aab a+b A+B xa+by" );
    for ( const bool ignoreCase : { false, true } ) {
        for ( const bool wholeWord : { false, true } ) {
            CAPTURE( ignoreCase, wholeWord );
            QuickFindPattern quickFind;
            quickFind.changeSearchPattern( literal, ignoreCase, false, wholeWord );
            const auto expectedPattern = wholeWord
                ? QStringLiteral( "\\b" ) + QRegularExpression::escape( literal )
                      + QStringLiteral( "\\b" )
                : QRegularExpression::escape( literal );
            REQUIRE( quickFind.regularExpression().pattern() == expectedPattern );
            RegularExpressionPattern filter{ wholeWord ? expectedPattern : literal, !ignoreCase,
                                             false, false, !wholeWord };
            Highlighter highlighter{ wholeWord ? expectedPattern : literal, ignoreCase, true,
                                     Qt::black, Qt::yellow };
            highlighter.setUseRegex( wholeWord );
            requireQueryParity( quickFind, filter, highlighter, text );
            klogg::vector<HighlightedMatch> matches;
            REQUIRE( quickFind.matchLine( text, matches ) );
            const size_t expectedCount = ( ignoreCase ? 2U : 1U ) + ( wholeWord ? 0U : 1U );
            REQUIRE( matches.size() == expectedCount );
        }
    }
}

TEST_CASE( "Quick Find rejects unavailable snapshots without mutable fallback",
           "[quickfind-snapshot]" )
{
    NavigationData source;
    SECTION( "unsupported source" ) { source.supportsSnapshots = false; }
    SECTION( "acquisition failure" ) { source.failAcquisition = true; }
    SECTION( "snapshot read failure" ) { source.failRead = true; }
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "accepted" ) );
    QuickFind finder{ source };
    SearchCleanup cleanup{ finder, {} };
    SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
    REQUIRE_NOTHROW( finder.searchForward( Selection{}, pattern.getMatcher() ) );
    REQUIRE_FALSE( takeResult( completed ).found );
    REQUIRE( source.observations->acquisitions.load() == 1 );
    REQUIRE( source.observations->mutableReads.load() == 0 );
}
