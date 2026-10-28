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
#include <stdexcept>
#include <utility>

#include "abstractlogdata.h"
#include "containers.h"
#include "platform/platform_clipboard.h"
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

    void makeUnreadable()
    {
        lines_.clear();
        unreadable_ = true;
        reads_ = 0;
    }

    size_t reads() const
    {
        return reads_;
    }

  protected:
    QString doGetLineString( LineNumber line ) const override
    {
        recordRead();
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

    bool doIsLineCopyable( LineNumber ) const override
    {
        recordRead();
        return true;
    }

    LineNumber doGetLineNumber( LineNumber index ) const override
    {
        recordRead();
        return index;
    }

    LinesCount doGetNbLine() const override
    {
        recordRead();
        return LinesCount( static_cast<uint64_t>( lines_.size() ) );
    }

    LineLength doGetMaxLength() const override
    {
        recordRead();
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
    void recordRead() const
    {
        ++reads_;
        if ( unreadable_ ) {
            throw std::runtime_error( "selection source is no longer readable" );
        }
    }

    QStringList lines_;
    bool unreadable_ = false;
    mutable size_t reads_ = 0;
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
    return klogg::platform::nativeTextLineSeparator();
}

} // namespace

TEST_CASE( "captured fragments copy owned payloads without accessing an unreadable source",
           "[selection][captured-portion]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "original" ) } };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 2_lnum, 8_lcol, 16_lcol ), 7_lnum, QStringLiteral( "A\tB" ) },
          { Portion( 0_lnum, 1_lcol, 2_lcol ), 7_lnum, QStringLiteral( "aa" ) },
          { Portion( 0_lnum, 0_lcol, 2_lcol ), 42_lnum, QStringLiteral( "aaa" ) },
          { Portion( 0_lnum, 0_lcol, 0_lcol ), 42_lnum, QStringLiteral( "a" ) } } );
    const QStringList fragments{ QStringLiteral( "aaa" ), QStringLiteral( "a" ),
                                 QStringLiteral( "aa" ), QStringLiteral( "A\tB" ) };
    const QStringList numbered{ QStringLiteral( "42: aaa" ), QStringLiteral( "42: a" ),
                                QStringLiteral( "7: aa" ), QStringLiteral( "7: A\tB" ) };
    logData.makeUnreadable();

    REQUIRE( selection.getSelectedLinesText( &logData ) == fragments );
    REQUIRE( selection.getSelectedText( &logData ) == fragments.join( selectionLineSeparator() ) );
    REQUIRE( selection.getSelectedText( &logData, true )
             == numbered.join( selectionLineSeparator() ) );
    REQUIRE( logData.reads() == 0 );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 0_lnum, 2_lnum } );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );
    REQUIRE( selection.isPortion() );
    REQUIRE( selection.isMultiPortion() );
    REQUIRE_FALSE( selection.isSingleLine() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getPortionsForLine( 0_lnum ).size() == 3 );
    REQUIRE( selection.getPortionForLine( 0_lnum ).endColumn() == 2_lcol );
    REQUIRE( selection.isPortionSelected( 2_lnum, 9_lcol, 15_lcol ) );
    REQUIRE_FALSE( selection.isLineSelected( 2_lnum ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 17_lcol ) );
}

TEST_CASE( "captured fragment payload owns raw QString storage and outlives replacement",
           "[selection][captured-portion]" )
{
    Selection selection;
    {
        QChar raw[]{ QChar( 'o' ), QChar( 'l' ), QChar( 'd' ) };
        QString payload = QString::fromRawData( raw, 3 );
        std::vector<Selection::CapturedPortion> fragments{
            { Portion( 0_lnum, 0_lcol, 2_lcol ), 19_lnum, payload } };
        selection.selectCapturedPortions( fragments );
        raw[ 0 ] = QChar( 'X' );
        fragments.front().text = QStringLiteral( "replaced" );
        payload.clear();
        REQUIRE( selection.getSelectedText( nullptr ) == QStringLiteral( "old" ) );
    }
    REQUIRE( selection.canUseAsSearchOperand() );
    const auto retainedSelection = selection;
    const auto retainedText = selection.getSelectedLinesText( nullptr );
    selection.selectCapturedPortions(
        { { Portion( 1_lnum, 0_lcol, 2_lcol ), 5_lnum, QStringLiteral( "new" ) } } );
    REQUIRE( selection.getSelectedText( nullptr, true ) == QStringLiteral( "5: new" ) );
    selection.clear();
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getSelectedText( nullptr ).isEmpty() );
    REQUIRE( selection.getSelectedLinesText( nullptr ).isEmpty() );
    REQUIRE( retainedSelection.getSelectedText( nullptr, true ) == QStringLiteral( "19: old" ) );
    REQUIRE( retainedText == QStringList{ QStringLiteral( "old" ) } );
}

TEST_CASE( "captured fragment crop preserves aligned payloads and search capability",
           "[selection][captured-portion]" )
{
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 5_lnum, 1_lcol, 3_lcol ), 11_lnum, QStringLiteral( "two" ) },
          { Portion( 2_lnum, 2_lcol, 4_lcol ), 30_lnum, QStringLiteral( "one" ) } } );
    selection.crop( 3_lnum );
    REQUIRE( selection.getSelectedText( nullptr, true ) == QStringLiteral( "30: one" ) );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 2_lnum } );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.isMultiPortion() );
    REQUIRE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 5_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    selection.crop( 1_lnum );
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.isPortion() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getSelectedLinesCount() == 0_lcount );
    REQUIRE( selection.getSelectedLinesText( nullptr ).isEmpty() );
    selection.selectRangeFromPrevious( 1_lnum );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 0_lnum, 1_lnum } );
}

