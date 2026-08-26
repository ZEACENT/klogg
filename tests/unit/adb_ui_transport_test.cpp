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

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <map>
#include <optional>

#include "adbdevicelistprovider.h"
#include "adblogcatdialog.h"
#include "adblogcatsource.h"
#include "adbprocesstransport.h"
#include "adbsmartsockettransport.h"
#include "commandargumenttokenizer.h"
#include "configuration.h"
#include "highlighterset.h"
#include "ioslogdialog.h"
#include "ioslogprocesstransport.h"
#include "livesourcetransport.h"
#include "optionsdialog.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "shortcuts.h"
#include "streaminglogdata.h"
#include "test_utils.h"

namespace {
bool isHeadlessDialogTestEnvironment()
{
    return QGuiApplication::screens().isEmpty()
           || QGuiApplication::platformName().compare( QStringLiteral( "offscreen" ),
                                                       Qt::CaseInsensitive )
                  == 0;
}

bool skipHeadlessOptionsDialogTest()
{
    if ( isHeadlessDialogTestEnvironment() ) {
        WARN( "OptionsDialog UI coverage is skipped on headless/offscreen platforms" );
        return true;
    }

    return false;
}

class ScopedAdbConfigurationGuard {
public:
    ScopedAdbConfigurationGuard()
        : config_( Configuration::getSynced() )
        , ansiOutput_( config_.adbLogcatAnsiOutputEnabled() )
    {
    }

    ~ScopedAdbConfigurationGuard()
    {
        config_.setAdbLogcatAnsiOutputEnabled( ansiOutput_ );
        config_.save();
    }

private:
    Configuration& config_;
    bool ansiOutput_;
};

class ScopedOptionsDialogConfigurationGuard {
public:
    ScopedOptionsDialogConfigurationGuard()
        : config_( Configuration::getSynced() )
        , savedSearches_( SavedSearches::getSynced() )
        , recentFiles_( RecentFiles::getSynced() )
        // OptionsDialog mirrors and (on Apply) persists the color-label match
        // defaults through HighlighterSetCollection, so snapshot it alongside
        // the other non-Configuration persistables the dialog touches.
        , highlighterSets_( HighlighterSetCollection::getSynced() )
    {
        REQUIRE( snapshotDir_.isValid() );
        snapshotPath_ = snapshotDir_.filePath( QStringLiteral( "settings.ini" ) );

        QSettings snapshot( snapshotPath_, QSettings::IniFormat );
        config_.saveToStorage( snapshot );
        savedSearches_.saveToStorage( snapshot );
        recentFiles_.saveToStorage( snapshot );
        highlighterSets_.saveToStorage( snapshot );
        snapshot.sync();
    }

    ~ScopedOptionsDialogConfigurationGuard()
    {
        QSettings snapshot( snapshotPath_, QSettings::IniFormat );
        config_.retrieveFromStorage( snapshot );
        config_.save();

        savedSearches_.retrieveFromStorage( snapshot );
        savedSearches_.save();

        recentFiles_.retrieveFromStorage( snapshot );
        recentFiles_.save();

        highlighterSets_.retrieveFromStorage( snapshot );
        highlighterSets_.save();
    }

private:
    Configuration& config_;
    SavedSearches& savedSearches_;
    RecentFiles& recentFiles_;
    HighlighterSetCollection& highlighterSets_;
    QTemporaryDir snapshotDir_;
    QString snapshotPath_;
};

template <typename Base>
class GenerationDrivenProcessTransport : public Base {
public:
    using Base::Base;
    using Generation = LiveSourceTransport::Generation;

    bool startAndWait()
    {
        const auto generation = nextGeneration();
        currentGeneration_ = generation;
        bool completed = false;
        bool connected = false;
        const auto stateConnection = QObject::connect(
            this, &LiveSourceTransport::stateChanged,
            [ & ]( Generation reportedGeneration, LiveSourceTransport::State state ) {
                if ( reportedGeneration != generation ) {
                    return;
                }
                if ( state == LiveSourceTransport::State::Connected
                     || state == LiveSourceTransport::State::Error ) {
                    completed = true;
                    connected = state == LiveSourceTransport::State::Connected;
                }
            } );

        this->start( generation );
        QElapsedTimer deadline;
        deadline.start();
        while ( !completed && deadline.elapsed() < 6000 ) {
            QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        }
        QObject::disconnect( stateConnection );
        return connected;
    }

    void startAsync()
    {
        const auto generation = nextGeneration();
        currentGeneration_ = generation;
        this->start( generation );
    }

    void stopCurrent()
    {
        const auto generation = currentGeneration_;
        currentGeneration_.reset();
        if ( generation.has_value() ) {
            this->stop( *generation );
        }
    }

private:
    Generation nextGeneration()
    {
        return ++generationCounter_;
    }

    Generation generationCounter_{ 0 };
    std::optional<Generation> currentGeneration_;
};

class TestAdbProcessTransport : public GenerationDrivenProcessTransport<AdbProcessTransport> {
public:
    using TestBase = GenerationDrivenProcessTransport<AdbProcessTransport>;
    using TestBase::TestBase;
    using Command = ProcessLiveSourceTransport::Command;

    Command streamingCommandForTest() const
    {
        return streamingCommand();
    }

    Command clearCommandForTest() const
    {
        return clearCommand();
    }
};

class ImmediateFailureAdbProcessTransport
    : public GenerationDrivenProcessTransport<AdbProcessTransport> {
public:
    ImmediateFailureAdbProcessTransport()
        : GenerationDrivenProcessTransport<AdbProcessTransport>(
              QString{}, QStringLiteral( "serial-123" ), {} )
    {
    }

protected:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return Command{ QStringLiteral( "cmd" ),
                        { QStringLiteral( "/c" ),
                          QStringLiteral( "exit" ),
                          QStringLiteral( "/b" ),
                          QStringLiteral( "7" ) } };
#else
        return Command{ QStringLiteral( "/bin/sh" ),
                        { QStringLiteral( "-c" ), QStringLiteral( "exit 7" ) } };
#endif
    }
};

class ReentrantReconnectAdbProcessTransport final : public ImmediateFailureAdbProcessTransport {
public:
    QString stderrFilePathForTest() const
    {
        return stderrFilePath();
    }

protected:
    void startProcessAsync( QProcess& ) override
    {
        // Keep the replacement capture alive without starting a second process.
    }
};

class LongRunningAdbProcessTransport final
    : public GenerationDrivenProcessTransport<AdbProcessTransport> {
public:
    LongRunningAdbProcessTransport()
        : GenerationDrivenProcessTransport<AdbProcessTransport>(
              QString{}, QStringLiteral( "serial-123" ), {} )
    {
    }

    QString stderrFilePathForTest() const
    {
        return stderrFilePath();
    }

protected:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return Command{ QStringLiteral( "ping" ),
                        { QStringLiteral( "-n" ),
                          QStringLiteral( "30" ),
                          QStringLiteral( "127.0.0.1" ) } };
#else
        return Command{ QStringLiteral( "/bin/sleep" ), { QStringLiteral( "30" ) } };
#endif
    }
};

struct DeviceListTaskState {
    QSemaphore taskEntered;
    QSemaphore allowTaskToFinish;
    QString result = QStringLiteral( "snapshot" );
};

class SnapshotDeviceListProvider final : public DeviceListProviderBase<QString> {
public:
    explicit SnapshotDeviceListProvider( std::shared_ptr<DeviceListTaskState> state )
        : DeviceListProviderBase<QString>( [ state, snapshot = state->result ] {
            state->taskEntered.release();
            if ( !state->allowTaskToFinish.tryAcquire( 1, 3000 ) ) {
                return QList<QString>{};
            }
            return QList<QString>{ snapshot };
        } )
        , state_( std::move( state ) )
    {
    }

protected:
    QList<QString> doListDevices( QString* ) const override
    {
        const auto state = state_;
        state->taskEntered.release();
        if ( !state->allowTaskToFinish.tryAcquire( 1, 3000 ) ) {
            return {};
        }
        return { state->result };
    }

    bool deviceMatches( const QString& device, const QString& deviceId ) const override
    {
        return device == deviceId;
    }

private:
    std::shared_ptr<DeviceListTaskState> state_;
};

class TestIosLogProcessTransport : public GenerationDrivenProcessTransport<IosLogProcessTransport> {
public:
    using TestBase = GenerationDrivenProcessTransport<IosLogProcessTransport>;
    using TestBase::TestBase;
    using Command = ProcessLiveSourceTransport::Command;

    Command streamingCommandForTest() const
    {
        return streamingCommand();
    }

    Command clearCommandForTest() const
    {
        return clearCommand();
    }

    QString stderrFilePathForTest() const
    {
        return stderrFilePath();
    }

    void filterReceivedBytesForTest( QByteArray& data )
    {
        filterReceivedBytes( data );
    }
};

QString makeCaptureId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}

bool waitForLineCount( const std::shared_ptr<StreamingLogData>& logData,
                       unsigned long long lineCount )
{
    QElapsedTimer deadline;
    deadline.start();
    while ( logData->getNbLine().get() < lineCount && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        QTest::qWait( 50 );
    }
    return logData->getNbLine().get() >= lineCount;
}

bool waitForSourceState( const AdbLogcatSource& source, AdbLogcatSource::State state )
{
    QElapsedTimer deadline;
    deadline.start();
    while ( source.state() != state && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        QTest::qWait( 50 );
    }
    return source.state() == state;
}

// Transport teardown retires QProcess objects with deleteLater(), which only
// takes effect once a DeferredDelete event is delivered. A late process signal
// or a ~QProcess destruction cascade arriving during the final pass posts new
// deferred deletions that no later pass delivers, and LeakSanitizer then
// reports the retired QProcess at binary exit (CI ubuntu-22.04-asan-ubsan-lsan
// flake, seen twice with the identical 6675-bytes/26-allocations signature).
// Qt 6 has no public API to query the posted-event queue, so make teardown
// deterministic by construction: alternate explicit DeferredDelete delivery
// with processEvents (which drains the posted queue to a fixpoint, cascades
// included) and always end on processEvents, never on sendPostedEvents.
void drainLiveSourceEvents( int settleMs )
{
    QCoreApplication::processEvents();
    QTest::qWait( settleMs );
    for ( auto pass = 0; pass < 4; ++pass ) {
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        QCoreApplication::processEvents();
    }
}

bool hasFutureWatcherChild( const QObject& object )
{
    const auto children = object.children();
    return std::any_of( children.cbegin(), children.cend(), []( const QObject* child ) {
        return QString::fromLatin1( child->metaObject()->className() )
            .startsWith( QStringLiteral( "QFutureWatcher" ) );
    } );
}
} // namespace

TEST_CASE( "Default live-source factory selects the explicit ADB backend" )
{
    DefaultLiveSourceTransportFactory factory;
    LiveSourceTransportConfig config;
    config.sourceType = LiveLogSourceType::AdbLogcat;
    config.deviceId = QStringLiteral( "SERIAL-42" );

    // Missing backend configuration is modern and environment-independent.
    auto defaultTransport = factory.create( config );
    REQUIRE( defaultTransport != nullptr );
    CHECK( dynamic_cast<klogg::livecapture::adb::AdbSmartSocketTransport*>( defaultTransport.get() )
           != nullptr );

    // The process compatibility backend is reachable only through an explicit
    // discriminator together with an explicit executable. It never performs
    // lookup on behalf of an incomplete/default config.
    config.adbBackend = AdbTransportBackend::Process;
    CHECK( factory.create( config ) == nullptr );
    config.executable = QStringLiteral( "/explicit/legacy/adb" );
    auto processTransport = factory.create( config );
    REQUIRE( processTransport != nullptr );
    CHECK( dynamic_cast<AdbProcessTransport*>( processTransport.get() ) != nullptr );

    config.adbBackend = AdbTransportBackend::SmartSocket;
    config.executable.clear();
    auto smartSocketTransport = factory.create( config );
    REQUIRE( smartSocketTransport != nullptr );
    CHECK( dynamic_cast<klogg::livecapture::adb::AdbSmartSocketTransport*>(
               smartSocketTransport.get() )
           != nullptr );

    config.sourceType = LiveLogSourceType::IosLogStream;
    config.iosBackend = IosTransportBackend::LegacyProcess;
    CHECK( factory.create( config ) == nullptr );
    config.executable = QStringLiteral( "/explicit/legacy/pymobiledevice3" );
    auto iosTransport = factory.create( config );
    REQUIRE( iosTransport != nullptr );
    CHECK( dynamic_cast<IosLogProcessTransport*>( iosTransport.get() ) != nullptr );
}

