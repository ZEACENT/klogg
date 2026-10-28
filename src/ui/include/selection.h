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

#ifndef SELECTION_H
#define SELECTION_H

#include <QList>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "linetypes.h"
#include "logsnapshot.h"

class AbstractLogData;

class Portion {
  public:
    Portion()
        : line_{}
        , startColumn_{ -1 }
        , endColumn_{ -1 }
    {
    }

    Portion( LineNumber line, LineColumn startColumn, LineColumn endColumn )
        : line_{ line }
        , startColumn_{ startColumn }
        , endColumn_{ endColumn }
    {
    }

    // Display columns drive painting and navigation. Optional inclusive raw
    // columns let batch selections copy original text without expanding tabs.
    Portion( LineNumber line, LineColumn startColumn, LineColumn endColumn,
             LineColumn rawStartColumn, LineColumn rawEndColumn )
        : Portion( line, startColumn, endColumn )
    {
        rawColumns_ = std::make_pair( rawStartColumn, rawEndColumn );
    }

    const std::optional<std::pair<LineColumn, LineColumn>>& rawColumns() const
    {
        return rawColumns_;
    }

    LineNumber line() const
    {
        return *line_;
    }
    LineColumn startColumn() const
    {
        return startColumn_;
    }
    LineColumn endColumn() const
    {
        return endColumn_;
    }

    bool isValid() const
    {
        return !!line_ && endColumn_ >= startColumn_ && startColumn_ >= 0_lcol
               && ( !rawColumns_ || ( rawColumns_->first >= 0_lcol
                                      && rawColumns_->second >= rawColumns_->first ) );
    }

    LineLength size() const
    {
        return ( endColumn_ - startColumn_ ) + 1_length;
    }

  private:
    OptionalLineNumber line_;
    LineColumn startColumn_;
    LineColumn endColumn_;
    std::optional<std::pair<LineColumn, LineColumn>> rawColumns_;
};

// Represents a selection in an AbstractLogView
class Selection {
  public:
    // Construct an empty selection
    Selection();

    // Clear the selection
    void clear()
    {
        selectedPartial_.line = {};
        selectedLine_ = {};
        selectedRange_.startLine = {};
        portionPayload_.reset();
        portionLayout_.reset();
        navigationBounds_.reset();
        toggledRanges_.clear();
        lastToggledLine_ = {};
    }

    // Select one line
    void selectLine( LineNumber line )
    {
        clear();
        selectedLine_ = line;
    }
    // Select a portion of line (both start and end included)
    void selectPortion( LineNumber line, LineColumn startColumn, LineColumn endColumn );
    void selectPortion( const Portion& selection )
    {
        selectPortion( selection.line(), selection.startColumn(), selection.endColumn() );
    }
    enum class PortionOverlap : std::uint8_t { Merge, Preserve };

    // Select partial fragments in stable row/start order, ignoring invalid ones.
    // Merge unions compatible overlaps, never adjacency. Preserve keeps distinct
    // overlapping occurrences separate after caller-side capture merging.
    void selectPortions( std::vector<Portion> portions,
                         PortionOverlap overlap = PortionOverlap::Merge );
    struct CapturedPortion {
        Portion portion;
        LineNumber sourceLineNumber;
        QString text;
        std::optional<klogg::LogRowId> identity = std::nullopt;
        OptionalLineNumber presentedRow = std::nullopt;
    };

    // Own the captured text and its one-based source line label (getLineNumber()).
    // Sort by portion row/start and preserve distinct overlaps. With a stable identity,
    // portion.line() is immutable snapshot order and presentedRow is current geometry
    // (nullopt means hidden); legacy entries use portion.line() as their display row.
    // Ignore invalid/empty entries. Caller establishes row eligibility; copying never
    // consults logData, and hidden fragments remain part of the semantic selection.
    void selectCapturedPortions( std::vector<CapturedPortion> portions );

    // Resolve each stable fragment/occurrence identity once and atomically replace
    // visible geometry and navigation. Payload order/labels are unchanged; legacy
    // entries are not passed to the resolver.
    void remapCapturedRows(
        const std::function<OptionalLineNumber( const klogg::LogRowId& )>& resolveRow );

