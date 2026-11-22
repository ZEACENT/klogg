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
#include <QPointer>

#include "containers.h"
#include <QtGlobal>

#include "log.h"

#include "qtcompat/qtcompat.h"

struct TabGroupManager::PreparedRemoval::State {
    QPointer<TabGroupManager> owner;
    std::shared_ptr<const uint8_t> base;
    std::shared_ptr<const uint8_t> next;
    QList<TabGroup> candidate;
    bool changed = false;
    bool installed = false;
    bool notified = false;
};
TabGroupManager::PreparedRemoval::PreparedRemoval( std::unique_ptr<State> state )
    : state_( std::move( state ) ) {}
TabGroupManager::PreparedRemoval::~PreparedRemoval() = default;

bool TabGroupManager::PreparedRemoval::isCurrent() const noexcept
{
    return !state_->installed && state_->owner
        && state_->owner->mutationIdentity_ == state_->base;
}

bool TabGroupManager::PreparedRemoval::install() noexcept
{
    if ( !isCurrent() ) return false;
    if ( state_->changed ) {
        state_->owner->groups_.swap( state_->candidate );
        state_->owner->mutationIdentity_ = std::move( state_->next );
    }
    state_->installed = true;
    return true;
}

void TabGroupManager::PreparedRemoval::notify()
{
    if ( !state_->installed || state_->notified ) return;
    state_->notified = true;
    const auto owner = state_->owner;
    const bool changed = state_->changed;
    // The observer may destroy either the update or its owner. No member access
    // follows publication; the signal describes the owner's current state.
    if ( changed && owner ) Q_EMIT owner->groupsChanged();
}

std::unique_ptr<TabGroupManager::PreparedRemoval>
TabGroupManager::prepareTabRemovals( const QStringList& paths )
{
    auto state = std::make_unique<PreparedRemoval::State>();
    state->owner = this;
    state->base = mutationIdentity_;
    state->candidate = groups_;
    for ( const auto& path : paths ) {
        for ( klogg::ContainerIndex index = 0; index < state->candidate.size(); ++index ) {
            // Do not detach/copy a shared group list for an absent path.
            if ( !state->candidate.at( index ).tabPaths.contains( path ) ) continue;
            auto& group = state->candidate[ index ];
            group.tabPaths.removeAll( path );
            if ( group.tabPaths.isEmpty() ) state->candidate.removeAt( index );
            state->changed = true;
            break;
        }
    }
    if ( state->changed ) state->next = std::make_shared<const uint8_t>( 0 );
    return std::unique_ptr<PreparedRemoval>( new PreparedRemoval( std::move( state ) ) );
}

void TabGroupManager::advanceMutationIdentity()
{
    mutationIdentity_ = std::make_shared<const uint8_t>( 0 );
}

QList<TabGroup> TabGroupManager::groups() const
{
    return groups_;
}

TabGroup* TabGroupManager::mutableGroupById( const QString& groupId )
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

const TabGroup* TabGroupManager::groupForTab( const QString& tabPath ) const
{
    const auto match = std::find_if( groups_.cbegin(), groups_.cend(),
                                     [ & ]( const TabGroup& group ) {
                                         return group.tabPaths.contains( tabPath );
                                     } );
    return match == groups_.cend() ? nullptr : &*match;
}

void TabGroupManager::createGroup( const QString& name, const QColor& color )
{
    advanceMutationIdentity();
    groups_.append( TabGroup( name, color ) );
    Q_EMIT groupsChanged();
}

void TabGroupManager::deleteGroup( const QString& groupId )
{
    for ( int i = 0; i < groups_.size(); ++i ) {
        if ( groups_[ i ].id == groupId ) {
            advanceMutationIdentity();
            groups_.removeAt( i );
            Q_EMIT groupsChanged();
            return;
        }
    }
}

void TabGroupManager::renameGroup( const QString& groupId, const QString& newName )
{
    if ( auto* group = mutableGroupById( groupId ) ) {
        advanceMutationIdentity();
        group->name = newName;
        Q_EMIT groupsChanged();
    }
}

void TabGroupManager::setGroupColor( const QString& groupId, const QColor& color )
{
    if ( auto* group = mutableGroupById( groupId ) ) {
        advanceMutationIdentity();
        group->color = color;
        Q_EMIT groupsChanged();
    }
}

void TabGroupManager::toggleCollapsed( const QString& groupId )
{
    if ( auto* group = mutableGroupById( groupId ) ) {
        advanceMutationIdentity();
        group->collapsed = !group->collapsed;
        Q_EMIT this->groupCollapsedChanged( groupId, group->collapsed );
        Q_EMIT groupsChanged();
    }
}

void TabGroupManager::addTabToGroup( const QString& groupId, const QString& tabPath )
{
    // First remove from any existing group
    removeTabFromGroup( tabPath );

    if ( auto* group = mutableGroupById( groupId ) ) {
        if ( !group->tabPaths.contains( tabPath ) ) {
            advanceMutationIdentity();
            group->tabPaths.append( tabPath );
            Q_EMIT groupsChanged();
        }
    }
}

void TabGroupManager::removeTabFromGroup( const QString& tabPath )
{
    auto update = prepareTabRemovals( { tabPath } );
    if ( update->install() ) update->notify();
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
    deleteGroup( groupId );
}

void TabGroupManager::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "TabGroupManager::retrieveFromStorage";

    advanceMutationIdentity();
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
