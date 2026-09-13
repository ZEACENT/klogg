/*
 * Copyright (C) 2011, 2012 Nicolas Bonnefon and other contributors
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

// This file implements the Overview class.
// It provides support for drawing the match overview sidebar but
// the actual drawing is done in AbstractLogView which uses this class.

#include "linetypes.h"
#include "log.h"

#include "logfiltereddata.h"

#include "overview.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace {

uint64_t sourceLineBoundaryForPixel( uint64_t pixelBoundary, uint64_t lineCount,
                                     uint64_t height )
{
    Q_ASSERT( height > 0 );
    Q_ASSERT( pixelBoundary <= height );

    // ceil(pixelBoundary * lineCount / height), decomposed to avoid the
    // potentially overflowing pixelBoundary * lineCount product. height and
    // pixelBoundary originate from an unsigned viewport size, so the remaining
    // product is bounded by UINT32_MAX squared and fits in uint64_t.
    const auto wholeLinesPerPixel = lineCount / height;
    const auto remainder = lineCount % height;
    const auto partialProduct = pixelBoundary * remainder;
    return pixelBoundary * wholeLinesPerPixel + partialProduct / height
           + ( partialProduct % height != 0 ? 1 : 0 );
}

void appendWeightedLine( klogg::vector<Overview::WeightedLine>& lines, int position,
                         LinesCount count )
{
    if ( count == 0_lcount ) {
        return;
    }

    lines.emplace_back( position );
    const auto additionalWeight = std::min<uint64_t>(
        count.get() - 1, static_cast<uint64_t>( Overview::WeightedLine::WEIGHT_STEPS - 1 ) );
    for ( uint64_t load = 0; load < additionalWeight; ++load ) {
        lines.back().load();
    }
}

uint64_t pixelForSourceLine( uint64_t line, uint64_t lineCount, uint64_t height )
{
    Q_ASSERT( lineCount > 0 );
    Q_ASSERT( line < lineCount );
    Q_ASSERT( lineCount <= height );

    // This path is used only when both factors are bounded by the unsigned
    // viewport height, so the product fits in uint64_t without a wider type.
    return line * height / lineCount;
}

template <typename RangeCounter>
unsigned aggregateOverviewRanges( LinesCount linesInFile, unsigned viewportHeight,
                                  klogg::vector<Overview::WeightedLine>& matchLines,
                                  klogg::vector<Overview::WeightedLine>& markLines,
                                  RangeCounter countRange )
{
    const auto lineCount = linesInFile.get();
    const auto height = static_cast<uint64_t>( viewportHeight );
    if ( lineCount == 0 || height == 0 ) {
        return 0;
    }

    unsigned workCount = 0;
    if ( lineCount <= height ) {
        // More pixels than lines creates empty pixel buckets. Visit each source
        // line once instead of issuing a range query for every empty pixel.
        for ( uint64_t line = 0; line < lineCount; ++line ) {
            const auto counts = countRange( LineNumber( line ), LineNumber( line + 1 ) );
            const auto position
                = static_cast<int>( pixelForSourceLine( line, lineCount, height ) );
            appendWeightedLine( matchLines, position, counts.matches );
            appendWeightedLine( markLines, position, counts.marks );
            ++workCount;
        }
        return workCount;
    }

    auto firstLine = 0_lnum;
    for ( unsigned position = 0; position < viewportHeight; ++position ) {
        const auto endLine = LineNumber( sourceLineBoundaryForPixel(
            static_cast<uint64_t>( position ) + 1, lineCount, height ) );
        const auto counts = countRange( firstLine, endLine );
        const auto weightedPosition = static_cast<int>( position );
        appendWeightedLine( matchLines, weightedPosition, counts.matches );
        appendWeightedLine( markLines, weightedPosition, counts.marks );
        firstLine = endLine;
        ++workCount;
    }
    return workCount;
}

} // namespace

Overview::Overview()
    : matchLines_()
    , markLines_()
{
    logFilteredData_ = nullptr;
    height_ = 0;
    dirty_ = true;
    visible_ = false;
}

void Overview::setFilteredData( const LogFilteredData* logFilteredData )
{
    LOG_INFO << "OverviewWidget::setFilteredData " << (void*)logFilteredData;

    logFilteredData_ = logFilteredData;
    // Drop any folder-mode explicit list so a later single-file attach takes
    // precedence (symmetric with setMatchLines clearing the LogFilteredData ptr).
    explicitMatchLines_.clear();
    explicitMarkLines_.clear();
    dirty_ = true;
}

void Overview::setMatchLines( const std::vector<LineNumber>& matchLines )
{
    explicitMatchLines_ = matchLines;
    // Folder mode owns its match list; decouple from any LogFilteredData.
    logFilteredData_ = nullptr;
    dirty_ = true;
}

void Overview::setMarkLines( const std::vector<LineNumber>& markLines )
{
    explicitMarkLines_ = markLines;
    // Folder mode owns its mark list; decouple from any LogFilteredData
    // (symmetric with setMatchLines).
    logFilteredData_ = nullptr;
    dirty_ = true;
}

void Overview::updateData( LinesCount totalNbLine )
{
    LOG_INFO << "OverviewWidget::updateData " << totalNbLine;

    linesInFile_ = totalNbLine;
    dirty_ = true;
}

void Overview::updateView( unsigned height )
{
    // We don't touch the cache if the height hasn't changed
    if ( ( height != height_ ) || ( dirty_ == true ) ) {
        height_ = height;

        recalculatesLines();
    }
}

const klogg::vector<Overview::WeightedLine>* Overview::getMatchLines() const
{
    return &matchLines_;
}

const klogg::vector<Overview::WeightedLine>* Overview::getMarkLines() const
{
    return &markLines_;
}

std::pair<int, int> Overview::getViewLines() const
{
    int top = 0;
    int bottom = static_cast<int>( height_ ) - 1;

    if ( linesInFile_.get() > 0 ) {
        top = static_cast<int>( ( topLine_.get() ) * height_ / ( linesInFile_.get() ) );

        bottom = top + static_cast<int>( nbLines_.get() * height_ / ( linesInFile_.get() ) );
    }

    return std::make_pair( top, bottom );
}

LineNumber Overview::fileLineFromY( int position ) const
{
    const auto line = static_cast<LineNumber::UnderlyingType>(
        static_cast<LineNumber::UnderlyingType>( position ) * linesInFile_.get() / static_cast<LineNumber::UnderlyingType>( height_ ) );

    return LineNumber{ line };
}

int Overview::yFromFileLine( LineNumber fileLine ) const
{
    int position = 0;

    if ( linesInFile_.get() > 0 )
        position = static_cast<int>( fileLine.get() * height_ / linesInFile_.get() );

    return position;
}

// Update the internal cache
void Overview::recalculatesLines()
{
    LOG_INFO << "OverviewWidget::recalculatesLines";

    // Clear in every branch so a transition between data sources (single-file
    // <-> folder) cannot leave stale entries from the previous mode.
    matchLines_.clear();
    markLines_.clear();
    lastAggregationWorkCount_ = 0;

    if ( logFilteredData_ != nullptr ) {
        const auto visibility = logFilteredData_->visibility();
        const bool hasVisibleMatches
            = visibility.testFlag( LogFilteredData::VisibilityFlags::Matches )
              && logFilteredData_->getNbMatches() != 0_lcount;
        const bool hasVisibleMarks
            = visibility.testFlag( LogFilteredData::VisibilityFlags::Marks )
              && logFilteredData_->getNbMarks() != 0_lcount;
        if ( hasVisibleMatches || hasVisibleMarks ) {
            lastAggregationWorkCount_ = aggregateOverviewRanges(
                linesInFile_, height_, matchLines_, markLines_,
                [ this ]( LineNumber first, LineNumber end ) {
                    return logFilteredData_->countLineTypesInRange( first, end );
                } );
        }
    }
    else if ( linesInFile_.get() > 0 && height_ > 0
              && ( !explicitMatchLines_.empty() || !explicitMarkLines_.empty() ) ) {
        // Folder result lists are sorted. Advance to each range boundary with
        // binary search so dense folder matches share the viewport-bounded path
        // used by single-file and live sources.
        auto firstMatch = explicitMatchLines_.cbegin();
        auto firstMark = explicitMarkLines_.cbegin();
        lastAggregationWorkCount_ = aggregateOverviewRanges(
            linesInFile_, height_, matchLines_, markLines_,
            [ this, &firstMatch, &firstMark ]( LineNumber, LineNumber end ) {
                const auto endMatch
                    = std::lower_bound( firstMatch, explicitMatchLines_.cend(), end );
                const auto endMark
                    = std::lower_bound( firstMark, explicitMarkLines_.cend(), end );
                const auto matchCount = static_cast<uint64_t>(
                    std::distance( firstMatch, endMatch ) );
                const auto markCount = static_cast<uint64_t>(
                    std::distance( firstMark, endMark ) );
                firstMatch = endMatch;
                firstMark = endMark;
                return LogFilteredData::LineTypeRangeCounts{
                    LinesCount( matchCount ), LinesCount( markCount ) };
            } );
    }
    else {
        LOG_INFO << "Overview::recalculatesLines: no overview categories";
    }

    dirty_ = false;
}
