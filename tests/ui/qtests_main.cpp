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
#include <QtConcurrent>

#if defined( Q_OS_UNIX )
#include <sys/resource.h>
#endif

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
    return runner.result();
}
