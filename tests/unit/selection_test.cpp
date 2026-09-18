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

} // namespace

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
