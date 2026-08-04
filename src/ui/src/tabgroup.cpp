/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
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

#include "tabgroup.h"

#include <algorithm>

#include <QSettings>
#include <QtGlobal>

#include "log.h"

#include "qtcompat/qtcompat.h"

QList<TabGroup> TabGroupManager::groups() const
{
    return groups_;
}

TabGroup* TabGroupManager::groupById( const QString& groupId )
{
    const auto match = std::find_if( groups_.begin(), groups_.end(),
                                     [ & ]( const TabGroup& group ) {
                                         return group.id == groupId;
                                     } );
    return match == groups_.end() ? nullptr : &*match;
}

const TabGroup* TabGroupManager::groupById( const QString& groupId ) const
{
    const auto match = std::find_if( groups_.cbegin(), groups_.cend(),
                                     [ & ]( const TabGroup& group ) {
                                         return group.id == groupId;
                                     } );
    return match == groups_.cend() ? nullptr : &*match;
}

QString TabGroupManager::groupIdForTab( const QString& tabPath ) const
{
    const auto match = std::find_if( groups_.cbegin(), groups_.cend(),
                                     [ & ]( const TabGroup& group ) {
                                         return group.tabPaths.contains( tabPath );
                                     } );
    return match == groups_.cend() ? QString{} : match->id;
}

TabGroup* TabGroupManager::groupForTab( const QString& tabPath )
{
    const auto match = std::find_if( groups_.begin(), groups_.end(),
                                     [ & ]( const TabGroup& group ) {
                                         return group.tabPaths.contains( tabPath );
                                     } );
    return match == groups_.end() ? nullptr : &*match;
}

void TabGroupManager::createGroup( const QString& name, const QColor& color )
{
    groups_.append( TabGroup( name, color ) );
    Q_EMIT groupsChanged();
}

void TabGroupManager::deleteGroup( const QString& groupId )
{
    for ( int i = 0; i < groups_.size(); ++i ) {
        if ( groups_[ i ].id == groupId ) {
            groups_.removeAt( i );
            Q_EMIT groupsChanged();
            return;
        }
    }
}

void TabGroupManager::renameGroup( const QString& groupId, const QString& newName )
{
    if ( auto* group = groupById( groupId ) ) {
        group->name = newName;
        Q_EMIT groupsChanged();
    }
}

void TabGroupManager::setGroupColor( const QString& groupId, const QColor& color )
{
    if ( auto* group = groupById( groupId ) ) {
        group->color = color;
        Q_EMIT groupsChanged();
    }
}

void TabGroupManager::toggleCollapsed( const QString& groupId )
{
    if ( auto* group = groupById( groupId ) ) {
        group->collapsed = !group->collapsed;
        Q_EMIT groupCollapsedChanged( groupId, group->collapsed );
        Q_EMIT groupsChanged();
    }
}

void TabGroupManager::addTabToGroup( const QString& groupId, const QString& tabPath )
{
    // First remove from any existing group
    removeTabFromGroup( tabPath );

    if ( auto* group = groupById( groupId ) ) {
        if ( !group->tabPaths.contains( tabPath ) ) {
            group->tabPaths.append( tabPath );
            Q_EMIT groupsChanged();
        }
    }
}

void TabGroupManager::removeTabFromGroup( const QString& tabPath )
{
    for ( int i = 0; i < groups_.size(); ++i ) {
        auto& group = groups_[ i ];
        if ( group.tabPaths.removeAll( tabPath ) > 0 ) {
            if ( group.tabPaths.isEmpty() ) {
                groups_.removeAt( i );
            }
            Q_EMIT groupsChanged();
            return;
        }
    }
}

void TabGroupManager::moveTabToGroup( const QString& tabPath, const QString& newGroupId )
{
    removeTabFromGroup( tabPath );
    if ( !newGroupId.isEmpty() ) {
        addTabToGroup( newGroupId, tabPath );
    }
}

void TabGroupManager::ungroupAll( const QString& groupId )
{
    if ( auto* group = groupById( groupId ) ) {
        group->tabPaths.clear();
        deleteGroup( groupId );
    }
}

void TabGroupManager::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "TabGroupManager::retrieveFromStorage";

    groups_.clear();

    const int groupCount = settings.beginReadArray( "tabGroups" );
    for ( int i = 0; i < groupCount; ++i ) {
        settings.setArrayIndex( i );

        TabGroup group;
        group.id = settings.value( "id" ).toString();
        group.name = settings.value( "name" ).toString();
        group.color = QColor( settings.value( "color", "#5B8CFF" ).toString() );
        group.collapsed = settings.value( "collapsed", false ).toBool();
        const auto tabPathsValue = settings.value( "tabPaths" );
        group.tabPaths = tabPathsValue.toStringList();
        if ( group.tabPaths.isEmpty() ) {
            const auto tabPathsText = settings.value( "tabPathsText" ).toString();
            if ( !tabPathsText.isEmpty() ) {
                group.tabPaths = tabPathsText.split( QLatin1Char( '\n' ), klogg::qtcompat::skipEmptyParts() );
            }
        }

        if ( !group.id.isEmpty() && !group.name.isEmpty() && !group.tabPaths.isEmpty() ) {
            groups_.append( group );
        }
    }
    settings.endArray();
}

void TabGroupManager::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "TabGroupManager::saveToStorage";

    settings.remove( "tabGroups" );
    settings.beginWriteArray( "tabGroups" );
    for ( int i = 0; i < groups_.size(); ++i ) {
        settings.setArrayIndex( i );
        const auto& group = groups_[ i ];
        settings.setValue( "id", group.id );
        settings.setValue( "name", group.name );
        settings.setValue( "color", group.color.name( QColor::HexArgb ) );
        settings.setValue( "collapsed", group.collapsed );
        settings.setValue( "tabPaths", group.tabPaths );
        settings.setValue( "tabPathsText", group.tabPaths.join( QLatin1Char( '\n' ) ) );
    }
    settings.endArray();
    settings.sync();
}
