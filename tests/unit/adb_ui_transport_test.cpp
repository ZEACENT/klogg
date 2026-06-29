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

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QGroupBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QUuid>
#include <QWidget>

#include <map>

#include "adbprocesstransport.h"
#include "adbdevicelistprovider.h"
#include "adblogcatsource.h"
#include "adblogcatdialog.h"
#include "commandargumenttokenizer.h"
#include "configuration.h"
#include "ioslogprocesstransport.h"
#include "livesourcetransport.h"
#include "optionsdialog.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "streaminglogdata.h"
#include "shortcuts.h"
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
        , executable_( config_.adbExecutable() )
        , extraArgs_( config_.adbLogcatExtraArgs() )
        , ansiOutput_( config_.adbLogcatAnsiOutputEnabled() )
    {
    }

    ~ScopedAdbConfigurationGuard()
    {
        config_.setAdbExecutable( executable_ );
        config_.setAdbLogcatExtraArgs( extraArgs_ );
        config_.setAdbLogcatAnsiOutputEnabled( ansiOutput_ );
        config_.save();
    }

  private:
    Configuration& config_;
    QString executable_;
    QString extraArgs_;
    bool ansiOutput_;
};

class ScopedOptionsDialogConfigurationGuard {
  public:
    ScopedOptionsDialogConfigurationGuard()
        : config_( Configuration::getSynced() )
        , savedSearches_( SavedSearches::getSynced() )
        , recentFiles_( RecentFiles::getSynced() )
    {
        REQUIRE( snapshotDir_.isValid() );
        snapshotPath_ = snapshotDir_.filePath( QStringLiteral( "settings.ini" ) );

        QSettings snapshot( snapshotPath_, QSettings::IniFormat );
        config_.saveToStorage( snapshot );
        savedSearches_.saveToStorage( snapshot );
        recentFiles_.saveToStorage( snapshot );
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
    }

  private:
    Configuration& config_;
    SavedSearches& savedSearches_;
    RecentFiles& recentFiles_;
    QTemporaryDir snapshotDir_;
    QString snapshotPath_;
};

class TestAdbProcessTransport : public AdbProcessTransport {
  public:
    using AdbProcessTransport::AdbProcessTransport;
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

class ImmediateFailureAdbProcessTransport : public AdbProcessTransport {
  public:
    ImmediateFailureAdbProcessTransport()
        : AdbProcessTransport( QString{}, QStringLiteral( "serial-123" ), {} )
    {
    }

  protected:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return Command{ QStringLiteral( "cmd" ),
                        { QStringLiteral( "/c" ), QStringLiteral( "exit" ),
                          QStringLiteral( "/b" ), QStringLiteral( "7" ) } };
#else
        return Command{ QStringLiteral( "/bin/sh" ),
                        { QStringLiteral( "-c" ), QStringLiteral( "exit 7" ) } };
#endif
    }
};

class TestIosLogProcessTransport : public IosLogProcessTransport {
  public:
    using IosLogProcessTransport::IosLogProcessTransport;
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

bool waitForLineCount( const std::shared_ptr<StreamingLogData>& logData, unsigned long long lineCount )
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
} // namespace

TEST_CASE( "AdbProcessTransport builds normalized streaming and clear commands" )
{
    TestAdbProcessTransport transport( QString{}, QStringLiteral( "emulator-5554" ),
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
                             QStringLiteral( "C:\\temp\\log.txt" ),
                             QStringLiteral( "--pattern" ), QStringLiteral( "regex\\d+" ),
                             QStringLiteral( "--title" ), QStringLiteral( "hello world" ) } );
}

