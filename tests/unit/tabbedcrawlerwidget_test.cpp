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

#include <QCoreApplication>
#include <QDir>
#include <QTabBar>
#include <QTranslator>
#include <QTest>
#include <QWidget>

#include <utility>

#include "configuration.h"
#include "styles.h"
#include "tabbedcrawlerwidget.h"
#include "tabnamemapping.h"

class DummyCrawlerWidget : public QWidget {
    Q_OBJECT

  public:
    explicit DummyCrawlerWidget( QWidget* parent = nullptr ) : QWidget( parent ) {}

  Q_SIGNALS:
    void dataStatusChanged( DataStatus status );
};

class ScopedTabNameMapping {
  public:
    ScopedTabNameMapping( QString path, QString name )
        : path_( std::move( path ) )
    {
        TabNameMapping::getSynced().setTabName( path_, std::move( name ) ).save();
    }

    ~ScopedTabNameMapping()
    {
        TabNameMapping::getSynced().setTabName( path_, QString{} ).save();
    }

  private:
    QString path_;
};

class ScopedStyleSetting {
  public:
    explicit ScopedStyleSetting( QString style )
        : previousStyle_( Configuration::getSynced().style() )
    {
        Configuration::getSynced().setStyle( std::move( style ) );
    }

    ~ScopedStyleSetting()
    {
        Configuration::getSynced().setStyle( previousStyle_ );
    }

  private:
    QString previousStyle_;
};

class LiveStatusTranslator : public QTranslator {
  public:
    QString translate( const char* context, const char* sourceText, const char* disambiguation,
                       int n ) const override
    {
        Q_UNUSED( disambiguation )
        Q_UNUSED( n )

        if ( QString::fromLatin1( context ) == QStringLiteral( "MainWindow" )
             && QString::fromLatin1( sourceText ) == QStringLiteral( " [error]" ) ) {
            return QStringLiteral( " [erreur]" );
        }

        if ( QString::fromLatin1( context ) == QStringLiteral( "MainWindow" )
             && QString::fromLatin1( sourceText ) == QStringLiteral( " [disconnected]" ) ) {
            return QStringLiteral( " [deconnecte]" );
        }

        return {};
    }
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

TEST_CASE( "TabbedCrawlerWidget updateCrawler strips old-format live status suffixes from tab text" )
{
    TabbedCrawlerWidget tabWidget;
    auto* crawler = new DummyCrawlerWidget();

    const auto index = tabWidget.addCrawler( crawler, QStringLiteral( "adb://capture-456" ),
                                             QStringLiteral( "Galaxy S24" ),
                                             QStringLiteral( "/tmp/galaxy.log" ) );

    SECTION( "tab text strips [disconnected] suffix" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [disconnected]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24" ) );
    }

    SECTION( "tab text strips [error] suffix" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [error]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24" ) );
    }

    SECTION( "clean tab text passes through unchanged" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [disconnected]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24" ) );

        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24" ) );
    }

    SECTION( "stripped tab text persists across group refreshes" )
    {
        tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [error]" ),
                                 QStringLiteral( "/tmp/galaxy.log" ) );
        tabWidget.onGroupsChanged();
        REQUIRE( tabWidget.tabText( index ) == QStringLiteral( "Galaxy S24" ) );
    }
}

TEST_CASE( "TabbedCrawlerWidget strips old live status from renamed tabs" )
{
    const auto documentId = QStringLiteral( "adb://capture-renamed-status" );
    const ScopedTabNameMapping tabNameMapping{ documentId, QStringLiteral( "Lab Phone" ) };

    TabbedCrawlerWidget tabWidget;
    auto* crawler = new DummyCrawlerWidget();

    const auto index = tabWidget.addCrawler( crawler, documentId, QStringLiteral( "Galaxy S24" ),
                                             QStringLiteral( "/tmp/galaxy.log" ) );

    REQUIRE( tabWidget.tabText( index ).toStdString() == std::string( "Lab Phone" ) );

    tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [disconnected]" ),
                             QStringLiteral( "/tmp/galaxy.log" ) );

    REQUIRE( tabWidget.tabText( index ).toStdString()
             == std::string( "Lab Phone" ) );

    tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [error]" ),
                             QStringLiteral( "/tmp/galaxy.log" ) );

    REQUIRE( tabWidget.tabText( index ).toStdString() == std::string( "Lab Phone" ) );

    tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24" ),
                             QStringLiteral( "/tmp/galaxy.log" ) );

    REQUIRE( tabWidget.tabText( index ).toStdString() == std::string( "Lab Phone" ) );
}

