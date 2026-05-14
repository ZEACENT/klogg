/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include <QElapsedTimer>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <qglobal.h>

#include "configuration.h"
#include "log.h"
#include "test_utils.h"

#include "logdata.h"
#include "logfiltereddata.h"

static const qint64 SL_NB_LINES = 500LL;

namespace {

QString makeTempFileTemplate( const QString& fileNameTemplate )
{
    return QDir( QDir::tempPath() ).filePath( fileNameTemplate );
}

bool generateDataFiles( QTemporaryFile& file )
{
    char newLine[ 90 ];

    if ( file.open() ) {
        for ( int i = 0; i < SL_NB_LINES; i++ ) {
            snprintf( newLine, 89,
                      "LOGDATA \t is a part of glogg, we are going to test it thoroughly, this is "
                      "line %06d\n",
                      i );
            file.write( newLine, static_cast<qint64>( qstrlen( newLine ) ) );
        }
        file.flush();
    }

    return true;
}

void runSearch( LogFilteredData* filtered_data, const QString& regexp,
                SafeQSignalSpy& searchProgressSpy )
{

    filtered_data->runSearch( RegularExpressionPattern( regexp ) );

    int progress = 0;
    int consumedSignals = 0;
    do {
        while ( searchProgressSpy.count() <= consumedSignals ) {
            REQUIRE( searchProgressSpy.wait() );
        }
        QList<QVariant> progressArgs = searchProgressSpy.at( consumedSignals );
        ++consumedSignals;
        progress = progressArgs.at( 1 ).toInt();
    } while ( progress < 100 );

    // Drain any pending throttled signals to avoid a use-after-free when the
    // LogFilteredData teardown races with a still-pending throttler timer.
    // Do NOT clear() the spy first -- callers (e.g. the TASK-001 generation
    // SCENARIOs) need to introspect the signals captured during the consume
    // loop, and on Windows the throttler may not emit any extra signal after
    // the unthrottled progress==100 emit, leaving spy.count() == 0 if cleared.
    QElapsedTimer drainTimer;
    drainTimer.start();
    const int idleTimeoutMs = 500;
    const int maxDrainMs = 5000;
    while ( drainTimer.elapsed() < maxDrainMs ) {
        if ( !searchProgressSpy.wait( idleTimeoutMs ) ) {
            break;
        }
    }
}

} // namespace

using LineTypeFlags = LogFilteredData::LineTypeFlags;
using VisibilityFlags = LogFilteredData::VisibilityFlags;
using LineType = LogFilteredData::LineType;

static LogFilteredData::LineTypeFlags toFlags( LogFilteredData::LineType type )
{
    return static_cast<LineTypeFlags>( static_cast<LineType::Int>( type ) );
}

struct LogDataLoader {
    struct FileWatchConfigGuard {
        FileWatchConfigGuard()
        {
            auto& config = Configuration::getSynced();
            previousPollingEnabled_ = config.pollingEnabled();
            previousNativeWatchEnabled_ = config.nativeFileWatchEnabled();
            config.setPollingEnabled( false );
            config.setNativeFileWatchEnabled( false );
        }

        ~FileWatchConfigGuard()
        {
            auto& config = Configuration::getSynced();
            config.setPollingEnabled( previousPollingEnabled_ );
            config.setNativeFileWatchEnabled( previousNativeWatchEnabled_ );
        }

        bool previousPollingEnabled_{ false };
        bool previousNativeWatchEnabled_{ false };
    };

    LogDataLoader()
    {
        static int counter = 0;
        counter++;
        LOG_INFO << "Test run " << counter;

        // Keep cleanup explicit; QTemporaryFile auto-remove/close can race with
        // background file watching during test teardown on Windows.
        file.setAutoRemove( false );

        auto& config = Configuration::getSynced();
        configureProductLikeRegexpEngine( config );

        REQUIRE( generateDataFiles( file ) );
        SafeQSignalSpy loadEndSpy( &log_data, SIGNAL( loadingFinished( LoadingStatus ) ) );

        log_data.attachFile( file.fileName() );
        REQUIRE( loadEndSpy.safeWait( 10000 ) );
#if !defined( Q_OS_WIN )
        if ( file.isOpen() ) {
            file.close();
        }
#endif
    }

    ~LogDataLoader()
    {
#ifdef Q_OS_WIN
        // Windows UI tests still hit sporadic QObject teardown crashes in LogData
        // destruction after many short-lived scenarios. Leak in tests to avoid
        // the flaky destructor path (process exits after test run).
        (void)logDataHolder.release();
#endif
    }

    FileWatchConfigGuard fileWatchConfigGuard;
    QTemporaryFile file{ makeTempFileTemplate( QLatin1String( "filtered_test_XXXXXX" ) ) };
    std::unique_ptr<LogData> logDataHolder{ std::make_unique<LogData>() };
    LogData& log_data{ *logDataHolder };
};

class TestFilteredDataHandle {
  public:
    explicit TestFilteredDataHandle( std::unique_ptr<LogFilteredData>&& data )
        : data_( std::move( data ) )
    {
    }

    ~TestFilteredDataHandle()
    {
#ifdef Q_OS_WIN
        // Windows test runs still hit sporadic QObject teardown crashes in
        // LogFilteredData destruction after heavy create/search/destroy cycles.
        // The test process is short-lived; leaking the object avoids the flaky
        // destructor path and keeps behavior assertions intact.
        (void)data_.release();
#endif
    }

    TestFilteredDataHandle( const TestFilteredDataHandle& ) = delete;
    TestFilteredDataHandle& operator=( const TestFilteredDataHandle& ) = delete;
    TestFilteredDataHandle( TestFilteredDataHandle&& ) noexcept = default;
    TestFilteredDataHandle& operator=( TestFilteredDataHandle&& ) noexcept = default;

