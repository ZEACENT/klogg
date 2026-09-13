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
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QMetaType>
#include <QThreadPool>
#include <QtConcurrent>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined( Q_OS_UNIX )
#include <sys/resource.h>
#endif

#include <capturestore.h>
#include <configuration.h>
#include <filewatcher.h>
#include <highlighterset.h>
#include <linetypes.h>
#include <persistentinfo.h>

#include <logger.h>
#include <test_utils.h>

const bool PersistentInfo::ForcePortable = true;

namespace {
constexpr auto TeardownTimeoutEnvironment = "KLOGG_TEST_TEARDOWN_TIMEOUT_MS";

int configuredTeardownTimeoutMs( int defaultTimeoutMs )
{
    bool valid = false;
    const auto configuredTimeout
        = qEnvironmentVariableIntValue( TeardownTimeoutEnvironment, &valid );
    return valid && configuredTimeout > 0 ? configuredTimeout : defaultTimeoutMs;
}

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

    const auto describeLimit = []( rlim_t limit ) {
        return limit == RLIM_INFINITY ? std::string{ "infinity" }
                                      : std::to_string( static_cast<unsigned long long>( limit ) );
    };

    rlimit fdLimit{};
    if ( getrlimit( RLIMIT_NOFILE, &fdLimit ) != 0 ) {
        const auto error = errno;
        fprintf( stderr, "configureTestFdLimit: getrlimit(RLIMIT_NOFILE) failed: %s\n",
                 std::strerror( error ) );
        return;
    }

    const rlim_t targetLimit
        = ( fdLimit.rlim_max < DesiredFdLimit ) ? fdLimit.rlim_max : DesiredFdLimit;
    if ( targetLimit > fdLimit.rlim_cur ) {
        auto raisedLimit = fdLimit;
        raisedLimit.rlim_cur = targetLimit;
        if ( setrlimit( RLIMIT_NOFILE, &raisedLimit ) != 0 ) {
            const auto error = errno;
            fprintf( stderr, "configureTestFdLimit: setrlimit(RLIMIT_NOFILE, soft=%s) failed: %s\n",
                     describeLimit( targetLimit ).c_str(), std::strerror( error ) );
        }
    }

    rlimit effectiveLimit{};
    if ( getrlimit( RLIMIT_NOFILE, &effectiveLimit ) != 0 ) {
        const auto error = errno;
        fprintf( stderr, "configureTestFdLimit: effective getrlimit(RLIMIT_NOFILE) failed: %s\n",
                 std::strerror( error ) );
        return;
    }

    const auto softLimit = describeLimit( effectiveLimit.rlim_cur );
    const auto hardLimit = describeLimit( effectiveLimit.rlim_max );
    fprintf( stderr, "configureTestFdLimit: effective RLIMIT_NOFILE soft=%s hard=%s\n",
             softLimit.c_str(), hardLimit.c_str() );
#else
    fprintf( stderr, "configureTestFdLimit: RLIMIT_NOFILE is unavailable on this platform\n" );
#endif
}
} // namespace

namespace {

// Catch2 v2 listener that drives asynchronous teardown to a bounded fixed
// point after every test. Object destruction can post worker work, worker
// completion can post Qt events, and deferred deletion can enqueue FileWatcher
// removals. Two quiet passes prevent that chain from spilling into the next test.
//
// CaptureStore is deliberately NOT drained here: its cleanup runs on its own
// std::thread set whose shutdown flag has no re-arm path, so it can only be
// stopped once, at process exit (see shutdownBackgroundWorkers below).
class ThreadDrainListener : public Catch::TestEventListenerBase {
  public:
    using Catch::TestEventListenerBase::TestEventListenerBase;

    void testCaseEnded( Catch::TestCaseStats const& testCaseStats ) override
    {
        constexpr int DefaultDrainTimeoutMs = 30000;
        constexpr int EventDrainSliceMs = 50;
        constexpr int RequiredQuietPasses = 2;

        const auto drainTimeoutMs = configuredTeardownTimeoutMs( DefaultDrainTimeoutMs );
        auto* const threadPool = QThreadPool::globalInstance();

        QElapsedTimer elapsed;
        elapsed.start();

        int quietPasses = 0;

        const auto failDrain = [ & ]( const char* stage, int pass ) {
            fprintf( stderr,
                     "ThreadDrainListener: fatal stage=%s test=\"%s\" pass=%d "
                     "elapsed_ms=%lld timeout_ms=%d quiet_passes=%d active_qthreads=%d\n",
                     stage, testCaseStats.testInfo.name.c_str(), pass,
                     static_cast<long long>( elapsed.elapsed() ), drainTimeoutMs, quietPasses,
                     threadPool->activeThreadCount() );
            fflush( stderr );
            // Continuing would let work from this case access the next case's state.
            std::_Exit( EXIT_FAILURE );
        };

        const auto remainingTimeMs = [ & ]( const char* stage, int pass ) {
            const auto remaining = drainTimeoutMs - elapsed.elapsed();
            if ( remaining <= 0 ) {
                failDrain( stage, pass );
            }
            return static_cast<int>( remaining );
        };

        for ( int pass = 1;; ++pass ) {
            if ( !threadPool->waitForDone( remainingTimeMs( "global-qthreadpool", pass ) ) ) {
                failDrain( "global-qthreadpool", pass );
            }

            if ( auto* watcher = FileWatcher::existingInstanceForTest();
                 watcher != nullptr
                 && !watcher->waitForIdleForTest( remainingTimeMs( "filewatcher", pass ) ) ) {
                failDrain( "filewatcher", pass );
            }

            QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
            QCoreApplication::processEvents( QEventLoop::AllEvents, EventDrainSliceMs );

            auto* watcher = FileWatcher::existingInstanceForTest();
            const auto watcherNotificationsFlushed
                = watcher != nullptr && watcher->flushPendingNotificationsForTest();
            const auto threadPoolIdle = threadPool->waitForDone( 0 );
            const auto watcherIdle = watcher == nullptr || watcher->waitForIdleForTest( 0 );
            quietPasses = threadPoolIdle && watcherIdle && !watcherNotificationsFlushed
                              ? quietPasses + 1
                              : 0;
            if ( quietPasses == RequiredQuietPasses ) {
                return;
            }
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