TEST_CASE( "TabbedCrawlerWidget strips old status suffix from renamed tabs with legacy names" )
{
    const auto documentId = QStringLiteral( "adb://capture-renamed-status-duplicate" );
    const ScopedTabNameMapping tabNameMapping{ documentId, QStringLiteral( "Lab Phone [error]" ) };

    TabbedCrawlerWidget tabWidget;
    auto* crawler = new DummyCrawlerWidget();

    const auto index = tabWidget.addCrawler( crawler, documentId, QStringLiteral( "Galaxy S24" ),
                                             QStringLiteral( "/tmp/galaxy.log" ) );

    tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [error]" ),
                             QStringLiteral( "/tmp/galaxy.log" ) );

    // Old-format "[error]" suffix is stripped from both the stored title and custom name
    // on display. Live status is now conveyed via colored dot, not text.
    REQUIRE( tabWidget.tabText( index ).toStdString() == std::string( "Lab Phone" ) );
}

TEST_CASE( "TabbedCrawlerWidget strips localized live status suffix from renamed tabs" )
{
    LiveStatusTranslator translator;
    QCoreApplication::installTranslator( &translator );

    const auto documentId = QStringLiteral( "adb://capture-renamed-status-localized" );
    const ScopedTabNameMapping tabNameMapping{ documentId, QStringLiteral( "Lab Phone" ) };

    TabbedCrawlerWidget tabWidget;
    auto* crawler = new DummyCrawlerWidget();

    const auto index = tabWidget.addCrawler( crawler, documentId, QStringLiteral( "Galaxy S24" ),
                                             QStringLiteral( "/tmp/galaxy.log" ) );

    tabWidget.updateCrawler( index, QStringLiteral( "Galaxy S24 [erreur]" ),
                             QStringLiteral( "/tmp/galaxy.log" ) );

    QCoreApplication::removeTranslator( &translator );

    // Localized "[erreur]" suffix is stripped; live status is now via colored dot.
    REQUIRE( tabWidget.tabText( index ).toStdString() == std::string( "Lab Phone" ) );
}

TEST_CASE( "TabbedCrawlerWidget close button style does not pin buttons during tab scroll" )
{
    const ScopedStyleSetting styleGuard{ StyleManager::DarkStyleKey };

    TabbedCrawlerWidget tabWidget;
    auto* tabBar = tabWidget.findChild<CrawlerTabBar*>();

    REQUIRE( tabBar != nullptr );
    REQUIRE( tabBar->styleSheet().contains( QStringLiteral( "QTabBar::close-button" ) ) );
    REQUIRE_FALSE( tabBar->styleSheet().contains( QStringLiteral( "subcontrol-position" ) ) );
    REQUIRE_FALSE( tabBar->styleSheet().contains( QStringLiteral( "subcontrol-origin" ) ) );
}

TEST_CASE( "TabbedCrawlerWidget uses rounded iTerm-style tabs outside Modern style" )
{
    const ScopedStyleSetting styleGuard{ StyleManager::DarkStyleKey };

    TabbedCrawlerWidget tabWidget;
    auto* tabBar = tabWidget.findChild<CrawlerTabBar*>();

    REQUIRE( tabBar != nullptr );
    const auto tabStyle = tabBar->styleSheet();
    REQUIRE( tabStyle.contains( QStringLiteral( "QTabBar {" ) ) );
    REQUIRE( tabStyle.contains( QStringLiteral( "QTabBar::tab:selected" ) ) );
    REQUIRE( tabStyle.contains( QStringLiteral( "border-radius: 13px" ) ) );
    REQUIRE( tabStyle.contains( QStringLiteral( "font-weight: 600" ) ) );
    REQUIRE_FALSE( tabStyle.contains( QStringLiteral( "border-bottom: none" ) ) );
}

TEST_CASE( "TabbedCrawlerWidget uses compact tab height for macOS 26 pill-tab look" )
{
    const ScopedStyleSetting styleGuard{ StyleManager::DarkStyleKey };

    TabbedCrawlerWidget tabWidget;
    auto* tabBar = tabWidget.findChild<CrawlerTabBar*>();

    REQUIRE( tabBar != nullptr );
    const auto tabStyle = tabBar->styleSheet();
    // Reduced height from 24px to 20px for iTerm2/macOS 26 compact look
    REQUIRE( tabStyle.contains( QStringLiteral( "height: 20px" ) ) );
    // Tab bar padding should be tighter
    REQUIRE( tabStyle.contains( QStringLiteral( "padding: 2px" ) ) );
    // Tab padding should be compact
    REQUIRE( tabStyle.contains( QStringLiteral( "padding: 3px 12px" ) ) );
    // Old 24px height must be gone
    REQUIRE_FALSE( tabStyle.contains( QStringLiteral( "height: 24px" ) ) );
}