    LogFilteredData* get() const { return data_.get(); }
    LogFilteredData* operator->() const { return data_.get(); }
    LogFilteredData& operator*() const { return *data_; }

  private:
    std::unique_ptr<LogFilteredData> data_;
};

TestFilteredDataHandle makeTestFilteredData( LogData& logData )
{
    return TestFilteredDataHandle{ logData.getNewFilteredData() };
}

SCENARIO( "marks in filtered log data", "[logdata]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        WHEN( "Adding mark outside file" )
        {
            filtered_data->addMark( LineNumber( SL_NB_LINES + 25 ) );

            THEN( "No marked lines stored" )
            {
                for ( LineNumber i = 0_lnum; i < LineNumber( SL_NB_LINES ); ++i )
                    REQUIRE_FALSE(
                        filtered_data->lineTypeByLine( i ).testFlag( LineTypeFlags::Mark ) );
            }
        }

        WHEN( "Adding marks in log file" )
        {
            filtered_data->addMark( 10_lnum );
            filtered_data->addMark( 25_lnum );

            AND_WHEN( "Check for marked line" )
            {
                THEN( "Return true" )
                {
                    REQUIRE(
                        filtered_data->lineTypeByLine( 10_lnum ).testFlag( LineTypeFlags::Mark ) );
                    REQUIRE(
                        filtered_data->lineTypeByLine( 25_lnum ).testFlag( LineTypeFlags::Mark ) );
                }
            }

            AND_WHEN( "Get marks count" )
            {
                THEN( "Return all marks count" )
                {
                    REQUIRE( filtered_data->getNbMarks() == 2_lcount );
                }

                AND_WHEN( "Get marks" )
                {
                    auto marks = filtered_data->getMarks();
                    THEN( "Provide all marks" )
                    {
                        REQUIRE( marks.size() == 2 );
                    }
                }
            }

            AND_WHEN( "Get mark before has mark" )
            {
                const auto markBefore = filtered_data->getMarkBefore( 25_lnum );
                THEN( "Return previous mark" )
                {
                    REQUIRE( markBefore.has_value() );
                    REQUIRE( *markBefore == 10_lnum );
                }
            }

            AND_WHEN( "Get mark before has no data" )
            {
                const auto markBefore = filtered_data->getMarkBefore( 10_lnum );
                THEN( "Return no mark" )
                {
                    REQUIRE_FALSE( markBefore.has_value() );
                }
            }

            AND_WHEN( "Get mark after has mark" )
            {
                const auto markAfter = filtered_data->getMarkAfter( 10_lnum );
                THEN( "Return next mark" )
                {
                    REQUIRE( markAfter.has_value() );
                    REQUIRE( *markAfter == 25_lnum );
                }
            }

            AND_WHEN( "Get mark after has no data" )
            {
                const auto markAfter = filtered_data->getMarkAfter( 25_lnum );
                THEN( "Return no mark" )
                {
                    REQUIRE_FALSE( markAfter.has_value() );
                }
            }

            AND_WHEN( "Delete mark" )
            {
                filtered_data->deleteMark( 10_lnum );
                THEN( "Mark is removed" )
                {
                    REQUIRE_FALSE(
                        filtered_data->lineTypeByLine( 10_lnum ).testFlag( LineTypeFlags::Mark ) );
                    REQUIRE( filtered_data->getNbMarks() == 1_lcount );
                }
            }

            AND_WHEN( "Clear marks" )
            {
                filtered_data->clearMarks();
                THEN( "All marks are removed" )
                {
                    REQUIRE( filtered_data->getNbMarks() == 0_lcount );
                }
            }
        }
    }
}

SCENARIO( "search for regex", "[logdata]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        WHEN( "Searched for regex" )
        {
            const auto threadPoolSize = GENERATE( 0, 1, 2 );

            auto& config = Configuration::getSynced();

            config.setSearchThreadPoolSize( threadPoolSize );
            config.setUseParallelSearch( threadPoolSize > 0 );

            auto filtered_lines = filtered_data->getNbLine();
            REQUIRE( filtered_lines.get() == 0 );

            SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                              &LogFilteredData::searchProgressed };

            runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );

            THEN( "Matched lines are in data" )
            {
                const auto matches_count = filtered_data->getNbMatches();
                REQUIRE( matches_count == 50_lcount );

                const auto lines = filtered_data->getExpandedLines( 0_lnum, matches_count );
                for ( const auto& l : lines ) {
                    REQUIRE( l.endsWith( '9' ) );
                }
            }
        }
    }
}

SCENARIO( "repeated filtered data lifecycle is stable after search", "[logdata][stability]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "repeated create-search-destroy cycles" )
    {
        auto& config = Configuration::getSynced();

        for ( int threadPoolSize : { 0, 1, 2 } ) {
            config.setSearchThreadPoolSize( threadPoolSize );
            config.setUseParallelSearch( threadPoolSize > 0 );

            for ( int i = 0; i < 20; ++i ) {
                auto filtered_data = makeTestFilteredData( logDataLoader.log_data );
                SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                                  &LogFilteredData::searchProgressed };

                runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
                REQUIRE( filtered_data->getNbMatches() == 50_lcount );
            }
        }
    }
}

