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

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <QApplication>
#include <QDir>
#include <QMetaType>
#include <QThreadPool>
#include <QtConcurrent>

#include <cstdio>

#if defined( Q_OS_UNIX )
#include <sys/resource.h>
#endif

#include <capturestore.h>
#include <configuration.h>
#include <linetypes.h>
#include <highlighterset.h>
#include <persistentinfo.h>

#include <logger.h>
#include <test_utils.h>

const bool PersistentInfo::ForcePortable = true;

namespace {
void configureTestTempDir()
{
    // Use the executable directory instead of the process working directory so
    // direct runs and CTest runs use the same temp-file location.
    const auto tempDir = QDir::cleanPath( QCoreApplication::applicationDirPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) );

    // Keep UI tests deterministic in local reruns: stale files from previous runs
    // can accumulate native watcher resources and hit low per-process fd limits.
    QDir tempDirectory{ tempDir };
    if ( tempDirectory.exists() ) {
        tempDirectory.removeRecursively();
    }

    QDir{}.mkpath( tempDir );

    const auto tempDirUtf8 = QDir::toNativeSeparators( tempDir ).toUtf8();
    qputenv( "TMP", tempDirUtf8 );
    qputenv( "TEMP", tempDirUtf8 );
    qputenv( "TMPDIR", tempDirUtf8 );
}

void configureTestFdLimit()
{
#if defined( Q_OS_UNIX )
    constexpr rlim_t DesiredFdLimit = 1024;

    rlimit fdLimit{};
    if ( getrlimit( RLIMIT_NOFILE, &fdLimit ) == 0 ) {
        const rlim_t targetLimit
            = ( fdLimit.rlim_max < DesiredFdLimit ) ? fdLimit.rlim_max : DesiredFdLimit;
        if ( targetLimit > fdLimit.rlim_cur ) {
            fdLimit.rlim_cur = targetLimit;
            (void)setrlimit( RLIMIT_NOFILE, &fdLimit );
        }
    }
#endif
}
} // namespace

namespace {

// Catch2 v2 listener that drains the global QThreadPool after every test
// case. The integration tests run serially in one process, so a forgotten
// QtConcurrent task that outlives its test would otherwise keep running
// (and touching dead temp files / destroyed models) while the NEXT test
// executes — a classic source of flaky cross-test failures on slower
// Windows/x86 CI legs. Joining between test cases converts such leaks into
// a deterministic, attributable stall at the end of the offending test.
//
// Why this cannot deadlock: the wait runs on the main thread, and no global
// -pool task in this codebase blocks on the main thread — there is no
// Qt::BlockingQueuedConnection anywhere in src/, QuickFind search futures are
// interrupted and joined by widget destructors (stopSearchAndWait, which runs
// before this listener fires), and decompressor/device-list futures only
// deliver their finished signal through the queued event loop. A task that
// never finishes on its own would hang the wait forever, so the wait is
// bounded: on timeout the test case is named and the run continues.
//
// The wait is cheap when idle (waitForDone returns immediately with an empty
// pool), so per-test-case granularity costs nothing in the common case.
//
// CaptureStore is deliberately NOT drained here: its cleanup runs on its own
// std::thread set whose shutdown flag has no re-arm path, so it can only be
// stopped once, at process exit (see shutdownBackgroundWorkers below).
class ThreadDrainListener : public Catch::TestEventListenerBase {
  public:
    using Catch::TestEventListenerBase::TestEventListenerBase;

    void testCaseEnded( Catch::TestCaseStats const& testCaseStats ) override
    {
        // Generous bound: normal pool tasks (search on small temp files)
        // finish in milliseconds; this only trips for genuinely leaked work.
        constexpr int DrainTimeoutMs = 30000;

        if ( !QThreadPool::globalInstance()->waitForDone( DrainTimeoutMs ) ) {
            fprintf( stderr,
                     "ThreadDrainListener: global QThreadPool still busy %d ms after "
                     "test case \"%s\" -- leaked QtConcurrent task?\n",
                     DrainTimeoutMs,
                     testCaseStats.testInfo.name.c_str() );
        }
    }
};

CATCH_REGISTER_LISTENER( ThreadDrainListener )

} // namespace

class TestRunner : public QObject {
    Q_OBJECT

  public:
    TestRunner( int argc, char** argv )
        : argc_( argc )
        , argv_( argv )
    {
    }

    int result()
    {
        return result_;
    }

  public Q_SLOTS:
    void process()
    {
        result_ = Catch::Session().run( argc_, argv_ );
        Q_EMIT finished( result_ );
    }

  Q_SIGNALS:
    void finished( int );

  private:
    int argc_;
    char** argv_;

    int result_;
};

#include "qtests_main.moc"

int main( int argc, char* argv[] )
{
    QApplication a( argc, argv );

    logging::enableLogging( true, logging::LogLevel::Warning );
    configureTestTempDir();
    configureTestFdLimit();

    qRegisterMetaType<LinesCount>( "LinesCount" );
    qRegisterMetaType<LineNumber>( "LineNumber" );
    qRegisterMetaType<LineLength>( "LineLength" );

    auto& config = Configuration::getSynced();
    config.setSearchReadBufferSizeLines( 10 );
    config.setIndexReadBufferSizeMb( 1 );
    config.setUseSearchResultsCache( false );
    config.setConfirmTabClose( false );
    configureProductLikeRegexpEngine( config );
    config.save();

    auto higthlighters = HighlighterSetCollection::getSynced();

#if defined( Q_OS_WIN ) || defined( Q_OS_MAC )
    config.setPollingEnabled( true );
    config.setPollIntervalMs( 1000 );
#else
    config.setPollingEnabled( false );
#endif

    // Native file watching (efsw) is flaky in Windows CI/local test runs and can
    // emit corrupted paths during rapid temp-file teardown. Keep polling enabled
    // for file-change coverage, but disable native watcher for deterministic tests.
#ifdef Q_OS_WIN
    config.setNativeFileWatchEnabled( false );
#else
    config.setNativeFileWatchEnabled( true );
#endif

    QThreadPool::globalInstance()->reserveThread();

    TestRunner runner( argc, argv );

    runner.process();
    CaptureStore::shutdownBackgroundWorkers();
    return runner.result();
}