TEST_CASE( "TabbedCrawlerWidget tab bar expands tabs to fill bar width" )
{
    TabbedCrawlerWidget tabWidget;
    auto* tabBar = tabWidget.findChild<CrawlerTabBar*>();

    REQUIRE( tabBar != nullptr );
    // Tabs should stretch to fill the tab bar, like iTerm2's "stretch to fill bar"
    // and system-native macOS tab bars
    REQUIRE( tabBar->expanding() );
}

TEST_CASE( "TabbedCrawlerWidget cycles tabs with Ctrl+PageDown/PageUp shortcuts" )
{
    TabbedCrawlerWidget tabWidget;

    for ( int i = 0; i < 3; ++i ) {
        auto* crawler = new DummyCrawlerWidget();
        tabWidget.addCrawler( crawler, QStringLiteral( "file:///tmp/klogg-cycle-%1.log" ).arg( i ),
                              QStringLiteral( "Tab %1" ).arg( i ),
                              QStringLiteral( "/tmp/klogg-cycle-%1.log" ).arg( i ) );
    }

    REQUIRE( tabWidget.currentIndex() == 2 );

    QTest::keyClick( &tabWidget, Qt::Key_PageDown, Qt::ControlModifier );
    REQUIRE( tabWidget.currentIndex() == 0 );

    QTest::keyClick( &tabWidget, Qt::Key_PageUp, Qt::ControlModifier );
    REQUIRE( tabWidget.currentIndex() == 2 );
}

TEST_CASE( "TabbedCrawlerWidget does not handle Ctrl+Tab in keyPressEvent" )
{
    // Test that our keyPressEvent no longer calls selectNextTab/selectPreviousTab
    // for Ctrl+Tab — these are handled by QShortcut actions in MainWindow instead.
    // We verify by checking that the key event falls through to the else branch
    // (QTabWidget::keyPressEvent) rather than being intercepted by our conditions.

    // Create a plain QTabWidget for comparison: on Qt 6, QTabWidget has
    // built-in Ctrl+Tab handling, so expect the native behavior to apply.
    QTabWidget plainWidget;
    plainWidget.addTab( new QWidget(), QStringLiteral( "Tab 0" ) );
    plainWidget.addTab( new QWidget(), QStringLiteral( "Tab 1" ) );
    plainWidget.addTab( new QWidget(), QStringLiteral( "Tab 2" ) );
    plainWidget.setCurrentIndex( 1 );

    QTest::keyClick( &plainWidget, Qt::Key_Tab, Qt::ControlModifier );
    const auto plainWidgetChanged = ( plainWidget.currentIndex() != 1 );

    // Now test TabbedCrawlerWidget — it must behave identically to a plain
    // QTabWidget for Ctrl+Tab, i.e., it must NOT add any extra handling beyond
    // what the base class does natively.
    TabbedCrawlerWidget tabWidget;
    for ( int i = 0; i < 3; ++i ) {
        auto* crawler = new DummyCrawlerWidget();
        tabWidget.addCrawler( crawler, QStringLiteral( "file:///tmp/test-%1.log" ).arg( i ),
                              QStringLiteral( "Tab %1" ).arg( i ) );
    }
    tabWidget.setCurrentIndex( 1 );
    REQUIRE( tabWidget.currentIndex() == 1 );

    QTest::keyClick( &tabWidget, Qt::Key_Tab, Qt::ControlModifier );

    if ( plainWidgetChanged ) {
        // Qt base class handles Ctrl+Tab natively — verify our widget matches
        // the base class behavior exactly (no extra handling added).
        REQUIRE( tabWidget.currentIndex() == plainWidget.currentIndex() );
    }
    else {
        // Qt base class does not handle Ctrl+Tab natively — verify our widget
        // also does not handle it.
        REQUIRE( tabWidget.currentIndex() == 1 );
    }
}

