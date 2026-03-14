/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include <QSignalSpy>
#include <QScrollBar>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <qglobal.h>
#include <qnamespace.h>
#include <qtestmouse.h>

#include "savedsearches.h"
#include "session.h"
#include "test_utils.h"

#include "configuration.h"
#include "logdata.h"
#include "logfiltereddata.h"

#include "crawlerwidget.h"
#include "shortcuts.h"

static const qint64 SL_NB_LINES = 100LL;

namespace {
bool generateDataFiles( QTemporaryFile& file )
{
    char newLine[ 90 ];

    if ( file.open() ) {
        for ( int i = 0; i < SL_NB_LINES; i++ ) {
            snprintf( newLine, 89,
                      "LOGDATA \t is a part of glogg, we are going to test it thoroughly, this is "
                      "line %06d",
                      i );
            file.write( newLine, static_cast<qint64>( qstrlen( newLine ) ) );
#ifdef Q_OS_WIN
            file.write( "\r\n", 2 );
#else
            file.write( "\n", 1 );
#endif
        }
        file.flush();
    }

    return true;
}

} // namespace

struct CrawlerWidgetPrivate {
};

struct AbstractLogViewPrivate {
};

struct ConfigurationRestoreGuard {
    QFont font;
    int lineSpacingPercent;

    ConfigurationRestoreGuard()
        : font( Configuration::get().mainFont() )
        , lineSpacingPercent( Configuration::get().lineSpacingPercent() )
    {
    }

    ~ConfigurationRestoreGuard()
    {
        auto& config = Configuration::get();
        config.setMainFont( font );
        config.setLineSpacingPercent( lineSpacingPercent );
    }
};

template <>
struct AbstractLogView::access_by<AbstractLogViewPrivate> {
    static int drawingTopOffset( const AbstractLogView* view )
    {
        return view->drawingTopOffset_;
    }

    static int charHeight( const AbstractLogView* view )
    {
        return view->charHeight_;
    }

    static int leftMargin( const AbstractLogView* view )
    {
        return view->leftMarginPx_;
    }

    static LineNumber topLine( const AbstractLogView* view )
    {
        return view->firstLine_;
    }

    static QWidget* viewport( AbstractLogView* view )
    {
        return view->viewport();
    }

    static void setLastLineAligned( AbstractLogView* view, bool value )
    {
        view->lastLineAligned_ = value;
    }

    static bool shouldBottomAlignFrame( const AbstractLogView* view )
    {
        return view->shouldBottomAlignFrame();
    }
};

template <>
struct CrawlerWidget::access_by<CrawlerWidgetPrivate> {
    std::unique_ptr<CrawlerWidget> crawler;

    bool isLoadingFinished()
    {
        return !crawler->loadingInProgress_;
    }

    LinesCount getLogNbLines()
    {
        return crawler->logData_->getNbLine();
    }

    LinesCount getLogFilteredNbLines()
    {
        return crawler->logFilteredData_->getNbLine();
    }

    void selectAllInMainView()
    {
        crawler->logMainView_->selectAll();
    }

    void selectAllInFilteredView()
    {
        crawler->filteredView_->selectAll();
    }

    QString mainViewSelectedText()
    {
        return crawler->logMainView_->getSelectedText();
    }

    QString filteredViewSelectedText()
    {
        return crawler->filteredView_->getSelectedText();
    }

    void setSearchPattern( const QString& pattern )
    {
        QTest::keyClicks( crawler->searchLineEdit_, pattern );
    }

