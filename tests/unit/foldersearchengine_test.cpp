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

#include "test_utils.h"

#include "configuration.h"
#include "encodingdetector.h"
#include "foldersearchengine.h"
#include "foldersearchtypes.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace {

bool waitUntil( const std::function<bool()>& predicate, int timeoutMilliseconds = 3000 )
{
    QElapsedTimer timer;
    timer.start();
    // Single evaluation per iteration: a volatile predicate can flip
    // true -> false between the loop condition and a trailing re-read.
    while ( timer.elapsed() < timeoutMilliseconds ) {
        if ( predicate() ) {
            return true;
        }
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        QThread::msleep( 1 );  // lint-allow: test-timing -- poll pacing under the waitUntil deadline
    }
    QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
    return predicate();
}

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

// Forces the folder fast path's config preconditions (block scan on + vectorscan
// engine) for the duration of a test and restores the host's saved settings on
// destruction -- including when a REQUIRE aborts the test mid-way (Catch2
// throws), where a manual restore at the end of the test body would be skipped
// and leak into subsequent tests.
class ScopedFastPathConfig {
  public:
    ScopedFastPathConfig()
        : config_( Configuration::get() )
        , savedBlockScan_( config_.useBlockScan() )
        , savedEngine_( config_.regexpEngine() )
    {
        config_.setUseBlockScan( true );
        config_.setRegexpEnging( RegexpEngine::Vectorscan );
    }

    ~ScopedFastPathConfig()
    {
        config_.setUseBlockScan( savedBlockScan_ );
        config_.setRegexpEnging( savedEngine_ );
    }

    ScopedFastPathConfig( const ScopedFastPathConfig& ) = delete;
    ScopedFastPathConfig& operator=( const ScopedFastPathConfig& ) = delete;
    ScopedFastPathConfig( ScopedFastPathConfig&& ) = delete;
    ScopedFastPathConfig& operator=( ScopedFastPathConfig&& ) = delete;

  private:
    Configuration& config_;
    bool savedBlockScan_;
    RegexpEngine savedEngine_;
};

// Encodes an ASCII QString into UTF-16 little-endian bytes, optionally with a
// BOM (FF FE). For ASCII text every code unit's high byte is 0x00, which is
// exactly what exercises the multi-byte newline finder.
QByteArray toUtf16LE( const QString& text, bool withBom )
{
    QByteArray out;
    if ( withBom ) {
        out.append( '\xff' );
        out.append( '\xfe' );
    }
    for ( const QChar& ch : text ) {
        const unsigned int u = ch.unicode();
        out.append( static_cast<char>( u & 0xFF ) );
        out.append( static_cast<char>( ( u >> 8 ) & 0xFF ) );
    }
    return out;
}

// Encodes an ASCII QString into UTF-16 big-endian bytes, optionally with a BOM
// (FE FF). For UTF-16BE the LF sequence is 0x00 0x0A, so the 0x0A byte sits at
// lineFeedIndex==1 -- this exercises the !isCheckForward branch of
// findNextMultiByteDelimeter.
QByteArray toUtf16BE( const QString& text, bool withBom )
{
    QByteArray out;
    if ( withBom ) {
        out.append( '\xfe' );
        out.append( '\xff' );
    }
    for ( const QChar& ch : text ) {
        const unsigned int u = ch.unicode();
        out.append( static_cast<char>( ( u >> 8 ) & 0xFF ) );
        out.append( static_cast<char>( u & 0xFF ) );
    }
    return out;
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

TEST_CASE( "scanFile fast path honors shouldStop before scanning", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A UTF-8 file with a real match, well under the 16 MiB fast-path cap and
    // with no context capture so the whole-file vectorscan fast path is taken.
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    // The fast path only runs when block scan is enabled AND the vectorscan
    // engine is selected. Both are defaults, but either can be flipped by the
    // host's saved QSettings (e.g. perf.useBlockScan=0). Force them on
    // (restored on scope exit) so this test is a deterministic RED/GREEN for
    // the fast-path cancellation gate regardless of the local configuration.
    const ScopedFastPathConfig scopedConfig;

    // Build the matcher AFTER setting the engine: hasBufferScan() (and the
    // cached vectorscan buffer scanner) is decided at construction time.
    auto matcher = matcherFor( "MATCH" );
    if ( !matcher->hasBufferScan() ) {
        // No buffer scanner in Qt-regex-only builds (KLOGG_USE_VECTORSCAN=OFF):
        // the fast path cannot run, so pass vacuously -- same contract as
        // patternmatcher_test's block-scan tests.
        return;
    }

    // Sanity: without a stop request the match IS found (proves the file matches
    // and the fast path runs, so the stopped case below is genuinely RED without
    // the fix rather than passing for the wrong reason).
    const auto foundGroup
        = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                        [] { return false; } );
    REQUIRE( foundGroup.matches.size() == 1 );

    // With stop already requested, the fast path must bail out BEFORE readAll +
    // scanBuffer and return an empty group for this file.
    const auto stoppedGroup
        = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                        [] { return true; } );
    REQUIRE( stoppedGroup.filePath == path );
    REQUIRE( stoppedGroup.matches.empty() );
}