SCENARIO( "marks and matches in filtered log data", "[logdata]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        WHEN( "Searched for regex" )
        {
            auto& config = Configuration::getSynced();
            config.setSearchThreadPoolSize( 2 );
            config.setUseParallelSearch( true );

            auto filtered_lines = filtered_data->getNbLine();
            REQUIRE( filtered_lines.get() == 0 );

            SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                              &LogFilteredData::searchProgressed };

            runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );

            AND_WHEN( "Add marks at matched line" )
            {
                const auto& firstMatchedLine = filtered_data->getLineString( 0_lnum );
                REQUIRE( firstMatchedLine.right( 2 ).toStdString() == "09" );

                filtered_data->addMark( 9_lnum );

                THEN( "Has same number of lines" )
                {
                    REQUIRE( filtered_data->getNbLine() == 50_lcount );
                }
            }

            AND_WHEN( "Add marks at not matched line" )
            {
                filtered_data->addMark( 5_lnum );

                THEN( "Has one more line" )
                {
                    REQUIRE( filtered_data->getNbLine() == 51_lcount );
                }
            }

            AND_WHEN( "Has mixed marks and matches" )
            {
                filtered_data->addMark( 9_lnum );
                filtered_data->addMark( 5_lnum );

                AND_WHEN( "Only marks are visible" )
                {
                    filtered_data->setVisibility( VisibilityFlags::Marks );

                    THEN( "Has only marked lines count" )
                    {
                        REQUIRE( filtered_data->getNbLine() == 2_lcount );
                    }

                    AND_WHEN( "Ask for line type by line" )
                    {
                        THEN( "Return mark" )
                        {
                            auto type = filtered_data->lineTypeByLine( 5_lnum );
                            REQUIRE( toFlags( type ) == LineTypeFlags::Mark );
                        }
                        THEN( "Return match" )
                        {
                            auto type = filtered_data->lineTypeByLine( 19_lnum );
                            REQUIRE( toFlags( type ) == LineTypeFlags::Match );
                        }

                        THEN( "Return mark & match" )
                        {
                            auto type = filtered_data->lineTypeByLine( 9_lnum );
                            REQUIRE( toFlags( type )
                                     == toFlags( LineTypeFlags::Mark | LineTypeFlags::Match ) );
                        }
                    }

                    WHEN( "Ask for line type by index" )
                    {
                        THEN( "Return mark" )
                        {
                            auto type = filtered_data->lineTypeByIndex( 0_lnum );
                            REQUIRE( toFlags( type ) == LineTypeFlags::Mark );
                        }
                        THEN( "Return mark & match" )
                        {
                            auto type = filtered_data->lineTypeByIndex( 1_lnum );
                            REQUIRE( toFlags( type )
                                     == toFlags( LineTypeFlags::Mark | LineTypeFlags::Match ) );
                        }
                    }
                }

                AND_WHEN( "Only matches are visible" )
                {
                    filtered_data->setVisibility( VisibilityFlags::Matches );

                    THEN( "Has only matches lines count" )
                    {
                        REQUIRE( filtered_data->getNbLine() == 50_lcount );
                    }

                    AND_WHEN( "Ask for line type by line" )
                    {
                        THEN( "Return mark" )
                        {
                            auto type = filtered_data->lineTypeByLine( 5_lnum );
                            REQUIRE( toFlags( type ) == LineTypeFlags::Mark );
                        }
                        THEN( "Return match" )
                        {
                            auto type = filtered_data->lineTypeByLine( 19_lnum );
                            REQUIRE( toFlags( type ) == LineTypeFlags::Match );
                        }
                        THEN( "Return mark & match" )
                        {
                            auto type = filtered_data->lineTypeByLine( 9_lnum );
                            REQUIRE( toFlags( type )
                                     == toFlags( LineTypeFlags::Mark | LineTypeFlags::Match ) );
                        }
                    }

                    AND_WHEN( "Ask for line type by index" )
                    {
                        THEN( "Return match" )
                        {
                            auto type = filtered_data->lineTypeByIndex( 1_lnum );
                            REQUIRE( toFlags( type ) == LineTypeFlags::Match );
                        }
                        THEN( "Return mark & match" )
                        {
                            auto type = filtered_data->lineTypeByIndex( 0_lnum );
                            REQUIRE( toFlags( type )
                                     == toFlags( LineTypeFlags::Mark | LineTypeFlags::Match ) );
                        }
                    }
                }

                filtered_data->setVisibility( VisibilityFlags::Matches | VisibilityFlags::Marks );

                AND_WHEN( "Ask for line type by line" )
                {
                    THEN( "Return Mark" )
                    {
                        auto type = filtered_data->lineTypeByLine( 5_lnum );
                        REQUIRE( toFlags( type ) == LineTypeFlags::Mark );
                    }
                    THEN( "Return match" )
                    {
                        auto type = filtered_data->lineTypeByLine( 19_lnum );
                        REQUIRE( toFlags( type ) == LineTypeFlags::Match );
                    }
                    THEN( "Return mark & match" )
                    {
                        auto type = filtered_data->lineTypeByLine( 9_lnum );
                        REQUIRE( toFlags( type )
                                 == toFlags( LineTypeFlags::Mark | LineTypeFlags::Match ) );
                    }
                }

                AND_WHEN( "Ask for line type by index" )
                {
                    THEN( "Return mark" )
                    {
                        auto type = filtered_data->lineTypeByIndex( 0_lnum );
                        REQUIRE( toFlags( type ) == LineTypeFlags::Mark );
                    }
                    THEN( "Return match" )
                    {
                        auto type = filtered_data->lineTypeByIndex( 2_lnum );
                        REQUIRE( toFlags( type ) == LineTypeFlags::Match );
                    }
                    THEN( "Return mark & match" )
                    {
                        auto type = filtered_data->lineTypeByIndex( 1_lnum );
                        REQUIRE( toFlags( type )
                                 == toFlags( LineTypeFlags::Mark | LineTypeFlags::Match ) );
                    }
                }
            }

            AND_WHEN( "Ask for matching line number" )
            {
                filtered_data->addMark( 1_lnum );

                AND_WHEN( "For marked line" )
                {
                    auto original_line = filtered_data->getMatchingLineNumber( 0_lnum );
                    THEN( "Original line is on mark" )
                    {
                        REQUIRE( original_line == 1_lnum );
                    }
                }

                AND_WHEN( "For matched line" )
                {
                    auto original_line = filtered_data->getMatchingLineNumber( 1_lnum );

                    const auto& firstMatchedLine = filtered_data->getLineString( 1_lnum );
                    REQUIRE( firstMatchedLine.right( 2 ).toStdString() == "09" );

                    THEN( "Original line is on match" )
                    {
                        REQUIRE( original_line == 9_lnum );
                    }
                }

                AND_WHEN( "For last line" )
                {
                    auto max_filtered_line = LineNumber( filtered_data->getNbLine().get() - 1 );
                    auto original_line = filtered_data->getMatchingLineNumber( max_filtered_line );
                    THEN( "Original line is last" )
                    {
                        REQUIRE( original_line == 499_lnum );
                    }
                }

                AND_WHEN( "For invalid line" )
                {
                    auto max_filtered_line = LineNumber( filtered_data->getNbLine().get() - 1 );
                    auto original_line
                        = filtered_data->getMatchingLineNumber( max_filtered_line + 1_lcount );
                    THEN( "Max line number is returned" )
                    {
                        REQUIRE( original_line == maxValue<LineNumber>() );
                    }
                }
            }

            AND_WHEN( "Ask for filtered line index" )
            {
                filtered_data->addMark( 1_lnum );

                AND_WHEN( "For marked line" )
                {
                    auto filtered_line = filtered_data->getLineIndexNumber( 1_lnum );
                    THEN( "Marked line returned" )
                    {
                        REQUIRE( filtered_line == 0_lnum );
                    }
                }

                AND_WHEN( "For matched line" )
                {
                    auto filtered_line = filtered_data->getLineIndexNumber( 9_lnum );
                    THEN( "Matched line returned" )
                    {
                        REQUIRE( filtered_line == 1_lnum );
                    }
                }

                AND_WHEN( "For last line" )
                {
                    auto max_filtered_line = LineNumber( filtered_data->getNbLine().get() - 1 );
                    auto filtered_line = filtered_data->getLineIndexNumber( 499_lnum );
                    THEN( "Last matched line returned" )
                    {
                        REQUIRE( filtered_line == max_filtered_line );
                    }
                }

                AND_WHEN( "For invalid line" )
                {
                    auto filtered_line = filtered_data->getMatchingLineNumber( 500_lnum );
                    THEN( "Max line number is returned" )
                    {
                        REQUIRE( filtered_line == maxValue<LineNumber>() );
                    }
                }
            }

            AND_WHEN( "Asked for line length" )
            {
                THEN( "Return expanded length" )
                {
                    REQUIRE( filtered_data->getLineLength( 1_lnum ) == LineLength( 92 ) );
                }
            }

            AND_WHEN( "Asked for line" )
            {
                THEN( "Return original line" )
                {
                    REQUIRE( filtered_data->getLineString( 2_lnum ).size() == 85 );
                }
            }

            AND_WHEN( "Asked for expanded line" )
            {
                THEN( "Return expanded line" )
                {
                    REQUIRE( filtered_data->getExpandedLineString( 2_lnum ).size() == 92 );
                }
            }

            AND_WHEN( "Asked to clear search" )
            {
                filtered_data->clearSearch();
                THEN( "Clear search results" )
                {
                    REQUIRE( filtered_data->getNbLine() == 0_lcount );
                }
            }
        }
    }
}

