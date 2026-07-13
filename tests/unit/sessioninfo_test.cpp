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
#include <QUuid>

#include "sessioninfo.h"

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

TEST_CASE( "SessionInfo stores and restores current tab index and dirty shutdown flag" )
{
    const auto dirPath = makeTestDir( "sessioninfo" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "sessioninfo.ini" );
    const auto geometry = QByteArray::fromHex( "6b6c6f6767" ); // "klogg"

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        SessionInfo sessionInfo;
        sessionInfo.add( "window-1" );
        sessionInfo.setGeometry( "window-1", geometry );
        sessionInfo.setOpenFiles( "window-1",
                                  {
                                      SessionInfo::OpenFile{ "/tmp/a.log", 17, "ctx-a" },
                                      SessionInfo::OpenFile{ "/tmp/b.log", 29, "ctx-b" },
                                  } );
        sessionInfo.setCurrentFileIndex( "window-1", 1 );
        sessionInfo.setDirtyShutdown( true );

        sessionInfo.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    SessionInfo restoredSession;
    restoredSession.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredSession.hadUncleanShutdown() );
    REQUIRE( restoredSession.windows().contains( "window-1" ) );
    REQUIRE( restoredSession.geometry( "window-1" ) == geometry );
    REQUIRE( restoredSession.currentFileIndex( "window-1" ) == 1 );

    const auto restoredOpenFiles = restoredSession.openFiles( "window-1" );
    REQUIRE( restoredOpenFiles.size() == 2 );
    REQUIRE( restoredOpenFiles.at( 0 ).fileName == "/tmp/a.log" );
    REQUIRE( restoredOpenFiles.at( 0 ).topLine == 17 );
    REQUIRE( restoredOpenFiles.at( 0 ).viewContext == "ctx-a" );
    REQUIRE( restoredOpenFiles.at( 1 ).fileName == "/tmp/b.log" );
    REQUIRE( restoredOpenFiles.at( 1 ).topLine == 29 );
    REQUIRE( restoredOpenFiles.at( 1 ).viewContext == "ctx-b" );
}

TEST_CASE( "SessionInfo can read legacy v1 session format without currentFileIndex" )
{
    const auto dirPath = makeTestDir( "sessioninfo_v1" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "sessioninfo-v1.ini" );

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        settings.beginGroup( "Window" );
        settings.setValue( "version", 1 );
        settings.beginWriteArray( "windows" );
        settings.setArrayIndex( 0 );
        settings.setValue( "id", "legacy-window" );
        settings.setValue( "geometry", QByteArray::fromHex( "31" ) );
        settings.beginGroup( "OpenFiles" );
        settings.setValue( "version", 1 );
        settings.beginWriteArray( "openFiles" );
        settings.setArrayIndex( 0 );
        settings.setValue( "fileName", "/tmp/legacy.log" );
        settings.setValue( "topLine", 7 );
        settings.setValue( "viewContext", "legacy-ctx" );
        settings.endArray();
        settings.endGroup();
        settings.endArray();
        settings.endGroup();
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    SessionInfo restoredSession;
    restoredSession.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredSession.windows().contains( "legacy-window" ) );
    REQUIRE( restoredSession.currentFileIndex( "legacy-window" ) == -1 );

    const auto restoredOpenFiles = restoredSession.openFiles( "legacy-window" );
    REQUIRE( restoredOpenFiles.size() == 1 );
    REQUIRE( restoredOpenFiles.at( 0 ).fileName == "/tmp/legacy.log" );
    REQUIRE( restoredOpenFiles.at( 0 ).topLine == 7 );
    REQUIRE( restoredOpenFiles.at( 0 ).viewContext == "legacy-ctx" );
}

TEST_CASE( "SessionInfo round-trips a folder-tagged document via sourceType", "[folder]" )
{
    // Proves the existing v2 schema carries a folder document (sourceType=
    // "folder", fileName = folder path, plus a view-context blob) through
    // saveToStorage -> retrieveFromStorage with no format change. This is the
    // persistence contract the folder session save/restore path relies on.
    const auto dirPath = makeTestDir( "sessioninfo_folder" );
    REQUIRE( QDir{ dirPath }.exists() );
    const auto settingsPath = QDir{ dirPath }.filePath( "sessioninfo-folder.ini" );

    const QString folderPath = "/var/log/myapp";
    const QString viewContext = R"({"P":"ERROR","IC":true,"RE":false,"IR":false,"BC":false})";

    {
        QSettings settings( settingsPath, QSettings::IniFormat );

        SessionInfo sessionInfo;
        sessionInfo.add( "window-folder" );
        sessionInfo.setOpenFiles(
            "window-folder",
            { SessionInfo::OpenFile{ folderPath, 0, viewContext,
                                     /*sourceType=*/QStringLiteral( "folder" ),
                                     /*displayName=*/QStringLiteral( "myapp" ),
                                     /*sourceSpec=*/QString{} } } );
        sessionInfo.setCurrentFileIndex( "window-folder", 0 );
        sessionInfo.saveToStorage( settings );
        settings.sync();
        REQUIRE( settings.status() == QSettings::NoError );
    }

    QSettings restoredSettings( settingsPath, QSettings::IniFormat );
    SessionInfo restoredSession;
    restoredSession.retrieveFromStorage( restoredSettings );

    REQUIRE( restoredSession.windows().contains( "window-folder" ) );
    const auto restoredOpenFiles = restoredSession.openFiles( "window-folder" );
    REQUIRE( restoredOpenFiles.size() == 1 );
    REQUIRE( restoredOpenFiles.at( 0 ).fileName == folderPath );
    REQUIRE( restoredOpenFiles.at( 0 ).sourceType == QStringLiteral( "folder" ) );
    REQUIRE( restoredOpenFiles.at( 0 ).displayName == QStringLiteral( "myapp" ) );
    REQUIRE( restoredOpenFiles.at( 0 ).viewContext == viewContext );
}
