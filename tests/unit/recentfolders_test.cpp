/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
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
#include "recentfolders.h"

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

TEST_CASE( "RecentFolders::addRecent puts latest first", "[folder]" )
{
    RecentFolders folders;
    folders.removeAll();

    folders.addRecent( "/tmp/a" );
    folders.addRecent( "/tmp/b" );
    folders.addRecent( "/tmp/c" );

    const auto list = folders.recentFolders();
    REQUIRE( list.size() == 3 );
    REQUIRE( list.at( 0 ) == "/tmp/c" );
    REQUIRE( list.at( 1 ) == "/tmp/b" );
    REQUIRE( list.at( 2 ) == "/tmp/a" );
}

TEST_CASE( "RecentFolders::addRecent moves an existing folder to front and removes the duplicate",
           "[folder]" )
{
    RecentFolders folders;
    folders.removeAll();

    folders.addRecent( "/tmp/a" );
    folders.addRecent( "/tmp/b" );
    folders.addRecent( "/tmp/c" );

    // Re-add the oldest; it must jump to the front and leave no duplicate.
    folders.addRecent( "/tmp/a" );

    const auto list = folders.recentFolders();
    REQUIRE( list.size() == 3 );
    REQUIRE( list.at( 0 ) == "/tmp/a" );
    REQUIRE( list.at( 1 ) == "/tmp/c" );
    REQUIRE( list.at( 2 ) == "/tmp/b" );
    // No duplicate anywhere.
    REQUIRE( list.count( "/tmp/a" ) == 1 );
}

TEST_CASE( "RecentFolders trims the list to MAX_RECENT_FILES when exceeded", "[folder]" )
{
    RecentFolders folders;
    folders.removeAll();

    // Insert more than the cap (MAX_RECENT_FILES == 25).
    for ( int i = 0; i < MAX_RECENT_FILES + 5; ++i ) {
        folders.addRecent( QStringLiteral( "/tmp/folder_%1" ).arg( i ) );
    }

    const auto list = folders.recentFolders();
    REQUIRE( list.size() == MAX_RECENT_FILES );
    // The most recently added is at the front.
    REQUIRE( list.at( 0 )
             == QStringLiteral( "/tmp/folder_%1" ).arg( MAX_RECENT_FILES + 5 - 1 ) );
    // The oldest (folder_4) is the first to be evicted; folder_5 is the tail.
    REQUIRE( list.last()
             == QStringLiteral( "/tmp/folder_%1" ).arg( 5 ) );
}

TEST_CASE( "RecentFolders::removeAll clears the list", "[folder]" )
{
    RecentFolders folders;
    folders.addRecent( "/tmp/a" );
    folders.addRecent( "/tmp/b" );
    REQUIRE( folders.recentFolders().size() == 2 );

    folders.removeAll();
    REQUIRE( folders.recentFolders().isEmpty() );
}

TEST_CASE( "RecentFolders::removeRecent removes a single entry", "[folder]" )
{
    RecentFolders folders;
    folders.removeAll();
    folders.addRecent( "/tmp/a" );
    folders.addRecent( "/tmp/b" );

    folders.removeRecent( "/tmp/a" );

    const auto list = folders.recentFolders();
    REQUIRE( list.size() == 1 );
    REQUIRE( list.at( 0 ) == "/tmp/b" );
}

TEST_CASE( "RecentFolders persists and restores the list across sessions", "[folder]" )
{
    const auto dirPath = makeTestDir( "recentfolders" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "recentfolders.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        RecentFolders folders;
        folders.removeAll();
        folders.addRecent( "/var/log" );
        folders.addRecent( "/tmp/klogg" );
        folders.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    // A fresh instance reads back the same list, latest first.
    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    RecentFolders restored;
    restored.retrieveFromStorage( restoredSettings );

    const auto list = restored.recentFolders();
    REQUIRE( list.size() == 2 );
    REQUIRE( list.at( 0 ) == "/tmp/klogg" );
    REQUIRE( list.at( 1 ) == "/var/log" );
}

TEST_CASE( "RecentFolders round-trips the maxMenuItems setting", "[folder]" )
{
    const auto dirPath = makeTestDir( "recentfolders_max" );
    const auto settingsPath = QDir{ dirPath }.filePath( "recentfolders_max.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );
        RecentFolders folders;
        folders.removeAll();
        folders.addRecent( "/tmp/a" );
        folders.setFoldersHistoryMaxItems( 10 );
        folders.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    RecentFolders restored;
    restored.retrieveFromStorage( restoredSettings );

    REQUIRE( restored.foldersHistoryMaxItems() == 10 );
    REQUIRE( restored.getNumberItemsToShow() == 1 ); // only one folder stored
}
