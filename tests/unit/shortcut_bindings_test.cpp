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

#include <algorithm>

#include "shortcuts.h"

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
    QStringList keysToCheck = { "1", "2", "3", "4", "5", "6", "7", "8", "9",
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
                                commandShortcutModifier() + "+Shift+R",
                                "Ctrl+Tab", "Ctrl+Shift+Tab" };

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
