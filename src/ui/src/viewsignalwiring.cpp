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

#include "viewsignalwiring.h"

#include <algorithm>

#include "abstractlogview.h"
#include "configuration.h"
#include "fontutils.h"
#include "searchtoolbar.h"

ViewSignalWiring::ViewSignalWiring( QObject* parent, SearchToolbar* searchToolbar, Hooks hooks )
    : QObject( parent )
    , searchToolbar_( searchToolbar )
    , hooks_( std::move( hooks ) )
{
}

void ViewSignalWiring::wireView( AbstractLogView* view )
{
    if ( view == nullptr ) {
        return;
    }

    if ( hooks_.sendToScratchpad ) {
        connect( view, &AbstractLogView::sendSelectionToScratchpad, this,
                 [ this, view ]() { hooks_.sendToScratchpad( view->getSelectedText() ); } );
    }
    if ( hooks_.replaceScratchpad ) {
        connect( view, &AbstractLogView::replaceScratchpadWithSelection, this,
                 [ this, view ]() { hooks_.replaceScratchpad( view->getSelectedText() ); } );
    }

    connect( view, QOverload<const QString&>::of( &AbstractLogView::addToSearch ), this,
             [ this ]( const QString& selection ) { addToSearch( selection ); } );
    connect( view, QOverload<const QString&>::of( &AbstractLogView::replaceSearch ), this,
             [ this ]( const QString& selection ) { replaceSearch( selection ); } );
    connect( view, QOverload<const QString&>::of( &AbstractLogView::excludeFromSearch ), this,
             [ this ]( const QString& selection ) { excludeFromSearch( selection ); } );

    if ( hooks_.saveSplitterSizes ) {
        connect( view, &AbstractLogView::saveDefaultSplitterSizes, this,
                 [ this ]() { hooks_.saveSplitterSizes(); } );
    }

    connect( view, &AbstractLogView::changeFontSize, this,
             [ this ]( bool increase ) { changeFontSize( increase ); } );

    if ( hooks_.exitView ) {
        connect( view, &AbstractLogView::exitView, this,
                 [ this, view ]() { hooks_.exitView( view ); } );
    }

    if ( hooks_.applyConfiguration ) {
        connect( view, &AbstractLogView::highlightersChange, this,
                 [ this ]() { hooks_.applyConfiguration(); } );
    }

    views_.push_back( view );
}

void ViewSignalWiring::wireHover( AbstractLogView* view )
{
    if ( view == nullptr ) {
        return;
    }

    if ( hooks_.hoveredOverLine ) {
        connect( view, &AbstractLogView::mouseHoveredOverLine, this,
                 [ this, view ]( LineNumber line ) { hooks_.hoveredOverLine( view, line ); } );
    }
    if ( hooks_.leftHoveringZone ) {
        connect( view, &AbstractLogView::mouseLeftHoveringZone, this,
                 [ this ]() { hooks_.leftHoveringZone(); } );
    }
}

void ViewSignalWiring::addToSearch( const QString& searchString )
{
    // An empty selection (e.g. a folder group-header row, which is not
    // copyable) composes nothing; appending it would corrupt the pattern
    // ("X or " / "X|" match-all).
    if ( searchString.isEmpty() ) {
        return;
    }
    const auto newPattern = searchToolbar_->escapeSearchPattern( searchString );
    QString currentPattern = searchToolbar_->currentSearchText();
    searchToolbar_->setSearchPattern(
        searchToolbar_->combinePatterns( currentPattern, newPattern ) );
}

void ViewSignalWiring::excludeFromSearch( const QString& searchString )
{
    // See addToSearch: an empty exclusion would produce "X and not()" which
    // matches nothing.
    if ( searchString.isEmpty() ) {
        return;
    }
    QString currentPattern = searchToolbar_->currentSearchText();

    const auto wasInBooleanCombinationMode = searchToolbar_->isBoolean();
    if ( !wasInBooleanCombinationMode ) {
        // Wrap the existing pattern as one boolean operand. Must backslash-escape
        // embedded double-quotes (not the no-op replace('"',"\"")); reuse the
        // shared helper so this can't drift from escapeSearchPattern again.
        currentPattern = searchToolbar_->wrapBooleanOperand( currentPattern );
    }

    searchToolbar_->setBoolean( true );

    const auto newPattern = searchToolbar_->escapeSearchPattern( searchString );

    if ( !currentPattern.isEmpty() ) {
        currentPattern.append( " and " );
    }

    currentPattern.append( "not(" ).append( newPattern ).append( ')' );
    searchToolbar_->setSearchPattern( currentPattern );
}

void ViewSignalWiring::replaceSearch( const QString& searchString )
{
    // See addToSearch: an empty replacement would clear the pattern (or
    // compose a match-all quoted-empty operand in boolean mode).
    if ( searchString.isEmpty() ) {
        return;
    }
    searchToolbar_->setSearchPattern( searchToolbar_->escapeSearchPattern( searchString ) );
}

void ViewSignalWiring::changeFontSize( bool increase )
{
    auto& fontConfig = Configuration::get();

    const auto fontInfo = QFontInfo( fontConfig.mainFont() );
    const auto availableSizes = FontUtils::availableFontSizes( fontInfo.family() );

    auto currentSize
        = std::find( availableSizes.cbegin(), availableSizes.cend(), fontInfo.pointSize() );
    if ( currentSize == availableSizes.cend() ) {
        // The configured size is not in the family's list (bitmap font with
        // discrete sizes, or a hand-edited/legacy config): stepping from a
        // past-the-end iterator is UB. Clamp to the nearest listed size
        // before stepping. availableFontSizes() always returns at least the
        // standard sizes plus 10..19, so it is never empty.
        const auto nearest
            = std::lower_bound( availableSizes.cbegin(), availableSizes.cend(),
                                fontInfo.pointSize() );
        if ( nearest == availableSizes.cend() ) {
            // Configured size is above the whole range -> clamp to largest.
            currentSize = std::prev( nearest );
        }
        else if ( nearest == availableSizes.cbegin() ) {
            // At or below the whole range -> clamp to smallest.
            currentSize = nearest;
        }
        else {
            currentSize = ( *nearest - fontInfo.pointSize()
                            < fontInfo.pointSize() - *std::prev( nearest ) )
                              ? nearest
                              : std::prev( nearest );
        }
    }
    if ( increase && currentSize != std::prev( availableSizes.cend() ) ) {
        currentSize = std::next( currentSize );
    }
    else if ( !increase && currentSize != availableSizes.begin() ) {
        currentSize = std::prev( currentSize );
    }

    if ( currentSize != availableSizes.cend() ) {
        // Start from the configured font so weight/italic/family hints are
        // preserved; only the point size changes.
        QFont newFont = fontConfig.mainFont();
        newFont.setPointSize( *currentSize );

        fontConfig.setMainFont( newFont );
        // Fan out to every registered view (main + all filtered/results views,
        // including frozen keep-results panes).
        for ( const auto& view : views_ ) {
            if ( view != nullptr ) {
                view->updateFont( newFont );
            }
        }
    }
}