TEST_CASE( "AdbProcessTransport builds normalized streaming and clear commands" )
{
    TestAdbProcessTransport transport(
        QString{}, QStringLiteral( "emulator-5554" ),
        QStringLiteral( "-v threadtime -T \"2026-03-15 12:34:56.000\" *:I" ) );

    const auto streaming = transport.streamingCommandForTest();
    // When no explicit path is configured, the program is either bare "adb"
    // (host has adb on PATH) or the absolute path of an installed adb (resolved
    // from a well-known install location).  The argument decoration is what
    // this test exists to verify.
    REQUIRE_FALSE( streaming.program.isEmpty() );
    // Either bare "adb"/"adb.exe" (host has adb on PATH and findAdbAtKnownLocation()
    // returned empty) or an absolute path resolved from a well-known install
    // location.  Qt normalizes paths to forward slashes on Windows too, so the
    // path-suffix check uses "/adb" / "/adb.exe" on every platform.
    {
        const auto& program = streaming.program;
        const QFileInfo info( program );
        const auto leaf = info.fileName().toLower();
        REQUIRE( ( program == QStringLiteral( "adb" ) || program == QStringLiteral( "adb.exe" )
                   || ( info.isAbsolute()
                        && ( leaf == QStringLiteral( "adb" )
                             || leaf == QStringLiteral( "adb.exe" ) ) ) ) );
    }
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "emulator-5554" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "-v" ),
                             QStringLiteral( "threadtime" ), QStringLiteral( "-T" ),
                             QStringLiteral( "2026-03-15 12:34:56.000" ),
                             QStringLiteral( "*:I" ) } );

    const auto clear = transport.clearCommandForTest();
    REQUIRE( clear.program == streaming.program );
    REQUIRE( clear.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "emulator-5554" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "-c" ) } );
}

TEST_CASE( "AdbProcessTransport preserves literal backslashes in extra args" )
{
    TestAdbProcessTransport transport(
        QString{}, QStringLiteral( "serial-123" ),
        QStringLiteral( "--path C:\\temp\\log.txt --pattern regex\\d+ --title hello\\ world" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "serial-123" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "--path" ),
                             QStringLiteral( "C:\\temp\\log.txt" ), QStringLiteral( "--pattern" ),
                             QStringLiteral( "regex\\d+" ), QStringLiteral( "--title" ),
                             QStringLiteral( "hello world" ) } );
}

TEST_CASE( "AdbProcessTransport preserves empty quoted extra args" )
{
    TestAdbProcessTransport transport( QString{}, QStringLiteral( "serial-123" ),
                                       QStringLiteral( "--empty '' --quoted \"\"" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "serial-123" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "--empty" ), QString{},
                             QStringLiteral( "--quoted" ), QString{} } );
}

TEST_CASE( "AdbProcessTransport adds logcat color modifier when ANSI output is enabled" )
{
    TestAdbProcessTransport transport( QString{}, QStringLiteral( "serial-123" ),
                                       QStringLiteral( "-v threadtime *:I" ), true );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "serial-123" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "-v" ),
                             QStringLiteral( "color" ), QStringLiteral( "-v" ),
                             QStringLiteral( "threadtime" ), QStringLiteral( "*:I" ) } );
}

TEST_CASE( "IosLogProcessTransport builds normalized streaming commands" )
{
    TestIosLogProcessTransport transport( QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
                                          QStringLiteral( "00008030-001C195E36D8802E" ),
                                          QStringLiteral( "--match \"process name\"" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.program == QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ) );
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "--no-color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ),
                             QStringLiteral( "--match" ), QStringLiteral( "process name" ) } );
}

TEST_CASE( "ProcessLiveSourceTransport creates unique private stderr capture files" )
{
    TestIosLogProcessTransport first( QStringLiteral( "pymobiledevice3" ),
                                      QStringLiteral( "first-device" ), QString{} );
    TestIosLogProcessTransport second( QStringLiteral( "pymobiledevice3" ),
                                       QStringLiteral( "second-device" ), QString{} );

    const QFileInfo firstFile( first.stderrFilePathForTest() );
    const QFileInfo secondFile( second.stderrFilePathForTest() );
    REQUIRE( firstFile.exists() );
    REQUIRE( secondFile.exists() );
    REQUIRE( firstFile.absoluteFilePath() != secondFile.absoluteFilePath() );
#ifndef Q_OS_WIN
    const auto permissions = firstFile.permissions();
    CHECK_FALSE( permissions.testFlag( QFileDevice::ReadGroup ) );
    CHECK_FALSE( permissions.testFlag( QFileDevice::WriteGroup ) );
    CHECK_FALSE( permissions.testFlag( QFileDevice::ReadOther ) );
    CHECK_FALSE( permissions.testFlag( QFileDevice::WriteOther ) );
#endif
}

TEST_CASE( "ProcessLiveSourceTransport keeps a reentrant reconnect capture alive" )
{
    ReentrantReconnectAdbProcessTransport transport;
    const auto initialPath = transport.stderrFilePathForTest();
    QString failedPath;
    bool reconnected = false;
    QObject::connect( &transport, &LiveSourceTransport::errorOccurred, &transport, [ & ] {
        if ( !reconnected ) {
            failedPath = transport.stderrFilePathForTest();
            reconnected = true;
            transport.startAsync();
        }
    } );

    CHECK_FALSE( transport.startAndWait() );
    REQUIRE( reconnected );
    REQUIRE( failedPath != initialPath );
    REQUIRE( transport.stderrFilePathForTest() != failedPath );
    REQUIRE( QFileInfo::exists( transport.stderrFilePathForTest() ) );

    transport.stopCurrent();
}

TEST_CASE( "ProcessLiveSourceTransport removes detached stderr captures after disconnect" )
{
    LongRunningAdbProcessTransport transport;
    REQUIRE( transport.startAndWait() );

    const auto detachedPath = transport.stderrFilePathForTest();
    REQUIRE( QFileInfo::exists( detachedPath ) );

    transport.stopCurrent();
    REQUIRE( transport.stderrFilePathForTest() != detachedPath );
    REQUIRE( QFileInfo::exists( transport.stderrFilePathForTest() ) );

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    while ( QFileInfo::exists( detachedPath ) && cleanupTimer.elapsed() < 3000 ) {
        QCoreApplication::processEvents();
        QTest::qWait( 10 );
    }
    CHECK_FALSE( QFileInfo::exists( detachedPath ) );
}

TEST_CASE( "IosLogProcessTransport passes color flags as pymobiledevice3 top-level options" )
{
    TestIosLogProcessTransport colorTransport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, true );
    TestIosLogProcessTransport plainTransport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, false );

    const auto colorCmd = colorTransport.streamingCommandForTest();
    const auto plainCmd = plainTransport.streamingCommandForTest();