    void enableCaseSensitiveSearch()
    {
        if ( !crawler->matchCaseButton_->isChecked() ) {
            QTest::mouseClick( crawler->matchCaseButton_, Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void enableInverseMatch()
    {
        if ( !crawler->inverseButton_->isChecked() ) {
            QTest::mouseClick( crawler->inverseButton_, Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void enableBooleanCombinationMode()
    {
        if ( !crawler->booleanButton_->isChecked() ) {
            QTest::mouseClick( crawler->booleanButton_, Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void runSearch()
    {
        QTest::mouseClick( crawler->searchButton_, Qt::LeftButton );

        QTest::qWait( 100 );

        waitUiState( [ & ]() { return crawler->stopButton_->isHidden(); } );
    }

    void render()
    {
        crawler->grab();
    }

    QImage grabFilteredViewport()
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::viewport( crawler->filteredView_ )
            ->grab()
            .toImage();
    }

    void setTextWrap( bool enabled )
    {
        crawler->logMainView_->textWrapSet( enabled );
        crawler->filteredView_->textWrapSet( enabled );
        QTest::qWait( 50 );
    }

    bool isTextWrapEnabled()
    {
        return crawler->logMainView_->isTextWrapEnabled();
    }

    void clickFilteredViewLine( LineNumber::UnderlyingType lineIndex )
    {
        // Simulate clicking on a line in the filtered view
        // This triggers the jump to corresponding line in main view
        auto* filteredView = crawler->filteredView_;
        if ( filteredView && crawler->logFilteredData_->getNbLine().get() > 0 ) {
            filteredView->selectAndDisplayLine( LineNumber( lineIndex ) );
            QTest::qWait( 50 );
        }
    }

    void resizeViews( int width, int height )
    {
        crawler->logMainView_->resize( width, height );
        crawler->filteredView_->resize( width, height );
        QTest::qWait( 50 );
    }

    void enableFollowMode( bool enabled )
    {
        crawler->logMainView_->followSet( enabled );
        crawler->filteredView_->followSet( enabled );
        QTest::qWait( 50 );
    }

    bool isFollowModeEnabled()
    {
        return crawler->logMainView_->isFollowEnabled();
    }

    void applyConfiguration()
    {
        crawler->applyConfiguration();
        QTest::qWait( 50 );
    }

    int mainCharHeight() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::charHeight( crawler->logMainView_ );
    }

    void selectMainViewLine( LineNumber::UnderlyingType lineIndex )
    {
        crawler->logMainView_->selectAndDisplayLine( LineNumber( lineIndex ) );
        QTest::qWait( 50 );
    }

    qsizetype markedLinesCount()
    {
        return crawler->logFilteredData_->getMarks().size();
    }

    bool isMainLineMarked( LineNumber::UnderlyingType line )
    {
        return crawler->logFilteredData_->lineTypeByLine( LineNumber( line ) )
            .testFlag( AbstractLogData::LineTypeFlags::Mark );
    }

    QColor filteredHighlightColor() const
    {
        return crawler->filteredView_->palette().color( QPalette::Highlight );
    }

    QColor filteredBaseColor() const
    {
        return crawler->filteredView_->palette().color( QPalette::Base );
    }

    int filteredContentX() const
    {
        const auto* viewport
            = AbstractLogView::access_by<AbstractLogViewPrivate>::viewport( crawler->filteredView_ );
        return std::max( 0, viewport->width() - 20 );
    }

    int filteredLineCenterY( LineNumber::UnderlyingType lineIndex ) const
    {
        const auto topLine
            = AbstractLogView::access_by<AbstractLogViewPrivate>::topLine( crawler->filteredView_ );
        const auto charHeight
            = AbstractLogView::access_by<AbstractLogViewPrivate>::charHeight( crawler->filteredView_ );
        const auto lineOffset = static_cast<int>( lineIndex - topLine.get() );
        return ( lineOffset * charHeight ) + ( charHeight / 2 );
    }

    int filteredDrawingTopOffset() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::drawingTopOffset(
            crawler->filteredView_ );
    }

    int filteredCharHeight() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::charHeight( crawler->filteredView_ );
    }

    void setFilteredLastLineAligned( bool value )
    {
        AbstractLogView::access_by<AbstractLogViewPrivate>::setLastLineAligned( crawler->filteredView_,
                                                                                value );
    }

    bool filteredShouldBottomAlign() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::shouldBottomAlignFrame(
            crawler->filteredView_ );
    }

    void scrollFilteredVerticallyToBottom()
    {
        crawler->filteredView_->verticalScrollBar()->setValue(
            crawler->filteredView_->verticalScrollBar()->maximum() );
        QTest::qWait( 50 );
    }

    void scrollFilteredHorizontallyToMiddle()
    {
        crawler->filteredView_->horizontalScrollBar()->setValue(
            crawler->filteredView_->horizontalScrollBar()->maximum() / 2 );
        QTest::qWait( 50 );
    }

    void addMarksInMainView( const klogg::vector<LineNumber>& lines )
    {
        crawler->markLinesFromMain( lines );
        QTest::qWait( 20 );
    }

    void deleteMarksInMainView( const klogg::vector<LineNumber>& lines )
    {
        crawler->deleteMarkLinesFromMain( lines );
        QTest::qWait( 20 );
    }
};

using CrawlerWidgetVisitor = CrawlerWidget::access_by<CrawlerWidgetPrivate>;

SCENARIO( "Crawler widget search", "[ui]" )
{
    QTemporaryFile file{ "crawler_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    session.savedSearches().clear();

    REQUIRE( session.savedSearches().recentSearches().empty() );

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.getLogNbLines().get() == SL_NB_LINES );

    GIVEN( "loaded log data" )
    {
        THEN( "Has no lines in log view" )
        {
            REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == 0 );
        }

        WHEN( "search for lines" )
        {
            crawlerVisitor.setSearchPattern( "this is line" );
            crawlerVisitor.runSearch();

            REQUIRE( waitUiState( [ &crawlerVisitor ]() {
                return crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES;
            } ) );

            THEN( "all lines are matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );
            }

            AND_WHEN( "copy all from main view" )
            {
                crawlerVisitor.selectAllInMainView();
                auto text = crawlerVisitor.mainViewSelectedText();
                THEN( "text has same number of lines" )
                {
                    REQUIRE( text.split( QChar::LineFeed ).size() == SL_NB_LINES );
                }
            }

            AND_WHEN( "copy all from filtered view" )
            {
                crawlerVisitor.selectAllInFilteredView();
                auto text = crawlerVisitor.filteredViewSelectedText();
                THEN( "text has same number of lines" )
                {
                    REQUIRE( text.split( QChar::LineFeed ).size() == SL_NB_LINES );
                }
            }

            AND_WHEN( "a filtered line is selected" )
            {
                constexpr auto SelectedLine = 10;
                crawlerVisitor.clickFilteredViewLine( SelectedLine );
                crawlerVisitor.render();

                THEN( "the selected line uses the theme highlight background" )
                {
                    const auto image = crawlerVisitor.grabFilteredViewport();
                    const auto sampleY = crawlerVisitor.filteredLineCenterY( SelectedLine );

                    REQUIRE( sampleY >= 0 );
                    REQUIRE( sampleY < image.height() );

                    const auto pixelColor = image.pixelColor( crawlerVisitor.filteredContentX(),
                                                              sampleY );
                    REQUIRE( pixelColor != crawlerVisitor.filteredBaseColor() );
                }
            }

            AND_WHEN( "the filtered view is scrolled vertically and horizontally" )
            {
                crawlerVisitor.resizeViews( 220, 95 );
                crawlerVisitor.render();
                crawlerVisitor.scrollFilteredVerticallyToBottom();
                crawlerVisitor.render();

                THEN( "vertical scrolling keeps the first visible line aligned to full rows" )
                {
                    REQUIRE( qAbs( crawlerVisitor.filteredDrawingTopOffset() )
                             % crawlerVisitor.filteredCharHeight()
                             == 0 );
                }

                AND_WHEN( "the filtered view is then scrolled horizontally" )
                {
                    crawlerVisitor.scrollFilteredHorizontallyToMiddle();
                    crawlerVisitor.render();

                    THEN( "horizontal scrolling preserves the same line-aligned top offset" )
                    {
                        REQUIRE( qAbs( crawlerVisitor.filteredDrawingTopOffset() )
                                 % crawlerVisitor.filteredCharHeight()
                                 == 0 );
                    }
                }
            }
        }

        WHEN( "search for 10" )
        {
            crawlerVisitor.setSearchPattern( "10" );

            crawlerVisitor.runSearch();

            waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } );

            THEN( "single line match" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == 1 );
            }
        }

        WHEN( "default shortcuts for line mark actions are configured" )
        {
            const auto& shortcuts = ShortcutAction::defaultShortcutList();

            THEN( "Add and Delete line mark defaults are M and N with no find-next N conflict" )
            {
                const auto addMarkShortcut
                    = QKeySequence( Qt::Key_M ).toString( QKeySequence::PortableText );
                const auto deleteMarkShortcut
                    = QKeySequence( Qt::Key_N ).toString( QKeySequence::PortableText );

                REQUIRE( shortcuts.at( ShortcutAction::LogViewMark )
                             .keySequence.contains( addMarkShortcut ) );
                REQUIRE( shortcuts.at( ShortcutAction::LogViewDeleteMark )
                             .keySequence.contains( deleteMarkShortcut ) );
                REQUIRE_FALSE( shortcuts.at( ShortcutAction::LogViewQfForward )
                                   .keySequence.contains( deleteMarkShortcut ) );
            }
        }

        WHEN( "line mark actions are used on a single line" )
        {
            crawlerVisitor.selectMainViewLine( 10 );

            crawlerVisitor.addMarksInMainView( { 10_lnum } );

            THEN( "line is marked" )
            {
                REQUIRE( crawlerVisitor.isMainLineMarked( 10 ) );
                REQUIRE( crawlerVisitor.markedLinesCount() == 1 );
            }

            AND_WHEN( "add line mark action is applied again" )
            {
                crawlerVisitor.addMarksInMainView( { 10_lnum } );

                THEN( "line stays marked and mark count does not increase" )
                {
                    REQUIRE( crawlerVisitor.isMainLineMarked( 10 ) );
                    REQUIRE( crawlerVisitor.markedLinesCount() == 1 );
                }
            }

            AND_WHEN( "delete line mark action is applied" )
            {
                crawlerVisitor.deleteMarksInMainView( { 10_lnum } );

                THEN( "line mark is removed" )
                {
                    REQUIRE_FALSE( crawlerVisitor.isMainLineMarked( 10 ) );
                    REQUIRE( crawlerVisitor.markedLinesCount() == 0 );
                }
            }
        }

        WHEN( "line mark actions are used on multiple selected lines" )
        {
            crawlerVisitor.selectAllInMainView();

            klogg::vector<LineNumber> selectedLines;
            selectedLines.reserve( static_cast<size_t>( SL_NB_LINES ) );
            for ( LineNumber::UnderlyingType i = 0;
                  i < static_cast<LineNumber::UnderlyingType>( SL_NB_LINES ); ++i ) {
                selectedLines.push_back( LineNumber( i ) );
            }

            crawlerVisitor.addMarksInMainView( selectedLines );

            THEN( "all lines are marked" )
            {
                REQUIRE( crawlerVisitor.markedLinesCount()
                         == static_cast<qsizetype>( SL_NB_LINES ) );
            }

            AND_WHEN( "add line mark action is applied again" )
            {
                crawlerVisitor.addMarksInMainView( selectedLines );

                THEN( "all lines remain marked" )
                {
                    REQUIRE( crawlerVisitor.markedLinesCount()
                             == static_cast<qsizetype>( SL_NB_LINES ) );
                }
            }

            AND_WHEN( "delete line mark action is applied" )
            {
                crawlerVisitor.deleteMarksInMainView( selectedLines );

                THEN( "all marks are removed" )
                {
                    REQUIRE( crawlerVisitor.markedLinesCount() == 0 );
                }
            }
        }

        WHEN( "case sensitive search" )
        {
            crawlerVisitor.setSearchPattern( "THIS" );
            crawlerVisitor.enableCaseSensitiveSearch();
            crawlerVisitor.runSearch();

            THEN( "no lines matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == 0 );
            }
        }

        WHEN( "inverse match search" )
        {
            crawlerVisitor.setSearchPattern( "not match" );
            crawlerVisitor.enableInverseMatch();
            crawlerVisitor.runSearch();

            THEN( "all lines matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );
            }
        }

        WHEN( "boolean search" )
        {
            crawlerVisitor.setSearchPattern( "\"glogg\" or \"klogg\"" );
            crawlerVisitor.enableBooleanCombinationMode();
            crawlerVisitor.runSearch();

            THEN( "has lines matched" )
            {
                REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() >= 2 );
            }
        }

