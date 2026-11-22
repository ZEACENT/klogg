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
#include <QTemporaryDir>
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

TEST_CASE( "TabGroupManager prepares multi-group removals without publishing a prefix",
           "[workspace-publication]" )
{
    TabGroupManager manager;
    manager.createGroup( "First", Qt::red );
    const auto first = manager.groups().front().id;
    manager.addTabToGroup( first, "close-a" );
    manager.addTabToGroup( first, "keep" );
    manager.createGroup( "Second", Qt::blue );
    const auto second = manager.groups().back().id;
    manager.addTabToGroup( second, "close-b" );
    QSignalSpy changed{ &manager, &TabGroupManager::groupsChanged };
    auto pending = manager.prepareTabRemovals( { "close-a", "close-b" } );
    REQUIRE( pending );
    CHECK( manager.groupIdForTab( "close-a" ) == first );
    CHECK( manager.groupIdForTab( "close-b" ) == second );
    CHECK( changed.count() == 0 );
    REQUIRE( pending->isCurrent() );
    REQUIRE( pending->install() );
    CHECK_FALSE( pending->install() );
    CHECK( changed.count() == 0 );
    CHECK( manager.groupIdForTab( "close-a" ).isEmpty() );
    CHECK( manager.groupIdForTab( "close-b" ).isEmpty() );
    CHECK( manager.groupIdForTab( "keep" ) == first );
    CHECK( manager.groups().size() == 1 );
    bool otherOwnerInstalled = false;
    QObject::connect( &manager, &TabGroupManager::groupsChanged, &manager, [&] {
        CHECK( otherOwnerInstalled );
        CHECK( manager.groupIdForTab( "close-a" ).isEmpty() );
        CHECK( manager.groupIdForTab( "close-b" ).isEmpty() );
    } );
    otherOwnerInstalled = true;
    pending->notify();
    pending->notify();
    CHECK( changed.count() == 1 );
}

TEST_CASE( "TabGroupManager prepared removal detects ABA and preserves no-op dependencies",
           "[workspace-publication]" )
{
    TabGroupManager manager;
    manager.createGroup( "Original", Qt::red );
    const auto id = manager.groups().front().id;
    manager.addTabToGroup( id, "keep" );
    auto pending = manager.prepareTabRemovals( { "keep" } );
    REQUIRE( pending->isCurrent() );
    SECTION( "a native edit and reversal invalidates the candidate" ) {
        manager.renameGroup( id, "Temporary" );
        manager.renameGroup( id, "Original" );
        CHECK_FALSE( pending->isCurrent() );
        CHECK_FALSE( pending->install() );
        QSignalSpy changed{ &manager, &TabGroupManager::groupsChanged };
        pending->notify();
        CHECK( changed.count() == 0 );
        CHECK( manager.groupIdForTab( "keep" ) == id );
    }
    SECTION( "no-op installation does not invalidate another candidate" ) {
        auto noop = manager.prepareTabRemovals( { "absent" } );
        QSignalSpy changed{ &manager, &TabGroupManager::groupsChanged };
        REQUIRE( noop->install() );
        noop->notify();
        CHECK( changed.count() == 0 );
        CHECK( pending->isCurrent() );
        REQUIRE( pending->install() );
        pending->notify();
        CHECK( changed.count() == 1 );
    }
}

TEST_CASE( "TabGroupManager prepared removal survives discarded and deleted owners",
           "[workspace-publication]" )
{
    auto manager = std::make_unique<TabGroupManager>();
    manager->createGroup( "Owner", Qt::red );
    const auto id = manager->groups().front().id;
    manager->addTabToGroup( id, "close" );
    {
        auto discarded = manager->prepareTabRemovals( { "close" } );
        CHECK( discarded->isCurrent() );
    }
    CHECK( manager->groupIdForTab( "close" ) == id );
    auto pending = manager->prepareTabRemovals( { "close" } );
    SECTION( "owner disappears before installation" ) {
        manager.reset();
        CHECK_FALSE( pending->isCurrent() );
        CHECK_FALSE( pending->install() );
        pending->notify();
    }
    SECTION( "publication observer destroys the update and owner" ) {
        REQUIRE( pending->install() );
        QObject observer;
        QObject::connect( manager.get(), &TabGroupManager::groupsChanged, &observer, [&] {
            pending.reset();
            manager.reset();
        } );
        pending->notify();
        CHECK_FALSE( pending );
        CHECK_FALSE( manager );
    }
}

TEST_CASE( "TabGroupManager native writes and storage reload invalidate prepared removal",
           "[workspace-publication]" )
{
    TabGroupManager manager;
    manager.createGroup( "Owner", Qt::red );
    const auto id = manager.groups().front().id;
    manager.addTabToGroup( id, "close" );
    auto pending = manager.prepareTabRemovals( { "close" } );
    SECTION( "color" ) { manager.setGroupColor( id, Qt::blue ); }
    SECTION( "collapse" ) { manager.toggleCollapsed( id ); }
    SECTION( "add member" ) { manager.addTabToGroup( id, "other" ); }
    SECTION( "remove member" ) { manager.removeTabFromGroup( "close" ); }
    SECTION( "delete group" ) { manager.deleteGroup( id ); }
    SECTION( "ungroup" ) { manager.ungroupAll( id ); }
    SECTION( "create group" ) { manager.createGroup( "Other", Qt::blue ); }
    SECTION( "reload identical persisted state" ) {
        QTemporaryDir root;
        REQUIRE( root.isValid() );
        QSettings settings{ root.filePath( "groups.ini" ), QSettings::IniFormat };
        manager.saveToStorage( settings );
        manager.retrieveFromStorage( settings );
        CHECK( manager.groupIdForTab( "close" ) == id );
    }
    CHECK_FALSE( pending->isCurrent() );
    CHECK_FALSE( pending->install() );
    QSignalSpy changed{ &manager, &TabGroupManager::groupsChanged };
    pending->notify();
    CHECK( changed.count() == 0 );
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