#ifdef Q_OS_MAC
    // Wrapped with /usr/bin/script + a shell that redirects the inner
    // command's stderr to the transport's temp file (outside the PTY) so
    // pymobiledevice3 diagnostics never reach the log view.
    REQUIRE( colorCmd.program == QStringLiteral( "/usr/bin/script" ) );
    REQUIRE( colorCmd.arguments.size() == 12 );
    REQUIRE( colorCmd.arguments[ 0 ] == QStringLiteral( "-q" ) );
    REQUIRE( colorCmd.arguments[ 1 ] == QStringLiteral( "/dev/null" ) );
    REQUIRE( colorCmd.arguments[ 2 ] == QStringLiteral( "/bin/sh" ) );
    REQUIRE( colorCmd.arguments[ 3 ] == QStringLiteral( "-c" ) );
    REQUIRE( colorCmd.arguments[ 4 ] == QStringLiteral( "exec \"$@\" 2>\"$0\"" ) );
    REQUIRE( colorCmd.arguments[ 5 ] == colorTransport.stderrFilePathForTest() );
    REQUIRE( colorCmd.arguments.mid( 6 )
             == QStringList{ QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
                             QStringLiteral( "--color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ) } );
#else
    REQUIRE( colorCmd.arguments
             == QStringList{ QStringLiteral( "--color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ) } );
#endif

    REQUIRE( plainCmd.arguments
             == QStringList{ QStringLiteral( "--no-color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ) } );
}

TEST_CASE( "IosLogProcessTransport preserves empty quoted extra args" )
{
    TestIosLogProcessTransport transport( QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
                                          QStringLiteral( "00008030-001C195E36D8802E" ),
                                          QStringLiteral( "--tunnel '' --match \"\"" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "--no-color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ),
                             QStringLiteral( "--tunnel" ), QString{}, QStringLiteral( "--match" ),
                             QString{} } );
}

TEST_CASE( "IosLogProcessTransport wraps with PTY when ANSI output is enabled" )
{
    // pymobiledevice3 checks isatty() in addition to the --color flag; when
    // QProcess pipes stdout, isatty() is false so ANSI codes are never emitted.
    // The fix: when ansiOutputEnabled is true, wrap the command with
    // /usr/bin/script -q /dev/null so that pymobiledevice3 sees a TTY and
    // actually produces ANSI escape codes.
    TestIosLogProcessTransport colorTransport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, true );
    TestIosLogProcessTransport plainTransport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, false );

    const auto colorCmd = colorTransport.streamingCommandForTest();
#ifdef Q_OS_MAC
    // On macOS, the command is wrapped with script to allocate a PTY.  The
    // inner command runs via sh -c 'exec "$@" 2>"$0"' so its stderr is
    // redirected to the transport's temp file (outside the PTY) and cannot
    // leak into the log view.
    REQUIRE( colorCmd.program == QStringLiteral( "/usr/bin/script" ) );
    REQUIRE( colorCmd.arguments[ 0 ] == QStringLiteral( "-q" ) );
    REQUIRE( colorCmd.arguments[ 1 ] == QStringLiteral( "/dev/null" ) );
    REQUIRE( colorCmd.arguments[ 2 ] == QStringLiteral( "/bin/sh" ) );
    REQUIRE( colorCmd.arguments[ 3 ] == QStringLiteral( "-c" ) );
    REQUIRE( colorCmd.arguments[ 4 ] == QStringLiteral( "exec \"$@\" 2>\"$0\"" ) );
    REQUIRE( colorCmd.arguments[ 5 ] == colorTransport.stderrFilePathForTest() );
    REQUIRE( colorCmd.arguments.mid( 6 )
             == QStringList{ QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
                             QStringLiteral( "--color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ) } );
#else
    // On non-macOS, script is not available; fall back to bare --color.
    REQUIRE( colorCmd.program == QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ) );
    REQUIRE( colorCmd.arguments
             == QStringList{ QStringLiteral( "--color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ) } );
#endif

    // Without ANSI, no PTY wrapper is needed.
    const auto plainCmd = plainTransport.streamingCommandForTest();
    REQUIRE( plainCmd.program == QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ) );
    REQUIRE( plainCmd.arguments
             == QStringList{ QStringLiteral( "--no-color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ), QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ) } );
}

TEST_CASE( "IosLogProcessTransport strips script PTY header from received data" )
{
    // macOS script(1) emits a ^D\b\b prefix at the start of its output
    // (the literal bytes 0x5e 0x44 0x08 0x08).  The transport must strip
    // this garbage so it doesn't appear as a spurious first line in the log.
    TestIosLogProcessTransport colorTransport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, true );
    TestIosLogProcessTransport plainTransport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, false );

    // Case 1: full prefix arrives in one chunk
    QByteArray ptyOutput = QByteArrayLiteral( "^D" ) + QByteArray( 2, '\b' )
                           + QByteArrayLiteral( "Default 12:34:56 App Message\n" );
    colorTransport.filterReceivedBytesForTest( ptyOutput );
#ifdef Q_OS_MAC
    REQUIRE( ptyOutput == QByteArrayLiteral( "Default 12:34:56 App Message\n" ) );
#else
    REQUIRE( ptyOutput.startsWith( QByteArrayLiteral( "^D" ) ) );
#endif

    // Second chunk: no more ^D\b\b to strip.
    QByteArray secondChunk = QByteArrayLiteral( "Warning 12:34:57 App Another\n" );
    colorTransport.filterReceivedBytesForTest( secondChunk );
    REQUIRE( secondChunk == QByteArrayLiteral( "Warning 12:34:57 App Another\n" ) );

    // Plain transport never filters.
    QByteArray plainData = QByteArrayLiteral( "some data\n" );
    plainTransport.filterReceivedBytesForTest( plainData );
    REQUIRE( plainData == QByteArrayLiteral( "some data\n" ) );
}

TEST_CASE( "IosLogProcessTransport handles split PTY prefix across chunks" )
{
#ifdef Q_OS_MAC
    // The ^D\b\b prefix (4 bytes) may arrive split across two reads.
    // The transport must buffer the partial prefix and strip it once
    // the rest arrives, without leaking bytes into the log.
    TestIosLogProcessTransport transport( QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
                                          QStringLiteral( "00008030-001C195E36D8802E" ), QString{},
                                          true );

    // First chunk: only "^D" (2 of 4 bytes of the prefix)
    QByteArray chunk1 = QByteArrayLiteral( "^D" );
    transport.filterReceivedBytesForTest( chunk1 );
    REQUIRE( chunk1.isEmpty() ); // buffered, not emitted

    // Second chunk: remaining "\b\b" + real data
    QByteArray chunk2 = QByteArray( 2, '\b' ) + QByteArrayLiteral( "Default 12:34:56 Msg\n" );
    transport.filterReceivedBytesForTest( chunk2 );
    REQUIRE( chunk2 == QByteArrayLiteral( "Default 12:34:56 Msg\n" ) );

    // Subsequent chunks pass through unchanged.
    QByteArray chunk3 = QByteArrayLiteral( "Next line\n" );
    transport.filterReceivedBytesForTest( chunk3 );
    REQUIRE( chunk3 == QByteArrayLiteral( "Next line\n" ) );
#else
    SUCCEED( "Split-prefix test is macOS-only." );
#endif
}

TEST_CASE( "IosLogProcessTransport PTY wrapper forces ANSI output from script-emulating process" )
{
#ifdef Q_OS_MAC
    // Skip on headless/offscreen CI where /usr/bin/script may not behave.
    if ( isHeadlessDialogTestEnvironment() ) {
        WARN( "PTY integration test skipped on headless/offscreen platforms" );
        return;
    }

    // Create a mock pymobiledevice3 that only emits ANSI codes when stdout is a TTY.
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto scriptPath = tempDir.filePath( QStringLiteral( "pymobiledevice3" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\n"
                  "if [ -t 1 ]; then\n"
                  "  printf '\\033[31mDefault\\033[0m 12:00:00 App Hello\\n'\n"
                  "else\n"
                  "  echo 'NO_ANSI 12:00:00 App Hello'\n"
                  "fi\n"
                  "echo 'STDERR_LEAK pymobiledevice3 ERROR Device not found' >&2\n"
                  "sleep 5\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    // With ANSI enabled (script wrapper), the process should emit ANSI codes.
    {
        TestIosLogProcessTransport colorTransport( scriptPath, QStringLiteral( "DEVICE_UDID" ),
                                                   QString{}, true );
        SafeQSignalSpy bytesSpy(
            &colorTransport,
            SIGNAL( bytesReceived( LiveSourceTransport::Generation, QByteArray ) ) );

        REQUIRE( colorTransport.startAndWait() );

        QByteArray accumulated;
        QElapsedTimer deadline;
        deadline.start();
        while ( deadline.elapsed() < 3000 ) {
            QCoreApplication::processEvents();
            QTest::qWait( 50 );
            if ( bytesSpy.count() > 0 ) {
                for ( int i = 0; i < bytesSpy.count(); ++i ) {
                    accumulated += bytesSpy.at( i ).at( 1 ).toByteArray();
                }
                break;
            }
        }

        INFO( "Accumulated bytes: " << accumulated.toStdString() );
        // Must contain the ANSI escape code (0x1b) — proving the PTY wrapper
        // forced the process to see a TTY.
        REQUIRE( accumulated.contains( '\x1b' ) );
        // Must NOT contain the no-TTY fallback text.
        REQUIRE_FALSE( accumulated.contains( QByteArrayLiteral( "NO_ANSI" ) ) );
        // pymobiledevice3's stderr must NOT leak into the log view, even though
        // the PTY (script) merges stdout/stderr — the sh -c wrapper redirects
        // the inner command's stderr to the transport's temp file.
        REQUIRE_FALSE( accumulated.contains( QByteArrayLiteral( "STDERR_LEAK" ) ) );

        colorTransport.stopCurrent();
        QCoreApplication::processEvents();
        QTest::qWait( 500 );
        QCoreApplication::processEvents();
    }

    // Without ANSI (no script wrapper), the process should NOT emit ANSI codes.
    {
        TestIosLogProcessTransport plainTransport( scriptPath, QStringLiteral( "DEVICE_UDID" ),
                                                   QString{}, false );
        SafeQSignalSpy bytesSpy(
            &plainTransport,
            SIGNAL( bytesReceived( LiveSourceTransport::Generation, QByteArray ) ) );

        REQUIRE( plainTransport.startAndWait() );

        QByteArray accumulated;
        QElapsedTimer deadline;
        deadline.start();
        while ( deadline.elapsed() < 3000 ) {
            QCoreApplication::processEvents();
            QTest::qWait( 50 );
            if ( bytesSpy.count() > 0 ) {
                for ( int i = 0; i < bytesSpy.count(); ++i ) {
                    accumulated += bytesSpy.at( i ).at( 1 ).toByteArray();
                }
                break;
            }
        }

        INFO( "Accumulated bytes: " << accumulated.toStdString() );
        // Must NOT contain ANSI escape codes — plain pipe, no TTY.
        REQUIRE_FALSE( accumulated.contains( '\x1b' ) );
        // Must contain the no-TTY fallback text.
        REQUIRE( accumulated.contains( QByteArrayLiteral( "NO_ANSI" ) ) );
        // Without the PTY wrapper, stderr goes to the temp file too — no leak.
        REQUIRE_FALSE( accumulated.contains( QByteArrayLiteral( "STDERR_LEAK" ) ) );

        plainTransport.stopCurrent();
        QCoreApplication::processEvents();
        QTest::qWait( 500 );
        QCoreApplication::processEvents();
    }
#else
    SUCCEED( "PTY wrapper test is macOS-only." );
#endif
}

TEST_CASE( "IosLogProcessTransport clear command is an inert no-op" )
{
    TestIosLogProcessTransport transport( QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
                                          QStringLiteral( "00008030-001C195E36D8802E" ),
                                          QString{} );

    const auto clear = transport.clearCommandForTest();
#ifdef Q_OS_WIN
    REQUIRE( clear.program == QStringLiteral( "cmd" ) );
    REQUIRE(
        clear.arguments
        == QStringList{ QStringLiteral( "/c" ), QStringLiteral( "exit" ), QStringLiteral( "0" ) } );
#else
    REQUIRE( clear.program == QStringLiteral( "true" ) );
    REQUIRE( clear.arguments.isEmpty() );
#endif
}

TEST_CASE( "IosLogProcessTransport lists devices using full JSON output" )
{
#ifdef Q_OS_MAC
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto scriptPath = tempDir.filePath( QStringLiteral( "pymobiledevice3" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\n"
                  "printf '[{\"Identifier\":\"00008030\",\"DeviceName\":\"Test iPhone\","
                  "\"ProductType\":\"iPhone14,2\",\"ProductVersion\":\"17.0\"}]\\n'\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    QString error;
    const auto devices = IosLogProcessTransport::listDevices( scriptPath, &error );
    REQUIRE( error.isEmpty() );
    REQUIRE( devices.size() == 1 );
    CHECK( devices.front().udid == QStringLiteral( "00008030" ) );
    CHECK( devices.front().description == QStringLiteral( "Test iPhone" ) );
    CHECK( devices.front().productType == QStringLiteral( "iPhone14,2" ) );
    CHECK( devices.front().productVersion == QStringLiteral( "17.0" ) );
#else
    SUCCEED( "pymobiledevice3 device listing is macOS-only." );
#endif
}

TEST_CASE( "IosLogProcessTransport falls back to --simple when full listing fails" )
{
#ifdef Q_OS_MAC
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto scriptPath = tempDir.filePath( QStringLiteral( "pymobiledevice3" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    // Legacy (full) listing fails; --simple returns UDID-only JSON
    script.write( "#!/bin/sh\n"
                  "case \"$*\" in\n"
                  "  *--simple*) printf '[\"00008030\"]\\n' ;;\n"
                  "  *) echo 'Full listing not supported' >&2; exit 2 ;;\n"
                  "esac\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    QString error;
    const auto devices = IosLogProcessTransport::listDevices( scriptPath, &error );
    REQUIRE( error.isEmpty() );
    REQUIRE( devices.size() == 1 );
    CHECK( devices.front().udid == QStringLiteral( "00008030" ) );
#else
    SUCCEED( "pymobiledevice3 device listing is macOS-only." );
#endif
}

TEST_CASE( "IosLogProcessTransport reports unsupported device listing off macOS" )
{
#ifndef Q_OS_MAC
    QString error;
    const auto devices = IosLogProcessTransport::listDevices( QString{}, &error );
    REQUIRE( devices.isEmpty() );
    REQUIRE_FALSE( error.isEmpty() );
#else
    SUCCEED( "iOS device listing is macOS-only and covered by command construction here." );
#endif
}

TEST_CASE( "AdbProcessTransport reports startup failures through the transport interface" )
{
    TestAdbProcessTransport transport( QStringLiteral( "/path/that/does/not/exist/adb" ),
                                       QStringLiteral( "serial" ), {} );
    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );

    REQUIRE_FALSE( transport.startAndWait() );
    REQUIRE( errorSpy.safeWait() );
    REQUIRE( stateSpy.count() >= 1 );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
}

TEST_CASE( "AdbProcessTransport listDevices returns an error when adb cannot start" )
{
    QString error;
    const auto devices = AdbProcessTransport::listDevices(
        QStringLiteral( "/path/that/does/not/exist/adb" ), &error );

    REQUIRE( devices.isEmpty() );
    REQUIRE_FALSE( error.isEmpty() );
}

TEST_CASE( "AdbDeviceListProvider returns same results as static listDevices" )
{
    // The provider abstraction should return identical results to the
    // old static method on AdbProcessTransport.
    QString providerError;
    AdbDeviceListProvider provider( QStringLiteral( "/path/that/does/not/exist/adb" ) );
    const auto devices = provider.listDevices( &providerError );

    QString staticError;
    const auto staticDevices = AdbProcessTransport::listDevices(
        QStringLiteral( "/path/that/does/not/exist/adb" ), &staticError );

    REQUIRE( devices.isEmpty() );
    REQUIRE( staticDevices.isEmpty() );
    REQUIRE_FALSE( providerError.isEmpty() );
    REQUIRE_FALSE( staticError.isEmpty() );
}

TEST_CASE( "AdbDeviceListProvider reports unknown availability on discovery error" )
{
    AdbDeviceListProvider provider( QStringLiteral( "/nonexistent/adb" ) );
    const auto result = provider.deviceAvailability( QStringLiteral( "any-serial" ) );

    CHECK( result.availability == DeviceAvailability::Unknown );
    REQUIRE( result.error.has_value() );
    CHECK( result.error->category == klogg::livecapture::ErrorCategory::Configuration );
}

TEST_CASE( "AdbDeviceListProvider listDevicesAsync returns a valid future" )
{
    AdbDeviceListProvider provider( QStringLiteral( "/nonexistent/adb" ) );
    auto future = provider.listDevicesAsync();

    // The future should be valid (has been started).
    // Use isStarted() instead of isValid() for Qt 5 compatibility
    // (isValid() was added in Qt 6.0).
    REQUIRE( future.isStarted() );

    // Wait for it to complete — should resolve to an empty list since
    // the executable doesn't exist.
    future.waitForFinished();
    REQUIRE( future.result().isEmpty() );
}

TEST_CASE( "IosDeviceListProvider reports unknown availability on discovery error" )
{
    IosDeviceListProvider provider( QStringLiteral( "/nonexistent/pymobiledevice3" ) );
    const auto result = provider.deviceAvailability( QStringLiteral( "any-udid" ) );

    CHECK( result.availability == DeviceAvailability::Unknown );
    REQUIRE( result.error.has_value() );
    CHECK( result.error->category == klogg::livecapture::ErrorCategory::Configuration );
}

TEST_CASE( "IosDeviceListProvider listDevicesAsync returns a valid future" )
{
    IosDeviceListProvider provider( QStringLiteral( "/nonexistent/pymobiledevice3" ) );
    auto future = provider.listDevicesAsync();

    // Use isStarted() instead of isValid() for Qt 5 compatibility.
    REQUIRE( future.isStarted() );

    future.waitForFinished();
    // On non-macOS or with nonexistent executable, result should be empty.
    REQUIRE( future.result().isEmpty() );
}

TEST_CASE( "AdbProcessTransport surfaces immediate post-start failures as transport errors" )
{
    ImmediateFailureAdbProcessTransport transport;
    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );

    REQUIRE_FALSE( transport.startAndWait() );
    REQUIRE( errorSpy.safeWait() );
    REQUIRE( stateSpy.count() >= 1 );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
}

TEST_CASE( "OptionsDialog exposes no legacy live-source executable or raw-argument controls" )
{
    ScopedOptionsDialogConfigurationGuard configGuard;

    OptionsDialog dialog;

    const QStringList retiredObjectNames{
        QStringLiteral( "adbExecutableLineEdit" ),    QStringLiteral( "adbLogcatArgsLineEdit" ),
        QStringLiteral( "adbDetectButton" ),          QStringLiteral( "adbExecutableLabel" ),
        QStringLiteral( "adbLogcatArgsLabel" ),       QStringLiteral( "adbHelpLabel" ),
        QStringLiteral( "iosLogExecutableLineEdit" ), QStringLiteral( "iosLogArgsLineEdit" ),
        QStringLiteral( "iosLogDetectButton" ),
    };
    for ( const auto& objectName : retiredObjectNames ) {
        INFO( "retired object: " << objectName.toStdString() );
        CHECK( dialog.findChild<QObject*>( objectName ) == nullptr );
    }
    CHECK( dialog.findChildren<QFileDialog*>().isEmpty() );

    auto* backupCount
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceRollingBackupCountSpinBox" ) );
    REQUIRE( backupCount != nullptr );
    CHECK( backupCount->specialValueText() == QStringLiteral( "Keep all" ) );
    CHECK( backupCount->toolTip().contains( QStringLiteral( "keep all" ), Qt::CaseInsensitive ) );
    CHECK_FALSE( backupCount->toolTip().contains( QStringLiteral( "only the current" ),
                                                  Qt::CaseInsensitive ) );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );
}

TEST_CASE( "OptionsDialog default shortcut table keeps Apply and OK enabled" )
{
    if ( skipHeadlessOptionsDialogTest() ) {
        return;
    }

    ScopedOptionsDialogConfigurationGuard configGuard;
    auto& config = Configuration::getSynced();
    config.setShortcuts( {} );
    config.save();

    OptionsDialog dialog;
    auto* buttonBox = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );
    REQUIRE( buttonBox != nullptr );

    auto* okButton = buttonBox->button( QDialogButtonBox::Ok );
    auto* applyButton = buttonBox->button( QDialogButtonBox::Apply );
    REQUIRE( okButton != nullptr );
    REQUIRE( applyButton != nullptr );
    CHECK( okButton->isEnabled() );
    CHECK( applyButton->isEnabled() );
}

TEST_CASE( "OptionsDialog persists changed font size from preferences" )
{
    if ( skipHeadlessOptionsDialogTest() ) {
        return;
    }

    ScopedOptionsDialogConfigurationGuard configGuard;
    auto& config = Configuration::getSynced();
    const auto originalFont = config.mainFont();
    const auto baseSize = originalFont.pointSize() > 0 ? originalFont.pointSize() : 10;
    config.setMainFont( QFont{ originalFont.family(), baseSize } );
    config.save();

    OptionsDialog dialog;
    auto* fontSizeBox = dialog.findChild<QComboBox*>( QStringLiteral( "fontSizeBox" ) );
    REQUIRE( fontSizeBox != nullptr );

    const auto requestedSize = baseSize == 13 ? 14 : 13;
    auto sizeIndex = fontSizeBox->findText( QString::number( requestedSize ) );
    if ( sizeIndex == -1 ) {
        fontSizeBox->addItem( QString::number( requestedSize ) );
        sizeIndex = fontSizeBox->findText( QString::number( requestedSize ) );
    }
    REQUIRE( sizeIndex != -1 );
    fontSizeBox->setCurrentIndex( sizeIndex );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );

    const auto restoredFont = Configuration::getSynced().mainFont();
    CHECK( restoredFont.pointSize() == requestedSize );
}

TEST_CASE( "OptionsDialog reset buttons restore defaults and can be applied" )
{
    if ( skipHeadlessOptionsDialogTest() ) {
        return;
    }

    ScopedOptionsDialogConfigurationGuard configGuard;

    auto& config = Configuration::getSynced();
    config.setVersionCheckingEnabled( false );
    config.setLineSpacingPercent( Configuration::MaxLineSpacingPercent );
    config.setPollIntervalMs( 12345 );
    config.setAdbLogcatAnsiOutputEnabled( true );
    config.setIosLogAnsiOutputEnabled( true );
    config.setUseSearchResultsCache( false );
    config.setShortcuts( { { ShortcutAction::MainWindowOpenFile,
                             QStringList{ QStringLiteral( "Ctrl+Shift+P" ) } } } );
    config.save();

    auto& savedSearches = SavedSearches::getSynced();
    savedSearches.setHistorySize( 7 );
    savedSearches.save();
    auto& recentFiles = RecentFiles::getSynced();
    recentFiles.setFilesHistoryMaxItems( 9 );
    recentFiles.save();

    OptionsDialog dialog;
    const QStringList resetButtons{
        QStringLiteral( "resetGeneralDefaultsButton" ),
        QStringLiteral( "resetViewDefaultsButton" ),
        QStringLiteral( "resetFileDefaultsButton" ),
        QStringLiteral( "resetLiveSourceDefaultsButton" ),
        QStringLiteral( "restoreShortcutsDefaults" ),
        QStringLiteral( "resetAdvancedDefaultsButton" ),
    };

    for ( const auto& objectName : resetButtons ) {
        auto* button = dialog.findChild<QPushButton*>( objectName );
        REQUIRE( button != nullptr );
        button->click();
        QCoreApplication::processEvents();
    }

    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );

    const Configuration defaults;
    const SavedSearches defaultSavedSearches;
    const RecentFiles defaultRecentFiles;
    const auto& restoredConfig = Configuration::getSynced();

    CHECK( restoredConfig.versionCheckingEnabled() == defaults.versionCheckingEnabled() );
    CHECK( restoredConfig.lineSpacingPercent() == defaults.lineSpacingPercent() );
    CHECK( restoredConfig.pollIntervalMs() == defaults.pollIntervalMs() );
    CHECK( restoredConfig.adbLogcatAnsiOutputEnabled() == defaults.adbLogcatAnsiOutputEnabled() );
    CHECK( restoredConfig.iosLogAnsiOutputEnabled() == defaults.iosLogAnsiOutputEnabled() );
    CHECK( restoredConfig.useSearchResultsCache() == defaults.useSearchResultsCache() );
    CHECK( SavedSearches::getSynced().historySize() == defaultSavedSearches.historySize() );
    CHECK( RecentFiles::getSynced().filesHistoryMaxItems()
           == defaultRecentFiles.filesHistoryMaxItems() );

    const auto restoredOpenFileShortcuts = ShortcutAction::shortcutKeys(
        ShortcutAction::MainWindowOpenFile, restoredConfig.shortcuts() );
    const auto defaultOpenFileShortcuts
        = ShortcutAction::shortcutKeys( ShortcutAction::MainWindowOpenFile, {} );
    CHECK_FALSE(
        restoredOpenFileShortcuts.contains( QKeySequence( QStringLiteral( "Ctrl+Shift+P" ) ) ) );
    for ( const auto& defaultShortcut : defaultOpenFileShortcuts ) {
        CHECK( restoredOpenFileShortcuts.contains( defaultShortcut ) );
    }
}

TEST_CASE( "OptionsDialog File and Live Source tab widgets do not overlap vertically" )
{
    ScopedOptionsDialogConfigurationGuard configGuard;

    OptionsDialog dialog;

    auto* tabWidget = dialog.findChild<QTabWidget*>( QStringLiteral( "tabWidget" ) );
    REQUIRE( tabWidget != nullptr );

    dialog.show();
    QCoreApplication::processEvents();

    // Check each tab for widget overlap at minimum dialog size
    const QStringList tabNames{
        QStringLiteral( "file_watch_tab" ),
        QStringLiteral( "liveSourceTab" ),
    };

    for ( const auto& tabName : tabNames ) {
        auto* tab = dialog.findChild<QWidget*>( tabName );
        REQUIRE( tab != nullptr );
        tabWidget->setCurrentWidget( tab );
        QCoreApplication::processEvents();

        dialog.resize( dialog.minimumSizeHint() );
        QCoreApplication::processEvents();

        auto* layout = tab->layout();
        REQUIRE( layout != nullptr );

        QList<QWidget*> visibleChildren;
        for ( int i = 0; i < layout->count(); ++i ) {
            auto* item = layout->itemAt( i );
            if ( item && item->widget() && item->widget()->isVisible() ) {
                visibleChildren.append( item->widget() );
            }
        }

        REQUIRE( visibleChildren.size() >= 2 );

        for ( int i = 0; i < visibleChildren.size() - 1; ++i ) {
            auto* current = visibleChildren[ i ];
            auto* next = visibleChildren[ i + 1 ];

            const auto currentBottom = current->geometry().bottom();
            const auto nextTop = next->geometry().top();
            const int gap = nextTop - currentBottom;

            INFO( "[" << tabName.toStdString() << "] Widget " << i << ": \""
                      << current->objectName().toStdString() << "\" bottom=" << currentBottom
                      << " vs Widget " << ( i + 1 ) << ": \"" << next->objectName().toStdString()
                      << "\" top=" << nextTop << " gap=" << gap );

            CHECK( gap >= 1 );
        }
    }

    dialog.close();
    QCoreApplication::processEvents();
}

namespace {
// A minimal ProcessLiveSourceTransport subclass that runs a long-lived process
// without ADB-specific argument decoration, for testing disconnect behavior.
class LongRunningTestTransport
    : public GenerationDrivenProcessTransport<ProcessLiveSourceTransport> {
public:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "ping" ),
                 { QStringLiteral( "-n" ),
                   QStringLiteral( "60" ),
                   QStringLiteral( "127.0.0.1" ) } };
#else
        return { QStringLiteral( "sleep" ), { QStringLiteral( "60" ) } };
#endif
    }

    Command clearCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ), { QStringLiteral( "/c" ), QStringLiteral( "echo" ) } };
#else
        return { QStringLiteral( "true" ), {} };
#endif
    }
};

class FiniteSuccessfulTestTransport
    : public GenerationDrivenProcessTransport<ProcessLiveSourceTransport> {
public:
    QString stderrFilePathForTest() const
    {
        return stderrFilePath();
    }

    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ),
                 { QStringLiteral( "/c" ),
                   QStringLiteral( "ping -n 4 127.0.0.1 > nul & exit /b 0" ) } };
#else
        return { QStringLiteral( "/bin/sh" ),
                 { QStringLiteral( "-c" ), QStringLiteral( "sleep 0.5; exit 0" ) } };
#endif
    }

    Command clearCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ), { QStringLiteral( "/c" ), QStringLiteral( "echo" ) } };
#else
        return { QStringLiteral( "true" ), {} };
#endif
    }
};

// Exits during startup after writing a recognizable line to stderr — exercises
// the generation-driven startup-failure error-capture path.
class StartupStderrFailureTransport
    : public GenerationDrivenProcessTransport<ProcessLiveSourceTransport> {
public:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ),
                 { QStringLiteral( "/c" ),
                   QStringLiteral( "echo startup-boom 1>&2 & exit /b 13" ) } };
