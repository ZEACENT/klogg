/*
 * Copyright (C) 2026
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

#ifndef KLOGG_SEARCHGENERATION_H
#define KLOGG_SEARCHGENERATION_H

#include <QtGlobal>

namespace klogg {

// Returns true when an incoming searchProgressed signal should be dropped
// because its generation does not match the receiver's currently-active
// generation.  Used as the staleness gate in CrawlerWidget::updateFilteredView
// after the disconnect/reconnect-around-replaceCurrentSearch hack was retired
// (see TASK-001 in README.md backlog and docs/PORTABILITY.md).
//
// Both ordering directions are considered stale -- a newer generation arriving
// at a receiver that hasn't observed the corresponding runSearch() yet is just
// as suspect as an older generation arriving after the search was replaced --
// so the contract is plain inequality rather than `incoming < active`.
inline bool isStaleSearchGeneration( quint64 incoming, quint64 active ) noexcept
{
    return incoming != active;
}

} // namespace klogg

#endif