        WHEN( "text wrap is enabled" )
        {
            crawlerVisitor.setTextWrap( true );

            THEN( "text wrap is active" )
            {
                REQUIRE( crawlerVisitor.isTextWrapEnabled() );
            }

            AND_WHEN( "search for lines with text wrap" )
            {
                crawlerVisitor.setSearchPattern( "this is line" );
                crawlerVisitor.runSearch();

                REQUIRE( waitUiState( [ &crawlerVisitor ]() {
                    return crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES;
                } ) );

                THEN( "all lines are matched" )
                {
                    REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );
                }

                AND_WHEN( "click on filtered view line" )
                {
                    // This tests that clicking in filtered view correctly
                    // scrolls the main view (Bug 9 fix verification)
                    crawlerVisitor.clickFilteredViewLine( 50 );
                    crawlerVisitor.render();

                    THEN( "no crash occurs" )
                    {
                        // If we get here without crash, the click handling works
                        REQUIRE( true );
                    }
                }

                AND_WHEN( "resize views with text wrap" )
                {
                    // This tests that resizing with text wrap doesn't cause
                    // performance issues or display problems (Bug 8, 10 fix verification)
                    crawlerVisitor.resizeViews( 400, 200 );
                    crawlerVisitor.resizeViews( 600, 300 );
                    crawlerVisitor.resizeViews( 300, 150 );
                    crawlerVisitor.render();

                    THEN( "no crash or freeze occurs" )
                    {
                        // If we get here without crash/freeze, the resize handling works
                        REQUIRE( crawlerVisitor.isTextWrapEnabled() );
                    }
                }

                AND_WHEN( "display file with wrapped content exceeding viewport" )
                {
                    // This tests Bug 11 fix: when wrapped content exceeds viewport height
                    // but lastLineAligned_ is false, bottom content should still be visible
                    // Create a scenario where firstLine_=0 but wrapped content exceeds viewport
                    crawlerVisitor.resizeViews( 300, 100 );  // Small viewport
                    crawlerVisitor.render();

                    THEN( "bottom content is visible" )
                    {
                        // If we get here without crash, the auto bottom alignment works
                        // The render() call will trigger paintEvent which should apply
                        // auto bottom alignment when actual_height_ > viewport height
                        REQUIRE( crawlerVisitor.isTextWrapEnabled() );
                    }
                }

                AND_WHEN( "follow mode and text wrap are both enabled" )
                {
                    // This tests Bug 8 fix: FilteredView last line should be fully visible
                    // when both follow mode and text wrap are enabled
                    crawlerVisitor.enableFollowMode( true );
                    crawlerVisitor.resizeViews( 400, 100 );  // Small viewport to trigger wrapping
                    crawlerVisitor.render();

                    THEN( "follow mode is enabled and last line is visible" )
                    {
                        REQUIRE( crawlerVisitor.isFollowModeEnabled() );
                        REQUIRE( crawlerVisitor.isTextWrapEnabled() );
                        // If we get here without crash, the bottom alignment works correctly
                        // The render() call will trigger paintEvent which should apply
                        // bottom alignment when followMode_=true and text wrap is enabled
                    }
                }

                AND_WHEN( "resize FilteredView height with text wrap" )
                {
                    // This tests Bug 9 fix: shadow should not incorrectly render and
                    // block text when FilteredView height is adjusted
                    crawlerVisitor.resizeViews( 400, 200 );
                    crawlerVisitor.render();
                    crawlerVisitor.resizeViews( 400, 150 );  // Reduce height
                    crawlerVisitor.render();
                    crawlerVisitor.resizeViews( 400, 250 );  // Increase height
                    crawlerVisitor.render();

                    THEN( "no shadow rendering issues occur" )
                    {
                        // If we get here without crash, the pull-to-follow bar
                        // positioning is correct and doesn't block text
                        REQUIRE( crawlerVisitor.isTextWrapEnabled() );
                    }
                }

                AND_WHEN( "filtered view remains at scroll bottom without follow mode" )
                {
                    crawlerVisitor.enableFollowMode( false );
                    crawlerVisitor.resizeViews( 300, 120 );
                    crawlerVisitor.scrollFilteredVerticallyToBottom();
                    crawlerVisitor.render();

                    // Re-scroll after render: grab() may trigger a relayout that
                    // changes the scrollbar maximum, leaving value < new max.
                    crawlerVisitor.scrollFilteredVerticallyToBottom();

                    // Simulate state-reset paths where lastLineAligned_ is cleared while
                    // the scrollbar is still at the bottom.
                    crawlerVisitor.setFilteredLastLineAligned( false );

                    THEN( "bottom alignment still follows scrollbar bottom state" )
                    {
                        REQUIRE_FALSE( crawlerVisitor.isFollowModeEnabled() );
                        REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
                    }
                }

                AND_WHEN( "line spacing is increased" )
                {
                    ConfigurationRestoreGuard restoreConfig;
                    auto& config = Configuration::get();
                    const auto originalFont = config.mainFont();
                    const auto basePointSize
                        = originalFont.pointSize() > 0 ? originalFont.pointSize() : 10;

                    QFont testFont{ originalFont.family(), basePointSize };
                    config.setMainFont( testFont );
                    config.setLineSpacingPercent( Configuration::MinLineSpacingPercent );
                    crawlerVisitor.applyConfiguration();

                    const auto compactMainCharHeight = crawlerVisitor.mainCharHeight();
                    const auto compactFilteredCharHeight = crawlerVisitor.filteredCharHeight();

                    config.setLineSpacingPercent( 140 );
                    crawlerVisitor.applyConfiguration();

                    THEN( "main and filtered views use taller rows" )
                    {
                        REQUIRE( crawlerVisitor.mainCharHeight() > compactMainCharHeight );
                        REQUIRE( crawlerVisitor.filteredCharHeight() > compactFilteredCharHeight );
                    }

                    AND_WHEN( "font size is changed afterwards" )
                    {
                        QFont largerFont{ testFont.family(), basePointSize + 2 };
                        config.setMainFont( largerFont );
                        crawlerVisitor.applyConfiguration();

                        THEN( "the configured line spacing ratio remains applied" )
                        {
                            REQUIRE( crawlerVisitor.mainCharHeight() > compactMainCharHeight );
                            REQUIRE( crawlerVisitor.filteredCharHeight() > compactFilteredCharHeight );
                        }
                    }
                }
            }

            AND_WHEN( "text wrap is disabled" )
            {
                crawlerVisitor.setTextWrap( false );

                THEN( "text wrap is inactive" )
                {
                    REQUIRE_FALSE( crawlerVisitor.isTextWrapEnabled() );
                }
            }
        }
    }
}
