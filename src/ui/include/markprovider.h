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

#ifndef MARKPROVIDER_H
#define MARKPROVIDER_H

#include "linetypes.h"

#include <optional>

// Read-only mark source injected into views that have no LogFilteredData (the
// folder main view). Supplies "is this line marked" plus the next/previous
// marked line, mirroring the subset of LogFilteredData the mark-bullet
// rendering (LineTypeFlags::Mark) and the LogViewNextMark/LogViewPrevMark
// navigation consult. Lets folder-mode marks reuse the single-file view's mark
// rendering/navigation without re-introducing LogFilteredData. The implementer
// must outlive any view it is set on.
class MarkProvider {
  public:
    virtual ~MarkProvider() = default;
    virtual bool isMarked( LineNumber line ) const = 0;
    virtual std::optional<LineNumber> markAfter( LineNumber line ) const = 0;
    virtual std::optional<LineNumber> markBefore( LineNumber line ) const = 0;
};

#endif // MARKPROVIDER_H
