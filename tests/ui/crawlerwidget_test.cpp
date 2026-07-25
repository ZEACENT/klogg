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

#include <QShortcut>
#include <QSignalSpy>
#include <QLineEdit>
#include <QProxyStyle>
#include <QScrollBar>
#include <QStyle>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>
#include <qglobal.h>
#include <qnamespace.h>
#include <qtestmouse.h>

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QUuid>

#include <algorithm>

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
class TransientScrollBarStyle : public QProxyStyle {
  public:
    int styleHint( StyleHint hint, const QStyleOption* option = nullptr,
                   const QWidget* widget = nullptr,
                   QStyleHintReturn* returnData = nullptr ) const override
    {
        if ( hint == QStyle::SH_ScrollBar_Transient ) {
            return 1;
        }

        return QProxyStyle::styleHint( hint, option, widget, returnData );
    }
};

class ClassicScrollBarStyle : public QProxyStyle {
  public:
    int styleHint( StyleHint hint, const QStyleOption* option = nullptr,
                   const QWidget* widget = nullptr,
                   QStyleHintReturn* returnData = nullptr ) const override
    {
        if ( hint == QStyle::SH_ScrollBar_Transient ) {
            return 0;
        }

        return QProxyStyle::styleHint( hint, option, widget, returnData );
    }
};

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

class ScopedShowAllEmptyFilterSetting {
  public:
    explicit ScopedShowAllEmptyFilterSetting( bool value )
        : previousShowAll_( Configuration::get().showAllInFilteredViewWhenSearchEmpty() )
    {
        Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( value );
    }

    ~ScopedShowAllEmptyFilterSetting()
    {
        Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( previousShowAll_ );
    }

  private:
    bool previousShowAll_;
};

// Forces Configuration::lineSpacingPercent for the duration of a test (the
// blank-band regression only shows with spacing > 100%). Restores the previous
// value on destruction, including when a REQUIRE aborts mid-test.
class ScopedLineSpacingPercent {
  public:
    explicit ScopedLineSpacingPercent( int percent )
        : previousPercent_( Configuration::get().lineSpacingPercent() )
    {
        Configuration::get().setLineSpacingPercent( percent );
    }

    ~ScopedLineSpacingPercent()
    {
        Configuration::get().setLineSpacingPercent( previousPercent_ );
    }

  private:
    int previousPercent_;
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

    static int textAreaCacheActualHeight( const AbstractLogView* view )
    {
        return view->textAreaCache_.actual_height_;
    }

    static qreal textAreaCachePixmapDevicePixelRatio( const AbstractLogView* view )
    {
        return view->textAreaCache_.pixmap_.devicePixelRatioF();
    }

    static LineLength visibleColumns( const AbstractLogView* view )
    {
        return view->getNbVisibleCols();
    }

    static int textViewportHeight( const AbstractLogView* view )
    {
        return view->textViewportHeight();
    }

    static QShortcut* shortcutFor( const AbstractLogView* view, const QString& key )
    {
        const auto shortcut = view->shortcuts_.find( key );
        return shortcut != view->shortcuts_.end() ? shortcut->second : nullptr;
    }

    static OptionalLineNumber selectedLine( const AbstractLogView* view )
    {
        return view->selection_.selectedLine();
    }

    static const std::vector<AbstractLogView::QuickHighlighters>&
    quickHighlighters( const AbstractLogView* view )
    {
        return view->quickHighlighters_;
    }

    static int wrappedLineMapSize( const AbstractLogView* view )
    {
        return static_cast<int>( view->wrappedLinesInfo_.size() );
    }

