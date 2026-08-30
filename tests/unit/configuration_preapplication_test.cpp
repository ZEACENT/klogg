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

#include <string>

#include <QGuiApplication>
#include <QList>
#include <QSettings>
#include <QTemporaryDir>

#include "configuration.h"
#include "shortcuts.h"

namespace {
constexpr auto CtrlGDefaultsMigrationMarker = "shortcuts.ctrlGDefaultsMigrated";

void writeShortcut( QSettings& settings, const std::string& action, const QStringList& keys )
{
    settings.beginWriteArray( QStringLiteral( "shortcuts" ) );
    settings.setArrayIndex( 0 );
    settings.setValue( QStringLiteral( "action" ), QString::fromStdString( action ) );
    settings.setValue( QStringLiteral( "keys" ), keys );
    settings.endArray();
    settings.sync();
}
} // namespace

TEST_CASE( "Configuration shortcut migration is safe before QGuiApplication construction" )
{
    REQUIRE( QGuiApplication::instance() == nullptr );
    QTemporaryDir settingsRoot;
    REQUIRE( settingsRoot.isValid() );

    SECTION( "custom find-next binding is preserved" )
    {
        QSettings settings( settingsRoot.filePath( QStringLiteral( "custom.ini" ) ),
                            QSettings::IniFormat );
        const QStringList customKeys{
            QStringLiteral( "Meta+G" ),
            QStringLiteral( "F3" ),
        };
        writeShortcut( settings, ShortcutAction::LogViewQfForward, customKeys );

        Configuration configuration;
        configuration.retrieveFromStorage( settings );

        REQUIRE( QGuiApplication::instance() == nullptr );
        REQUIRE( configuration.shortcuts().count( ShortcutAction::LogViewQfForward ) == 1 );
        CHECK( configuration.shortcuts().at( ShortcutAction::LogViewQfForward )
               == customKeys );
        CHECK( settings.value( CtrlGDefaultsMigrationMarker, false ).toBool() );
    }

    SECTION( "exact historical find-next defaults are removed idempotently" )
    {
        const QList<QStringList> historicalDefaults{
            { QStringLiteral( "Ctrl+G" ) },
            { QStringLiteral( "Meta+G" ) },
            { QStringLiteral( "F3" ), QStringLiteral( "Ctrl+G" ) },
            { QStringLiteral( "Ctrl+G" ), QStringLiteral( "F3" ) },
            { QStringLiteral( "Meta+G" ), QStringLiteral( "Ctrl+G" ) },
            { QStringLiteral( "Ctrl+G" ), QStringLiteral( "Meta+G" ) },
        };
        int fixtureIndex = 0;
        for ( const auto& keys : historicalDefaults ) {
            INFO( "historical fixture " << fixtureIndex );
            QSettings settings(
                settingsRoot.filePath(
                    QStringLiteral( "legacy-%1.ini" ).arg( fixtureIndex++ ) ),
                QSettings::IniFormat );
            writeShortcut( settings, ShortcutAction::LogViewQfForward, keys );

            Configuration configuration;
            configuration.retrieveFromStorage( settings );
            CHECK( configuration.shortcuts().count( ShortcutAction::LogViewQfForward ) == 0 );
            REQUIRE( settings.value( CtrlGDefaultsMigrationMarker, false ).toBool() );

            Configuration restored;
            restored.retrieveFromStorage( settings );
            CHECK( restored.shortcuts().count( ShortcutAction::LogViewQfForward ) == 0 );
            CHECK( QGuiApplication::instance() == nullptr );
        }
    }
}
