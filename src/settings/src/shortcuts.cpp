/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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

#include <functional>

#include <QApplication>
#include <QShortcut>
#include <QWidget>

#include "shortcuts.h"

QString commandShortcutModifier()
{
#ifdef Q_OS_MACOS
    return QStringLiteral( "Meta" );
#else
    return QStringLiteral( "Ctrl" );
#endif
}

QStringList getKeyBindings( QKeySequence::StandardKey standardKey )
{
    auto bindings = QKeySequence::keyBindings( standardKey );
    QStringList stringBindings;
    std::transform( bindings.cbegin(), bindings.cend(), std::back_inserter( stringBindings ),
                    []( const auto& keySequence ) {
                        return keySequence.toString( QKeySequence::PortableText );
                    } );

    return stringBindings;
}

QStringList uniqueKeyBindings( const QStringList& bindings )
{
    QStringList uniqueBindings;
    for ( const auto& binding : bindings ) {
        const auto normalized = QKeySequence( binding ).toString( QKeySequence::PortableText );
        if ( !normalized.isEmpty() && !uniqueBindings.contains( normalized ) ) {
            uniqueBindings.push_back( normalized );
        }
    }
    return uniqueBindings;
}

QStringList ShortcutAction::defaultShortcutKeys( const std::string& action )
{
    const auto& shortcuts = defaultShortcutList();
    const auto actionShortcuts = shortcuts.find( action );
    return actionShortcuts != shortcuts.end() ? actionShortcuts->second.keySequence : QStringList{};
}

void ShortcutAction::registerShortcut( const ConfiguredShortcuts& configuredShortcuts,
                                       std::map<QString, QShortcut*>& shortcutsStorage,
                                       QWidget* shortcutsParent, Qt::ShortcutContext context,
                                       const std::string& action,
                                       const std::function<void()>& func )
{
    const auto keysConfiguration = configuredShortcuts.find( action );
    const auto keys = keysConfiguration != configuredShortcuts.end()
                          ? keysConfiguration->second
                          : ShortcutAction::defaultShortcutKeys( action );

    for ( const auto& key : keys ) {
        if ( key.isEmpty() ) {
            continue;
        }

        auto shortcut = shortcutsStorage.extract( key );
        if ( shortcut ) {
            shortcut.mapped()->deleteLater();
        }

        registerShortcut( key, shortcutsStorage, shortcutsParent, context, func );
    }
}

void ShortcutAction::registerShortcut( const QString& key,
                                       std::map<QString, QShortcut*>& shortcutsStorage,
                                       QWidget* shortcutsParent, Qt::ShortcutContext context,
                                       const std::function<void()>& func )
{
    auto newShortcut = new QShortcut( QKeySequence( key ), shortcutsParent );
    newShortcut->setContext( context );
    newShortcut->connect( newShortcut, &QShortcut::activated, shortcutsParent,
                          [ func ] { func(); } );
    shortcutsStorage.emplace( key, newShortcut );
}

QList<QKeySequence> ShortcutAction::shortcutKeys( const std::string& action,
                                                  const ConfiguredShortcuts& configuredShortcuts )
{
    const auto keysConfiguration = configuredShortcuts.find( action );
    const auto keys = keysConfiguration != configuredShortcuts.end()
                          ? keysConfiguration->second
                          : ShortcutAction::defaultShortcutKeys( action );

    QList<QKeySequence> shortcuts;
    std::transform( keys.cbegin(), keys.cend(), std::back_inserter( shortcuts ),
                    []( const QString& hotkeys ) { return QKeySequence( hotkeys ); } );

    return shortcuts;
}

