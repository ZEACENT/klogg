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

// This file implements Selection.
// Selection modes are mutually exclusive. Batch portions discard invalid
// coordinates; callers remain responsible for bounds against the source data.

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

#include "abstractlogdata.h"
#include "containers.h"
#include "linetypes.h"
#include "log.h"
#include "platform/platform_clipboard.h"
#include "selection.h"

Selection::Selection()
{
    selectedPartial_.startColumn = 0_lcol;
    selectedPartial_.endColumn = 0_lcol;

    selectedRange_.endLine = 0_lnum;
}

void Selection::selectPortion( LineNumber line, LineColumn startColumn, LineColumn endColumn )
{
    clear();

    selectedPartial_.line = line;
    selectedPartial_.startColumn = std::min( startColumn, endColumn );
    selectedPartial_.endColumn = std::max( startColumn, endColumn );
}

void Selection::selectPortions( std::vector<Portion> portions, PortionOverlap overlap )
{
    clear();
    portions.erase( std::remove_if( portions.begin(), portions.end(),
                                    []( const Portion& portion ) { return !portion.isValid(); } ),
                    portions.end() );
    const auto rowStartLess = []( const Portion& lhs, const Portion& rhs ) {
        return lhs.line() < rhs.line()
               || ( lhs.line() == rhs.line() && lhs.startColumn() < rhs.startColumn() );
    };
    if ( !std::is_sorted( portions.begin(), portions.end(), rowStartLess ) ) {
        std::stable_sort( portions.begin(), portions.end(), rowStartLess );
    }

    std::vector<SelectedPortion> selectedPortions;
    selectedPortions.reserve( portions.size() );
    for ( const auto& portion : portions ) {
        if ( overlap == PortionOverlap::Merge && !selectedPortions.empty() ) {
            const auto& previous = selectedPortions.back().portion;
            const auto& raw = portion.rawColumns();
            const auto& previousRaw = previous.rawColumns();
            // Mixed coordinate conventions cannot be merged without source text.
            const bool compatible = raw.has_value() == previousRaw.has_value()
                                    && ( !raw || ( raw->first <= previousRaw->second
                                                  && previousRaw->first <= raw->second ) );
            if ( previous.line() == portion.line()
                 && portion.startColumn() <= previous.endColumn() && compatible ) {
                const auto endColumn = std::max( previous.endColumn(), portion.endColumn() );
                const auto merged = raw
                    ? Portion( portion.line(), previous.startColumn(), endColumn,
                               std::min( previousRaw->first, raw->first ),
                               std::max( previousRaw->second, raw->second ) )
                    : Portion( portion.line(), previous.startColumn(), endColumn );
                selectedPortions.back().portion = merged;
                continue;
            }
        }
        selectedPortions.push_back( { portion, std::nullopt, std::nullopt } );
    }
    std::vector<OptionalLineNumber> rows;
    rows.reserve( selectedPortions.size() );
    for ( const auto& selected : selectedPortions ) {
        rows.push_back( selected.portion.line() );
    }
    setPortions( std::move( selectedPortions ), std::move( rows ) );
}

void Selection::selectCapturedPortions( std::vector<CapturedPortion> portions )
{
    portions.erase( std::remove_if( portions.begin(), portions.end(),
                                    []( const CapturedPortion& captured ) {
                                        return !captured.portion.isValid() || captured.text.isEmpty();
                                    } ),
                    portions.end() );
    const auto rowStartLess = []( const CapturedPortion& lhs, const CapturedPortion& rhs ) {
        return lhs.portion.line() < rhs.portion.line()
               || ( lhs.portion.line() == rhs.portion.line()
                    && lhs.portion.startColumn() < rhs.portion.startColumn() );
    };
    if ( !std::is_sorted( portions.begin(), portions.end(), rowStartLess ) ) {
        std::stable_sort( portions.begin(), portions.end(), rowStartLess );
    }

    // Prepare the complete owned batch and display index before replacing selection.
    std::vector<SelectedPortion> selectedPortions;
    std::vector<OptionalLineNumber> rows;
    selectedPortions.reserve( portions.size() );
    rows.reserve( portions.size() );
    for ( auto& captured : portions ) {
        // QString copies may still borrow storage when constructed with fromRawData.
        captured.text.detach();
        selectedPortions.push_back(
            { captured.portion,
              std::make_pair( captured.sourceLineNumber, std::move( captured.text ) ),
              captured.identity } );
        rows.push_back( captured.identity ? captured.presentedRow : captured.portion.line() );
    }
    Selection capturedSelection;
    capturedSelection.setPortions( std::move( selectedPortions ), std::move( rows ) );
    *this = std::move( capturedSelection );
}