TEST_CASE( "ordinary gestures replace captured payload without changing ordinary copy",
           "[selection][captured-portion]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "current" ), QStringLiteral( "second" ) } };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 2_lcol ), 99_lnum, QStringLiteral( "old" ) } } );
    QStringList expected;
    SECTION( "line" )
    {
        selection.selectLine( 0_lnum );
        expected = QStringList{ QStringLiteral( "current" ) };
    }
    SECTION( "range" )
    {
        selection.selectRange( 0_lnum, 1_lnum );
        expected = QStringList{ QStringLiteral( "current" ), QStringLiteral( "second" ) };
    }
    SECTION( "shift range" )
    {
        selection.selectRangeFromPrevious( 1_lnum );
        expected = QStringList{ QStringLiteral( "current" ), QStringLiteral( "second" ) };
    }
    SECTION( "toggle" )
    {
        selection.toggleLine( 1_lnum );
        expected = QStringList{ QStringLiteral( "second" ) };
    }
    SECTION( "ordinary portion" )
    {
        selection.selectPortion( 0_lnum, 0_lcol, 2_lcol );
        expected = QStringList{ QStringLiteral( "cur" ) };
    }
    SECTION( "ordinary batch" )
    {
        selection.selectPortions( { Portion( 1_lnum, 0_lcol, 2_lcol ) } );
        expected = QStringList{ QStringLiteral( "sec" ) };
    }
    SECTION( "empty captured batch" ) { selection.selectCapturedPortions( {} ); }
    SECTION( "clear" ) { selection.clear(); }
    REQUIRE( selection.getSelectedLinesText( &logData ) == expected );
    REQUIRE( selection.getSelectedText( &logData ) == expected.join( selectionLineSeparator() ) );

    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 0_lcol ), 3_lnum, QStringLiteral( "x" ) } } );
    logData.makeUnreadable();
    REQUIRE( selection.getSelectedText( &logData, true ) == QStringLiteral( "3: x" ) );
    REQUIRE( logData.reads() == 0 );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.canUseAsSearchOperand() );
}

TEST_CASE( "search operand capability distinguishes fragments from painting portions",
           "[selection][selection-capability]" )
{
    Selection selection;
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.selectLine( 0_lnum );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.selectRange( 0_lnum, 1_lnum );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.clear();
    selection.toggleLine( 0_lnum );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.selectPortion( 0_lnum, 1_lcol, 0_lcol );
    REQUIRE( selection.canUseAsSearchOperand() );
    selection.selectPortions( { Portion( 0_lnum, 0_lcol, 0_lcol ) } );
    REQUIRE( selection.canUseAsSearchOperand() );
    selection.selectPortions( { Portion( 0_lnum, 0_lcol, 0_lcol ),
                                Portion( 0_lnum, 1_lcol, 1_lcol ) } );
    REQUIRE( selection.isPortion() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.selectPortions( { Portion( 0_lnum, 0_lcol, 1_lcol ),
                                Portion( 0_lnum, 1_lcol, 2_lcol ) },
                              Selection::PortionOverlap::Preserve );
    REQUIRE( selection.isPortion() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.selectCapturedPortions(
        { { Portion{}, 1_lnum, QStringLiteral( "invalid" ) },
          { Portion( 0_lnum, 0_lcol, 0_lcol ), 1_lnum, QString{} } } );
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 1_lcol ), 1_lnum, QStringLiteral( "ab" ) },
          { Portion( 0_lnum, 1_lcol, 2_lcol ), 1_lnum, QStringLiteral( "bc" ) } } );
    REQUIRE( selection.isPortion() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
}

TEST_CASE( "occurrence navigation bounds are independent of captured fragments",
           "[selection][selection-navigation-bounds]" )
{
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 4_lcol, 7_lcol ), 1_lnum, QStringLiteral( "left" ) },
          { Portion( 0_lnum, 12_lcol, 16_lcol ), 1_lnum, QStringLiteral( "right" ) } } );
    // Lookahead captures can extend past the occurrence's navigation anchor.
    selection.setNavigationBounds( FilePosition( 0_lnum, 1_lcol ),
                                   FilePosition( 0_lnum, 3_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 3_lcol ) );
    REQUIRE( selection.isPortion() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getSelectedLinesText( nullptr )
             == QStringList{ QStringLiteral( "left" ), QStringLiteral( "right" ) } );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
}

TEST_CASE( "zero fragment occurrences retain navigation without fabricating selected text",
           "[selection][selection-navigation-bounds]" )
{
    Selection selection;
    selection.selectCapturedPortions( {} );
    selection.setNavigationBounds( FilePosition( 2_lnum, 3_lcol ),
                                   FilePosition( 2_lnum, 5_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 3_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 5_lcol ) );
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.isPortion() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getLines().empty() );
    REQUIRE( selection.getSelectedLinesCount() == 0_lcount );
    REQUIRE( selection.getSelectedText( nullptr ).isEmpty() );
    REQUIRE( selection.getSelectedLinesText( nullptr ).isEmpty() );

    SECTION( "clear retires empty occurrence bounds" ) { selection.clear(); }
    SECTION( "crop retires empty occurrence bounds" ) { selection.crop( 1_lnum ); }
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 0_lcol ) );
}

