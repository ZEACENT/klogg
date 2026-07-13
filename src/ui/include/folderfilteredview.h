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

#ifndef FOLDERFILTEREDVIEW_H
#define FOLDERFILTEREDVIEW_H

#include "abstractlogview.h"
#include "foldersearchresults.h"

// The bottom (results) view for folder-wide search, the folder-mode analogue of
// FilteredView. It reads a FolderSearchResults: visible rows are either Data
// (a matched line) or Header (a per-file group header). The base class draws
// each row's text; this class only supplies the per-row metadata (line kind for
// header-vs-data branching, the source-file local line number for the gutter,
// and Match line-type so match rows get the match bullet style).
class FolderFilteredView : public AbstractLogView {
    Q_OBJECT

  public:
    FolderFilteredView( FolderSearchResults* results, const QuickFindPattern* const quickFindPattern,
                        QWidget* parent = nullptr );

  protected:
    AbstractLogData::LineType lineType( LineNumber lineNumber ) const override;
    LineNumber displayLineNumber( LineNumber lineNumber ) const override;
    LineNumber maxDisplayLineNumber() const override;
    LineKind lineKind( LineNumber lineNumber ) const override;

    // All visible lines are folder-search results; search-range graying does
    // not apply.
    bool shouldApplySearchRangeGraying() const override;

  public:
    // Test seam: the line-type the paint path uses for a result row, exposed so
    // headless tests can assert the Mark bullet will render (lineType itself is
    // protected).
    AbstractLogData::LineType lineTypeForTest( LineNumber lineNumber ) const
    {
        return lineType( lineNumber );
    }

  private:
    FolderSearchResults* results_;
};

#endif // FOLDERFILTEREDVIEW_H