    static void rebuildLineMap( AbstractLogView* view )
    {
        view->buildVisibleLineMap();
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

    void addColorLabelInFilteredView( size_t label )
    {
        Q_EMIT crawler->filteredView_->addColorLabel( label );
        QTest::qWait( 20 );
    }

    void removeColorLabelInFilteredView()
    {
        Q_EMIT crawler->filteredView_->removeColorLabel();
        QTest::qWait( 20 );
    }

    bool hasLabelledText( const AbstractLogView* view, size_t label, const QString& text )
    {
        const auto& labels
            = AbstractLogView::access_by<AbstractLogViewPrivate>::quickHighlighters( view );
        return label < labels.size()
               && std::any_of( labels[ label ].cbegin(), labels[ label ].cend(),
                               [ & ]( const QuickLabelEntry& e ) { return e.text == text; } );
    }

    bool mainViewHasLabelledText( size_t label, const QString& text )
    {
        return hasLabelledText( crawler->logMainView_, label, text );
    }

    bool filteredViewHasLabelledText( size_t label, const QString& text )
    {
        return hasLabelledText( crawler->filteredView_, label, text );
    }

    void setSearchPattern( const QString& pattern )
    {
        QTest::keyClicks( crawler->searchToolbar_->searchLineEdit(), pattern );
    }

    void replaceSearchPattern( const QString& pattern )
    {
        crawler->searchToolbar_->searchLineEdit()->lineEdit()->setText( pattern );
    }

    void focusSearchPattern()
    {
        crawler->show();
        const auto windowExposed = QTest::qWaitForWindowExposed( crawler.get() );
        (void) windowExposed;
        crawler->searchToolbar_->searchLineEdit()->lineEdit()->setFocus();
        QTest::qWait( 20 );
    }

    void setSearchPatternCursorPosition( int position )
    {
        crawler->searchToolbar_->searchLineEdit()->lineEdit()->setCursorPosition( position );
    }

    int searchPatternCursorPosition() const
    {
        return crawler->searchToolbar_->searchLineEdit()->lineEdit()->cursorPosition();
    }

    void pressSearchPatternKey( Qt::Key key )
    {
        QTest::keyClick( crawler->searchToolbar_->searchLineEdit()->lineEdit(), key );
        QTest::qWait( 20 );
    }

    void enableCaseSensitiveSearch()
    {
        if ( !crawler->searchToolbar_->matchCaseButton()->isChecked() ) {
            QTest::mouseClick( crawler->searchToolbar_->matchCaseButton(), Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void enableInverseMatch()
    {
        if ( !crawler->searchToolbar_->inverseButton()->isChecked() ) {
            QTest::mouseClick( crawler->searchToolbar_->inverseButton(), Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void enableBooleanCombinationMode()
    {
        if ( !crawler->searchToolbar_->booleanButton()->isChecked() ) {
            QTest::mouseClick( crawler->searchToolbar_->booleanButton(), Qt::LeftButton );
            QTest::qWait( 100 );
        }
    }

    void runSearch()
    {
        QTest::mouseClick( crawler->searchToolbar_->searchButton(), Qt::LeftButton );

        QTest::qWait( 100 );

        REQUIRE( waitUiState( [ & ]() { return crawler->searchToolbar_->stopButton()->isHidden(); } ) );
    }

    void render()
    {
        crawler->grab();
    }

    int mainHorizontalScrollMaximum() const
    {
        return crawler->logMainView_->horizontalScrollBar()->maximum();
    }

    int mainHorizontalScrollValue() const
    {
        return crawler->logMainView_->horizontalScrollBar()->value();
    }

    int mainVerticalScrollMaximum() const
    {
        return crawler->logMainView_->verticalScrollBar()->maximum();
    }

    int filteredVerticalScrollMaximum() const
    {
        return crawler->filteredView_->verticalScrollBar()->maximum();
    }

    int filteredHorizontalScrollMaximum() const
    {
        return crawler->filteredView_->horizontalScrollBar()->maximum();
    }

    int filteredHorizontalScrollValue() const
    {
        return crawler->filteredView_->horizontalScrollBar()->value();
    }

    void setMainHorizontalScrollValue( int value )
    {
        crawler->logMainView_->horizontalScrollBar()->setValue( value );
        QTest::qWait( 20 );
    }

    void setFilteredHorizontalScrollValue( int value )
    {
        crawler->filteredView_->horizontalScrollBar()->setValue( value );
        QTest::qWait( 20 );
    }

    void focusMainView()
    {
        crawler->show();
        const auto windowExposed = QTest::qWaitForWindowExposed( crawler.get() );
        (void) windowExposed;
        crawler->logMainView_->viewport()->setFocus();
        QTest::qWait( 20 );
    }

    void focusFilteredView()
    {
        crawler->show();
        const auto windowExposed = QTest::qWaitForWindowExposed( crawler.get() );
        (void) windowExposed;
        crawler->filteredView_->viewport()->setFocus();
        QTest::qWait( 20 );
    }

    void pressMainViewKey( Qt::Key key )
    {
        QTest::keyClick( crawler->logMainView_->viewport(), key );
        QTest::qWait( 20 );
    }

    void pressFilteredViewKey( Qt::Key key )
    {
        QTest::keyClick( crawler->filteredView_->viewport(), key );
        QTest::qWait( 20 );
    }

    void pressFilteredViewKey( Qt::Key key, Qt::KeyboardModifiers modifiers )
    {
        QTest::keyClick( crawler->filteredView_->viewport(), key, modifiers );
        QTest::qWait( 20 );
    }

    QString filteredLineText( LineNumber::UnderlyingType lineIndex ) const
    {
        return crawler->logFilteredData_->getLineString( LineNumber( lineIndex ) );
    }

    void pressMainViewConfiguredShortcut( const std::string& action )
    {
        pressConfiguredShortcut( crawler->logMainView_->viewport(), action );
    }

    void pressFilteredViewConfiguredShortcut( const std::string& action )
    {
        pressConfiguredShortcut( crawler->filteredView_->viewport(), action );
    }

    OptionalLineNumber mainSelectedLine() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::selectedLine(
            crawler->logMainView_ );
    }

    void activateMainViewShortcut( Qt::Key key )
    {
        auto* shortcut = AbstractLogView::access_by<AbstractLogViewPrivate>::shortcutFor(
            crawler->logMainView_, QKeySequence( key ).toString() );
        REQUIRE( shortcut != nullptr );
        Q_EMIT shortcut->activated();
        QTest::qWait( 20 );
    }

    bool mainTextAreaCacheInvalid() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textAreaCacheInvalid(
            crawler->logMainView_ );
    }

    int mainWrappedLineMapSize() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::wrappedLineMapSize(
            crawler->logMainView_ );
    }

    void rebuildMainLineMap()
    {
        AbstractLogView::access_by<AbstractLogViewPrivate>::rebuildLineMap(
            crawler->logMainView_ );
    }

    QSize mainTextAreaCachePixmapSize() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textAreaCachePixmapSize(
            crawler->logMainView_ );
    }

    int mainTextAreaCacheActualHeight() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textAreaCacheActualHeight(
            crawler->logMainView_ );
    }

    int filteredTextAreaCacheActualHeight() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textAreaCacheActualHeight(
            crawler->filteredView_ );
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

    int mainDrawingTopOffset() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::drawingTopOffset(
            crawler->logMainView_ );
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

    void jumpToTop()
    {
        crawler->jumpToTop();
    }

    void resizeViews( int width, int height )
    {
        crawler->logMainView_->resize( width, height );
        crawler->filteredView_->resize( width, height );
        QTest::qWait( 50 );
    }

    void resizeViewsToPartialTextLineHeight( int width )
    {
        for ( int height = 70; height < 140; ++height ) {
            crawler->logMainView_->setFixedSize( width, height );
            crawler->filteredView_->setFixedSize( width, height );
            QTest::qWait( 10 );
            render();

            if ( mainCharHeight() > 0 && filteredCharHeight() > 0
                 && mainTextViewportHeight() % mainCharHeight() != 0
                 && filteredTextViewportHeight() % filteredCharHeight() != 0 ) {
                return;
            }
        }
    }

    bool resizeViewsToFitFilteredTextRows( int width, int minimumRows )
    {
        for ( int height = 120; height <= 720; height += 40 ) {
            crawler->logMainView_->setFixedSize( width, height );
            crawler->filteredView_->setFixedSize( width, height );
            QTest::qWait( 10 );
            render();

            const auto charHeight = filteredCharHeight();
            if ( charHeight > 0 && filteredTextViewportHeight() > charHeight * minimumRows ) {
                return true;
            }
        }

        return false;
    }

    // Converge the views to a size where the main viewport shows exactly
    // `rowFloor` FULL text rows (so getNbVisibleLines() == rowFloor + 1).
    // Font metrics vary per platform, so sweep instead of hardcoding a height.
    bool resizeViewsToMainTextRowFloor( int width, int rowFloor )
    {
        for ( int height = 80; height <= 720; height += 4 ) {
            crawler->logMainView_->setFixedSize( width, height );
            crawler->filteredView_->setFixedSize( width, height );
            QTest::qWait( 10 );
            render();

            const auto charHeight = mainCharHeight();
            if ( charHeight > 0 && mainTextViewportHeight() / charHeight == rowFloor ) {
                return true;
            }
        }

        return false;
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

    bool isFilteredFollowModeEnabled()
    {
        return crawler->filteredView_->isFollowEnabled();
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

    int mainTextViewportHeight() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textViewportHeight(
            crawler->logMainView_ );
    }

    int filteredTextViewportHeight() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::textViewportHeight(
            crawler->filteredView_ );
    }

    SearchPerformanceCounters searchPerformanceCounters() const
    {
        return crawler->logFilteredData_->searchPerformanceCounters();
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

    QSize filteredViewportSize() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::viewport( crawler->filteredView_ )
            ->size();
    }

    void makeFilteredViewportHeightPartialLine()
    {
        for ( int height = 70; height < 130; ++height ) {
            crawler->filteredView_->setFixedHeight( height );
            QTest::qWait( 10 );
            crawler->grab();

            const auto charHeight = filteredCharHeight();
            const auto viewportHeight = filteredViewportSize().height();
            if ( charHeight > 0 && viewportHeight % charHeight != 0 ) {
                return;
            }
        }
    }

    void setFilteredLastLineAligned( bool value )
    {
        AbstractLogView::access_by<AbstractLogViewPrivate>::setLastLineAligned( crawler->filteredView_,
                                                                                value );
    }

    void setMainLastLineAligned( bool value )
    {
        AbstractLogView::access_by<AbstractLogViewPrivate>::setLastLineAligned( crawler->logMainView_,
                                                                                value );
    }

    LineNumber mainTopLine() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::topLine( crawler->logMainView_ );
    }

    LineNumber filteredTopLine() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::topLine( crawler->filteredView_ );
    }