TEST_CASE( "no-op crop preserves occurrence navigation bounds",
           "[selection][selection-navigation-bounds]" )
{
    Selection selection;
    SECTION( "captured occurrence" )
    {
        selection.selectCapturedPortions(
            { { Portion( 2_lnum, 4_lcol, 7_lcol ), 3_lnum, QStringLiteral( "text" ) } } );
    }
    SECTION( "zero fragment occurrence" ) { selection.selectCapturedPortions( {} ); }
    selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                                   FilePosition( 2_lnum, 3_lcol ) );
    // updateData crops even when the source keeps its size or only appends rows.
    selection.crop( 2_lnum );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 3_lcol ) );
    selection.crop( 8_lnum );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 3_lcol ) );
}

TEST_CASE( "selection edits retire occurrence navigation bounds",
           "[selection][selection-navigation-bounds]" )
{
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 2_lnum, 4_lcol, 7_lcol ), 1_lnum, QStringLiteral( "left" ) },
          { Portion( 5_lnum, 12_lcol, 16_lcol ), 2_lnum, QStringLiteral( "right" ) } } );
    selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                                   FilePosition( 5_lnum, 3_lcol ) );
    FilePosition previous( 0_lnum, 0_lcol );
    FilePosition next( 0_lnum, 0_lcol );
    SECTION( "clear" ) { selection.clear(); }
    SECTION( "whole line replacement" )
    {
        selection.selectLine( 4_lnum );
        previous = FilePosition( 4_lnum, 0_lcol );
        next = FilePosition( 5_lnum, 0_lcol );
    }
    SECTION( "ordinary partial replacement" )
    {
        selection.selectPortion( 4_lnum, 1_lcol, 2_lcol );
        previous = FilePosition( 4_lnum, 0_lcol );
        next = FilePosition( 4_lnum, 3_lcol );
    }
    SECTION( "ordinary batch replacement" )
    {
        selection.selectPortions( { Portion( 4_lnum, 1_lcol, 2_lcol ) } );
        previous = FilePosition( 4_lnum, 0_lcol );
        next = FilePosition( 4_lnum, 3_lcol );
    }
    SECTION( "captured batch replacement" )
    {
        selection.selectCapturedPortions(
            { { Portion( 4_lnum, 1_lcol, 2_lcol ), 9_lnum, QStringLiteral( "ab" ) } } );
        previous = FilePosition( 4_lnum, 0_lcol );
        next = FilePosition( 4_lnum, 3_lcol );
    }
    SECTION( "range replacement" )
    {
        selection.selectRange( 1_lnum, 2_lnum );
        previous = FilePosition( 1_lnum, 0_lcol );
        next = FilePosition( 3_lnum, 0_lcol );
    }
    SECTION( "shift range replacement" )
    {
        selection.selectRangeFromPrevious( 4_lnum );
        previous = FilePosition( 2_lnum, 0_lcol );
        next = FilePosition( 5_lnum, 0_lcol );
    }
    SECTION( "toggle replacement" )
    {
        selection.toggleLine( 1_lnum );
        previous = FilePosition( 1_lnum, 0_lcol );
        next = FilePosition( 2_lnum, 0_lcol );
    }
    SECTION( "crop falls back to surviving fragment geometry" )
    {
        selection.crop( 3_lnum );
        previous = FilePosition( 2_lnum, 3_lcol );
        next = FilePosition( 2_lnum, 8_lcol );
    }
    SECTION( "crop retires bounds even when only capture geometry is removed" )
    {
        selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                                       FilePosition( 2_lnum, 3_lcol ) );
        selection.crop( 3_lnum );
        previous = FilePosition( 2_lnum, 3_lcol );
        next = FilePosition( 2_lnum, 8_lcol );
    }
    REQUIRE( selection.getPreviousPosition() == previous );
    REQUIRE( selection.getNextPosition() == next );
}