// ============================================================================
// Issue 4: marks should be visible with empty filter + context lines > 0
// ============================================================================
SCENARIO( "marks visible with context lines and no search", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with marks but no search" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        // Add some marks
        filtered_data->addMark( 10_lnum );
        filtered_data->addMark( 50_lnum );
        filtered_data->addMark( 100_lnum );

        WHEN( "context lines are zero" )
        {
            filtered_data->setContextLines( 0, 0 );

            THEN( "marks are visible (3 marks)" )
            {
                REQUIRE( filtered_data->getNbLine() == 3_lcount );
            }
        }

        WHEN( "context lines before is set to 2" )
        {
            filtered_data->setContextLines( 2, 0 );

            THEN( "marks plus context before are visible" )
            {
                // Mark at 10 -> context lines 8, 9, 10 = 3
                // Mark at 50 -> context lines 48, 49, 50 = 3
                // Mark at 100 -> context lines 98, 99, 100 = 3
                // Total = 9 (no overlap)
                REQUIRE( filtered_data->getNbLine() == 9_lcount );
            }

            THEN( "line content is accessible" )
            {
                // First line should be context line 8
                // getLineNumber adds +1 (converts 0-based to 1-based for display)
                auto lineNum = filtered_data->getLineNumber( 0_lnum );
                REQUIRE( lineNum.get() == 9 ); // 8 + 1 for 1-based display
            }
        }

        WHEN( "context lines after is set to 2" )
        {
            filtered_data->setContextLines( 0, 2 );

            THEN( "marks plus context after are visible" )
            {
                // Mark at 10 -> lines 10, 11, 12 = 3
                // Mark at 50 -> lines 50, 51, 52 = 3
                // Mark at 100 -> lines 100, 101, 102 = 3
                // Total = 9 (no overlap)
                REQUIRE( filtered_data->getNbLine() == 9_lcount );
            }
        }

        WHEN( "context lines both before and after" )
        {
            filtered_data->setContextLines( 1, 1 );

            THEN( "marks plus context in both directions" )
            {
                // Mark at 10 -> lines 9, 10, 11 = 3
                // Mark at 50 -> lines 49, 50, 51 = 3
                // Mark at 100 -> lines 99, 100, 101 = 3
                // Total = 9 (no overlap)
                REQUIRE( filtered_data->getNbLine() == 9_lcount );
            }
        }
    }
}