void Selection::remapCapturedRows(
    const std::function<OptionalLineNumber( const klogg::LogRowId& )>& resolveRow )
{
    // Complete resolution before changing any geometry: failures must leave the
    // previous index and occurrence anchors coherent. Repeated fragments resolve once.
    std::map<klogg::LogRowId, OptionalLineNumber> resolvedRows;
    const auto resolveIdentity = [&]( const klogg::LogRowId& identity ) {
        const auto [resolved, inserted] = resolvedRows.try_emplace( identity );
        if ( inserted ) {
            resolved->second = resolveRow( identity );
        }
        return resolved->second;
    };
    std::vector<OptionalLineNumber> projectedRows;
    const auto& entries = portions();
    projectedRows.reserve( entries.size() );
    bool changed = false;
    for ( std::size_t index = 0; index < entries.size(); ++index ) {
        const auto& selected = entries[ index ];
        auto row = layout().rows[ index ];
        if ( selected.identity ) {
            row = resolveIdentity( *selected.identity );
            changed = changed || layout().rows[ index ] != row;
        }
        projectedRows.push_back( row );
    }
    auto navigation = navigationBounds_;
    if ( navigation && navigation->identity ) {
        navigation->presentedRow = resolveIdentity( *navigation->identity );
        if ( navigation->presentedRow ) {
            const auto nextRowDelta = navigation->next.line() - navigation->previous.line();
            navigation->previous = FilePosition( *navigation->presentedRow,
                                                  navigation->previous.column() );
            navigation->next = FilePosition( *navigation->presentedRow + nextRowDelta,
                                              navigation->next.column() );
        }
    }
    else if ( changed ) {
        // Legacy bounds have no identity with which to follow a layout change.
        navigation.reset();
    }

    // Resolve and construct everything before publishing either geometry or anchors.
    // Zero-fragment occurrences still publish their independently resolved navigation.
    if ( changed ) {
        portionLayout_ = makePortionLayout( entries, std::move( projectedRows ) );
    }
    navigationBounds_ = navigation;
}

const std::vector<Selection::SelectedPortion>& Selection::portions() const
{
    static const std::vector<SelectedPortion> empty;
    return portionPayload_ ? portionPayload_->portions : empty;
}

const Selection::PortionLayout& Selection::layout() const
{
    static const PortionLayout empty;
    return portionLayout_ ? *portionLayout_ : empty;
}

void Selection::setPortions( std::vector<SelectedPortion> entries,
                             std::vector<OptionalLineNumber> rows )
{
    if ( entries.empty() ) {
        portionPayload_.reset();
        portionLayout_.reset();
        return;
    }
    auto payload = std::make_shared<PortionPayload>();
    payload->portions = std::move( entries );
    std::set<klogg::LogRowId> identities;
    OptionalLineNumber previousLegacyRow;
    for ( const auto& selected : payload->portions ) {
        if ( selected.identity ) {
            if ( identities.insert( *selected.identity ).second ) {
                ++payload->lineCount;
            }
        }
        else if ( previousLegacyRow != selected.portion.line() ) {
            previousLegacyRow = selected.portion.line();
            ++payload->lineCount;
        }
    }
    auto presentation = makePortionLayout( payload->portions, std::move( rows ) );
    portionPayload_ = std::move( payload );
    portionLayout_ = std::move( presentation );
}

std::shared_ptr<const Selection::PortionLayout>
Selection::makePortionLayout( const std::vector<SelectedPortion>& entries,
                              std::vector<OptionalLineNumber> rows )
{
    auto presentation = std::make_shared<PortionLayout>();
    presentation->rows = std::move( rows );
    presentation->visible.reserve( entries.size() );
    for ( std::size_t index = 0; index < entries.size(); ++index ) {
        if ( presentation->rows[ index ] ) {
            presentation->visible.push_back( index );
        }
    }
    const auto rowStartLess = [&]( std::size_t lhs, std::size_t rhs ) {
        return presentation->rows[ lhs ] < presentation->rows[ rhs ]
               || ( presentation->rows[ lhs ] == presentation->rows[ rhs ]
                    && entries[ lhs ].portion.startColumn() < entries[ rhs ].portion.startColumn() );
    };
    if ( !std::is_sorted( presentation->visible.begin(), presentation->visible.end(), rowStartLess ) ) {
        std::stable_sort( presentation->visible.begin(), presentation->visible.end(), rowStartLess );
    }
    return presentation;
}

