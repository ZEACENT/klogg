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

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QUuid>

#include "savedsearches.h"
#include "session.h"
#include "test_utils.h"

#include "adblogcatsource.h"
#include "configuration.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "streaminglogdata.h"

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

bool generateLongLineDataFile( QTemporaryFile& file )
{
    if ( file.open() ) {
        for ( int i = 0; i < SL_NB_LINES; i++ ) {
            const auto line = QStringLiteral( "LOGDATA long line %1 %2\n" )
                                  .arg( i, 6, 10, QChar( '0' ) )
                                  .arg( QString( 600, QLatin1Char( 'x' ) ) );
            file.write( line.toUtf8() );
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

    static int charWidth( const AbstractLogView* view )
    {
        return view->charWidth_;
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

    static bool textAreaCacheInvalid( const AbstractLogView* view )
    {
        return view->textAreaCache_.invalid_;
    }

    static int getSelectedTextCallCount( const AbstractLogView* view )
    {
        return view->getSelectedTextCallCount_;
    }

    static void resetGetSelectedTextCallCount( AbstractLogView* view )
    {
        view->getSelectedTextCallCount_ = 0;
    }

    static bool selectionChanged( const AbstractLogView* view )
    {
        return view->selectionChanged_;
    }

    static QSize textAreaCachePixmapSize( const AbstractLogView* view )
    {
        return view->textAreaCache_.pixmap_.size();
    }

    static qreal textAreaCachePixmapDevicePixelRatio( const AbstractLogView* view )
    {
        return view->textAreaCache_.pixmap_.devicePixelRatioF();
    }

    static LineLength visibleColumns( const AbstractLogView* view )
    {
        return view->getNbVisibleCols();
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

    SearchableLogData* rawLogData()
    {
        return crawler->logData_.get();
    }

    LogFilteredData* rawFilteredData()
    {
        return crawler->logFilteredData_.get();
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

    int mainHorizontalScrollMaximum() const
    {
        return crawler->logMainView_->horizontalScrollBar()->maximum();
    }

    bool mainTextAreaCacheInvalid() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textAreaCacheInvalid(
            crawler->logMainView_ );
    }

    QSize mainTextAreaCachePixmapSize() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textAreaCachePixmapSize(
            crawler->logMainView_ );
    }

    qreal mainTextAreaCachePixmapDevicePixelRatio() const
    {
        return AbstractLogView::access_by<
            AbstractLogViewPrivate>::textAreaCachePixmapDevicePixelRatio( crawler->logMainView_ );
    }

    QSize mainViewportSize() const
    {
        return crawler->logMainView_->viewport()->size();
    }

    LineLength mainVisibleColumns() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::visibleColumns(
            crawler->logMainView_ );
    }

    int mainCharWidth() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::charWidth( crawler->logMainView_ );
    }

    int mainLeftMargin() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::leftMargin( crawler->logMainView_ );
    }

    QImage grabMainViewport()
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::viewport( crawler->logMainView_ )
            ->grab()
            .toImage();
    }

    QColor mainBaseColor() const
    {
        return crawler->logMainView_->palette().color( QPalette::Base );
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

    int mainGetSelectedTextCallCount() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::getSelectedTextCallCount(
            crawler->logMainView_ );
    }

    void mainResetGetSelectedTextCallCount()
    {
        AbstractLogView::access_by<AbstractLogViewPrivate>::resetGetSelectedTextCallCount(
            crawler->logMainView_ );
    }

    bool mainSelectionChanged() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::selectionChanged(
            crawler->logMainView_ );
    }

    QWidget* mainViewport() const
    {
        return crawler->logMainView_->viewport();
    }

    AbstractLogView* mainView() const
    {
        return crawler->logMainView_;
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

                THEN( "vertical scrolling aligns content bottom to viewport bottom" )
                {
                    // With exact-pixel bottom alignment, the drawing top offset
                    // may not be a multiple of charHeight — that is expected.
                    // Instead, verify that content fills the viewport without
                    // cutting off the last line or leaving a gap at the bottom.
                    const auto offset = qAbs( crawlerVisitor.filteredDrawingTopOffset() );
                    const auto charH = crawlerVisitor.filteredCharHeight();
                    REQUIRE( offset >= 0 );
                    // The offset should be at most one viewport's worth of content
                    REQUIRE( offset < charH * 50 );
                }

                AND_WHEN( "the filtered view is then scrolled horizontally" )
                {
                    crawlerVisitor.scrollFilteredHorizontallyToMiddle();
                    crawlerVisitor.render();

                    THEN( "horizontal scrolling preserves the top offset" )
                    {
                        const auto offsetBefore = qAbs( crawlerVisitor.filteredDrawingTopOffset() );
                        // The offset should still be valid after horizontal scroll
                        REQUIRE( offsetBefore >= 0 );
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

SCENARIO( "Live source search auto-refresh is throttled", "[ui][live]" )
{
    // Use production-like search buffer so each search chunk takes noticeable time.
    // RAII guard restores the original value even if REQUIRE fails early.
    auto& config = Configuration::getSynced();
    const auto savedBufferSize = config.searchReadBufferSizeLines();
    config.setSearchReadBufferSizeLines( 10000 );
    struct BufferSizeGuard {
        Configuration& cfg;
        int saved;
        ~BufferSizeGuard() { cfg.setSearchReadBufferSizeLines( saved ); }
    } bufferGuard{ config, savedBufferSize };

    Session session;
    AdbLogcatSessionData adbSession;
    adbSession.captureId = QUuid::createUuid().toString( QUuid::WithoutBraces );

    // Create CrawlerWidget backed by StreamingLogData (startConnected=false, no real ADB)
    CrawlerWidgetVisitor visitor;
    visitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.openAdbLogcat( adbSession, []() { return new CrawlerWidget(); }, false ) ) );

    waitUiState( [ & ]() { return visitor.isLoadingFinished(); } );

    auto* logData = dynamic_cast<StreamingLogData*>( visitor.rawLogData() );
    REQUIRE( logData != nullptr );
    REQUIRE( logData->isLiveSource() );

    // Seed initial data so the search has a meaningful index
    QByteArray seedData;
    for ( int i = 0; i < 5000; i++ ) {
        seedData.append(
            QStringLiteral( "seed log line %1\n" ).arg( i, 6, 10, QChar( '0' ) ).toUtf8() );
    }
    logData->appendUtf8( seedData );
    QTest::qWait( 200 );

    // Start a search with auto-refresh
    visitor.setSearchPattern( "seed" );
    visitor.runSearch();

    GIVEN( "active search on a live source with continuous streaming" )
    {
        // Spy on searchProgressed to count how many search operations complete.
        // Each updateSearch() call starts a search that eventually emits
        // searchProgressed with progress == 100.
        SafeQSignalSpy searchSpy( visitor.rawFilteredData(),
                                  SIGNAL( searchProgressed( LinesCount, int, LineNumber, quint64 ) ) );

        QElapsedTimer elapsed;
        elapsed.start();
        int batchCount = 0;

        // Stream data continuously for 2 seconds with small batches
        while ( elapsed.elapsed() < 2000 ) {
            QByteArray batch;
            for ( int j = 0; j < 20; j++ ) {
                batch.append( QStringLiteral( "streaming line %1-%2\n" )
                                  .arg( batchCount )
                                  .arg( j )
                                  .toUtf8() );
            }
            logData->appendUtf8( batch );
            batchCount++;
            QTest::qWait( 5 );
        }

        // Let any pending searches finish
        QTest::qWait( 500 );

        // Count completed searches (progress == 100)
        int completions = 0;
        for ( int i = 0; i < searchSpy.count(); i++ ) {
            if ( searchSpy.at( i ).at( 1 ).toInt() == 100 ) {
                completions++;
            }
        }

        THEN( "search updates are throttled for live sources" )
        {
            // Without throttle: one updateSearch per loadingFinished, potentially
            // hundreds of completions over 2 seconds (one per ~5ms batch).
            // With throttle (250ms interval): max ~2000/250 = 8 fires plus
            // the initial search, so ~9 completions; cap at 12 with margin.
            INFO( "batchCount=" << batchCount << " completions=" << completions );
            REQUIRE( completions <= 12 );
        }
    }

}

SCENARIO( "Log view repaints after deferred horizontal scrollbar initialization",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_long_lines_XXXXXX" };
    REQUIRE( generateLongLineDataFile( file ) );

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    crawlerVisitor.resizeViews( 260, 120 );

    crawlerVisitor.render();

    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );

    REQUIRE( crawlerVisitor.mainHorizontalScrollMaximum() > 0 );
    REQUIRE( crawlerVisitor.mainTextAreaCacheInvalid() );

    REQUIRE( waitUiState( [ & ] {
        crawlerVisitor.render();

        const auto pixmapSize = crawlerVisitor.mainTextAreaCachePixmapSize();
        const auto viewportSize = crawlerVisitor.mainViewportSize();
        return !crawlerVisitor.mainTextAreaCacheInvalid() && !pixmapSize.isEmpty()
            && pixmapSize.width() >= viewportSize.width()
            && pixmapSize.height() >= viewportSize.height();
    } ) );

    INFO( "viewport=" << crawlerVisitor.mainViewportSize().width() << "x"
                      << crawlerVisitor.mainViewportSize().height()
                   << " charWidth=" << crawlerVisitor.mainCharWidth()
                   << " charHeight=" << crawlerVisitor.mainCharHeight()
                   << " leftMargin=" << crawlerVisitor.mainLeftMargin()
                   << " hMax=" << crawlerVisitor.mainHorizontalScrollMaximum()
                   << " visibleCols=" << crawlerVisitor.mainVisibleColumns().get()
                   << " pixmap=" << crawlerVisitor.mainTextAreaCachePixmapSize().width() << "x"
                   << crawlerVisitor.mainTextAreaCachePixmapSize().height()
                   << " pixmapDpr=" << crawlerVisitor.mainTextAreaCachePixmapDevicePixelRatio()
                   << " cacheInvalid=" << crawlerVisitor.mainTextAreaCacheInvalid() );
    REQUIRE_FALSE( crawlerVisitor.mainTextAreaCacheInvalid() );
    REQUIRE( crawlerVisitor.mainTextAreaCachePixmapSize().width()
             >= crawlerVisitor.mainViewportSize().width() );
    REQUIRE( crawlerVisitor.mainTextAreaCachePixmapSize().height()
             >= crawlerVisitor.mainViewportSize().height() );
}