TEST_CASE( "stable captured rows separate immutable copy order from visible geometry",
           "[selection][selection-row-remap]" )
{
    const klogg::LogRowId firstId{ klogg::LogRowId::Origin::Memory, 1, 10, 7 };
    const klogg::LogRowId secondId{ klogg::LogRowId::Origin::Memory, 1, 20, 7 };
    const klogg::LogRowId hiddenId{ klogg::LogRowId::Origin::Memory, 2, 10, 7 };
    SelectionStubLogData logData{ { QStringLiteral( "mutable" ) } };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 2_lnum, 4_lcol, 6_lcol ), 8_lnum, QStringLiteral( "hidden" ), hiddenId, {} },
          { Portion( 1_lnum, 2_lcol, 4_lcol ), 8_lnum, QStringLiteral( "second" ), secondId, 1_lnum },
          { Portion( 0_lnum, 1_lcol, 3_lcol ), 8_lnum, QStringLiteral( "overlap" ), firstId, 8_lnum },
          { Portion( 0_lnum, 0_lcol, 2_lcol, 0_lcol, 1_lcol ), 8_lnum,
            QStringLiteral( "first" ), firstId, 8_lnum } } );
    logData.makeUnreadable();
    const QStringList payloads{ QStringLiteral( "first" ), QStringLiteral( "overlap" ),
                                QStringLiteral( "second" ), QStringLiteral( "hidden" ) };
    const auto copied = selection.getSelectedText( &logData, true );
    REQUIRE( selection.getSelectedLinesText( &logData ) == payloads );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 1_lnum, 8_lnum } );
    REQUIRE( selection.getSelectedLinesCount() == 3_lcount );
    REQUIRE( selection.getPortionsForLine( 8_lnum ).size() == 2 );
    REQUIRE( selection.getPortionForLine( 8_lnum ).line() == 8_lnum );
    REQUIRE( selection.getPortionForLine( 8_lnum ).rawColumns()
             == std::make_optional( std::make_pair( 0_lcol, 1_lcol ) ) );
    REQUIRE( selection.isPortionSelected( 8_lnum, 1_lcol, 3_lcol ) );
    REQUIRE_FALSE( selection.isPortionSelected( 8_lnum, 0_lcol, 3_lcol ) );
    REQUIRE_FALSE( selection.isPortion( 0_lnum ) );
    REQUIRE_FALSE( selection.isPortion( 2_lnum ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 1_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 8_lnum, 4_lcol ) );

    selection.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
        if ( id == firstId ) return 2_lnum;
        if ( id == secondId ) return 0_lnum;
        REQUIRE( id == hiddenId );
        return 9_lnum;
    } );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 0_lnum, 2_lnum, 9_lnum } );
    REQUIRE( selection.getSelectedLinesCount() == 3_lcount );
    REQUIRE( selection.getPortionsForLine( 2_lnum ).size() == 2 );
    REQUIRE( selection.getPortionForLine( 2_lnum ).line() == 2_lnum );
    REQUIRE_FALSE( selection.isPortion( 8_lnum ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 9_lnum, 7_lcol ) );
    REQUIRE( selection.getSelectedText( &logData, true ) == copied );
    REQUIRE( selection.getSelectedLinesText( &logData ) == payloads );
    REQUIRE( logData.reads() == 0 );
}

TEST_CASE( "folding and cropping hide stable geometry without removing captured payload",
           "[selection][selection-row-remap]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Capture, 3, 4, 5 };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 100_lnum, 2_lcol, 4_lcol ), 6_lnum, QStringLiteral( "abc" ), identity, 3_lnum } } );
    selection.setNavigationBounds( FilePosition( 3_lnum, 0_lcol ), FilePosition( 3_lnum, 1_lcol ) );
    selection.crop( 2_lnum );
    REQUIRE( selection.getSelectedText( nullptr, true ) == QStringLiteral( "6: abc" ) );
    REQUIRE_FALSE( selection.isEmpty() );
    REQUIRE( selection.isPortion() );
    REQUIRE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.getLines().empty() );
    REQUIRE_FALSE( selection.isPortion( 3_lnum ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 0_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 0_lcol ) );

    selection.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
        REQUIRE( id == identity );
        return 1_lnum;
    } );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 1_lnum } );
    REQUIRE( selection.getPortionForLine( 1_lnum ).line() == 1_lnum );
    REQUIRE( selection.getNextPosition() == FilePosition( 1_lnum, 5_lcol ) );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 1_lnum, 1_lcol ) );
    selection.crop( 2_lnum );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 1_lnum } );

    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return {}; } );
    REQUIRE( selection.getLines().empty() );
    REQUIRE( selection.getSelectedLinesCount() == 1_lcount );
    REQUIRE( selection.getSelectedText( nullptr ) == QStringLiteral( "abc" ) );
    const auto retained = selection;
    selection.clear();
    REQUIRE( selection.isEmpty() );
    REQUIRE( selection.getSelectedLinesCount() == 0_lcount );
    REQUIRE( retained.getSelectedText( nullptr ) == QStringLiteral( "abc" ) );
}

TEST_CASE( "stable selected row counts use identity rather than snapshot or display indices",
           "[selection][selection-row-remap]" )
{
    const klogg::LogRowId firstId{ klogg::LogRowId::Origin::Memory, 7, 1, 2 };
    const klogg::LogRowId secondId{ klogg::LogRowId::Origin::Memory, 7, 2, 2 };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 0_lcol ), 3_lnum, QStringLiteral( "a" ), firstId, 2_lnum },
          { Portion( 1_lnum, 0_lcol, 0_lcol ), 3_lnum, QStringLiteral( "b" ), firstId, 2_lnum },
          { Portion( 2_lnum, 0_lcol, 0_lcol ), 3_lnum, QStringLiteral( "c" ), secondId, {} } } );
    REQUIRE( selection.getSelectedLinesCount() == 2_lcount );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 2_lnum } );
    REQUIRE( selection.getPortionsForLine( 2_lnum ).size() == 2 );
    REQUIRE( selection.getSelectedLinesText( nullptr )
             == QStringList{ QStringLiteral( "a" ), QStringLiteral( "b" ), QStringLiteral( "c" ) } );
}