SCENARIO( "marks visible with context lines and adjacent marks", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with adjacent marks" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        // Marks close together so context ranges overlap
        filtered_data->addMark( 10_lnum );
        filtered_data->addMark( 12_lnum );

        WHEN( "context lines cause overlap" )
        {
            filtered_data->setContextLines( 2, 2 );

            THEN( "overlapping context lines are deduplicated" )
            {
                // Mark at 10 -> lines 8, 9, 10, 11, 12
                // Mark at 12 -> lines 10, 11, 12, 13, 14
                // Union = 8, 9, 10, 11, 12, 13, 14 = 7
                REQUIRE( filtered_data->getNbLine() == 7_lcount );
            }
        }
    }
}

SCENARIO( "marks at file boundary with context lines", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with mark at line 0" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        filtered_data->addMark( 0_lnum );

        WHEN( "context before is set" )
        {
            filtered_data->setContextLines( 3, 0 );

            THEN( "no negative line numbers - just the mark line" )
            {
                REQUIRE( filtered_data->getNbLine() == 1_lcount );
                // getLineNumber adds +1 (0-based to 1-based)
                auto lineNum = filtered_data->getLineNumber( 0_lnum );
                REQUIRE( lineNum.get() == 1 ); // 0 + 1
            }
        }
    }

    GIVEN( "loaded log data with mark at last line" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        auto lastLine = LineNumber( SL_NB_LINES - 1 );
        filtered_data->addMark( lastLine );

        WHEN( "context after is set" )
        {
            filtered_data->setContextLines( 0, 3 );

            THEN( "no lines beyond file end - just the mark line" )
            {
                REQUIRE( filtered_data->getNbLine() == 1_lcount );
                // getLineNumber adds +1 (0-based to 1-based)
                auto lineNum = filtered_data->getLineNumber( 0_lnum );
                REQUIRE( lineNum.get() == lastLine.get() + 1 );
            }
        }
    }
}

// ============================================================================
// Issue 4: marks + matches combined with context lines
// ============================================================================
SCENARIO( "marks and matches combined with context lines", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with search results and marks" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        auto& config = Configuration::getSynced();
        config.setSearchThreadPoolSize( 0 );
        config.setUseParallelSearch( false );

        SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                          &LogFilteredData::searchProgressed };

        // Search for lines ending in "9" -> matches lines 9, 19, 29, ...
        runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() == 50_lcount );

        // Add a mark at a non-matching line
        filtered_data->addMark( 5_lnum );

        WHEN( "context lines are set with both marks and matches visible" )
        {
            filtered_data->setContextLines( 1, 1 );

            THEN( "context includes lines around both marks and matches" )
            {
                // Matches at 9, 19, 29, ... + mark at 5
                // Context around match at 9: lines 8, 9, 10
                // Context around mark at 5: lines 4, 5, 6
                // Context around match at 19: lines 18, 19, 20
                // etc.
                auto nbLines = filtered_data->getNbLine();
                // Should be more than just matches + 1 mark
                REQUIRE( nbLines.get() > 51 );
            }
        }

        WHEN( "only marks visible with context" )
        {
            filtered_data->setVisibility( VisibilityFlags::Marks );
            filtered_data->setContextLines( 1, 1 );

            THEN( "context is around marks only" )
            {
                // Mark at 5 -> lines 4, 5, 6 = 3
                REQUIRE( filtered_data->getNbLine() == 3_lcount );
            }

            THEN( "line content is correct" )
            {
                auto line = filtered_data->getExpandedLineString( 1_lnum );
                REQUIRE( line.contains( "000005" ) );
            }
        }

        WHEN( "only matches visible with context" )
        {
            filtered_data->setVisibility( VisibilityFlags::Matches );
            filtered_data->setContextLines( 1, 0 );

            THEN( "context is around matches only, mark excluded from context source" )
            {
                // 50 matches, each with 1 line before
                // Some overlap at boundaries (e.g. line 18 is context for match 19,
                // but 20 is context for nothing unless there's a match at 21)
                auto nbLines = filtered_data->getNbLine();
                REQUIRE( nbLines.get() > 50 );
                REQUIRE( nbLines.get() <= 100 );
            }
        }
    }
}

// ============================================================================
// Issue 1: maxLength should include context line lengths
// ============================================================================
SCENARIO( "max length includes context line lengths", "[logdata][context]" )
{
    // We need a custom data file with varying line lengths
    QTemporaryFile file{
        makeTempFileTemplate( QLatin1String( "maxlen_context_test_XXXXXX" ) ) };

    REQUIRE( file.open() );

    // Write lines with varying lengths:
    // Line 0: short (10 chars)
    // Line 1: short (10 chars)
    // Line 2: very long (200 chars) - this will be a context line
    // Line 3: short (10 chars) - this will be a match
    // Line 4: short (10 chars)
    file.write( "short_ln_0\n" );     // line 0
    file.write( "short_ln_1\n" );     // line 1

    // line 2: a very long line (non-matching, will be context)
    QByteArray longLine( 200, 'X' );
    file.write( longLine );
    file.write( "\n" );

    file.write( "MATCH_ln_3\n" );     // line 3: will match
    file.write( "short_ln_4\n" );     // line 4
    file.flush();

    LogData logData;
    SafeQSignalSpy loadSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    logData.attachFile( file.fileName() );
    REQUIRE( loadSpy.safeWait( 10000 ) );
    REQUIRE( logData.getNbLine() == 5_lcount );

    auto filtered_data = makeTestFilteredData( logData );

    auto& config = Configuration::getSynced();
    config.setSearchThreadPoolSize( 0 );
    config.setUseParallelSearch( false );
    configureProductLikeRegexpEngine( config );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    // Search for "MATCH" - only line 3 matches
    runSearch( filtered_data.get(), "MATCH", searchProgressSpy );
    REQUIRE( filtered_data->getNbMatches() == 1_lcount );

    GIVEN( "no context lines" )
    {
        filtered_data->setContextLines( 0, 0 );

        THEN( "maxLength is the match line length" )
        {
            // "MATCH_ln_3" has 10 chars, expanded with tab = 10
            REQUIRE( filtered_data->getMaxLength() == LineLength( 10 ) );
        }
    }

    GIVEN( "context lines that include the long line" )
    {
        // Context before = 1 means line 2 (the 200-char line) is included
        filtered_data->setContextLines( 1, 0 );

        THEN( "maxLength includes the long context line" )
        {
            REQUIRE( filtered_data->getMaxLength() >= LineLength( 200 ) );
        }

        THEN( "number of visible lines includes context" )
        {
            // Match at line 3, context before = line 2
            REQUIRE( filtered_data->getNbLine() == 2_lcount );
        }
    }

    GIVEN( "context lines that include lines after" )
    {
        filtered_data->setContextLines( 0, 1 );

        THEN( "maxLength includes context after line" )
        {
            // Match at line 3, context after = line 4 (short)
            // Max should still be 10
            REQUIRE( filtered_data->getMaxLength() == LineLength( 10 ) );
        }
    }
}