TEST_CASE( "Folder matching never crosses physical record boundaries", "[folder][folder-normalization]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "foo\nbar\nfoo bar\n" ) );
    const ScopedFastPathConfig restore;
    for ( const auto backend : { RegexpEngine::Vectorscan, RegexpEngine::QRegularExpression } ) {
        for ( const bool blockScan : { false, true } ) {
            INFO( "backend=" << static_cast<int>( backend ) << " blockScan=" << blockScan );
            Configuration::get().setRegexpEnging( backend );
            Configuration::get().setUseBlockScan( blockScan );
            auto matcher = matcherFor( "foo\\s+bar" );
            const auto group = FolderSearchEngine::scanFile( path, *matcher );
            REQUIRE( group.filePath == path );
            REQUIRE( group.matches.size() == 1 );
            REQUIRE( group.matches.front().localLine == 2_lnum );
        }
    }
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

TEST_CASE( "FolderSearchEngine retains zero-match sources in batch results",
           "[folder][folder-snapshot][folder-batch-retention]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString aPath = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );
    const QString bPath = writeFile( dir, "b.log", QByteArray( "nothing here\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ aPath, bPath }, RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 2 );
    REQUIRE( results[ 0 ].filePath == aPath );
    REQUIRE( results[ 0 ].matches.size() == 1 );
    REQUIRE( results[ 1 ].filePath == bPath );
    REQUIRE( results[ 1 ].matches.empty() );
    REQUIRE( results[ 1 ].content );
    REQUIRE( results[ 1 ].content->readLine( 0_lnum, "UTF-8" ) == "nothing here" );
    REQUIRE( engine.matchCount() == 1 );
    REQUIRE( engine.takeResults().empty() );
    REQUIRE_FALSE( engine.takeCompletedGroup( 0 ) );
    REQUIRE_FALSE( engine.takeCompletedGroup( 1 ) );
}

TEST_CASE( "FolderSearchEngine emits searchStarted and searchFinished", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    std::vector<quint64> startedGenerations;
    std::vector<quint64> finishedGenerations;
    QObject::connect( &engine, &FolderSearchEngine::searchStarted, &engine,
                      [ &startedGenerations ]( quint64 generation ) {
                          startedGenerations.push_back( generation );
                      } );
    QObject::connect( &engine, &FolderSearchEngine::searchFinished, &engine,
                      [ &finishedGenerations ]( quint64 generation ) {
                          finishedGenerations.push_back( generation );
                      } );
    const auto gen = engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern( "MATCH" ) );

    REQUIRE( startedGenerations == std::vector<quint64>{ gen } );
    REQUIRE( finishedGenerations == std::vector<quint64>{ gen } );
}

TEST_CASE( "FolderSearchEngine enumerates folder requests off the caller thread",
           "[folder][enumeration][async]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    const auto callerThread = std::this_thread::get_id();
    std::atomic<bool> ranOffCallerThread{ false };
    std::atomic<bool> folderMatched{ false };
    std::atomic<bool> initiallyRunning{ false };
    std::atomic<int> enumerationCalls{ 0 };
    FolderSearchEngine engine(
        [ & ]( const QString& folderPath, const FolderSearchEngine::StopPredicate& shouldStop ) {
            ++enumerationCalls;
            ranOffCallerThread = std::this_thread::get_id() != callerThread;
            folderMatched = folderPath == dir.path();
            initiallyRunning = !shouldStop();
            return QStringList{ path };
        } );

    QStringList events;
    QStringList snapshotPaths;
    quint64 snapshotGeneration = 0;
    int finishedCount = 0;
    QObject::connect( &engine, &FolderSearchEngine::folderSnapshotReady, &engine,
                      [ & ]( const QStringList& filePaths, quint64 generation ) {
                          events.append( QStringLiteral( "snapshot" ) );
                          snapshotPaths = filePaths;
                          snapshotGeneration = generation;
                      },
                      Qt::QueuedConnection );
    QObject::connect( &engine, &FolderSearchEngine::searchStarted, &engine,
                      [ &events ]( quint64 ) {
                          events.append( QStringLiteral( "started" ) );
                      },
                      Qt::QueuedConnection );
    QObject::connect( &engine, &FolderSearchEngine::searchFinished, &engine,
                      [ &finishedCount ]( quint64 ) { ++finishedCount; },
                      Qt::QueuedConnection );

    const auto generation
        = engine.startFolderSearch( dir.path(), RegularExpressionPattern( "MATCH" ) );
    REQUIRE( waitUntil( [ &finishedCount ] { return finishedCount > 0; } ) );

    REQUIRE( enumerationCalls.load() == 1 );
    CHECK( ranOffCallerThread.load() );
    CHECK( folderMatched.load() );
    CHECK( initiallyRunning.load() );
    CHECK( snapshotPaths == QStringList{ path } );
    CHECK( snapshotGeneration == generation );
    REQUIRE( events.size() >= 2 );
    CHECK( events.at( 0 ) == QStringLiteral( "snapshot" ) );
    CHECK( events.at( 1 ) == QStringLiteral( "started" ) );
}

TEST_CASE( "FolderSearchEngine cancels stale enumeration before publishing its snapshot",
           "[folder][enumeration][generation]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString latestPath = writeFile( dir, "latest.log", QByteArray( "MATCH\n" ) );

    QSemaphore firstEntered;
    std::atomic<int> enumerationCalls{ 0 };
    std::atomic<bool> firstCanceled{ false };
    FolderSearchEngine engine(
        [ & ]( const QString&, const FolderSearchEngine::StopPredicate& shouldStop ) {
            const int call = ++enumerationCalls;
            if ( call == 1 ) {
                firstEntered.release();
                while ( !shouldStop() ) {
                    std::this_thread::yield();
                }
                firstCanceled = true;
                return QStringList{};
            }
            return QStringList{ latestPath };
        } );

    QStringList snapshotPaths;
    quint64 snapshotGeneration = 0;
    int snapshotCount = 0;
    int finishedCount = 0;
    QObject::connect( &engine, &FolderSearchEngine::folderSnapshotReady, &engine,
                      [ & ]( const QStringList& filePaths, quint64 generation ) {
                          ++snapshotCount;
                          snapshotPaths = filePaths;
                          snapshotGeneration = generation;
                      },
                      Qt::QueuedConnection );
    QObject::connect( &engine, &FolderSearchEngine::searchFinished, &engine,
                      [ &finishedCount ]( quint64 ) { ++finishedCount; },
                      Qt::QueuedConnection );

    engine.startFolderSearch( dir.path(), RegularExpressionPattern( "FIRST" ) );
    REQUIRE( firstEntered.tryAcquire( 1, 3000 ) );
    const auto latestGeneration
        = engine.startFolderSearch( dir.path(), RegularExpressionPattern( "MATCH" ) );
    REQUIRE( waitUntil( [ & ] { return snapshotCount == 1 && finishedCount >= 2; } ) );

    CHECK( firstCanceled.load() );
    CHECK( enumerationCalls.load() == 2 );
    CHECK( snapshotCount == 1 );
    CHECK( snapshotPaths == QStringList{ latestPath } );
    CHECK( snapshotGeneration == latestGeneration );
}

TEST_CASE( "FolderSearchEngine supersedes an uncooperative blocked enumeration",
           "[folder][enumeration][generation][lifetime]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString latestPath = writeFile( dir, "latest.log", QByteArray( "MATCH\n" ) );

    struct BlockedEnumerationState {
        QSemaphore firstEntered;
        QSemaphore releaseFirst;
        std::atomic<int> enumerationCalls{ 0 };
        std::atomic<bool> firstReturned{ false };
    };
    const auto state = std::make_shared<BlockedEnumerationState>();
    FolderSearchEngine engine(
        [ state, latestPath ]( const QString&, const FolderSearchEngine::StopPredicate& ) {
            const int call = ++state->enumerationCalls;
            if ( call == 1 ) {
                state->firstEntered.release();
                state->releaseFirst.acquire();
                state->firstReturned = true;
                return QStringList{};
            }
            return QStringList{ latestPath };
        } );

    QStringList snapshotPaths;
    quint64 snapshotGeneration = 0;
    int snapshotCount = 0;
    int finishedCount = 0;
    QObject::connect( &engine, &FolderSearchEngine::folderSnapshotReady, &engine,
                      [ & ]( const QStringList& filePaths, quint64 generation ) {
                          ++snapshotCount;
                          snapshotPaths = filePaths;
                          snapshotGeneration = generation;
                      },
                      Qt::QueuedConnection );
    QObject::connect( &engine, &FolderSearchEngine::searchFinished, &engine,
                      [ &finishedCount ]( quint64 ) { ++finishedCount; },
                      Qt::QueuedConnection );

    engine.startFolderSearch( dir.path(), RegularExpressionPattern( "FIRST" ) );
    REQUIRE( state->firstEntered.tryAcquire( 1, 3000 ) );
    struct ReleaseBlockedEnumeration {
        std::shared_ptr<BlockedEnumerationState> state;
        bool active{ true };
        ~ReleaseBlockedEnumeration()
        {
            if ( active ) {
                state->releaseFirst.release();
            }
        }
    } releaseGuard{ state };

    const auto latestGeneration
        = engine.startFolderSearch( dir.path(), RegularExpressionPattern( "MATCH" ) );
    REQUIRE( waitUntil( [ & ] { return snapshotCount == 1 && finishedCount >= 2; } ) );

    CHECK_FALSE( state->firstReturned.load() );
    CHECK( state->enumerationCalls.load() == 2 );
    CHECK( snapshotPaths == QStringList{ latestPath } );
    CHECK( snapshotGeneration == latestGeneration );

    state->releaseFirst.release();
    releaseGuard.active = false;
    REQUIRE( waitUntil( [ state ] { return state->firstReturned.load(); } ) );
}

TEST_CASE( "FolderSearchEngine bounds quarantined blocked enumerations",
           "[folder][enumeration][generation][lifetime]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString latestPath = writeFile( dir, "latest.log", QByteArray( "MATCH\n" ) );

    struct BoundedEnumerationState {
        QSemaphore enteredFirst;
        QSemaphore enteredSecond;
        QSemaphore releaseFirst;
        QSemaphore releaseSecond;
        std::atomic<int> calls{ 0 };
    };
    const auto state = std::make_shared<BoundedEnumerationState>();
    struct ReleaseBlockedEnumerations {
        std::shared_ptr<BoundedEnumerationState> state;
        ~ReleaseBlockedEnumerations()
        {
            state->releaseFirst.release();
            state->releaseSecond.release();
        }
    } releaseGuard{ state };

    FolderSearchEngine engine(
        [ state, latestPath ]( const QString&, const FolderSearchEngine::StopPredicate& ) {
            const int call = ++state->calls;
            if ( call == 1 ) {
                state->enteredFirst.release();
                state->releaseFirst.acquire();
                return QStringList{};
            }
            if ( call == 2 ) {
                state->enteredSecond.release();
                state->releaseSecond.acquire();
                return QStringList{};
            }
            return QStringList{ latestPath };
        } );

    int snapshotCount = 0;
    QObject::connect( &engine, &FolderSearchEngine::folderSnapshotReady, &engine,
                      [ &snapshotCount ]( const QStringList&, quint64 ) {
                          ++snapshotCount;
                      },
                      Qt::QueuedConnection );

    engine.startFolderSearch( dir.path(), RegularExpressionPattern( "FIRST" ) );
    REQUIRE( state->enteredFirst.tryAcquire( 1, 3000 ) );
    engine.startFolderSearch( dir.path(), RegularExpressionPattern( "SECOND" ) );
    REQUIRE( state->enteredSecond.tryAcquire( 1, 3000 ) );
    engine.startFolderSearch( dir.path(), RegularExpressionPattern( "MATCH" ) );

    QThread::msleep( 200 );  // lint-allow: test-timing -- absence assertion: give a would-be third enumeration a grace window
    QCoreApplication::processEvents();
    CHECK( state->calls.load() == 2 );
    CHECK( snapshotCount == 0 );

    state->releaseFirst.release();
    REQUIRE( waitUntil( [ & ] { return snapshotCount == 1; } ) );
    CHECK( state->calls.load() == 3 );
    state->releaseSecond.release();
}

TEST_CASE( "FolderSearchEngine rejects invalid folder patterns before enumeration",
           "[folder][enumeration][validation]" )
{
    std::atomic<int> enumerationCalls{ 0 };
    FolderSearchEngine engine(
        [ & ]( const QString&, const FolderSearchEngine::StopPredicate& ) {
            ++enumerationCalls;
            return QStringList{};
        } );

    int snapshotCount = 0;
    int finishedCount = 0;
    QObject::connect( &engine, &FolderSearchEngine::folderSnapshotReady, &engine,
                      [ &snapshotCount ]( const QStringList&, quint64 ) {
                          ++snapshotCount;
                      },
                      Qt::QueuedConnection );
    QObject::connect( &engine, &FolderSearchEngine::searchFinished, &engine,
                      [ &finishedCount ]( quint64 ) { ++finishedCount; },
                      Qt::QueuedConnection );

    engine.startFolderSearch( QStringLiteral( "/unused" ),
                              RegularExpressionPattern( QStringLiteral( "[invalid" ) ) );
    REQUIRE( waitUntil( [ &finishedCount ] { return finishedCount > 0; } ) );

    CHECK( enumerationCalls.load() == 0 );
    CHECK( snapshotCount == 0 );
}

TEST_CASE( "FolderSearchEngine clears stale results when folder enumeration is unavailable",
           "[folder][enumeration][validation]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern( "MATCH" ) );
    REQUIRE( engine.matchCount() == 1 );
    REQUIRE( engine.takeResults().size() == 1 );

    int finishedCount = 0;
    QObject::connect( &engine, &FolderSearchEngine::searchFinished, &engine,
                      [ &finishedCount ]( quint64 ) { ++finishedCount; },
                      Qt::QueuedConnection );
    engine.startFolderSearch( dir.path(), RegularExpressionPattern( "MATCH" ) );
    REQUIRE( waitUntil( [ &finishedCount ] { return finishedCount > 0; } ) );

    CHECK( engine.matchCount() == 0 );
    CHECK( engine.takeResults().empty() );
}

TEST_CASE( "FolderSearchEngine destruction does not wait for blocked enumeration",
           "[folder][enumeration][lifetime]" )
{
    struct BlockingEnumeration {
        QSemaphore entered;
        QSemaphore release;
        std::atomic<bool> cancellationObserved{ false };
        std::atomic<bool> returned{ false };
    };
    const auto state = std::make_shared<BlockingEnumeration>();

    auto engine = std::make_unique<FolderSearchEngine>(
        [ state ]( const QString&, const FolderSearchEngine::StopPredicate& shouldStop ) {
            state->entered.release();
            state->release.acquire();
            state->cancellationObserved = shouldStop();
            state->returned = true;
            return QStringList{};
        } );
    engine->startFolderSearch( QStringLiteral( "/blocked" ),
                               RegularExpressionPattern( "MATCH" ) );
    REQUIRE( state->entered.tryAcquire( 1, 3000 ) );

    QElapsedTimer destructionTimer;
    destructionTimer.start();
    engine.reset();
    // Local-only latency budget on a cancellation path: CI covers the contract
    // through the cancellationObserved/returned handshake below, not through
    // the time this destruction took.
    KLOGG_CHECK_PERF_BUDGET( destructionTimer.elapsed() < 1000 );  // lint-allow: perf-budget -- Local destruction speed; CI checks cancellation.

    state->release.release();
    REQUIRE( waitUntil( [ &state ] { return state->returned.load(); } ) );
    CHECK( state->cancellationObserved.load() );
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

// --- multi-encoding support (read-only) ---

TEST_CASE( "scanFile matches UTF-16LE text with correct raw byte offsets", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // UTF-16LE with BOM (FF FE) so detection is deterministic. Each char is
    // 2 bytes, so the BOM shifts every offset by 2 vs. the no-BOM case.
    //   BOM(2) "foo\n"(8) -> line 0 = [0,10)
    //   "ERROR here\n"(22) -> line 1 = [10,32)
    //   "baz\n"(8) -> line 2 = [32,40)
    const QByteArray content = toUtf16LE( QStringLiteral( "foo\nERROR here\nbaz\n" ), true );
    const QString path = writeFile( dir, "utf16le.log", content );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.filePath == path );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.sourceCodec != nullptr );
    // Detection must have picked a UTF-16 codec (not fallen back to UTF-8).
    REQUIRE( group.sourceCodec->name().contains( "UTF-16" ) );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 10 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 32 ) ); // past the whole LF
}

