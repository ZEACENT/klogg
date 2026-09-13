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

// Unit tests for Overview aggregation across folder, single-file, and live
// sources, including exact pixel mapping and bounded work for dense results.

#include <catch2/catch.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>

#include "configuration.h"
#include "linetypes.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "overview.h"
#include "searchablelogdata.h"
#include "streaminglogdata.h"
#include "test_utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

TEST_CASE( "Overview folder-mode setMatchLines maps lines to y positions",
           "[overview][folder]" )
{
    Overview o;
    // 100000-line file, 200px tall overview.
    o.updateData( LinesCount( 100000 ) );

    // Lines 0 and 100 both map to y = 0 (collapse + weight bump);
    // 25000 -> y = 50; 50000 -> y = 100.
    o.setMatchLines( { 0_lnum, 100_lnum, 25000_lnum, 50000_lnum } );
    o.updateView( 200 );

    const auto* matches = o.getMatchLines();
    REQUIRE( matches != nullptr );
    REQUIRE( matches->size() == 3 ); // 0 and 100 collapse onto y=0
    REQUIRE( matches->at( 0 ).position() == 0 );
    REQUIRE( matches->at( 0 ).weight() == 1 ); // load() called once for line 100
    REQUIRE( matches->at( 1 ).position() == 50 );
    REQUIRE( matches->at( 1 ).weight() == 0 );
    REQUIRE( matches->at( 2 ).position() == 100 );
    REQUIRE( matches->at( 2 ).weight() == 0 );

    // Folder mode represents matches only: marks stay empty.
    const auto* marks = o.getMarkLines();
    REQUIRE( marks->empty() );
}

TEST_CASE( "Overview folder-mode y<->line mapping is inverse at exact points",
           "[overview][folder]" )
{
    Overview o;
    o.updateData( LinesCount( 100000 ) );
    o.setMatchLines( { 25000_lnum } );
    o.updateView( 200 );

    // yFromFileLine(25000) == 50 exactly, and fileLineFromY(50) == 25000.
    REQUIRE( o.yFromFileLine( 25000_lnum ) == 50 );
    REQUIRE( o.fileLineFromY( 50 ) == 25000_lnum );
}

TEST_CASE( "Overview folder-mode is reproducible across repeated setMatchLines",
           "[overview][folder]" )
{
    Overview o;
    o.updateData( LinesCount( 100000 ) );
    o.setMatchLines( { 100_lnum, 25000_lnum } );
    o.updateView( 200 );
    const auto sizeBefore = o.getMatchLines()->size();

    // A second setMatchLines on the same Overview must produce the same result
    // (no stale entries carried over from the previous call).
    o.setMatchLines( { 100_lnum, 25000_lnum } );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->size() == sizeBefore );
    REQUIRE( o.getMarkLines()->empty() );
}

TEST_CASE( "Overview setFilteredData drops a folder-mode match list",
           "[overview][folder]" )
{
    Overview o;
    o.updateData( LinesCount( 100000 ) );
    o.setMatchLines( { 100_lnum, 25000_lnum } );
    o.updateView( 200 );
    REQUIRE_FALSE( o.getMatchLines()->empty() );

    // Re-associating a (null) LogFilteredData must clear the explicit list, so a
    // later single-file attach cannot inherit folder marks.
    o.setFilteredData( nullptr );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->empty() );
    REQUIRE( o.getMarkLines()->empty() );

    // And the folder branch can be re-entered afterwards.
    o.setMatchLines( { 50000_lnum } );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->size() == 1 );
}

TEST_CASE( "Overview folder-mode with empty file or empty list draws nothing",
           "[overview][folder]" )
{
    Overview o;
    // No matches and a zero-line file: nothing is drawn.
    o.updateData( LinesCount( 0 ) );
    o.setMatchLines( { 10_lnum } );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->empty() );

    // Non-empty file but empty match list: still nothing.
    o.updateData( LinesCount( 1000 ) );
    o.setMatchLines( {} );
    o.updateView( 200 );
    REQUIRE( o.getMatchLines()->empty() );
}

