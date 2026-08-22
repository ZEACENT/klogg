/*
 * Copyright (C) 2026
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

#include <catch2/catch.hpp>

#include <QKeySequence>
#include <QString>
#include <QToolButton>

#include <algorithm>

#include "quickfindwidget.h"
#include "shortcuts.h"

namespace {
QKeySequence normalizedRuntimeSequence( const QKeySequence& sequence )
{
    return QKeySequence( sequence.toString( QKeySequence::NativeText ), QKeySequence::NativeText );
}

bool isExactRuntimeMatch( const QKeySequence& left, const QKeySequence& right )
{
    const auto normalizedLeft = normalizedRuntimeSequence( left );
    const auto normalizedRight = normalizedRuntimeSequence( right );
    return normalizedLeft.matches( normalizedRight ) == QKeySequence::ExactMatch
           && normalizedRight.matches( normalizedLeft ) == QKeySequence::ExactMatch;
}

bool containsExactRuntimeMatch( const QStringList& bindings, const QKeySequence& expected )
{
    return std::any_of( bindings.cbegin(), bindings.cend(), [ &expected ]( const auto& binding ) {
        return isExactRuntimeMatch(
            QKeySequence( binding, QKeySequence::PortableText ), expected );
    } );
}
} // namespace

TEST_CASE( "Shortcut bindings: disconnect and reconnect source have defaults" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    SECTION( "MainWindowDisconnectSource is bound to Ctrl+Shift+D" )
    {
        auto it = shortcuts.find( ShortcutAction::MainWindowDisconnectSource );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+D" ) );
    }

    SECTION( "MainWindowReconnectSource is bound to Ctrl+Shift+R" )
    {
        auto it = shortcuts.find( ShortcutAction::MainWindowReconnectSource );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+R" ) );
    }
}

TEST_CASE( "Shortcut bindings: tab cycling follows VSCode defaults" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    SECTION( "Next tab is bound to Ctrl+Tab" )
    {
        const auto it = shortcuts.find( ShortcutAction::MainWindowNextTab );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( QStringLiteral( "Ctrl+Tab" ) ) );
    }

    SECTION( "Previous tab is bound to Ctrl+Shift+Tab" )
    {
        const auto it = shortcuts.find( ShortcutAction::MainWindowPreviousTab );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( QStringLiteral( "Ctrl+Shift+Tab" ) ) );
    }
}

TEST_CASE( "Shortcut bindings: color labels use plain digit keys 1-9" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    const std::string colorLabelActions[] = {
        ShortcutAction::LogViewAddColorLabel1, ShortcutAction::LogViewAddColorLabel2,
        ShortcutAction::LogViewAddColorLabel3, ShortcutAction::LogViewAddColorLabel4,
        ShortcutAction::LogViewAddColorLabel5, ShortcutAction::LogViewAddColorLabel6,
        ShortcutAction::LogViewAddColorLabel7, ShortcutAction::LogViewAddColorLabel8,
        ShortcutAction::LogViewAddColorLabel9,
    };

    for ( int i = 0; i < 9; ++i ) {
        DYNAMIC_SECTION( "Color label " << ( i + 1 ) << " is bound to plain digit key" )
        {
            auto it = shortcuts.find( colorLabelActions[ i ] );
            REQUIRE( it != shortcuts.end() );
            const auto expectedKey = QKeySequence( Qt::Key_1 + i ).toString();
            REQUIRE( it->second.keySequence.contains( expectedKey ) );
        }
    }

    SECTION( "Remove color label (None) is bound to plain digit key 0" )
    {
        auto it = shortcuts.find( ShortcutAction::LogViewRemoveColorLabel );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( QKeySequence( Qt::Key_0 ).toString() ) );
    }
}

TEST_CASE( "Shortcut bindings: crawler visibility and filter options use Ctrl+Shift+digit" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    SECTION( "Marks and matches visibility is Ctrl+Shift+1" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerChangeVisibilityToMarksAndMatches );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+1" ) );
    }

    SECTION( "Marks visibility is Ctrl+Shift+2" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerChangeVisibilityToMarks );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+2" ) );
    }

    SECTION( "Matches visibility is Ctrl+Shift+3" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerChangeVisibilityToMatches );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+3" ) );
    }

    SECTION( "Case matching is Ctrl+Shift+4" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerEnableCaseMatching );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+4" ) );
    }

    SECTION( "Regex is Ctrl+Shift+5" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerEnableRegex );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+5" ) );
    }

    SECTION( "Inverse matching is Ctrl+Shift+6" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerEnableInverseMatching );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+6" ) );
    }

    SECTION( "Regex combining is Ctrl+Shift+7" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerEnableRegexCombining );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+7" ) );
    }

    SECTION( "Auto refresh is Ctrl+Shift+8" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerEnableAutoRefresh );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+8" ) );
    }

    SECTION( "Keep results is Ctrl+Shift+9" )
    {
        auto it = shortcuts.find( ShortcutAction::CrawlerKeepResults );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( commandShortcutModifier() + "+Shift+9" ) );
    }
}

TEST_CASE( "Shortcut bindings: open from clipboard does not steal text paste" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();
    const auto it = shortcuts.find( ShortcutAction::MainWindowOpenFromClipboard );
    REQUIRE( it != shortcuts.end() );

    const auto pasteKeys = QKeySequence::keyBindings( QKeySequence::Paste );
    for ( const auto& configuredKey : it->second.keySequence ) {
        const auto configuredSequence = QKeySequence( configuredKey );
        for ( const auto& pasteKey : pasteKeys ) {
            CHECK( configuredSequence.matches( pasteKey ) != QKeySequence::ExactMatch );
            CHECK( pasteKey.matches( configuredSequence ) != QKeySequence::ExactMatch );
        }
    }
}

TEST_CASE( "Shortcut bindings: Go to Line is the only normalized Ctrl+G runtime owner" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();
    const auto ctrlG = QKeySequence( QStringLiteral( "Ctrl+G" ), QKeySequence::PortableText );

    const auto jumpToLine = shortcuts.find( ShortcutAction::LogViewJumpToLine );
    REQUIRE( jumpToLine != shortcuts.end() );
    CHECK( jumpToLine->second.keySequence
           == QStringList{ ctrlG.toString( QKeySequence::PortableText ) } );

    std::vector<std::string> ctrlGOwners;
    for ( const auto& [ action, shortcut ] : shortcuts ) {
        if ( containsExactRuntimeMatch( shortcut.keySequence, ctrlG ) ) {
            ctrlGOwners.push_back( action );
        }
    }

    REQUIRE( ctrlGOwners.size() == 1 );
    CHECK( ctrlGOwners.front() == ShortcutAction::LogViewJumpToLine );

    const auto findNext = shortcuts.find( ShortcutAction::LogViewQfForward );
    REQUIRE( findNext != shortcuts.end() );
    REQUIRE_FALSE( findNext->second.keySequence.isEmpty() );
    CHECK_FALSE( containsExactRuntimeMatch( findNext->second.keySequence, ctrlG ) );

    const auto f3 = QKeySequence( Qt::Key_F3 );
    CHECK( containsExactRuntimeMatch( findNext->second.keySequence, f3 ) );

    for ( auto left = findNext->second.keySequence.cbegin();
          left != findNext->second.keySequence.cend(); ++left ) {
        for ( auto right = std::next( left ); right != findNext->second.keySequence.cend();
              ++right ) {
            CHECK_FALSE( isExactRuntimeMatch(
                QKeySequence( *left, QKeySequence::PortableText ),
                QKeySequence( *right, QKeySequence::PortableText ) ) );
        }
    }
}

TEST_CASE( "QuickFind next button uses the first safe Find Next binding" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();
    const auto findNext = shortcuts.find( ShortcutAction::LogViewQfForward );
    REQUIRE( findNext != shortcuts.end() );
    REQUIRE_FALSE( findNext->second.keySequence.isEmpty() );

    QuickFindWidget quickFind;
    const auto buttons = quickFind.findChildren<QToolButton*>();
    const auto nextButton = std::find_if( buttons.cbegin(), buttons.cend(), []( const auto* button ) {
        return button->text() == QStringLiteral( "Next" );
    } );
    REQUIRE( nextButton != buttons.cend() );

    const auto ctrlG = QKeySequence( QStringLiteral( "Ctrl+G" ), QKeySequence::PortableText );
    CHECK_FALSE( isExactRuntimeMatch( ( *nextButton )->shortcut(), ctrlG ) );
    CHECK( isExactRuntimeMatch(
        ( *nextButton )->shortcut(),
        QKeySequence( findNext->second.keySequence.front(), QKeySequence::PortableText ) ) );
}

TEST_CASE( "Shortcut bindings: no duplicate key bindings across all default shortcuts" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    // Build a map from key binding -> list of actions that use it
    std::map<QString, std::vector<std::string>> keyToActions;
    for ( const auto& [ action, shortcut ] : shortcuts ) {
        for ( const auto& key : shortcut.keySequence ) {
            if ( !key.isEmpty() ) {
                keyToActions[ key ].push_back( action );
            }
        }
    }

    // Check that the swapped keys don't have conflicts
    QStringList keysToCheck = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
                                commandShortcutModifier() + "+Shift+1",
                                commandShortcutModifier() + "+Shift+2",
                                commandShortcutModifier() + "+Shift+3",
                                commandShortcutModifier() + "+Shift+4",
                                commandShortcutModifier() + "+Shift+5",
                                commandShortcutModifier() + "+Shift+6",
                                commandShortcutModifier() + "+Shift+7",
                                commandShortcutModifier() + "+Shift+8",
                                commandShortcutModifier() + "+Shift+9",
                                commandShortcutModifier() + "+Shift+D",
                                commandShortcutModifier() + "+Shift+R", "Ctrl+Tab",
                                "Ctrl+Shift+Tab" };

    for ( const auto& key : keysToCheck ) {
        DYNAMIC_SECTION( "Key " << key.toStdString() << " has at most one binding" )
        {
            auto it = keyToActions.find( key );
            if ( it != keyToActions.end() ) {
                INFO( "Actions using key: " );
                for ( const auto& action : it->second ) {
                    INFO( "  " << action );
                }
                CHECK( it->second.size() <= 1 );
            }
        }
    }
}

TEST_CASE( "commandShortcutModifier: returns platform-appropriate modifier for command shortcuts" )
{
    // On macOS Qt maps "Meta" in QKeySequence strings to ⌘ Command so
    // that application shortcuts follow the macOS HIG convention of using
    // Command as the primary modifier. "Ctrl" maps to physical Control.
    // On other platforms "Ctrl" is the expected primary modifier.
#ifdef Q_OS_MACOS
    REQUIRE( commandShortcutModifier() == QStringLiteral( "Meta" ) );
#else
    REQUIRE( commandShortcutModifier() == QStringLiteral( "Ctrl" ) );
#endif
}

TEST_CASE( "Shortcut bindings: editable defaults have no duplicate displayed keys" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    std::map<QString, std::vector<std::string>> keyToActions;
    for ( const auto& [ action, shortcut ] : shortcuts ) {
        const auto visibleShortcutCount = std::min<qsizetype>( 2, shortcut.keySequence.size() );
        for ( auto shortcutIndex = 0; shortcutIndex < visibleShortcutCount; ++shortcutIndex ) {
            const auto key = QKeySequence( shortcut.keySequence.at( shortcutIndex ) )
                                 .toString( QKeySequence::NativeText );
            if ( !key.isEmpty() ) {
                keyToActions[ key ].push_back( action );
            }
        }
    }

    for ( const auto& [ key, actions ] : keyToActions ) {
        DYNAMIC_SECTION( "Displayed key " << key.toStdString() << " is unique" )
        {
            INFO( "Actions using displayed key:" );
            for ( const auto& action : actions ) {
                INFO( "  " << action );
            }
            CHECK( actions.size() <= 1 );
        }
    }
}

TEST_CASE( "Shortcut bindings: Follow file and Go to top have single-key defaults" )
{
    const auto& shortcuts = ShortcutAction::defaultShortcutList();

    SECTION( "MainWindowFollowFile is bound to F" )
    {
        auto it = shortcuts.find( ShortcutAction::MainWindowFollowFile );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( QKeySequence( Qt::Key_F ).toString() ) );
    }

    SECTION( "MainWindowGoToTop is bound to T" )
    {
        auto it = shortcuts.find( ShortcutAction::MainWindowGoToTop );
        REQUIRE( it != shortcuts.end() );
        REQUIRE( it->second.keySequence.contains( QKeySequence( Qt::Key_T ).toString() ) );
    }
}
