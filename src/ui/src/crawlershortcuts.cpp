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

#include "crawlershortcuts.h"

#include <QComboBox>
#include <QKeySequence>
#include <QShortcut>
#include <QToolButton>

#include "configuration.h"
#include "abstractlogview.h"
#include "searchtoolbar.h"
#include "shortcuts.h"

namespace klogg {

void registerCrawlerShortcuts( QWidget* host, std::map<QString, QShortcut*>& shortcuts,
                               const CrawlerShortcutHooks& hooks )
{
    const auto& config = Configuration::get();
    const auto& configuredShortcuts = config.shortcuts();

    auto* const visibilityBox = hooks.visibilityBox ? hooks.visibilityBox() : nullptr;
    if ( visibilityBox != nullptr ) {
        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerChangeVisibilityForward, [ visibilityBox ]() {
                visibilityBox->setCurrentIndex( ( visibilityBox->currentIndex() + 1 )
                                                % visibilityBox->count() );
            } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerChangeVisibilityBackward, [ visibilityBox ]() {
                int nextIndex = visibilityBox->currentIndex() - 1;
                if ( nextIndex < 0 ) {
                    nextIndex = visibilityBox->count() - 1;
                }
                visibilityBox->setCurrentIndex( nextIndex );
            } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerChangeVisibilityToMarksAndMatches, [ visibilityBox ]() {
                if ( visibilityBox->count() > 0 ) {
                    visibilityBox->setCurrentIndex( 0 );
                }
            } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerChangeVisibilityToMarks, [ visibilityBox ]() {
                if ( visibilityBox->count() > 1 ) {
                    visibilityBox->setCurrentIndex( 1 );
                }
            } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerChangeVisibilityToMatches, [ visibilityBox ]() {
                if ( visibilityBox->count() > 2 ) {
                    visibilityBox->setCurrentIndex( 2 );
                }
            } );
    }

    auto* const searchToolbar = hooks.searchToolbar ? hooks.searchToolbar() : nullptr;
    if ( searchToolbar != nullptr ) {
        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerEnableCaseMatching,
            [ searchToolbar ]() { searchToolbar->matchCaseButton()->toggle(); } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerEnableRegex,
            [ searchToolbar ]() { searchToolbar->useRegexpButton()->toggle(); } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerEnableInverseMatching,
            [ searchToolbar ]() { searchToolbar->inverseButton()->toggle(); } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerEnableRegexCombining,
            [ searchToolbar ]() { searchToolbar->booleanButton()->toggle(); } );

        if ( hooks.includeAutoRefresh ) {
            ShortcutAction::registerShortcut(
                configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
                ShortcutAction::CrawlerEnableAutoRefresh,
                [ searchToolbar ]() { searchToolbar->searchRefreshButton()->toggle(); } );
        }

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerKeepResults,
            [ searchToolbar ]() { searchToolbar->keepSearchResultsButton()->toggle(); } );
    }

    if ( hooks.changeTopViewSize ) {
        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerIncreseTopViewSize,
            [ hooks ]() { hooks.changeTopViewSize( 1 ); } );

        ShortcutAction::registerShortcut(
            configuredShortcuts, shortcuts, host, Qt::WidgetWithChildrenShortcut,
            ShortcutAction::CrawlerDecreaseTopViewSize,
            [ hooks ]() { hooks.changeTopViewSize( -1 ); } );
    }

    if ( hooks.activeView ) {
        const auto exitSearchKeySequence = QKeySequence( QKeySequence::Cancel );
        ShortcutAction::registerShortcut( exitSearchKeySequence.toString(), shortcuts, host,
                                          Qt::WidgetWithChildrenShortcut, [ hooks ]() {
                                              auto* const activeView = hooks.activeView();
                                              if ( activeView != nullptr ) {
                                                  activeView->setFocus();
                                              }
                                          } );
    }
}

} // namespace klogg
