/*
 * Copyright (C) 2010, 2013 Nicolas Bonnefon and other contributors
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

// This file implements QuickFind.
// This class implements the Quick Find mechanism using references
// to the logData, the QFP and the selection passed.
// Search is started just after the selection and the selection is updated
// if a match is found.

#if defined( KLOGG_ASAN_BUILD )
#include <thread>
#endif

#include <QMetaObject>
#include <QtConcurrent>

#include <exception>

#include "abstractlogdata.h"
#include "linetypes.h"
#include "log.h"
#include "quickfindpattern.h"
#include "selection.h"

#include "quickfind.h"
#include "matchprojection.h"

namespace {
LineNumber presentedRow( const klogg::LogSnapshot& snapshot, const klogg::SnapshotRow& row )
{
    const auto presented = snapshot.presentedRowForIdentity( row.identity );
    if ( !presented ) {
        throw klogg::SnapshotError{ "Quick Find snapshot has no frozen presented-row mapping" };
    }
    return *presented;
}

// PresentedRows are ordered in view order, but synthetic headers can leave gaps.
// Locate the starting snapshot ordinal without scanning every preceding row.
LineNumber firstRowAtOrAfter( const klogg::LogSnapshot& snapshot, LineNumber position )
{
    uint64_t first = 0;
    auto last = snapshot.lineCount().get();
    while ( first < last ) {
        const auto middle = first + ( last - first ) / 2;
        if ( presentedRow( snapshot, snapshot.row( LineNumber{ middle } ) ) < position ) {
            first = middle + 1;
        }
        else {
            last = middle;
        }
    }
    return LineNumber{ first };
}

QuickFindResult occurrenceResult( LineNumber ordinal, const klogg::SnapshotRow& row,
                                  LineNumber line, const QString& text,
                                  const QRegularExpressionMatch& match )
{
    QuickFindResult result;
    result.found = true;
    result.line = line;
    result.rowIdentity = row.identity;
    const klogg::MatchTextColumns columns{ text };
    const auto occurrence = klogg::projectMatchOccurrence( match );
    std::vector<Selection::CapturedPortion> fragments;
    for ( const auto& fragment : occurrence.fragments ) {
        const auto end = fragment.start + fragment.length;
        fragments.push_back( {
            Portion{ ordinal, columns.displayColumn( fragment.start ),
                     columns.displayColumn( end ) - 1_length,
                     fragment.start, end - 1_length },
            row.sourceLineNumber, text.mid( fragment.start.get(), fragment.length.get() ),
            row.identity, line } );
    }
    result.selection.selectCapturedPortions( std::move( fragments ) );
    auto nextRaw = match.capturedEnd();
    FilePosition next{ line, columns.displayColumn( LineColumn{ nextRaw } ) };
    if ( match.capturedLength() == 0 ) {
        if ( nextRaw == text.size() ) {
            next = FilePosition{ line + 1_lcount, 0_lcol };
        }
        else {
            const bool surrogate = text.at( nextRaw ).isHighSurrogate()
                                   && nextRaw + 1 < text.size()
                                   && text.at( nextRaw + 1 ).isLowSurrogate();
            nextRaw += surrogate ? 2 : 1;
            next = FilePosition{ line, columns.displayColumn( LineColumn{ nextRaw } ) };
        }
    }
    result.selection.setNavigationBounds(
        FilePosition{ line, columns.displayColumn( occurrence.start ) }, next, row.identity );
    return result;
}
} // namespace

void SearchingNotifier::reset()
{
    dotToDisplay_ = 0;
    startTime_ = QTime::currentTime();
}

void SearchingNotifier::sendNotification( LineNumber current_line, LinesCount nb_lines,
                                          bool backward )
{
    LOG_DEBUG << "Emitting Searching....";
    const auto progress = static_cast<int>(
        backward ? ( ( nb_lines.get() - current_line.get() ) / nb_lines.get() * 100 )
                 : ( current_line.get() / nb_lines.get() ) * 100 );

    Q_EMIT notify( QFNotificationProgress( progress ) );
    startTime_ = QTime::currentTime().addMSecs( -800 );
}

void QuickFind::LastMatchPosition::set( LineNumber line, LineColumn column )
{
    if ( ( !line_.has_value() ) || ( ( line <= *line_ ) && ( column < column_ ) ) ) {
        line_ = line;
        column_ = column;
    }
}

void QuickFind::LastMatchPosition::set( const FilePosition& position )
{
    set( position.line(), position.column() );
}

bool QuickFind::LastMatchPosition::isLater( OptionalLineNumber line, LineColumn column ) const
{
    if ( !line_.has_value() || !line.has_value() )
        return false;
    else if ( ( *line == *line_ ) && ( column >= column_ ) )
        return true;
    else if ( *line > *line_ )
        return true;
    else
        return false;
}

bool QuickFind::LastMatchPosition::isLater( const FilePosition& position ) const
{
    return isLater( position.line(), position.column() );
}

bool QuickFind::LastMatchPosition::isSooner( OptionalLineNumber line, LineColumn column ) const
{
    if ( !line_.has_value() || !line.has_value() )
        return false;
    else if ( ( *line == *line_ ) && ( column <= column_ ) )
        return true;
    else if ( *line < *line_ )
        return true;
    else
        return false;
}

bool QuickFind::LastMatchPosition::isSooner( const FilePosition& position ) const
{
    return isSooner( position.line(), position.column() );
}

QuickFind::QuickFind( const AbstractLogData& logData )
    : logData_( logData )
    , searchingNotifier_()
    , incrementalSearchStatus_()
{
    connect( &searchingNotifier_, &SearchingNotifier::notify, this, &QuickFind::sendNotification,
             Qt::DirectConnection );

    qRegisterMetaType<QuickFindResult>();
    connect( &operationWatcher_, &QFutureWatcher<QuickFindResult>::finished, this,
             &QuickFind::onSearchFutureReady );
}

Selection QuickFind::incrementalSearchStop()
{
    if ( incrementalSearchStatus_.isOngoing() ) {
        Selection s = incrementalSearchStatus_.initialSelection();
        incrementalSearchStatus_ = IncrementalSearchStatus();
        ++requestId_;
        interruptRequested_.set();

        return s;
    }
    else {
        return Selection{};
    }
}

Selection QuickFind::incrementalSearchAbort()
{
    if ( incrementalSearchStatus_.isOngoing() ) {
        Selection s = incrementalSearchStatus_.initialSelection();
        incrementalSearchStatus_ = IncrementalSearchStatus();
        ++requestId_;
        interruptRequested_.set();
        return s;
    }
    else {
        return Selection{};
    }
}

void QuickFind::interruptAndWait()
{
    interruptRequested_.set();
    operationWatcher_.waitForFinished();
}

#if defined( KLOGG_ASAN_BUILD )
void QuickFind::pauseBeforeLineReadForTesting( std::atomic<bool>& entered )
{
    interruptAndWait();
    beforeLineReadCallbackForTesting_ = [ &entered ]( const AtomicFlag& interruptRequested ) {
        entered.store( true, std::memory_order_release );
        while ( !interruptRequested ) {
            std::this_thread::yield();
        }
    };
}

void QuickFind::runBeforeLineReadCallbackForTesting()
{
    if ( beforeLineReadCallbackForTesting_ ) {
        auto callback = std::move( beforeLineReadCallbackForTesting_ );
        beforeLineReadCallbackForTesting_ = {};
        callback( interruptRequested_ );
    }
}
#endif

void QuickFind::stopSearch()
{
    LOG_INFO << "Stop search for quickfind " << this;
    ++requestId_;
    interruptAndWait();
}

void QuickFind::onSearchFutureReady()
{
    auto result = operationFuture_.result();
    if ( result.requestId != requestId_ ) {
        return;
    }
    if ( !result.found && incrementalSearchStatus_.direction() != None ) {
        result.line = incrementalSearchStatus_.position().line();
        result.selection.selectPortion( *result.line, 0_lcol, 0_lcol );
    }
    Q_EMIT searchDone( result );
}

void QuickFind::incrementallySearchForward( Selection selection, QuickFindMatcher matcher )
{
    LOG_DEBUG << "QuickFind::incrementallySearchForward";

    interruptAndWait();

    // Position where we start the search from
    FilePosition start_position = selection.getNextPosition();

    if ( incrementalSearchStatus_.direction() == Forward ) {
        // An incremental search is active, we restart the search
        // from the initial point
        LOG_DEBUG << "Restart search from initial point";
        start_position = incrementalSearchStatus_.position();
    }
    else {
        // It's a new search so we search from the selection
        incrementalSearchStatus_ = IncrementalSearchStatus( Forward, start_position, selection );
    }

    startSearch( Forward, start_position, selection, matcher );
}

void QuickFind::incrementallySearchBackward( Selection selection, QuickFindMatcher matcher )
{
    LOG_DEBUG << "QuickFind::incrementallySearchBackward";

    interruptAndWait();

    // Position where we start the search from
    FilePosition start_position = selection.getPreviousPosition();

    if ( incrementalSearchStatus_.direction() == Backward ) {
        // An incremental search is active, we restart the search
        // from the initial point
        LOG_DEBUG << "Restart search from initial point";
        start_position = incrementalSearchStatus_.position();
    }
    else {
        // It's a new search so we search from the selection
        incrementalSearchStatus_ = IncrementalSearchStatus( Backward, start_position, selection );
    }

    startSearch( Backward, start_position, selection, matcher );
}

void QuickFind::searchForward( Selection selection, QuickFindMatcher matcher )
{
    incrementalSearchStatus_ = IncrementalSearchStatus();
    interruptAndWait();

    startSearch( Forward, selection.getNextPosition(), selection, matcher );
}

void QuickFind::searchBackward( Selection selection, QuickFindMatcher matcher )
{
    incrementalSearchStatus_ = IncrementalSearchStatus();
    interruptAndWait();

    startSearch( Backward, selection.getPreviousPosition(), selection, matcher );
}

void QuickFind::startSearch( QFDirection direction, const FilePosition& startPosition,
                            const Selection& selection, const QuickFindMatcher& matcher )
{
    const auto requestId = ++requestId_;
    klogg::LogSnapshot::Ptr snapshot;
    // This is the only source access: acceptance captures both immutable text
    // and presented-row membership on the source owner's thread.
    try {
        snapshot = logData_.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
        if ( !snapshot ) {
            throw klogg::SnapshotError{ "The source cannot provide an immutable snapshot" };
        }
    }
    catch ( const std::exception& error ) {
        sendNotification( QFNotification{ tr( "Quick Find unavailable: %1" )
                                              .arg( QString::fromUtf8( error.what() ) ) } );
    }
    catch ( ... ) {
        sendNotification( QFNotification{ tr( "Quick Find snapshot acquisition failed" ) } );
    }

    interruptRequested_.clear();
    operationFuture_ = QtConcurrent::run(
        [ this, snapshot = std::move( snapshot ), startPosition, selection = selection,
          matcher = matcher, direction, requestId ] {
            QuickFindResult result;
            try {
                if ( snapshot ) {
                    result = direction == Forward
                        ? doSearchForward( *snapshot, startPosition, selection, matcher )
                        : doSearchBackward( *snapshot, startPosition, selection, matcher );
                }
            }
            catch ( const std::exception& error ) {
                sendNotification( QFNotification{ tr( "Quick Find failed: %1" )
                                                      .arg( QString::fromUtf8( error.what() ) ) } );
            }
            catch ( ... ) {
                sendNotification( QFNotification{ tr( "Quick Find snapshot read failed" ) } );
            }
            result.requestId = requestId;
            return result;
        } );
    operationWatcher_.setFuture( operationFuture_ );
}

QuickFindResult QuickFind::doSearchForward( const klogg::LogSnapshot& snapshot,
                                          const FilePosition& startPosition,
                                          const Selection& selection,
                                          const QuickFindMatcher& matcher )
{
    if ( !matcher.isActive() ) return {};

#if defined( KLOGG_ASAN_BUILD )
    runBeforeLineReadCallbackForTesting();
#endif

    if ( lastMatch_.isLater( startPosition ) ) {
        sendNotification( QFNotificationReachedEndOfFile{} );
        return {};
    }

    const auto count = snapshot.lineCount();
    auto ordinal = firstRowAtOrAfter( snapshot, startPosition.line() );
    searchingNotifier_.reset();
    while ( ordinal < count && !interruptRequested_ ) {
        const auto row = snapshot.row( ordinal );
        const auto line = presentedRow( snapshot, row );
        if ( row.copyable ) {
            const auto text = snapshot.lineString( ordinal );
            const klogg::MatchTextColumns columns{ text };
            const auto start = line == startPosition.line()
                                   ? columns.searchColumn( startPosition.column() ) : 0_lcol;
            if ( matcher.isLineMatching( text, start ) ) {
                Q_EMIT clearNotification();
                return occurrenceResult( ordinal, row, line, text, matcher.lastOccurrence() );
            }
        }
        ++ordinal;
        searchingNotifier_.ping( ordinal, count, false );
    }

    if ( !interruptRequested_ ) {
        lastMatch_.set( selection.getPreviousPosition() );
        sendNotification( QFNotificationReachedEndOfFile{} );
    }
    else {
        sendNotification( QFNotificationInterrupted{} );
    }
    return {};
}

QuickFindResult QuickFind::doSearchBackward( const klogg::LogSnapshot& snapshot,
                                           const FilePosition& startPosition,
                                           const Selection& selection,
                                           const QuickFindMatcher& matcher )
{
    if ( !matcher.isActive() ) return {};

#if defined( KLOGG_ASAN_BUILD )
    runBeforeLineReadCallbackForTesting();
#endif

    if ( firstMatch_.isSooner( startPosition ) ) {
        sendNotification( QFNotificationReachedBegininningOfFile{} );
        return {};
    }

    const auto count = snapshot.lineCount();
    if ( count == 0_lcount ) return {};
    auto next = firstRowAtOrAfter( snapshot, startPosition.line() );
    if ( next < count && presentedRow( snapshot, snapshot.row( next ) ) == startPosition.line() ) {
        ++next;
    }
    searchingNotifier_.reset();
    while ( next > 0_lnum && !interruptRequested_ ) {
        const auto ordinal = --next;
        const auto row = snapshot.row( ordinal );
        const auto line = presentedRow( snapshot, row );
        if ( row.copyable ) {
            const auto text = snapshot.lineString( ordinal );
            const klogg::MatchTextColumns columns{ text };
            const auto boundary = line == startPosition.line()
                                      ? columns.searchColumn( startPosition.column() )
                                      : LineColumn{ -1 };
            if ( matcher.isLineMatchingBackward( text, boundary ) ) {
                Q_EMIT clearNotification();
                return occurrenceResult( ordinal, row, line, text, matcher.lastOccurrence() );
            }
        }
        searchingNotifier_.ping( ordinal, count, true );
    }

    if ( !interruptRequested_ ) {
        firstMatch_.set( selection.getNextPosition() );
        sendNotification( QFNotificationReachedBegininningOfFile{} );
    }
    else {
        sendNotification( QFNotificationInterrupted{} );
    }
    return {};
}

void QuickFind::resetLimits()
{
    lastMatch_.reset();
    firstMatch_.reset();
}

void QuickFind::sendNotification( QFNotification notification )
{
    QMetaObject::invokeMethod(
        this, [ this, notification ] { Q_EMIT notify( notification ); }, Qt::QueuedConnection );
}