TEST_CASE( "AdbProcessTransport preserves empty quoted extra args" )
{
    TestAdbProcessTransport transport( QString{}, QStringLiteral( "serial-123" ),
                                       QStringLiteral( "--empty '' --quoted \"\"" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "serial-123" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "--empty" ),
                             QString{}, QStringLiteral( "--quoted" ), QString{} } );
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
    TestIosLogProcessTransport transport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "--match \"process name\"" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.program == QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ) );
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "--no-color" ), QStringLiteral( "syslog" ),
                             QStringLiteral( "live" ),
                             QStringLiteral( "--udid" ),
                             QStringLiteral( "00008030-001C195E36D8802E" ),
                             QStringLiteral( "--match" ),
                             QStringLiteral( "process name" ) } );
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
    TestIosLogProcessTransport transport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
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
    TestIosLogProcessTransport transport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{}, true );

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
        TestIosLogProcessTransport colorTransport( scriptPath,
                                                   QStringLiteral( "DEVICE_UDID" ), QString{}, true );
        SafeQSignalSpy bytesSpy( &colorTransport, SIGNAL( bytesReceived( QByteArray ) ) );

        REQUIRE( colorTransport.connectTransport() );

        QByteArray accumulated;
        QElapsedTimer deadline;
        deadline.start();
        while ( deadline.elapsed() < 3000 ) {
            QCoreApplication::processEvents();
            QTest::qWait( 50 );
            if ( bytesSpy.count() > 0 ) {
                for ( int i = 0; i < bytesSpy.count(); ++i ) {
                    accumulated += bytesSpy.at( i ).at( 0 ).toByteArray();
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

        colorTransport.disconnectTransport();
        QCoreApplication::processEvents();
        QTest::qWait( 500 );
        QCoreApplication::processEvents();
    }

    // Without ANSI (no script wrapper), the process should NOT emit ANSI codes.
    {
        TestIosLogProcessTransport plainTransport( scriptPath,
                                                   QStringLiteral( "DEVICE_UDID" ), QString{}, false );
        SafeQSignalSpy bytesSpy( &plainTransport, SIGNAL( bytesReceived( QByteArray ) ) );

        REQUIRE( plainTransport.connectTransport() );

        QByteArray accumulated;
        QElapsedTimer deadline;
        deadline.start();
        while ( deadline.elapsed() < 3000 ) {
            QCoreApplication::processEvents();
            QTest::qWait( 50 );
            if ( bytesSpy.count() > 0 ) {
                for ( int i = 0; i < bytesSpy.count(); ++i ) {
                    accumulated += bytesSpy.at( i ).at( 0 ).toByteArray();
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

        plainTransport.disconnectTransport();
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
    TestIosLogProcessTransport transport(
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ), QString{} );

    const auto clear = transport.clearCommandForTest();
#ifdef Q_OS_WIN
    REQUIRE( clear.program == QStringLiteral( "cmd" ) );
    REQUIRE( clear.arguments == QStringList{ QStringLiteral( "/c" ), QStringLiteral( "exit" ),
                                             QStringLiteral( "0" ) } );
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
    SafeQSignalSpy errorSpy( &transport, SIGNAL( errorOccurred( QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );

    REQUIRE_FALSE( transport.connectTransport() );
    REQUIRE( errorSpy.safeWait() );
    REQUIRE( stateSpy.count() >= 1 );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
}

TEST_CASE( "AdbProcessTransport listDevices returns an error when adb cannot start" )
{
    QString error;
    const auto devices
        = AdbProcessTransport::listDevices( QStringLiteral( "/path/that/does/not/exist/adb" ),
                                            &error );

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
    const auto staticDevices
        = AdbProcessTransport::listDevices( QStringLiteral( "/path/that/does/not/exist/adb" ),
                                            &staticError );

    REQUIRE( devices.isEmpty() );
    REQUIRE( staticDevices.isEmpty() );
    REQUIRE_FALSE( providerError.isEmpty() );
    REQUIRE_FALSE( staticError.isEmpty() );
}

TEST_CASE( "AdbDeviceListProvider isDeviceAvailable returns true on subprocess error" )
{
    // When the list command itself fails, isDeviceAvailable should return true
    // (optimistic fallback — let connectTransport handle the real error).
    AdbDeviceListProvider provider( QStringLiteral( "/nonexistent/adb" ) );
    CHECK( provider.isDeviceAvailable( QStringLiteral( "any-serial" ) ) );
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

TEST_CASE( "IosDeviceListProvider isDeviceAvailable returns true on subprocess error" )
{
    IosDeviceListProvider provider( QStringLiteral( "/nonexistent/pymobiledevice3" ) );
    CHECK( provider.isDeviceAvailable( QStringLiteral( "any-udid" ) ) );
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
    SafeQSignalSpy errorSpy( &transport, SIGNAL( errorOccurred( QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );

    REQUIRE_FALSE( transport.connectTransport() );
    REQUIRE( errorSpy.safeWait() );
    REQUIRE( stateSpy.count() >= 1 );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
}

TEST_CASE( "OptionsDialog loads and persists adb settings" )
{
    if ( skipHeadlessOptionsDialogTest() ) {
        return;
    }

    ScopedAdbConfigurationGuard configGuard;
    auto& savedSearches = SavedSearches::getSynced();
    auto& recentFiles = RecentFiles::getSynced();
    Q_UNUSED( savedSearches );
    Q_UNUSED( recentFiles );
    auto& config = Configuration::getSynced();
    config.setAdbExecutable( QStringLiteral( "/initial/adb" ) );
    config.setAdbLogcatExtraArgs( QStringLiteral( "-v brief" ) );
    config.save();

    OptionsDialog dialog;
    auto* adbExecutableLineEdit = dialog.findChild<QLineEdit*>( QStringLiteral( "adbExecutableLineEdit" ) );
    auto* adbLogcatArgsLineEdit = dialog.findChild<QLineEdit*>( QStringLiteral( "adbLogcatArgsLineEdit" ) );

    REQUIRE( adbExecutableLineEdit != nullptr );
    REQUIRE( adbLogcatArgsLineEdit != nullptr );
    REQUIRE( adbExecutableLineEdit->text() == QStringLiteral( "/initial/adb" ) );
    REQUIRE( adbLogcatArgsLineEdit->text() == QStringLiteral( "-v brief" ) );

    adbExecutableLineEdit->setText( QStringLiteral( "/updated/adb" ) );
    adbLogcatArgsLineEdit->setText( QStringLiteral( "-v threadtime ActivityManager:I *:S" ) );

    REQUIRE( QMetaObject::invokeMethod( &dialog, "updateConfigFromDialog", Qt::DirectConnection ) );

    auto& restoredConfig = Configuration::getSynced();
    REQUIRE( restoredConfig.adbExecutable() == QStringLiteral( "/updated/adb" ) );
    REQUIRE( restoredConfig.adbLogcatExtraArgs()
             == QStringLiteral( "-v threadtime ActivityManager:I *:S" ) );
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
    config.setAdbExecutable( QStringLiteral( "/custom/adb" ) );
    config.setAdbLogcatAnsiOutputEnabled( true );
    config.setIosLogExecutable( QStringLiteral( "/custom/pymobiledevice3" ) );
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
    CHECK( restoredConfig.adbExecutable() == defaults.adbExecutable() );
    CHECK( restoredConfig.adbLogcatAnsiOutputEnabled()
           == defaults.adbLogcatAnsiOutputEnabled() );
    CHECK( restoredConfig.iosLogExecutable() == defaults.iosLogExecutable() );
    CHECK( restoredConfig.iosLogAnsiOutputEnabled() == defaults.iosLogAnsiOutputEnabled() );
    CHECK( restoredConfig.useSearchResultsCache() == defaults.useSearchResultsCache() );
    CHECK( SavedSearches::getSynced().historySize() == defaultSavedSearches.historySize() );
    CHECK( RecentFiles::getSynced().filesHistoryMaxItems()
           == defaultRecentFiles.filesHistoryMaxItems() );

    const auto restoredOpenFileShortcuts
        = ShortcutAction::shortcutKeys( ShortcutAction::MainWindowOpenFile,
                                        restoredConfig.shortcuts() );
    const auto defaultOpenFileShortcuts
        = ShortcutAction::shortcutKeys( ShortcutAction::MainWindowOpenFile, {} );
    CHECK_FALSE( restoredOpenFileShortcuts.contains( QKeySequence( QStringLiteral( "Ctrl+Shift+P" ) ) ) );
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
            auto* current = visibleChildren[i];
            auto* next = visibleChildren[i + 1];

            const auto currentBottom = current->geometry().bottom();
            const auto nextTop = next->geometry().top();
            const int gap = nextTop - currentBottom;

            INFO( "[" << tabName.toStdString() << "] Widget " << i << ": \""
                  << current->objectName().toStdString() << "\" bottom=" << currentBottom
                  << " vs Widget " << ( i + 1 ) << ": \""
                  << next->objectName().toStdString() << "\" top=" << nextTop
                  << " gap=" << gap );

            CHECK( gap >= 1 );
        }
    }

    dialog.close();
    QCoreApplication::processEvents();
}

TEST_CASE( "OptionsDialog adb detect button fills the executable field with the resolved adb path" )
{
    if ( skipHeadlessOptionsDialogTest() ) {
        return;
    }

    if ( AdbProcessTransport::detectAdbExecutable().isEmpty() ) {
        WARN( "No adb installed at a well-known location -- skipping detect button test" );
        return;
    }

    ScopedAdbConfigurationGuard configGuard;
    auto& savedSearches = SavedSearches::getSynced();
    auto& recentFiles = RecentFiles::getSynced();
    Q_UNUSED( savedSearches );
    Q_UNUSED( recentFiles );
    auto& config = Configuration::getSynced();
    config.setAdbExecutable( QString{} );
    config.save();

    OptionsDialog dialog;
    auto* adbExecutableLineEdit
        = dialog.findChild<QLineEdit*>( QStringLiteral( "adbExecutableLineEdit" ) );
    auto* adbDetectButton
        = dialog.findChild<QPushButton*>( QStringLiteral( "adbDetectButton" ) );

    REQUIRE( adbExecutableLineEdit != nullptr );
    REQUIRE( adbDetectButton != nullptr );
    REQUIRE( adbDetectButton->isEnabled() );
    REQUIRE( adbExecutableLineEdit->text().isEmpty() );

    adbDetectButton->click();
    QCoreApplication::processEvents();

    const auto filled = adbExecutableLineEdit->text();
    INFO( "Filled value after detect click: " << filled.toStdString() );
    REQUIRE_FALSE( filled.isEmpty() );
    REQUIRE( QFileInfo( filled ).isAbsolute() );
    REQUIRE( QFile::exists( filled ) );
    REQUIRE( QFileInfo( filled ).isExecutable() );
}

namespace {
// A minimal ProcessLiveSourceTransport subclass that runs a long-lived process
// without ADB-specific argument decoration, for testing disconnect behavior.
class LongRunningTestTransport : public ProcessLiveSourceTransport {
  public:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "ping" ), { QStringLiteral( "-n" ), QStringLiteral( "60" ),
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

class FiniteSuccessfulTestTransport : public ProcessLiveSourceTransport {
  public:
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
// the connectTransport() startup-failure error-capture path.
class StartupStderrFailureTransport : public ProcessLiveSourceTransport {
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
} // namespace

TEST_CASE( "ProcessLiveSourceTransport suppresses errorOccurred during intentional disconnect" )
{
    LongRunningTestTransport transport;

    SafeQSignalSpy errorSpy( &transport, SIGNAL( errorOccurred( QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );

    // Connect -- the long-running process starts successfully
    REQUIRE( transport.connectTransport() );

    // Disconnect -- sets disconnectRequested_ = true before terminating
    transport.disconnectTransport();

    // Process events to let any queued signals through
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();

    // Verify: no errorOccurred signals should have been emitted during disconnect
    // (the disconnectRequested_ guard should suppress them)
    CHECK( errorSpy.count() == 0 );

    // The final state should be Disconnected (not Error)
    REQUIRE( stateSpy.count() >= 1 );
    const auto lastState
        = stateSpy.at( stateSpy.count() - 1 ).at( 0 ).value<LiveSourceTransport::State>();
    CHECK( lastState == LiveSourceTransport::State::Disconnected );
}

TEST_CASE( "ProcessLiveSourceTransport treats unexpected clean process exit as error" )
{
    FiniteSuccessfulTestTransport transport;

    SafeQSignalSpy errorSpy( &transport, SIGNAL( errorOccurred( QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );

    REQUIRE( transport.connectTransport() );

    REQUIRE( errorSpy.safeWait( 3000 ) );
    REQUIRE_FALSE( transport.lastError().isEmpty() );

    REQUIRE( stateSpy.count() >= 1 );
    const auto lastState
        = stateSpy.at( stateSpy.count() - 1 ).at( 0 ).value<LiveSourceTransport::State>();
    CHECK( lastState == LiveSourceTransport::State::Error );
}

TEST_CASE( "ProcessLiveSourceTransport async disconnect returns immediately" )
{
    LongRunningTestTransport transport;

    REQUIRE( transport.connectTransport() );

    QElapsedTimer timer;
    timer.start();
    transport.disconnectTransport();
    const auto elapsed = timer.elapsed();

    // Disconnect should complete in well under 100ms (no blocking waitForFinished)
    CHECK( elapsed < 100 );

    // Process events to let async cleanup finish
    QCoreApplication::processEvents();
    QTest::qWait( 2000 );
    QCoreApplication::processEvents();
}

// ---------------------------------------------------------------------------
// connectTransportAsync — non-blocking startup detection (replaces the
// blocking waitForStarted + grace loop for the auto-reconnect path)
// ---------------------------------------------------------------------------

TEST_CASE( "ProcessLiveSourceTransport connectTransportAsync returns immediately" )
{
    LongRunningTestTransport transport;

    QElapsedTimer timer;
    timer.start();
    transport.connectTransportAsync();
    const auto elapsed = timer.elapsed();

    // Must not block for the 250ms grace period or 3s waitForStarted.
    CHECK( elapsed < 100 );

    transport.disconnectTransport();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
}

namespace {
bool spyContainsState( const SafeQSignalSpy& spy, LiveSourceTransport::State target )
{
    for ( int i = 0; i < spy.count(); ++i ) {
        if ( spy.at( i ).at( 0 ).value<LiveSourceTransport::State>() == target ) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE( "ProcessLiveSourceTransport connectTransportAsync connects via grace timer" )
{
    LongRunningTestTransport transport;

    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );
    transport.connectTransportAsync();

    // safeWait returns on the first signal (Connecting); we must keep
    // processing events until the grace timer fires and transitions to
    // Connected.
    QElapsedTimer deadline;
    deadline.start();
    while ( !spyContainsState( stateSpy, LiveSourceTransport::State::Connected )
            && deadline.elapsed() < 2000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
    }
    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Connected ) );

    transport.disconnectTransport();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
}

TEST_CASE( "ProcessLiveSourceTransport connectTransportAsync detects startup failure" )
{
    TestAdbProcessTransport transport( QStringLiteral( "/path/that/does/not/exist/adb" ),
                                      QStringLiteral( "serial" ), {} );

    SafeQSignalSpy errorSpy( &transport, SIGNAL( errorOccurred( QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );
    transport.connectTransportAsync();

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

TEST_CASE( "ProcessLiveSourceTransport connectTransportAsync detects fast exit" )
{
    StartupStderrFailureTransport transport;

    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );
    transport.connectTransportAsync();

    // The process exits (with stderr) within the grace period; the finished
    // handler must detect this and transition to Error.
    QElapsedTimer deadline;
    deadline.start();
    while ( !spyContainsState( stateSpy, LiveSourceTransport::State::Error )
            && deadline.elapsed() < 2000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
    }
    REQUIRE( spyContainsState( stateSpy, LiveSourceTransport::State::Error ) );
    REQUIRE( transport.lastError().contains( QStringLiteral( "startup-boom" ) ) );

    transport.disconnectTransport();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
}

TEST_CASE( "ProcessLiveSourceTransport reconnects immediately after async disconnect" )
{
    LongRunningTestTransport transport;

    REQUIRE( transport.connectTransport() );
    transport.disconnectTransport();

    // Immediately reconnect -- should work since createProcess() made a fresh QProcess
    REQUIRE( transport.connectTransport() );

    transport.disconnectTransport();

    // Process events to let async cleanup finish
    QCoreApplication::processEvents();
    QTest::qWait( 2000 );
    QCoreApplication::processEvents();
}

TEST_CASE( "AdbLogcatDialog reads adb defaults from configuration and saves edits on accept" )
{
    if ( isHeadlessDialogTestEnvironment() ) {
        WARN( "AdbLogcatDialog UI coverage is skipped on headless/offscreen platforms" );
        return;
    }

    ScopedAdbConfigurationGuard configGuard;
    auto& config = Configuration::getSynced();
    config.setAdbExecutable( QStringLiteral( "/configured/adb" ) );
    config.setAdbLogcatExtraArgs( QStringLiteral( "-v color" ) );
    config.setAdbLogcatAnsiOutputEnabled( true );
    config.save();

    AdbLogcatDialog dialog;
    auto* adbExecutableEdit = dialog.findChild<QLineEdit*>( QStringLiteral( "adbExecutableEdit" ) );
    auto* extraArgsEdit = dialog.findChild<QLineEdit*>( QStringLiteral( "extraArgsEdit" ) );
    auto* ansiOutputCheckBox
        = dialog.findChild<QCheckBox*>( QStringLiteral( "ansiOutputCheckBox" ) );
    auto* deviceCombo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    auto* buttonBox = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );

    REQUIRE( adbExecutableEdit != nullptr );
    REQUIRE( extraArgsEdit != nullptr );
    REQUIRE( deviceCombo != nullptr );
    REQUIRE( ansiOutputCheckBox != nullptr );
    REQUIRE( buttonBox != nullptr );
    REQUIRE( adbExecutableEdit->text() == QStringLiteral( "/configured/adb" ) );
    REQUIRE( extraArgsEdit->text() == QStringLiteral( "-v color" ) );
    REQUIRE( ansiOutputCheckBox->isChecked() );

    adbExecutableEdit->setText( QStringLiteral( "/saved/adb" ) );
    extraArgsEdit->setText( QStringLiteral( "-v threadtime *:I" ) );
    ansiOutputCheckBox->setChecked( false );
    deviceCombo->addItem( QStringLiteral( "Pixel 8 (ABC123)" ), QStringLiteral( "ABC123" ) );
    deviceCombo->setCurrentIndex( 0 );
    QCoreApplication::processEvents();

    const auto sessionData = dialog.sessionData();
    REQUIRE( sessionData.adbExecutable == QStringLiteral( "/saved/adb" ) );
    REQUIRE( sessionData.deviceSerial == QStringLiteral( "ABC123" ) );
    REQUIRE( sessionData.deviceDescription == QStringLiteral( "Pixel 8 (ABC123)" ) );
    REQUIRE( sessionData.extraArgs == QStringLiteral( "-v threadtime *:I" ) );
    REQUIRE_FALSE( sessionData.ansiOutputEnabled );
    REQUIRE_FALSE( sessionData.captureId.isEmpty() );

    auto* okButton = buttonBox->button( QDialogButtonBox::Ok );
    REQUIRE( okButton != nullptr );
    REQUIRE( okButton->isEnabled() );
    okButton->click();

    auto& restoredConfig = Configuration::getSynced();
    REQUIRE( restoredConfig.adbExecutable() == QStringLiteral( "/saved/adb" ) );
    REQUIRE( restoredConfig.adbLogcatExtraArgs() == QStringLiteral( "-v threadtime *:I" ) );
    REQUIRE_FALSE( restoredConfig.adbLogcatAnsiOutputEnabled() );
}

TEST_CASE( "iOS log stream session data serializes its source type" )
{
    const AdbLogcatSessionData iosSessionData{
        QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QStringLiteral( "--network" ),
        QStringLiteral( "ios-capture" ),
        QStringLiteral( "/tmp/ios.log" ),
        LiveLogSourceType::IosLogStream,
        true,
    };

    const auto json = QString::fromUtf8(
        QJsonDocument( iosSessionData.toJson() ).toJson( QJsonDocument::Compact ) );
    const auto restored = AdbLogcatSessionData::fromJson( json );

    REQUIRE( restored.sourceType == LiveLogSourceType::IosLogStream );
    REQUIRE( restored.documentId() == QStringLiteral( "ios-log://ios-capture" ) );
    REQUIRE( restored.persistedSourceType() == QStringLiteral( "ios_log_stream" ) );
    REQUIRE( restored.deviceSerial == QStringLiteral( "00008030-001C195E36D8802E" ) );
    REQUIRE( restored.extraArgs == QStringLiteral( "--network" ) );
    REQUIRE( restored.ansiOutputEnabled );
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
    const AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::IosLogStream,
    };

    AdbLogcatSource source( sessionData, logData );

    REQUIRE( source.connectSource() );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( waitForLineCount( logData, 1 ) );

    REQUIRE( source.clearAndRestart() );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( source.lastError().isEmpty() );
    REQUIRE( waitForLineCount( logData, 1 ) );

    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
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
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( waitForLineCount( logData, 1 ) );
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );

    QElapsedTimer clearTimer;
    clearTimer.start();
    REQUIRE( source.clearAndRestart() );
    REQUIRE( clearTimer.elapsed() < 2000 );
    REQUIRE( logData->getNbLine().get() == 0 );

    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
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
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( waitForLineCount( logData, 1 ) );

    REQUIRE_FALSE( source.clearAndRestart() );
    REQUIRE( logData->getNbLine().get() == 0 );
    REQUIRE( source.state() == AdbLogcatSource::State::Error );
    REQUIRE( source.lastError().contains( QStringLiteral( "device disconnected during clear" ) ) );

    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
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
    const AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "00008030-001C195E36D8802E" ),
        QStringLiteral( "iPhone Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::IosLogStream,
    };

    AdbLogcatSource source( sessionData, logData );

    REQUIRE( source.connectSource() );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( waitForLineCount( logData, 1 ) );

    QFile marker( unavailableMarker );
    REQUIRE( marker.open( QIODevice::WriteOnly | QIODevice::Text ) );
    marker.write( "unavailable\n" );
    marker.close();

    REQUIRE( source.clearAndRestart() );
    REQUIRE( logData->getNbLine().get() == 0 );
    REQUIRE( source.state() == AdbLogcatSource::State::Error );
    REQUIRE_FALSE( source.lastError().isEmpty() );

    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
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
class StreamingScriptTransport : public ProcessLiveSourceTransport {
  public:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ),
                 { QStringLiteral( "/c" ),
                   QStringLiteral( "for /L %i in (1,1,5) do @(echo line %i & ping -n 1 -w 50 "
                                   "127.0.0.1 > nul)" ) } };
#else
        return { QStringLiteral( "/bin/sh" ),
                 { QStringLiteral( "-c" ),
                   QStringLiteral(
                       "i=1; while [ $i -le 5 ]; do echo line $i; i=$((i+1)); sleep 0.05; done" ) } };
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
                      [ &accumulated ]( const QByteArray& data ) { accumulated += data; } );

    KLOGG_REQUIRE_OR_WARN_SKIP(
        transport.connectTransport(),
        "StreamingScriptTransport: connectTransport failed in this environment "
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

    transport.disconnectTransport();
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
    TestIosLogProcessTransport transport( QStringLiteral( "~/Library/Python/3.9/bin/pymobiledevice3" ),
                                          QStringLiteral( "DEVICE_UDID" ), {} );
    const auto streaming = transport.streamingCommandForTest();
    REQUIRE_FALSE( streaming.program.startsWith( QLatin1Char( '~' ) ) );
    REQUIRE( streaming.program.contains( QStringLiteral( "Library/Python/3.9/bin/pymobiledevice3" ) ) );
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

namespace {
// Helper: read all lines from a StreamingLogData as a single string for assertions
QString allLogLines( const std::shared_ptr<StreamingLogData>& logData )
{
    QStringList lines;
    const auto count = logData->getNbLine().get();
    for ( LineNumber::UnderlyingType i = 0; i < count; ++i ) {
        lines.append( logData->getLineString( LineNumber( i ) ) );
    }
    return lines.join( QLatin1Char( '\n' ) );
}

// Helper: wait until auto-reconnect is no longer active (max attempts exhausted
// or reconnect cancelled), i.e., the reconnect timer has stopped.
bool waitForReconnectExhausted( const AdbLogcatSource& source, int timeoutMs = 10000 )
{
    QElapsedTimer deadline;
    deadline.start();
    while ( deadline.elapsed() < timeoutMs ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
        QTest::qWait( 50 );
        // Exhausted when both the timer isn't running and state is Error
        // (not Disconnected, which would mean manual disconnect)
        if ( !source.isAutoReconnectActive()
             && source.state() == AdbLogcatSource::State::Error ) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE( "AdbLogcatSource keeps auto-reconnect failures out of streaming log data" )
{
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );

    // Use a non-existent executable path so that connectTransport() always fails
    // synchronously (waitForStarted returns false), triggering the error+reconnect
    // cycle without depending on process timing.
    const AdbLogcatSessionData sessionData{
        QStringLiteral( "/nonexistent/path/to/adb" ),
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Test Device" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };

    AdbLogcatSource source( sessionData, logData );
    source.setAutoReconnectEnabled( true );
    source.setAutoReconnectMaxAttempts( 1 ); // Only 1 retry to keep test fast

    SafeQSignalSpy attemptSpy( &source, &AdbLogcatSource::reconnectAttemptStarted );

    // connectSource() will fail because the executable doesn't exist →
    // triggers scheduleReconnect → timer fires → attemptReconnect fails →
    // max attempts reached → streaming log must stay empty (silent retries)
    REQUIRE_FALSE( source.connectSource() );
    REQUIRE( source.state() == AdbLogcatSource::State::Error );

    // Wait for the reconnect cycle to complete (max attempts exhausted)
    REQUIRE( waitForReconnectExhausted( source, 5000 ) );

    // At least one reconnect attempt should have been started
    REQUIRE( attemptSpy.count() >= 1 );

    // Reconnect failures must NOT be written to the streaming log data
    // (fully silent retries — failures are surfaced via the status bar only).
    const auto logText = allLogLines( logData );
    INFO( "Streaming log content: " << logText.toStdString() );

    CHECK_FALSE( logText.contains( QStringLiteral( "auto-reconnect attempt" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "failed" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "max attempts" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "giving up" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "reconnected" ) ) );

    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
}

TEST_CASE( "AdbLogcatSource exponential backoff preserves attempt count after rapid "
           "post-connect death" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based backoff test on Windows." );
    return;
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    // Script that outputs a line, runs long enough to survive the grace period
    // (~500ms), then exits. This triggers the brief-connect-then-die pattern
    // that would reset the backoff counter without the reconnectionProven_ guard.
    const auto scriptPath = tempDir.filePath( QStringLiteral( "adb" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    // No stdout output — the process exits without producing data.
    // This triggers the failure path (reconnectionProven_ stays false).
    script.write( "#!/bin/sh\n"
                  "sleep 0.5\n"
                  "echo 'device disconnected' >&2\n"
                  "exit 1\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    const AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Test Device" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };

    AdbLogcatSource source( sessionData, logData );
    source.setAutoReconnectEnabled( true );
    source.setAutoReconnectMaxAttempts( 3 );

    // First connection succeeds (process starts, outputs, survives grace period)
    REQUIRE( source.connectSource() );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );

    // Wait for the process to exit and trigger auto-reconnect
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );
    REQUIRE( source.isAutoReconnectActive() );

    // Wait for the first reconnect attempt to start
    SafeQSignalSpy attemptSpy( &source, &AdbLogcatSource::reconnectAttemptStarted );
    REQUIRE( attemptSpy.safeWait( 3000 ) );

    // The reconnect attempt should have incremented the counter
    // (reconnectAttempt_ should be >= 1 after attemptReconnect increments it)
    const auto attemptAfterFirstReconnect = source.reconnectAttempt();
    INFO( "reconnectAttempt after first reconnect: " << attemptAfterFirstReconnect );
    CHECK( attemptAfterFirstReconnect >= 1 );

    // Wait for the reconnect to fail (process exits after 0.5s) and trigger
    // another scheduleReconnect cycle
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );

    // After the reconnect attempt connected briefly then died, the backoff
    // counter should still be preserved (NOT reset to 0).
    // The reconnectionProven_ flag prevents reset on brief connections.
    const auto attemptAfterSecondError = source.reconnectAttempt();
    INFO( "reconnectAttempt after second error: " << attemptAfterSecondError );
    CHECK( attemptAfterSecondError >= 1 );

    source.cancelAutoReconnect();
    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 500 );
    QCoreApplication::processEvents();
#endif
}

TEST_CASE( "AdbLogcatSource keeps async reconnect failure out of streaming log" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based async failure test on Windows." );
    return;
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    // Script that runs just long enough to survive the grace period (~400ms)
    // then exits with an error. This triggers the async failure path where
    // connectTransport() returns true but the process dies shortly after.
    const auto scriptPath = tempDir.filePath( QStringLiteral( "adb" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\n"
                  "sleep 0.4\n"
                  "echo 'device offline' >&2\n"
                  "exit 1\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    const AdbLogcatSessionData sessionData{
        scriptPath,
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Test Device" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };

    AdbLogcatSource source( sessionData, logData );
    source.setAutoReconnectEnabled( true );
    source.setAutoReconnectMaxAttempts( 2 );

    // Initial connect: process starts, survives grace period, then dies
    REQUIRE( source.connectSource() );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );

    // Wait for async process death → Error → scheduleReconnect
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );
    REQUIRE( source.isAutoReconnectActive() );

    // Wait for the reconnect attempt to fire and the process to die again
    SafeQSignalSpy attemptSpy( &source, &AdbLogcatSource::reconnectAttemptStarted );
    REQUIRE( attemptSpy.safeWait( 5000 ) );

    // Wait for the reconnect attempt's process to die asynchronously
    REQUIRE( waitForSourceState( source, AdbLogcatSource::State::Error ) );

    // Reconnect failures must NOT be written to the streaming log data
    // (fully silent retries — failures are surfaced via the status bar only).
    // The mock script's stderr ("device offline") must also not leak.
    const auto logText = allLogLines( logData );
    INFO( "Streaming log content after async reconnect failure:\n" << logText.toStdString() );

    CHECK_FALSE( logText.contains( QStringLiteral( "auto-reconnect attempt" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "failed" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "device offline" ) ) );
    CHECK_FALSE( logText.contains( QStringLiteral( "reconnected" ) ) );

    source.cancelAutoReconnect();
    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 500 );
    QCoreApplication::processEvents();
#endif
}

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
    auto* resetButton = dialog.findChild<QPushButton*>( QStringLiteral( "resetFileDefaultsButton" ) );
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
    CHECK( restoredConfig.liveAutoReconnectMaxAttempts() == defaults.liveAutoReconnectMaxAttempts() );
    CHECK( restoredConfig.liveCaptureRollingMaxFileSize()
           == defaults.liveCaptureRollingMaxFileSize() );
    CHECK( restoredConfig.liveCaptureRollingBackupCount()
           == defaults.liveCaptureRollingBackupCount() );
}

TEST_CASE( "ProcessLiveSourceTransport surfaces real stderr on startup failure" )
{
    StartupStderrFailureTransport transport;

    REQUIRE_FALSE( transport.connectTransport() );
    // stderr is redirected to a file via setStandardErrorFile(); the startup
    // path must read that file (not readAllStandardError(), which is empty).
    REQUIRE( transport.lastError().contains( QStringLiteral( "startup-boom" ) ) );

    transport.disconnectTransport();
    QTest::qWait( 200 );
}

TEST_CASE( "AdbLogcatSource does not auto-reconnect before being enabled" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based auto-reconnect default test on Windows." );
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    // A non-existent executable fails connectTransport() deterministically in
    // waitForStarted() (no dependence on the 250ms startup-grace window).
    const AdbLogcatSessionData sessionData{
        QStringLiteral( "/path/that/does/not/exist/adb" ),
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Pixel Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };
    AdbLogcatSource source( sessionData, logData );

    // No setAutoReconnectEnabled() call: the member default must be disabled so
    // a failing connect does not arm the reconnect timer.
    REQUIRE_FALSE( source.connectSource() );
    QCoreApplication::processEvents();
    REQUIRE_FALSE( source.isAutoReconnectActive() );

    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
#endif
}

TEST_CASE( "AdbLogcatSource manual reconnect resets the attempt counter" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based reconnect reset test on Windows." );
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    const auto captureId = makeCaptureId();
    auto logData = std::make_shared<StreamingLogData>( captureId, tempDir.path() );
    // A non-existent executable fails connectTransport() deterministically in
    // waitForStarted() (no dependence on the 250ms startup-grace window).
    const AdbLogcatSessionData sessionData{
        QStringLiteral( "/path/that/does/not/exist/adb" ),
        QStringLiteral( "emulator-5554" ),
        QStringLiteral( "Pixel Test" ),
        QString{},
        captureId,
        QString{},
        LiveLogSourceType::AdbLogcat,
    };
    AdbLogcatSource source( sessionData, logData );
    source.setAutoReconnectEnabled( true );
    source.setAutoReconnectMaxAttempts( 50 );

    REQUIRE_FALSE( source.connectSource() ); // fails, schedules first reconnect (~1s)

    // Wait for at least one auto-reconnect attempt to fire and increment.
    QElapsedTimer deadline;
    deadline.start();
    while ( source.reconnectAttempt() < 1 && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
        QTest::qWait( 20 );
    }
    REQUIRE( source.reconnectAttempt() >= 1 );

    // A manual reconnect must reset the stale attempt counter.
    source.reconnectSource();
    REQUIRE( source.reconnectAttempt() == 0 );

    source.setAutoReconnectEnabled( false );
    source.disconnectSource();
    QCoreApplication::processEvents();
    QTest::qWait( 200 );
    QCoreApplication::processEvents();
#endif
}

TEST_CASE( "DeviceListProvider async enumeration is safe against provider destruction" )
{
#ifdef Q_OS_WIN
    WARN( "Skipping POSIX shell based device-provider lifetime test on Windows." );
#else
    QTemporaryDir tempDir;
    REQUIRE( tempDir.isValid() );

    // A slow fake adb keeps the async task in flight while we destroy the
    // provider, exercising the lifetime guard in listDevicesAsync().
    const auto scriptPath = tempDir.filePath( QStringLiteral( "adb" ) );
    QFile script( scriptPath );
    REQUIRE( script.open( QIODevice::WriteOnly | QIODevice::Text ) );
    script.write( "#!/bin/sh\nsleep 1\nexit 0\n" );
    script.close();
    REQUIRE( script.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) );

    auto* provider = new AdbDeviceListProvider( scriptPath );
    QFuture<QList<AdbDeviceInfo>> future = provider->listDevicesAsync();

    // Destroy the provider while the task is still in flight. The QPointer guard
    // must keep this from dereferencing freed memory.
    delete provider;
    future.waitForFinished(); // must not crash

    REQUIRE( future.resultCount() == 1 );
    // Guarded path yields an empty list when the provider no longer exists.
    REQUIRE( future.result().isEmpty() );
#endif
}
