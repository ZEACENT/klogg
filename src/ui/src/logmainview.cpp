/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2017 Nicolas Bonnefon
 * and other contributors
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

// This file implements the LogMainView concrete class.
// Most of the actual drawing and event management is done in AbstractLogView
// Only behaviour specific to the main (top) view is implemented here.

#include "logmainview.h"

#include "abstractlogdata.h"
#include "log.h"
#include "logfiltereddata.h"
#include "overview.h"

#include "shortcuts.h"

LogMainView::LogMainView( const SearchableLogData* newLogData,
                          const QuickFindPattern* const quickFindPattern, Overview* overview,
                          OverviewWidget* overview_widget, QWidget* parent )
    : AbstractLogView( newLogData, quickFindPattern, parent )
{
    filteredData_ = nullptr;

    // The main data has a real (non NULL) Overview
    setOverview( overview, overview_widget );
}

// Just update our internal record.
void LogMainView::useNewFiltering( LogFilteredData* filteredData )
{
    filteredData_ = filteredData;

    if ( getOverview() != nullptr )
        getOverview()->setFilteredData( filteredData_ );

    forceRefresh();
}

AbstractLogData::LineType LogMainView::lineType( LineNumber lineNumber ) const
{
    if ( filteredData_ ) {
        return filteredData_->lineTypeByLine( lineNumber );
    }
    // Folder mode (no LogFilteredData): show the mark bullet for marked lines.
    if ( markProvider_ != nullptr && markProvider_->isMarked( lineNumber ) ) {
        return AbstractLogData::LineTypeFlags::Mark;
    }
    return AbstractLogData::LineTypeFlags::Plain;
}

void LogMainView::doRegisterShortcuts()
{
    LOG_INFO << "Registering shortcuts for main view";
    AbstractLogView::doRegisterShortcuts();
    // LogViewNextMark/LogViewPrevMark are registered by the base; this class
    // only overrides selectNextMark/selectPrevMark below to use the
    // LogFilteredData mark index instead of the default linear walk.
}

void LogMainView::selectNextMark()
{
    std::optional<LineNumber> line;
    if ( filteredData_ != nullptr ) {
        line = filteredData_->getMarkAfter( getViewPosition() );
    }
    else if ( markProvider_ != nullptr ) {
        // Folder mode: navigate the injected per-file mark source.
        line = markProvider_->markAfter( getViewPosition() );
    }
    if ( line.has_value() ) {
        selectAndDisplayLine( *line );
    }
}

void LogMainView::selectPrevMark()
{
    std::optional<LineNumber> line;
    if ( filteredData_ != nullptr ) {
        line = filteredData_->getMarkBefore( getViewPosition() );
    }
    else if ( markProvider_ != nullptr ) {
        line = markProvider_->markBefore( getViewPosition() );
    }
    if ( line.has_value() ) {
        selectAndDisplayLine( *line );
    }
}
