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

// Focused unit tests for SearchToolbar. These lock the pattern contract
// (currentRegularExpressionPattern is the verbatim move of the former
// crawlerwidget.cpp:1976-1978 construction) independent of CrawlerWidget, and
// verify the escape/combine helpers round-trip. Constructing SearchToolbar
// alone (null SavedSearches) also proves the folder-mode construction path is
// null-safe.

#include <catch2/catch.hpp>

#include <QComboBox>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QToolButton>
#include <Qt>

#include <string_view>

#include "regularexpression.h"
#include "regularexpressionpattern.h"
#include "searchtoolbar.h"

namespace {
// Helper: drive the widget through the Qt event loop so queued autoRun searches
// (if enabled) and signal delivery settle before assertions.
void pumpEvents()
{
    QTest::qWait( 10 );
}
} // namespace

TEST_CASE( "SearchToolbar constructs with null SavedSearches (folder mode)",
           "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );
    REQUIRE( toolbar.searchLineEdit() != nullptr );
    REQUIRE( toolbar.currentSearchText().isEmpty() );
    // No history is populated when SavedSearches is null.
    REQUIRE( toolbar.searchLineEdit()->count() == 0 );
    REQUIRE( toolbar.searchLineCompleter() != nullptr );
}

TEST_CASE( "SearchToolbar pattern contract mirrors crawlerwidget.cpp:1976-1978",
           "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "construction defaults: all toggles off (config defaults are applied by "
             "the host, not the toolbar)" )
    {
        toolbar.searchLineEdit()->setEditText( "ERROR" );
        const auto pattern = toolbar.currentRegularExpressionPattern();
        REQUIRE( pattern.pattern == "ERROR" );
        // matchCase unchecked => not case-sensitive
        REQUIRE( pattern.isCaseSensitive == false );
        REQUIRE( pattern.isExclude == false );
        REQUIRE( pattern.isBoolean == false );
        // isPlainText == !useRegexp. useRegexp defaults off => plainText true.
        REQUIRE( pattern.isPlainText == true );
        REQUIRE( toolbar.isUseRegexp() == false );
    }

    SECTION( "useRegexp on => isPlainText false" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setUseRegexp( true );
        const auto pattern = toolbar.currentRegularExpressionPattern();
        REQUIRE( pattern.isPlainText == false );
        REQUIRE( toolbar.isUseRegexp() == true );
    }

    SECTION( "matchCase on => isCaseSensitive true" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setMatchCase( true );
        REQUIRE( toolbar.currentRegularExpressionPattern().isCaseSensitive == true );
        REQUIRE( toolbar.isMatchCase() == true );
    }

    SECTION( "inverse on => isExclude true" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setInverse( true );
        REQUIRE( toolbar.currentRegularExpressionPattern().isExclude == true );
        REQUIRE( toolbar.isInverse() == true );
    }

    SECTION( "boolean on => isBoolean true" )
    {
        toolbar.searchLineEdit()->setEditText( "ERR" );
        toolbar.setBoolean( true );
        REQUIRE( toolbar.currentRegularExpressionPattern().isBoolean == true );
        REQUIRE( toolbar.isBoolean() == true );
    }

    SECTION( "all flags combined round-trip through getters" )
    {
        toolbar.searchLineEdit()->setEditText( "combined" );
        toolbar.setMatchCase( true );
        toolbar.setUseRegexp( true );
        toolbar.setInverse( true );
        toolbar.setBoolean( true );

        const auto pattern = toolbar.currentRegularExpressionPattern();
        REQUIRE( pattern.pattern == "combined" );
        REQUIRE( pattern.isCaseSensitive == true );
        REQUIRE( pattern.isExclude == true );
        REQUIRE( pattern.isBoolean == true );
        // useRegexp on => plainText false
        REQUIRE( pattern.isPlainText == false );

        // getters mirror the setters
        REQUIRE( toolbar.isMatchCase() == true );
        REQUIRE( toolbar.isUseRegexp() == true );
        REQUIRE( toolbar.isInverse() == true );
        REQUIRE( toolbar.isBoolean() == true );
    }
}