TEST_CASE( "row remapping retires affected occurrence bounds but preserves no-op navigation",
           "[selection][selection-row-remap]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 8, 1, 0 };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 4_lcol, 7_lcol ), 1_lnum, QStringLiteral( "text" ), identity, 2_lnum } } );
    selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ), FilePosition( 2_lnum, 3_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 2_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 3_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 5_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 5_lnum, 3_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 5_lnum, 8_lcol ) );
    selection.selectRangeFromPrevious( 6_lnum );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 5_lnum, 6_lnum } );
}

TEST_CASE( "failed stable row resolution leaves the previous display mapping intact",
           "[selection][selection-row-remap]" )
{
    const klogg::LogRowId firstId{ klogg::LogRowId::Origin::Memory, 9, 1, 0 };
    const klogg::LogRowId secondId{ klogg::LogRowId::Origin::Memory, 9, 2, 0 };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 1_lcol, 2_lcol ), 1_lnum, QStringLiteral( "ab" ), firstId, 2_lnum },
          { Portion( 1_lnum, 1_lcol, 2_lcol ), 1_lnum, QStringLiteral( "cd" ), secondId, 4_lnum } } );
    selection.setNavigationBounds( FilePosition( 2_lnum, 0_lcol ), FilePosition( 4_lnum, 1_lcol ) );
    REQUIRE_THROWS_AS(
        selection.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
            if ( id == firstId ) return 7_lnum;
            throw std::runtime_error( "row resolution failed" );
        } ),
        std::runtime_error );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 2_lnum, 4_lnum } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 4_lnum, 1_lcol ) );
    REQUIRE( selection.getSelectedLinesText( nullptr )
             == QStringList{ QStringLiteral( "ab" ), QStringLiteral( "cd" ) } );
}

TEST_CASE( "row remapping and crop preserve legacy captured selection behavior",
           "[selection][selection-row-remap]" )
{
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 3_lnum, 2_lcol, 4_lcol ), 4_lnum, QStringLiteral( "old" ) } } );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber {
        FAIL( "legacy selections must not resolve stable identities" );
        return {};
    } );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 3_lnum } );
    selection.crop( 2_lnum );
    REQUIRE( selection.isEmpty() );
    REQUIRE( selection.getSelectedText( nullptr ).isEmpty() );
}

TEST_CASE( "captured serialization preserves immutable text labels and native separators",
           "[selection][selection-serialization]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 12, 1, 0 };
    SelectionStubLogData logData{ { QStringLiteral( "mutable" ) } };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 1_lnum, 0_lcol, 1_lcol ), 9_lnum, QStringLiteral( "A\tB" ) },
          { Portion( 0_lnum, 1_lcol, 2_lcol ), 42_lnum, QStringLiteral( "aa" ) },
          { Portion( 0_lnum, 0_lcol, 1_lcol ), 42_lnum, QStringLiteral( "aa" ) },
          { Portion( 2_lnum, 0_lcol, 3_lcol ), 5_lnum, QStringLiteral( "hidden" ), identity, {} } } );
    logData.makeUnreadable();
    REQUIRE( selection.hasCapturedPayload() );
    size_t cancellationChecks = 0;
    const auto neverCancelled = [&] {
        ++cancellationChecks;
        return false;
    };
    const auto text = selection.serializeCapturedText( false, neverCancelled );
    REQUIRE( text.has_value() );
    REQUIRE( *text == selection.getSelectedText( &logData ) );
    REQUIRE( cancellationChecks >= 4 );
    const auto numbered = selection.serializeCapturedText( true, neverCancelled );
    REQUIRE( numbered.has_value() );
    REQUIRE( *numbered == selection.getSelectedText( &logData, true ) );
    REQUIRE( selection.serializeCapturedText( false, {} ) == text );
    REQUIRE( logData.reads() == 0 );
}

TEST_CASE( "captured serialization cancels before work and between fragments",
           "[selection][selection-serialization]" )
{
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 0_lcol ), 1_lnum, QStringLiteral( "a" ) },
          { Portion( 0_lnum, 1_lcol, 1_lcol ), 1_lnum, QStringLiteral( "b" ) },
          { Portion( 0_lnum, 2_lcol, 2_lcol ), 1_lnum, QStringLiteral( "c" ) } } );
    size_t checks = 0;
    size_t cancelOnCheck = 1;
    SECTION( "already cancelled" ) { cancelOnCheck = 1; }
    SECTION( "cancel during fragment serialization" ) { cancelOnCheck = 4; }
    const auto result = selection.serializeCapturedText( true, [&] {
        ++checks;
        return checks == cancelOnCheck;
    } );
    REQUIRE_FALSE( result.has_value() );
    REQUIRE( checks == cancelOnCheck );
    REQUIRE( selection.getSelectedLinesText( nullptr )
             == QStringList{ QStringLiteral( "a" ), QStringLiteral( "b" ), QStringLiteral( "c" ) } );
}

TEST_CASE( "captured serialization checks cancellation within one long fragment",
           "[selection][selection-serialization]" )
{
    Selection selection;
    QString longText( 512 * 1024, QChar( 'x' ) );
    // Preserve embedded NUL and a surrogate pair straddling a serialization chunk.
    longText[ 3 ] = QChar::Null;
    longText[ 64 * 1024 - 1 ] = QChar( 0xD83D );
    longText[ 64 * 1024 ] = QChar( 0xDE00 );
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, LineColumn( longText.size() - 1 ) ), 11_lnum, longText } } );
    size_t checks = 0;
    const auto result = selection.serializeCapturedText( false, [&] {
        ++checks;
        return checks == 6;
    } );
    REQUIRE_FALSE( result.has_value() );
    REQUIRE( checks == 6 );
    const auto completed = selection.serializeCapturedText( false, {} );
    REQUIRE( completed.has_value() );
    REQUIRE( *completed == longText );
    REQUIRE( selection.getSelectedText( nullptr ) == longText );
}

