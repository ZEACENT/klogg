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
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <map>

#include "adbprocesstransport.h"
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
    REQUIRE( colorCmd.arguments
             == QStringList{ QStringLiteral( "-q" ), QStringLiteral( "/dev/null" ),
                             QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
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
    // On macOS, the command is wrapped with script to allocate a PTY.
    REQUIRE( colorCmd.program == QStringLiteral( "/usr/bin/script" ) );
    REQUIRE( colorCmd.arguments
             == QStringList{ QStringLiteral( "-q" ), QStringLiteral( "/dev/null" ),
                             QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ),
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
