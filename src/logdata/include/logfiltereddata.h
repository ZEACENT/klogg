/*
 * Copyright (C) 2009, 2010, 2011, 2012, 2017 Nicolas Bonnefon and other contributors
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

#ifndef LOGFILTEREDDATA_H
#define LOGFILTEREDDATA_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>

#include <QByteArray>
#include <QList>
#include <QFutureWatcher>
#include <QObject>
#include <QStringList>

#include "abstractlogdata.h"
#include "coalescingtimer.h"
#include "hsregularexpression.h"
#include "linetypes.h"
#include "logfiltereddataworker.h"
#include "reviewmarks.h"
#include "searchpreparation.h"

class SearchableLogData;

// A list of matches found in a LogData, it stores all the matching lines,
// which can be accessed using the AbstractLogData interface, together with
// the original line number where they were found.
// Constructing such objet does not start the search.
// This object should be constructed by a LogData.
class LogFilteredData : public AbstractLogData, public klogg::ReviewMarksOwner {
    Q_OBJECT

  public:
    template <class T>
    struct access_by;

    // Constructor used by LogData
    explicit LogFilteredData( const SearchableLogData* logData );
    ~LogFilteredData();

    // Starts an async search, sending newDataAvailable() when new data is found.
    // Replacement work is dispatched off the owner thread after the prior
    // operation stops. Retirement permanently closes this admission path.
    void runSearch( const RegularExpressionPattern& regExp, LineNumber startLine,
                    LineNumber endLine );
    // Shortcut for runSearch on all file
    void runSearch( const RegularExpressionPattern& regExp );

    // Add to the existing search, starting at the line when the search was
    // last stopped. Used when the file on disk has been added too.
    void updateSearch( LineNumber startLine, LineNumber endLine );
    // Interrupt the running search if one is in progress.
    // Nothing is done if no search is in progress.
    void interruptSearch();
    // Close search admission and retire workers asynchronously. The source must
    // outlive this model; completion includes its matching reader detach.
    QFuture<void> retire();

#ifdef KLOGG_TESTS
    // Test seam: true while a throttled publication (results or status) is
    // still armed. Tests drain the event loop until this is false instead of
    // waiting out a fixed idle window after each search.
    bool hasPendingPublicationsForTest() const
    {
        return pendingSearchResults_.has_value() || pendingSearchStatus_.has_value()
               || searchResultsRefreshTimer_.isPending()
               || searchStatusRefreshTimer_.isPending();
    }
#endif
    // Clear the search and results, invalidating queued publications from the
    // abandoned logical search.
    void clearSearch( bool dropCache = false );
    void setAllLinesVisible( bool visible );
    bool allLinesVisible() const { return allLinesVisible_; }

    // Returns the line number in the original LogData where the element
    // 'index' was found.
    LineNumber getMatchingLineNumber( LineNumber index ) const;
    // Returns the line 'index' in filterd log data that matches
    // given original line number
    LineNumber getLineIndexNumber( LineNumber lineNumber ) const;

    // Returns the number of lines in the source log data
    LinesCount getNbTotalLines() const;
    // Returns the number of matches (independently of the visibility)
    LinesCount getNbMatches() const;
    // Returns the number of marks (independently of the visibility)
    LinesCount getNbMarks() const;

    // Counts the visible overview categories in the half-open source-line range
    // [first, end). A line that is both matched and marked is counted as a
    // match when matches are visible; marks retain it when matches are hidden.
    // Plain lines mirrored by allLinesVisible are never counted.
    struct LineTypeRangeCounts {
        LinesCount matches;
        LinesCount marks;
    };
    LineTypeRangeCounts countLineTypesInRange( LineNumber first, LineNumber end ) const;

    LineType lineTypeByIndex( LineNumber index ) const;
    LineType lineTypeByLine( LineNumber lineNumber ) const;

    struct MarksSnapshot {
        uint64_t revision = 0;
        QList<LineNumber> marks;
    };

    // Owner-thread value read of this filtered model's stored marks, independent
    // of result visibility. Indices refer to its source model, NOT stable row
    // identities or evidence addresses. The owner-local revision advances only
    // on stored mark changes. No search/source/presentation mutation.
    MarksSnapshot marksSnapshot() const;
    QObject* reviewMarksObject() override { return this; }
    uint64_t reviewMarksRevision() const override { return marksRevision_; }
    std::optional<klogg::ReviewMarksSnapshot> reviewMarksSnapshot() const override;
    std::optional<klogg::ReviewMarkLocation> resolveReviewMark( const klogg::LogRowId& identity ) const override;
    klogg::ReviewMarksPreparer prepareReviewMarks(
        const klogg::ReviewMarksSnapshot& candidate, klogg::ReviewMarksBudget& budget ) override;

    // Private replacement preparation never reads mutable model state on workers.
    // Acquire every owner before installation; guards must not span event-loop turns.
    static constexpr size_t MaxPreparedContextRows = 256 * 1024;
    static constexpr size_t MaxPreparedBitmapBytes = 8 * 1024 * 1024;
    struct MarksUpdateCapture;
    struct PreparedMarksUpdate;
    class MarksUpdatePublication {
      public:
        ~MarksUpdatePublication();
        bool install() noexcept;
      private:
        friend class LogFilteredData;
        struct State;
        explicit MarksUpdatePublication( std::unique_ptr<State> state );
        std::unique_ptr<State> state_;
    };
    std::shared_ptr<const MarksUpdateCapture> captureMarksUpdate( const QList<LineNumber>& candidate ) const;
    static std::shared_ptr<PreparedMarksUpdate> prepareMarksUpdate(
        const std::shared_ptr<const MarksUpdateCapture>& captured );
    std::unique_ptr<MarksUpdatePublication> lockMarksUpdate(
        const std::shared_ptr<PreparedMarksUpdate>& prepared );

    // Capture on the owner, prepare on a worker, then acquire all owners without
    // yielding. Install all results, release every guard, and only then notify.
    // This model boundary does not publish UI query drafts or folder search rows.
    struct SearchUpdateCapture;
    struct PreparedSearchUpdate;
    struct SearchUpdatePreparation {
        SearchPreparation::Status status = SearchPreparation::Status::InvalidInput;
        std::shared_ptr<PreparedSearchUpdate> update;
    };
    class SearchUpdatePublication {
      public:
        ~SearchUpdatePublication();
        bool install();
        bool isCurrent() const;
        void release();
        void notify();
      private:
        friend class LogFilteredData;
        struct State;
        explicit SearchUpdatePublication( std::unique_ptr<State> state );
        std::unique_ptr<State> state_;
    };
    std::shared_ptr<const SearchUpdateCapture> captureSearchUpdate(
        const RegularExpressionPattern& pattern, LineNumber startLine = 0_lnum,
        std::optional<LineNumber> endLine = std::nullopt ) const;
    static SearchUpdatePreparation prepareSearchUpdate(
        const std::shared_ptr<const SearchUpdateCapture>& captured, AtomicFlag& cancelled );
    std::unique_ptr<SearchUpdatePublication> lockSearchUpdate(
        const std::shared_ptr<PreparedSearchUpdate>& prepared );

    // Marks interface (delegated to a Marks object)

    // Add a mark at the given line
    void addMark( LineNumber line );
    // Get the first mark after the line passed
    OptionalLineNumber getMarkAfter( LineNumber line ) const;
    // Get the first mark before the line passed
    OptionalLineNumber getMarkBefore( LineNumber line ) const;
    // Delete the mark present on the passed line
    void deleteMark( LineNumber line );
    // Toggle presence of the mark on the passed line.
    void toggleMark( LineNumber line );
    // Completely clear the marks list.
    void clearMarks();
    // Get all marked lines
    QList<LineNumber> getMarks() const;

    // Changes what the AbstractLogData returns via its getXLines/getNbLines
    // API.
    enum class VisibilityFlags {
        None = static_cast<LineType::Int>( LineTypeFlags::Plain ), // this is for internal use
        Matches = static_cast<LineType::Int>( LineTypeFlags::Match ),
        Marks = static_cast<LineType::Int>( LineTypeFlags::Mark ),
    };
    Q_ENUM( VisibilityFlags );
    Q_DECLARE_FLAGS( Visibility, VisibilityFlags )
    void setVisibility( Visibility visibility );
    Visibility visibility() const;
    // Native query submission can switch a marks-only pane back to matches.
    // Prepare that projection with the query, not as a later visible edit.
    std::shared_ptr<const SearchUpdateCapture> captureSearchUpdate(
        const RegularExpressionPattern& pattern, Visibility resultVisibility,
        LineNumber startLine = 0_lnum,
        std::optional<LineNumber> endLine = std::nullopt ) const;

    // Empty-query policy is a reset, not a regex matching every source row.
    std::shared_ptr<const SearchUpdateCapture> captureSearchReset(
        bool allLinesVisible, Visibility resultVisibility ) const;

    // Context lines support (similar to grep -A -B -C)
    void setContextLines( int before, int after );
    int contextLinesBefore() const { return contextLinesBefore_; }
    int contextLinesAfter() const { return contextLinesAfter_; }

    void iterateOverLines( const std::function<void( LineNumber )>& callback ) const;

    // Alias kept for callers that prefer a self-documenting name; signals
    // use plain quint64 (see LogFilteredDataWorker::OperationGeneration for
    // the rationale).
    using SearchGeneration = LogFilteredDataWorker::OperationGeneration;

    // Generation of the search currently active. Only a new logical query
    // advances this counter; incremental updates retain it. This
    // accessor returns that value.  Receivers of searchProgressed can compare
    // the signal-supplied generation against this value to drop stale signals
    // that were queued before the search was replaced.
    SearchGeneration currentSearchGeneration() const { return workerThread_.currentGeneration(); }

    // Readiness belongs to the current operation/cutoff, not merely its query generation.
    bool isSearchComplete() const;
    // Owner-thread value observation of this model's query, including kept
    // results. Does not capture source content or consult a view's paint state.
    RegularExpressionPattern appliedSearchPattern() const { return currentRegExp_; }
    CompletedSearchResult::Ptr completedSearchResult() const;
    klogg::LogSnapshot::Ptr createReadSnapshot( klogg::SnapshotScope scope ) const override;
    OptionalLineNumber rowForIdentity( const klogg::LogRowId& identity ) const override;
    bool containsRowIdentity( const klogg::LogRowId& identity ) const override;
    SearchPerformanceCounters searchPerformanceCounters() const
    {
        return workerThread_.performanceCounters();
    }

    // Advance the generation counter without launching a search.  Called by
    // CrawlerWidget::replaceCurrentSearch (and any other "abandon all
    // in-flight progress signals from the previous search outright"
    // pathway) so that queued metacalls still on the receiver's event queue
    // are recognised as stale and dropped.  Does NOT advance for the
    // Stop-button pathway -- that one wants the final progress signal to
    // reach the receiver and trigger UI cleanup.
    void bumpSearchGeneration();

  Q_SIGNALS:
    // Published after a complete mark edit, with any review guards released.
    void marksChanged();
    // Sent when the search has progressed, give the number of matches (so far)
    // and the percentage of completion.  The generation identifies which
    // runSearch() / updateSearch() call produced this signal -- see
    // currentSearchGeneration().
    void searchProgressed( LinesCount nbMatches, int progress, LineNumber initialLine,
                           quint64 generation );
    void searchResultsChanged( LinesCount nbMatches, LineNumber initialLine, bool terminal,
                               quint64 generation );

  private Q_SLOTS:
    void handleSearchProgressed( LinesCount nbMatches, int progress, LineNumber initialLine,
                                 quint64 generation );

  private:
    std::shared_ptr<SearchUpdateCapture> captureSearchUpdateState() const;
    bool isSearchUpdateCurrent( const PreparedSearchUpdate& prepared ) const;
    void attachReaderIfNeeded() const;
    void detachReaderIfNeeded() const;

    // Implementation of virtual functions
    QString doGetLineString( LineNumber line ) const override;
    QString doGetExpandedLineString( LineNumber line ) const override;
    klogg::vector<AnsiColorSpan> doGetLineAnsiColors( LineNumber line ) const override;
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount number ) const override;
    klogg::vector<QString> doGetExpandedLines( LineNumber first, LinesCount number ) const override;
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount number,
                                     const std::function<QString( LineNumber )>& lineGetter ) const;
    LineNumber doGetLineNumber( LineNumber index ) const override;
    LinesCount doGetNbLine() const override;
    LineLength doGetMaxLength() const override;
    LineLength doGetLineLength( LineNumber line ) const override;

    void doSetDisplayEncoding( const char* encoding ) override;
    QTextCodec* doGetDisplayEncoding() const override;

    void doAttachReader() const override;
    void doDetachReader() const override;

    // Insert new mark into filteredItemsCache_.
    void updateCacheWithMark( uint32_t index, LineNumber line );

    // Returns whether the line number passed is in our list of matching ones.
    bool isLineMatched( LineNumber lineNumber ) const;
    // Returns wheither the passed line has a mark on it.
    bool isLineMarked( LineNumber line ) const;

    // Build list of lines including context lines
    // Returns a vector of line numbers in display order (with context lines)
    void rebuildContextLinesList() const;
    static klogg::vector<LineNumber> buildContextLines(
        const SearchResultArray& matches, const SearchResultArray& marks, Visibility visibility,
        int before, int after, LinesCount totalLines );

    // List of the matching line numbers
    SearchResultArray matching_lines_;
    SearchResultArray marks_;
    uint64_t marksRevision_ = 0;
    bool marksPublicationLocked_ = false;
    class ReviewMarksUpdate;
    SearchResultArray marks_and_matches_;
    bool allLinesVisible_ = false;

    const SearchableLogData* sourceLogData_;

    RegularExpressionPattern currentRegExp_;
    CompletedSearchResult::Ptr completedResult_;
    mutable SearchableLogData::ReadSnapshot::Ptr presentedSource_;
    mutable bool completedSourceInvalid_ = false;
    void setPresentedSource( SearchableLogData::ReadSnapshot::Ptr source ) const;
    void refreshPresentedSourceForDisplay();
    SearchResultArray presentedRows( const SearchableLogData::ReadSnapshot& source ) const;
    LineLength maxLength_;
    // Width decoding is presentation work, not mark-membership mutation.
    // Track only unmeasured additions; removing the measured maximum invalidates
    // the cache once rather than rescanning on every edit in a batch.
    mutable std::optional<LineLength> maxLengthMarks_{ 0_length };
    mutable OptionalLineNumber longestMark_;
    mutable SearchResultArray unmeasuredMarks_;
    // Number of lines of the LogData that has been searched for:
    LinesCount nbLinesProcessed_;

    Visibility visibility_;

    // Context lines configuration
    int contextLinesBefore_ = 0;
    int contextLinesAfter_ = 0;

    // Cached list of line numbers including context lines (mutable for lazy evaluation)
    mutable klogg::vector<LineNumber> contextLinesList_;
    mutable bool contextLinesListValid_ = false;
    // Widths can require decoding; row membership/count queries must not.
    mutable std::optional<LineLength> maxLengthContext_;

    struct SearchResultsPayload {
        LinesCount nbMatches = 0_lcount;
        LineNumber initialLine = 0_lnum;
        bool terminal = false;
        quint64 generation = 0;
    };
    struct SearchStatusPayload {
        LinesCount nbMatches = 0_lcount;
        int progress = 0;
        LineNumber initialLine = 0_lnum;
        quint64 generation = 0;
    };

    void cancelPendingPublications();
    void finishRetirement();
    void publishPendingSearchResults();
    void publishPendingSearchStatus();
    void publishTerminal( LinesCount nbMatches, LineNumber initialLine, quint64 generation );

    std::optional<SearchResultsPayload> pendingSearchResults_;
    std::optional<SearchStatusPayload> pendingSearchStatus_;
    klogg::CoalescingTimer searchResultsRefreshTimer_;
    klogg::CoalescingTimer searchStatusRefreshTimer_;

    LogFilteredDataWorker workerThread_;
    mutable bool readerAttached_{ false };
    mutable bool shuttingDown_{ false };
    QFutureWatcher<void> retirementWatcher_;
    QFutureInterface<void> retirementCompletion_;

  private:
    struct CachedSearchResult {
        SearchResultArray matching_lines;
        LineLength maxLength;
        CompletedSearchResult::Ptr completed;
    };

    using SearchCacheKey = std::tuple<RegularExpressionPattern, LineNumber::UnderlyingType,
                                      LineNumber::UnderlyingType>;
    struct SearchCacheKeyHash {
        template <class T>
        void hash_combine( std::size_t& seed, const T& v ) const
        {
            seed ^= std::hash<T>()( v ) + 0x9e3779b9 + ( seed << 6 ) + ( seed >> 2 );
        }
        std::size_t operator()( const SearchCacheKey& k ) const
        {
            size_t seed = qHash( std::get<0>( k ).pattern );

            hash_combine( seed, std::get<0>( k ).isPlainText );
            hash_combine( seed, std::get<0>( k ).isBoolean );
            hash_combine( seed, std::get<0>( k ).isCaseSensitive );
            hash_combine( seed, std::get<0>( k ).isExclude );
            hash_combine( seed, std::get<1>( k ) );
            hash_combine( seed, std::get<2>( k ) );
            return seed;
        }
    };

    std::unordered_map<SearchCacheKey, CachedSearchResult, SearchCacheKeyHash> searchResultsCache_;
    SearchCacheKey currentSearchKey_;

    SearchCacheKey makeCacheKey( const RegularExpressionPattern& regExp, LineNumber startLine,
                                 LineNumber endLine )
    {
        return std::make_tuple( regExp, startLine.get(), endLine.get() );
    }

    void updateSearchResultsCache();

    inline LineNumber getExpectedSearchEnd( const SearchCacheKey& cacheKey ) const
    {
        return LineNumber( std::get<2>( cacheKey ) );
    }

    // Utility functions
    const SearchResultArray& currentResultArray() const;
    LineNumber findLogDataLine( LineNumber lineNum ) const;
    LineNumber findFilteredLine( LineNumber lineNum ) const;

    void updateMarksProjection( OptionalLineNumber addedLine, OptionalLineNumber removedLine );
    LineLength marksMaxLength() const;
};

Q_DECLARE_OPERATORS_FOR_FLAGS( LogFilteredData::Visibility )
Q_DECLARE_METATYPE( LogFilteredData::Visibility )

#endif