#else
        return { QStringLiteral( "/bin/sh" ),
                 { QStringLiteral( "-c" ), QStringLiteral( "echo startup-boom >&2; exit 13" ) } };
#endif
    }

    Command clearCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ), { QStringLiteral( "/c" ), QStringLiteral( "echo" ) } };
#else
        return { QStringLiteral( "true" ), {} };
#endif
    }
};

class DeferredStartTestTransport
    : public GenerationDrivenProcessTransport<ProcessLiveSourceTransport> {
public:
    enum class Mode { LongRunning, StderrFailure };

    DeferredStartTestTransport( Mode mode, AsyncStartupTiming timing )
        : mode_( mode )
        , timing_( timing )
    {
    }

    Command streamingCommand() const override
    {
        if ( mode_ == Mode::StderrFailure ) {
#ifdef Q_OS_WIN
            return { QStringLiteral( "cmd" ),
                     { QStringLiteral( "/c" ),
                       QStringLiteral( "echo startup-boom 1>&2 & exit /b 13" ) } };
#else
            return { QStringLiteral( "/bin/sh" ),
                     { QStringLiteral( "-c" ),
                       QStringLiteral( "echo startup-boom >&2; exit 13" ) } };
#endif
        }

#ifdef Q_OS_WIN
        return { QStringLiteral( "ping" ),
                 { QStringLiteral( "-n" ),
                   QStringLiteral( "60" ),
                   QStringLiteral( "127.0.0.1" ) } };
#else
        return { QStringLiteral( "sleep" ), { QStringLiteral( "60" ) } };
#endif
    }

    Command clearCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ), { QStringLiteral( "/c" ), QStringLiteral( "echo" ) } };