TEST_CASE( "scanFile matches UTF-16BE text (lineFeedIndex==1 advance path)", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // UTF-16BE with BOM (FE FF). Same byte layout as the LE case (2 bytes/char
    // + 2-byte BOM), so offsets match -- but the LF sequence is 0x00 0x0A, so
    // lineFeedIndex==1 and the past-newline advance must use nl+1, not nl+2.
    const QByteArray content = toUtf16BE( QStringLiteral( "foo\nERROR here\nbaz\n" ), true );
    const QString path = writeFile( dir, "utf16be.log", content );

    auto matcher = matcherFor( "ERROR" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );

    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.sourceCodec != nullptr );
    REQUIRE( group.sourceCodec->name().contains( "UTF-16" ) );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 10 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 32 ) );
}

TEST_CASE( "scanFile does not skip a UTF-16LE file as binary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A UTF-16LE ASCII file is full of 0x00 high bytes; the binary NUL check
    // is gated on lineFeedWidth==1, so this must NOT be skipped as binary and
    // MUST still match.
    const QByteArray content = toUtf16LE( QStringLiteral( "all good\nMATCH line\n" ), true );
    const QString path = writeFile( dir, "utf16le_nul.log", content );

    auto matcher = matcherFor( "MATCH" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
}

TEST_CASE( "scanFile multi-byte line spanning a read-block boundary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // A UTF-16LE line longer than the tiny block size, with the LF sequence
    // split across the carried bytes -- proves carry keeps raw bytes and the
    // multi-byte finder resolves the LF in the joined view.
    const QByteArray content = toUtf16LE(
        QStringLiteral( "aaaaaa NEEDLE bbbbbbbbbbbb\n" ), true );
    const QString path = writeFile( dir, "long16.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 16 ); // tiny block size
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    // Line 0 includes the BOM in its raw byte range ([0, end)); the codec
    // strips the BOM on decode, so this is correct raw-file-space accounting.
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 0 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( content.size() ) );
}

TEST_CASE( "FolderSearchEngine mixes UTF-8 and UTF-16LE files with per-file codec", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString utf8Path = writeFile( dir, "a.log", QByteArray( "MATCH\n" ) );
    const QString utf16Path
        = writeFile( dir, "b.log", toUtf16LE( QStringLiteral( "MATCH\n" ), true ) );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ utf8Path, utf16Path },
                              RegularExpressionPattern( "MATCH" ) );
    const auto results = engine.takeResults();

    REQUIRE( results.size() == 2 );
    REQUIRE( results[ 0 ].filePath == utf8Path );
    REQUIRE( results[ 0 ].matches.size() == 1 );
    // UTF-8 file: sourceCodec may be the UTF-8 codec, but must not be a UTF-16.
    REQUIRE( results[ 1 ].filePath == utf16Path );
    REQUIRE( results[ 1 ].matches.size() == 1 );
    REQUIRE( results[ 1 ].sourceCodec != nullptr );
    REQUIRE( results[ 1 ].sourceCodec->name().contains( "UTF-16" ) );
    REQUIRE( engine.matchCount() == 2 );
}

