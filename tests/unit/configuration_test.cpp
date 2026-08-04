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

#include <QDir>
#include <QSettings>
#include <QUuid>

#include "configuration.h"

namespace {
QString makeTestDir( const QString& prefix )
{
    const auto dirPath = QDir::cleanPath( QDir::currentPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) + QDir::separator()
                                          + prefix + QLatin1Char( '_' )
                                          + QUuid::createUuid().toString( QUuid::WithoutBraces ) );
    QDir{}.mkpath( dirPath );
    return dirPath;
}
} // namespace

TEST_CASE( "Configuration defaults line spacing to an editor-friendly value" )
{
    Configuration config;

    REQUIRE( config.lineSpacingPercent() == Configuration::DefaultLineSpacingPercent );
}

TEST_CASE( "Configuration stores and restores line spacing percent" )
{
    const auto dirPath = makeTestDir( "configuration" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        Configuration config;
        config.setLineSpacingPercent( 145 );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.lineSpacingPercent() == 145 );
}

TEST_CASE( "Configuration stores and restores splitter sizes" )
{
    const auto dirPath = makeTestDir( "configuration_splitter_sizes" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-splitter-sizes.ini" );
    const QList<int> expectedSizes{ 420, 180 };

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        Configuration config;
        config.setSplitterSizes( expectedSizes );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.splitterSizes() == expectedSizes );
}

TEST_CASE( "Configuration clamps line spacing percent to supported bounds" )
{
    Configuration config;

    config.setLineSpacingPercent( Configuration::MinLineSpacingPercent - 20 );
    REQUIRE( config.lineSpacingPercent() == Configuration::MinLineSpacingPercent );

    config.setLineSpacingPercent( Configuration::MaxLineSpacingPercent + 25 );
    REQUIRE( config.lineSpacingPercent() == Configuration::MaxLineSpacingPercent );

    const auto dirPath = makeTestDir( "configuration_clamp" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-clamp.ini" );

    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.setValue( "mainFont.lineSpacingPercent", Configuration::MaxLineSpacingPercent + 50 );
    settings.sync();
    REQUIRE( settings.status() == QSettings::NoError );

    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( settings );

    REQUIRE( restoredConfig.lineSpacingPercent() == Configuration::MaxLineSpacingPercent );
}

TEST_CASE( "Configuration stores and restores adb defaults" )
{
    const auto dirPath = makeTestDir( "configuration_adb" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-adb.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        Configuration config;
        config.setAdbExecutable( QStringLiteral( "/opt/android/platform-tools/adb" ) );
        config.setAdbLogcatExtraArgs( QStringLiteral( "-v threadtime *:I" ) );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.adbExecutable()
             == QStringLiteral( "/opt/android/platform-tools/adb" ) );
    REQUIRE( restoredConfig.adbLogcatExtraArgs() == QStringLiteral( "-v threadtime *:I" ) );
}

TEST_CASE( "Configuration stores and restores iOS log defaults" )
{
    const auto dirPath = makeTestDir( "configuration_ios_log" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-ios-log.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        Configuration config;
        config.setIosLogExecutable( QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ) );
        config.setIosLogExtraArgs( QStringLiteral( "--no-color --match SpringBoard" ) );
        config.setIosLogAnsiOutputEnabled( true );
        config.setAdbLogcatAnsiOutputEnabled( true );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.iosLogExecutable()
             == QStringLiteral( "/opt/homebrew/bin/pymobiledevice3" ) );
    REQUIRE( restoredConfig.iosLogExtraArgs()
             == QStringLiteral( "--no-color --match SpringBoard" ) );
    REQUIRE( restoredConfig.iosLogAnsiOutputEnabled() );
    REQUIRE( restoredConfig.adbLogcatAnsiOutputEnabled() );
}

TEST_CASE( "Configuration defaults auto-reconnect to disabled" )
{
    Configuration config;

    REQUIRE_FALSE( config.liveAutoReconnectEnabled() );
}

TEST_CASE( "Configuration stores and restores auto-reconnect settings" )
{
    const auto dirPath = makeTestDir( "configuration_reconnect" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-reconnect.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        Configuration config;
        config.setLiveAutoReconnectEnabled( true );
        config.setLiveAutoReconnectMaxAttempts( 5 );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.liveAutoReconnectEnabled() );
    REQUIRE( restoredConfig.liveAutoReconnectMaxAttempts() == 5 );
}

