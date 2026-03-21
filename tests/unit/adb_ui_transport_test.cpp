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

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLineEdit>
#include <QPushButton>

#include "adbprocesstransport.h"
#include "adblogcatdialog.h"
#include "configuration.h"
#include "livesourcetransport.h"
#include "optionsdialog.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "test_utils.h"

namespace {
bool isHeadlessDialogTestEnvironment()
{
    return QGuiApplication::screens().isEmpty()
           || QGuiApplication::platformName().compare( QStringLiteral( "offscreen" ),
                                                       Qt::CaseInsensitive )
                  == 0;
}

class ScopedAdbConfigurationGuard {
  public:
    ScopedAdbConfigurationGuard()
        : config_( Configuration::getSynced() )
        , executable_( config_.adbExecutable() )
        , extraArgs_( config_.adbLogcatExtraArgs() )
    {
    }

    ~ScopedAdbConfigurationGuard()
    {
        config_.setAdbExecutable( executable_ );
        config_.setAdbLogcatExtraArgs( extraArgs_ );
        config_.save();
    }

  private:
    Configuration& config_;
    QString executable_;
    QString extraArgs_;
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
} // namespace

TEST_CASE( "AdbProcessTransport builds normalized streaming and clear commands" )
{
    TestAdbProcessTransport transport( QString{}, QStringLiteral( "emulator-5554" ),
                                       QStringLiteral( "-v threadtime -T \"2026-03-15 12:34:56.000\" *:I" ) );

    const auto streaming = transport.streamingCommandForTest();
    REQUIRE( streaming.program == QStringLiteral( "adb" ) );
    REQUIRE( streaming.arguments
             == QStringList{ QStringLiteral( "-s" ), QStringLiteral( "emulator-5554" ),
                             QStringLiteral( "logcat" ), QStringLiteral( "-v" ),
                             QStringLiteral( "threadtime" ), QStringLiteral( "-T" ),
                             QStringLiteral( "2026-03-15 12:34:56.000" ),
                             QStringLiteral( "*:I" ) } );

    const auto clear = transport.clearCommandForTest();
    REQUIRE( clear.program == QStringLiteral( "adb" ) );
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
#ifdef Q_OS_WIN
    TestAdbProcessTransport transport( QStringLiteral( "whoami.exe" ), QStringLiteral( "serial-123" ), {} );
#else
    TestAdbProcessTransport transport( QStringLiteral( "false" ), QStringLiteral( "serial-123" ), {} );
#endif
    SafeQSignalSpy errorSpy( &transport, SIGNAL( errorOccurred( QString ) ) );
    SafeQSignalSpy stateSpy( &transport, SIGNAL( stateChanged( LiveSourceTransport::State ) ) );

    REQUIRE_FALSE( transport.connectTransport() );
    REQUIRE( errorSpy.safeWait() );
    REQUIRE( stateSpy.count() >= 1 );
    REQUIRE_FALSE( transport.lastError().isEmpty() );
}

TEST_CASE( "OptionsDialog loads and persists adb settings" )
{
    if ( isHeadlessDialogTestEnvironment() ) {
        WARN( "OptionsDialog UI coverage is skipped on headless/offscreen platforms" );
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

    // Connect — the long-running process starts successfully
    REQUIRE( transport.connectTransport() );

    // Disconnect — sets disconnectRequested_ = true before terminating
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

    // Immediately reconnect — should work since createProcess() made a fresh QProcess
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
    config.save();

    AdbLogcatDialog dialog;
    auto* adbExecutableEdit = dialog.findChild<QLineEdit*>( QStringLiteral( "adbExecutableEdit" ) );
    auto* extraArgsEdit = dialog.findChild<QLineEdit*>( QStringLiteral( "extraArgsEdit" ) );
    auto* deviceCombo = dialog.findChild<QComboBox*>( QStringLiteral( "deviceCombo" ) );
    auto* buttonBox = dialog.findChild<QDialogButtonBox*>( QStringLiteral( "buttonBox" ) );

    REQUIRE( adbExecutableEdit != nullptr );
    REQUIRE( extraArgsEdit != nullptr );
    REQUIRE( deviceCombo != nullptr );
    REQUIRE( buttonBox != nullptr );
    REQUIRE( adbExecutableEdit->text() == QStringLiteral( "/configured/adb" ) );
    REQUIRE( extraArgsEdit->text() == QStringLiteral( "-v color" ) );

    adbExecutableEdit->setText( QStringLiteral( "/saved/adb" ) );
    extraArgsEdit->setText( QStringLiteral( "-v threadtime *:I" ) );
    deviceCombo->addItem( QStringLiteral( "Pixel 8 (ABC123)" ), QStringLiteral( "ABC123" ) );
    deviceCombo->setCurrentIndex( 0 );
    QCoreApplication::processEvents();

    const auto sessionData = dialog.sessionData();
    REQUIRE( sessionData.adbExecutable == QStringLiteral( "/saved/adb" ) );
    REQUIRE( sessionData.deviceSerial == QStringLiteral( "ABC123" ) );
    REQUIRE( sessionData.deviceDescription == QStringLiteral( "Pixel 8 (ABC123)" ) );
    REQUIRE( sessionData.extraArgs == QStringLiteral( "-v threadtime *:I" ) );
    REQUIRE_FALSE( sessionData.captureId.isEmpty() );

    auto* okButton = buttonBox->button( QDialogButtonBox::Ok );
    REQUIRE( okButton != nullptr );
    REQUIRE( okButton->isEnabled() );
    okButton->click();

    auto& restoredConfig = Configuration::getSynced();
    REQUIRE( restoredConfig.adbExecutable() == QStringLiteral( "/saved/adb" ) );
    REQUIRE( restoredConfig.adbLogcatExtraArgs() == QStringLiteral( "-v threadtime *:I" ) );
}
