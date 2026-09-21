/*
 * Copyright (C) 2026
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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>
#include <QtConcurrent>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#if defined( Q_OS_UNIX )
#include <dirent.h>
#include <sys/resource.h>
#endif

#include "configuration.h"
#include "crawlerwidget.h"
#include "filewatcher.h"
#include "session.h"
#include "test_utils.h"

#if defined( Q_OS_UNIX )
namespace {

constexpr auto TeardownTimeoutEnvironment = "KLOGG_TEST_TEARDOWN_TIMEOUT_MS";
constexpr auto TeardownTimeoutChildEnvironment = "KLOGG_TEST_TEARDOWN_TIMEOUT_CHILD";
constexpr rlim_t LifecycleFdLimit = 64;
// Keep enough room for transient Qt/font/indexing descriptors without masking a
// per-lifecycle native watcher leak under the low process limit.
constexpr rlim_t LifecycleFdHeadroom = 32;
constexpr int InsufficientFdHeadroomExitCode = 77;
constexpr int LifecycleIterations = 48;
constexpr int WatcherIdleTimeoutMs = 5000;

bool countOpenFileDescriptors( std::size_t& openFileDescriptors, std::string& error )
{
    DIR* descriptorDirectory = opendir( "/proc/self/fd" );
    if ( descriptorDirectory == nullptr ) {
        descriptorDirectory = opendir( "/dev/fd" );
    }
    if ( descriptorDirectory == nullptr ) {
        error = std::strerror( errno );
        return false;
    }

    const auto enumerationDescriptor = dirfd( descriptorDirectory );
    if ( enumerationDescriptor < 0 ) {
        error = std::strerror( errno );
        (void)closedir( descriptorDirectory );
        return false;
    }

    openFileDescriptors = 0;
    errno = 0;
    while ( const auto* entry = readdir( descriptorDirectory ) ) {
        const std::string name{ entry->d_name };
        if ( name.empty() || !std::all_of( name.begin(), name.end(), []( unsigned char character ) {
                 return std::isdigit( character );
             } ) ) {
            continue;
        }

        char* end = nullptr;
        const auto descriptor = std::strtol( name.c_str(), &end, 10 );
        if ( end != name.c_str() && *end == '\0' && descriptor != enumerationDescriptor ) {
            ++openFileDescriptors;
        }
    }

    const auto enumerationError = errno;
    if ( closedir( descriptorDirectory ) != 0 && enumerationError == 0 ) {
        error = std::strerror( errno );
        return false;
    }
    if ( enumerationError != 0 ) {
        error = std::strerror( enumerationError );
        return false;
    }

    return true;
}

class ScopedNativeFileWatch {
public:
    ScopedNativeFileWatch()
        : configuration_{ Configuration::get() }
        , nativeEnabled_{ configuration_.nativeFileWatchEnabled() }
        , pollingEnabled_{ configuration_.pollingEnabled() }
    {
        configuration_.setNativeFileWatchEnabled( true );
        configuration_.setPollingEnabled( false );
    }

    ~ScopedNativeFileWatch()
    {
        configuration_.setNativeFileWatchEnabled( nativeEnabled_ );
        configuration_.setPollingEnabled( pollingEnabled_ );
        if ( auto* watcher = FileWatcher::existingInstanceForTest(); watcher != nullptr ) {
            watcher->updateConfiguration();
            (void)watcher->waitForIdleForTest( WatcherIdleTimeoutMs );
        }
    }

    ScopedNativeFileWatch( const ScopedNativeFileWatch& ) = delete;
    ScopedNativeFileWatch& operator=( const ScopedNativeFileWatch& ) = delete;

private:
    Configuration& configuration_;
    bool nativeEnabled_;
    bool pollingEnabled_;
};

class ScopedFileWatchRegistration {
public:
    ScopedFileWatchRegistration( FileWatcher& watcher, QString path )
        : watcher_{ watcher }
        , path_{ std::move( path ) }
    {
        watcher_.addFile( path_ );
    }

    ~ScopedFileWatchRegistration()
    {
        (void)remove();
    }

    ScopedFileWatchRegistration( const ScopedFileWatchRegistration& ) = delete;
    ScopedFileWatchRegistration& operator=( const ScopedFileWatchRegistration& ) = delete;

    bool remove()
    {
        if ( active_ ) {
            watcher_.removeFile( path_ );
            active_ = false;
        }
        return watcher_.waitForIdleForTest( WatcherIdleTimeoutMs );
    }

private:
    FileWatcher& watcher_;
    QString path_;
    bool active_ = true;
};

class ScopedNoFileLimit {
public:
    ScopedNoFileLimit( rlim_t requestedLimit, rlim_t requiredHeadroom )
        : requiredHeadroom_{ requiredHeadroom }
    {
        if ( getrlimit( RLIMIT_NOFILE, &original_ ) != 0 ) {
            error_ = std::strerror( errno );
            return;
        }

        std::size_t openFileDescriptors = 0;
        if ( !countOpenFileDescriptors( openFileDescriptors, error_ ) ) {
            return;
        }
        if ( openFileDescriptors > std::numeric_limits<rlim_t>::max() - requiredHeadroom ) {
            error_ = "open descriptor count cannot be represented as an RLIMIT_NOFILE value";
            return;
        }
        openFileDescriptors_ = static_cast<rlim_t>( openFileDescriptors );

        const auto safeLowLimit
            = std::max( requestedLimit, openFileDescriptors_ + requiredHeadroom_ );
        effectiveLimit_ = original_.rlim_cur;
        if ( original_.rlim_cur > safeLowLimit ) {
            auto limited = original_;
            limited.rlim_cur = safeLowLimit;
            if ( setrlimit( RLIMIT_NOFILE, &limited ) != 0 ) {
                error_ = std::strerror( errno );
                return;
            }
            effectiveLimit_ = safeLowLimit;
            lowered_ = true;
        }

        rlimit effective{};
        if ( getrlimit( RLIMIT_NOFILE, &effective ) != 0 ) {
            error_ = std::strerror( errno );
            (void)restore();
            return;
        }
        effectiveLimit_ = effective.rlim_cur;
        usable_ = true;
    }

    ~ScopedNoFileLimit()
    {
        if ( !restore() ) {
            fprintf( stderr, "ScopedNoFileLimit: failed to restore RLIMIT_NOFILE: %s\n",
                     std::strerror( errno ) );
        }
    }

    ScopedNoFileLimit( const ScopedNoFileLimit& ) = delete;
    ScopedNoFileLimit& operator=( const ScopedNoFileLimit& ) = delete;

    bool isUsable() const
    {
        return usable_;
    }

    bool wasLowered() const
    {
        return lowered_;
    }

    rlim_t openFileDescriptors() const
    {
        return openFileDescriptors_;
    }

    rlim_t effectiveLimit() const
    {
        return effectiveLimit_;
    }

    rlim_t availableHeadroom() const
    {
        return effectiveLimit_ > openFileDescriptors_ ? effectiveLimit_ - openFileDescriptors_ : 0;
    }

    const std::string& error() const
    {
        return error_;
    }

    bool restore()
    {
        if ( !lowered_ ) {
            return true;
        }
        if ( setrlimit( RLIMIT_NOFILE, &original_ ) != 0 ) {
            return false;
        }
        lowered_ = false;
        return true;
    }

private:
    rlimit original_{};
    rlim_t requiredHeadroom_ = 0;
    rlim_t openFileDescriptors_ = 0;
    rlim_t effectiveLimit_ = 0;
    std::string error_;
    bool lowered_ = false;
    bool usable_ = false;
};

} // namespace

TEST_CASE( "A teardown drain timeout fails the integration-test process",
           "[.filewatcher-teardown-timeout][ui][filewatcher][lifecycle]" )
{
    if ( qEnvironmentVariableIsSet( TeardownTimeoutChildEnvironment ) ) {
        QSemaphore workerStarted;
        const auto future = QtConcurrent::run( [ &workerStarted ] {
            workerStarted.release();
            QThread::msleep( 250 );  // lint-allow: test-timing -- child keeps the watcher drained past the injected 20ms timeout
        } );
        Q_UNUSED( future );

        REQUIRE( workerStarted.tryAcquire( 1, 1000 ) );
        return;
    }

    QProcess timeoutChild;
    auto childEnvironment = QProcessEnvironment::systemEnvironment();
#if defined( KLOGG_TSAN_BUILD )
    // This subprocess deliberately hard-exits with a live worker to verify the
    // timeout contract. Suppress only that intentional child-process leak report.
    childEnvironment.insert( QStringLiteral( "TSAN_OPTIONS" ),
                             QStringLiteral( "halt_on_error=1:report_thread_leaks=0" ) );
#endif
    childEnvironment.insert( TeardownTimeoutEnvironment, QStringLiteral( "20" ) );
    childEnvironment.insert( TeardownTimeoutChildEnvironment, QStringLiteral( "1" ) );
    timeoutChild.setProcessEnvironment( childEnvironment );
    timeoutChild.start( QCoreApplication::applicationFilePath(),
                        { QStringLiteral( "-platform" ), QStringLiteral( "offscreen" ),
                          QStringLiteral( "[.filewatcher-teardown-timeout]" ) } );

    REQUIRE( timeoutChild.waitForStarted( 5000 ) );
    REQUIRE( timeoutChild.waitForFinished( 30000 ) );

    const auto childStderr = timeoutChild.readAllStandardError();
    INFO( childStderr.constData() );
    REQUIRE( timeoutChild.exitStatus() == QProcess::NormalExit );
    REQUIRE( timeoutChild.exitCode() == EXIT_FAILURE );
    REQUIRE( childStderr.contains( "ThreadDrainListener: fatal stage=global-qthreadpool" ) );
    REQUIRE(
        childStderr.contains( "A teardown drain timeout fails the integration-test process" ) );
}

#if defined( Q_OS_MAC )
TEST_CASE( "Native FileWatcher teardown closes kqueue descriptors",
           "[.filewatcher-lifecycle][ui][filewatcher][lifecycle][resource-limit]" )
{
    ScopedNativeFileWatch nativeFileWatch;
    QTemporaryDir fixtureRoot;
    REQUIRE( fixtureRoot.isValid() );

    constexpr int DirectoryFileCount = 8;
    for ( int fileIndex = 0; fileIndex < DirectoryFileCount; ++fileIndex ) {
        QFile file{ QDir{ fixtureRoot.path() }.filePath(
            QStringLiteral( "fixture-%1.log" ).arg( fileIndex ) ) };
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( file.write( "native watcher descriptor regression\n" ) > 0 );
    }

    const auto sourcePath
        = QDir{ fixtureRoot.path() }.filePath( QStringLiteral( "fixture-0.log" ) );
    auto& watcher = FileWatcher::getFileWatcher();
    REQUIRE( watcher.waitForIdleForTest( WatcherIdleTimeoutMs ) );

    std::size_t baselineDescriptors = 0;
    std::string descriptorError;
    REQUIRE( countOpenFileDescriptors( baselineDescriptors, descriptorError ) );
    INFO( descriptorError );
    const auto baselineFileCount = watcher.watchedFileCountForTest();
    const auto baselineDirectoryCount = watcher.watchedDirectoryCountForTest();

    ScopedFileWatchRegistration registration{ watcher, sourcePath };
    REQUIRE( watcher.waitForIdleForTest( WatcherIdleTimeoutMs ) );
    REQUIRE( watcher.watchedFileCountForTest() == baselineFileCount + 1 );
    REQUIRE( watcher.watchedDirectoryCountForTest() == baselineDirectoryCount + 1 );

    std::size_t activeDescriptors = 0;
    REQUIRE( countOpenFileDescriptors( activeDescriptors, descriptorError ) );
    INFO( "File descriptors before native watch="
          << baselineDescriptors << ", while active=" << activeDescriptors );
    REQUIRE( activeDescriptors > baselineDescriptors );

    REQUIRE( registration.remove() );
    REQUIRE( watcher.watchedFileCountForTest() == baselineFileCount );
    REQUIRE( watcher.watchedDirectoryCountForTest() == baselineDirectoryCount );

    std::size_t finalDescriptors = 0;
    REQUIRE( countOpenFileDescriptors( finalDescriptors, descriptorError ) );
    INFO( "File descriptors before native watch="
          << baselineDescriptors << ", after removal=" << finalDescriptors );
    REQUIRE( finalDescriptors == baselineDescriptors );
}
#endif

TEST_CASE( "File-backed UI lifecycles release watcher registrations",
           "[.filewatcher-lifecycle][ui][filewatcher][lifecycle][resource-limit]" )
{
    QTemporaryDir fixtureRoot;
    REQUIRE( fixtureRoot.isValid() );

    auto& watcher = FileWatcher::getFileWatcher();
    REQUIRE( watcher.waitForIdleForTest( WatcherIdleTimeoutMs ) );
    const auto baselineFileCount = watcher.watchedFileCountForTest();
    const auto baselineDirectoryCount = watcher.watchedDirectoryCountForTest();

    ScopedNoFileLimit fdLimit{ LifecycleFdLimit, LifecycleFdHeadroom };
    INFO( "RLIMIT_NOFILE fixture: open descriptors="
          << fdLimit.openFileDescriptors() << ", effective soft limit=" << fdLimit.effectiveLimit()
          << ", available headroom=" << fdLimit.availableHeadroom()
          << ", lowered=" << fdLimit.wasLowered() << ", status=" << fdLimit.error() );
    REQUIRE( fdLimit.isUsable() );
    if ( fdLimit.availableHeadroom() < LifecycleFdHeadroom ) {
        fprintf( stderr,
                 "FileWatcher lifecycle fixture skipped: inherited RLIMIT_NOFILE provides %llu "
                 "descriptors of headroom, but %llu are required for deterministic pressure\n",
                 static_cast<unsigned long long>( fdLimit.availableHeadroom() ),
                 static_cast<unsigned long long>( LifecycleFdHeadroom ) );
        fflush( stderr );
        std::_Exit( InsufficientFdHeadroomExitCode );
    }

    for ( int iteration = 0; iteration < LifecycleIterations; ++iteration ) {
        INFO( "Lifecycle iteration " << iteration );

        const auto directoryName = QStringLiteral( "lifecycle-%1" ).arg( iteration );
        QDir root{ fixtureRoot.path() };
        REQUIRE( root.mkpath( directoryName ) );

        const auto sourcePath
            = root.filePath( directoryName + QDir::separator() + QStringLiteral( "source.log" ) );
        QFile source{ sourcePath };
        REQUIRE( source.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( source.write( "lifecycle regression\n" ) > 0 );
        source.close();

        {
            Session session;
            std::unique_ptr<CrawlerWidget> crawler{ static_cast<CrawlerWidget*>(
                session.open( sourcePath, [] { return new CrawlerWidget(); } ) ) };
            REQUIRE( crawler != nullptr );
            REQUIRE( waitUiState( [ &crawler ] { return crawler->isFirstLoadDone(); } ) );
        }

        // Bound every asynchronous add/remove pair before starting the next
        // lifecycle. If this barrier reports idle, any count growth is a leak,
        // not queue latency, and must stop the test immediately.
        REQUIRE( watcher.waitForIdleForTest( WatcherIdleTimeoutMs ) );

        const auto currentFileCount = watcher.watchedFileCountForTest();
        const auto currentDirectoryCount = watcher.watchedDirectoryCountForTest();
        INFO( "FileWatcher reached idle with files="
              << currentFileCount << ", directories=" << currentDirectoryCount
              << "; baseline files=" << baselineFileCount
              << ", directories=" << baselineDirectoryCount );
        REQUIRE( currentFileCount == baselineFileCount );
        REQUIRE( currentDirectoryCount == baselineDirectoryCount );

        QTemporaryFile sentinel{ root.filePath(
            QStringLiteral( "sentinel-%1-XXXXXX" ).arg( iteration ) ) };
        REQUIRE( sentinel.open() );
    }

    REQUIRE( fdLimit.restore() );
}
#endif
