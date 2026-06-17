/*
 * Copyright (C) 2026 ZEACENT and other contributors
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

#include <QApplication>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTest>

#include "newversiondialog.h"

namespace {

// Generate a long changelog that simulates a detailed GitHub release body.
QStringList makeLongChangelog()
{
    QStringList changes;
    QString longBody;
    // Simulate a detailed release body with many lines
    longBody += QStringLiteral( "## What's New\n\n" );
    for ( int i = 1; i <= 50; ++i ) {
        longBody += QStringLiteral( "- Feature %1: This is a very detailed description "
                                    "of an exciting new feature that was added.\n" )
                        .arg( i );
    }
    longBody += QStringLiteral( "\n## Bug Fixes\n\n" );
    for ( int i = 1; i <= 50; ++i ) {
        longBody += QStringLiteral( "- Bug fix %1: Fixed a critical issue that caused "
                                    "unexpected behavior when processing large files.\n" )
                        .arg( i );
    }
    changes << longBody;
    return changes;
}

} // namespace

TEST_CASE( "NewVersionDialog: height is constrained with long changelog",
           "[newversiondialog]" )
{
    // The dialog must not grow unboundedly when the changelog is long.
    // Instead, the changelog area should be scrollable and height-limited.

    const auto longChanges = makeLongChangelog();
    REQUIRE_FALSE( longChanges.isEmpty() );

    NewVersionDialog dlg( QStringLiteral( "99.0.0.0" ),
                          QStringLiteral( "https://github.com/ZEACENT/klogg/releases/tag/v99.0.0.0" ),
                          longChanges );

    // The dialog height should be constrained (well under what a long
    // changelog would produce without a scroll area).
    const auto hint = dlg.sizeHint();

    // With a scroll area, the dialog should stay below ~600px.
    // Without it, the QMessageBox would easily exceed 2000px for 100+ lines.
    CHECK( hint.height() <= 600 );

    // The dialog should have a reasonable minimum height to be usable
    CHECK( hint.height() >= 200 );
}

TEST_CASE( "NewVersionDialog: changelog text browser is scrollable",
           "[newversiondialog]" )
{
    const auto longChanges = makeLongChangelog();

    NewVersionDialog dlg( QStringLiteral( "99.0.0.0" ),
                          QStringLiteral( "https://github.com/ZEACENT/klogg/releases/tag/v99.0.0.0" ),
                          longChanges );

    // Find the QTextBrowser — it must exist and have a vertical scroll bar
    // when the content exceeds its maximum height.
    const auto textBrowsers = dlg.findChildren<QTextBrowser*>();
    REQUIRE_FALSE( textBrowsers.isEmpty() );

    auto* browser = textBrowsers.first();
    REQUIRE( browser != nullptr );

    // The browser should have a constrained maximum height
    CHECK( browser->maximumHeight() == NewVersionDialog::kMaxChangesHeight );

    // Show the dialog offscreen to let layout settle, then check scrollbar
    dlg.show();
    QTest::qWait( 50 );

    auto* vScrollBar = browser->verticalScrollBar();
    REQUIRE( vScrollBar != nullptr );

    // The content should be longer than the visible area, making the
    // scrollbar necessary (maximum > 0 means scrolling is possible).
    CHECK( vScrollBar->maximum() > 0 );
}

TEST_CASE( "NewVersionDialog: no changelog section when changes are empty",
           "[newversiondialog]" )
{
    NewVersionDialog dlg( QStringLiteral( "99.0.0.0" ),
                          QStringLiteral( "https://github.com/ZEACENT/klogg/releases/tag/v99.0.0.0" ),
                          QStringList{} );

    // Without changes, there should be no QTextBrowser
    const auto textBrowsers = dlg.findChildren<QTextBrowser*>();
    CHECK( textBrowsers.isEmpty() );

    // Dialog should still have a reasonable size
    const auto hint = dlg.sizeHint();
    CHECK( hint.height() <= 600 );
    CHECK( hint.height() >= 100 );
}

TEST_CASE( "NewVersionDialog: short changelog also fits constraints",
           "[newversiondialog]" )
{
    QStringList shortChanges;
    shortChanges << QStringLiteral( "## Changes\n\n- Minor bug fix\n- Performance improvement" );

    NewVersionDialog dlg( QStringLiteral( "99.0.0.0" ),
                          QStringLiteral( "https://github.com/ZEACENT/klogg/releases/tag/v99.0.0.0" ),
                          shortChanges );

    const auto textBrowsers = dlg.findChildren<QTextBrowser*>();
    REQUIRE_FALSE( textBrowsers.isEmpty() );

    const auto hint = dlg.sizeHint();
    CHECK( hint.height() <= 600 );
    CHECK( hint.height() >= 150 );
}

TEST_CASE( "NewVersionDialog: buttons emit correct result", "[newversiondialog]" )
{
    NewVersionDialog dlg( QStringLiteral( "99.0.0.0" ),
                          QStringLiteral( "https://github.com/ZEACENT/klogg/releases/tag/v99.0.0.0" ),
                          QStringList{} );

    // Test that the dialog starts with the expected initial button state
    // (clickedButton should not be used before exec/show, but defaults to RemindLater)
    CHECK( dlg.clickedButton() == NewVersionDialog::RemindLater );
}