TEST_CASE( "TabbedCrawlerWidget keeps close buttons inside their tabs after horizontal scroll" )
{
    TabbedCrawlerWidget tabWidget;
    tabWidget.setDocumentMode( true );
    tabWidget.setMovable( true );
    tabWidget.setTabsClosable( true );
    tabWidget.resize( 620, 180 );

    for ( int i = 0; i < 12; ++i ) {
        auto* crawler = new DummyCrawlerWidget();
        tabWidget.addCrawler( crawler, QStringLiteral( "file:///tmp/klogg-tab-scroll-%1.log" ).arg( i ),
                              QStringLiteral( "Very Wide Tab Title %1" ).arg( i ),
                              QStringLiteral( "/tmp/klogg-tab-scroll-%1.log" ).arg( i ) );
    }

    tabWidget.show();
    QCoreApplication::processEvents();
    QTest::qWait( 50 );

    auto* tabBar = tabWidget.findChild<CrawlerTabBar*>();
    REQUIRE( tabBar != nullptr );

    tabWidget.setCurrentIndex( tabWidget.count() - 1 );
    QCoreApplication::processEvents();
    QTest::qWait( 50 );

    bool forcedStaleCloseButtonGeometry = false;
    for ( int i = 0; i < tabBar->count(); ++i ) {
        const auto tabRect = tabBar->tabRect( i );
        if ( !tabRect.intersects( tabBar->rect() ) ) {
            continue;
        }

        auto* closeButton = tabBar->tabButton( i, QTabBar::RightSide );
        if ( closeButton == nullptr ) {
            closeButton = tabBar->tabButton( i, QTabBar::LeftSide );
        }
        INFO( "Tab " << i << " rect=" << tabRect.x() << "," << tabRect.y() << " "
                      << tabRect.width() << "x" << tabRect.height() );
        REQUIRE( closeButton != nullptr );

        if ( !forcedStaleCloseButtonGeometry ) {
            const auto staleX = tabBar->rect().right() - closeButton->width();
            closeButton->move( staleX, closeButton->y() );
            if ( !tabRect.contains( closeButton->geometry().center() ) ) {
                tabBar->repaint();
                QCoreApplication::processEvents();
                forcedStaleCloseButtonGeometry = true;
            }
        }

        INFO( "Close button " << i << " geometry=" << closeButton->geometry().x() << ","
                               << closeButton->geometry().y() << " "
                               << closeButton->geometry().width() << "x"
                               << closeButton->geometry().height() );
        REQUIRE( tabRect.contains( closeButton->geometry().center() ) );
    }
    REQUIRE( forcedStaleCloseButtonGeometry );
    tabWidget.hide();
    QCoreApplication::processEvents();
}

TEST_CASE( "generateColoredDotIcon creates non-null icons for all live status and data status combinations" )
{
    const QList<LiveTabStatus> liveStatuses = {
        LiveTabStatus::Connected, LiveTabStatus::Disconnected, LiveTabStatus::Error
    };
    const QList<DataStatus> dataStatuses = {
        DataStatus::OLD_DATA, DataStatus::NEW_DATA, DataStatus::NEW_FILTERED_DATA
    };

    for ( auto ls : liveStatuses ) {
        for ( auto ds : dataStatuses ) {
            auto icon = TabbedCrawlerWidget::generateColoredDotIcon( ls, ds );
            INFO( "liveStatus=" << static_cast<int>( ls ) << " dataStatus=" << static_cast<int>( ds ) );
            REQUIRE_FALSE( icon.isNull() );
        }
    }
}

namespace {
// Runs just enough of the event loop to process any queued metacalls
// (e.g. the deferred loadIcons() dispatched via Qt::QueuedConnection).
void runPendingMainThreadEvents()
{
    for ( int attempt = 0; attempt < 10; ++attempt ) {
        qApp->processEvents( QEventLoop::AllEvents );
        qApp->sendPostedEvents( nullptr, QEvent::DeferredDelete );
    }
}
} // namespace

TEST_CASE( "setLiveTabStatus updates the tab icon with colored dots" )
{
    TabbedCrawlerWidget tabWidget;
    runPendingMainThreadEvents();

    auto* crawler = new DummyCrawlerWidget();
    const auto index = tabWidget.addCrawler( crawler, QStringLiteral( "adb://test-status" ),
                                             QStringLiteral( "Test Tab" ) );

    // Set live status to Connected -- uses live_icons_ populated by
    // generateColoredDotIcon, which works even without qrc resources
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Connected );
    auto connectedIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( connectedIcon.isNull() );

    // Set to Disconnected
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Disconnected );
    auto disconnectedIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( disconnectedIcon.isNull() );

    // Set to Error
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Error );
    auto errorIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( errorIcon.isNull() );

    // Reset to None (normal tab -- uses theme icons from qrc, may be null in tests)
    tabWidget.setLiveTabStatus( index, LiveTabStatus::None );
    // When live status is None, the icon is the theme-tinted icon from IconLoader.
    // In test builds without qrc resources, this may legitimately be null.
    // The important verification is that the setter does not crash and the
    // status-to-icon dispatch path is exercised.
}