#else
        return { QStringLiteral( "true" ), {} };
#endif
    }

    bool hasPendingStart() const
    {
        return pendingProcess_ != nullptr;
    }

    QProcess* pendingProcessForTest() const
    {
        return pendingProcess_;
    }

    QString stderrFilePathForTest() const
    {
        return stderrFilePath();
    }

    int startRequestCount() const
    {
        return startRequestCount_;
    }

    void releaseStart()
    {
        REQUIRE( pendingProcess_ != nullptr );
        auto* const process = pendingProcess_;
        pendingProcess_ = nullptr;
        process->start();
    }

protected:
    void startProcessAsync( QProcess& process ) override
    {
        pendingProcess_ = &process;
        ++startRequestCount_;
    }

    AsyncStartupTiming asyncStartupTiming() const override
    {
        return timing_;
    }

private:
    Mode mode_;
    AsyncStartupTiming timing_;
    QProcess* pendingProcess_ = nullptr;
    int startRequestCount_ = 0;
};
} // namespace

TEST_CASE( "ProcessLiveSourceTransport suppresses errorOccurred during intentional disconnect" )
{
    LongRunningTestTransport transport;

    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );

    // Connect -- the long-running process starts successfully
    REQUIRE( transport.startAndWait() );

    // Disconnect detaches the process and removes all transport signal
    // connections before terminating it.
    transport.stopCurrent();

    // Process events to let any queued signals through
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();

    // The detached process can no longer report its intentional termination.
    CHECK( errorSpy.count() == 0 );

    // The final state should be Disconnected (not Error)
    REQUIRE( stateSpy.count() >= 1 );
    const auto lastState
        = stateSpy.at( stateSpy.count() - 1 ).at( 1 ).value<LiveSourceTransport::State>();
    CHECK( lastState == LiveSourceTransport::State::Disconnected );
}

TEST_CASE( "ProcessLiveSourceTransport treats unexpected clean process exit as error" )
{
    FiniteSuccessfulTestTransport transport;

    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );

    REQUIRE( transport.startAndWait() );
    const auto failedCapturePath = transport.stderrFilePathForTest();
    REQUIRE( QFileInfo::exists( failedCapturePath ) );

    REQUIRE( errorSpy.safeWait( 3000 ) );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
    REQUIRE( transport.stderrFilePathForTest() != failedCapturePath );
    REQUIRE( QFileInfo::exists( transport.stderrFilePathForTest() ) );

    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    while ( QFileInfo::exists( failedCapturePath ) && cleanupTimer.elapsed() < 3000 ) {
        QCoreApplication::processEvents();
        QTest::qWait( 10 );
    }
    CHECK_FALSE( QFileInfo::exists( failedCapturePath ) );

    REQUIRE( stateSpy.count() >= 1 );
    const auto lastState
        = stateSpy.at( stateSpy.count() - 1 ).at( 1 ).value<LiveSourceTransport::State>();
    CHECK( lastState == LiveSourceTransport::State::Error );
}

TEST_CASE( "ProcessLiveSourceTransport async disconnect returns immediately" )
{
    LongRunningTestTransport transport;

    REQUIRE( transport.startAndWait() );

    QElapsedTimer timer;
    timer.start();
    transport.stopCurrent();
    const auto elapsed = timer.elapsed();

    // Disconnect should complete in well under 100ms (no blocking waitForFinished)
    CHECK( elapsed < 100 );

    // Process events to let async cleanup finish
    QCoreApplication::processEvents();
    QTest::qWait( 2000 );
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------------
// generation start — non-blocking startup detection (replaces the
// blocking waitForStarted + grace loop for the auto-reconnect path)
// ---------------------------------------------------------------------------

TEST_CASE( "ProcessLiveSourceTransport generation start returns with startup pending" )
{
    DeferredStartTestTransport transport( DeferredStartTestTransport::Mode::LongRunning,
                                          { 3000, 20 } );
    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );

    transport.startAsync();

    REQUIRE( transport.startRequestCount() == 1 );
    REQUIRE( transport.hasPendingStart() );
    REQUIRE( stateSpy.count() == 1 );
    CHECK( stateSpy.at( 0 ).at( 1 ).value<LiveSourceTransport::State>()
           == LiveSourceTransport::State::Connecting );
    CHECK( errorSpy.count() == 0 );

    transport.stopCurrent();
}

namespace {
bool spyContainsState( const SafeQSignalSpy& spy, LiveSourceTransport::State target )
{
    for ( int i = 0; i < spy.count(); ++i ) {
        if ( spy.at( i ).at( 1 ).value<LiveSourceTransport::State>() == target ) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE( "ProcessLiveSourceTransport starts grace only after QProcess started" )
{
    DeferredStartTestTransport transport( DeferredStartTestTransport::Mode::LongRunning,
                                          { 3000, 20 } );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );
    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    transport.startAsync();

    // Hold QProcess in the pre-start phase longer than the post-start grace.
    QTest::qWait( 60 );
    QCoreApplication::processEvents();
    CHECK_FALSE( spyContainsState( stateSpy, LiveSourceTransport::State::Connected ) );
    CHECK_FALSE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );
    CHECK( errorSpy.count() == 0 );

    transport.releaseStart();
    QElapsedTimer deadline;
    deadline.start();
    while ( !spyContainsState( stateSpy, LiveSourceTransport::State::Connected )
            && deadline.elapsed() < 3000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
    }
    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Connected ) );

    transport.stopCurrent();
}

TEST_CASE( "ProcessLiveSourceTransport times out while waiting for QProcess started" )
{
    DeferredStartTestTransport transport( DeferredStartTestTransport::Mode::LongRunning,
                                          { 30, 10 } );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );
    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    transport.startAsync();

    QElapsedTimer deadline;
    deadline.start();
    while ( !spyContainsState( stateSpy, LiveSourceTransport::State::Error )
            && deadline.elapsed() < 1000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
    }

    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );
    CHECK_FALSE( spyContainsState( stateSpy, LiveSourceTransport::State::Connected ) );
    REQUIRE( errorSpy.count() == 1 );
    CHECK( transport.lastError().contains( QStringLiteral( "Timed out" ) ) );
}

TEST_CASE( "ProcessLiveSourceTransport retires a timed-out process before reentrant reconnect" )
{
    DeferredStartTestTransport transport( DeferredStartTestTransport::Mode::LongRunning,
                                          { 30, 20 } );
    auto* firstProcess = static_cast<QProcess*>( nullptr );
    auto* secondProcess = static_cast<QProcess*>( nullptr );
    QString firstCapturePath;
    QString secondCapturePath;
    bool reconnected = false;
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );

    QObject::connect( &transport, &LiveSourceTransport::errorOccurred, &transport, [ & ] {
        if ( reconnected ) {
            return;
        }
        reconnected = true;
        transport.startAsync();
        secondProcess = transport.pendingProcessForTest();
        secondCapturePath = transport.stderrFilePathForTest();
    } );

    transport.startAsync();
    firstProcess = transport.pendingProcessForTest();
    firstCapturePath = transport.stderrFilePathForTest();
    REQUIRE( firstProcess != nullptr );
    REQUIRE( QFileInfo::exists( firstCapturePath ) );

    QElapsedTimer deadline;
    deadline.start();
    while ( !reconnected && deadline.elapsed() < 1000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
    }

    REQUIRE( reconnected );
    REQUIRE( secondProcess != nullptr );
    REQUIRE( secondProcess != firstProcess );
    REQUIRE( secondCapturePath != firstCapturePath );
    REQUIRE( QFileInfo::exists( secondCapturePath ) );

    REQUIRE( transport.startRequestCount() == 2 );
    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );
    REQUIRE( stateSpy.at( stateSpy.count() - 1 ).at( 1 ).value<LiveSourceTransport::State>()
             == LiveSourceTransport::State::Connecting );

    transport.stopCurrent();
}

