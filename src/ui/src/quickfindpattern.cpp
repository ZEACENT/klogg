/*
 * Copyright (C) 2010 Nicolas Bonnefon and other contributors
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

// This file implements QuickFindPattern.
// This class implements part of the Quick Find mechanism, it only stores the
// current search pattern, once it has been confirmed (return pressed),
// it can be asked to return the matches in a specific string.

#include <qregularexpression.h>

#include "linetypes.h"
#include "quickfindpattern.h"
#include "matchprojection.h"
#include "regularexpressionpattern.h"

#include "configuration.h"

#include "quickfind.h"

constexpr Qt::GlobalColor QfForeColor = Qt::black;

bool QuickFindMatcher::isLineMatching( const QString& line, LineColumn column ) const
{
    if ( !isActive_ )
        return false;

    lastOccurrence_ = regexp_.match( line, column.get() );
    return lastOccurrence_.hasMatch();
}

bool QuickFindMatcher::isLineMatchingBackward( const QString& line, LineColumn column ) const
{
    if ( !isActive_ )
        return false;

    auto matches = regexp_.globalMatch( line );
    lastOccurrence_ = {};
    while ( matches.hasNext() ) {
        const auto nextMatch = matches.next();
        if ( column.get() >= 0
             && ( nextMatch.capturedStart() >= column.get()
                  || nextMatch.capturedEnd() > column.get() ) ) {
            break;
        }
        lastOccurrence_ = nextMatch;
    }
    return lastOccurrence_.hasMatch();
}

std::pair<LineColumn, LineColumn> QuickFindMatcher::getLastMatch() const
{
    return { LineColumn{ lastOccurrence_.capturedStart() },
             LineColumn{ lastOccurrence_.capturedEnd() - 1 } };
}

void QuickFindPattern::changeSearchPattern( const QString& pattern, bool isRegex, bool isWholeWord )
{
    pattern_ = pattern;
    isRegex_ = isRegex;
    isWholeWord_ = isWholeWord;

    const RegularExpressionPattern query{ pattern, isCaseSensitive_, false, false, !isRegex };
    regexp_ = query.toRegularExpression();
    if ( isWholeWord ) {
        regexp_.setPattern( "\\b" + regexp_.pattern() + "\\b" );
    }

    if ( regexp_.isValid() && ( !pattern_.isEmpty() ) )
        active_ = true;
    else
        active_ = false;

    Q_EMIT patternUpdated();
}

void QuickFindPattern::changeSearchPattern( const QString& pattern, bool ignoreCase, bool isRegex, bool isWholeWord )
{
    isCaseSensitive_ = !ignoreCase;
    changeSearchPattern( pattern, isRegex, isWholeWord );
}

bool QuickFindPattern::matchLine( const QString& line,
                                  klogg::vector<HighlightedMatch>& matches ) const
{
    matches.clear();

    if ( active_ ) {
        const auto backColor = Configuration::get().qfBackColor();
        klogg::visitLineMatchFragments(
            regexp_, line, klogg::MatchProjection::CapturesOrFullMatch,
            [ & ]( LineColumn start, LineLength length ) {
                matches.emplace_back( start, length, QfForeColor, backColor );
            } );
    }

    return ( !matches.empty() );
}

QuickFindMatcher QuickFindPattern::getMatcher() const
{
    return QuickFindMatcher( active_, regexp_ );
}