TEST_CASE( "SearchToolbar escape/combine helpers (ports crawlerwidget.cpp:1155-1182)",
           "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "escape wraps in quotes when boolean mode is on" )
    {
        toolbar.setBoolean( true );
        const auto escaped = toolbar.escapeSearchPattern( "error", false );
        // boolean mode: prepend/append '"'
        REQUIRE( escaped.startsWith( '"' ) );
        REQUIRE( escaped.endsWith( '"' ) );
    }

    SECTION( "boolean escape backslash-escapes embedded double-quotes" )
    {
        // Regression: escapeSearchPattern used to call replace('"',"\"") -- a
        // 1-char literal (just '"') so the replace was a no-op and an embedded
        // quote broke the wrapped boolean expression. The 2-char literal "\\\""
        // (backslash+quote) escapes it to \".
        toolbar.setBoolean( true );
        const auto escaped = toolbar.escapeSearchPattern( "a\"b", false );
        REQUIRE( escaped == "\"a\\\"b\"" );
    }

    SECTION( "combine joins regex with '|'" )
    {
        toolbar.setUseRegexp( true );
        toolbar.setBoolean( false );
        QString current = "foo";
        toolbar.combinePatterns( current, "bar" );
        REQUIRE( current == "foo|bar" );
    }

    SECTION( "combine joins boolean with ' or '" )
    {
        toolbar.setBoolean( true );
        QString current = "foo";
        toolbar.combinePatterns( current, "bar" );
        REQUIRE( current == "foo or bar" );
    }

    SECTION( "combine on empty current does not prepend a separator" )
    {
        toolbar.setUseRegexp( true );
        QString current;
        toolbar.combinePatterns( current, "first" );
        REQUIRE( current == "first" );
    }
}

TEST_CASE( "SearchToolbar wrapBooleanOperand quotes and escapes (excludeFromSearch path)",
           "[searchtoolbar]" )
{
    // CrawlerWidget::excludeFromSearch calls wrapBooleanOperand directly on the
    // current search text when transitioning into boolean mode -- the toggle may
    // still be off, so it can't rely on escapeSearchPattern's boolean branch.
    // This locks the operand-wrapping contract independently of the toggle.
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "plain pattern is surrounded by quotes" )
    {
        REQUIRE( toolbar.wrapBooleanOperand( "error" ) == "\"error\"" );
    }

    SECTION( "embedded double-quote is backslash-escaped" )
    {
        // Regression: excludeFromSearch inlined replace('"',"\"") -- a no-op
        // 1-char literal -- so input a"b wrapped to the broken "a"b" instead of
        // the correct "a\"b".
        REQUIRE( toolbar.wrapBooleanOperand( "a\"b" ) == "\"a\\\"b\"" );
    }

    SECTION( "empty pattern becomes a pair of quotes" )
    {
        REQUIRE( toolbar.wrapBooleanOperand( "" ) == "\"\"" );
    }

    SECTION( "multiple embedded quotes are each escaped" )
    {
        REQUIRE( toolbar.wrapBooleanOperand( "\"quoted\"" ) == "\"\\\"quoted\\\"\"" );
    }
}

TEST_CASE( "wrapBooleanOperand escapes backslashes so the operand stays parseable",
           "[searchtoolbar]" )
{
    // Regression (CodeRabbit, searchtoolbar.cpp:284-295): a boolean operand
    // ending in a backslash used to wrap as "C:\Users\" -- the trailing '\'
    // makes the boolean quote parser treat the CLOSING quote as escaped, so the
    // whole operand became "unmatched quotes" and the search failed. Escape
    // backslashes first (C-style '\\'), then embedded quotes, so the closing
    // quote sits after an even backslash run and the parser sees a real closer.
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "trailing backslash is doubled before wrapping" )
    {
        // C:\Users\  ->  "C:\\Users\\"  (backslash run before the closer is even)
        REQUIRE( toolbar.wrapBooleanOperand( "C:\\Users\\" ) == "\"C:\\\\Users\\\\\"" );
    }

    SECTION( "wrapped trailing-backslash operand parses and matches as boolean" )
    {
        const auto wrapped = toolbar.wrapBooleanOperand( "C:\\Users\\" );
        RegularExpression expression(
            RegularExpressionPattern( wrapped, false, false, true, true ) );
        REQUIRE( expression.isValid() );
        REQUIRE_FALSE( expression.errorString().contains( QLatin1String( "unmatched" ) ) );

        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ "path is C:\\Users\\" } ) );
        REQUIRE_FALSE( matcher->hasMatch( std::string_view{ "path is C:\\Users" } ) );
    }

    SECTION( "internal backslashes round-trip through the boolean parser" )
    {
        const auto wrapped = toolbar.wrapBooleanOperand( "C:\\Users\\admin" );
        RegularExpression expression(
            RegularExpressionPattern( wrapped, false, false, true, true ) );
        REQUIRE( expression.isValid() );

        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ "hello C:\\Users\\admin world" } ) );
    }
}

