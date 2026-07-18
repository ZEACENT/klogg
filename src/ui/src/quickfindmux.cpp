/*
 * Copyright (C) 2013, 2014 Nicolas Bonnefon and other contributors
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

#include "log.h"

#include "configuration.h"
#include "persistentinfo.h"
#include "quickfindmux.h"

#include "qfnotifications.h"

QuickFindMux::QuickFindMux( const std::shared_ptr<QuickFindPattern>& pattern )
    : QObject()
    , pattern_( pattern )
    , registeredSearchables_()
{
    selector_ = nullptr;

    // Forward the pattern's signal to our listeners
    connect( pattern_.get(), SIGNAL( patternUpdated() ), this, SLOT( notifyPatternChanged() ) );
}

//
// Public member functions
//
void QuickFindMux::registerSelector( const QuickFindMuxSelectorInterface* selector )
{
    LOG_DEBUG << "QuickFindMux::registerSelector";

    // The selector object we will use when forwarding search requests
    selector_ = selector;

    unregisterAllSearchables();

    if ( !selector ) {
        return;
    }

    for ( auto i : selector_->getAllSearchables() )
        registerSearchable( i );
}

void QuickFindMux::setDirection( QFDirection direction )
{
    LOG_DEBUG << "QuickFindMux::setDirection: new direction: " << direction;
    currentDirection_ = direction;
}

//
// Public Q_SLOTS:
//
void QuickFindMux::searchNext()
{
    LOG_DEBUG << "QuickFindMux::searchNext";
    if ( currentDirection_ == Forward )
        searchForward();
    else
        searchBackward();
}

void QuickFindMux::searchPrevious()
{
    LOG_DEBUG << "QuickFindMux::searchPrevious";
    if ( currentDirection_ == Forward )
        searchBackward();
    else
        searchForward();
}

void QuickFindMux::searchForward()
{
    LOG_DEBUG << "QuickFindMux::searchForward";

    if ( auto searchable = getSearchableWidget() )
        searchable->searchForward();
}

void QuickFindMux::searchBackward()
{
    LOG_DEBUG << "QuickFindMux::searchBackward";

    if ( auto searchable = getSearchableWidget() )
        searchable->searchBackward();
}

void QuickFindMux::setNewPattern( const QString& newPattern, bool ignoreCase, bool isRegexSearch, bool isWholeWord )
{
    const auto& config = Configuration::get();

    LOG_DEBUG << "QuickFindMux::setNewPattern";

    // If we must do an incremental search, we do it now
    if ( config.isQuickfindIncremental() ) {
        pattern_->changeSearchPattern( newPattern, ignoreCase, isRegexSearch, isWholeWord );
        if ( auto searchable = getSearchableWidget() ) {
            if ( currentDirection_ == Forward )
                searchable->incrementallySearchForward();
            else
                searchable->incrementallySearchBackward();
        }
    }
}

void QuickFindMux::confirmPattern( const QString& newPattern, bool ignoreCase, bool isRegexSearch, bool isWholeWord )
{
    pattern_->changeSearchPattern( newPattern, ignoreCase, isRegexSearch, isWholeWord );

    if ( Configuration::get().isQuickfindIncremental() ) {
        if ( auto searchable = getSearchableWidget() )
            searchable->incrementalSearchStop();
    }
}

void QuickFindMux::cancelSearch()
{
    if ( Configuration::get().isQuickfindIncremental() ) {
        if ( auto searchable = getSearchableWidget() )
            searchable->incrementalSearchAbort();
    }
}

//
// Private Q_SLOTS:
//
void QuickFindMux::changeQuickFind( const QString& new_pattern, QFDirection new_direction )
{
    pattern_->changeSearchPattern( new_pattern );
    setDirection( new_direction );
}

void QuickFindMux::notifyPatternChanged()
{
    Q_EMIT patternChanged( pattern_->getPattern(), !pattern_->isCaseSensitive(), pattern_->isRegex(), pattern_->isWholeWord() );
}

//
// Private member functions
//

// Use the registered 'selector' to determine where to send the search requests.
SearchableWidgetInterface* QuickFindMux::getSearchableWidget() const
{
    LOG_DEBUG << "QuickFindMux::getSearchableWidget";

    SearchableWidgetInterface* searchable = nullptr;

    if ( selector_ )
        searchable = selector_->getActiveSearchable();
    else
        LOG_WARNING << "QuickFindMux::getActiveSearchable() no registered selector";

    return searchable;
}

void QuickFindMux::registerSearchable( QObject* searchable )
{
    LOG_DEBUG << "QuickFindMux::registerSearchable";

    // The searchable can change our qf pattern
    connect( searchable, SIGNAL( changeQuickFind( const QString&, QuickFindMux::QFDirection ) ),
             this, SLOT( changeQuickFind( const QString&, QuickFindMux::QFDirection ) ) );
    // Send us notifications
    connect( searchable, SIGNAL( notifyQuickFind( const QFNotification& ) ), this,
             SIGNAL( notify( const QFNotification& ) ) );

    // And clear them
    connect( searchable, SIGNAL( clearQuickFindNotification() ), this,
             SIGNAL( clearNotification() ) );
    // Search can be initiated by the view itself
    connect( searchable, SIGNAL( searchNext() ), this, SLOT( searchNext() ) );
    connect( searchable, SIGNAL( searchPrevious() ), this, SLOT( searchPrevious() ) );

    registeredSearchables_.push_back( searchable );
}

void QuickFindMux::unregisterAllSearchables()
{
    for ( const auto& searchable : registeredSearchables_ ) {
        // Entries may have been destroyed with their owning document (e.g. a
        // closed folder results pane); disconnect only live ones.
        if ( searchable != nullptr ) {
            disconnect( searchable, nullptr, this, nullptr );
        }
    }

    registeredSearchables_.clear();
}