TEST_CASE( "scanFile resolves a UTF-16LE line-feed split across a block boundary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "AA NEEDLE\nBB NEEDLE\n" in UTF-16LE: 20 bytes/line; the first LF (0x0A 0x00)
    // is at raw bytes 18-19. blockSize=19 leaves the 0x0A at the end of block 1
    // and the 0x00 at the start of block 2 -- a split LF that must still produce
    // TWO matched lines (regression: without seam handling it merges into one).
    const QByteArray content = toUtf16LE( QStringLiteral( "AA NEEDLE\nBB NEEDLE\n" ), true );
    const QString path = writeFile( dir, "le.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 19 );

    REQUIRE( group.matches.size() == 2 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
}

TEST_CASE( "scanFile resolves a UTF-16BE line-feed split across a block boundary", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // UTF-16BE LF is 0x00 0x0A; with blockSize=19 the 0x00 sits at the end of
    // block 1 and the 0x0A at the start of block 2. Must still be two lines.
    const QByteArray content = toUtf16BE( QStringLiteral( "AA NEEDLE\nBB NEEDLE\n" ), true );
    const QString path = writeFile( dir, "be.log", content );

    auto matcher = matcherFor( "NEEDLE" );
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 19 );

    REQUIRE( group.matches.size() == 2 );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
}

// ============================ grep -A/-B/-C context ============================

TEST_CASE( "scanFile -A emits N after-context rows tagged Context", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "HIT\nx1\nx2\nx3\nx4\n" ) );
    auto matcher = matcherFor( "HIT" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                                     {}, { 0, /*after=*/2 } );
    REQUIRE( group.matches.size() == 3 );
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 2 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 2 ].localLine == 2_lnum );
}

