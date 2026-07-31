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
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QUuid>

#include "tabgroup.h"

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

TEST_CASE( "TabGroupManager removes an empty group when the last tab is removed" )
{
    TabGroupManager manager;
    QSignalSpy changedSpy( &manager, &TabGroupManager::groupsChanged );

    manager.createGroup( "Errors", QColor( "#FF3344" ) );
    REQUIRE( manager.groups().size() == 1 );

    const auto groupId = manager.groups().front().id;
    manager.addTabToGroup( groupId, "/tmp/a.log" );
    REQUIRE( manager.groupIdForTab( "/tmp/a.log" ) == groupId );

    manager.removeTabFromGroup( "/tmp/a.log" );

    REQUIRE( manager.groupIdForTab( "/tmp/a.log" ).isEmpty() );
    REQUIRE( manager.groups().isEmpty() );
    REQUIRE( changedSpy.count() >= 3 );
}

TEST_CASE( "TabGroupManager locates later groups and their tabs" )
{
    TabGroupManager manager;
    manager.createGroup( "Frontend", QColor( "#3366FF" ) );
    manager.createGroup( "Backend", QColor( "#00AA66" ) );
    REQUIRE( manager.groups().size() == 2 );

    const auto firstGroupId = manager.groups().front().id;
    const auto secondGroupId = manager.groups().back().id;
    const auto backendPath = QStringLiteral( "/tmp/backend.log" );
    manager.addTabToGroup( secondGroupId, backendPath );

    REQUIRE( manager.groupById( secondGroupId ) != nullptr );
    CHECK( manager.groupById( secondGroupId )->name == QStringLiteral( "Backend" ) );
    CHECK( manager.groupById( QStringLiteral( "missing" ) ) == nullptr );
    CHECK( manager.groupIdForTab( backendPath ) == secondGroupId );
    CHECK( manager.groupIdForTab( QStringLiteral( "/tmp/missing.log" ) ).isEmpty() );
    CHECK( manager.groupForTab( backendPath ) == manager.groupById( secondGroupId ) );
    CHECK( manager.groupById( firstGroupId ) != nullptr );

    const auto& constManager = manager;
    REQUIRE( constManager.groupById( secondGroupId ) != nullptr );
    CHECK( constManager.groupById( secondGroupId )->name == QStringLiteral( "Backend" ) );
    CHECK( constManager.groupById( QStringLiteral( "missing" ) ) == nullptr );
}

TEST_CASE( "TabGroupManager moveTabToGroup with empty target removes group membership" )
{
    TabGroupManager manager;
    manager.createGroup( "Debug", QColor( "#4488FF" ) );

    const auto groupId = manager.groups().front().id;
    manager.addTabToGroup( groupId, "/tmp/debug.log" );
    REQUIRE( manager.groupIdForTab( "/tmp/debug.log" ) == groupId );

    manager.moveTabToGroup( "/tmp/debug.log", QString{} );

    REQUIRE( manager.groupIdForTab( "/tmp/debug.log" ).isEmpty() );
    REQUIRE( manager.groups().isEmpty() );
}

TEST_CASE( "TabGroupManager persists and restores groups with tabs and collapsed state" )
{
    const auto dirPath = makeTestDir( "tabgroup" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "tabgroup.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );
        TabGroupManager manager;
        manager.createGroup( "Backend", QColor( "#00AA66" ) );
        const auto groupId = manager.groups().front().id;
        manager.addTabToGroup( groupId, "/tmp/backend-1.log" );
        manager.addTabToGroup( groupId, "/tmp/backend-2.log" );
        manager.toggleCollapsed( groupId );

        manager.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    TabGroupManager restoredManager;
    restoredManager.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredManager.groups().size() == 1 );
    const auto restoredGroup = restoredManager.groups().front();
    REQUIRE( restoredGroup.name == "Backend" );
    REQUIRE( restoredGroup.color == QColor( "#00AA66" ) );
    REQUIRE( restoredGroup.collapsed );
    REQUIRE( restoredGroup.tabPaths.contains( "/tmp/backend-1.log" ) );
    REQUIRE( restoredGroup.tabPaths.contains( "/tmp/backend-2.log" ) );
}