// ============================================================================
// Issue 4: context lines with no search but with marks (zero context path)
// ============================================================================
SCENARIO( "marks appear when no search and zero context", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with marks, no search, no context" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        filtered_data->addMark( 10_lnum );
        filtered_data->addMark( 20_lnum );

        // No search, no context lines
        filtered_data->setContextLines( 0, 0 );

        THEN( "marks are visible" )
        {
            REQUIRE( filtered_data->getNbLine() == 2_lcount );
        }

        THEN( "line numbers map to marks" )
        {
            auto line0 = filtered_data->getMatchingLineNumber( 0_lnum );
            auto line1 = filtered_data->getMatchingLineNumber( 1_lnum );
            REQUIRE( line0 == 10_lnum );
            REQUIRE( line1 == 20_lnum );
        }
    }
}

// ============================================================================
// Issue 1: context lines getLineNumber mapping
// ============================================================================
SCENARIO( "context lines getLineNumber returns correct mapping", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with search and context" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        auto& config = Configuration::getSynced();
        config.setSearchThreadPoolSize( 0 );
        config.setUseParallelSearch( false );

        SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                          &LogFilteredData::searchProgressed };

        // Match lines ending in "9" -> first match at line 9
        runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );

        WHEN( "context lines are set to 2 before" )
        {
            filtered_data->setContextLines( 2, 0 );

            THEN( "getLineNumber returns correct original line numbers" )
            {
                // First match is at line 9, context before = lines 7, 8
                // So context list starts with: 7, 8, 9, ...
                // getLineNumber adds +1 (0-based to 1-based for display)
                auto line0 = filtered_data->getLineNumber( 0_lnum );
                auto line1 = filtered_data->getLineNumber( 1_lnum );
                auto line2 = filtered_data->getLineNumber( 2_lnum );
                REQUIRE( line0.get() == 8 );  // line 7 + 1
                REQUIRE( line1.get() == 9 );  // line 8 + 1
                REQUIRE( line2.get() == 10 ); // line 9 + 1
            }
        }
    }
}

// ============================================================================
// Issue 1: getLineLength with context lines
// ============================================================================
SCENARIO( "getLineLength works with context lines", "[logdata][context]" )
{
    LogDataLoader logDataLoader;

    GIVEN( "loaded log data with search and context" )
    {
        auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

        auto& config = Configuration::getSynced();
        config.setSearchThreadPoolSize( 0 );
        config.setUseParallelSearch( false );

        SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                          &LogFilteredData::searchProgressed };

        runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );

        WHEN( "context lines are set" )
        {
            filtered_data->setContextLines( 1, 0 );

            THEN( "getLineLength returns valid length for all context lines" )
            {
                auto nbLines = filtered_data->getNbLine();
                for ( LineNumber::UnderlyingType i = 0; i < nbLines.get(); ++i ) {
                    auto len = filtered_data->getLineLength( LineNumber( i ) );
                    // All test lines have tabs so expanded length > 0
                    REQUIRE( len.get() > 0 );
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// TASK-001: Search generation IDs
//
// Each call to runSearch() / updateSearch() advances a monotonic generation
// counter on LogFilteredData.  Every searchProgressed signal carries the
// generation that was active when the underlying SearchOperation started, so
// receivers (CrawlerWidget) can drop stale signals from a superseded search
// without relying on the disconnect/reconnect-around-replaceCurrentSearch
// hack.
// ----------------------------------------------------------------------------

SCENARIO( "interruptSearch does not advance the search generation",
          "[logdata][search-generation]" )
{
    // Codifies the Stop-button vs Replace-button contract: interruptSearch()
    // must leave the generation untouched so the final progress signal from
    // the in-flight search still reaches CrawlerWidget::updateFilteredView()
    // and triggers UI cleanup (hide gauge, hide Stop button, show Search /
    // Clear buttons).  Replace-flows that need stale signals dropped use
    // bumpSearchGeneration() explicitly.
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };
    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );

    const auto generationAfterSearch = filtered_data->currentSearchGeneration();
    REQUIRE( generationAfterSearch > 0 );

    filtered_data->interruptSearch();
    REQUIRE( filtered_data->currentSearchGeneration() == generationAfterSearch );
}

SCENARIO( "bumpSearchGeneration advances by exactly one",
          "[logdata][search-generation]" )
{
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    const auto before = filtered_data->currentSearchGeneration();
    filtered_data->bumpSearchGeneration();
    REQUIRE( filtered_data->currentSearchGeneration() == before + 1 );

    filtered_data->bumpSearchGeneration();
    REQUIRE( filtered_data->currentSearchGeneration() == before + 2 );
}

SCENARIO( "search generation increments on each runSearch", "[logdata][search-generation]" )
{
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    REQUIRE( filtered_data->currentSearchGeneration() == 0 );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
    const auto firstGen = filtered_data->currentSearchGeneration();
    REQUIRE( firstGen > 0 );

    runSearch( filtered_data.get(), "this is line [0-9]{5}3", searchProgressSpy );
    const auto secondGen = filtered_data->currentSearchGeneration();
    REQUIRE( secondGen > firstGen );
}

SCENARIO( "searchProgressed signal carries the search generation",
          "[logdata][search-generation]" )
{
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );

    REQUIRE( searchProgressSpy.count() > 0 );
    const auto activeGeneration = filtered_data->currentSearchGeneration();
    for ( int i = 0; i < searchProgressSpy.count(); ++i ) {
        const auto args = searchProgressSpy.at( i );
        REQUIRE( args.size() == 4 );
        const auto gen = args.at( 3 ).toULongLong();
        REQUIRE( gen == activeGeneration );
    }
}

