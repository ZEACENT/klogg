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

#ifndef KLOGG_MATCHER_CACHE_H
#define KLOGG_MATCHER_CACHE_H

#include <memory>
#include <vector>

#include "regularexpression.h"

class PatternMatcher;

class MatcherCache {
  public:
    MatcherCache() = default;

    MatcherCache( const MatcherCache& ) = delete;
    MatcherCache& operator=( const MatcherCache& ) = delete;

    std::unique_ptr<PatternMatcher> acquire( const std::shared_ptr<RegularExpression>& expression );

    void release( std::unique_ptr<PatternMatcher> matcher );

    void clear();

  private:
    std::weak_ptr<RegularExpression> cachedExpression_;
    std::vector<std::unique_ptr<PatternMatcher>> pool_;
};

#endif