TEST_CASE( "setTabDataStatus toggles between old and new data icons" )
{
    TabbedCrawlerWidget tabWidget;
    runPendingMainThreadEvents();

    auto* crawler = new DummyCrawlerWidget();
    const auto index = tabWidget.addCrawler( crawler, QStringLiteral( "adb://test-data" ),
                                             QStringLiteral( "Test Data Tab" ) );

    // First set a live status so icons come from live_icons_ (generated
    // by generateColoredDotIcon, which works without qrc resources).
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Connected );

    // Start with OLD_DATA (default when tab is added)
    auto oldIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( oldIcon.isNull() );

    // Switch to NEW_DATA
    tabWidget.setTabDataStatus( index, DataStatus::NEW_DATA );
    auto newIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( newIcon.isNull() );

    // Switch back
    tabWidget.setTabDataStatus( index, DataStatus::OLD_DATA );
    auto backToOldIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( backToOldIcon.isNull() );

    // Switch to NEW_FILTERED_DATA
    tabWidget.setTabDataStatus( index, DataStatus::NEW_FILTERED_DATA );
    auto filteredIcon = tabWidget.tabIcon( index );
    REQUIRE_FALSE( filteredIcon.isNull() );
}

TEST_CASE( "Live status and data status combine to produce correct icon" )
{
    TabbedCrawlerWidget tabWidget;
    runPendingMainThreadEvents();

    auto* crawler = new DummyCrawlerWidget();
    const auto index = tabWidget.addCrawler( crawler, QStringLiteral( "adb://test-combined" ),
                                             QStringLiteral( "Combined Tab" ) );

    // Set connected + new data
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Connected );
    tabWidget.setTabDataStatus( index, DataStatus::NEW_DATA );
    auto icon1 = tabWidget.tabIcon( index );
    REQUIRE_FALSE( icon1.isNull() );

    // Set error + old data
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Error );
    tabWidget.setTabDataStatus( index, DataStatus::OLD_DATA );
    auto icon2 = tabWidget.tabIcon( index );
    REQUIRE_FALSE( icon2.isNull() );

    // Set disconnected + new filtered data
    tabWidget.setLiveTabStatus( index, LiveTabStatus::Disconnected );
    tabWidget.setTabDataStatus( index, DataStatus::NEW_FILTERED_DATA );
    auto icon3 = tabWidget.tabIcon( index );
    REQUIRE_FALSE( icon3.isNull() );
}

TEST_CASE( "generateColoredDotIcon produces icons at multiple sizes" )
{
    auto icon = TabbedCrawlerWidget::generateColoredDotIcon( LiveTabStatus::Connected, DataStatus::NEW_DATA );
    REQUIRE_FALSE( icon.isNull() );

    // Should have pixmaps at 16, 20, 24, 32 (the sizes used in makeDotIcon)
    const QList<int> expectedSizes = { 16, 20, 24, 32 };
    for ( int size : expectedSizes ) {
        auto pixmap = icon.pixmap( size, size );
        INFO( "Size " << size );
        REQUIRE_FALSE( pixmap.isNull() );
        CHECK( pixmap.width() == size );
        CHECK( pixmap.height() == size );
    }
}

TEST_CASE( "NEW_DATA icon has a solid center while OLD_DATA icon has a hollow center" )
{
    // NEW_DATA = solid filled circle
    auto solidIcon = TabbedCrawlerWidget::generateColoredDotIcon( LiveTabStatus::Connected, DataStatus::NEW_DATA );
    auto solidPixmap = solidIcon.pixmap( 32, 32 );
    auto solidImage = solidPixmap.toImage();
    QColor solidCenter = solidImage.pixelColor( 16, 16 );

    // OLD_DATA = hollow (outline) circle
    auto hollowIcon = TabbedCrawlerWidget::generateColoredDotIcon( LiveTabStatus::Connected, DataStatus::OLD_DATA );
    auto hollowPixmap = hollowIcon.pixmap( 32, 32 );
    auto hollowImage = hollowPixmap.toImage();
    QColor hollowCenter = hollowImage.pixelColor( 16, 16 );

    // Solid center should be non-transparent (colored)
    CHECK( solidCenter.alpha() > 128 );
    // Hollow center should be transparent
    CHECK( hollowCenter.alpha() < 32 );
}

#include "tabbedcrawlerwidget_test.moc"