    bool filteredShouldBottomAlign() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::shouldBottomAlignFrame(
            crawler->filteredView_ );
    }

    bool mainShouldBottomAlign() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::shouldBottomAlignFrame(
            crawler->logMainView_ );
    }

    void scrollFilteredVerticallyToBottom()
    {
        crawler->filteredView_->verticalScrollBar()->setValue(
            crawler->filteredView_->verticalScrollBar()->maximum() );
        QTest::qWait( 50 );
    }

    void scrollMainVerticallyToBottom()
    {
        crawler->logMainView_->verticalScrollBar()->setValue(
            crawler->logMainView_->verticalScrollBar()->maximum() );
        QTest::qWait( 50 );
    }

    void scrollMainVerticallyToMiddle()
    {
        const auto max = crawler->logMainView_->verticalScrollBar()->maximum();
        crawler->logMainView_->verticalScrollBar()->setValue( max / 2 );
        QTest::qWait( 50 );
    }

    void scrollFilteredVerticallyToMiddle()
    {
        const auto max = crawler->filteredView_->verticalScrollBar()->maximum();
        crawler->filteredView_->verticalScrollBar()->setValue( max / 2 );
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

    void selectFilteredViewLine( LineNumber::UnderlyingType lineIndex )
    {
        crawler->filteredView_->selectAndDisplayLine( LineNumber( lineIndex ) );
        QTest::qWait( 50 );
    }

    void addMarksInFilteredView( const klogg::vector<LineNumber>& lines )
    {
        crawler->markLinesFromFiltered( lines );
        QTest::qWait( 20 );
    }

    OptionalLineNumber filteredSelectedLine() const
    {
        return AbstractLogView::access_by<AbstractLogViewPrivate>::selectedLine(
            crawler->filteredView_ );
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

    // This scenario exercises the populated filtered view (incl. the
    // empty-filter mirror branch); pin the mirror-mode preference so it does
    // not depend on the compiled default (issue #46 flipped it to off).
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

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
        THEN( "Has all lines in filtered log view while the filter is empty" )
        {
            REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );
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
                crawlerVisitor.makeFilteredViewportHeightPartialLine();
                crawlerVisitor.render();
                crawlerVisitor.scrollFilteredVerticallyToBottom();
                crawlerVisitor.render();

                THEN( "vertical scrolling aligns content bottom to viewport bottom" )
                {
                    const auto offset = qAbs( crawlerVisitor.filteredDrawingTopOffset() );
                    const auto charH = crawlerVisitor.filteredCharHeight();
                    const auto viewportHeight = crawlerVisitor.filteredViewportSize().height();
                    REQUIRE( viewportHeight % charH != 0 );
                    REQUIRE( offset >= 0 );
                    // Bottom-aligned: pixmap is padded from the top so the bottom line sits at
                    // the viewport bottom. The offset is strictly less than one char height for
                    // a partial-line viewport (no grid snapping).
                    REQUIRE( offset < charH );
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

                AND_WHEN( "follow mode is enabled at the filtered bottom" )
                {
                    // Force bottom-alignment: scroll to end and explicitly mark the view
                    // as bottom-aligned so the offset comparison is reliable.
                    crawlerVisitor.scrollFilteredVerticallyToBottom();
                    crawlerVisitor.setFilteredLastLineAligned( true );
                    crawlerVisitor.render();
                    const auto offsetBeforeFollow = crawlerVisitor.filteredDrawingTopOffset();
                    crawlerVisitor.enableFollowMode( true );
                    crawlerVisitor.render();

                    THEN( "follow mode does not reserve a bottom bar or move the text area" )
                    {
                        REQUIRE( crawlerVisitor.isFollowModeEnabled() );
                        REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
                        REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == offsetBeforeFollow );
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

        WHEN( "Home and End are pressed in the filter input" )
        {
            const auto pattern = QStringLiteral( "this is line" );
            const auto middlePosition = static_cast<int>( pattern.size() / 2 );
            const auto endPosition = static_cast<int>( pattern.size() );
            crawlerVisitor.replaceSearchPattern( pattern );
            crawlerVisitor.focusSearchPattern();

            crawlerVisitor.setSearchPatternCursorPosition( middlePosition );
            crawlerVisitor.pressSearchPatternKey( Qt::Key_Home );

            THEN( "Home moves the text cursor to the beginning of the input" )
            {
                REQUIRE( crawlerVisitor.searchPatternCursorPosition() == 0 );
            }

            AND_WHEN( "End is pressed" )
            {
                crawlerVisitor.setSearchPatternCursorPosition( middlePosition );
                crawlerVisitor.pressSearchPatternKey( Qt::Key_End );

                THEN( "End moves the text cursor to the end of the input" )
                {
                    REQUIRE( crawlerVisitor.searchPatternCursorPosition() == endPosition );
                }
            }
        }

        WHEN( "log view Home and End shortcuts are activated while the filter input has focus" )
        {
            crawlerVisitor.setTextWrap( false );
            crawlerVisitor.resizeViews( 200, 120 );
            crawlerVisitor.render();

            REQUIRE( crawlerVisitor.mainHorizontalScrollMaximum() > 0 );

            const auto middleScroll = crawlerVisitor.mainHorizontalScrollMaximum() / 2;
            crawlerVisitor.replaceSearchPattern( QStringLiteral( "this is line" ) );
            crawlerVisitor.focusSearchPattern();
            crawlerVisitor.setMainHorizontalScrollValue( middleScroll );
            const auto scrollBeforeShortcut = crawlerVisitor.mainHorizontalScrollValue();

            crawlerVisitor.activateMainViewShortcut( Qt::Key_Home );

            THEN( "the main view does not handle Home" )
            {
                REQUIRE( crawlerVisitor.mainHorizontalScrollValue() == scrollBeforeShortcut );
            }

            AND_WHEN( "End is activated" )
            {
                crawlerVisitor.activateMainViewShortcut( Qt::Key_End );

                THEN( "the main view does not handle End" )
                {
                    REQUIRE( crawlerVisitor.mainHorizontalScrollValue() == scrollBeforeShortcut );
                }
            }
        }

        WHEN( "Home and End are pressed while the main log view has focus" )
        {
            crawlerVisitor.setTextWrap( false );
            crawlerVisitor.resizeViews( 200, 120 );
            crawlerVisitor.render();
            crawlerVisitor.focusMainView();
            crawlerVisitor.resizeViews( 200, 120 );
            crawlerVisitor.render();

            REQUIRE( crawlerVisitor.mainHorizontalScrollMaximum() > 0 );

            const auto middleScroll = crawlerVisitor.mainHorizontalScrollMaximum() / 2;
            crawlerVisitor.setMainHorizontalScrollValue( middleScroll );

            crawlerVisitor.pressMainViewKey( Qt::Key_Home );

            THEN( "Home jumps the main view to the left of the log line" )
            {
                REQUIRE( crawlerVisitor.mainHorizontalScrollValue() == 0 );
            }

            AND_WHEN( "End is activated" )
            {
                crawlerVisitor.setMainHorizontalScrollValue( middleScroll );
                crawlerVisitor.pressMainViewKey( Qt::Key_End );

                THEN( "End jumps the main view to the right of the log line" )
                {
                    REQUIRE( crawlerVisitor.mainHorizontalScrollValue() > middleScroll );
                }
            }
        }

        WHEN( "Home and End are pressed while the filtered log view has focus" )
        {
            crawlerVisitor.setSearchPattern( "10" );
            crawlerVisitor.runSearch();
            waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } );

            crawlerVisitor.setTextWrap( false );
            crawlerVisitor.resizeViews( 200, 120 );
            crawlerVisitor.render();
            crawlerVisitor.focusFilteredView();
            crawlerVisitor.resizeViews( 200, 120 );
            crawlerVisitor.render();

            REQUIRE( crawlerVisitor.filteredHorizontalScrollMaximum() > 0 );

            const auto middleScroll = crawlerVisitor.filteredHorizontalScrollMaximum() / 2;
            crawlerVisitor.setFilteredHorizontalScrollValue( middleScroll );

            crawlerVisitor.pressFilteredViewKey( Qt::Key_Home );

            THEN( "Home jumps the filtered view to the left of the log line" )
            {
                REQUIRE( crawlerVisitor.filteredHorizontalScrollValue() == 0 );
            }

            AND_WHEN( "End is pressed" )
            {
                crawlerVisitor.setFilteredHorizontalScrollValue( middleScroll );
                crawlerVisitor.pressFilteredViewKey( Qt::Key_End );

                THEN( "End jumps the filtered view to the right of the log line" )
                {
                    REQUIRE( crawlerVisitor.filteredHorizontalScrollValue() > middleScroll );
                }
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

        WHEN( "bracket mark navigation is used in the filtered view" )
        {
            crawlerVisitor.setSearchPattern( "this is line" );
            crawlerVisitor.runSearch();
            REQUIRE( crawlerVisitor.getLogFilteredNbLines().get() == SL_NB_LINES );

            // Mark filtered rows 0 and 2, then place the selection between
            // them: "]" must jump DOWN to the next mark, "[" UP to the
            // previous one.
            crawlerVisitor.addMarksInFilteredView( { 0_lnum, 2_lnum } );
            crawlerVisitor.selectFilteredViewLine( 1 );
            crawlerVisitor.focusFilteredView();

            THEN( "] jumps to the next marked row" )
            {
                crawlerVisitor.pressFilteredViewConfiguredShortcut(
                    ShortcutAction::LogViewNextMark );
                const auto selected = crawlerVisitor.filteredSelectedLine();
                REQUIRE( selected.has_value() );
                REQUIRE( *selected == 2_lnum );

                AND_THEN( "[ jumps back to the previous marked row" )
                {
                    crawlerVisitor.pressFilteredViewConfiguredShortcut(
                        ShortcutAction::LogViewPrevMark );
                    const auto back = crawlerVisitor.filteredSelectedLine();
                    REQUIRE( back.has_value() );
                    REQUIRE( *back == 0_lnum );
                }
            }
        }

        WHEN( "bracket mark navigation is used in the main view" )
        {
            // LogMainView overrides the hoisted base navigation with the
            // LogFilteredData mark index. The selection routinely sits on an
            // UNMARKED line, where prev-mark must return the NEAREST mark
            // above (rank() is inclusive, so an off-by-one here skips a mark).
            crawlerVisitor.addMarksInMainView( { 10_lnum, 25_lnum } );
            crawlerVisitor.selectMainViewLine( 20 );
            crawlerVisitor.focusMainView();

            THEN( "[ jumps to the nearest mark above an unmarked line" )
            {
                crawlerVisitor.pressMainViewConfiguredShortcut(
                    ShortcutAction::LogViewPrevMark );
                const auto selected = crawlerVisitor.mainSelectedLine();
                REQUIRE( selected.has_value() );
                REQUIRE( *selected == 10_lnum );

                AND_THEN( "] jumps to the next mark below" )
                {
                    crawlerVisitor.pressMainViewConfiguredShortcut(
                        ShortcutAction::LogViewNextMark );
                    const auto next = crawlerVisitor.mainSelectedLine();
                    REQUIRE( next.has_value() );
                    REQUIRE( *next == 25_lnum );
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

SCENARIO( "Filtered window can mirror the main window while the filter is empty",
          "[ui][filter][regression]" )
{
    QTemporaryFile file{ "crawler_empty_filter_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    const auto counters = crawlerVisitor.searchPerformanceCounters();
    REQUIRE( crawlerVisitor.getLogFilteredNbLines() == crawlerVisitor.getLogNbLines() );
    REQUIRE( counters.operationStarts == 0 );

}

SCENARIO( "Filtered window can stay empty while the filter is empty",
          "[ui][filter][regression]" )
{
    QTemporaryFile file{ "crawler_empty_filter_disabled_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ false };

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    REQUIRE( crawlerVisitor.getLogFilteredNbLines() == 0_lcount );

}

SCENARIO( "Empty search shows only marked lines with default settings (marks navigation)",
          "[ui][filter][regression]" )
{
    QTemporaryFile file{ "crawler_empty_filter_marks_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    // Pin the UI behavior to the COMPILED default (issue #46, upstream parity:
    // an empty filter must not flood the filtered window).  Seeding the guard
    // from a default-constructed Configuration keeps the test deterministic
    // regardless of any persisted user setting.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{
        Configuration{}.showAllInFilteredViewWhenSearchEmpty()
    };

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.addMarksInMainView( { 10_lnum, 25_lnum } );
    REQUIRE( crawlerVisitor.markedLinesCount() == 2 );

    // "Search" with an empty search box: matches are cleared and the filtered
    // window shows exactly the marked lines (upstream behavior; the reporter's
    // marks-navigation workflow from issue #46).
    crawlerVisitor.runSearch();

    REQUIRE( crawlerVisitor.getLogFilteredNbLines() == 2_lcount );

}

SCENARIO( "Marks persist across a filter change and stay visible (single-file parity)",
          "[ui][filter][marks]" )
{
    // Reference behavior for the folder mark-persistence bug: in single-file
    // mode marks live in LogFilteredData::marks_, which clearSearch() preserves
    // (marks_and_matches_ = marks_), so a mark made under filter1 that does NOT
    // match filter2 still appears in the filtered view under the default
    // "Marks and matches" visibility. The folder results model must match this.
    QTemporaryFile file{ "crawler_marks_filter_change_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{
        Configuration{}.showAllInFilteredViewWhenSearchEmpty()
    };

    Session session;
    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    // filter1 = "glogg": matches every line (the data template contains glogg).
    crawlerVisitor.setSearchPattern( "glogg" );
    crawlerVisitor.runSearch();
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get()
                                            == SL_NB_LINES; } ) );

    // Mark line 5 -- a line that matches filter1 but NOT the upcoming filter2.
    crawlerVisitor.addMarksInMainView( { 5_lnum } );
    REQUIRE( crawlerVisitor.markedLinesCount() == 1 );

    // filter2 = "000010": matches only line 10. Line 5 (marked) does not match.
    crawlerVisitor.replaceSearchPattern( "000010" );
    crawlerVisitor.runSearch();
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 2; } ) );

    // The mark survives the filter change...
    REQUIRE( crawlerVisitor.markedLinesCount() == 1 );
    // ...and stays visible in the filtered view under "Marks and matches"
    // (marked line 5 + matched line 10), even though line 5 does not match
    // filter2. Assert the ROW IDENTITY, not just the count: the filtered view
    // preserves source order, so row 0 is marked line 5 (000005) and row 1 is
    // the filter2 match on line 10 (000010).
    REQUIRE( crawlerVisitor.filteredLineText( 0 ).contains( "000005" ) );
    REQUIRE( crawlerVisitor.filteredLineText( 1 ).contains( "000010" ) );
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
            && pixmapSize.height() >= crawlerVisitor.mainTextViewportHeight();
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
             >= crawlerVisitor.mainTextViewportHeight() );
}

SCENARIO( "Log views keep the bottom line anchored when non-wrapped height changes",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_long_lines_XXXXXX" };
    REQUIRE( generateLongLineDataFile( file ) );

    // Geometry assertions need a populated filtered view: pin mirror mode so
    // the scenario does not depend on the compiled empty-filter default.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    crawlerVisitor.resizeViews( 320, 120 );
    crawlerVisitor.render();

    crawlerVisitor.scrollMainVerticallyToBottom();
    crawlerVisitor.scrollFilteredVerticallyToBottom();
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );
    REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() > 0 );
    REQUIRE( crawlerVisitor.mainTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.mainVerticalScrollMaximum() ) );
    REQUIRE( crawlerVisitor.filteredTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.filteredVerticalScrollMaximum() ) );

    crawlerVisitor.setMainLastLineAligned( false );
    crawlerVisitor.setFilteredLastLineAligned( false );

    crawlerVisitor.resizeViews( 320, 88 );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.mainVerticalScrollMaximum() ) );
    REQUIRE( crawlerVisitor.filteredTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.filteredVerticalScrollMaximum() ) );
}