TEST_CASE( "captured serialization capability excludes ordinary and empty selections",
           "[selection][selection-serialization]" )
{
    Selection selection;
    REQUIRE_FALSE( selection.hasCapturedPayload() );
    REQUIRE_FALSE( selection.serializeCapturedText( false, {} ).has_value() );
    SECTION( "ordinary partial" ) { selection.selectPortion( 0_lnum, 0_lcol, 2_lcol ); }
    SECTION( "ordinary batch" ) { selection.selectPortions( { Portion( 0_lnum, 0_lcol, 2_lcol ) } ); }
    SECTION( "ordinary line" ) { selection.selectLine( 0_lnum ); }
    SECTION( "ordinary range" ) { selection.selectRange( 0_lnum, 2_lnum ); }
    SECTION( "toggled line" ) { selection.toggleLine( 0_lnum ); }
    SECTION( "empty captured batch" ) { selection.selectCapturedPortions( {} ); }
    REQUIRE_FALSE( selection.hasCapturedPayload() );
    REQUIRE_FALSE( selection.serializeCapturedText( true, {} ).has_value() );
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 0_lcol ), 1_lnum, QStringLiteral( "x" ) } } );
    REQUIRE( selection.hasCapturedPayload() );
    selection.clear();
    REQUIRE_FALSE( selection.hasCapturedPayload() );
    REQUIRE_FALSE( selection.serializeCapturedText( false, {} ).has_value() );
}

TEST_CASE( "copied captured selections isolate layout edits and retain immutable payload",
           "[selection][selection-copy-isolation]" )
{
    const klogg::LogRowId firstId{ klogg::LogRowId::Origin::Memory, 21, 1, 0 };
    const klogg::LogRowId secondId{ klogg::LogRowId::Origin::Memory, 21, 2, 0 };
    const klogg::LogRowId hiddenId{ klogg::LogRowId::Origin::Memory, 21, 3, 0 };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 1_lcol, 2_lcol ), 4_lnum, QStringLiteral( "ab" ), firstId, 2_lnum },
          { Portion( 1_lnum, 1_lcol, 2_lcol ), 7_lnum, QStringLiteral( "cd" ), secondId, 5_lnum },
          { Portion( 2_lnum, 1_lcol, 2_lcol ), 9_lnum, QStringLiteral( "ef" ), hiddenId, {} } } );
    selection.setNavigationBounds( FilePosition( 2_lnum, 0_lcol ), FilePosition( 5_lnum, 1_lcol ) );
    const Selection retained = selection;
    const auto payload = retained.serializeCapturedText( true, {} );
    REQUIRE( payload.has_value() );

    selection.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
        if ( id == firstId ) return 8_lnum;
        if ( id == secondId ) return 0_lnum;
        return {};
    } );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 0_lnum, 8_lnum } );
    REQUIRE( retained.getLines() == klogg::vector<LineNumber>{ 2_lnum, 5_lnum } );
    REQUIRE( retained.getPreviousPosition() == FilePosition( 2_lnum, 0_lcol ) );
    REQUIRE( retained.getNextPosition() == FilePosition( 5_lnum, 1_lcol ) );
    REQUIRE( selection.serializeCapturedText( true, {} ) == payload );

    selection.crop( 1_lnum );
    REQUIRE( selection.getLines() == klogg::vector<LineNumber>{ 0_lnum } );
    REQUIRE( selection.getSelectedLinesCount() == 3_lcount );
    REQUIRE( selection.serializeCapturedText( true, {} ) == payload );
    REQUIRE( retained.getLines() == klogg::vector<LineNumber>{ 2_lnum, 5_lnum } );

    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 0_lcol ), 1_lnum, QStringLiteral( "x" ) } } );
    REQUIRE( selection.getSelectedText( nullptr ) == QStringLiteral( "x" ) );
    selection.clear();
    REQUIRE( selection.isEmpty() );
    REQUIRE( retained.serializeCapturedText( true, {} ) == payload );
    REQUIRE( retained.getSelectedLinesCount() == 3_lcount );
}

