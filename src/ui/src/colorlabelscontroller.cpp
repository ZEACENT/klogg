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
#include <limits>
#include <utility>

#include <QShortcut>
#include <QThread>

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

ColorLabelsController::Snapshot ColorLabelsController::snapshot() const
{
    Q_ASSERT( QThread::currentThread() == thread() );
    return { revision_, manager_.colorLabels() };
}

ColorLabelsController::EditResult ColorLabelsController::apply(
    const std::vector<Edit>& edits, std::optional<uint64_t> expectedRevision )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( expectedRevision && *expectedRevision != revision_ ) {
        return EditResult::Conflict;
    }
    size_t textUnits = 0;
    size_t textCount = 0;
    if ( const auto error = validateEdits( edits, manager_.colorLabels().size(), textUnits,
                                          textCount ) ) {
        return *error;
    }
    auto prepared = manager_;
    for ( const auto& edit : edits ) {
        applyEdit( prepared, edit );
    }
    return publish( std::move( prepared ) );
}

std::optional<ColorLabelsController::EditResult> ColorLabelsController::validateEdits(
    const std::vector<Edit>& edits, size_t labelCount, size_t& textUnits, size_t& textCount )
{
    if ( edits.size() > MaxEdits ) {
        return EditResult::ResourceLimit;
    }
    for ( const auto& edit : edits ) {
        switch ( edit.kind ) {
        case Edit::Kind::Set:
            if ( edit.label >= labelCount ) {
                return EditResult::InvalidArgument;
            }
            break;
        case Edit::Kind::Remove:
        case Edit::Kind::Clear:
            break;
        default:
            return EditResult::InvalidArgument;
        }
        // Explicit requests fail instead of inheriting the legacy selection
        // truncation policy. No prefix of an invalid batch is ever published.
        const auto count = static_cast<size_t>( edit.texts.size() );
        if ( count > ColorLabelsManager::MaxBulkLabelTexts - textCount ) {
            return EditResult::ResourceLimit;
        }
        textCount += count;
        for ( const auto& text : edit.texts ) {
            const auto size = static_cast<size_t>( text.size() );
            if ( size > MaxEditTextUnits - textUnits ) {
                return EditResult::ResourceLimit;
            }
            textUnits += size;
        }
    }
    return {};
}

void ColorLabelsController::applyEdit( ColorLabelsManager& manager, const Edit& edit )
{
    switch ( edit.kind ) {
    case Edit::Kind::Set:
        manager.setColorLabel( edit.label, edit.texts, edit.defaults );
        break;
    case Edit::Kind::Remove:
        manager.removeColorLabel( edit.texts );
        break;
    case Edit::Kind::Clear:
        manager.clear();
        break;
    }
}

void ColorLabelsController::applyGuiEdit( const Edit& edit )
{
    // GUI selections retain the existing manager's deduplication/truncation
    // behavior, but use exactly the same mutation and publication path.
    auto prepared = manager_;
    applyEdit( prepared, edit );
    publish( std::move( prepared ) );
}

ColorLabelsController::EditResult ColorLabelsController::publish( ColorLabelsManager prepared )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( manager_.hasSameState( prepared ) ) {
        return EditResult::Unchanged;
    }
    if ( revision_ == std::numeric_limits<uint64_t>::max() ) {
        return EditResult::ResourceLimit;
    }
    install( preparePublication( std::move( prepared ) ) );
    Q_EMIT labelsChanged();
    return EditResult::Applied;
}

ColorLabelsController::PreparedPublication ColorLabelsController::preparePublication(
    ColorLabelsManager state ) const
{
    PreparedPublication prepared;
    prepared.changed = !manager_.hasSameState( state );
    prepared.state = std::move( state );
    if ( prepared.changed ) {
        const auto labels = prepared.state.colorLabels();
        prepared.views.reserve( views_.size() );
        for ( const auto& view : views_ ) {
            if ( view ) {
                prepared.views.emplace_back( view, labels );
            }
        }
    }
    return prepared;
}

void ColorLabelsController::install( PreparedPublication prepared )
{
    if ( !prepared.changed ) {
        return;
    }
    manager_ = std::move( prepared.state );
    ++revision_;
    for ( auto& projection : prepared.views ) {
        if ( projection.first ) {
            projection.first->setQuickHighlighters( std::move( projection.second ) );
        }
    }
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
    applyGuiEdit( { Edit::Kind::Set, label, selectedTexts( sourceView ),
                    HighlighterSetCollection::get().quickHighlighterDefaults() } );
}

void ColorLabelsController::addNextColorLabelToSelection( const AbstractLogView* sourceView )
{
    auto prepared = manager_;
    prepared.setNextColorLabel(
        selectedTexts( sourceView ), HighlighterSetCollection::get().quickHighlighterDefaults() );
    publish( std::move( prepared ) );
}

void ColorLabelsController::removeColorLabelFromSelection( const AbstractLogView* sourceView )
{
    applyGuiEdit( { Edit::Kind::Remove, 0, selectedTexts( sourceView ), {} } );
}

void ColorLabelsController::clearColorLabels()
{
    applyGuiEdit( { Edit::Kind::Clear, 0, {}, {} } );
}

void ColorLabelsController::setQuickColorLabelDefaults( bool ignoreCase, bool wholeWord )
{
    auto& highlighterSetCollection = HighlighterSetCollection::get();
    highlighterSetCollection.setQuickHighlighterDefaults(
        QuickHighlighterDefaults{ ignoreCase, wholeWord } );
    highlighterSetCollection.save();
}

QStringList ColorLabelsController::selectedTexts( const AbstractLogView* sourceView ) const
{
    if ( sourceView != nullptr ) {
        return sourceView->getSelectedLinesText();
    }
    if ( activeViewProvider_ != nullptr ) {
        if ( auto* const view = activeViewProvider_() ) {
            return view->getSelectedLinesText();
        }
    }
    return {};
}