LineNumber Selection::displayedRow( std::size_t index ) const
{
    const auto& row = layout().rows[ index ];
    if ( !row ) {
        throw std::logic_error( "Visible selection index references a hidden row" );
    }
    return *row;
}

Portion Selection::displayedPortion( std::size_t index ) const
{
    const auto& selected = portions()[ index ];
    const auto& portion = selected.portion;
    const auto& raw = portion.rawColumns();
    const auto row = displayedRow( index );
    return raw ? Portion( row, portion.startColumn(), portion.endColumn(), raw->first, raw->second )
               : Portion( row, portion.startColumn(), portion.endColumn() );
}

bool Selection::canUseAsSearchOperand() const
{
    if ( selectedPartial_.line ) {
        return getPortionForLine( *selectedPartial_.line ).isValid();
    }
    return portions().size() == 1;
}

std::vector<std::size_t>::const_iterator
Selection::firstPortionForLine( LineNumber line ) const
{
    return std::lower_bound( layout().visible.begin(), layout().visible.end(), line,
                             [this]( std::size_t index, LineNumber row ) {
                                 return layout().rows[ index ] < row;
                             } );
}

std::vector<Portion> Selection::getPortionsForLine( LineNumber line ) const
{
    if ( selectedPartial_.line && *selectedPartial_.line == line ) {
        return { getPortionForLine( line ) };
    }
    const auto first = firstPortionForLine( line );
    const auto last = std::find_if( first, layout().visible.end(),
                                    [this, line]( std::size_t index ) {
                                        return layout().rows[ index ] != line;
                                    } );
    std::vector<Portion> portions;
    portions.reserve( static_cast<std::size_t>( last - first ) );
    for ( auto it = first; it != last; ++it ) {
        portions.push_back( displayedPortion( *it ) );
    }
    return portions;
}

bool Selection::isPortion( LineNumber line ) const
{
    if ( selectedPartial_.line && *selectedPartial_.line == line ) {
        return true;
    }
    const auto first = firstPortionForLine( line );
    return first != layout().visible.end() && layout().rows[ *first ] == line;
}

void Selection::selectRange( LineNumber startLine, LineNumber endLine )
{
    clear();

    selectedRange_.startLine = std::min( startLine, endLine );
    selectedRange_.endLine = std::max( startLine, endLine );

    selectedRange_.firstLine = startLine;
}

void Selection::toggleLine( LineNumber line )
{
    navigationBounds_.reset();
    // Fold the primary whole-line/range selection into the toggled set so
    // toggling composes with it; a portion selection is superseded by this
    // line-oriented gesture.
    if ( selectedLine_.has_value() ) {
        insertToggledRange( *selectedLine_, *selectedLine_ );
        selectedLine_ = {};
    }
    else if ( selectedRange_.startLine.has_value() ) {
        insertToggledRange( *selectedRange_.startLine, selectedRange_.endLine );
        selectedRange_.startLine = {};
    }
    selectedPartial_.line = {};
    portionPayload_.reset();
    portionLayout_.reset();

    // The last ctrl-clicked line becomes the shift+click anchor whether the
    // toggle added or removed it.
    lastToggledLine_ = line;

    for ( auto it = toggledRanges_.begin(); it != toggledRanges_.end(); ++it ) {
        if ( line >= it->first && line <= it->second ) {
            // The line is already selected: remove it, splitting the
            // interval if it sits in the middle.
            if ( it->first == line && it->second == line ) {
                toggledRanges_.erase( it );
            }
            else if ( it->first == line ) {
                it->first = line + 1_lcount;
            }
            else if ( it->second == line ) {
                it->second = line - 1_lcount;
            }
            else {
                const auto oldEnd = it->second;
                it->second = line - 1_lcount;
                toggledRanges_.insert( it + 1, { line + 1_lcount, oldEnd } );
            }
            return;
        }
    }

    insertToggledRange( line, line );
}