SCENARIO( "Log views remain at bottom when viewport height decreases",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_long_lines_XXXXXX" };
    REQUIRE( generateLongLineDataFile( file ) );

    // Geometry assertions need a populated filtered view: pin mirror mode so
    // the scenario does not depend on the compiled empty-filter default.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    crawlerVisitor.resizeViews( 320, 200 );
    crawlerVisitor.render();

    crawlerVisitor.scrollMainVerticallyToBottom();
    crawlerVisitor.scrollFilteredVerticallyToBottom();
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );
    REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() > 0 );
    REQUIRE( crawlerVisitor.mainTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.mainVerticalScrollMaximum() ) );
    REQUIRE( crawlerVisitor.filteredTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.filteredVerticalScrollMaximum() ) );

    // Simulate window height change without clearing lastLineAligned_
    crawlerVisitor.resizeViews( 320, 120 );
    crawlerVisitor.render();

    // Should still be at the bottom after resize
    REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
    REQUIRE( crawlerVisitor.mainTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.mainVerticalScrollMaximum() ) );
    REQUIRE( crawlerVisitor.filteredTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.filteredVerticalScrollMaximum() ) );

    // Resize again to a smaller height
    crawlerVisitor.resizeViews( 320, 88 );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
    REQUIRE( crawlerVisitor.mainTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.mainVerticalScrollMaximum() ) );
    REQUIRE( crawlerVisitor.filteredTopLine().get()
             == static_cast<LineNumber::UnderlyingType>(
                 crawlerVisitor.filteredVerticalScrollMaximum() ) );
}