TEST_CASE( "scanFile -B emits N before-context rows with correct byte offsets", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // "p1\np2\nHIT\n" -> p1 at [0,3), p2 at [3,6), HIT at [6,10)
    const QString path = writeFile( dir, "a.log", QByteArray( "p1\np2\nHIT\n" ) );
    auto matcher = matcherFor( "HIT" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                                     {}, { 2 /*before*/, 0 } );
    REQUIRE( group.matches.size() == 3 );
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 0 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 3 ) );
    REQUIRE( group.matches[ 1 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 2 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 2 ].localLine == 2_lnum );
}

TEST_CASE( "scanFile -C merges overlapping windows without duplicate rows", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // lines: 0:m0, 1:a, 2:m1 -- m0 and m1 both match; -C1 -> before=1,after=1
    const QString path = writeFile( dir, "a.log", QByteArray( "m0\na\nm1\n" ) );
    auto matcher = matcherFor( "m" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                                     {}, { 1, 1 } );
    REQUIRE( group.matches.size() == 3 ); // {m0, a(context), m1} -- no duplicate
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 1 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 2 ].localLine == 2_lnum );
    REQUIRE( group.matches[ 2 ].role == klogg::folder::RecordRole::Match );
}

TEST_CASE( "scanFile -B at the first line emits no before-context (no underflow)", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "HIT\nx\n" ) );
    auto matcher = matcherFor( "HIT" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                                     {}, { 5 /*before*/, 0 } );
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
}

TEST_CASE( "scanFile -A past EOF stops cleanly (trailing line, no final newline)", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "HIT\nx1" ) ); // no trailing \n
    auto matcher = matcherFor( "HIT" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                                     {}, { 0, 3 } );
    REQUIRE( group.matches.size() == 2 ); // Match@0, Context@1; after undrained
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 1 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
}

TEST_CASE( "scanFile default context is byte-identical to legacy match-only", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "foo\nbar ERROR\nbaz\n" ) );
    auto matcher = matcherFor( "ERROR" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher ); // default context {}
    REQUIRE( group.matches.size() == 1 );
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 0 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 0 ].lineStartByte == OffsetInFile( 4 ) );
    REQUIRE( group.matches[ 0 ].lineEndByte == OffsetInFile( 14 ) );
    REQUIRE( group.matches[ 0 ].lineLength == 9_length );
}