void Selection::insertToggledRange( LineNumber firstLine, LineNumber lastLine )
{
    // First interval that is not entirely before (or adjacent to) the new one
    auto rangeIt = std::lower_bound( toggledRanges_.begin(), toggledRanges_.end(), firstLine,
                                     []( const auto& range, LineNumber line ) {
                                         return range.second + 1_lcount < line;
                                     } );

    // Merge every following interval the new range overlaps or touches
    while ( rangeIt != toggledRanges_.end() && !( lastLine + 1_lcount < rangeIt->first ) ) {
        firstLine = std::min( firstLine, rangeIt->first );
        lastLine = std::max( lastLine, rangeIt->second );
        rangeIt = toggledRanges_.erase( rangeIt );
    }

    toggledRanges_.insert( rangeIt, { firstLine, lastLine } );
}

bool Selection::isInToggledRanges( LineNumber line ) const
{
    // toggledRanges_ is ascending and non-overlapping
    const auto rangeIt = std::upper_bound( toggledRanges_.begin(), toggledRanges_.end(), line,
                                           []( LineNumber needle, const auto& range ) {
                                               return needle < range.first;
                                           } );

    if ( rangeIt == toggledRanges_.begin() ) {
        return false;
    }

    return line <= std::prev( rangeIt )->second;
}

void Selection::selectRangeFromPrevious( LineNumber line )
{
    navigationBounds_.reset();
    LineNumber previous_line;

    if ( selectedLine_.has_value() )
        previous_line = *selectedLine_;
    else if ( selectedRange_.startLine.has_value() )
        previous_line = selectedRange_.firstLine;
    else if ( selectedPartial_.line.has_value() )
        previous_line = *selectedPartial_.line;
    else if ( !layout().visible.empty() )
        previous_line = displayedRow( layout().visible.front() );
    else if ( lastToggledLine_.has_value() ) {
        // Shift+click on a ctrl-click selection extends a range from the last
        // toggled line and merges it into the toggled set, keeping the
        // non-contiguous lines already selected.
        insertToggledRange( std::min( *lastToggledLine_, line ),
                            std::max( *lastToggledLine_, line ) );
        return;
    }
    else
        previous_line = 0_lnum;

    selectRange( previous_line, line );
}

void Selection::crop( LineNumber last_line )
{
    // Source updates also crop unchanged/append-only selections. Check the visible
    // index, never immutable snapshot order, before retiring occurrence anchors.
    const bool croppedPortions = !layout().visible.empty()
                                 && layout().rows[ layout().visible.back() ] > last_line;
    if ( croppedPortions ) {
        const auto& entries = portions();
        auto rows = layout().rows;
        bool removedLegacyRows = false;
        for ( std::size_t index = 0; index < entries.size(); ++index ) {
            if ( entries[ index ].identity ) {
                if ( rows[ index ] > last_line ) {
                    // Folding changes geometry, not immutable snapshot membership.
                    rows[ index ].reset();
                }
            }
            else if ( entries[ index ].portion.line() > last_line ) {
                removedLegacyRows = true;
            }
        }
        if ( removedLegacyRows ) {
            // Legacy cropping changes membership. Publish new values without
            // mutating payload or layout retained by an asynchronous reader.
            std::vector<SelectedPortion> retained;
            std::vector<OptionalLineNumber> retainedRows;
            retained.reserve( entries.size() );
            retainedRows.reserve( entries.size() );
            for ( std::size_t index = 0; index < entries.size(); ++index ) {
                const auto& selected = entries[ index ];
                if ( selected.identity || selected.portion.line() <= last_line ) {
                    retained.push_back( selected );
                    retainedRows.push_back( rows[ index ] );
                }
            }
            setPortions( std::move( retained ), std::move( retainedRows ) );
        }
        else {
            portionLayout_ = makePortionLayout( entries, std::move( rows ) );
        }
    }
    if ( navigationBounds_ && navigationBounds_->identity ) {
        // The next bound may be a valid row+1 EOF sentinel. Only the occurrence
        // row determines visibility; retain its identity/columns while hidden.
        if ( navigationBounds_->presentedRow > last_line ) {
            navigationBounds_->presentedRow.reset();
        }
    }
    else if ( navigationBounds_
              && ( navigationBounds_->previous.line() > last_line
                   || navigationBounds_->next.line() > last_line || croppedPortions ) ) {
        navigationBounds_.reset();
    }
    if ( selectedLine_.has_value() && *selectedLine_ > last_line )
        selectedLine_ = {};

    if ( selectedPartial_.line.has_value() && *selectedPartial_.line > last_line )
        selectedPartial_.line = {};

    if ( selectedRange_.endLine > last_line )
        selectedRange_.endLine = last_line;

    if ( selectedRange_.startLine.has_value() && *selectedRange_.startLine > last_line )
        selectedRange_.startLine = last_line;

    // The shift anchor must not resurrect lines beyond the cropped range.
    if ( lastToggledLine_.has_value() && *lastToggledLine_ > last_line )
        lastToggledLine_ = last_line;

    for ( auto it = toggledRanges_.begin(); it != toggledRanges_.end(); ) {
        if ( it->first > last_line ) {
            it = toggledRanges_.erase( it );
        }
        else {
            if ( it->second > last_line ) {
                it->second = last_line;
            }
            ++it;
        }
    }
}

