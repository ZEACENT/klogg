/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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

#ifndef KLOGG_REGULAR_EXPRESSION_PATTERN_H
#define KLOGG_REGULAR_EXPRESSION_PATTERN_H

#include <QRegularExpression>
#include <QString>
#include <atomic>
#include <cstdint>
#include <string>
#include <tuple>

#include "uuid.h"

struct RegularExpressionPattern {

    QString pattern;
    bool isCaseSensitive = true;
    bool isExclude = false;
    bool isBoolean = false;
    bool isPlainText = false;
    bool isPrefilter = false;

    RegularExpressionPattern() = default;

    explicit RegularExpressionPattern( const QString& expression )
        : RegularExpressionPattern( expression, true, false, false, false )
    {
    }

    RegularExpressionPattern( const QString& expression, bool caseSensitive, bool inverse,
                              bool boolean, bool plainText )
        : pattern( expression )
        , isCaseSensitive( caseSensitive )
        , isExclude( inverse )
        , isBoolean( boolean )
        , isPlainText( plainText )
        , patternId_( nextId() )
    {
    }

    std::string id() const
    {
        return patternId_;
    }

    // Capture groups participate in matching semantics: suppressing unnamed
    // groups invalidates or renumbers backreferences and capture conditionals.
    // Predicate and occurrence consumers must compile the same expression.
    static QRegularExpression::PatternOptions qtPatternOptions( bool caseSensitive )
    {
        QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
        if ( !caseSensitive ) {
            options |= QRegularExpression::CaseInsensitiveOption;
        }
        return options;
    }

    QRegularExpression toRegularExpression() const
    {
        const auto finalPattern = isPlainText ? QRegularExpression::escape( pattern ) : pattern;
        return QRegularExpression{ finalPattern, qtPatternOptions( isCaseSensitive ) };
    }

    explicit operator QRegularExpression() const
    {
        return toRegularExpression();
    }

    bool operator==( const RegularExpressionPattern& other ) const
    {
        return std::tie( pattern, isCaseSensitive, isExclude, isBoolean, isPlainText )
               == std::tie( other.pattern, other.isCaseSensitive, isExclude, isBoolean,
                            isPlainText );
    }

  private:
    static std::string nextId()
    {
        static std::atomic<uint> counter_ = 0;
        return std::string{ "p_" } + std::to_string( counter_++ );
    }

    std::string patternId_;
};

#endif