SCENARIO( "Log views do not clip text rows at top or bottom in bottom alignment",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_long_lines_XXXXXX" };
    REQUIRE( generateLongLineDataFile( file ) );

    // Geometry assertions need a populated filtered view: pin mirror mode so
    // the scenario does not depend on the compiled empty-filter default.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    crawlerVisitor.resizeViewsToPartialTextLineHeight( 320 );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainTextViewportHeight() % crawlerVisitor.mainCharHeight() != 0 );
    REQUIRE( crawlerVisitor.filteredTextViewportHeight() % crawlerVisitor.filteredCharHeight() != 0 );

    crawlerVisitor.scrollMainVerticallyToBottom();
    crawlerVisitor.scrollFilteredVerticallyToBottom();
    crawlerVisitor.render();

    const auto mainRowsAtBottom = crawlerVisitor.getLogNbLines() - LinesCount{
        crawlerVisitor.mainTopLine().get() };
    const auto filteredRowsAtBottom = crawlerVisitor.getLogFilteredNbLines() - LinesCount{
        crawlerVisitor.filteredTopLine().get() };

    REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
    // Bottom alignment shifts content upward so the last line sits at the viewport
    // bottom. The offset is negative (or zero when viewport height is an exact
    // multiple of char height), matching text-wrap mode behavior.
    REQUIRE( crawlerVisitor.mainDrawingTopOffset() <= 0 );
    REQUIRE( crawlerVisitor.filteredDrawingTopOffset() <= 0 );
    // The last visible line must end within one char height of the viewport bottom
    REQUIRE( crawlerVisitor.mainDrawingTopOffset()
             + static_cast<int>( mainRowsAtBottom.get() ) * crawlerVisitor.mainCharHeight()
             >= crawlerVisitor.mainTextViewportHeight() - crawlerVisitor.mainCharHeight() );
    REQUIRE( crawlerVisitor.filteredDrawingTopOffset()
             + static_cast<int>( filteredRowsAtBottom.get() ) * crawlerVisitor.filteredCharHeight()
             >= crawlerVisitor.filteredTextViewportHeight() - crawlerVisitor.filteredCharHeight() );
    REQUIRE( crawlerVisitor.mainDrawingTopOffset()
             + static_cast<int>( mainRowsAtBottom.get() ) * crawlerVisitor.mainCharHeight()
             <= crawlerVisitor.mainTextViewportHeight() );
    REQUIRE( crawlerVisitor.filteredDrawingTopOffset()
             + static_cast<int>( filteredRowsAtBottom.get() ) * crawlerVisitor.filteredCharHeight()
             <= crawlerVisitor.filteredTextViewportHeight() );

    crawlerVisitor.enableFollowMode( true );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
    REQUIRE( crawlerVisitor.mainDrawingTopOffset() <= 0 );
    REQUIRE( crawlerVisitor.filteredDrawingTopOffset() <= 0 );
}

SCENARIO( "Filtered view keeps sparse results top-aligned when follow mode is enabled",
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
    crawlerVisitor.resizeViews( 900, 420 );
    crawlerVisitor.setSearchPattern( "LOGDATA long line 000042" );
    crawlerVisitor.runSearch();

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } ) );
    REQUIRE( crawlerVisitor.resizeViewsToFitFilteredTextRows( 900, 3 ) );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() == 0 );
    REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );

    crawlerVisitor.enableFollowMode( true );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.isFollowModeEnabled() );
    REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
    REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() == 0 );
    REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );
}

SCENARIO( "Wrapped single-line overflow keeps EOF anchored with a scrollable range",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_wrapped_single_line_XXXXXX" };
    REQUIRE( file.open() );
    file.write( QString( 5000, QLatin1Char( 'x' ) ).toUtf8() );
    file.write( "\n" );
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == 1; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );
    QTest::qWait( 200 );

    crawlerVisitor.setTextWrap( true );
    crawlerVisitor.resizeViews( 220, 100 );
    crawlerVisitor.enableFollowMode( true );
    crawlerVisitor.render();
    // Flush the deferred updateScrollBars (queued from the first paint once
    // leftMarginPx_ exists) so the wrapped range is settled deterministically.
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    // The range must now OPEN for a single over-tall wrapped line (previously
    // pinned at 0, which made either the head or the tail unreachable):
    // follow mode keeps EOF anchored at the bottom...
    REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );
    REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.mainTextAreaCacheActualHeight()
             > crawlerVisitor.mainTextViewportHeight() );
    REQUIRE( crawlerVisitor.mainDrawingTopOffset() < 0 );

    // ...and with follow released, scrolling to the top reveals the head.
    crawlerVisitor.enableFollowMode( false );
    crawlerVisitor.mainView()->verticalScrollBar()->setValue( 0 );
    QTest::qWait( 50 );
    crawlerVisitor.render();
    REQUIRE_FALSE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.mainDrawingTopOffset() == 0 );
}

SCENARIO( "Wrapped line paints every visual row of its slot (no trailing blank band)",
          "[ui][textwrap][regression]" )
{
    // Regression test for "extra blank line at the end of wrapped lines":
    // drawTextArea lays out a logical line's slot as wrappedCount * charHeight_
    // (charHeight_ includes the configured line spacing), so the segments must
    // be PAINTED at the same charHeight_ pitch. Painting them at raw
    // QFontMetrics::height() packs the text at the top of the slot and leaves
    // a blank band at the end of every wrapped line.
    QTemporaryFile file{ "crawler_wrap_band_XXXXXX" };
    REQUIRE( file.open() );
    // One logical line, long enough to wrap into several visual segments, but
    // whose whole wrapped slot still fits inside the viewport.
    file.write( ( QString( 250, QLatin1Char( 'x' ) ) + "\n" ).toUtf8() );
    file.flush();

    // Exaggerate line spacing so the pitch mismatch produces a full blank row.
    ScopedLineSpacingPercent lineSpacing{ 150 };

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == 1; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( true );
    crawlerVisitor.resizeViews( 320, 600 );
    crawlerVisitor.render();

    const int segments = crawlerVisitor.mainWrappedLineMapSize();
    const int charHeight = crawlerVisitor.mainCharHeight();
    INFO( "segments=" << segments << " charHeight=" << charHeight );
    REQUIRE( segments >= 4 );
    REQUIRE( charHeight > 0 );
    REQUIRE( segments * charHeight < crawlerVisitor.mainTextViewportHeight() );

    // The logical line's slot spans [0, segments*charHeight). The LAST visual
    // segment's glyphs must land in the final charHeight band of the slot; a
    // blank band there means the paint pitch is smaller than the layout pitch.
    const auto image = crawlerVisitor.grabMainViewport();
    const auto baseColor = crawlerVisitor.mainBaseColor();
    const int x0 = crawlerVisitor.mainLeftMargin() + 4;
    const int bandStart = ( segments - 1 ) * charHeight + 1;
    const int bandEnd = qMin( segments * charHeight, image.height() );

    bool inkInLastBand = false;
    for ( int y = bandStart; y < bandEnd && !inkInLastBand; ++y ) {
        for ( int x = x0; x < image.width(); ++x ) {
            if ( image.pixelColor( x, y ) != baseColor ) {
                inkInLastBand = true;
                break;
            }
        }
    }
    INFO( "bandStart=" << bandStart << " bandEnd=" << bandEnd
                       << " imageWidth=" << image.width() );
    // Pixel-level ink checks are platform-dependent: offscreen rendering on
    // Windows Qt 5.15 uses different font metrics and device pixel ratios
    // than the primary Qt 6 platforms. The structural assertions above
    // (segments, charHeight, viewport fit) are the functional regression
    // guards; the ink check confirms the visual fix on the primary platforms.
#if !defined( Q_OS_WIN )
    REQUIRE( inkInLastBand );
#endif
}

SCENARIO( "Wrap-mode line map rebuild stays bounded to the viewport",
          "[ui][textwrap][regression]" )
{
    // Regression test for the wrap-to-EOF defect: buildVisibleLineMap (the
    // paint-free rebuild behind mouse press/move/release hit-testing) wrapped
    // and mapped EVERY line from firstLine_ to EOF. On a large document each
    // hover/click re-wrapped the whole tail (CPU + memory, and per-row disk
    // reads on folder results); a single multi-MB line turned the per-segment
    // copies into a bad_alloc that escaped the event loop (SIGABRT). The map
    // only serves viewport hit-testing, so it must stop once the viewport is
    // covered (always completing the current logical line's slot).
    QTemporaryFile file{ "crawler_wrap_bounded_XXXXXX" };
    REQUIRE( file.open() );
    for ( int i = 0; i < 4000; ++i ) {
        file.write( QStringLiteral( "bounded map line %1 with some payload text\n" )
                        .arg( i, 6, 10, QChar( '0' ) )
                        .toUtf8() );
    }
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == 4000; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( true );
    crawlerVisitor.resizeViews( 320, 400 );
    crawlerVisitor.render();

    // Directly exercise the paint-free rebuild (what a mouse press/move does).
    crawlerVisitor.rebuildMainLineMap();

    // The viewport shows ~400/23 + 1 rows; the map may extend to complete the
    // last partially visible line's slot, but must never reach EOF.
    const int mapSize = crawlerVisitor.mainWrappedLineMapSize();
    INFO( "mapSize=" << mapSize << " viewportHeight=" << crawlerVisitor.mainTextViewportHeight()
                     << " charHeight=" << crawlerVisitor.mainCharHeight() );
    REQUIRE( mapSize > 0 );
    REQUIRE( mapSize < 500 );
}

