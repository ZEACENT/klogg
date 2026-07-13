/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
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

#include "folderfilteredview.h"

#include "linetypes.h"

FolderFilteredView::FolderFilteredView( FolderSearchResults* results,
                                        const QuickFindPattern* const quickFindPattern,
                                        QWidget* parent )
    : AbstractLogView( results, quickFindPattern, parent )
    , results_( results )
{
}

AbstractLogData::LineType FolderFilteredView::lineType( LineNumber lineNumber ) const
{
    // Header rows are decorative.
    if ( results_ != nullptr && results_->lineKind( lineNumber ) == LineKind::Header ) {
        return {};
    }
    // A grep -A/-B/-C context row renders PLAIN (no Match bullet); a real Match
    // row carries Match. Either may also carry Mark if the user marked that
    // source line. Parity with single-file FilteredView's Match|Mark.
    AbstractLogData::LineType flags{};
    if ( results_ != nullptr && results_->isMatchRow( lineNumber ) ) {
        flags |= AbstractLogData::LineTypeFlags::Match;
    }
    if ( markProvider_ != nullptr && markProvider_->isMarked( lineNumber ) ) {
        flags |= AbstractLogData::LineTypeFlags::Mark;
    }
    return flags;
}

LineNumber FolderFilteredView::displayLineNumber( LineNumber lineNumber ) const
{
    // The gutter shows the matched line's 1-based number within its source file.
    // Header rows have no number (and the base class skips drawing it for them).
    if ( results_ == nullptr ) {
        return 0_lnum;
    }
    if ( results_->lineKind( lineNumber ) == LineKind::Header ) {
        return 0_lnum;
    }
    const auto source = results_->sourceForLine( lineNumber );
    return source.localLine + 1_lcount;
}

LineNumber FolderFilteredView::maxDisplayLineNumber() const
{
    if ( results_ == nullptr ) {
        return 0_lnum;
    }
    return results_->maxLocalLine() + 1_lcount;
}

LineKind FolderFilteredView::lineKind( LineNumber lineNumber ) const
{
    return results_ != nullptr ? results_->lineKind( lineNumber ) : LineKind::Data;
}

bool FolderFilteredView::shouldApplySearchRangeGraying() const
{
    return false;
}