const ShortcutAction::ShortcutList& ShortcutAction::defaultShortcutList()
{
    static ShortcutList defaultShortcutKeys = {
        {
            MainWindowNewWindow,
            {
                QApplication::tr( "Open new window" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenFile,
            {
                QApplication::tr( "Open file" ),
                getKeyBindings( QKeySequence::Open ),
            },
        },
        {
            MainWindowCloseFile,
            {
                QApplication::tr( "Close file" ),
                getKeyBindings( QKeySequence::Close ),
            },
        },
        {
            MainWindowCloseAll,
            {
                QApplication::tr( "Close all files" ),
                QStringList{},
            },
        },
        {
            MainWindowSelectAll,
            {
                QApplication::tr( "Select all" ),
                getKeyBindings( QKeySequence::SelectAll ),
            },
        },
        {
            MainWindowCopy,
            {
                QApplication::tr( "Copy selection to clipboard" ),
                getKeyBindings( QKeySequence::Copy ),
            },
        },
        {
            MainWindowQuit,
            {
                QApplication::tr( "Exit application" ),
                QStringList{ commandShortcutModifier() + "+Q" },
            },
        },
        {
            MainWindowFullScreen,
            {
                QApplication::tr( "Full Screen" ),
                QStringList{},
            },
        },
        {
            MainWindowMax,
            {
                QApplication::tr( "Maximize window" ),
                QStringList{},
            },
        },
        {
            MainWindowMin,
            {
                QApplication::tr( "Minimize Window" ),
                QStringList{},
            },
        },
        {
            MainWindowPreference,
            {
                QApplication::tr( "Preferences" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenQf,
            {
                QApplication::tr( "Open quick find" ),
                getKeyBindings( QKeySequence::Find ),
            },
        },
        {
            MainWindowOpenQfForward,
            {
                QApplication::tr( "Quick find forward" ),
                QStringList{ QKeySequence( Qt::Key_Apostrophe ).toString() },
            },
        },
        {
            MainWindowOpenQfBackward,
            {
                QApplication::tr( "Quick find backward" ),
                QStringList{ QKeySequence( Qt::Key_QuoteDbl ).toString() },
            },
        },
        {
            MainWindowFocusSearchInput,
            {
                QApplication::tr( "Set focus to search input" ),
                QStringList{ { commandShortcutModifier() + "+S",
                               commandShortcutModifier() + "+Shift+F" } },
            },
        },
        {
            MainWindowClearFile,
            {
                QApplication::tr( "Clear file" ),
                QStringList{ getKeyBindings( QKeySequence::Cut ) },
            },
        },
        {
            MainWindowOpenContainingFolder,
            {
                QApplication::tr( "Open containing folder" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenInEditor,
            {
                QApplication::tr( "Open file in editor" ),
                QStringList{},
            },
        },
        {
            MainWindowCopyPathToClipboard,
            {
                QApplication::tr( "Copy file path to clipboard" ),
                QStringList{},
            },
        },
        {
            MainWindowOpenFromClipboard,
            {
                QApplication::tr( "Paste text from clipboard" ),
                // Do not bind this to QKeySequence::Paste by default. It is an
                // application command, not text editing, and stealing Cmd/Ctrl+V
                // breaks paste in child dialogs and platform-native panels.
                QStringList{},
            },
        },
        {
            MainWindowOpenFromUrl,
            {
                QApplication::tr( "Open file from URL" ),
                QStringList{},
            },
        },
        {
            MainWindowDisconnectSource,
            {
                QApplication::tr( "Disconnect live source" ),
                QStringList{ commandShortcutModifier() + "+Shift+D" },
            },
        },
        {
            MainWindowReconnectSource,
            {
                QApplication::tr( "Reconnect live source" ),
                QStringList{ commandShortcutModifier() + "+Shift+R" },
            },
        },
        {
            MainWindowFollowFile,
            {
                QApplication::tr( "Monitor file changes" ),
                { { QKeySequence( Qt::Key_F ).toString(),
                    QKeySequence( Qt::Key_F10 ).toString() } },
            },
        },
        {
            MainWindowGoToTop,
            {
                QApplication::tr( "Jump to the beginning of the file" ),
                QStringList{ QKeySequence( Qt::Key_T ).toString() },
            },
        },
        {
            MainWindowTextWrap,
            {
                QApplication::tr( "Toggle text wrap" ),
                QStringList{ QKeySequence( Qt::Key_W ).toString() },
            },
        },
        {
            MainWindowReload,
            {
                QApplication::tr( "Reload file" ),
                QStringList{ getKeyBindings( QKeySequence::Refresh ) },
            },
        },
        {
            MainWindowStop,
            {
                QApplication::tr( "Stop file loading" ),
                QStringList{ getKeyBindings( QKeySequence::Cancel ) },
            },
        },
        {
            MainWindowScratchpad,
            {
                QApplication::tr( "Open scratchpad" ),
                QStringList{},
            },
        },
        {
            MainWindowSelectOpenFile,
            {
                QApplication::tr( "Switch to file" ),
                QStringList{ commandShortcutModifier() + "+Shift+O" },
            },
        },
        {
            MainWindowNextTab,
            {
                QApplication::tr( "Switch to next tab" ),
                QStringList{ "Ctrl+Tab" },
            },
        },
        {
            MainWindowPreviousTab,
            {
                QApplication::tr( "Switch to previous tab" ),
                QStringList{ "Ctrl+Shift+Tab" },
            },
        },
        {
            CrawlerChangeVisibilityForward,
            {
                QApplication::tr( "Change filtered lines visibility forward" ),
                QStringList{ QKeySequence( Qt::Key_V ).toString() },
            },
        },
        {
            CrawlerChangeVisibilityBackward,
            {
                QApplication::tr( "Change filtered lines visibility backward" ),
                QStringList{ "Shift+V" },
            },
        },
        {
            CrawlerChangeVisibilityToMarksAndMatches,
            {
                QApplication::tr( "Change filtered lines visibility to marks and matches" ),
                QStringList{ commandShortcutModifier() + "+Shift+1" },
            },
        },
        {
            CrawlerChangeVisibilityToMarks,
            {
                QApplication::tr( "Change filtered lines visibility to marks" ),
                QStringList{ commandShortcutModifier() + "+Shift+2" },
            },
        },
        {
            CrawlerChangeVisibilityToMatches,
            {
                QApplication::tr( "Change filtered lines visibility to matches" ),
                QStringList{ commandShortcutModifier() + "+Shift+3" },
            },
        },
        {
            CrawlerIncreseTopViewSize,
            {
                QApplication::tr( "Increase main view" ),
                QStringList{ QKeySequence( Qt::Key_Plus ).toString() },
            },
        },
        {
            CrawlerDecreaseTopViewSize,
            {
                QApplication::tr( "Decrease main view" ),
                QStringList{ QKeySequence( Qt::Key_Minus ).toString() },
            },
        },
        {
            CrawlerEnableCaseMatching,
            {
                QApplication::tr( "Enable case matching" ),
                QStringList{ commandShortcutModifier() + "+Shift+4" },
            },
        },
        {
            CrawlerEnableRegex,
            {
                QApplication::tr( "Enable regex" ),
                QStringList{ commandShortcutModifier() + "+Shift+5" },
            },
        },
        {
            CrawlerEnableInverseMatching,
            {
                QApplication::tr( "Enable inverse matching" ),
                QStringList{ commandShortcutModifier() + "+Shift+6" },
            },
        },
        {
            CrawlerEnableRegexCombining,
            {
                QApplication::tr( "Enable regex combining" ),
                QStringList{ commandShortcutModifier() + "+Shift+7" },
            },
        },
        {
            CrawlerEnableAutoRefresh,
            {
                QApplication::tr( "Enable auto refresh" ),
                QStringList{ commandShortcutModifier() + "+Shift+8" },
            },
        },
        {
            CrawlerKeepResults,
            {
                QApplication::tr( "Keep search results" ),
                QStringList{ commandShortcutModifier() + "+Shift+9" },
            },
        },
        // remove by commit 0b75b9d6
        // { QfFindNext, { QApplication::tr( "QuickFind: Find next" ), QStringList{ getKeyBindings(
        // QKeySequence::FindNext ) } } }, { QfFindPrev, { QApplication::tr( "QuickFind: Find
        // previous"
        // ), QStringList{ getKeyBindings( QKeySequence::FindPrevious ) } } },
        {
            LogViewMark,
            {
                QApplication::tr( "Add line mark" ),
                QStringList{ QKeySequence( Qt::Key_M ).toString() },
            },
        },
        {
            LogViewDeleteMark,
            {
                QApplication::tr( "Delete line mark" ),
                QStringList{ QKeySequence( Qt::Key_N ).toString() },
            },
        },
        {
            LogViewNextMark,
            {
                QApplication::tr( "Jump to next mark" ),
                QStringList{ QKeySequence( Qt::Key_BracketRight ).toString() },
            },
        },
        {
            LogViewPrevMark,
            {
                QApplication::tr( "Jump to previous mark" ),
                QStringList{ QKeySequence( Qt::Key_BracketLeft ).toString() },
            },
        },
        {
            LogViewSelectionUp,
            {
                QApplication::tr( "Move selection up" ),
                { { QKeySequence( Qt::Key_Up ).toString(), QKeySequence( Qt::Key_K ).toString() } },
            },
        },
        {
            LogViewSelectionDown,
            {
                QApplication::tr( "Move selection down" ),
                { { QKeySequence( Qt::Key_Down ).toString(),
                    QKeySequence( Qt::Key_J ).toString() } },
            },
        },
        {
            LogViewScrollUp,
            {
                QApplication::tr( "Scroll up" ),
                QStringList{ "Alt+Up" },
            },
        },
        {
            LogViewScrollDown,
            {
                QApplication::tr( "Scroll down" ),
                QStringList{ "Alt+Down" },
            },
        },
        {
            LogViewScrollLeft,
            {
                QApplication::tr( "Scroll left" ),
                { { QKeySequence( Qt::Key_Left ).toString(),
                    QKeySequence( Qt::Key_H ).toString() } },
            },
        },
        {
            LogViewScrollRight,
            {
                QApplication::tr( "Scroll right" ),
                { { QKeySequence( Qt::Key_Right ).toString(),
                    QKeySequence( Qt::Key_L ).toString() } },
            },
        },
        {
            LogViewJumpToStartOfLine,
            {
                QApplication::tr( "Jump to the beginning of the current line" ),
                { QKeySequence( Qt::Key_Home ).toString(),
                  QKeySequence( Qt::Key_AsciiCircum ).toString() },
            },
        },
        {
            LogViewJumpToEndOfLine,
            {
                QApplication::tr( "Jump to the end start of the current line" ),
                QStringList{ QKeySequence( Qt::Key_Dollar ).toString() },
            },
        },
        {
            LogViewJumpToRightOfScreen,
            {
                QApplication::tr( "Jump to the right of the text" ),
                QStringList{ QKeySequence( Qt::Key_End ).toString() },
            },
        },
        {
            LogViewJumpToBottom,
            {
                QApplication::tr( "Jump to the bottom of the text" ),
                QStringList{ { commandShortcutModifier() + "+End", "Shift+G" } },
            },
        },
        {
            LogViewJumpToTop,
            {
                QApplication::tr( "Jump to the top of the text" ),
                QStringList{ commandShortcutModifier() + "+Home" },
            },
        },
        {
            LogViewJumpToLine,
            {
                QApplication::tr( "Jump to line" ),
                QStringList{ commandShortcutModifier() + "+L" },
            },
        },
        {
            LogViewQfForward,
            {
                QApplication::tr( "Main view: find next" ),
                uniqueKeyBindings( getKeyBindings( QKeySequence::FindNext ) << commandShortcutModifier() + "+G" ),
            },
        },
        {
            LogViewQfBackward,
            { QApplication::tr( "Main view: find previous" ),
              uniqueKeyBindings( getKeyBindings( QKeySequence::FindPrevious ) << "Shift+N"
                                                                               << commandShortcutModifier() + "+Shift+G" ) },
        },
        {
            LogViewQfSelectedForward,
            { QApplication::tr( "Set selection to QuickFind and find next" ),
              { QKeySequence( Qt::Key_Asterisk ).toString(),
                QKeySequence( Qt::Key_Period ).toString() } },
        },
        {
            LogViewQfSelectedBackward,
            {
                QApplication::tr( "Set selection to QuickFind and find previous" ),
                QStringList{ QKeySequence( Qt::Key_Slash ).toString(),
                             QKeySequence( Qt::Key_Comma ).toString() },
            },
        },
        {
            LogViewExitView,
            {
                QApplication::tr( "Release focus from view" ),
                QStringList{ QKeySequence( Qt::Key_Space ).toString() },
            },
        },
        {
            LogViewAddColorLabel1,
            {
                QApplication::tr( "Highlight text with color 1" ),
                QStringList{ QKeySequence( Qt::Key_1 ).toString() },
            },
        },
        {
            LogViewAddColorLabel2,
            {
                QApplication::tr( "Highlight text with color 2" ),
                QStringList{ QKeySequence( Qt::Key_2 ).toString() },
            },
        },
        {
            LogViewAddColorLabel3,
            {
                QApplication::tr( "Highlight text with color 3" ),
                QStringList{ QKeySequence( Qt::Key_3 ).toString() },
            },
        },
        {
            LogViewAddColorLabel4,
            {
                QApplication::tr( "Highlight text with color 4" ),
                QStringList{ QKeySequence( Qt::Key_4 ).toString() },
            },
        },
        {
            LogViewAddColorLabel5,
            {
                QApplication::tr( "Highlight text with color 5" ),
                QStringList{ QKeySequence( Qt::Key_5 ).toString() },
            },
        },
        {
            LogViewAddColorLabel6,
            {
                QApplication::tr( "Highlight text with color 6" ),
                QStringList{ QKeySequence( Qt::Key_6 ).toString() },
            },
        },
        {
            LogViewAddColorLabel7,
            {
                QApplication::tr( "Highlight text with color 7" ),
                QStringList{ QKeySequence( Qt::Key_7 ).toString() },
            },
        },
        {
            LogViewAddColorLabel8,
            {
                QApplication::tr( "Highlight text with color 8" ),
                QStringList{ QKeySequence( Qt::Key_8 ).toString() },
            },
        },
        {
            LogViewAddColorLabel9,
            {
                QApplication::tr( "Highlight text with color 9" ),
                QStringList{ QKeySequence( Qt::Key_9 ).toString() },
            },
        },
        {
            LogViewAddNextColorLabel,
            {
                QApplication::tr( "Highlight text with next color" ),
                QStringList{ commandShortcutModifier() + "+D" },
            },
        },
        {
            LogViewRemoveColorLabel,
            {
                QApplication::tr( "Remove color label from selection" ),
                QStringList{ QKeySequence( Qt::Key_0 ).toString() },
            },
        },
        {
            LogViewClearColorLabels,
            {
                QApplication::tr( "Clear all color labels" ),
                QStringList{ commandShortcutModifier() + "+Shift+0" },
            },
        },
        {
            LogViewSendSelectionToScratchpad,
            {
                QApplication::tr( "Send selection to scratchpad" ),
                QStringList{ commandShortcutModifier() + "+E" },
            },
        },
        {
            LogViewReplaceScratchpadWithSelection,
            {
                QApplication::tr( "Replace scratchpad with selection" ),
                QStringList{ commandShortcutModifier() + "+Shift+E" },
            },
        },
        {
            LogViewAddToSearch,
            {
                QApplication::tr( "Add selection to search pattern" ),
                QStringList{ "Shift+A" },
            },
        },
        {
            LogViewExcludeFromSearch,
            {
                QApplication::tr( "Exclude selection from search pattern " ),
                QStringList{ "Shift+E" },
            },
        },
        {
            LogViewReplaceSearch,
            {
                QApplication::tr( "Replace search pattern with selection" ),
                QStringList{ "Shift+R" },
            },
        },
        {
            LogViewSelectLinesUp,
            {
                QApplication::tr( "Select lines down" ),
                QStringList{ "Shift+Up" },
            },
        },
        {
            LogViewSelectLinesDown,
            {
                QApplication::tr( "Select lines up" ),
                QStringList{ "Shift+Down" },
            },
        },
    };
    return defaultShortcutKeys;
}