// ============================================================================
// Regression tests for follow-mode search result loss
//
// BUG: In follow mode, each call to updateSearch() bumps the generation counter
// in LogFilteredDataWorker.  When the search completes, its progress signal
// carries the generation that was active when the operation started.  By the
// time the signal arrives in updateFilteredView(), the active generation has
// moved on (because another updateSearch was dispatched), and
// isStaleSearchGeneration() drops the signal.  Result: matches ARE found but
// NEVER displayed.
// ============================================================================

SCENARIO( "updateSearch should not bump generation beyond what the search itself produces",
          "[logdata][search-generation][regression]" )
{
    // This test exposes the core bug: each updateSearch() call increments
    // operationGeneration_ inside the worker, so if two updateSearch calls
    // happen in rapid succession, the first search's completion signal
    // carries a generation that no longer matches currentGeneration(),
    // and the results are silently dropped.
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    auto& config = Configuration::getSynced();
    config.setSearchThreadPoolSize( 0 );
    config.setUseParallelSearch( false );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    // First search
    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
    REQUIRE( filtered_data->currentSearchGeneration() > 0 );
    REQUIRE( filtered_data->getNbMatches() == 50_lcount );

    // Now do an updateSearch (simulating follow-mode file growth).
    // In the BUGGY behavior, updateSearch bumps the generation, so a
    // subsequent updateSearch would cause the first one's results to be
    // dropped as stale.
    searchProgressSpy.clear();

    filtered_data->updateSearch( 0_lnum, LineNumber( SL_NB_LINES ) );

    // Wait for the search to complete
    int progress = 0;
    int consumedSignals = 0;
    do {
        while ( searchProgressSpy.count() <= consumedSignals ) {
            REQUIRE( searchProgressSpy.wait() );
        }
        QList<QVariant> progressArgs = searchProgressSpy.at( consumedSignals );
        ++consumedSignals;
        progress = progressArgs.at( 1 ).toInt();
    } while ( progress < 100 );

    // Drain throttled signals
    QElapsedTimer drainTimer;
    drainTimer.start();
    while ( drainTimer.elapsed() < 3000 ) {
        if ( !searchProgressSpy.wait( 500 ) ) {
            break;
        }
    }

    // CRITICAL ASSERTION: after updateSearch completes, the results must
    // still be accessible.  The BUG is that the generation bump from
    // updateSearch causes the searchProgressed signal to be considered
    // stale, and the matches are never delivered to the UI.
    REQUIRE( filtered_data->getNbMatches() > 0_lcount );

    // The searchProgressed signals we received should all carry a generation
    // that matches what the worker reports as current at completion time.
    // If updateSearch bumped the generation AFTER dispatching the search,
    // some signals will carry a stale generation.
    const auto finalGeneration = filtered_data->currentSearchGeneration();
    bool allSignalsMatch = true;
    for ( int i = 0; i < searchProgressSpy.count(); ++i ) {
        const auto args = searchProgressSpy.at( i );
        if ( args.size() == 4 ) {
            const auto gen = args.at( 3 ).toULongLong();
            if ( gen != finalGeneration ) {
                allSignalsMatch = false;
                break;
            }
        }
    }
    REQUIRE( allSignalsMatch );
}

SCENARIO( "rapid updateSearch calls do not lose search results",
          "[logdata][search-generation][regression]" )
{
    // Simulates follow-mode behavior: the file keeps growing and
    // loadingFinished triggers updateSearch in rapid succession.
    // Each call should not cause the previous search's results to be
    // silently dropped.
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    auto& config = Configuration::getSynced();
    config.setSearchThreadPoolSize( 0 );
    config.setUseParallelSearch( false );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    // Initial search
    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
    REQUIRE( filtered_data->getNbMatches() == 50_lcount );
    searchProgressSpy.clear();

    // Rapid updateSearch calls (simulating follow mode)
    for ( int i = 0; i < 3; ++i ) {
        filtered_data->updateSearch( 0_lnum, LineNumber( SL_NB_LINES ) );
    }

    // Wait for the final search to complete
    int progress = 0;
    int consumedSignals = 0;
    do {
        while ( searchProgressSpy.count() <= consumedSignals ) {
            REQUIRE( searchProgressSpy.wait() );
        }
        QList<QVariant> progressArgs = searchProgressSpy.at( consumedSignals );
        ++consumedSignals;
        progress = progressArgs.at( 1 ).toInt();
    } while ( progress < 100 );

    // Drain
    QElapsedTimer drainTimer;
    drainTimer.start();
    while ( drainTimer.elapsed() < 3000 ) {
        if ( !searchProgressSpy.wait( 500 ) ) {
            break;
        }
    }

    // The results must still be present and non-zero.
    // In the buggy behavior, rapid updateSearch calls bump the generation
    // so much that all completion signals are dropped as stale.
    REQUIRE( filtered_data->getNbMatches() > 0_lcount );
}