Portion Selection::getPortionForLine( LineNumber line ) const
{
    if ( selectedPartial_.line.has_value() && *selectedPartial_.line == line ) {
        return Portion( *selectedPartial_.line, selectedPartial_.startColumn,
                        selectedPartial_.endColumn );
    }
    const auto first = firstPortionForLine( line );
    if ( first != layout().visible.end() && layout().rows[ *first ] == line ) {
        return displayedPortion( *first );
    }
    return {};
}

bool Selection::isLineSelected( LineNumber line ) const
{
    return ( selectedLine_.has_value() && line == *selectedLine_ )
           || ( selectedRange_.startLine.has_value() && line >= *selectedRange_.startLine
                && line <= selectedRange_.endLine )
           || isInToggledRanges( line );
}

bool Selection::isPortionSelected( LineNumber line, LineColumn startColumn,
                                   LineColumn endColumn ) const
{
    if ( isLineSelected( line ) ) {
        return true;
    }

    if ( selectedPartial_.line ) {
        const auto portion = getPortionForLine( line );
        return portion.isValid() && startColumn >= portion.startColumn()
               && endColumn <= portion.endColumn();
    }
    for ( auto it = firstPortionForLine( line );
          it != layout().visible.end() && layout().rows[ *it ] == line; ++it ) {
        const auto& portion = portions()[ *it ].portion;
        if ( startColumn >= portion.startColumn() && endColumn <= portion.endColumn() ) {
            return true;
        }
    }
    return false;
}

OptionalLineNumber Selection::selectedLine() const
{
    if ( selectedLine_.has_value() ) {
        return selectedLine_;
    }
    // A single ctrl-clicked line answers as the single selected line so the
    // context menu and search/selection-range actions treat it uniformly.
    if ( isSingleToggledLine() ) {
        return toggledRanges_.front().first;
    }
    return {};
}

klogg::vector<LineNumber> Selection::getLines() const
{
    klogg::vector<LineNumber> selection;

    if ( selectedLine_.has_value() ) {
        selection.push_back( *selectedLine_ );
    }
    else if ( selectedPartial_.line.has_value() ) {
        selection.push_back( *selectedPartial_.line );
    }
    else if ( !portions().empty() ) {
        for ( const auto index : layout().visible ) {
            const auto row = displayedRow( index );
            if ( selection.empty() || selection.back() != row ) {
                selection.push_back( row );
            }
        }
    }
    else if ( selectedRange_.startLine.has_value() ) {
        selection.resize( selectedRange_.size().get() );
        std::iota( selection.begin(), selection.end(), *selectedRange_.startLine );
    }
    else {
        for ( const auto& range : toggledRanges_ ) {
            for ( LineNumber line = range.first;; ++line ) {
                selection.push_back( line );
                if ( line == range.second ) {
                    break;
                }
            }
        }
    }

    return selection;
}

LinesCount Selection::getSelectedLinesCount() const
{
    if ( selectedLine_ || selectedPartial_.line ) {
        return 1_lcount;
    }
    auto count = selectedRange_.size();
    if ( portionPayload_ ) {
        count += portionPayload_->lineCount;
    }
    for ( const auto& range : toggledRanges_ ) {
        count += ( range.second - range.first ) + 1_lcount;
    }
    return count;
}