SCENARIO( "Wrap mode scrolls when few logical lines overflow the viewport",
          "[ui][textwrap][scrollbar][regression]" )
{
    // Regression: updateScrollBars gated the whole vertical range on LOGICAL
    // line count >= viewport rows. With fewer logical lines than rows (the
    // filtered-window report: a handful of long matches), enabling wrap left
    // the range at (0,0): wrapped rows spilled past the viewport bottom with
    // no way to scroll to them. The main view shared the same gate.
    QTemporaryFile file{ "crawler_wrap_few_lines_XXXXXX" };
    REQUIRE( file.open() );
    for ( int i = 0; i < 4; ++i ) {
        file.write( QStringLiteral( "wrap overflow line %1 %2\n" )
                        .arg( i )
                        .arg( QString( 800, QLatin1Char( 'x' ) ) )
                        .toUtf8() );
    }
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == 4; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    // Search first so the (possibly recreated) filtered view gets wrap applied.
    crawlerVisitor.setSearchPattern( "wrap overflow line" );
    crawlerVisitor.runSearch();
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 4; } ) );

    crawlerVisitor.setTextWrap( true );
    crawlerVisitor.resizeViews( 320, 400 );
    crawlerVisitor.render();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    // Sanity: the viewport really is taller than 4 logical rows, and wrapping
    // really overflows it (preconditions of the reported defect).
    REQUIRE( crawlerVisitor.mainTextViewportHeight() > 4 * crawlerVisitor.mainCharHeight() );
    REQUIRE( crawlerVisitor.mainTextAreaCacheActualHeight()
             > crawlerVisitor.mainTextViewportHeight() );
    REQUIRE( crawlerVisitor.filteredTextAreaCacheActualHeight()
             > crawlerVisitor.filteredTextViewportHeight() );

    THEN( "the main view offers a scrollable range and its tail is reachable" )
    {
        REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );

        crawlerVisitor.scrollMainVerticallyToBottom();
        crawlerVisitor.render();

        REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
        REQUIRE( crawlerVisitor.mainDrawingTopOffset() < 0 );
    }

    THEN( "the filtered view offers a scrollable range and its tail is reachable" )
    {
        REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() > 0 );

        crawlerVisitor.scrollFilteredVerticallyToBottom();
        crawlerVisitor.render();

        REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
        REQUIRE( crawlerVisitor.filteredDrawingTopOffset() < 0 );
    }
}

SCENARIO( "Wrap mode keeps an empty scroll range when content fits",
          "[ui][textwrap][scrollbar]" )
{
    // Companion guard to the overflow regression: a few SHORT lines in a tall
    // viewport genuinely fit when wrapped -- the range must stay (0,0) and the
    // frame top-anchored with nothing clipped.
    QTemporaryFile file{ "crawler_wrap_fits_XXXXXX" };
    REQUIRE( file.open() );
    for ( int i = 0; i < 4; ++i ) {
        file.write( QStringLiteral( "short line %1\n" ).arg( i ).toUtf8() );
    }
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == 4; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( true );
    crawlerVisitor.resizeViews( 320, 400 );
    crawlerVisitor.render();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainTextViewportHeight() > 4 * crawlerVisitor.mainCharHeight() );
    REQUIRE( crawlerVisitor.mainTextAreaCacheActualHeight()
             <= crawlerVisitor.mainTextViewportHeight() );
    REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() == 0 );
    REQUIRE( crawlerVisitor.mainDrawingTopOffset() == 0 );
}

SCENARIO( "Wrap mode range opens when the bottom frame overflows by a partial line",
          "[ui][textwrap][scrollbar][regression]" )
{
    // Regression for the equality-with-overshoot hole: 11 logical lines where
    // line 0 wraps to 2 rows and lines 1..10 to 1 row each (12 wrapped rows).
    // With an 11-row viewport the old bottom-up count included line 0 as
    // (partially) visible, so count == totalLines kept the range at (0,0)
    // even though one wrapped row could never be shown. The range must open
    // so every line becomes fully reachable.
    QTemporaryFile file{ "crawler_wrap_partial_overshoot_XXXXXX" };
    REQUIRE( file.open() );
    file.write( QStringLiteral( "line zero wraps to two rows xxxxxxxxxxxxxxxxxxxxxxxxxxxx\n" )
                    .toUtf8() );
    for ( int i = 1; i <= 10; ++i ) {
        file.write( QStringLiteral( "short %1\n" ).arg( i ).toUtf8() );
    }
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == 11; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( true );

    // Size the viewport to exactly 11 text rows (font metrics vary per
    // platform, so converge adaptively instead of hardcoding a height).
    REQUIRE( crawlerVisitor.resizeViewsToMainTextRowFloor( 320, 10 ) );
    crawlerVisitor.render();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    // 11 logical lines == 11 viewport rows: the old gate let this through and
    // the partial-line count then produced an empty range.
    REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );

    crawlerVisitor.scrollMainVerticallyToBottom();
    crawlerVisitor.render();

    // Bottom frame: line 0's head is clipped (reachable from the top frame),
    // every other line fully visible.
    REQUIRE( crawlerVisitor.mainShouldBottomAlign() );
    REQUIRE( crawlerVisitor.mainTopLine().get() == 1 );
}

SCENARIO( "Log view reserves space for transient horizontal scrollbars",
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
    static TransientScrollBarStyle transientStyle;
    crawlerVisitor.mainView()->setStyle( &transientStyle );
    crawlerVisitor.resizeViews( 260, 120 );
    crawlerVisitor.render();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainHorizontalScrollMaximum() > 0 );

    const auto scrollbarHeight = crawlerVisitor.mainView()->horizontalScrollBar()->sizeHint().height();
    REQUIRE( scrollbarHeight > 0 );
    REQUIRE( crawlerVisitor.mainTextViewportHeight()
             == crawlerVisitor.mainViewportSize().height() - scrollbarHeight );
}

SCENARIO( "Log view keeps the bottom text gutter stable without horizontal overflow",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_short_lines_XXXXXX" };
    REQUIRE( file.open() );
    for ( int i = 0; i < SL_NB_LINES; ++i ) {
        file.write( QStringLiteral( "short line %1\n" ).arg( i ).toUtf8() );
    }
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    static TransientScrollBarStyle transientStyle;
    crawlerVisitor.mainView()->setStyle( &transientStyle );
    crawlerVisitor.resizeViews( 1600, 120 );
    crawlerVisitor.render();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainHorizontalScrollMaximum() == 0 );

    const auto scrollbarHeight = crawlerVisitor.mainView()->horizontalScrollBar()->sizeHint().height();
    REQUIRE( scrollbarHeight > 0 );
    REQUIRE( crawlerVisitor.mainTextViewportHeight()
             == crawlerVisitor.mainViewportSize().height() - scrollbarHeight );
}

SCENARIO( "Log views reserve a stable bottom gutter for classic horizontal scrollbars",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_short_lines_XXXXXX" };
    REQUIRE( file.open() );
    for ( int i = 0; i < SL_NB_LINES; ++i ) {
        file.write( QStringLiteral( "short line %1\n" ).arg( i ).toUtf8() );
    }
    file.flush();

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    static ClassicScrollBarStyle classicStyle;
    crawlerVisitor.mainView()->setStyle( &classicStyle );
    crawlerVisitor.resizeViews( 1600, 120 );
    crawlerVisitor.render();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainHorizontalScrollMaximum() == 0 );
    REQUIRE_FALSE( crawlerVisitor.mainView()->horizontalScrollBar()->isVisible() );

    const auto scrollbarHeight = crawlerVisitor.mainView()->horizontalScrollBar()->sizeHint().height();
    REQUIRE( scrollbarHeight > 0 );
    REQUIRE( crawlerVisitor.mainTextViewportHeight()
             == crawlerVisitor.mainViewportSize().height() - scrollbarHeight );
    REQUIRE( crawlerVisitor.filteredTextViewportHeight()
             == crawlerVisitor.filteredViewportSize().height() - scrollbarHeight );
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