TEST_CASE( "Folder overview preserves independent matches and marks with sparse pixels",
           "[overview][folder][aggregation]" )
{
    Overview overview;
    overview.updateData( 4_lcount );
    overview.setMatchLines( { 0_lnum, 3_lnum } );
    overview.setMarkLines( { 1_lnum, 3_lnum } );
    overview.updateView( 10 );

    const auto* matches = overview.getMatchLines();
    REQUIRE( matches->size() == 2 );
    REQUIRE( matches->at( 0 ).position() == 0 );
    REQUIRE( matches->at( 1 ).position() == 7 );
    const auto* marks = overview.getMarkLines();
    REQUIRE( marks->size() == 2 );
    REQUIRE( marks->at( 0 ).position() == 2 );
    REQUIRE( marks->at( 1 ).position() == 7 );
    REQUIRE( overview.lastAggregationWorkCountForTest() == 4 );

    overview.updateView( 0 );
    REQUIRE( overview.getMatchLines()->empty() );
    REQUIRE( overview.getMarkLines()->empty() );
    REQUIRE( overview.lastAggregationWorkCountForTest() == 0 );
}

TEST_CASE( "Overview aggregation maps maximum line counts without overflow",
           "[overview][folder][aggregation][boundaries]" )
{
    const auto lineCount = maxValue<LinesCount>();
    Overview overview;
    overview.updateData( lineCount );
    overview.setMatchLines(
        { 0_lnum, LineNumber( lineCount.get() / 2 ), LineNumber( lineCount.get() - 1 ) } );
    overview.updateView( 3 );

    const auto* matches = overview.getMatchLines();
    REQUIRE( matches->size() == 3 );
    for ( int position = 0; position < 3; ++position ) {
        REQUIRE( matches->at( static_cast<std::size_t>( position ) ).position()
                 == position );
        REQUIRE( matches->at( static_cast<std::size_t>( position ) ).weight() == 0 );
    }
    REQUIRE( overview.lastAggregationWorkCountForTest() == 3 );
}

TEST_CASE( "Dense folder results use viewport-bounded overview aggregation",
           "[overview][folder][aggregation][dense]" )
{
    constexpr int LineCount = 20000;
    constexpr unsigned Height = 37;
    std::vector<LineNumber> matchingLines;
    matchingLines.reserve( LineCount );
    for ( int line = 0; line < LineCount; ++line ) {
        matchingLines.emplace_back( LineNumber( static_cast<uint64_t>( line ) ) );
    }

    Overview overview;
    overview.updateData( LinesCount( LineCount ) );
    overview.setMatchLines( matchingLines );
    overview.updateView( Height );

    const auto* matches = overview.getMatchLines();
    REQUIRE( matches->size() == Height );
    for ( unsigned position = 0; position < Height; ++position ) {
        REQUIRE( matches->at( position ).position() == static_cast<int>( position ) );
        REQUIRE( matches->at( position ).weight()
                 == Overview::WeightedLine::WEIGHT_STEPS - 1 );
    }
    REQUIRE( overview.lastAggregationWorkCountForTest() == Height );
}

namespace {

QByteArray makeOverviewLog( int lineCount, const std::vector<int>& matchingLines )
{
    QByteArray data;
    data.reserve( lineCount * 12 );
    for ( int line = 0; line < lineCount; ++line ) {
        data.append( std::binary_search( matchingLines.cbegin(), matchingLines.cend(), line )
                         ? "MATCH "
                         : "plain " );
        data.append( QByteArray::number( line ) );
        data.append( '\n' );
    }
    return data;
}

std::optional<LinesCount>
waitForOverviewSearch( SafeQSignalSpy& searchProgressSpy,
                       LogFilteredData::SearchGeneration expectedGeneration,
                       int timeoutMs = 10000 )
{
    QElapsedTimer timer;
    timer.start();
    int consumedSignals = 0;
    while ( true ) {
        while ( consumedSignals < searchProgressSpy.count() ) {
            const auto args = searchProgressSpy.at( consumedSignals++ );
            if ( args.size() >= 4 && args.at( 1 ).toInt() == 100
                 && args.at( 3 ).toULongLong() == expectedGeneration ) {
                return args.at( 0 ).value<LinesCount>();
            }
        }

        const auto remaining = timeoutMs - static_cast<int>( timer.elapsed() );
        if ( remaining <= 0 ) {
            return std::nullopt;
        }
        searchProgressSpy.wait( qMin( 100, remaining ) );
    }
}

class OverviewSearchConfigGuard {
  public:
    OverviewSearchConfigGuard()
        : config_( Configuration::get() )
        , previousThreadPoolSize_( config_.searchThreadPoolSize() )
        , previousParallelSearch_( config_.useParallelSearch() )
        , previousResultsCache_( config_.useSearchResultsCache() )
        , previousRegexpEngine_( config_.regexpEngine() )
    {
        config_.setSearchThreadPoolSize( 0 );
        config_.setUseParallelSearch( false );
        config_.setUseSearchResultsCache( false );
        configureProductLikeRegexpEngine( config_ );
    }

