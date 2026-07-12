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

#include <QSettings>
#include <algorithm>

#include "configuration.h"
#include "containers.h"
#include "log.h"
#include "recentfolders.h"

void RecentFolders::removeRecent( const QString& text )
{
    recentFolders_.removeAll( text );
}

void RecentFolders::removeAll()
{
    recentFolders_.clear();
    recentFolders_.reserve( MAX_RECENT_FILES );
}
void RecentFolders::addRecent( const QString& text )
{
    // Remove any copy of the about to be added folder path
    removeRecent( text );

    // Add at the front
    recentFolders_.push_front( text );

    // Trim the list if it's too long
    while ( recentFolders_.size() > MAX_RECENT_FILES )
        recentFolders_.pop_back();
}

int RecentFolders::getNumberItemsToShow() const
{
    return std::min( klogg::isize( recentFolders_ ), foldersHistoryMaxItemsToShow_ );
}

int RecentFolders::foldersHistoryMaxItems() const
{
    return foldersHistoryMaxItemsToShow_;
}

void RecentFolders::setFoldersHistoryMaxItems( const int recentMaxFoldersToShow )
{
    if ( recentMaxFoldersToShow > 0 ) {
        foldersHistoryMaxItemsToShow_ = std::clamp( recentMaxFoldersToShow, 0, MAX_RECENT_FILES );
    }
}

QStringList RecentFolders::recentFolders() const
{
    return recentFolders_;
}

//
// Persistable virtual functions implementation
//

void RecentFolders::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "RecentFolders::saveToStorage";

    settings.beginGroup( "RecentFolders" );
    settings.setValue( "version", RECENTFOLDERS_VERSION );
    settings.remove( "foldersHistory" );
    settings.beginWriteArray( "foldersHistory" );
    for ( int i = 0; i < recentFolders_.size(); ++i ) {
        settings.setArrayIndex( i );
        settings.setValue( "name", recentFolders_.at( i ) );
    }
    settings.endArray();
    settings.setValue( "maxMenuItems", foldersHistoryMaxItemsToShow_ );
    settings.endGroup();
}

void RecentFolders::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "RecentFolders::retrieveFromStorage";

    removeAll();

    if ( settings.contains( "RecentFolders/version" ) ) {
        settings.beginGroup( "RecentFolders" );
        if ( settings.value( "version" ).toInt() == RECENTFOLDERS_VERSION ) {
            int size = settings.beginReadArray( "foldersHistory" );
            size = std::min( size, MAX_RECENT_FILES );
            for ( int i = 0; i < size; ++i ) {
                settings.setArrayIndex( i );
                QString folder = settings.value( "name" ).toString();
                recentFolders_.append( folder );
            }
            settings.endArray();
        }
        else {
            LOG_ERROR << "Unknown version of recent folders, ignoring it...";
        }
        setFoldersHistoryMaxItems(
            settings.value( "maxMenuItems", DEFAULT_MAX_ITEMS_TO_SHOW ).toInt() );
        settings.endGroup();
    }
}