TEST_CASE( "SearchToolbar emits the expected signals", "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    SECTION( "searchRequested fires on Search button click" )
    {
        QSignalSpy spy( &toolbar, &SearchToolbar::searchRequested );
        toolbar.searchButton()->click();
        pumpEvents();
        REQUIRE( spy.count() >= 1 );
    }

    SECTION( "stopRequested fires on Stop button click (button enabled first)" )
    {
        // stopButton is disabled until a search is in progress.
        toolbar.setSearchInProgress( true );
        QSignalSpy spy( &toolbar, &SearchToolbar::stopRequested );
        toolbar.stopButton()->click();
        pumpEvents();
        REQUIRE( spy.count() >= 1 );
    }

    SECTION( "optionsChanged fires on matchCase toggle" )
    {
        QSignalSpy spy( &toolbar, &SearchToolbar::optionsChanged );
        toolbar.matchCaseButton()->toggle();
        pumpEvents();
        REQUIRE( toolbar.isMatchCase() == true );
        REQUIRE( spy.count() >= 1 );
    }

    SECTION( "autoRefreshChanged fires on searchRefresh toggle" )
    {
        QSignalSpy spy( &toolbar, &SearchToolbar::autoRefreshChanged );
        toolbar.searchRefreshButton()->toggle();
        pumpEvents();
        REQUIRE( toolbar.isAutoRefresh() == true );
        REQUIRE( spy.count() >= 1 );
    }
}

TEST_CASE( "SearchToolbar setSearchInProgress button dance", "[searchtoolbar]" )
{
    SearchToolbar toolbar( nullptr, nullptr );

    // Use isHidden() (not isVisible()) because the parent toolbar is never
    // shown in this headless test; isHidden() directly reflects the show()/
    // hide() calls made by setSearchInProgress.
    REQUIRE( toolbar.stopButton()->isHidden() == true );
    REQUIRE( toolbar.searchButton()->isHidden() == false );

    SECTION( "busy shows+enables stop, hides search+clear" )
    {
        toolbar.setSearchInProgress( true );
        REQUIRE( toolbar.stopButton()->isHidden() == false );
        REQUIRE( toolbar.stopButton()->isEnabled() == true );
        REQUIRE( toolbar.searchButton()->isHidden() == true );
        REQUIRE( toolbar.clearButton()->isHidden() == true );
    }

    SECTION( "idle again hides+disables stop, shows search+clear" )
    {
        toolbar.setSearchInProgress( true );
        toolbar.setSearchInProgress( false );
        REQUIRE( toolbar.stopButton()->isHidden() == true );
        REQUIRE( toolbar.stopButton()->isEnabled() == false );
        REQUIRE( toolbar.searchButton()->isHidden() == false );
        REQUIRE( toolbar.clearButton()->isHidden() == false );
    }
}

TEST_CASE( "SearchToolbar session round-trip (view-context flags)", "[searchtoolbar]" )
{
    // Mirrors CrawlerWidgetContext persistence: matchCase/useRegexp/inverse/
    // boolean/autoRefresh must round-trip through the setters/getters.
    SearchToolbar toolbar( nullptr, nullptr );

    toolbar.setMatchCase( true );
    toolbar.setUseRegexp( true );
    toolbar.setInverse( true );
    toolbar.setBoolean( true );
    toolbar.setAutoRefresh( true );

    REQUIRE( toolbar.isMatchCase() == true );
    REQUIRE( toolbar.isUseRegexp() == true );
    REQUIRE( toolbar.isInverse() == true );
    REQUIRE( toolbar.isBoolean() == true );
    REQUIRE( toolbar.isAutoRefresh() == true );

    // toggle back to false
    toolbar.setMatchCase( false );
    toolbar.setUseRegexp( false );
    toolbar.setInverse( false );
    toolbar.setBoolean( false );
    toolbar.setAutoRefresh( false );

    REQUIRE( toolbar.isMatchCase() == false );
    REQUIRE( toolbar.isUseRegexp() == false );
    REQUIRE( toolbar.isInverse() == false );
    REQUIRE( toolbar.isBoolean() == false );
    REQUIRE( toolbar.isAutoRefresh() == false );
}
