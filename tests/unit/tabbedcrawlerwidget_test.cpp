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

#include <QDir>
#include <QWidget>

#include "tabbedcrawlerwidget.h"

class DummyCrawlerWidget : public QWidget {
    Q_OBJECT

  public:
    explicit DummyCrawlerWidget( QWidget* parent = nullptr ) : QWidget( parent ) {}

  Q_SIGNALS:
    void dataStatusChanged( DataStatus status );
};

TEST_CASE( "TabbedCrawlerWidget keeps live tab title and tooltip across group refreshes" )
{
    TabbedCrawlerWidget tabWidget;
    auto* crawler = new DummyCrawlerWidget();

    const auto index = tabWidget.addCrawler( crawler, QStringLiteral( "adb://capture-123" ),
                                             QStringLiteral( "Pixel 8 Pro" ),
                                             QStringLiteral( "/tmp/pixel.log" ) );

    REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Pixel 8 Pro" ) );
    REQUIRE( tabWidget.tabToolTip( index ) == QDir::toNativeSeparators( QStringLiteral( "/tmp/pixel.log" ) ) );

    tabWidget.onGroupsChanged();

    REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Pixel 8 Pro" ) );
    REQUIRE( tabWidget.tabToolTip( index ) == QDir::toNativeSeparators( QStringLiteral( "/tmp/pixel.log" ) ) );

    tabWidget.updateCrawler( index, QStringLiteral( "Pixel 8 Pro" ),
                             QStringLiteral( "/tmp/pixel-saved.log" ) );
    tabWidget.onGroupsChanged();

    REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Pixel 8 Pro" ) );
    REQUIRE( tabWidget.tabToolTip( index )
             == QDir::toNativeSeparators( QStringLiteral( "/tmp/pixel-saved.log" ) ) );
}

TEST_CASE( "TabbedCrawlerWidget updateCrawler reflects disconnect and error state in tab text" )
{
    TabbedCrawlerWidget tabWidget;
    auto* crawler = new DummyCrawlerWidget();

    const auto index = tabWidget.addCrawler( crawler, QStringLiteral( "adb://capture-456" ),
                                             QStringLiteral( "Galaxy S24" ),
                                             QStringLiteral( "/tmp/galaxy.log" ) );

    SECTION( "tab text shows [disconnected] suffix" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [disconnected]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24 [disconnected]" ) );
    }

    SECTION( "tab text shows [error] suffix" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [error]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24 [error]" ) );
    }

    SECTION( "tab text restores to normal on reconnect" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [disconnected]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24 [disconnected]" ) );

        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24" ) );
    }

    SECTION( "tab text persists across group refreshes" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [error]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        tabWidget.onGroupsChanged();
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24 [error]" ) );
    }
}

#include "tabbedcrawlerwidget_test.moc"
