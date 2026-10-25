/*
 * Copyright (C) 2026 Nicolas Bonnefon and other contributors
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

#include <catch2/catch.hpp>

#include <QStringList>
#include <utility>

#include "abstractlogdata.h"
#include "containers.h"
#include "selection.h"

namespace {

// Minimal in-memory AbstractLogData so selection text extraction can be
// exercised without a file on disk.
class SelectionStubLogData : public AbstractLogData {
  public:
    explicit SelectionStubLogData( QStringList lines )
        : lines_{ std::move( lines ) }
    {
    }

  protected:
    QString doGetLineString( LineNumber line ) const override
    {
        return lines_.at( static_cast<klogg::ContainerIndex>( line.get() ) );
    }

    QString doGetExpandedLineString( LineNumber line ) const override
    {
        return doGetLineString( line );
    }

    klogg::vector<QString> doGetLines( LineNumber firstLine, LinesCount number ) const override
    {
        klogg::vector<QString> result;
        result.reserve( static_cast<size_t>( number.get() ) );
        for ( LineNumber line = firstLine; line < firstLine + number; ++line ) {
            result.push_back( doGetLineString( line ) );
        }
        return result;
    }

    klogg::vector<QString> doGetExpandedLines( LineNumber firstLine,
                                               LinesCount number ) const override
    {
        return doGetLines( firstLine, number );
    }

    LineNumber doGetLineNumber( LineNumber index ) const override
    {
        return index;
    }

    LinesCount doGetNbLine() const override
    {
        return LinesCount( static_cast<uint64_t>( lines_.size() ) );
    }

    LineLength doGetMaxLength() const override
    {
        LineLength maxLength{};
        for ( const auto& line : lines_ ) {
            maxLength = std::max( maxLength, LineLength( line.size() ) );
        }
        return maxLength;
    }

    LineLength doGetLineLength( LineNumber line ) const override
    {
        return LineLength( doGetLineString( line ).size() );
    }

    void doSetDisplayEncoding( const char* ) override
    {
    }

    QTextCodec* doGetDisplayEncoding() const override
    {
        return nullptr;
    }

    void doAttachReader() const override
    {
    }

    void doDetachReader() const override
    {
    }

  private:
    QStringList lines_;
};

// Models projected rows with a non-copyable header and repeated source numbers.
class SelectionProjectedLogData : public SelectionStubLogData {
  public:
    using SelectionStubLogData::SelectionStubLogData;

  protected:
    QString doGetExpandedLineString( LineNumber line ) const override
    {
        return untabify( doGetLineString( line ) );
    }

    bool doIsLineCopyable( LineNumber line ) const override
    {
        return line != 1_lnum;
    }

    LineNumber doGetLineNumber( LineNumber ) const override
    {
        return 7_lnum;
    }
};

QString selectionLineSeparator()
{
#if defined( Q_OS_WIN )
    return QStringLiteral( "\r\n" );
#else
    return QStringLiteral( "\n" );
#endif
}

} // namespace

TEST_CASE( "partial fragments are ordered and enumerate unique selected rows",
           "[selection][multi-portion]" )
{
    Selection selection;
    selection.selectPortions( { Portion( 3_lnum, 4_lcol, 6_lcol ),
                                Portion( 1_lnum, 5_lcol, 7_lcol ),
                                Portion( 1_lnum, 0_lcol, 2_lcol ) } );

    REQUIRE_FALSE( selection.isEmpty() );
    REQUIRE( selection.isPortion() );
    REQUIRE( selection.isMultiPortion() );
    REQUIRE_FALSE( selection.isSingleLine() );
    REQUIRE_FALSE( selection.selectedLine().has_value() );
    REQUIRE( selection.isPortion( 1_lnum ) );
    REQUIRE( selection.isPortion( 3_lnum ) );
    REQUIRE_FALSE( selection.isPortion( 2_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 1_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 1_lnum, 3_lnum } );

    const auto portions = selection.getPortionsForLine( 1_lnum );
    REQUIRE( portions.size() == 2 );
    REQUIRE( portions[ 0 ].startColumn() == 0_lcol );
    REQUIRE( portions[ 0 ].endColumn() == 2_lcol );
    REQUIRE( portions[ 1 ].startColumn() == 5_lcol );
    REQUIRE( portions[ 1 ].endColumn() == 7_lcol );
    REQUIRE( selection.getPortionsForLine( 2_lnum ).empty() );
    REQUIRE( selection.getPortionForLine( 1_lnum ).startColumn() == 0_lcol );
    REQUIRE( selection.isPortionSelected( 1_lnum, 5_lcol, 7_lcol ) );
    REQUIRE( selection.isPortionSelected( 3_lnum, 5_lcol, 5_lcol ) );
    REQUIRE_FALSE( selection.isPortionSelected( 1_lnum, 2_lcol, 5_lcol ) );
    REQUIRE_FALSE( selection.isPortionSelected( 2_lnum, 0_lcol, 0_lcol ) );
    // LineColumn subtraction saturates at zero, matching ordinary portion navigation.
    REQUIRE( selection.getPreviousPosition() == FilePosition( 1_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 3_lnum, 7_lcol ) );
}

TEST_CASE( "partial fragments ignore invalid inputs and merge overlaps but not adjacency",
           "[selection][multi-portion]" )
{
    Selection selection;
    selection.selectPortions( { Portion{}, Portion( 1_lnum, 7_lcol, 6_lcol ),
                                Portion( 1_lnum, LineColumn( -1 ), 2_lcol ),
                                Portion( 1_lnum, 2_lcol, 4_lcol ),
                                Portion( 1_lnum, 0_lcol, 2_lcol ),
                                Portion( 1_lnum, 1_lcol, 1_lcol ),
                                Portion( 1_lnum, 5_lcol, 6_lcol ) } );
    const auto portions = selection.getPortionsForLine( 1_lnum );
    REQUIRE( portions.size() == 2 );
    REQUIRE( portions[ 0 ].startColumn() == 0_lcol );
    REQUIRE( portions[ 0 ].endColumn() == 4_lcol );
    REQUIRE( portions[ 1 ].startColumn() == 5_lcol );
    REQUIRE( portions[ 1 ].endColumn() == 6_lcol );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.isMultiPortion() );

    selection.selectPortions( { Portion( 2_lnum, 1_lcol, 3_lcol ),
                                Portion( 2_lnum, 1_lcol, 3_lcol ) } );
    REQUIRE( selection.isPortion() );
    REQUIRE( selection.isMultiPortion() );
    REQUIRE( selection.getPortionsForLine( 2_lnum ).size() == 1 );

    selection.selectPortions( { Portion{} } );
    REQUIRE( selection.isEmpty() );
    REQUIRE( selection.getSelectedLinesCount() == 0_lcount );
}

TEST_CASE( "partial fragment copy preserves each adjacent occurrence in row order",
           "[selection][multi-portion]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "abcXYZtail" ),
                                    QStringLiteral( "second" ) } };
    Selection selection;
    selection.selectPortions( { Portion( 1_lnum, 1_lcol, 3_lcol ),
                                Portion( 0_lnum, 3_lcol, 5_lcol ),
                                Portion( 0_lnum, 0_lcol, 2_lcol ) } );
    const QStringList fragments{ QStringLiteral( "abc" ), QStringLiteral( "XYZ" ),
                                 QStringLiteral( "eco" ) };
    REQUIRE( selection.getSelectedLinesText( &logData ) == fragments );
    REQUIRE( selection.getSelectedText( &logData ) == fragments.join( selectionLineSeparator() ) );
    // AbstractLogData converts zero-based source indices to one-based labels.
    const QStringList numbered{ QStringLiteral( "1: abc" ), QStringLiteral( "1: XYZ" ),
                                QStringLiteral( "2: eco" ) };
    REQUIRE( selection.getSelectedText( &logData, true )
             == numbered.join( selectionLineSeparator() ) );
}

TEST_CASE( "preserved overlapping occurrences remain distinct copy fragments",
           "[selection][multi-portion]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "aaa" ) } };
    Selection selection;
    // Two distinct lookahead occurrences of (?=(aa)), not two captures of one match.
    selection.selectPortions( { Portion( 0_lnum, 1_lcol, 2_lcol ),
                                Portion( 0_lnum, 0_lcol, 1_lcol ) },
                              Selection::PortionOverlap::Preserve );
    const auto portions = selection.getPortionsForLine( 0_lnum );
    REQUIRE( portions.size() == 2 );
    REQUIRE( portions[ 0 ].startColumn() == 0_lcol );
    REQUIRE( portions[ 1 ].startColumn() == 1_lcol );
    REQUIRE( selection.getSelectedText( &logData )
             == QStringLiteral( "aa" ) + selectionLineSeparator() + QStringLiteral( "aa" ) );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.isPortionSelected( 0_lnum, 0_lcol, 1_lcol ) );
    REQUIRE( selection.isPortionSelected( 0_lnum, 1_lcol, 2_lcol ) );
    REQUIRE_FALSE( selection.isPortionSelected( 0_lnum, 0_lcol, 2_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 3_lcol ) );

    // Equal starts retain occurrence order; a nested final fragment must not
    // move the forward search anchor back inside the larger selection.
    selection.selectPortions( { Portion( 0_lnum, 0_lcol, 2_lcol ),
                                Portion( 0_lnum, 0_lcol, 0_lcol ) },
                              Selection::PortionOverlap::Preserve );
    REQUIRE( selection.getSelectedText( &logData )
             == QStringLiteral( "aaa" ) + selectionLineSeparator() + QStringLiteral( "a" ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 3_lcol ) );
}

TEST_CASE( "partial copy uses expanded columns and preserves projected row identity",
           "[selection][multi-portion]" )
{
    SelectionProjectedLogData logData{ { QStringLiteral( "\tAB" ), QStringLiteral( "header" ),
                                        QStringLiteral( "\tCD" ) } };
    Selection selection;
    selection.selectPortions( { Portion( 2_lnum, 8_lcol, 9_lcol ),
                                Portion( 1_lnum, 0_lcol, 5_lcol ),
                                Portion( 0_lnum, 8_lcol, 9_lcol ) } );
    REQUIRE( selection.getSelectedLinesText( &logData )
             == QStringList{ QStringLiteral( "AB" ), QStringLiteral( "CD" ) } );
    REQUIRE( selection.getSelectedText( &logData, true )
             == QStringLiteral( "8: AB" ) + selectionLineSeparator() + QStringLiteral( "8: CD" ) );

    selection.selectPortion( 0_lnum, 9_lcol, 8_lcol );
    REQUIRE_FALSE( selection.isMultiPortion() );
    REQUIRE( selection.getPortionsForLine( 0_lnum ).size() == 1 );
    REQUIRE( selection.getSelectedText( &logData ) == QStringLiteral( "AB" ) );
}

TEST_CASE( "raw fragment columns preserve tabs while display columns drive hit testing",
           "[selection][multi-portion][raw-portion]" )
{
    SelectionProjectedLogData logData{ { QStringLiteral( "\tprefix A\tB tail" ) } };
    Selection selection;
    // The leading tab shifts A to display column 15; the inner tab is column 16-23.
    selection.selectPortions( { Portion( 0_lnum, 15_lcol, 24_lcol, 8_lcol, 10_lcol ) } );
    REQUIRE( selection.getSelectedText( &logData ) == QStringLiteral( "A\tB" ) );
    REQUIRE( selection.isPortionSelected( 0_lnum, 16_lcol, 23_lcol ) );
    REQUIRE_FALSE( selection.isPortionSelected( 0_lnum, 8_lcol, 10_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 25_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 14_lcol ) );

    SECTION( "overlap merges display and raw extents" )
    {
        selection.selectPortions( { Portion( 0_lnum, 15_lcol, 23_lcol, 8_lcol, 9_lcol ),
                                    Portion( 0_lnum, 16_lcol, 24_lcol, 9_lcol, 10_lcol ) } );
        REQUIRE( selection.getPortionsForLine( 0_lnum ).size() == 1 );
        REQUIRE( selection.getSelectedText( &logData ) == QStringLiteral( "A\tB" ) );
    }
    SECTION( "invalid raw coordinates are ignored" )
    {
        selection.selectPortions( { Portion( 0_lnum, 15_lcol, 24_lcol, 10_lcol, 8_lcol ),
                                    Portion( 0_lnum, 15_lcol, 24_lcol, LineColumn( -1 ), 8_lcol ) } );
        REQUIRE( selection.isEmpty() );
    }
}

TEST_CASE( "cropping partial fragments drops missing rows and updates navigation anchors",
           "[selection][multi-portion]" )
{
    Selection selection;
    selection.selectPortions( { Portion( 5_lnum, 1_lcol, 3_lcol ),
                                Portion( 2_lnum, 2_lcol, 4_lcol ) } );
    selection.crop( 3_lnum );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 2_lnum } );
    REQUIRE( selection.isMultiPortion() );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 5_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    selection.selectRangeFromPrevious( 4_lnum );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 2_lnum, 3_lnum, 4_lnum } );
    REQUIRE_FALSE( selection.isPortion() );

    selection.selectPortions( { Portion( 5_lnum, 1_lcol, 3_lcol ) } );
    selection.crop( 3_lnum );
    REQUIRE( selection.isEmpty() );
    REQUIRE( selection.getSelectedLinesCount() == 0_lcount );
}

TEST_CASE( "selection gestures replace partial fragments and fragments replace other modes",
           "[selection][multi-portion]" )
{
    Selection selection;
    selection.selectPortions( { Portion( 2_lnum, 1_lcol, 3_lcol ),
                                Portion( 5_lnum, 1_lcol, 3_lcol ) } );
    REQUIRE( selection.isMultiPortion() );

    SECTION( "line" ) { selection.selectLine( 7_lnum ); }
    SECTION( "range" ) { selection.selectRange( 7_lnum, 8_lnum ); }
    SECTION( "toggle" ) { selection.toggleLine( 7_lnum ); }
    SECTION( "single portion" ) { selection.selectPortion( 7_lnum, 3_lcol, 1_lcol ); }
    SECTION( "clear" ) { selection.clear(); }

    REQUIRE_FALSE( selection.isMultiPortion() );
    REQUIRE( selection.getPortionsForLine( 2_lnum ).empty() );
    REQUIRE( selection.getPortionsForLine( 5_lnum ).empty() );
    selection.selectPortions( { Portion( 1_lnum, 0_lcol, 1_lcol ) } );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 1_lnum } );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE_FALSE( selection.isLineSelected( 7_lnum ) );
    selection.selectPortions( {} );
    REQUIRE( selection.isEmpty() );
}

TEST_CASE( "clearing a range removes membership copy and shift anchor",
           "[selection][selection-clear]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "zero" ), QStringLiteral( "one" ),
                                    QStringLiteral( "two" ) } };
    Selection selection;
    selection.selectRange( 1_lnum, 2_lnum );
    SECTION( "range is not empty" )
    {
        REQUIRE_FALSE( selection.isEmpty() );
    }
    SECTION( "clear removes the range" )
    {
        selection.clear();
        REQUIRE_FALSE( selection.isLineSelected( 1_lnum ) );
        REQUIRE( selection.getLines().empty() );
        REQUIRE( selection.getSelectedLinesCount() == 0_lcount );
        REQUIRE( selection.getSelectedText( &logData ).isEmpty() );
        selection.selectRangeFromPrevious( 2_lnum );
        REQUIRE( selection.isLineSelected( 0_lnum ) );
    }
}

TEST_CASE( "single line and portion selection counts match their enumerated rows",
           "[selection][selection-count]" )
{
    Selection selection;
    SECTION( "whole line" ) { selection.selectLine( 2_lnum ); }
    SECTION( "portion" ) { selection.selectPortion( 2_lnum, 5_lcol, 2_lcol ); }
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 2_lnum } );
}

TEST_CASE( "toggled disjoint lines are all selected and enumerated in row order",
           "[selection]" )
{
    Selection selection;

    selection.toggleLine( 2_lnum );
    selection.toggleLine( 5_lnum );

    REQUIRE( selection.isLineSelected( 2_lnum ) );
    REQUIRE( selection.isLineSelected( 5_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 0_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 3_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 4_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 6_lnum ) );

    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );

    const auto lines = selection.getLines();
    REQUIRE( lines.size() == 2 );
    REQUIRE( lines[ 0 ] == 2_lnum );
    REQUIRE( lines[ 1 ] == 5_lnum );
}

TEST_CASE( "toggling an already selected line removes it", "[selection]" )
{
    Selection selection;

    selection.toggleLine( 2_lnum );
    selection.toggleLine( 5_lnum );
    selection.toggleLine( 2_lnum );

    REQUIRE_FALSE( selection.isLineSelected( 2_lnum ) );
    REQUIRE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );

    const auto lines = selection.getLines();
    REQUIRE( lines.size() == 1 );
    REQUIRE( lines[ 0 ] == 5_lnum );

    selection.toggleLine( 5_lnum );
    REQUIRE( selection.isEmpty() );
}

TEST_CASE( "toggling a line inside a merged toggled range splits it", "[selection]" )
{
    Selection selection;

    selection.toggleLine( 2_lnum );
    selection.toggleLine( 3_lnum );
    selection.toggleLine( 4_lnum );
    selection.toggleLine( 3_lnum );

    REQUIRE( selection.isLineSelected( 2_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 3_lnum ) );
    REQUIRE( selection.isLineSelected( 4_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );
}

TEST_CASE( "toggling adjacent lines merges them without double counting", "[selection]" )
{
    Selection selection;

    selection.toggleLine( 2_lnum );
    selection.toggleLine( 3_lnum );

    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );

    selection.toggleLine( 1_lnum );
    selection.toggleLine( 5_lnum );
    // 4 bridges the [1,3] block and 5: everything merges into [1,5]
    selection.toggleLine( 4_lnum );

    REQUIRE( selection.getSelectedLinesCount() == 5_lcount );
    for ( auto line = 1_lnum; line <= 5_lnum; ++line ) {
        REQUIRE( selection.isLineSelected( line ) );
    }

    const auto lines = selection.getLines();
    REQUIRE( lines.size() == 5 );
    for ( auto i = 0u; i < lines.size(); ++i ) {
        REQUIRE( lines[ i ] == LineNumber( i + 1 ) );
    }
}

TEST_CASE( "plain line and range selections replace the toggled set", "[selection]" )
{
    Selection selection;
    selection.toggleLine( 2_lnum );
    selection.toggleLine( 5_lnum );

    selection.selectLine( 7_lnum );
    REQUIRE( selection.isSingleLine() );
    REQUIRE_FALSE( selection.isLineSelected( 2_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.isLineSelected( 7_lnum ) );

    selection.toggleLine( 1_lnum );
    selection.selectRange( 8_lnum, 9_lnum );
    REQUIRE_FALSE( selection.isLineSelected( 1_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 7_lnum ) );
    REQUIRE( selection.isLineSelected( 8_lnum ) );
    REQUIRE( selection.isLineSelected( 9_lnum ) );

    selection.toggleLine( 3_lnum );
    selection.clear();
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.isLineSelected( 3_lnum ) );
}

TEST_CASE( "toggling outside a selected range adds the line to the selection",
           "[selection]" )
{
    Selection selection;

    selection.selectRange( 2_lnum, 4_lnum );
    selection.toggleLine( 7_lnum );

    for ( auto line = 2_lnum; line <= 4_lnum; ++line ) {
        REQUIRE( selection.isLineSelected( line ) );
    }
    REQUIRE( selection.isLineSelected( 7_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 4_lcount );

    const auto lines = selection.getLines();
    REQUIRE( lines.size() == 4 );
    REQUIRE( lines[ 0 ] == 2_lnum );
    REQUIRE( lines[ 1 ] == 3_lnum );
    REQUIRE( lines[ 2 ] == 4_lnum );
    REQUIRE( lines[ 3 ] == 7_lnum );
}

TEST_CASE( "toggling inside a selected range removes only that line", "[selection]" )
{
    Selection selection;

    selection.selectRange( 2_lnum, 5_lnum );
    selection.toggleLine( 3_lnum );

    REQUIRE( selection.isLineSelected( 2_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 3_lnum ) );
    REQUIRE( selection.isLineSelected( 4_lnum ) );
    REQUIRE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 3_lcount );

    const auto lines = selection.getLines();
    REQUIRE( lines.size() == 3 );
    REQUIRE( lines[ 0 ] == 2_lnum );
    REQUIRE( lines[ 1 ] == 4_lnum );
    REQUIRE( lines[ 2 ] == 5_lnum );
}

TEST_CASE( "toggling a single selected line removes it from the selection", "[selection]" )
{
    Selection selection;

    selection.selectLine( 4_lnum );
    selection.toggleLine( 9_lnum );

    REQUIRE( selection.isLineSelected( 4_lnum ) );
    REQUIRE( selection.isLineSelected( 9_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );

    selection.toggleLine( 4_lnum );
    REQUIRE_FALSE( selection.isLineSelected( 4_lnum ) );
    REQUIRE( selection.isLineSelected( 9_lnum ) );
}

TEST_CASE( "crop bounds every toggled range", "[selection]" )
{
    Selection selection;

    selection.toggleLine( 2_lnum );
    selection.toggleLine( 5_lnum );
    selection.toggleLine( 8_lnum );

    selection.crop( 6_lnum );

    REQUIRE( selection.isLineSelected( 2_lnum ) );
    REQUIRE( selection.isLineSelected( 5_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 8_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );

    // Cropping inside a merged range clamps its end
    selection.toggleLine( 3_lnum );
    selection.toggleLine( 4_lnum );
    selection.crop( 3_lnum );

    REQUIRE( selection.isLineSelected( 2_lnum ) );
    REQUIRE( selection.isLineSelected( 3_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 4_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );
}

TEST_CASE( "shift-click after toggling extends a range from the last toggled line",
           "[selection]" )
{
    Selection selection;

    selection.toggleLine( 5_lnum );
    selection.toggleLine( 10_lnum );
    selection.selectRangeFromPrevious( 12_lnum );

    // The range from the last toggled line is added; earlier toggles survive.
    REQUIRE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.isLineSelected( 10_lnum ) );
    REQUIRE( selection.isLineSelected( 12_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 0_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 6_lnum ) );

    const auto lines = selection.getLines();
    REQUIRE( lines.size() == 4 );
    REQUIRE( lines[ 0 ] == 5_lnum );
    REQUIRE( lines[ 1 ] == 10_lnum );
    REQUIRE( lines[ 3 ] == 12_lnum );
}

TEST_CASE( "shift-click after toggling a line off anchors on the last toggled line",
           "[selection]" )
{
    Selection selection;

    selection.toggleLine( 5_lnum );
    selection.toggleLine( 10_lnum );
    selection.toggleLine( 10_lnum );
    selection.selectRangeFromPrevious( 13_lnum );

    // The anchor is the last ctrl-clicked line even though it toggled off; the
    // extended range covers it again.
    REQUIRE( selection.isLineSelected( 5_lnum ) );
    REQUIRE( selection.isLineSelected( 10_lnum ) );
    REQUIRE( selection.isLineSelected( 13_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 7_lnum ) );
}

TEST_CASE( "crop clamps the shift anchor of a toggled selection", "[selection]" )
{
    Selection selection;

    selection.toggleLine( 2_lnum );
    selection.toggleLine( 8_lnum );

    // Data shrinks: line 8 is gone, so the anchor must not resurrect it.
    selection.crop( 6_lnum );
    selection.selectRangeFromPrevious( 4_lnum );

    REQUIRE_FALSE( selection.isLineSelected( 7_lnum ) );
    REQUIRE_FALSE( selection.isLineSelected( 8_lnum ) );
    REQUIRE( selection.isLineSelected( 2_lnum ) );
    REQUIRE( selection.isLineSelected( 4_lnum ) );
    REQUIRE( selection.isLineSelected( 6_lnum ) );
}

TEST_CASE( "a single toggled line behaves as a single line selection", "[selection]" )
{
    Selection selection;

    selection.toggleLine( 7_lnum );

    REQUIRE( selection.isSingleLine() );
    REQUIRE( selection.selectedLine().has_value() );
    REQUIRE( *selection.selectedLine() == 7_lnum );

    // Toggling it off leaves an empty selection again.
    selection.toggleLine( 7_lnum );
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.isSingleLine() );
    REQUIRE_FALSE( selection.selectedLine().has_value() );
}

TEST_CASE( "selected text covers every toggled line in row order", "[selection]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "line zero" ), QStringLiteral( "line one" ),
                                    QStringLiteral( "line two" ), QStringLiteral( "line three" ),
                                    QStringLiteral( "line four" ) } };

    Selection selection;
    selection.toggleLine( 3_lnum );
    selection.toggleLine( 1_lnum );

    const auto linesText = selection.getSelectedLinesText( &logData );
    REQUIRE( linesText
             == QStringList{ QStringLiteral( "line one" ), QStringLiteral( "line three" ) } );

    const auto expectedText = QStringLiteral( "line one" ) +
#if defined( Q_OS_WIN )
                              QChar::CarriageReturn +
#endif
                              QChar::LineFeed + QStringLiteral( "line three" );
    REQUIRE( selection.getSelectedText( &logData ) == expectedText );

    // A range folded into the toggled set is copied in row order as well
    selection.selectRange( 0_lnum, 1_lnum );
    selection.toggleLine( 4_lnum );
    const auto mixedText = selection.getSelectedLinesText( &logData );
    REQUIRE( mixedText
             == QStringList{ QStringLiteral( "line zero" ), QStringLiteral( "line one" ),
                             QStringLiteral( "line four" ) } );
}