TEST_CASE( "FolderSearchEngine matchCount counts only Match rows with context", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString path = writeFile( dir, "a.log", QByteArray( "HIT\nx1\nx2\n" ) );
    auto matcher = matcherFor( "HIT" );

    FolderSearchEngine engine;
    engine.scanSynchronously( QStringList{ path }, RegularExpressionPattern{ QStringLiteral( "HIT" ) },
                              { 0, 2 } );
    // 3 records (1 Match + 2 Context) but only 1 true match.
    REQUIRE( engine.matchCount() == 1 );
}

TEST_CASE( "scanFile context window larger than the file", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // 3-line file, match on line 1, before=100/after=100 -> {C0, M1, C2}
    const QString path = writeFile( dir, "a.log", QByteArray( "p\nHIT\nq\n" ) );
    auto matcher = matcherFor( "HIT" );

    const auto group = FolderSearchEngine::scanFile( path, *matcher, FolderSearchEngine::DefaultBlockSize,
                                                     {}, { 100, 100 } );
    REQUIRE( group.matches.size() == 3 );
    REQUIRE( group.matches[ 0 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 0 ].localLine == 0_lnum );
    REQUIRE( group.matches[ 1 ].role == klogg::folder::RecordRole::Match );
    REQUIRE( group.matches[ 1 ].localLine == 1_lnum );
    REQUIRE( group.matches[ 2 ].role == klogg::folder::RecordRole::Context );
    REQUIRE( group.matches[ 2 ].localLine == 2_lnum );
}

TEST_CASE( "binary files are skipped without invoking the encoding detector", "[folder]" )
{
    // Perf regression guard for many-small-binary trees (e.g. /Library/Logs
    // WiFi CoreCapture .gz/.bin): detecting the encoding of a binary file is
    // wasted uchardet work (~ms per file). The BOM-less-NUL binary check must
    // run BEFORE detection, not after. BOM-bearing UTF-16/32 still detects.
    ScopedFastPathConfig fastPath;
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );

    QByteArray binaryBytes;
    for ( int i = 0; i < 4096; ++i ) {
        binaryBytes.append( static_cast<char>( i & 0xFF ) ); // includes NULs, no BOM prefix
    }
    QStringList paths;
    for ( int f = 0; f < 4; ++f ) {
        paths << writeFile( dir, QString( "bin_%1.bin" ).arg( f ), binaryBytes );
    }
    // One plain-text file with a match, proving the scan itself still works.
    paths << writeFile( dir, "text.log", QByteArray( "nothing here\nneedle here\n" ) );

    auto& uchardetCalls = EncodingDetector::uchardetInvocationsForTesting();
    const auto before = uchardetCalls.load();

    FolderSearchEngine engine;
    engine.scanSynchronously( paths, RegularExpressionPattern{ "needle" } );
    const auto groups = engine.takeResults();

    quint64 matches = 0;
    for ( const auto& g : groups ) {
        matches += static_cast<quint64>( g.matches.size() );
    }
    REQUIRE( matches == 1 );
    // Binaries: skipped pre-detection; the ASCII text file: UTF-8 fast path.
    // Neither may reach uchardet.
    REQUIRE( uchardetCalls.load() == before );
}

TEST_CASE( "Folder scan arena bounds payload and boundary stores across many members",
           "[folder][folder-snapshot][folder-arena]" )
{
    QTemporaryDir files;
    QTemporaryDir spills;
    REQUIRE( files.isValid() );
    REQUIRE( spills.isValid() );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 32;
    options.boundaryMemoryBudget = 32;
    options.temporaryDirectory = spills.path();
    auto arena = std::make_shared<klogg::folder::ScanArena>( options );
    auto matcher = matcherFor( "ERROR" );
    std::vector<klogg::folder::ScannedFileContent::Ptr> retained;
    uint64_t incarnation = 0;
    for ( int index = 0; index < 40; ++index ) {
        const QByteArray bytes = "before\nERROR " + QByteArray::number( index ) + "\nafter\n";
        const auto path = writeFile( files, QString::number( index ) + ".log", bytes );
        auto group = FolderSearchEngine::scanFile( path, *matcher, 7, {}, { 1, 1 }, arena );
        REQUIRE( group.content );
        REQUIRE( group.content->readBytes( 0, bytes.size() ) == bytes );
        REQUIRE( group.content->residentBytes()
                 <= options.byteMemoryBudget + options.boundaryMemoryBudget );
        const auto id = group.content->identity( 1_lnum );
        if ( index == 0 ) {
            incarnation = id.incarnation;
        }
        REQUIRE( id.incarnation == incarnation );
        REQUIRE( id.member == static_cast<uint64_t>( index + 1 ) );
        retained.push_back( group.content );
        REQUIRE( QFile::remove( path ) );
    }
    REQUIRE( retained.front()->spillFileNamesForTesting().size() == 2 );
    REQUIRE( QDir( spills.path() ).entryList( QDir::Files ).size() == 2 );
    arena.reset();
    REQUIRE( retained.front()->readLine( 1_lnum, "UTF-8" ) == "ERROR 0" );
    retained.erase( retained.begin(), retained.end() - 1 );
    REQUIRE( QDir( spills.path() ).entryList( QDir::Files ).size() == 2 );
    retained.clear();
    REQUIRE( QDir( spills.path() ).entryList( QDir::Files ).isEmpty() );
}