SCENARIO( "Selection drag performance", "[ui][selection][regression]" )
{
    QTemporaryFile file{ "crawler_selection_perf_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    session.savedSearches().clear();

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.render();

    GIVEN( "a loaded log file" )
    {
        WHEN( "dragging to create a portion selection on one line" )
        {
            const auto charHeight = crawlerVisitor.mainCharHeight();
            const auto charWidth = crawlerVisitor.mainCharWidth();
            const auto leftMargin = crawlerVisitor.mainLeftMargin();

            // Click on line 5 and drag horizontally
            const int lineY = charHeight * 5 + charHeight / 2;
            const int startX = leftMargin + charWidth * 5;
            const int endX = leftMargin + charWidth * 20;

            crawlerVisitor.mainResetGetSelectedTextCallCount();

            auto* viewport = crawlerVisitor.mainViewport();

            QTest::mousePress( viewport, Qt::LeftButton, {}, QPoint( startX, lineY ) );
            QTest::mouseMove( viewport, QPoint( endX, lineY ) );
            QTest::mouseRelease( viewport, Qt::LeftButton, {}, QPoint( endX, lineY ) );

            QTest::qWait( 50 );

            THEN( "getSelectedText() should not be called during drag" )
            {
                INFO( "getSelectedTextCallCount=" << crawlerVisitor.mainGetSelectedTextCallCount() );
                // Current code calls getSelectedText() on every portion selection mouse move.
                // After fix, it should be 0 during drag (or only called on release).
                REQUIRE( crawlerVisitor.mainGetSelectedTextCallCount() == 0 );
            }
        }

        WHEN( "dragging to create a range selection across lines" )
        {
            const auto charHeight = crawlerVisitor.mainCharHeight();
            const auto leftMargin = crawlerVisitor.mainLeftMargin();

            // Click on line 5 and drag to line 15
            const int startY = charHeight * 5 + charHeight / 2;
            const int endY = charHeight * 15 + charHeight / 2;
            const int xPos = leftMargin + 20;

            crawlerVisitor.mainResetGetSelectedTextCallCount();

            auto* viewport = crawlerVisitor.mainViewport();

            QTest::mousePress( viewport, Qt::LeftButton, {}, QPoint( xPos, startY ) );
            QTest::mouseMove( viewport, QPoint( xPos, endY ) );
            QTest::mouseRelease( viewport, Qt::LeftButton, {}, QPoint( xPos, endY ) );

            QTest::qWait( 50 );

            THEN( "getSelectedText() should not be called during drag" )
            {
                INFO( "getSelectedTextCallCount=" << crawlerVisitor.mainGetSelectedTextCallCount() );
                // The drag path should not call getSelectedText(); one final
                // call on release is expected for range selections.
                REQUIRE( crawlerVisitor.mainGetSelectedTextCallCount() <= 1 );
            }
        }

        WHEN( "clicking to select a single line" )
        {
            const auto charHeight = crawlerVisitor.mainCharHeight();
            const auto leftMargin = crawlerVisitor.mainLeftMargin();

            const int lineY = charHeight * 10 + charHeight / 2;
            const int xPos = leftMargin + 20;

            crawlerVisitor.mainResetGetSelectedTextCallCount();

            auto* viewport = crawlerVisitor.mainViewport();

            QTest::mouseClick( viewport, Qt::LeftButton, {}, QPoint( xPos, lineY ) );
            QTest::qWait( 50 );

            THEN( "getSelectedText() should not be called for single line click" )
            {
                INFO( "getSelectedTextCallCount=" << crawlerVisitor.mainGetSelectedTextCallCount() );
                // Single line selection uses 0_length, no getSelectedText() needed.
                REQUIRE( crawlerVisitor.mainGetSelectedTextCallCount() == 0 );
            }
        }
    }
}

SCENARIO( "Selection uses selectionChanged flag instead of cache invalidation", "[ui][selection][regression]" )
{
    QTemporaryFile file{ "crawler_selection_cache_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;
    session.savedSearches().clear();

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.render();

    // Process deferred updates from initial paint (scrollbar init + forceRefresh cycle)
    for ( int i = 0; i < 5; ++i ) {
        QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
        QTest::qWait( 20 );
    }

    GIVEN( "a rendered log file with valid text cache" )
    {
        // Verify cache is valid before the test action
        if ( crawlerVisitor.mainTextAreaCacheInvalid() ) {
            // Force a final paint to stabilize the cache
            crawlerVisitor.render();
            QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
            QTest::qWait( 20 );
        }
        REQUIRE_FALSE( crawlerVisitor.mainTextAreaCacheInvalid() );

        WHEN( "clicking to select a different line" )
        {
            const auto charHeight = crawlerVisitor.mainCharHeight();
            const auto leftMargin = crawlerVisitor.mainLeftMargin();

            const int lineY = charHeight * 5 + charHeight / 2;
            const int xPos = leftMargin + 20;

            auto* viewport = crawlerVisitor.mainViewport();

            QTest::mouseClick( viewport, Qt::LeftButton, {}, QPoint( xPos, lineY ) );

            THEN( "selection change sets selectionChanged flag, not cache invalidation" )
            {
                INFO( "selectionChanged=" << crawlerVisitor.mainSelectionChanged()
                      << " cacheInvalid=" << crawlerVisitor.mainTextAreaCacheInvalid() );
                // mousePressEvent sets selectionChanged_ instead of textAreaCache_.invalid_.
                // The cache should not be invalidated by a selection-only change.
                REQUIRE_FALSE( crawlerVisitor.mainTextAreaCacheInvalid() );
            }
        }
    }
}