// The tab behaviour is a bit odd at the moment, full lines are not expanded
// but partials (part of line) are, they probably should not ideally.
QString Selection::getSelectedText( const AbstractLogData* logData, bool lineNumbers ) const
{
    const auto selectionData = getSelectionWithLineNumbers( logData );

    QString text;

    const auto selectionSizeEstimate = std::accumulate(
        selectionData.begin(), selectionData.end(), klogg::isize( selectionData ),
        []( const auto& acc, const auto& next ) { return acc + next.second.size(); } );

    text.reserve( selectionSizeEstimate );

    for ( const auto& [ lineNumber, line ] : selectionData ) {
        if ( !text.isEmpty() ) {
            text.append( klogg::platform::nativeTextLineSeparator() );
        }

        if ( lineNumbers ) {
            text.append( QStringLiteral( "%1: %2" ).arg( lineNumber.get() ).arg( line ) );
        }
        else {
            text.append( line );
        }
    }

    return text;
}

bool Selection::hasCapturedPayload() const
{
    return !portions().empty() && portions().front().capturedText.has_value();
}

std::optional<QString> Selection::serializeCapturedText(
    bool withLineNumbers, const std::function<bool()>& cancelled ) const
{
    if ( !hasCapturedPayload() ) {
        return std::nullopt;
    }
    const auto isCancelled = [&] { return cancelled && cancelled(); };
    constexpr auto chunkSize = klogg::ContainerIndex{ 64 } * 1024;
    const auto separator = klogg::platform::nativeTextLineSeparator();
    QString text;
    for ( const auto& selected : portions() ) {
        if ( isCancelled() ) {
            return std::nullopt;
        }
        const auto& captured = selected.capturedText;
        if ( !captured ) {
            throw std::logic_error( "Captured selection contains an uncaptured fragment" );
        }
        const auto& [lineNumber, payload] = *captured;
        if ( !text.isEmpty() ) {
            text.append( separator );
        }
        if ( withLineNumbers ) {
            text.append( QString::number( lineNumber.get() ) );
            text.append( QStringLiteral( ": " ) );
        }
        // Append directly from owned storage, without an intermediate text list or
        // an uninterruptible whole-fragment append. Offsets are UTF-16 code units.
        for ( klogg::ContainerIndex offset = 0; offset < payload.size(); ) {
            if ( isCancelled() ) {
                return std::nullopt;
            }
            const auto length = std::min( chunkSize, payload.size() - offset );
            text.append( payload.constData() + offset, length );
            offset += length;
        }
    }
    if ( isCancelled() ) {
        return std::nullopt;
    }
    return text;
}

QStringList Selection::getSelectedLinesText( const AbstractLogData* logData ) const
{
    const auto selectionData = getSelectionWithLineNumbers( logData );

    QStringList texts;
    texts.reserve( static_cast<QStringList::size_type>( selectionData.size() ) );
    for ( const auto& [ lineNumber, line ] : selectionData ) {
        texts.append( line );
    }

    return texts;
}

