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

#include "colorlabelscontroller.h"

#include <array>

#include <QShortcut>

#include "abstractlogview.h"
#include "configuration.h"
#include "highlighterset.h"
#include "shortcuts.h"

ColorLabelsController::ColorLabelsController(
    QWidget* shortcutsParent, std::function<AbstractLogView*()> activeViewProvider )
    : QObject( shortcutsParent )
    , shortcutsParent_( shortcutsParent )
    , activeViewProvider_( std::move( activeViewProvider ) )
{
}

void ColorLabelsController::watchView( AbstractLogView* view )
{
    if ( view == nullptr ) {
        return;
    }

    connect( view, &AbstractLogView::addColorLabel, this,
             [ this, view ]( size_t label ) { addColorLabelToSelection( label, view ); } );
    connect( view, &AbstractLogView::addNextColorLabel, this,
             [ this, view ]() { addNextColorLabelToSelection( view ); } );
    connect( view, &AbstractLogView::removeColorLabel, this,
             [ this, view ]() { removeColorLabelFromSelection( view ); } );
    connect( view, &AbstractLogView::clearColorLabels, this,
             [ this ]() { clearColorLabels(); } );
    connect( view, &AbstractLogView::quickColorLabelDefaultsChanged, this,
             [ this ]( bool ignoreCase, bool wholeWord ) {
                 setQuickColorLabelDefaults( ignoreCase, wholeWord );
             } );

    views_.push_back( view );

    // A late-registered view (e.g. a keep-results pane created after labels
    // were set) must start in sync with the rest of the tab.
    view->setQuickHighlighters( manager_.colorLabels() );
}

void ColorLabelsController::registerShortcuts()
{
    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }
    shortcuts_.clear();

    const auto& configuredShortcuts = Configuration::get().shortcuts();

    const std::array<std::string, 9> colorLabels = {
        ShortcutAction::LogViewAddColorLabel1, ShortcutAction::LogViewAddColorLabel2,
        ShortcutAction::LogViewAddColorLabel3, ShortcutAction::LogViewAddColorLabel4,
        ShortcutAction::LogViewAddColorLabel5, ShortcutAction::LogViewAddColorLabel6,
        ShortcutAction::LogViewAddColorLabel7, ShortcutAction::LogViewAddColorLabel8,
        ShortcutAction::LogViewAddColorLabel9,
    };

    for ( auto label = 0u; label < colorLabels.size(); ++label ) {
        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts_, shortcutsParent_, Qt::WidgetWithChildrenShortcut,
            colorLabels[ label ],
            [ this, label ]() { addColorLabelToSelection( label, nullptr ); } );
    }

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, shortcutsParent_, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewAddNextColorLabel,
        [ this ]() { addNextColorLabelToSelection( nullptr ); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, shortcutsParent_, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewRemoveColorLabel,
        [ this ]() { removeColorLabelFromSelection( nullptr ); } );

    ShortcutAction::registerShortcut(
        configuredShortcuts, shortcuts_, shortcutsParent_, Qt::WidgetWithChildrenShortcut,
        ShortcutAction::LogViewClearColorLabels, [ this ]() { clearColorLabels(); } );
}

void ColorLabelsController::addColorLabelToSelection( size_t label,
                                                      const AbstractLogView* sourceView )
{
    applyToViews( manager_.setColorLabel(
        label, selectedText( sourceView ),
        HighlighterSetCollection::get().quickHighlighterDefaults() ) );
}

void ColorLabelsController::addNextColorLabelToSelection( const AbstractLogView* sourceView )
{
    applyToViews( manager_.setNextColorLabel(
        selectedText( sourceView ), HighlighterSetCollection::get().quickHighlighterDefaults() ) );
}

void ColorLabelsController::removeColorLabelFromSelection( const AbstractLogView* sourceView )
{
    applyToViews( manager_.removeColorLabel( selectedText( sourceView ) ) );
}

void ColorLabelsController::clearColorLabels()
{
    applyToViews( manager_.clear() );
}

void ColorLabelsController::setQuickColorLabelDefaults( bool ignoreCase, bool wholeWord )
{
    auto& highlighterSetCollection = HighlighterSetCollection::get();
    highlighterSetCollection.setQuickHighlighterDefaults(
        QuickHighlighterDefaults{ ignoreCase, wholeWord } );
    highlighterSetCollection.save();
}

void ColorLabelsController::applyToViews(
    const ColorLabelsManager::QuickHighlightersCollection& labels )
{
    for ( const auto& view : views_ ) {
        if ( view != nullptr ) {
            view->setQuickHighlighters( labels );
        }
    }
}

QString ColorLabelsController::selectedText( const AbstractLogView* sourceView ) const
{
    if ( sourceView != nullptr ) {
        return sourceView->getSelectedText();
    }
    if ( activeViewProvider_ != nullptr ) {
        if ( auto* const view = activeViewProvider_() ) {
            return view->getSelectedText();
        }
    }
    return {};
}