    ~OverviewSearchConfigGuard()
    {
        config_.setSearchThreadPoolSize( previousThreadPoolSize_ );
        config_.setUseParallelSearch( previousParallelSearch_ );
        config_.setUseSearchResultsCache( previousResultsCache_ );
        config_.setRegexpEnging( previousRegexpEngine_ );
    }

    OverviewSearchConfigGuard( const OverviewSearchConfigGuard& ) = delete;
    OverviewSearchConfigGuard& operator=( const OverviewSearchConfigGuard& ) = delete;

  private:
    Configuration& config_;
    int previousThreadPoolSize_;
    bool previousParallelSearch_;
    bool previousResultsCache_;
    RegexpEngine previousRegexpEngine_;
};

class OverviewFilteredDataFixture {
  public:
    OverviewFilteredDataFixture( QByteArray contents, bool liveSource )
        : file_( QDir( temporaryDirectory_.path() )
                     .filePath( QStringLiteral( "overview_XXXXXX.log" ) ) )
    {
        REQUIRE( temporaryDirectory_.isValid() );

        if ( liveSource ) {
            auto source = std::make_unique<StreamingLogData>(
                QUuid::createUuid().toString( QUuid::WithoutBraces ),
                temporaryDirectory_.path() );
            SafeQSignalSpy readySpy{ source.get(),
                                     &StreamingLogData::loadingFinished };
            REQUIRE( readySpy.safeWait() );
            readySpy.clear();
            if ( !contents.isEmpty() ) {
                source->appendUtf8( contents );
                REQUIRE( readySpy.safeWait() );
            }
            sourceData_ = std::move( source );
        }
        else {
            REQUIRE( file_.open() );
            REQUIRE( file_.write( contents ) == contents.size() );
            REQUIRE( file_.flush() );

            auto source = std::make_unique<LogData>();
            SafeQSignalSpy readySpy{ source.get(), &LogData::loadingFinished };
            source->attachFile( file_.fileName() );
            REQUIRE( readySpy.safeWait() );
            sourceData_ = std::move( source );
        }

        filteredData_ = sourceData_->getNewFilteredData();
        REQUIRE( filteredData_ != nullptr );
    }

    LogFilteredData& filteredData() { return *filteredData_; }
    LinesCount lineCount() const { return sourceData_->getNbLine(); }

    void search( LinesCount expectedMatches )
    {
        SafeQSignalSpy searchProgressSpy{ filteredData_.get(),
                                          &LogFilteredData::searchProgressed };
        filteredData_->runSearch( RegularExpressionPattern( QStringLiteral( "MATCH" ) ) );
        const auto terminalResult = waitForOverviewSearch(
            searchProgressSpy, filteredData_->currentSearchGeneration() );
        REQUIRE( terminalResult.has_value() );
        REQUIRE( *terminalResult == expectedMatches );
    }

  private:
    OverviewSearchConfigGuard searchConfigGuard_;
    QTemporaryDir temporaryDirectory_;
    QTemporaryFile file_;
    std::unique_ptr<SearchableLogData> sourceData_;
    std::unique_ptr<LogFilteredData> filteredData_;
};

void requireWeightedLines(
    const klogg::vector<Overview::WeightedLine>* actual,
    const std::vector<std::pair<int, int>>& expectedPositionAndWeight )
{
    REQUIRE( actual != nullptr );
    REQUIRE( actual->size() == expectedPositionAndWeight.size() );
    for ( std::size_t index = 0; index < expectedPositionAndWeight.size(); ++index ) {
        REQUIRE( actual->at( index ).position()
                 == expectedPositionAndWeight.at( index ).first );
        REQUIRE( actual->at( index ).weight()
                 == expectedPositionAndWeight.at( index ).second );
    }
}

} // namespace

TEST_CASE( "Overview LogFilteredData path maps exact pixels, applies precedence, and caps weights",
           "[overview][single-file][aggregation]" )
{
    const std::vector<int> matchingLines{ 0, 1, 2, 3, 5, 7, 19 };
    OverviewFilteredDataFixture fixture( makeOverviewLog( 20, matchingLines ), false );
    fixture.search( 7_lcount );

    auto& filteredData = fixture.filteredData();
    filteredData.addMark( 4_lnum );
    filteredData.addMark( 5_lnum ); // Match takes precedence over this mark.
    filteredData.addMark( 6_lnum );
    filteredData.addMark( 8_lnum );
    filteredData.addMark( 18_lnum );

    Overview overview;
    overview.setFilteredData( &filteredData );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 4 );

    requireWeightedLines( overview.getMatchLines(), { { 0, 2 }, { 1, 1 }, { 3, 0 } } );
    requireWeightedLines( overview.getMarkLines(), { { 0, 0 }, { 1, 1 }, { 3, 0 } } );
}