TEST_CASE( "Folder scan retains fast and multibyte fallback bytes exactly once",
           "[folder][folder-snapshot][folder-arena]" )
{
    ScopedFastPathConfig config;
    QTemporaryDir dir;
    auto matcher = matcherFor( "ERROR" );
    for ( const bool utf16 : { false, true } ) {
        const QByteArray bytes = utf16 ? toUtf16LE( "first\nERROR last", true )
                                       : QByteArray( "first\nERROR last" );
        const auto path = writeFile( dir, "source.log", bytes );
        const auto group = FolderSearchEngine::scanFile( path, *matcher, 19 );
        REQUIRE( group.content );
        REQUIRE( group.content->byteSize() == bytes.size() );
        REQUIRE( group.content->readBytes( 0, bytes.size() ) == bytes );
        REQUIRE( QFile::remove( path ) );
        REQUIRE( group.content->readLine( 1_lnum, group.content->codecName() ) == "ERROR last" );
        REQUIRE_THROWS_AS( group.content->readBytes( 0, bytes.size() + 1 ), klogg::SnapshotError );
    }
}

TEST_CASE( "Folder snapshot retains consumed streaming bytes rather than reopening source",
           "[folder][folder-snapshot][folder-arena]" )
{
    QTemporaryDir dir;
    const QByteArray original( "ERROR original\nsecond original\n" );
    const auto path = writeFile( dir, "changing.log", original );
    auto matcher = matcherFor( "ERROR" );
    bool rewritten = false;
    const auto stop = [&]() {
        if ( !rewritten ) {
            writeFile( dir, "changing.log", QByteArray( "ERROR replaced\nsecond replaced\n" ) );
            rewritten = true;
        }
        return false;
    };
    // Context disables speculative readAll; the first stop callback runs after
    // matching a line in the raw block that must already have been retained.
    const auto group = FolderSearchEngine::scanFile( path, *matcher, 1024, stop, { 1, 1 } );
    REQUIRE( rewritten );
    REQUIRE( group.content );
    REQUIRE( group.content->readBytes( 0, original.size() ) == original );
}

TEST_CASE( "Folder scan arena preserves interleaved concurrent member spans",
           "[folder][folder-snapshot][folder-arena]" )
{
    QTemporaryDir spills;
    REQUIRE( spills.isValid() );
    IndexedContentBuilder::Options options;
    options.byteMemoryBudget = 16;
    options.boundaryMemoryBudget = 8;
    options.temporaryDirectory = spills.path();
    auto arena = std::make_shared<klogg::folder::ScanArena>( options );
    std::vector<klogg::folder::ScannedFileContent::Ptr> retained( 4 );
    std::vector<std::thread> writers;
    std::atomic<int> ready{ 0 };
    QSemaphore continueWriting;
    for ( size_t index = 0; index < retained.size(); ++index ) {
        writers.emplace_back( [&, index]() {
            const auto member = arena->nextMember();
            const QByteArray part = QByteArray( static_cast<int>( 32 + index ),
                                                static_cast<char>( 'A' + index ) ) + '\n';
            std::vector<klogg::folder::ScanSpan> spans;
            std::vector<klogg::folder::ScanSpan> rowSpans;
            arena->append( spans, 0, part );
            const qint64 firstEnd = part.size();
            arena->appendLineEnds( rowSpans, 0, &firstEnd, 1 );
            if ( ready.fetch_add( 1 ) + 1 == static_cast<int>( retained.size() ) ) {
                continueWriting.release( static_cast<int>( retained.size() ) );
            }
            continueWriting.acquire();
            arena->append( spans, part.size(), part );
            const qint64 lineEnd = 2 * static_cast<qint64>( part.size() );
            arena->appendLineEnds( rowSpans, 1, &lineEnd, 1 );
            retained[ index ] = arena->seal( std::move( spans ), lineEnd, member, "UTF-8",
                                             2_lcount, std::move( rowSpans ) );
        } );
    }
    for ( auto& writer : writers ) {
        writer.join();
    }
    for ( size_t index = 0; index < retained.size(); ++index ) {
        const QByteArray line( static_cast<int>( 32 + index ), static_cast<char>( 'A' + index ) );
        const QByteArray expected = line + '\n' + line + '\n';
        REQUIRE( retained[ index ]->readBytes( 0, expected.size() ) == expected );
        REQUIRE( retained[ index ]->readLine( 0_lnum, "UTF-8" ) == QString::fromLatin1( line ) );
        REQUIRE( retained[ index ]->readLine( 1_lnum, "UTF-8" ) == QString::fromLatin1( line ) );
    }
    REQUIRE( retained.front()->spillFileNamesForTesting().size() == 2 );
    REQUIRE( QDir( spills.path() ).entryList( QDir::Files ).size() == 2 );
}

TEST_CASE( "Folder streaming drain releases source leases without a second result owner",
           "[folder][folder-snapshot][folder-drain]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "drain.log", "ERROR retained\n" );
    FolderSearchEngine engine;
    engine.scanSynchronously( { path }, RegularExpressionPattern{ "ERROR" } );
    std::weak_ptr<const klogg::folder::ScannedFileContent> lease;
    {
        auto group = engine.takeCompletedGroup( 0 );
        REQUIRE( group );
        REQUIRE( group->content );
        lease = group->content;
    }
    REQUIRE( lease.expired() );
    REQUIRE_FALSE( engine.takeCompletedGroup( 0 ) );
    REQUIRE( engine.takeResults().empty() );
}

TEST_CASE( "Folder generation abandonment releases undrained completed results",
           "[folder][folder-snapshot][folder-abandonment]" )
{
    QTemporaryDir dir;
    const auto path = writeFile( dir, "abandoned.log", "ERROR retained\n" );
    FolderSearchEngine engine;
    engine.scanSynchronously( { path }, RegularExpressionPattern{ "ERROR" } );
    engine.bumpGeneration();
    REQUIRE_FALSE( engine.takeCompletedGroup( 0 ) );
    REQUIRE( engine.takeResults().empty() );
}