    // Search operands require one nonempty contiguous fragment, not every
    // selection shape that isPortion() exposes for painting.
    bool canUseAsSearchOperand() const;

    std::vector<Portion> getPortionsForLine( LineNumber line ) const;
    // Identifies either batch-selection mode, including a single surviving fragment.
    bool isMultiPortion() const
    {
        return portionPayload_ != nullptr;
    }

    // Select a range of lines (both start and end included)
    void selectRange( LineNumber startLine, LineNumber endLine );

    // Toggle one line's membership in the selection (ctrl+click behaviour):
    // an unselected line is added, an already selected line is removed.
    // A whole-line or range selection is folded into the toggled set first,
    // so toggling composes with it; a portion selection is superseded.
    void toggleLine( LineNumber line );

    // Select a range from the previously selected line or beginning
    // of range (shift+click behaviour)
    void selectRangeFromPrevious( LineNumber line );

    // Crop to the last display row. Stable captured rows outside the range become
    // hidden without losing payload; ordinary/legacy selections are truncated.
    void crop( LineNumber lastLine );

    // Returns whether the selection is empty
    bool isEmpty() const
    {
        return !selectedPartial_.line && !selectedLine_ && !selectedRange_.startLine
               && !portionPayload_ && toggledRanges_.empty();
    }

    // Returns whether the selection is a single line
    bool isSingleLine() const
    {
        return !!selectedLine_ || isSingleToggledLine();
    }

    // Returns whether the selection is a portion of line
    bool isPortion() const
    {
        return !!selectedPartial_.line || isMultiPortion();
    }

    // Returns whether the selection is a portion of line
    bool isPortion( LineNumber line ) const;

    // Returns the first portion on the passed line, or an invalid portion.
    // Use getPortionsForLine() when all fragments on that line are needed.
    Portion getPortionForLine( LineNumber line ) const;
    // Get selected visible display rows in order, excluding hidden captured rows.
    klogg::vector<LineNumber> getLines() const;

    // Count selected source rows, deduplicating stable identities even when hidden.
    LinesCount getSelectedLinesCount() const;

    // Returns wether the line passed is selected (entirely).
    bool isLineSelected( LineNumber line ) const;

    // Returns wether the line passed is selected in certain range.
    bool isPortionSelected( LineNumber line, LineColumn startColumn, LineColumn endColumn ) const;

    // Returns the line selected or -1 if not a single line selection
    OptionalLineNumber selectedLine() const;

    // Returns captured payloads without accessing logData, or reads the current
    // source for ordinary selections.
    QString getSelectedText( const AbstractLogData* logData, bool lineNumbers = false ) const;

    // Worker-safe serialization of an immutable captured selection copy. No source
    // access is possible. Cancellation or noncaptured input returns nullopt; an empty
    // callback never cancels. Checks occur per fragment, per bounded text chunk,
    // and before returning success. Hidden fragments remain in semantic copy order.
    bool hasCapturedPayload() const;
    std::optional<QString> serializeCapturedText(
        bool withLineNumbers, const std::function<bool()>& cancelled ) const;

    // Returns the selected text as one string per whole line or partial fragment,
    // in row/start order. Ordinary selections skip non-copyable rows, e.g. folder
    // headers; captured selections return their owned payloads without source reads.
    // Multiple fragments on one row yield separate entries. Unlike getSelectedText(),
    // nothing is joined with platform line separators: consumers matching text per log
    // line (e.g. color labels) must use this, as an entry containing a line
    // feed can never match a single line.
    QStringList getSelectedLinesText( const AbstractLogData* logData ) const;

    // Occurrence navigation is independent of capture geometry, including zero
    // fragments. A stable identity names previous.line()'s source row. Remapping
    // preserves columns and next.line()'s relative row delta (e.g. an EOF sentinel).
    // Hidden stable anchors retain semantics but report neutral navigation positions.
    // Ordinary selection replacement/clear retires the anchor.
    void setNavigationBounds( FilePosition previous, FilePosition next,
                              std::optional<klogg::LogRowId> identity = std::nullopt );
    bool hasStableNavigationAnchor() const;

    // Return the position immediately after the current selection
    // (used for searches).
    // This is the next character or the start of the next line.
    FilePosition getNextPosition() const;