TEST_CASE( "ProcessLiveSourceTransport generation start detects startup failure" )
{
    TestAdbProcessTransport transport( QStringLiteral( "/path/that/does/not/exist/adb" ),
                                       QStringLiteral( "serial" ), {} );

    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );
    transport.startAsync();

    // A non-existent executable triggers errorOccurred(FailedToStart) within
    // the next event loop cycle.
    QElapsedTimer deadline;
    deadline.start();
    while ( !spyContainsState( stateSpy, LiveSourceTransport::State::Error )
            && deadline.elapsed() < 2000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
    }
    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );
    REQUIRE( errorSpy.count() >= 1 );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
}

TEST_CASE( "ProcessLiveSourceTransport preserves stderr during post-start grace" )
{
    DeferredStartTestTransport transport( DeferredStartTestTransport::Mode::StderrFailure,
                                          { 3000, 1000 } );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::Generation,
                                                               LiveSourceTransport::State ) ) );
    SafeQSignalSpy errorSpy( &transport,
                             SIGNAL( errorOccurred( LiveSourceTransport::Generation, QString ) ) );
    transport.startAsync();

    QTest::qWait( 60 );
    QCoreApplication::processEvents();
    CHECK_FALSE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );

    transport.releaseStart();
    QElapsedTimer deadline;
    deadline.start();
    while ( !spyContainsState( stateSpy, LiveSourceTransport::State::Error )
            && deadline.elapsed() < 3000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
    }
    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );
    CHECK_FALSE( spyContainsState( stateSpy, LiveSourceTransport::State::Connected ) );
    REQUIRE( errorSpy.count() == 1 );
    REQUIRE( transport.lastError().contains( QStringLiteral( "startup-boom" ) ) );
    CHECK( errorSpy.at( 0 ).at( 1 ).toString().contains( QStringLiteral( "startup-boom" ) ) );

    transport.stopCurrent();
}

TEST_CASE( "ProcessLiveSourceTransport reconnects immediately after async disconnect" )
{
    LongRunningTestTransport transport;

    REQUIRE( transport.startAndWait() );
    transport.stopCurrent();

    // Immediately reconnect -- should work since createProcess() made a fresh QProcess
    REQUIRE( transport.startAndWait() );

    transport.stopCurrent();

    // Process events to let async cleanup finish
    QCoreApplication::processEvents();
    QTest::qWait( 2000 );
    QCoreApplication::processEvents();
}

TEST_CASE( "Live-source dialogs expose only typed built-in transport controls" )
{
    ScopedAdbConfigurationGuard configGuard;
    const auto emptyAdbDiscovery = []( klogg::livecapture::Generation generation ) {
        return DeviceDiscoveryResult<AdbDeviceInfo>{ generation, {}, std::nullopt };
    };
    const auto emptyIosDiscovery = []( klogg::livecapture::Generation generation ) {
        return DeviceDiscoveryResult<IosDeviceInfo>{ generation, {}, std::nullopt };
    };

    AdbLogcatDialog adbDialog( emptyAdbDiscovery );
    IosLogDialog iosDialog( emptyIosDiscovery );

    for ( auto* dialog : std::vector<QDialog*>{ &adbDialog, &iosDialog } ) {
        for ( auto* lineEdit : dialog->findChildren<QLineEdit*>() ) {
            INFO( "line edit object: " << lineEdit->objectName().toStdString() );
            CHECK( qobject_cast<QAbstractSpinBox*>( lineEdit->parentWidget() ) != nullptr );
        }
        CHECK( dialog->findChildren<QFileDialog*>().isEmpty() );
        CHECK( dialog->findChild<QObject*>( QStringLiteral( "adbExecutableEdit" ) ) == nullptr );
        CHECK( dialog->findChild<QObject*>( QStringLiteral( "extraArgsEdit" ) ) == nullptr );
        CHECK( dialog->findChild<QObject*>( QStringLiteral( "iosLogExecutableEdit" ) ) == nullptr );

        const auto formLayouts = dialog->findChildren<QFormLayout*>();
        REQUIRE( formLayouts.size() == 1 );
        auto* form = formLayouts.front();
        for ( int row = 0; row < form->rowCount(); ++row ) {
            // QLayoutItem::widget() is non-const on Qt 5; keep the item mutable.
            auto* labelItem = form->itemAt( row, QFormLayout::LabelRole );
            if ( labelItem == nullptr ) {
                continue;
            }
            const auto* label = qobject_cast<QLabel*>( labelItem->widget() );
            REQUIRE( label != nullptr );
            INFO( "row " << row << " label object " << label->objectName().toStdString() );
            CHECK_FALSE( label->text().trimmed().isEmpty() );
        }
    }

    auto* adbDeviceCombo = adbDialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    REQUIRE( adbDeviceCombo != nullptr );
    adbDeviceCombo->addItem( QStringLiteral( "Pixel 8 (ABC123)" ), QStringLiteral( "ABC123" ) );
    adbDeviceCombo->setCurrentIndex( 0 );

    const auto sessionData = adbDialog.sessionData();
    CHECK( sessionData.adbExecutable.isEmpty() );
    CHECK( sessionData.extraArgs.isEmpty() );
    CHECK( sessionData.adbBackend == AdbTransportBackend::SmartSocket );
    CHECK( sessionData.runIntent == klogg::livecapture::RunIntent::Running );
    CHECK( sessionData.deviceSerial == QStringLiteral( "ABC123" ) );
}

TEST_CASE( "Default live-source dialogs fail closed without legacy environment discovery" )
{
    AdbLogcatDialog adbDialog;
    CHECK_FALSE( hasFutureWatcherChild( adbDialog ) );

    IosLogDialog iosDialog;
    REQUIRE( QMetaObject::invokeMethod( &iosDialog, "refreshDevices", Qt::DirectConnection ) );
    CHECK_FALSE( hasFutureWatcherChild( iosDialog ) );
}

TEST_CASE( "iOS log stream session data serializes its source type" )
{
    AdbLogcatSessionData iosSessionData{
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QStringLiteral( "--network" ),
        QStringLiteral( "ios-capture" ),
        QStringLiteral( "/tmp/ios.log" ),
        LiveLogSourceType::IosLogStream,
        true,
    };
    iosSessionData.iosBackend = IosTransportBackend::LegacyProcess;

    const auto json = QString::fromUtf8(
        QJsonDocument( iosSessionData.toJson() ).toJson( QJsonDocument::Compact ) );
    const auto restored = AdbLogcatSessionData::fromJson( json );

    REQUIRE( restored.sourceType == LiveLogSourceType::IosLogStream );
    REQUIRE( restored.documentId() == QStringLiteral( "ios-log://ios-capture" ) );
    REQUIRE( restored.persistedSourceType() == QStringLiteral( "ios_log_stream" ) );
    REQUIRE( restored.deviceSerial == QStringLiteral( "00008030-001C195E36D8802E" ) );
    REQUIRE( restored.adbExecutable.isEmpty() );
    REQUIRE( restored.extraArgs.isEmpty() );
    REQUIRE_FALSE( QJsonDocument::fromJson( json.toUtf8() )
                       .object()
                       .contains( QStringLiteral( "adbExecutable" ) ) );
    REQUIRE_FALSE( QJsonDocument::fromJson( json.toUtf8() )
                       .object()
                       .contains( QStringLiteral( "extraArgs" ) ) );
    REQUIRE( restored.iosBackend == IosTransportBackend::LegacyProcess );
    REQUIRE( restored.ansiOutputEnabled );
}

TEST_CASE( "Missing or tampered legacy backend fields fail closed to built-in transports" )
{
    const auto missingAndroid = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"json({"sourceType":"adb_logcat","adbExecutable":"adb","deviceSerial":"SERIAL","captureId":"capture-android"})json" ) );
    CHECK( missingAndroid.adbBackend == AdbTransportBackend::SmartSocket );

    const auto tamperedAndroid = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"json({"sourceType":"adb_logcat","adbBackend":"future_backend","adbExecutable":"adb","deviceSerial":"SERIAL","captureId":"capture-android"})json" ) );
    CHECK( tamperedAndroid.adbBackend == AdbTransportBackend::SmartSocket );

    const auto missingIos = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"json({"sourceType":"ios_log_stream","adbExecutable":"pymobiledevice3","deviceSerial":"UDID","captureId":"capture-ios"})json" ) );
    CHECK( missingIos.iosBackend == IosTransportBackend::Native );

    const auto tamperedIos = AdbLogcatSessionData::fromJson( QStringLiteral(
        R"json({"sourceType":"ios_log_stream","iosBackend":"future_backend","adbExecutable":"pymobiledevice3","deviceSerial":"UDID","captureId":"capture-ios"})json" ) );
    CHECK( tamperedIos.iosBackend == IosTransportBackend::Native );

    DefaultLiveSourceTransportFactory factory;
    LiveSourceTransportConfig androidConfig;
    androidConfig.sourceType = missingAndroid.sourceType;
    androidConfig.adbBackend = missingAndroid.adbBackend;
    androidConfig.executable = missingAndroid.adbExecutable;
    androidConfig.deviceId = missingAndroid.deviceSerial;
    const auto transport = factory.create( androidConfig );
    REQUIRE( transport != nullptr );
    CHECK( dynamic_cast<klogg::livecapture::adb::AdbSmartSocketTransport*>( transport.get() )
           != nullptr );
}

TEST_CASE( "Unavailable iOS native transport reports a source-neutral error" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );
    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );

    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::IosLogStream;
    sessionData.iosBackend = IosTransportBackend::Native;
    sessionData.deviceSerial = QStringLiteral( "native-ios-device" );
    sessionData.captureId = captureId;

    AdbLogcatSource source( sessionData, logData );
    REQUIRE_FALSE( source.connectSource() );
    CHECK_FALSE( source.lastError().contains( QStringLiteral( "ADB" ), Qt::CaseInsensitive ) );
    CHECK( source.lastError().contains( QStringLiteral( "live log" ), Qt::CaseInsensitive ) );
}