TEST_CASE( "cropping a copied mixed captured batch isolates legacy payload removal",
           "[selection][selection-copy-isolation]" )
{
    const klogg::LogRowId firstId{ klogg::LogRowId::Origin::Memory, 22, 1, 0 };
    const klogg::LogRowId hiddenId{ klogg::LogRowId::Origin::Memory, 22, 2, 0 };
    Selection original;
    original.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 0_lcol ), 1_lnum, QStringLiteral( "a" ), firstId, 5_lnum },
          { Portion( 1_lnum, 0_lcol, 0_lcol ), 2_lnum, QStringLiteral( "b" ) },
          { Portion( 2_lnum, 0_lcol, 0_lcol ), 3_lnum, QStringLiteral( "c" ), hiddenId, {} },
          { Portion( 4_lnum, 0_lcol, 0_lcol ), 5_lnum, QStringLiteral( "d" ) } } );
    Selection cropped = original;
    cropped.crop( 2_lnum );
    REQUIRE( cropped.getSelectedLinesText( nullptr )
             == QStringList{ QStringLiteral( "a" ), QStringLiteral( "b" ), QStringLiteral( "c" ) } );
    REQUIRE( cropped.getSelectedLinesCount() == 3_lcount );
    REQUIRE( cropped.getLines() == klogg::vector<LineNumber>{ 1_lnum } );
    REQUIRE( original.getSelectedLinesText( nullptr )
             == QStringList{ QStringLiteral( "a" ), QStringLiteral( "b" ), QStringLiteral( "c" ),
                             QStringLiteral( "d" ) } );
    REQUIRE( original.getSelectedLinesCount() == 4_lcount );
    REQUIRE( original.getLines() == klogg::vector<LineNumber>{ 1_lnum, 4_lnum, 5_lnum } );
    cropped.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
        return id == firstId ? 0_lnum : 3_lnum;
    } );
    REQUIRE( cropped.getLines() == klogg::vector<LineNumber>{ 0_lnum, 1_lnum, 3_lnum } );
    REQUIRE( original.getLines() == klogg::vector<LineNumber>{ 1_lnum, 4_lnum, 5_lnum } );
}

TEST_CASE( "ordinary copied batch selections retain independent crop and replacement behavior",
           "[selection][selection-copy-isolation]" )
{
    SelectionStubLogData logData{ { QStringLiteral( "abc" ), QStringLiteral( "def" ),
                                    QStringLiteral( "ghi" ) } };
    Selection original;
    original.selectPortions( { Portion( 0_lnum, 0_lcol, 1_lcol ),
                               Portion( 2_lnum, 1_lcol, 2_lcol ) } );
    Selection cropped = original;
    cropped.crop( 1_lnum );
    REQUIRE( cropped.getSelectedText( &logData ) == QStringLiteral( "ab" ) );
    REQUIRE( cropped.getSelectedLinesCount() == 1_lcount );
    REQUIRE( original.getSelectedLinesText( &logData )
             == QStringList{ QStringLiteral( "ab" ), QStringLiteral( "hi" ) } );
    REQUIRE( original.getSelectedLinesCount() == 2_lcount );
    cropped.toggleLine( 1_lnum );
    REQUIRE( cropped.getSelectedText( &logData ) == QStringLiteral( "def" ) );
    REQUIRE( original.getLines() == klogg::vector<LineNumber>{ 0_lnum, 2_lnum } );
}

TEST_CASE( "captured serialization retains its copied selection while the owner clears",
           "[selection][selection-copy-isolation]" )
{
    Selection owner;
    owner.selectCapturedPortions(
        { { Portion( 0_lnum, 0_lcol, 2_lcol ), 11_lnum, QStringLiteral( "one" ) },
          { Portion( 1_lnum, 0_lcol, 2_lcol ), 22_lnum, QStringLiteral( "two" ) } } );
    const Selection reader = owner;
    size_t checks = 0;
    const auto text = reader.serializeCapturedText( true, [&] {
        if ( ++checks == 1 ) {
            owner.clear();
        }
        return false;
    } );
    REQUIRE( owner.isEmpty() );
    REQUIRE( text.has_value() );
    REQUIRE( *text == QStringLiteral( "11: one" ) + selectionLineSeparator()
                          + QStringLiteral( "22: two" ) );
    REQUIRE( reader.hasCapturedPayload() );
    REQUIRE( reader.getSelectedLinesCount() == 2_lcount );
}

TEST_CASE( "stable occurrence anchors remap whole-match bounds rather than capture endpoints",
           "[selection][selection-stable-navigation]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 31, 1, 0 };
    Selection selection;
    selection.selectCapturedPortions(
        { { Portion( 0_lnum, 4_lcol, 6_lcol ), 1_lnum, QStringLiteral( "cap" ), identity, 2_lnum } } );
    selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                                   FilePosition( 2_lnum, 9_lcol ), identity );
    size_t resolutions = 0;
    selection.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
        REQUIRE( id == identity );
        ++resolutions;
        return 7_lnum;
    } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 7_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 7_lnum, 9_lcol ) );
    REQUIRE( resolutions == 1 );
    REQUIRE( selection.hasStableNavigationAnchor() );
    REQUIRE( selection.getPortionForLine( 7_lnum ).startColumn() == 4_lcol );
    REQUIRE( selection.getSelectedText( nullptr ) == QStringLiteral( "cap" ) );

    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return {}; } );
    REQUIRE( selection.hasStableNavigationAnchor() );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 0_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 4_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 4_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 4_lnum, 9_lcol ) );
}

