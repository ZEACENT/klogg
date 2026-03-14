/*
 * Copyright (C) 2011, 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sessioninfo.h"

#include <QSettings>

#include "log.h"

constexpr int OPENFILES_VERSION = 2;
constexpr int SESSION_VERSION = 2;
namespace {
constexpr auto WINDOWS_ARRAY_KEY = "windowList";
constexpr auto LEGACY_WINDOWS_ARRAY_KEY = "windows";
constexpr auto OPENFILES_ARRAY_KEY = "entries";
constexpr auto LEGACY_OPENFILES_ARRAY_KEY = "openFiles";
}

void SessionInfo::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "SessionInfo::retrieveFromStorage";

    windows_.clear();
    dirtyShutdown_ = false;

    settings.beginGroup( "Window" );

    const auto sessionVersion = settings.value( "version", 0 ).toInt();
    if ( sessionVersion == 1 || sessionVersion == SESSION_VERSION ) {
        dirtyShutdown_ = settings.value( "dirtyShutdown", false ).toBool();
        auto windowsCount = settings.beginReadArray( WINDOWS_ARRAY_KEY );
        if ( windowsCount == 0 ) {
            settings.endArray();
            windowsCount = settings.beginReadArray( LEGACY_WINDOWS_ARRAY_KEY );
        }
        for ( auto windowIndex = 0; windowIndex < windowsCount; ++windowIndex ) {
            settings.setArrayIndex( static_cast<int>( windowIndex ) );
            QString windowId = settings.value( "id" ).toString();
            auto window = Window{ windowId };
            window.geometry = settings.value( "geometry" ).toByteArray();
            window.currentFileIndex = settings.value( "currentFileIndex", -1 ).toInt();

            if ( settings.contains( "OpenFiles/version" ) ) {
                settings.beginGroup( "OpenFiles" );
                const auto openFilesVersion = settings.value( "version" ).toInt();
                if ( openFilesVersion == 1 || openFilesVersion == OPENFILES_VERSION ) {
                    int size = settings.beginReadArray( OPENFILES_ARRAY_KEY );
                    if ( size == 0 ) {
                        settings.endArray();
                        size = settings.beginReadArray( LEGACY_OPENFILES_ARRAY_KEY );
                    }
                    LOG_DEBUG << "SessionInfo: " << size << " files.";
                    for ( int i = 0; i < size; ++i ) {
                        settings.setArrayIndex( i );
                        QString file_name = settings.value( "fileName" ).toString();
                        uint64_t top_line = settings.value( "topLine" ).toULongLong();
                        QString view_context = settings.value( "viewContext" ).toString();
                        const auto sourceType
                            = openFilesVersion >= 2 ? settings.value( "sourceType" ).toString()
                                                    : QString{};
                        const auto displayName
                            = openFilesVersion >= 2 ? settings.value( "displayName" ).toString()
                                                    : QString{};
                        const auto sourceSpec
                            = openFilesVersion >= 2 ? settings.value( "sourceSpec" ).toString()
                                                    : QString{};
                        window.openFiles.emplace_back( file_name, top_line, view_context,
                                                       sourceType, displayName, sourceSpec );
                    }
                    settings.endArray();
                }
                else {
                    LOG_ERROR << "Unknown version of OpenFiles, ignoring it...";
                }
                settings.endGroup();
            }

            LOG_INFO << "Loaded settings for window session " << windowId;
            windows_.emplace_back( window );
        }
        settings.endArray();
    }
    else {
        LOG_ERROR << "Unknown version of session, ignoring it...";
    }

    settings.endGroup();
}

void SessionInfo::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "SessionInfo::saveToStorage";

    settings.beginGroup( "Window" );
    settings.setValue( "version", SESSION_VERSION );
    settings.setValue( "dirtyShutdown", dirtyShutdown_ );

    // "Window/windows" is ambiguous on Windows INI backend because keys are
    // case-insensitive. Use a distinct key while still reading legacy data.
    settings.beginWriteArray( WINDOWS_ARRAY_KEY );
    for ( auto windowIndex = 0u; windowIndex < windows_.size(); ++windowIndex ) {
        const auto& window = windows_.at( windowIndex );
        settings.setArrayIndex( static_cast<int>( windowIndex ) );

        settings.setValue( "id", window.id );
        settings.setValue( "geometry", window.geometry );
        settings.setValue( "currentFileIndex", window.currentFileIndex );

        settings.beginGroup( "OpenFiles" );
        settings.setValue( "version", OPENFILES_VERSION );
        settings.beginWriteArray( OPENFILES_ARRAY_KEY );
        for ( unsigned i = 0; i < window.openFiles.size(); ++i ) {
            settings.setArrayIndex( static_cast<int>( i ) );
            const OpenFile* open_file = &( window.openFiles.at( i ) );
            settings.setValue( "fileName", open_file->fileName );
            settings.setValue( "topLine", qint64( open_file->topLine ) );
            settings.setValue( "viewContext", open_file->viewContext );
            settings.setValue( "sourceType", open_file->sourceType );
            settings.setValue( "displayName", open_file->displayName );
            settings.setValue( "sourceSpec", open_file->sourceSpec );
        }
        settings.endArray();
        settings.endGroup(); // OpenFiles
    }
    settings.endArray();
    settings.endGroup(); // Win
    settings.sync();
}