TEST_CASE( "Overview LogFilteredData path leaves gaps when pixels outnumber source lines",
           "[overview][single-file][aggregation]" )
{
    OverviewFilteredDataFixture fixture( makeOverviewLog( 4, { 0, 3 } ), false );
    fixture.search( 2_lcount );
    fixture.filteredData().addMark( 1_lnum );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 10 );

    requireWeightedLines( overview.getMatchLines(), { { 0, 0 }, { 7, 0 } } );
    requireWeightedLines( overview.getMarkLines(), { { 2, 0 } } );
    REQUIRE( overview.lastAggregationWorkCountForTest() == fixture.lineCount().get() );
}

TEST_CASE( "Overview skips aggregation when matches and marks are hidden",
           "[overview][single-file][aggregation][empty]" )
{
    OverviewFilteredDataFixture fixture( makeOverviewLog( 100, { 0 } ), false );
    fixture.search( 1_lcount );
    fixture.filteredData().addMark( 1_lnum );
    fixture.filteredData().setVisibility( LogFilteredData::VisibilityFlags::None );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 40 );

    REQUIRE( overview.getMatchLines()->empty() );
    REQUIRE( overview.getMarkLines()->empty() );
    REQUIRE( overview.lastAggregationWorkCountForTest() == 0 );
}

TEST_CASE( "Overview LogFilteredData path draws nothing at zero height",
           "[overview][single-file][aggregation]" )
{
    OverviewFilteredDataFixture fixture( makeOverviewLog( 4, { 0, 3 } ), false );
    fixture.search( 2_lcount );
    fixture.filteredData().addMark( 1_lnum );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 0 );

    REQUIRE( overview.getMatchLines()->empty() );
    REQUIRE( overview.getMarkLines()->empty() );
}

TEST_CASE( "Overview LogFilteredData path draws nothing for an empty source",
           "[overview][single-file][aggregation]" )
{
    OverviewFilteredDataFixture fixture( {}, false );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 20 );

    REQUIRE( overview.getMatchLines()->empty() );
    REQUIRE( overview.getMarkLines()->empty() );
}

TEST_CASE( "Overview does not turn all-lines-visible plain lines into marks",
           "[overview][single-file][aggregation][all-lines-visible]" )
{
    OverviewFilteredDataFixture fixture( makeOverviewLog( 6, { 2 } ), false );
    fixture.search( 1_lcount );
    fixture.filteredData().setAllLinesVisible( true );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 3 );

    requireWeightedLines( overview.getMatchLines(), { { 1, 0 } } );
    REQUIRE( overview.getMarkLines()->empty() );
}

TEST_CASE( "Live LogFilteredData uses the same overview aggregation path",
           "[overview][live][aggregation]" )
{
    OverviewFilteredDataFixture fixture( makeOverviewLog( 10, { 0, 9 } ), true );
    fixture.search( 2_lcount );
    fixture.filteredData().addMark( 4_lnum );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( 5 );

    requireWeightedLines( overview.getMatchLines(), { { 0, 0 }, { 4, 0 } } );
    requireWeightedLines( overview.getMarkLines(), { { 2, 0 } } );
}

TEST_CASE( "Dense LogFilteredData results fill each overview pixel with capped weight",
           "[overview][single-file][aggregation][dense]" )
{
    constexpr int LineCount = 20000;
    constexpr unsigned Height = 37;
    OverviewFilteredDataFixture fixture( QByteArrayLiteral( "MATCH\n" ).repeated( LineCount ),
                                         false );
    fixture.search( LinesCount( LineCount ) );

    Overview overview;
    overview.setFilteredData( &fixture.filteredData() );
    overview.updateData( fixture.lineCount() );
    overview.updateView( Height );

    const auto* matches = overview.getMatchLines();
    REQUIRE( matches->size() == Height );
    for ( unsigned position = 0; position < Height; ++position ) {
        REQUIRE( matches->at( position ).position() == static_cast<int>( position ) );
        REQUIRE( matches->at( position ).weight() == Overview::WeightedLine::WEIGHT_STEPS - 1 );
    }
    REQUIRE( overview.getMarkLines()->empty() );

    // Test-only instrumentation counts aggregation work units, not wall time.
    // A 20,000-result input must issue one bounded range aggregation per output
    // pixel rather than visiting every result through iterateOverLines().
    REQUIRE( overview.lastAggregationWorkCountForTest() == Height );
}