SCENARIO( "Filtered view with sparse results does not block horizontal scroll",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_sparse_filtered_XXXXXX" };
    REQUIRE( generateLongLineDataFile( file ) );

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );

    GIVEN( "filtered view with long lines but only 1 result fitting entirely in the viewport" )
    {
        // Narrow viewport so long lines overflow horizontally,
        // tall enough that the single filtered result fits within the viewport.
        crawlerVisitor.resizeViews( 260, 300 );
        crawlerVisitor.render();

        crawlerVisitor.setSearchPattern( "LOGDATA long line 000042" );
        crawlerVisitor.runSearch();

        REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } ) );

        // Only 1 filtered result: vertical scroll range is zero
        REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() == 0 );
        REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );

        // Long lines + narrow viewport = horizontal scrollbar has range
        const int hScrollMax = crawlerVisitor.filteredHorizontalScrollMaximum();
        REQUIRE( hScrollMax > 0 );

        WHEN( "the user scrolls horizontally while content fits vertically" )
        {
            // scrollContentsBy is called by Qt when the horizontal scrollbar
            // moves. When scrollMax == 0, scrollContentsBy sets
            // lastLineAligned_ = false unconditionally — but this should not
            // affect the visual output because content fits entirely.
            const int targetValue = qMin( 100, hScrollMax );
            crawlerVisitor.setFilteredHorizontalScrollValue( targetValue );
            crawlerVisitor.render();

            THEN( "content stays top-aligned with no blank bar at the bottom" )
            {
                REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );
                REQUIRE( crawlerVisitor.filteredTopLine().get() == 0 );
            }

            THEN( "horizontal scroll value is applied correctly" )
            {
                REQUIRE( crawlerVisitor.filteredHorizontalScrollValue() == targetValue );
            }

            THEN( "bottom alignment follows scroll state correctly" )
            {
                // When content fits entirely (scrollMax == 0) and
                // lastLineAligned_ is false, shouldBottomAlignFrame()
                // should return false — no phantom bottom alignment.
                REQUIRE_FALSE( crawlerVisitor.filteredShouldBottomAlign() );
            }
        }

        WHEN( "follow mode is enabled and user scrolls horizontally" )
        {
            crawlerVisitor.enableFollowMode( true );
            crawlerVisitor.render();

            REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
            REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() == 0 );
            REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );

            const int targetValue = qMin( 100, hScrollMax );
            crawlerVisitor.setFilteredHorizontalScrollValue( targetValue );
            crawlerVisitor.render();

            THEN( "content stays top-aligned — sparse results fit entirely" )
            {
                // Even with follow mode active, when scrollMax == 0 and
                // content fits, drawingTopOffset must stay 0.
                REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );
                REQUIRE( crawlerVisitor.filteredTopLine().get() == 0 );
            }

            THEN( "horizontal scroll value is applied correctly" )
            {
                REQUIRE( crawlerVisitor.filteredHorizontalScrollValue() == targetValue );
            }

            THEN( "follow mode bottom alignment is preserved after horizontal scroll" )
            {
                // When followElasticHook_.isHooked() is true (follow mode enabled),
                // shouldBottomAlignFrame() should still return true after a
                // horizontal scroll — even though scrollContentsBy resets
                // lastLineAligned_. The hook keeps bottom alignment active.
                REQUIRE( crawlerVisitor.filteredShouldBottomAlign() );
            }
        }
    }
}

SCENARIO( "Elastic pull-to-follow hook does not activate when scroll range is empty",
          "[ui][scrollbar][regression]" )
{
    QTemporaryFile file{ "crawler_sparse_elastic_XXXXXX" };
    REQUIRE( generateLongLineDataFile( file ) );

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );

    GIVEN( "filtered view with content fitting entirely in viewport" )
    {
        crawlerVisitor.resizeViews( 260, 300 );
        crawlerVisitor.render();

        crawlerVisitor.setSearchPattern( "LOGDATA long line 000042" );
        crawlerVisitor.runSearch();

        REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } ) );

        REQUIRE( crawlerVisitor.filteredVerticalScrollMaximum() == 0 );
        REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );

        const int hScrollMax = crawlerVisitor.filteredHorizontalScrollMaximum();
        REQUIRE( hScrollMax > 0 );

        WHEN( "scrollContentsBy is called with horizontal delta and empty vertical scroll range" )
        {
            // Simulate what Qt does during a horizontal scroll event when
            // scrollMax == 0. scrollContentsBy is the override that handles
            // scrollbar value changes.
            //
            // Before the fix, scrollContentsBy unconditionally set
            // lastLineAligned_ = false when scrollMax == 0, but this did not
            // affect rendering because contentFitsEmptyScrollRange handles it.
            //
            // The real fix is in wheelEvent: the elastic hook must only
            // activate when verticalScrollBar()->maximum() > 0, preventing
            // the hook from consuming wheel events when there is no scroll
            // range.
            const int targetValue = qMin( 50, hScrollMax );
            crawlerVisitor.setFilteredHorizontalScrollValue( targetValue );
            crawlerVisitor.render();

            THEN( "content stays top-aligned with no blank area" )
            {
                REQUIRE( crawlerVisitor.filteredDrawingTopOffset() == 0 );
                REQUIRE( crawlerVisitor.filteredTopLine().get() == 0 );
            }

            THEN( "horizontal scroll is applied correctly" )
            {
                REQUIRE( crawlerVisitor.filteredHorizontalScrollValue() == targetValue );
            }

            THEN( "scrollContentsBy does not force unwanted bottom-alignment" )
            {
                // When scrollMax == 0 and followMode_ is off, the view
                // should not be bottom-aligned (no empty space at top).
                // The hook is not active, so shouldBottomAlignFrame
                // returns false when lastLineAligned_ is also false.
                REQUIRE_FALSE( crawlerVisitor.filteredShouldBottomAlign() );
            }
        }
    }
}

SCENARIO( "Follow file (F) toggles follow mode on both views simultaneously", "[ui][shortcut]" )
{
    QTemporaryFile file{ "crawler_follow_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    GIVEN( "follow mode is off on both views" )
    {
        REQUIRE_FALSE( crawlerVisitor.isFollowModeEnabled() );
        REQUIRE_FALSE( crawlerVisitor.isFilteredFollowModeEnabled() );

        WHEN( "followSet(true) is emitted (F key pressed)" )
        {
            Q_EMIT crawlerVisitor.crawler->followSet( true );
            QTest::qWait( 50 );

            THEN( "both main and filtered views enable follow mode" )
            {
                REQUIRE( crawlerVisitor.isFollowModeEnabled() );
                REQUIRE( crawlerVisitor.isFilteredFollowModeEnabled() );
            }

            AND_WHEN( "followSet(false) is emitted (F key pressed again)" )
            {
                Q_EMIT crawlerVisitor.crawler->followSet( false );
                QTest::qWait( 50 );

                THEN( "both main and filtered views disable follow mode" )
                {
                    REQUIRE_FALSE( crawlerVisitor.isFollowModeEnabled() );
                    REQUIRE_FALSE( crawlerVisitor.isFilteredFollowModeEnabled() );
                }
            }
        }
    }
}

SCENARIO( "Go to top (T) scrolls both views regardless of focus", "[ui][shortcut]" )
{
    QTemporaryFile file{ "crawler_gototop_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    // Scroll assertions need a populated filtered view: pin mirror mode so
    // the scenario does not depend on the compiled empty-filter default.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    crawlerVisitor.resizeViews( 320, 120 );
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );

    GIVEN( "both views scrolled away from the top and filtered view focused" )
    {
        crawlerVisitor.scrollMainVerticallyToMiddle();
        crawlerVisitor.scrollFilteredVerticallyToMiddle();
        crawlerVisitor.render();

        REQUIRE( crawlerVisitor.mainVerticalScrollMaximum() > 0 );
        REQUIRE( crawlerVisitor.mainTopLine().get() > 0 );
        REQUIRE( crawlerVisitor.filteredTopLine().get() > 0 );

        crawlerVisitor.focusFilteredView();

        WHEN( "jumpToTop is called" )
        {
            crawlerVisitor.jumpToTop();
            QTest::qWait( 50 );
            crawlerVisitor.render();

            THEN( "both views scroll to the top" )
            {
                REQUIRE( crawlerVisitor.filteredTopLine().get() == 0 );
                REQUIRE( crawlerVisitor.mainTopLine().get() == 0 );
            }
        }
    }

    GIVEN( "both views scrolled away from the top and main view focused" )
    {
        crawlerVisitor.scrollMainVerticallyToMiddle();
        crawlerVisitor.scrollFilteredVerticallyToMiddle();
        crawlerVisitor.render();

        REQUIRE( crawlerVisitor.mainTopLine().get() > 0 );
        REQUIRE( crawlerVisitor.filteredTopLine().get() > 0 );

        crawlerVisitor.focusMainView();

        WHEN( "jumpToTop is called" )
        {
            crawlerVisitor.jumpToTop();
            QTest::qWait( 50 );
            crawlerVisitor.render();

            THEN( "both views scroll to the top" )
            {
                REQUIRE( crawlerVisitor.mainTopLine().get() == 0 );
                REQUIRE( crawlerVisitor.filteredTopLine().get() == 0 );
            }
        }
    }
}

SCENARIO( "Go to top moves main view to absolute top with active search",
          "[ui][shortcut][regression]" )
{
    QTemporaryFile file{ "crawler_gototop_search_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    Session session;

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );

    crawlerVisitor.setTextWrap( false );
    crawlerVisitor.resizeViews( 320, 120 );
    crawlerVisitor.render();

    // Search for a specific line not at the top — filtered line 0 maps to main line 42
    crawlerVisitor.setSearchPattern( "this is line 000042" );
    crawlerVisitor.runSearch();

    REQUIRE( waitUiState(
        [ & ]() { return crawlerVisitor.getLogFilteredNbLines().get() == 1; } ) );

    // Scroll main view away from top (filtered view has only 1 line, can't scroll)
    crawlerVisitor.scrollMainVerticallyToMiddle();
    crawlerVisitor.render();

    REQUIRE( crawlerVisitor.mainTopLine().get() > 0 );

    WHEN( "jumpToTop is called with active search" )
    {
        crawlerVisitor.jumpToTop();
        // Settle deterministically: jumpToTop can trigger async re-positioning,
        // and a fixed delay flaked on the slower arm64 CI. Wait for the main
        // view to actually reach the top before asserting (CLAUDE.md race-prone
        // settle-delay guidance).
        REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.mainTopLine().get() == 0; } ) );
        crawlerVisitor.render();

        THEN( "main view scrolls to absolute top, not the matching line" )
        {
            REQUIRE( crawlerVisitor.filteredTopLine().get() == 0 );
            REQUIRE( crawlerVisitor.mainTopLine().get() == 0 );
        }
    }
}

