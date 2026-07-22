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

#ifndef KLOGG_EMPTYFILTERPOLICY_H
#define KLOGG_EMPTYFILTERPOLICY_H

namespace klogg {

// What a filtered/results pane should display while the search filter is
// empty.  The choice is per pane KIND, and the split is deliberate:
//
//  - Single-document lens views (CrawlerWidget: file tabs AND live-source
//    tabs -- adb logcat / iOS streams share the same widget and a plain
//    LogFilteredData) honor the user's showAllInFilteredViewWhenSearchEmpty
//    preference.  MirrorAllLines turns the filtered window into an
//    unfiltered mirror of the document without launching a search scan;
//    ClearResults leaves only marked lines (upstream behavior, issue #46).
//
//  - Cross-file grep results panes (FolderCrawlerWidget) always use
//    ClearResults: "every line of every file" is not a meaningful result
//    set, so an empty search there just clears the pane.
//
// Layering: the DECISION lives in the UI layer (it owns the search-text
// state); the MECHANISM lives in the data layer
// (LogFilteredData::setAllLinesVisible).  LogFilteredData resets the
// mechanism on every clearSearch/runSearch/updateSearch, so the widget
// re-asserts the resolved policy on each search/load/config transition.
enum class EmptyFilterPolicy {
    ClearResults,
    MirrorAllLines,
};

// Resolves the empty-filter policy for a single-document lens view from the
// current search text and the user preference.  Pure function with exactly
// one definition so CrawlerWidget's search/load/config code paths cannot
// silently diverge (and so the rule is unit-testable without a widget).
inline EmptyFilterPolicy emptyLensFilterPolicy( bool searchTextEmpty,
                                                bool showAllWhenEmptyPreferred )
{
    return searchTextEmpty && showAllWhenEmptyPreferred ? EmptyFilterPolicy::MirrorAllLines
                                                        : EmptyFilterPolicy::ClearResults;
}

} // namespace klogg

#endif // KLOGG_EMPTYFILTERPOLICY_H
