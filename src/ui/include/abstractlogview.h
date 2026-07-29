/*
 * Copyright (C) 2009, 2010, 2011, 2012, 2013, 2017 Nicolas Bonnefon
 * and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2021 Anton Filimonov and other contributors
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

#ifndef ABSTRACTLOGVIEW_H
#define ABSTRACTLOGVIEW_H

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <qchar.h>
#include <string_view>
#include <utility>
#include <vector>

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QColor>
#include <QEvent>
#include <QFontMetrics>

#include "abstractlogdata.h"
#include "linekind.h"
#include "highlighterset.h"
#include "linetypes.h"
#include "markprovider.h"
#include "overviewwidget.h"
#include "quickfind.h"
#include "quickfindmux.h"
#include "regularexpressionpattern.h"
#include "selection.h"
#include "viewtools.h"
#include "wrappedstring.h"

class QMenu;
class QAction;
class QShortcut;
class HighlightersMenu;

// Utility class representing a buffer for number entered on the keyboard
// The buffer keep at most 7 digits, and reset itself after a timeout.
class DigitsBuffer : public QObject {
    Q_OBJECT

  public:
    // Reset the buffer.
    void reset();
    // Add a single digit to the buffer (discarded if it's not a digit),
    // the timeout timer is reset.
    void add( char character );
    // Get the content of the buffer (0 if empty) and reset it.
    LineNumber::UnderlyingType content();

    bool isEmpty() const;

  protected:
    void timerEvent( QTimerEvent* event ) override;

  private:
    // Duration of the timeout in milliseconds.
    static constexpr int DigitsTimeout = 2000;

    QString digits_;

    QBasicTimer timer_;
};

class Overview;

// Base class representing the log view widget.
// It can be either the top (full) or bottom (filtered) view.
class AbstractLogView : public QAbstractScrollArea, public SearchableWidgetInterface {
    Q_OBJECT

  public:
    template <class T>
    struct access_by;

    // Constructor of the widget, the data set is passed.
    // The caller retains ownership of the data set.
    // The pointer to the QFP is used for colouring and QuickFind searches
    AbstractLogView( const AbstractLogData* newLogData, const QuickFindPattern* const quickFind,
                     QWidget* parent = nullptr );

    ~AbstractLogView() override;

    // rule of 5
    AbstractLogView( const AbstractLogView& ) = delete;
    AbstractLogView( AbstractLogView&& ) = delete;
    AbstractLogView& operator=( const AbstractLogView& ) = delete;
    AbstractLogView& operator=( AbstractLogView&& ) = delete;

    void updateFont( const QFont& font );

    // Refresh the widget when the data set has changed.
    void updateData();

    // Swap the underlying data set to `newLogData` (folder mode: the main view
    // is repointed at the file of the selected result row). Resets scroll,
    // selection and the wrap/line cache, then forces a full redraw. The caller
    // keeps ownership of the data and must keep it alive across paints.
    void setDataSource( const AbstractLogData* newLogData );
    // Exposed for testing: the exclusive end of the current search range.
    // After setDataSource it must span the whole document, otherwise every body
    // line renders as "out of search range" (gray).
    LineNumber searchEndLine() const { return searchEnd_; }
    // True when the visible-line coordinate map is populated for the current
    // layout (false after a layout change or data swap, until the next paint --
    // or ensureLineMapFresh -- rebuilds it). Tests assert this to know whether a
    // click will resolve; it reflects the map (wrappedLinesInfo_), not the
    // pixmap cache, so a paint-free rebuild (buildVisibleLineMap) satisfies it.
    bool isLineMapCurrent() const { return !wrappedLinesInfo_.empty(); }
    // Synchronously rebuild the visible-line map if a layout change (forceRefresh)
    // or data swap (setDataSource) has invalidated it. The mouse handlers call
    // this before converting coordinates so a click delivered before the next
    // async paint -- or on a viewport Qt never painted (hidden/unrealized) --
    // resolves to the current row instead of a stale/empty map.
    void ensureLineMapFresh();
#ifdef KLOGG_TESTS
    // Exposed for testing: resolve a viewport y to a line using the current map
    // (nullopt when the map is empty). Lets headless tests verify the paint-free
    // rebuild without synthesizing a mouse event.
    OptionalLineNumber lineAtYForTest( int yPos ) const { return convertCoordToLine( yPos ); }
    // Exposed for testing: how many times the paint-free map rebuild
    // (buildVisibleLineMap, driven by ensureLineMapFresh) has run. Lets headless
    // tests verify the signature cache skips redundant rebuilds across mouse
    // events that share the same data/geometry.
    int visibleLineMapBuildCount() const { return visibleLineMapBuildCount_; }
#endif
    // Instructs the widget to update it's content geometry,
    // used when the font is changed.
    void updateDisplaySize();
    // Return the line number of the top line of the view
    LineNumber getTopLine() const;
    // Return the text of the current selection.
    QString getSelectedText() const;
    // Return the selected text as one entry per selected line, in row order
    // (a portion selection yields a single entry). This is the per-line dual
    // of getSelectedText(): consumers that match text per log line (color
    // labels) must use it, as a label entry containing a line feed can never
    // match a single line.
    QStringList getSelectedLinesText() const;
    // True for partial selection
    bool isPartialSelection() const;
    // Instructs the widget to select the whole text.
    void selectAll();
    // Inject a read-only mark source (for views without LogFilteredData, e.g.
    // the folder main view and folder results view). Subclass lineType()
    // overrides OR in LineTypeFlags::Mark from it; the LogViewNextMark/PrevMark
    // navigation consults markAfter/markBefore. The caller owns the provider.
    void setMarkProvider( const MarkProvider* provider ) { markProvider_ = provider; }

    bool isFollowEnabled() const
    {
        return followMode_;
    }

    bool isTextWrapEnabled() const
    {
        return useTextWrap_;
    }

    void allowFollowMode( bool allow );

    void setSearchPattern( const RegularExpressionPattern& pattern );

    // Read-only access to the currently-wired search pattern. Used by tests to
    // assert that the host widget forwarded the search pattern (e.g. folder mode
    // forwards it so the paint pass highlights in-result matches). Has no
    // functional effect; the pattern is only consumed at paint time.
    RegularExpressionPattern searchPattern() const
    {
        return searchPattern_;
    }

    using QuickHighlighters = QList<QuickLabelEntry>;
    void setQuickHighlighters( const std::vector<QuickHighlighters>& wordHighlighters );

    void registerShortcuts();

  protected:
    void mousePressEvent( QMouseEvent* mouseEvent ) override;
    void mouseMoveEvent( QMouseEvent* mouseEvent ) override;
    void mouseReleaseEvent( QMouseEvent* ) override;
    void mouseDoubleClickEvent( QMouseEvent* mouseEvent ) override;
    void timerEvent( QTimerEvent* timerEvent ) override;
    void changeEvent( QEvent* changeEvent ) override;
    void paintEvent( QPaintEvent* paintEvent ) override;
    void resizeEvent( QResizeEvent* resizeEvent ) override;
    void scrollContentsBy( int dx, int dy ) override;
    void keyPressEvent( QKeyEvent* keyEvent ) override;
    void wheelEvent( QWheelEvent* wheelEvent ) override;
    bool event( QEvent* e ) override;

    // Must be implemented to return what LineType the line number is
    // (used for coloured bullets)
    virtual AbstractLogData::LineType lineType( LineNumber lineNumber ) const = 0;

    // What kind of row this visible line is. Plain single-file views only have
    // Data rows; folder-search results additionally interleave Header rows
    // (one per source file group). The base default is Data, so single-file
    // views are unaffected. Header rows skip the bullet/line-number gutters and
    // route clicks to collapse/expand instead of selection.
    virtual LineKind lineKind( LineNumber lineNumber ) const;

    // Line number to display for line at the given index
    virtual LineNumber displayLineNumber( LineNumber lineNumber ) const;
    virtual LineNumber lineIndex( LineNumber lineNumber ) const;
    virtual LineNumber maxDisplayLineNumber() const;

    // Returns whether search range graying should be applied (true by default)
    // FilteredView overrides this to return false since all visible lines are part of the filter
    virtual bool shouldApplySearchRangeGraying() const;

    // Read-only mark source (folder views), or null. Subclass lineType()
    // overrides consult this to OR in LineTypeFlags::Mark.
    const MarkProvider* markProvider_ = nullptr;

    // Get the overview associated with this view, or NULL if there is none
    Overview* getOverview() const
    {
        return overview_;
    }
    // Set the Overview and OverviewWidget
    void setOverview( Overview* overview, OverviewWidget* overviewWidget );

    // Returns the current "position" of the view as a line number,
    // it is either the selected line or the middle of the view.
    LineNumber getViewPosition() const;

    virtual void doRegisterShortcuts();
    void registerShortcut( const std::string& action, std::function<void()> func );

    // Next/previous-mark navigation backing the LogViewNextMark/LogViewPrevMark
    // shortcuts (registered once here in doRegisterShortcuts). The default
    // consults the injected MarkProvider when present and otherwise walks rows
    // via the virtual lineType(); LogMainView overrides to use its
    // LogFilteredData mark index (O(log n) on huge files).
    virtual void selectNextMark();
    virtual void selectPrevMark();

  Q_SIGNALS:
    // Sent up to the MainWindow to enable/disable the follow mode
    void followModeChanged( bool enabled );
    // Sent when the view wants the QuickFind widget pattern to change.
    void changeQuickFind( const QString& newPattern, QuickFindMux::QFDirection newDirection );
    // Sent when a new line has been selected by the user
    void newSelection( LineNumber startLine, LinesCount nLines, LineColumn startCol,
                       LineLength nSymbols );
    // Sent when a Header row is clicked (folder mode) so the view can
    // collapse/expand that file's result group. Not emitted for Data rows.
    void headerClicked( LineNumber lineNumber );
    // Sent up when quickFind wants to show a message to the user.
    void notifyQuickFind( const QFNotification& message );
    // Sent up when quickFind wants to clear the notification.
    void clearQuickFindNotification();
    // Sent when the view ask for a line to be marked
    // (click in the left margin).
    void markLines( const klogg::vector<LineNumber>& lines );
    // Sent when the view asks marks to be removed for selected lines.
    void deleteMarkLines( const klogg::vector<LineNumber>& lines );
    // Sent up when the user wants to add the selection to the search
    void addToSearch( const QString& selection );
    // Sent up when the user wants to replace the search with the selection
    void replaceSearch( const QString& selection );
    void excludeFromSearch( const QString& selection );
    // Sent up when the mouse is hovered over a line's margin
    void mouseHoveredOverLine( LineNumber line );
    // Sent up when the mouse leaves a line's margin
    void mouseLeftHoveringZone();
    // Sent up for view initiated quickfind searches
    void searchNext();
    void searchPrevious();
    // Sent up when the user has moved within the view
    void activity();
    // Sent up when the user want to exit this view
    // (switch to the next one)
    void exitView();

    void changeSearchLimits( LineNumber startLine, LineNumber endLine );
    void clearSearchLimits();

    void saveDefaultSplitterSizes();
    void sendSelectionToScratchpad();
    void replaceScratchpadWithSelection();
    void changeFontSize( bool increase );

    void addColorLabel( size_t label );
    void addNextColorLabel();
    void removeColorLabel();
    void clearColorLabels();
    void quickColorLabelDefaultsChanged( bool ignoreCase, bool wholeWord );
    void highlightersChange();

  public Q_SLOTS:
    // Makes the widget select and display the passed line.
    // Scrolling as necessary
    void trySelectLine( LineNumber newLine );
    void selectAndDisplayLine( LineNumber line );
    void selectPortionAndDisplayLine( LineNumber line, LinesCount nLines, LineColumn startCol,
                                      LineLength nSymbols );
    // Toggle a mark / delete a mark on the current selection (the M / N
    // shortcuts). Public so hosts can drive them programmatically (and tests
    // can exercise the markLines/deleteMarkLines wiring without a real
    // keypress, which is unreliable headless).
    void markSelected();
    void deleteMarksSelected();

    // Use the current QFP to go and select the next match.
    void searchForward() override;
    // Use the current QFP to go and select the previous match.
    void searchBackward() override;

    // Use the current QFP to go and select the next match (incremental)
    void incrementallySearchForward() override;
    // Use the current QFP to go and select the previous match (incremental)
    void incrementallySearchBackward() override;
    // Stop the current incremental search (typically when user press return)
    void incrementalSearchStop() override;
    // Abort the current incremental search (typically when user press esc)
    void incrementalSearchAbort() override;
    // Synchronously interrupt the QuickFind search and block until its
    // QThreadPool worker has finished. The worker holds a `const
    // AbstractLogData&` and reads it off the UI thread; a host whose
    // data-source members are destroyed before its Qt-child views (so
    // ~AbstractLogView, which joins the worker, runs after the data is freed)
    // must call this on each view BEFORE releasing the data, else the worker
    // reads freed memory (the FolderCrawlerWidget teardown use-after-free that
    // surfaced as the flaky Windows-x86 CI crash).
    void stopSearchAndWait();

    // Signals the follow mode has been enabled.
    void followSet( bool checked );

    // Signals the text wrap mode has been enabled.
    void textWrapSet( bool checked );

    // Signal the on/off status of the overview has been changed.
    void refreshOverview();

    // Make the view jump to the specified line, regardless of it
    // being on the screen or not. (does NOT Q_EMIT followDisabled() )
    void jumpToLine( LineNumber line );

    // Configure the setting of whether to show line number margin
    void setLineNumbersVisible( bool lineNumbersVisible );

    // Whether this view offers the "Set search start/end" + "Clear search
    // limits" context-menu actions. Those limit a LogFilteredData-driven
    // search; views backed by a search model with no range support (folder
    // mode: the streaming engine ignores limits) must hide them instead of
    // graying the view without limiting anything. Default true (single-file
    // behavior). Setting it also updates the menu actions' visibility
    // immediately.
    void setControlsSearchLimits( bool controlsSearchLimits );
    bool controlsSearchLimits() const
    {
        return controlsSearchLimits_;
    }

    // Whether the line-number margin is currently shown. Mirrors
    // setLineNumbersVisible; used to re-apply Configuration after a data-source
    // swap and by tests.
    bool isLineNumbersVisible() const
    {
        return lineNumbersVisible_;
    }

    // Swap the QuickFindPattern this view listens to (disconnects the old
    // pattern's patternUpdated signal and connects the new one). Used by folder
    // mode to rebind the views to the session-wide QuickFindPattern after
    // construction, so the app-wide QuickFindMux drives the folder's views.
    void setQuickFindPattern( const QuickFindPattern* qfp );
    // Inspection accessor for the current QuickFindPattern (tests + rebind checks).
    const QuickFindPattern* quickFindPattern() const
    {
        return quickFindPattern_;
    }

    // Force the next refresh to fully redraw the view by invalidating the cache.
    // To be used if the data might have changed.
    void forceRefresh();

    void setSearchLimits( LineNumber startLine, LineNumber endLine );

  private Q_SLOTS:
    void handlePatternUpdated();
    void addToSearch();
    void replaceSearch();
    void excludeFromSearch();
    void findNextSelected();
    void findPreviousSelected();
    void copy();
    void copyWithLineNumbers();
    void saveToFile();
    void saveSelectedToFile();
    void setSearchStart();
    void setSearchEnd();
    void setSelectionStart();
    void setSelectionEnd();
    void setQuickFindResult( bool hasMatch, const Portion& selection );
    void setColorLabel( QAction* action );

  private:
    // Linear fallback for selectNextMark/selectPrevMark when no MarkProvider is
    // injected: walk rows from `from` (exclusive) in the given direction and
    // return the first whose lineType carries Mark.
    std::optional<LineNumber> findMarkedLine( LineNumber from, bool forward ) const;

    // Graphic parameters
    static constexpr int OverviewWidth = 27;
    static constexpr int HookThreshold = 300;
    static constexpr int PullToFollowHookedHeight = 10;

    // Width of the bullet zone, including decoration
    int bulletZoneWidthPx_;

    // Total size of all margins and decorations in pixels
    int leftMarginPx_ = 0;

    // Digits buffer (for numeric keyboard entry)
    DigitsBuffer digitsBuffer_;

    // Follow mode
    bool followMode_ = false;

    // ElasticHook for follow mode
    ElasticHook followElasticHook_;

    // Whether to show line numbers or not
    bool lineNumbersVisible_ = false;
    // Whether the search-limits context-menu actions are offered (see
    // setControlsSearchLimits). True by default (single-file behavior).
    bool controlsSearchLimits_ = true;

    // Pointer to the CrawlerWidget's data set
    const AbstractLogData* logData_;

    // Pointer to the Overview object
    Overview* overview_ = nullptr;

    // Pointer to the OverviewWidget, this class doesn't own it,
    // but is responsible for displaying it (for purely aesthetic
    // reasons).
    OverviewWidget* overviewWidget_ = nullptr;

    bool selectionStarted_ = false;
    // Start of the selection (characters)
    FilePosition selectionStartPos_;
    // Current end of the selection (characters)
    FilePosition selectionCurrentEndPos_;
    QBasicTimer autoScrollTimer_;

    // Hovering state
    // Last line that has been hoovered on, -1 if none
    OptionalLineNumber lastHoveredLine_;

    // Marks (left margin click)
    bool markingClickInitiated_ = false;
    OptionalLineNumber markingClickLine_;

    Selection selection_;
    RegularExpressionPattern searchPattern_;

    std::vector<QuickHighlighters> quickHighlighters_ = std::vector<QuickHighlighters>{ 9 };

    // Position of the view, those are crucial to control drawing
    // firstLine gives the position of the view,
    // lastLineAligned == true make the bottom of the last line aligned
    // rather than the top of the top one.
    LineNumber firstLine_;
    bool lastLineAligned_ = false;
    bool useTextWrap_ = false;
    LineColumn firstCol_ = 0_lcol;

    struct WrappedLineData {
      LineNumber lineNumber;
      size_t wrappedLineIndex;
      // One WrappedString per LOGICAL line, shared by every visual segment of
      // that line (a line wrapping to N segments used to store N full copies
      // of its N-segment vector -- 16*N^2 bytes -- which turned a multi-MB
      // line into a bad_alloc escaping the event loop).
      std::shared_ptr<const WrappedString> wrappedString;
    };
    klogg::vector<WrappedLineData> wrappedLinesInfo_;

    LineNumber searchStart_;
    LineNumber searchEnd_;

    OptionalLineNumber selectionStart_;

    // Text handling
    int charWidth_ = 1;
    int charHeight_ = 10;

    // Configured font stored to prevent resets from style/theme changes
    QFont configuredFont_;

    // Cached visible column count to avoid repeated calculations
    mutable LineLength cachedVisibleCols_ = 0_length;
    mutable bool cachedVisibleColsValid_ = false;
    
    // Flag to defer scrollbar update after paintEvent() completes
    // Set to true when leftMarginPx_ is initialized during drawTextArea()
    mutable bool pendingScrollBarUpdate_ = false;

    // Flag indicating selection changed without content/viewport change.
    // When true, paintEvent triggers a redraw but does not mark the text cache
    // as independently invalid, separating selection updates from content updates.
    bool selectionChanged_ = false;

    // Popup menu
    QMenu* popupMenu_;
    QAction* copyAction_;
    QAction* copyWithLineNumbersAction_;
    QAction* markAction_;
    QAction* deleteMarkAction_;
    QAction* sendToScratchpadAction_;
    QAction* replaceInScratchpadAction_;
    QAction* saveToFileAction_;
    QAction* saveSelectedToFileAction_;
    QAction* findNextAction_;
    QAction* findPreviousAction_;
    QAction* addToSearchAction_;
    QAction* replaceSearchAction_;
    QAction* excludeFromSearchAction_;
    QAction* setSearchStartAction_;
    QAction* setSearchEndAction_;
    QAction* clearSearchLimitAction_;
    QAction* setSelectionStartAction_;
    QAction* setSelectionEndAction_;
    QAction* saveDefaultSplitterSizesAction_;
    HighlightersMenu* highlightersMenu_;
    QMenu* colorLabelsMenu_;

    std::map<QString, QShortcut*> shortcuts_;

    // Pointer to the CrawlerWidget's QFP object
    const QuickFindPattern* quickFindPattern_;
    // Our own QuickFind object
    QuickFind* quickFind_;

    // Vertical offset (in pixels) at which the first line of text is written
    int drawingTopOffset_ = 0;

    // Cache pixmap and associated info
    struct TextAreaCache {
        QPixmap pixmap_;
        bool invalid_;
        LineNumber first_line_;
        LineNumber last_line_;
        LineColumn first_column_;
        int actual_height_; // Actual pixel height of drawn content (for text wrap)
    };
    struct PullToFollowCache {
        QPixmap pixmap_;
        LineLength nb_columns_;
    };
    TextAreaCache textAreaCache_ = { {}, true, 0_lnum, 0_lnum, 0_lcol, 0 };
    PullToFollowCache pullToFollowCache_ = { {}, 0_length };
    QFontMetrics pixmapFontMetrics_;

    // Test instrumentation: counts calls to getSelectedText() for perf verification
    mutable int getSelectedTextCallCount_ = 0;

    // Signature of every input that determines wrappedLinesInfo_, captured each
    // time buildVisibleLineMap runs. ensureLineMapFresh compares the current key
    // against the last build's and skips the rebuild when they match: consecutive
    // mouse events that share data/geometry reuse the map instead of re-reading
    // and re-wrapping the rest of the file (the wrap-mode hover perf bug). If any
    // input changes the key changes too, so the cache cannot go stale by
    // construction -- there are no scattered dirty flags to keep in sync.
    struct VisibleLineMapKey {
        const AbstractLogData* logData = nullptr;
        uint64_t totalLines = 0; // logData_->getNbLine().get()
        uint64_t firstLine = 0; // firstLine_.get()
        bool useTextWrap = false;
        int viewportWidth = 0;
        int viewportHeight = 0;
        int charWidth = 0;
        int charHeight = 0;
        bool lineNumbersVisible = false;
        // Manual compare (project is C++17; defaulted operator== is C++20).
        bool operator==( const VisibleLineMapKey& other ) const
        {
            return logData == other.logData && totalLines == other.totalLines
                && firstLine == other.firstLine && useTextWrap == other.useTextWrap
                && viewportWidth == other.viewportWidth && viewportHeight == other.viewportHeight
                && charWidth == other.charWidth && charHeight == other.charHeight
                && lineNumbersVisible == other.lineNumbersVisible;
        }
        bool operator!=( const VisibleLineMapKey& other ) const { return !( *this == other ); }
    };
    VisibleLineMapKey currentVisibleLineMapKey() const;
    VisibleLineMapKey lastVisibleLineMapKey_{};
    bool visibleLineMapKeyValid_ = false;
    // Test instrumentation: counts paint-free map rebuilds (buildVisibleLineMap).
    int visibleLineMapBuildCount_ = 0;

    LinesCount getNbVisibleLines() const;
    // Composition of the bottom frame in wrap mode: how many logical lines the
    // frame holds (its top line counted even when only partially visible, so
    // the frame stays filled) and whether the wrapped document overflows the
    // viewport. The scroll range must be derived from BOTH: a document whose
    // lines all fit the frame count can still overflow (clipped top line), in
    // which case the range must open for the tail to be reachable.
    struct WrappedBottomFrame {
        LinesCount frameLines;
        bool contentOverflows;
    };
    WrappedBottomFrame getWrappedBottomFrame() const;
    LineLength getNbVisibleCols() const;
    int textViewportHeight() const;
    int horizontalScrollBarOverlayHeight() const;

    FilePosition convertCoordToFilePos( const QPoint& pos ) const;
    OptionalLineNumber convertCoordToLine( int yPos ) const;
    LineColumn convertCoordToColumn( int xPos ) const;
    // Rebuild wrappedLinesInfo_ (the viewport-y -> LineNumber map) from the
    // current data/layout WITHOUT a QPainter -- the geometry-only subset of
    // drawTextArea's per-line loop. Used by ensureLineMapFresh so hit-testing
    // works on viewports Qt never painted (hidden/unrealized) or right after a
    // streaming updateData that left the map stale. KEEP IN SYNC with the wrap
    // math in drawTextArea (same leftMarginPx_, availableWidth, font metrics).
    void buildVisibleLineMap();

    void displayLine( LineNumber line );
    void moveSelection( LinesCount delta, bool isDeltaNegative );
    void moveSelectionUp();
    void moveSelectionDown();
    void jumpToStartOfLine();
    void jumpToEndOfLine();
    void jumpToRightOfScreen();
    void jumpToTop();
    void jumpToBottom();
    void selectWordAtPosition( const FilePosition& pos );

    void updateSearchLimits();

    void createMenu();

    void considerMouseHovering( int xPos, int yPos );

    LineLength maxLineLength( const klogg::vector<LineNumber>& lines ) const;

    // Save specified lines in range [begin, end) to a file
    void saveLinesToFile( LineNumber begin, LineNumber end );

    // Search functions (for n/N)
    using QuickFindSearchFn = void ( QuickFind::* )( Selection, QuickFindMatcher );
    void searchUsingFunction( QuickFindSearchFn searchFunction );

    void updateScrollBars();

    LineNumber verticalScrollToLineNumber( int scrollPosition ) const;
    int lineNumberToVerticalScroll( LineNumber line ) const;
    double verticalScrollMultiplicator() const;

    void drawTextArea( QPaintDevice* paintDevice );
    QPixmap drawPullToFollowBar( int width, qreal pixelRatio );

    void disableFollow();

    // Utils functions
    void updateGlobalSelection();

    void selectAndDisplayRange( FilePosition pos );
    bool shouldBottomAlignFrame() const;
};

#endif
