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

#ifndef LINEKIND_H
#define LINEKIND_H

// Identifies what a given visible row represents in a log view.
//
// Plain single-file views only ever produce Data rows. Folder-search results
// additionally interleave Header rows (one per source file group), so the view
// can render full-width group headers and route clicks on them to
// collapse/expand instead of selection.
enum class LineKind {
    Data,
    Header,
};

#endif // LINEKIND_H
