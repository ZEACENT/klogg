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

#include "matchercache.h"

#include "log.h"

std::unique_ptr<PatternMatcher> MatcherCache::acquire(
    const std::shared_ptr<RegularExpression>& expression )
{
    if ( !expression ) {
        return nullptr;
    }

    const auto cached = cachedExpression_.lock();

    // If the expression changed or the prior one expired, evict all
    // pooled matchers to avoid returning stale matchers compiled for
    // a different pattern.
    if ( !cached || cached != expression ) {
        pool_.clear();
    }

    cachedExpression_ = expression;

    // Try to reuse a pooled matcher.
    if ( !pool_.empty() ) {
        auto matcher = std::move( pool_.back() );
        pool_.pop_back();
        return matcher;
    }

    // No pooled matcher available — create a new one.
    return expression->createMatcher();
}

void MatcherCache::release( std::unique_ptr<PatternMatcher> matcher )
{
    if ( matcher ) {
        pool_.push_back( std::move( matcher ) );
    }
}

void MatcherCache::clear()
{
    pool_.clear();
    cachedExpression_.reset();
}