TEST_CASE( "Controller-owned manual reconnect never starts a source-local generation" )
{
    class RecordingTransport final : public LiveSourceTransport {
    public:
        void start( Generation generation ) override
        {
            starts.push_back( generation );
        }

        void stop( Generation generation ) override
        {
            stops.push_back( generation );
        }

        void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override
        {
            clearGeneration = generation;
            clearRequestId = requestId;
        }
        QString lastError() const override { return {}; }

        void publishConnected( Generation generation )
        {
            Q_EMIT stateChanged( generation, State::Connected );
        }

        void finishClear()
        {
            REQUIRE( clearGeneration.has_value() );
            REQUIRE( clearRequestId.has_value() );
            Q_EMIT clearRemoteFinished( *clearGeneration, *clearRequestId, true, QString{} );
        }

        std::vector<Generation> starts;
        std::vector<Generation> stops;
        std::optional<Generation> clearGeneration;
        std::optional<ClearRequestId> clearRequestId;
    };

    class RecordingFactory final : public LiveSourceTransportFactory {
    public:
        std::unique_ptr<LiveSourceTransport>
        create( const LiveSourceTransportConfig& config ) const override
        {
            requestedConfigs.push_back( config );
            auto transport = std::make_unique<RecordingTransport>();
            created = transport.get();
            return transport;
        }

        mutable RecordingTransport* created{ nullptr };
        mutable std::vector<LiveSourceTransportConfig> requestedConfigs;
    };

    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );
    auto logData = std::make_shared<StreamingLogData>( makeCaptureId(), tempDir.path() );
    AdbLogcatSessionData sessionData;
    sessionData.captureId = makeCaptureId();
    sessionData.deviceSerial = QStringLiteral( "SERIAL" );
    RecordingFactory factory;
    AdbLogcatSource source( sessionData, logData, factory );
    CHECK( factory.created == nullptr );

    int controllerRestarts = 0;
    source.setControllerCallbacks( {}, {}, {}, {}, [ &controllerRestarts ] {
        ++controllerRestarts;
    } );

    LiveSourceTransportConfig config;
    config.deviceId = sessionData.deviceSerial;
    source.openTransport( 41u, config );
    REQUIRE( factory.created != nullptr );
    factory.created->publishConnected( 41u );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( factory.created->starts == std::vector<LiveSourceTransport::Generation>{ 41u } );

    REQUIRE( source.reconnectSource() );

    CHECK( controllerRestarts == 1 );
    CHECK( factory.created->starts == std::vector<LiveSourceTransport::Generation>{ 41u } );
    CHECK( factory.created->stops.empty() );

    int controllerStops = 0;
    controllerRestarts = 0;
    source.setControllerCallbacks( {}, {}, {}, [ &controllerStops ] { ++controllerStops; },
                                   [ &controllerRestarts ] { ++controllerRestarts; } );
    REQUIRE( source.clearAndRestart() );
    REQUIRE( controllerStops == 1 );
    REQUIRE( factory.created->clearGeneration.has_value() );

    REQUIRE( source.reconnectSource() );
    CHECK( controllerRestarts == 0 );
    CHECK( factory.created->starts == std::vector<LiveSourceTransport::Generation>{ 41u } );

    factory.created->finishClear();
    CHECK( controllerRestarts == 1 );

    auto nextConfig = config;
    nextConfig.ansiOutputEnabled = true;
    nextConfig.androidBuffers = QStringList{ QStringLiteral( "system" ) };
    source.openTransport( 42u, nextConfig );
    REQUIRE( factory.requestedConfigs.size() == 2u );
    CHECK_FALSE( factory.requestedConfigs.front().ansiOutputEnabled );
    CHECK( factory.requestedConfigs.back().ansiOutputEnabled );
    CHECK( factory.requestedConfigs.back().androidBuffers
           == QStringList{ QStringLiteral( "system" ) } );
    REQUIRE( factory.created != nullptr );
    CHECK( factory.created->starts == std::vector<LiveSourceTransport::Generation>{ 42u } );
}

TEST_CASE( "iOS log stream display name defaults to device name only" )
{
    const AdbLogcatSessionData iosSessionData{
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008150-001431410C78401C" ),
        QStringLiteral( "ZEACENT's iPhone 00008150-001431410C78401C iPhone18,3 26.5" ),
        QString{},
        QStringLiteral( "ios-capture" ),
        QString{},
        LiveLogSourceType::IosLogStream,
    };

    REQUIRE( iosSessionData.displayName() == QStringLiteral( "ZEACENT's iPhone" ) );
}

TEST_CASE( "AdbLogcatSource clears and restarts iOS log streams without remote clear" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based iOS stream restart test on Windows." );
    return;
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto scriptPath = tempDir.filePath( QStringLiteral( "pymobiledevice3" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\n"
                  "i=1\n"
                  "while :; do\n"
                  "  echo ios-live-line-$i\n"
                  "  i=$((i + 1))\n"
                  "  sleep 0.05\n"
                  "done\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::IosLogStream,
    };
    sessionData.iosBackend = IosTransportBackend::LegacyProcess;

    AdbLogcatSource source( sessionData, logData );

    REQUIRE( source.connectSource() );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Connected ) );
    REQUIRE( waitForLineCount( logData, 1 ) );

    REQUIRE( source.clearAndRestart() );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Connected ) );
    REQUIRE( source.lastError().isEmpty() );
    REQUIRE( waitForLineCount( logData, 1 ) );

    source.disconnectSource();
    drainLiveSourceEvents( 200 );
#endif
}

TEST_CASE( "AdbLogcatSource clears disconnected ADB capture without waiting for remote clear" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based ADB disconnect clear test on Windows." );
    return;
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto scriptPath = tempDir.filePath( QStringLiteral( "adb" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\n"
                  "case \"$*\" in\n"
                  "  *'logcat -c'*) sleep 10; exit 1 ;;\n"
                  "esac\n"
                  "echo adb-live-line-before-unplug\n"
                  "sleep 0.4\n"
                  "echo 'device disconnected' >&2\n"
                  "exit 17\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    const AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Pixel Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };

    AdbLogcatSource source( sessionData, logData );

    REQUIRE( source.connectSource() );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Connected ) );
    REQUIRE( waitForLineCount( logData, 1 ) );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );

    QElapsedTimer clearTimer;
    clearTimer.start();
    REQUIRE( source.clearAndRestart() );
    REQUIRE( clearTimer.elapsed() < 2000 );
    REQUIRE( logData->getNbLine().get() == 0 );

    source.disconnectSource();
    drainLiveSourceEvents( 200 );
#endif
}

TEST_CASE( "AdbLogcatSource clears connected ADB capture even when remote clear fails" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based ADB remote clear failure test on Windows." );
    return;
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto scriptPath = tempDir.filePath( QStringLiteral( "adb" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\n"
                  "case \"$*\" in\n"
                  "  *'logcat -c'*) echo 'device disconnected during clear' >&2; exit 17 ;;\n"
                  "esac\n"
                  "echo adb-live-line-before-clear\n"
                  "while :; do sleep 1; done\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    const AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Pixel Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };

    AdbLogcatSource source( sessionData, logData );

    REQUIRE( source.connectSource() );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Connected ) );
    REQUIRE( waitForLineCount( logData, 1 ) );

    REQUIRE( source.clearAndRestart() );
    REQUIRE( logData->getNbLine().get() == 0 );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );
    REQUIRE( source.lastError().contains( QStringLiteral( "device disconnected during clear" ) ) );

    source.disconnectSource();
    drainLiveSourceEvents( 200 );
#endif
}

TEST_CASE( "AdbLogcatSource clears iOS log stream capture even when restart cannot reconnect" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based iOS stream restart failure test on Windows." );
    return;
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto unavailableMarker = tempDir.filePath( QStringLiteral( "device-unavailable" ) );
    const auto scriptPath = tempDir.filePath( QStringLiteral( "pymobiledevice3" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( QStringLiteral( "#!/bin/sh\n"
                                  "MARKER='%1'\n"
                                  "if [ -f \"$MARKER\" ]; then\n"
                                  "  echo 'No iOS device connected' >&2\n"
                                  "  exit 17\n"
                                  "fi\n"
                                  "echo ios-live-line-before-unplug\n"
                                  "while :; do sleep 1; done\n" )
                      .arg( unavailableMarker )
                      .toUtf8() );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::IosLogStream,
    };
    sessionData.iosBackend = IosTransportBackend::LegacyProcess;

    AdbLogcatSource source( sessionData, logData );

    REQUIRE( source.connectSource() );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Connected ) );
    REQUIRE( waitForLineCount( logData, 1 ) );

    QFile marker( unavailableMarker );
    REQUIRE( marker.open( QIODevice::WriteOnly | QIODevice::Text ) );
    marker.write( "unavailable\n" );
    marker.close();

    REQUIRE( source.clearAndRestart() );
    REQUIRE( logData->getNbLine().get() == 0 );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );
    REQUIRE_FALSE( source.lastError().isEmpty() );

    source.disconnectSource();
    drainLiveSourceEvents( 200 );
#endif
}

// ----------------------------------------------------------------------------
// macOS GUI-launch reproduction:
//
// When klogg.app is launched from Finder/Dock/Spotlight, it inherits the
// launchd GUI session environment.  That environment does NOT include
// /usr/local/bin (Homebrew Intel), /opt/homebrew/bin (Homebrew Apple Silicon),
// or ~/Library/Android/sdk/platform-tools (Android SDK default).
// `/etc/paths` is consumed by path_helper(1), which only runs in shell rc
// files -- it has no effect on GUI-spawned processes.
//
// Result: when the user has not configured an explicit adb path, klogg ends
// up calling QProcess::start("adb", ...) with a PATH that does not contain
// adb anywhere, and the process fails to start.  Live streaming never
// connects.  Windows is unaffected because Android Studio/SDK Manager adds
// platform-tools to System PATH, which IS inherited by GUI apps.
//
// The fix: when no explicit adb is configured, probe the well-known install
// locations on disk and use the absolute path of whichever one exists.
// Falling back to bare "adb" only as a last resort preserves Linux/Windows
// behavior on hosts where adb is reachable via PATH.
// ----------------------------------------------------------------------------

TEST_CASE( "AdbProcessTransport preserves a user-configured adb executable verbatim" )
{
    TestAdbProcessTransport transport( QStringLiteral( "/explicit/path/to/adb" ),
                                       QStringLiteral( "serial" ), {} );
    REQUIRE( transport.streamingCommandForTest().program
             == QStringLiteral( "/explicit/path/to/adb" ) );
}

TEST_CASE( "AdbProcessTransport resolves to an absolute path when adb is installed at a "
           "well-known location and the user has not configured one" )
{
    static const QStringList knownAdbInstallLocations{
        QStringLiteral( "/usr/local/bin/adb" ),
        QStringLiteral( "/opt/homebrew/bin/adb" ),
        QDir::homePath() + QStringLiteral( "/Library/Android/sdk/platform-tools/adb" ),
    };

    QString availableLocation;
    for ( const auto& candidate : knownAdbInstallLocations ) {
        if ( QFile::exists( candidate ) && QFileInfo( candidate ).isExecutable() ) {
            availableLocation = candidate;
            break;
        }
    }

    if ( availableLocation.isEmpty() ) {
        WARN( "No adb installed at a well-known location -- skipping GUI-launch repro." );
        return;
    }

    TestAdbProcessTransport transport( QString{}, QStringLiteral( "emulator-5554" ), {} );
    const auto streaming = transport.streamingCommandForTest();

    INFO( "Resolved adb program: " << streaming.program.toStdString() );
    INFO( "Available installed adb at: " << availableLocation.toStdString() );

    // Bare "adb" silently fails when klogg.app is started from Finder/Dock,
    // because the inherited launchd PATH does not contain adb's directory.
    REQUIRE( QFileInfo( streaming.program ).isAbsolute() );
    REQUIRE( QFile::exists( streaming.program ) );
    REQUIRE( QFileInfo( streaming.program ).isExecutable() );
}

namespace {
class StreamingScriptTransport
    : public GenerationDrivenProcessTransport<ProcessLiveSourceTransport> {
public:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ),
                 { QStringLiteral( "/c" ),
                   QStringLiteral( "for /L %i in (1,1,5) do @(echo line %i & ping -n 1 -w 50 "
                                   "127.0.0.1 > nul)" ) } };
#else
        return {
            QStringLiteral( "/bin/sh" ),
            { QStringLiteral( "-c" ),
              QStringLiteral(
                  "i=1; while [ $i -le 5 ]; do echo line $i; i=$((i+1)); sleep 0.05; done" ) }
        };
#endif
    }

    Command clearCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ), { QStringLiteral( "/c" ), QStringLiteral( "echo" ) } };
#else
        return { QStringLiteral( "true" ), {} };
#endif
    }
};
} // namespace

TEST_CASE( "ProcessLiveSourceTransport delivers every line of a slow streaming process via "
           "bytesReceived" )
{
    StreamingScriptTransport transport;
    QByteArray accumulated;
    QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                      [ &accumulated ]( LiveSourceTransport::Generation, const QByteArray& data ) {
                          accumulated += data;
                      } );

    KLOGG_REQUIRE_OR_WARN_SKIP(
        transport.startAndWait(),
        "StreamingScriptTransport: transport start failed in this environment "
        "(observed on GitHub-hosted Windows runners; the streaming-pipeline "
        "behaviour itself is exercised on macOS / Linux runners)" );

    QElapsedTimer deadline;
    deadline.start();
    while ( accumulated.count( '\n' ) < 5 && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        QTest::qWait( 50 );
    }

    INFO( "Accumulated bytes: " << accumulated.toStdString() );
    CHECK( accumulated.contains( QByteArrayLiteral( "line 1" ) ) );
    CHECK( accumulated.contains( QByteArrayLiteral( "line 5" ) ) );
    CHECK( accumulated.count( '\n' ) == 5 );

    transport.stopCurrent();
    QCoreApplication::processEvents();
    QTest::qWait( 1500 );
    QCoreApplication::processEvents();
}