std::vector<std::pair<LineNumber, QString>>
Selection::getSelectionWithLineNumbers( const AbstractLogData* logData ) const
{
    std::vector<std::pair<LineNumber, QString>> selectionData;

    if ( selectedLine_.has_value() ) {
        if ( logData->isLineCopyable( selectedLine_.value() ) ) {
            selectionData.emplace_back( logData->getLineNumber( selectedLine_.value() ),
                                        logData->getLineString( *selectedLine_ ) );
        }
    }
    else if ( selectedPartial_.line.has_value() ) {
        if ( logData->isLineCopyable( selectedPartial_.line.value() ) ) {
            selectionData.emplace_back(
                logData->getLineNumber( selectedPartial_.line.value() ),
                logData->getExpandedLineString( *selectedPartial_.line )
                    .mid( selectedPartial_.startColumn.get(),
                          selectedPartial_.size().get() ) );
        }
    }
    else if ( !portions().empty() ) {
        selectionData.reserve( portions().size() );
        OptionalLineNumber cachedLine;
        QString text;
        bool cachedRaw = false;
        for ( const auto& selected : portions() ) {
            if ( selected.capturedText ) {
                selectionData.push_back( *selected.capturedText );
                continue;
            }
            const auto& portion = selected.portion;
            if ( !logData->isLineCopyable( portion.line() ) ) {
                continue;
            }
            const bool useRaw = portion.rawColumns().has_value();
            if ( cachedLine != portion.line() || cachedRaw != useRaw ) {
                text = useRaw ? logData->getLineString( portion.line() )
                              : logData->getExpandedLineString( portion.line() );
                cachedLine = portion.line();
                cachedRaw = useRaw;
            }
            const auto columns = portion.rawColumns().value_or(
                std::make_pair( portion.startColumn(), portion.endColumn() ) );
            const auto length = ( columns.second - columns.first ) + 1_length;
            selectionData.emplace_back( logData->getLineNumber( portion.line() ),
                                        text.mid( columns.first.get(), length.get() ) );
        }
    }
    else if ( selectedRange_.startLine.has_value() ) {
        const auto list = logData->getLines( *selectedRange_.startLine, selectedRange_.size() );
        LineNumber ln = *selectedRange_.startLine;

        for ( const auto& line : list ) {
            // Folder group headers (and any other non-copyable rows the data
            // layer marks) never reach the clipboard / search composition.
            if ( logData->isLineCopyable( ln ) ) {
                selectionData.emplace_back( logData->getLineNumber( ln ), line );
            }
            ln++;
        }
    }
    else {
        for ( const auto& range : toggledRanges_ ) {
            const auto list
                = logData->getLines( range.first, ( range.second - range.first ) + 1_lcount );
            LineNumber lineNumber = range.first;

            for ( const auto& line : list ) {
                if ( logData->isLineCopyable( lineNumber ) ) {
                    selectionData.emplace_back( logData->getLineNumber( lineNumber ), line );
                }
                lineNumber++;
            }
        }
    }

    return selectionData;
}

void Selection::setNavigationBounds( FilePosition previous, FilePosition next,
                                     std::optional<klogg::LogRowId> identity )
{
    navigationBounds_ = NavigationBounds{ previous, next, identity, previous.line() };
}

bool Selection::hasStableNavigationAnchor() const
{
    return navigationBounds_ && navigationBounds_->identity.has_value();
}

FilePosition Selection::getNextPosition() const
{
    if ( navigationBounds_ ) {
        return hasStableNavigationAnchor() && !navigationBounds_->presentedRow
                   ? FilePosition( 0_lnum, 0_lcol ) : navigationBounds_->next;
    }
    LineNumber line;
    LineColumn column = 0_lcol;

    if ( selectedLine_.has_value() ) {
        line = *selectedLine_ + 1_lcount;
    }
    else if ( selectedRange_.startLine.has_value() ) {
        line = selectedRange_.endLine + 1_lcount;
    }
    else if ( !toggledRanges_.empty() ) {
        line = toggledRanges_.back().second + 1_lcount;
    }
    else if ( selectedPartial_.line.has_value() ) {
        line = *selectedPartial_.line;
        column = selectedPartial_.endColumn + 1_length;
    }
    else if ( !layout().visible.empty() ) {
        line = displayedRow( layout().visible.back() );
        for ( auto it = firstPortionForLine( line ); it != layout().visible.end(); ++it ) {
            column = std::max( column, portions()[ *it ].portion.endColumn() + 1_length );
        }
    }

    return FilePosition( line, column );
}

FilePosition Selection::getPreviousPosition() const
{
    if ( navigationBounds_ ) {
        return hasStableNavigationAnchor() && !navigationBounds_->presentedRow
                   ? FilePosition( 0_lnum, 0_lcol ) : navigationBounds_->previous;
    }
    LineNumber line = 0_lnum;
    LineColumn column = 0_lcol;

    if ( selectedLine_.has_value() ) {
        line = *selectedLine_;
    }
    else if ( selectedRange_.startLine.has_value() ) {
        line = *selectedRange_.startLine;
    }
    else if ( !toggledRanges_.empty() ) {
        line = toggledRanges_.front().first;
    }
    else if ( selectedPartial_.line.has_value() ) {
        line = *selectedPartial_.line;
        column = selectedPartial_.startColumn - 1_length;
    }
    else if ( !layout().visible.empty() ) {
        const auto index = layout().visible.front();
        line = displayedRow( index );
        column = portions()[ index ].portion.startColumn() - 1_length;
    }

    return FilePosition( line, column );
}