TEST_CASE( "Folder matching normalizes CRLF identically across backends and block scan",
           "[folder][folder-normalization]" )
{
    ScopedFastPathConfig restore;
    QTemporaryDir dir;
    const auto path = writeFile( dir, "crlf.log", "prefix\r\nERROR\r\nlast\r\n" );
    for ( const auto backend : { RegexpEngine::Vectorscan, RegexpEngine::QRegularExpression } ) {
        for ( const bool blockScan : { false, true } ) {
            INFO( "backend=" << static_cast<int>( backend ) << " blockScan=" << blockScan );
            Configuration::get().setRegexpEnging( backend );
            Configuration::get().setUseBlockScan( blockScan );
            auto matcher = matcherFor( "^ERROR$" );
            const auto group = FolderSearchEngine::scanFile( path, *matcher, 7 );
            REQUIRE( group.matches.size() == 1 );
            REQUIRE( group.matches.front().localLine == 1_lnum );
            REQUIRE( group.matches.front().lineStartByte == OffsetInFile( 8 ) );
            REQUIRE( group.matches.front().lineEndByte == OffsetInFile( 15 ) );
            REQUIRE( group.content->readBytes( 8, 7 ) == "ERROR\r\n" );
        }
    }
}

TEST_CASE( "Folder matching uses decoded replacement characters across backends and block scan",
           "[folder][folder-normalization]" )
{
    ScopedFastPathConfig restore;
    QTemporaryDir dir;
    // Both the fast detector sample and the streaming first block are valid
    // ASCII, freezing UTF8 before the malformed second record is encountered.
    const QByteArray prefix = QByteArray( FolderSearchEngine::BinaryDetectionWindow - 1, 'a' ) + '\n';
    const auto replacement = QString::fromUtf8( "\xef\xbf\xbd" );
    for ( const auto& malformed : { QByteArray( "BAD \xff TOKEN\n" ),
                                    QByteArray( "BAD \xc3\n" ), QByteArray( "BAD \xc3" ) } ) {
        const bool withToken = malformed.contains( "TOKEN" );
        const auto pattern = QStringLiteral( "^BAD " ) + replacement
            + ( withToken ? QStringLiteral( " TOKEN$" ) : QStringLiteral( "$" ) );
        const auto path = writeFile( dir, "invalid.log", prefix + malformed );
        for ( const auto backend : { RegexpEngine::Vectorscan, RegexpEngine::QRegularExpression } ) {
            for ( const bool blockScan : { false, true } ) {
                INFO( "bytes=" << malformed.toHex().toStdString() << " backend="
                      << static_cast<int>( backend ) << " blockScan=" << blockScan );
                Configuration::get().setRegexpEnging( backend );
                Configuration::get().setUseBlockScan( blockScan );
                auto matcher = matcherFor( pattern );
                const auto group = FolderSearchEngine::scanFile(
                    path, *matcher, FolderSearchEngine::BinaryDetectionWindow );
                REQUIRE( group.matches.size() == 1 );
                REQUIRE( group.matches.front().localLine == 1_lnum );
                REQUIRE( group.matches.front().lineStartByte == OffsetInFile( prefix.size() ) );
                REQUIRE( group.content->readBytes( prefix.size(), malformed.size() ) == malformed );
            }
        }
    }
}

TEST_CASE( "Folder matching shares per-record BOM and single framing CR normalization",
           "[folder][folder-normalization]" )
{
    ScopedFastPathConfig restore;
    QTemporaryDir dir;
    for ( const bool bom : { true, false } ) {
        const QByteArray record = bom ? QByteArray( "\xef\xbb\xbf" ) + "ERROR\n"
                                       : QByteArray( "ERROR\r\r\n" );
        const auto path = writeFile( dir, "framing.log", bom ? record + record : record );
        for ( const auto backend : { RegexpEngine::Vectorscan, RegexpEngine::QRegularExpression } ) {
            for ( const bool blockScan : { false, true } ) {
                INFO( "BOM=" << bom << " backend=" << static_cast<int>( backend )
                      << " blockScan=" << blockScan );
                Configuration::get().setRegexpEnging( backend );
                Configuration::get().setUseBlockScan( blockScan );
                auto matcher = matcherFor( bom ? "^ERROR$" : "^ERROR\\r$" );
                const auto group = FolderSearchEngine::scanFile( path, *matcher );
                REQUIRE( group.matches.size() == ( bom ? 2u : 1u ) );
                REQUIRE( group.matches.front().lineStartByte == 0_offset );
                REQUIRE( group.matches.front().lineEndByte == OffsetInFile( record.size() ) );
                REQUIRE( group.content->readBytes( 0, record.size() ) == record );
            }
        }
    }
}

TEST_CASE( "Folder retained row locator crosses scanner boundary batches",
           "[folder][folder-snapshot][folder-row-locator]" )
{
    ScopedFastPathConfig restore;
    QTemporaryDir dir;
    constexpr size_t batch = klogg::folder::ScanArena::BoundaryBatchLines;
    constexpr size_t totalRows = 2 * batch + 3;
    QByteArray bytes;
    for ( size_t row = 0; row < totalRows; ++row ) {
        bytes += QByteArray::number( static_cast<qulonglong>( row ) ) + '\n';
    }
    const auto path = writeFile( dir, "batches.log", bytes );
    auto matcher = matcherFor( "never-present" );
    for ( const bool blockScan : { false, true } ) {
        Configuration::get().setUseBlockScan( blockScan );
        const auto group = FolderSearchEngine::scanFile( path, *matcher, 257 );
        REQUIRE( group.content );
        REQUIRE( group.content->lineCount() == LinesCount( totalRows ) );
        for ( const auto row : { size_t{ 0 }, batch - 1, batch, 2 * batch, totalRows - 1 } ) {
            REQUIRE( group.content->readLine( LineNumber( row ), "UTF-8" )
                     == QString::number( static_cast<qulonglong>( row ) ) );
        }
    }
}