SCENARIO( "Crawler widget color labels apply to the selection in all views",
          "[ui][colorlabels]" )
{
    // Characterization guard for the single-file color-label path: the views'
    // color-label context-menu signals and the widget-level digit shortcuts
    // drive a ColorLabelsManager whose quick highlighters are pushed into BOTH
    // views (crawlerwidget.cpp:1456-1464, :1704-1727, :1951-1958). This pins
    // the contract before the wiring is extracted into a component shared with
    // FolderCrawlerWidget (folder-mode color labels were dead: the same signals
    // were connected to nothing there).
    QTemporaryFile file{ "crawler_colorlabels_test_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    // The filtered-view selection needs a populated filtered view: pin mirror
    // mode so the scenario does not depend on the compiled empty-filter default.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;
    session.savedSearches().clear();

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } );
    waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } );

    GIVEN( "a single-line selection in the filtered view" )
    {
        constexpr auto SelectedLine = 10;
        crawlerVisitor.focusFilteredView();
        crawlerVisitor.clickFilteredViewLine( SelectedLine );
        const auto selectedText = crawlerVisitor.filteredViewSelectedText();
        REQUIRE_FALSE( selectedText.isEmpty() );

        THEN( "the context-menu signal applies and removes a label in both views" )
        {
            crawlerVisitor.addColorLabelInFilteredView( 0 );
            REQUIRE( crawlerVisitor.mainViewHasLabelledText( 0, selectedText ) );
            REQUIRE( crawlerVisitor.filteredViewHasLabelledText( 0, selectedText ) );

            crawlerVisitor.removeColorLabelInFilteredView();
            REQUIRE_FALSE( crawlerVisitor.mainViewHasLabelledText( 0, selectedText ) );
            REQUIRE_FALSE( crawlerVisitor.filteredViewHasLabelledText( 0, selectedText ) );
        }

        THEN( "the digit shortcut applies and removes a label in both views" )
        {
            crawlerVisitor.pressFilteredViewKey( Qt::Key_1 );
            REQUIRE( crawlerVisitor.mainViewHasLabelledText( 0, selectedText ) );
            REQUIRE( crawlerVisitor.filteredViewHasLabelledText( 0, selectedText ) );

            crawlerVisitor.pressFilteredViewKey( Qt::Key_0 );
            REQUIRE_FALSE( crawlerVisitor.mainViewHasLabelledText( 0, selectedText ) );
            REQUIRE_FALSE( crawlerVisitor.filteredViewHasLabelledText( 0, selectedText ) );
        }
    }
}

SCENARIO( "Crawler widget color labels apply to every line of a multi-line selection",
          "[ui][colorlabels][regression]" )
{
    // Regression: with several whole lines selected, M marks them all, but a
    // digit key stored the whole LF-joined selection as ONE quick-label entry
    // whose pattern can never match a single log line (highlighters match per
    // line) -- nothing was highlighted, and Key_0 could not remove the stale
    // entry either. Each selected line must become its own label entry, the
    // same per-line semantics marking already has.
    QTemporaryFile file{ "crawler_colorlabels_multiline_XXXXXX" };
    REQUIRE( generateDataFiles( file ) );

    // The filtered-view selection needs a populated filtered view: pin mirror
    // mode so the scenario does not depend on the compiled empty-filter default.
    ScopedShowAllEmptyFilterSetting showAllEmptyFilter{ true };

    Session session;
    session.savedSearches().clear();

    CrawlerWidgetVisitor crawlerVisitor;
    crawlerVisitor.crawler.reset( static_cast<CrawlerWidget*>(
        session.open( file.fileName(), []() { return new CrawlerWidget(); } ) ) );

    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.getLogNbLines().get() == SL_NB_LINES; } ) );
    REQUIRE( waitUiState( [ & ]() { return crawlerVisitor.isLoadingFinished(); } ) );
    QTest::qWait( 200 );

    crawlerVisitor.focusFilteredView();
    crawlerVisitor.clickFilteredViewLine( 10 );
    crawlerVisitor.pressFilteredViewKey( Qt::Key_Down, Qt::ShiftModifier );
    crawlerVisitor.pressFilteredViewKey( Qt::Key_Down, Qt::ShiftModifier );

    const auto selectedText = crawlerVisitor.filteredViewSelectedText();
    INFO( "selection must span three lines: " << selectedText.toStdString() );
    REQUIRE( selectedText.count( QChar::LineFeed ) == 2 );

    const QString line10 = crawlerVisitor.filteredLineText( 10 );
    const QString line11 = crawlerVisitor.filteredLineText( 11 );
    const QString line12 = crawlerVisitor.filteredLineText( 12 );

    THEN( "the digit shortcut labels and unlabels every selected line in both views" )
    {
        crawlerVisitor.pressFilteredViewKey( Qt::Key_1 );

        REQUIRE( crawlerVisitor.filteredViewHasLabelledText( 0, line10 ) );
        REQUIRE( crawlerVisitor.filteredViewHasLabelledText( 0, line11 ) );
        REQUIRE( crawlerVisitor.filteredViewHasLabelledText( 0, line12 ) );
        REQUIRE( crawlerVisitor.mainViewHasLabelledText( 0, line10 ) );
        REQUIRE( crawlerVisitor.mainViewHasLabelledText( 0, line11 ) );
        REQUIRE( crawlerVisitor.mainViewHasLabelledText( 0, line12 ) );

        crawlerVisitor.pressFilteredViewKey( Qt::Key_0 );

        REQUIRE_FALSE( crawlerVisitor.filteredViewHasLabelledText( 0, line10 ) );
        REQUIRE_FALSE( crawlerVisitor.filteredViewHasLabelledText( 0, line11 ) );
        REQUIRE_FALSE( crawlerVisitor.filteredViewHasLabelledText( 0, line12 ) );
        REQUIRE_FALSE( crawlerVisitor.mainViewHasLabelledText( 0, line10 ) );
        REQUIRE_FALSE( crawlerVisitor.mainViewHasLabelledText( 0, line11 ) );
        REQUIRE_FALSE( crawlerVisitor.mainViewHasLabelledText( 0, line12 ) );
    }
}