TEST_CASE( "Configuration defaults max capture file size to 1000 MB" )
{
    Configuration config;

    // 1000 MB in bytes
    REQUIRE( config.liveCaptureRollingMaxFileSize() == 1000LL * 1024 * 1024 );
}

TEST_CASE( "Configuration stores and restores capture rolling settings" )
{
    const auto dirPath = makeTestDir( "configuration_capture" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-capture.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        Configuration config;
        config.setLiveCaptureRollingMaxFileSize( 500LL * 1024 * 1024 );
        config.setLiveCaptureRollingBackupCount( 3 );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.liveCaptureRollingMaxFileSize() == 500LL * 1024 * 1024 );
    REQUIRE( restoredConfig.liveCaptureRollingBackupCount() == 3 );
}

TEST_CASE( "Configuration defaults empty filters to an empty filtered view" )
{
    Configuration config;

    // Upstream parity (issue #46): an empty filter must NOT flood the
    // filtered window with every line; users opt into the mirror mode.
    REQUIRE_FALSE( config.showAllInFilteredViewWhenSearchEmpty() );
}

TEST_CASE( "Configuration stores and restores empty-filter filtered-view behavior" )
{
    const auto dirPath = makeTestDir( "configuration_empty_filter" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration-empty-filter.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        // Store the non-default value so the round-trip stays discriminating
        // (the compiled default is false since issue #46).
        Configuration config;
        config.setShowAllInFilteredViewWhenSearchEmpty( true );
        config.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    Configuration restoredConfig;
    restoredConfig.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredConfig.showAllInFilteredViewWhenSearchEmpty() );
}

TEST_CASE( "Configuration live-capture rolling size MB view does not truncate sub-MB values",
           "[configuration]" )
{
    Configuration config;

    // A sub-megabyte byte value must round up to 1 MB, not truncate to 0
    // (which the live-source dialog treats as "unlimited").
    config.setLiveCaptureRollingMaxFileSize( 512LL * 1024 );
    REQUIRE( config.liveCaptureRollingMaxFileSizeMb() == 1 );

    // A value just under half a megabyte rounds down to 0 (legitimately
    // "unlimited"); exactly half rounds up.
    config.setLiveCaptureRollingMaxFileSize( 400LL * 1024 );
    REQUIRE( config.liveCaptureRollingMaxFileSizeMb() == 0 );

    // Whole-megabyte values round-trip exactly through the MB helpers.
    config.setLiveCaptureRollingMaxFileSizeMb( 250 );
    REQUIRE( config.liveCaptureRollingMaxFileSize() == 250LL * 1024 * 1024 );
    REQUIRE( config.liveCaptureRollingMaxFileSizeMb() == 250 );

    // 0 bytes stays 0 MB.
    config.setLiveCaptureRollingMaxFileSize( 0 );
    REQUIRE( config.liveCaptureRollingMaxFileSizeMb() == 0 );
}

TEST_CASE( "Configuration migrates stale perf.useBlockScan=false from pre-#41 installs" )
{
    const auto dirPath = makeTestDir( "configuration_blockscan" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "configuration.ini" );

    {
        // Simulate a pre-#41 install: the flag was persisted as false while the
        // compiled default at the time was also false. Since #41 the compiled
        // default is ON, but the persisted key silently wins forever (no UI
        // toggle exists), keeping the slow per-line search path.
        QSettings settings( settingsPath, QSettings::IniFormat );
        settings.setValue( "perf.useBlockScan", false );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings migratedSettings( settingsPath, QSettings::IniFormat );
    Configuration config;
    config.retrieveFromStorage( migratedSettings );

    // One-shot migration: the stale key is removed so the compiled default
    // applies; a marker suppresses re-migration.
    REQUIRE( Configuration{}.useBlockScan() );
    REQUIRE( config.useBlockScan() );
    REQUIRE( !migratedSettings.contains( "perf.useBlockScan" ) );
    REQUIRE( migratedSettings.value( "perf.useBlockScanMigrated", false ).toBool() );

    // A deliberate post-migration choice sticks (marker suppresses the sweep).
    migratedSettings.setValue( "perf.useBlockScan", false );
    migratedSettings.sync();
    Configuration config2;
    config2.retrieveFromStorage( migratedSettings );
    REQUIRE_FALSE( config2.useBlockScan() );
}