    // Idem from the position immediately before selection.
    FilePosition getPreviousPosition() const;

  private:
    // Row-ordered (lineNumber, text) pairs -- NOT a map: folder results can
    // legitimately select the same source line number from DIFFERENT files.
    // Copy follows the selection's immutable semantic row order, not source-label
    // order or a later display remapping.
    std::vector<std::pair<LineNumber, QString>>
    getSelectionWithLineNumbers( const AbstractLogData* logData ) const;

  private:
    // Line number currently selected, or -1 if none selected
    OptionalLineNumber selectedLine_;

    struct SelectedPartial {
        OptionalLineNumber line;
        LineColumn startColumn;
        LineColumn endColumn;

        LineLength size() const
        {
            return ( endColumn - startColumn ) + 1_length;
        }
    };
    struct SelectedRange {
        // The limits of the range, sorted
        OptionalLineNumber startLine;
        LineNumber endLine;
        // The line selected first, used for shift+click
        LineNumber firstLine;

        LinesCount size() const
        {
            return startLine ? ( endLine - startLine.value() ) + 1_lcount : 0_lcount;
        }
    };
    struct SelectedPartial selectedPartial_;
    struct SelectedRange selectedRange_;

    struct NavigationBounds {
        FilePosition previous;
        FilePosition next;
        std::optional<klogg::LogRowId> identity;
        OptionalLineNumber presentedRow;
    };
    std::optional<NavigationBounds> navigationBounds_;

    struct SelectedPortion {
        Portion portion;
        // Only semantic entries own payload; the display index stores no text.
        // Batch setters keep ownership homogeneous: either every entry is captured
        // or none is. Cropping/remapping cannot change that ownership mode.
        std::optional<std::pair<LineNumber, QString>> capturedText;
        std::optional<klogg::LogRowId> identity;
    };

    struct PortionPayload {
        // Semantic order never changes with presentation. Distinct occurrences may overlap.
        std::vector<SelectedPortion> portions;
        LinesCount lineCount = 0_lcount;
    };
    struct PortionLayout {
        // Geometry is separate from payload: remapping does not copy fragment text entries.
        std::vector<OptionalLineNumber> rows;
        std::vector<std::size_t> visible;
    };

    // Published batch values are immutable. Copying a captured selection into a
    // worker copies these two handles, not any per-fragment payload or layout vectors.
    // Empty batches use null handles; replacement/cropping cannot mutate other readers.
    std::shared_ptr<const PortionPayload> portionPayload_;
    std::shared_ptr<const PortionLayout> portionLayout_;
    const std::vector<SelectedPortion>& portions() const;
    const PortionLayout& layout() const;
    void setPortions( std::vector<SelectedPortion> entries,
                      std::vector<OptionalLineNumber> rows );
    static std::shared_ptr<const PortionLayout>
    makePortionLayout( const std::vector<SelectedPortion>& entries,
                       std::vector<OptionalLineNumber> rows );
    LineNumber displayedRow( std::size_t index ) const;
    Portion displayedPortion( std::size_t index ) const;
    std::vector<std::size_t>::const_iterator firstPortionForLine( LineNumber line ) const;

    // Inclusive [firstLine, lastLine] intervals toggled line-by-line
    // (ctrl+click), kept ascending, non-overlapping and non-adjacent
    // (merge-on-insert). Mutually exclusive with the primary selection above:
    // toggleLine folds any whole-line/range selection into this set and the
    // select*/clear members empty it (replace semantics).
    std::vector<std::pair<LineNumber, LineNumber>> toggledRanges_;

    // Most recent ctrl-clicked line, used as the shift+click anchor once the
    // selection lives entirely in the toggled set.
    OptionalLineNumber lastToggledLine_;

    // Returns whether the whole selection is exactly one toggled line.
    bool isSingleToggledLine() const
    {
        return toggledRanges_.size() == 1
               && toggledRanges_.front().first == toggledRanges_.front().second;
    }

    // Insert [firstLine, lastLine] into toggledRanges_, merging every
    // interval it overlaps or touches.
    void insertToggledRange( LineNumber firstLine, LineNumber lastLine );
    // Returns whether the line is covered by one of the toggled ranges.
    bool isInToggledRanges( LineNumber line ) const;
};

#endif