TEST_CASE( "zero fragment stable occurrence anchors remap hide and reveal without selecting text",
           "[selection][selection-stable-navigation]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 32, 1, 0 };
    Selection selection;
    selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                                   FilePosition( 2_lnum, 3_lcol ), identity );
    size_t resolutions = 0;
    selection.remapCapturedRows( [&]( const klogg::LogRowId& id ) -> OptionalLineNumber {
        REQUIRE( id == identity );
        ++resolutions;
        return 5_lnum;
    } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 5_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 5_lnum, 3_lcol ) );
    REQUIRE( resolutions == 1 );
    REQUIRE( selection.hasStableNavigationAnchor() );
    REQUIRE( selection.isEmpty() );
    REQUIRE_FALSE( selection.isPortion() );
    REQUIRE_FALSE( selection.hasCapturedPayload() );
    REQUIRE_FALSE( selection.canUseAsSearchOperand() );
    REQUIRE( selection.getSelectedText( nullptr ).isEmpty() );

    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return {}; } );
    REQUIRE( selection.hasStableNavigationAnchor() );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 0_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 8_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 8_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 8_lnum, 3_lcol ) );
    REQUIRE( selection.isEmpty() );
}

TEST_CASE( "stable zero-width EOF anchors preserve their relative next-row position",
           "[selection][selection-stable-navigation]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 36, 1, 0 };
    Selection selection;
    selection.setNavigationBounds( FilePosition( 2_lnum, 7_lcol ),
                                   FilePosition( 3_lnum, 0_lcol ), identity );
    // The next-row sentinel is valid even when the occurrence is on the last row.
    selection.crop( 2_lnum );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 7_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 3_lnum, 0_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 5_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 5_lnum, 7_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 6_lnum, 0_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return {}; } );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 0_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 9_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 9_lnum, 7_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 10_lnum, 0_lcol ) );
    REQUIRE( selection.isEmpty() );
}

TEST_CASE( "cropping stable occurrence anchors hides them until their row is revealed",
           "[selection][selection-stable-navigation]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 33, 1, 0 };
    Selection selection;
    SECTION( "captured occurrence" )
    {
        selection.selectCapturedPortions(
            { { Portion( 0_lnum, 4_lcol, 6_lcol ), 1_lnum, QStringLiteral( "cap" ), identity, 5_lnum } } );
    }
    SECTION( "zero fragment occurrence" ) {}
    selection.setNavigationBounds( FilePosition( 5_lnum, 1_lcol ),
                                   FilePosition( 5_lnum, 9_lcol ), identity );
    selection.crop( 3_lnum );
    REQUIRE( selection.hasStableNavigationAnchor() );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 0_lnum, 0_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 0_lnum, 0_lcol ) );
    selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 2_lnum; } );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 9_lcol ) );
}

TEST_CASE( "copied stable occurrence anchors isolate remapping clearing and replacement",
           "[selection][selection-stable-navigation]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 34, 1, 0 };
    Selection owner;
    owner.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                               FilePosition( 2_lnum, 3_lcol ), identity );
    const Selection retained = owner;
    owner.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber { return 7_lnum; } );
    REQUIRE( owner.getPreviousPosition() == FilePosition( 7_lnum, 1_lcol ) );
    REQUIRE( retained.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    REQUIRE( retained.getNextPosition() == FilePosition( 2_lnum, 3_lcol ) );
    REQUIRE( retained.hasStableNavigationAnchor() );
    SECTION( "clear" ) { owner.clear(); }
    SECTION( "ordinary replacement" ) { owner.selectLine( 0_lnum ); }
    SECTION( "captured replacement" )
    {
        owner.selectCapturedPortions(
            { { Portion( 0_lnum, 0_lcol, 0_lcol ), 1_lnum, QStringLiteral( "x" ), identity, 0_lnum } } );
    }
    SECTION( "legacy navigation replacement" )
    {
        owner.setNavigationBounds( FilePosition( 0_lnum, 0_lcol ), FilePosition( 0_lnum, 1_lcol ) );
    }
    REQUIRE_FALSE( owner.hasStableNavigationAnchor() );
    REQUIRE( retained.hasStableNavigationAnchor() );
    REQUIRE( retained.getNextPosition() == FilePosition( 2_lnum, 3_lcol ) );
}

TEST_CASE( "failed stable occurrence resolution preserves its prior navigation bounds",
           "[selection][selection-stable-navigation]" )
{
    const klogg::LogRowId identity{ klogg::LogRowId::Origin::Memory, 35, 1, 0 };
    Selection selection;
    selection.setNavigationBounds( FilePosition( 2_lnum, 1_lcol ),
                                   FilePosition( 2_lnum, 3_lcol ), identity );
    REQUIRE_THROWS_AS(
        selection.remapCapturedRows( []( const klogg::LogRowId& ) -> OptionalLineNumber {
            throw std::runtime_error( "occurrence identity no longer exists" );
        } ),
        std::runtime_error );
    REQUIRE( selection.hasStableNavigationAnchor() );
    REQUIRE( selection.getPreviousPosition() == FilePosition( 2_lnum, 1_lcol ) );
    REQUIRE( selection.getNextPosition() == FilePosition( 2_lnum, 3_lcol ) );
}

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

    const auto expectedText = QStringLiteral( "line one" ) + selectionLineSeparator()
                              + QStringLiteral( "line three" );
    REQUIRE( selection.getSelectedText( &logData ) == expectedText );

    // A range folded into the toggled set is copied in row order as well
    selection.selectRange( 0_lnum, 1_lnum );
    selection.toggleLine( 4_lnum );
    const auto mixedText = selection.getSelectedLinesText( &logData );
    REQUIRE( mixedText
             == QStringList{ QStringLiteral( "line zero" ), QStringLiteral( "line one" ),
                             QStringLiteral( "line four" ) } );
}