TEST_CASE( "expandTildePath expands bare tilde to home directory" )
{
    const auto homeDir = QStandardPaths::writableLocation( QStandardPaths::HomeLocation );
    REQUIRE( ui::internal::expandTildePath( QStringLiteral( "~" ) ) == homeDir );
}

TEST_CASE( "expandTildePath expands tilde-slash paths" )
{
    const auto homeDir = QStandardPaths::writableLocation( QStandardPaths::HomeLocation );
    REQUIRE( ui::internal::expandTildePath( QStringLiteral( "~/bin/tool" ) )
             == homeDir + QStringLiteral( "/bin/tool" ) );
}

TEST_CASE( "expandTildePath does not expand tilde-user syntax" )
{
    const auto input = QStringLiteral( "~otheruser/bin" );
    REQUIRE( ui::internal::expandTildePath( input ) == input );
}

TEST_CASE( "expandTildePath leaves non-tilde paths unchanged" )
{
    REQUIRE( ui::internal::expandTildePath( QStringLiteral( "/opt/homebrew/bin/tool" ) )
             == QStringLiteral( "/opt/homebrew/bin/tool" ) );
    REQUIRE( ui::internal::expandTildePath( QString{} ) == QString{} );
}

TEST_CASE( "IosLogProcessTransport expands tilde in user-configured executable" )
{
    TestIosLogProcessTransport transport(
        QStringLiteral( "~/Library/Python/3.9/bin/pymobiledevice3" ),
        QStringLiteral( "DEVICE_UDID" ), {} );
    const auto streaming = transport.streamingCommandForTest();
    REQUIRE_FALSE( streaming.program.startsWith( QLatin1Char( '~' ) ) );
    REQUIRE(
        streaming.program.contains( QStringLiteral( "Library/Python/3.9/bin/pymobiledevice3" ) ) );
}

TEST_CASE( "AdbProcessTransport expands tilde in user-configured executable" )
{
    TestAdbProcessTransport transport( QStringLiteral( "~/android/sdk/platform-tools/adb" ),
                                       QStringLiteral( "emulator-5554" ), {} );
    const auto streaming = transport.streamingCommandForTest();
    REQUIRE_FALSE( streaming.program.startsWith( QLatin1Char( '~' ) ) );
    REQUIRE( streaming.program.contains( QStringLiteral( "android/sdk/platform-tools/adb" ) ) );
}

// ---------------------------------------------------------------------------
// Live source auto-reconnect and rolling file settings tests
// ---------------------------------------------------------------------------

TEST_CASE( "OptionsDialog loads and persists live source auto-reconnect and rolling file settings" )
{
    if ( isHeadlessDialogTestEnvironment() ) {
        WARN( "OptionsDialog UI coverage is skipped on headless/offscreen platforms" );
        return;
    }

    ScopedOptionsDialogConfigurationGuard configGuard;
    auto& config = Configuration::getSynced();

    // Set non-default values
    config.setLiveAutoReconnectEnabled( true );
    config.setLiveAutoReconnectMaxAttempts( 10 );
    config.setLiveCaptureRollingMaxFileSize( 1048576 ); // 1 MB in bytes
    config.setLiveCaptureRollingBackupCount( 5 );
    config.save();

    OptionsDialog dialog;
    auto* autoReconnectCheckBox
        = dialog.findChild<QCheckBox*>( QStringLiteral( "liveSourceAutoReconnectCheckBox" ) );
    auto* maxAttemptsSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceMaxAttemptsSpinBox" ) );
    auto* maxFileSizeSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceRollingMaxFileSizeSpinBox" ) );
    auto* backupCountSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceRollingBackupCountSpinBox" ) );

    REQUIRE( autoReconnectCheckBox != nullptr );
    REQUIRE( maxAttemptsSpinBox != nullptr );
    REQUIRE( maxFileSizeSpinBox != nullptr );
    REQUIRE( backupCountSpinBox != nullptr );

    // Verify initial values loaded from config
    CHECK( autoReconnectCheckBox->isChecked() );
    CHECK( maxAttemptsSpinBox->value() == 10 );
    // UI stores MB, config stores bytes: 1048576 bytes = 1 MB
    CHECK( maxFileSizeSpinBox->value() == 1 );
    CHECK( backupCountSpinBox->value() == 5 );

    // Edit values
    autoReconnectCheckBox->setChecked( false );
    maxAttemptsSpinBox->setValue( 20 );
    maxFileSizeSpinBox->setValue( 50 ); // 50 MB
    backupCountSpinBox->setValue( 10 );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );

    auto& restoredConfig = Configuration::getSynced();
    CHECK( restoredConfig.liveAutoReconnectEnabled() == false );
    CHECK( restoredConfig.liveAutoReconnectMaxAttempts() == 20 );
    // 50 MB = 52428800 bytes
    CHECK( restoredConfig.liveCaptureRollingMaxFileSize() == 50 * 1024 * 1024 );
    CHECK( restoredConfig.liveCaptureRollingBackupCount() == 10 );
}

TEST_CASE( "OptionsDialog reset restores live source settings to defaults" )
{
    if ( isHeadlessDialogTestEnvironment() ) {
        WARN( "OptionsDialog UI coverage is skipped on headless/offscreen platforms" );
        return;
    }

    ScopedOptionsDialogConfigurationGuard configGuard;
    auto& config = Configuration::getSynced();

    // Set non-default values
    config.setLiveAutoReconnectEnabled( false );
    config.setLiveAutoReconnectMaxAttempts( 99 );
    config.setLiveCaptureRollingMaxFileSize( 999 * 1024 * 1024 );
    config.setLiveCaptureRollingBackupCount( 50 );
    config.save();

    OptionsDialog dialog;
    auto* resetButton
        = dialog.findChild<QPushButton*>( QStringLiteral( "resetFileDefaultsButton" ) );
    REQUIRE( resetButton != nullptr );

    // Click the reset button
    resetButton->click();
    QCoreApplication::processEvents();

    // Verify widgets show defaults
    auto* autoReconnectCheckBox
        = dialog.findChild<QCheckBox*>( QStringLiteral( "liveSourceAutoReconnectCheckBox" ) );
    auto* maxAttemptsSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceMaxAttemptsSpinBox" ) );
    auto* maxFileSizeSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceRollingMaxFileSizeSpinBox" ) );
    auto* backupCountSpinBox
        = dialog.findChild<QSpinBox*>( QStringLiteral( "liveSourceRollingBackupCountSpinBox" ) );

    REQUIRE( autoReconnectCheckBox != nullptr );
    REQUIRE( maxAttemptsSpinBox != nullptr );
    REQUIRE( maxFileSizeSpinBox != nullptr );
    REQUIRE( backupCountSpinBox != nullptr );

    const Configuration defaults;
    CHECK( autoReconnectCheckBox->isChecked() == defaults.liveAutoReconnectEnabled() );
    CHECK( maxAttemptsSpinBox->value() == defaults.liveAutoReconnectMaxAttempts() );
    CHECK( maxFileSizeSpinBox->value()
           == static_cast<int>( defaults.liveCaptureRollingMaxFileSize() / ( 1024 * 1024 ) ) );
    CHECK( backupCountSpinBox->value() == defaults.liveCaptureRollingBackupCount() );

    // Apply the reset values to config
    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );

    auto& restoredConfig = Configuration::getSynced();
    CHECK( restoredConfig.liveAutoReconnectEnabled() == defaults.liveAutoReconnectEnabled() );
    CHECK( restoredConfig.liveAutoReconnectMaxAttempts()
           == defaults.liveAutoReconnectMaxAttempts() );
    CHECK( restoredConfig.liveCaptureRollingMaxFileSize()
           == defaults.liveCaptureRollingMaxFileSize() );
    CHECK( restoredConfig.liveCaptureRollingBackupCount()
           == defaults.liveCaptureRollingBackupCount() );
}

TEST_CASE( "ProcessLiveSourceTransport surfaces real stderr on startup failure" )
{
    StartupStderrFailureTransport transport;

    REQUIRE_FALSE( transport.startAndWait() );
    // stderr is redirected to a file via setStandardErrorFile(); the startup
    // path must read that file (not readAllStandardError(), which is empty).
    REQUIRE( transport.lastError().contains( QStringLiteral( "startup-boom" ) ) );

    transport.stopCurrent();
    drainLiveSourceEvents( 200 );
}

TEST_CASE( "DeviceListProvider async enumeration snapshots work before provider destruction" )
{
    auto state = std::make_shared<DeviceListTaskState>();
    auto provider = std::make_unique<SnapshotDeviceListProvider>( state );
    auto future = provider->listDevicesAsync();

    REQUIRE( state->taskEntered.tryAcquire( 1, 3000 ) );
    state->result = QStringLiteral( "changed-after-dispatch" );
    provider.reset();
    state->allowTaskToFinish.release();
    future.waitForFinished();

    REQUIRE( future.resultCount() == 1 );
    REQUIRE( future.result() == QList<QString>{ QStringLiteral( "snapshot" ) } );
}

TEST_CASE( "AdbLogcatSessionData round-trips the bound output file and ANSI save mode",
           "[live-save-restore]" )
{
    AdbLogcatSessionData data;
    data.captureId = QStringLiteral( "cap-1234" );
    data.boundOutputFile = QStringLiteral( "/tmp/saved.log" );
    data.outputAnsiMode = LiveLogSaveAnsiMode::Preserve;

    const auto json
        = QString::fromUtf8( QJsonDocument( data.toJson() ).toJson( QJsonDocument::Compact ) );
    const auto restored = AdbLogcatSessionData::fromJson( json );

    REQUIRE( restored.captureId == QStringLiteral( "cap-1234" ) );
    REQUIRE( restored.boundOutputFile == QStringLiteral( "/tmp/saved.log" ) );
    REQUIRE( restored.outputAnsiMode == LiveLogSaveAnsiMode::Preserve );

    // A session serialized by an older klogg (before the mode was persisted)
    // must default to Strip so restore reopens in the historical default mode.
    const auto legacyJson
        = QStringLiteral( R"({"captureId":"cap-old","boundOutputFile":"/tmp/old.log"})" );
    const auto legacy = AdbLogcatSessionData::fromJson( legacyJson );
    REQUIRE( legacy.boundOutputFile == QStringLiteral( "/tmp/old.log" ) );
    REQUIRE( legacy.outputAnsiMode == LiveLogSaveAnsiMode::Strip );

    REQUIRE( restored.isValid() );
    for ( const auto& malformedCaptureId :
          QStringList{ QString{}, QStringLiteral( "." ), QStringLiteral( ".." ),
                       QStringLiteral( "../outside" ), QStringLiteral( "nested/capture" ),
                       QStringLiteral( "nested\\capture" ), QStringLiteral( "capture:stream" ),
                       QStringLiteral( "CON" ), QStringLiteral( "nul.txt" ),
                       QStringLiteral( "COM1" ), QStringLiteral( "LPT9.log" ),
                       QStringLiteral( "capture." ), QStringLiteral( "capture " ) } ) {
        auto malformed = restored;
        malformed.captureId = malformedCaptureId;
        INFO( "capture id: " << malformedCaptureId.toStdString() );
        REQUIRE_FALSE( malformed.isValid() );
    }
}