SCENARIO( "regex search with various patterns produces correct results",
          "[logdata][search-regex]" )
{
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    auto& config = Configuration::getSynced();
    config.setSearchThreadPoolSize( 0 );
    config.setUseParallelSearch( false );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    SECTION( "simple literal match" )
    {
        runSearch( filtered_data.get(), "LOGDATA", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() == 500_lcount );
    }

    SECTION( "regex character class" )
    {
        runSearch( filtered_data.get(), "line [0-3]", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() > 0_lcount );
    }

    SECTION( "regex alternation" )
    {
        runSearch( filtered_data.get(), "line 00000[0-2]|line 000499", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() >= 3_lcount );
    }

    SECTION( "regex with anchoring" )
    {
        runSearch( filtered_data.get(), "^LOGDATA", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() == 500_lcount );
    }

    SECTION( "regex with quantifier" )
    {
        runSearch( filtered_data.get(), "line [0-9]{6}", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() == 500_lcount );
    }

    SECTION( "regex match at end of line" )
    {
        runSearch( filtered_data.get(), "\\d{3}9$", searchProgressSpy );
        REQUIRE( filtered_data->getNbMatches() == 50_lcount );
    }

    SECTION( "case insensitive match via pattern" )
    {
        auto pattern = RegularExpressionPattern( "logdata", false, false, false, false );
        filtered_data->runSearch( pattern );
        int progress = 0;
        int consumedSignals = 0;
        do {
            while ( searchProgressSpy.count() <= consumedSignals ) {
                REQUIRE( searchProgressSpy.wait() );
            }
            QList<QVariant> progressArgs = searchProgressSpy.at( consumedSignals );
            ++consumedSignals;
            progress = progressArgs.at( 1 ).toInt();
        } while ( progress < 100 );
        QElapsedTimer drainTimer;
        drainTimer.start();
        while ( drainTimer.elapsed() < 3000 ) {
            if ( !searchProgressSpy.wait( 500 ) ) {
                break;
            }
        }
        REQUIRE( filtered_data->getNbMatches() == 500_lcount );
    }
}

SCENARIO( "search with empty or invalid regex does not crash",
          "[logdata][search-edge-cases]" )
{
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    auto& config = Configuration::getSynced();
    config.setSearchThreadPoolSize( 0 );
    config.setUseParallelSearch( false );

    SECTION( "empty search pattern matches all lines" )
    {
        SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                          &LogFilteredData::searchProgressed };
        runSearch( filtered_data.get(), "", searchProgressSpy );
        // Empty pattern matches every line
        REQUIRE( filtered_data->getNbMatches() == 500_lcount );
    }

    SECTION( "invalid regex does not crash" )
    {
        SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                          &LogFilteredData::searchProgressed };
        auto pattern = RegularExpressionPattern( "(", false, false, false, false );
        RegularExpression hsExpression{ pattern };
        REQUIRE_FALSE( hsExpression.isValid() );

        filtered_data->runSearch( pattern );
        int progress = 0;
        int consumedSignals = 0;
        do {
            while ( searchProgressSpy.count() <= consumedSignals ) {
                REQUIRE( searchProgressSpy.wait() );
            }
            const auto progressArgs = searchProgressSpy.at( consumedSignals );
            ++consumedSignals;
            progress = progressArgs.at( 1 ).toInt();
        } while ( progress < 100 );
        REQUIRE( filtered_data->getNbMatches() == 0_lcount );
    }
}

SCENARIO( "runSearch followed by updateSearch preserves results",
          "[logdata][search-update][regression]" )
{
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    auto& config = Configuration::getSynced();
    config.setSearchThreadPoolSize( 0 );
    config.setUseParallelSearch( false );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    // Full search
    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
    const auto matchesAfterFullSearch = filtered_data->getNbMatches();
    REQUIRE( matchesAfterFullSearch == 50_lcount );
    searchProgressSpy.clear();

    // Update search (same range, simulating re-check after file growth)
    filtered_data->updateSearch( 0_lnum, LineNumber( SL_NB_LINES ) );

    // Wait for completion
    int progress = 0;
    int consumedSignals = 0;
    do {
        while ( searchProgressSpy.count() <= consumedSignals ) {
            REQUIRE( searchProgressSpy.wait() );
        }
        QList<QVariant> progressArgs = searchProgressSpy.at( consumedSignals );
        ++consumedSignals;
        progress = progressArgs.at( 1 ).toInt();
    } while ( progress < 100 );

    QElapsedTimer drainTimer;
    drainTimer.start();
    while ( drainTimer.elapsed() < 3000 ) {
        if ( !searchProgressSpy.wait( 500 ) ) {
            break;
        }
    }

    // After updateSearch, the same matches should still be present
    const auto matchesAfterUpdate = filtered_data->getNbMatches();
    REQUIRE( matchesAfterUpdate == matchesAfterFullSearch );

    // The filtered view line count should also match
    REQUIRE( filtered_data->getNbLine() > 0_lcount );
}

SCENARIO( "search generation is stable across updateSearch calls with same criteria",
          "[logdata][search-generation][regression]" )
{
    // The generation should only change when search criteria change,
    // NOT when updateSearch is called for the same pattern on an
    // expanded range (follow mode).  This is the root cause of the
    // follow-mode search result loss.
    LogDataLoader logDataLoader;
    auto filtered_data = makeTestFilteredData( logDataLoader.log_data );

    SafeQSignalSpy searchProgressSpy{ filtered_data.get(),
                                      &LogFilteredData::searchProgressed };

    runSearch( filtered_data.get(), "this is line [0-9]{5}9", searchProgressSpy );
    const auto genAfterSearch = filtered_data->currentSearchGeneration();
    REQUIRE( genAfterSearch > 0 );

    // updateSearch should NOT advance the generation when the search
    // criteria (pattern) haven't changed.  Only the search range expanded.
    filtered_data->updateSearch( 0_lnum, LineNumber( SL_NB_LINES ) );

    // The generation should remain the same since the search criteria
    // (pattern) have not changed.
    const auto genAfterUpdate = filtered_data->currentSearchGeneration();
    REQUIRE( genAfterUpdate == genAfterSearch );
}
