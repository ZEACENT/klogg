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
