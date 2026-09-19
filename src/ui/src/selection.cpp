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
// This class implements the selection handling. No check is made on
// the validity of the selection, it must be handled by the caller.
// There are three types of selection, only one type might be active
// at any time.

#include <algorithm>
#include <numeric>

#include "abstractlogdata.h"
#include "containers.h"
#include "linetypes.h"
#include "log.h"
#include "selection.h"

Selection::Selection()
{
    selectedPartial_.startColumn = 0_lcol;
    selectedPartial_.endColumn = 0_lcol;

    selectedRange_.endLine = 0_lnum;
}

void Selection::selectPortion( LineNumber line, LineColumn startColumn, LineColumn endColumn )
{
    // First unselect any whole line or range
    selectedLine_ = {};
    selectedRange_.startLine = {};
    toggledRanges_.clear();
    lastToggledLine_ = {};

    selectedPartial_.line = line;
    selectedPartial_.startColumn = std::min( startColumn, endColumn );
    selectedPartial_.endColumn = std::max( startColumn, endColumn );
}

void Selection::selectRange( LineNumber startLine, LineNumber endLine )
{
    // First unselect any whole line and portion
    selectedLine_ = {};
    selectedPartial_.line = {};
    toggledRanges_.clear();
    lastToggledLine_ = {};

    selectedRange_.startLine = std::min( startLine, endLine );
    selectedRange_.endLine = std::max( startLine, endLine );

    selectedRange_.firstLine = startLine;
}

void Selection::toggleLine( LineNumber line )
{
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
    LineNumber previous_line;

    if ( selectedLine_.has_value() )
        previous_line = *selectedLine_;
    else if ( selectedRange_.startLine.has_value() )
        previous_line = selectedRange_.firstLine;
    else if ( selectedPartial_.line.has_value() )
        previous_line = *selectedPartial_.line;
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

    const auto portion = getPortionForLine( line );
    if ( !portion.isValid() ) {
        return false;
    }

    return startColumn >= portion.startColumn() && endColumn <= portion.endColumn();
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
    auto count = selectedRange_.size();
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
#if defined( Q_OS_WIN )
            text.append( QChar::CarriageReturn );
#endif
            text.append( QChar::LineFeed );
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

FilePosition Selection::getNextPosition() const
{
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

    return FilePosition( line, column );
}

FilePosition Selection::getPreviousPosition() const
{
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

    return FilePosition( line, column );
}
