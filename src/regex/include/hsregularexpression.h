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

#ifndef KLOGG_HS_REGULAR_EXPRESSION
#define KLOGG_HS_REGULAR_EXPRESSION

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <QRegularExpression>
#include <QString>

#include "containers.h"

#ifdef KLOGG_HAS_VECTORSCAN
#include <hs.h>

#include "resourcewrapper.h"
#endif

#include "regularexpressionpattern.h"

using MatchedPatterns = std::string;

class DefaultRegularExpressionMatcher {
  public:
    explicit DefaultRegularExpressionMatcher(
        const klogg::vector<RegularExpressionPattern>& patterns )
    {
        std::transform(
            patterns.cbegin(), patterns.cend(), std::back_inserter( regexp_ ),
            []( const auto& pattern ) { return static_cast<QRegularExpression>( pattern ); } );
    }

    MatchedPatterns match( const std::string_view& utf8Data ) const
    {
        MatchedPatterns matchedPatterns( regexp_.size(), 0 );
        std::transform( regexp_.cbegin(), regexp_.cend(), matchedPatterns.begin(),
                        [ utf8Data ]( const auto& regexp ) {
                            return regexp
                                .match(
                                    QString::fromUtf8( utf8Data.data(), klogg::isize( utf8Data ) ) )
                                .hasMatch();
                            ;
                        } );

        return matchedPatterns;
    }

  private:
    klogg::vector<QRegularExpression> regexp_;
};

#ifdef KLOGG_HAS_VECTORSCAN

using HsScratch = UniqueResource<hs_scratch_t, hs_free_scratch>;
using HsDatabase = SharedResource<hs_database_t>;

struct HsMatcherContext {

    HsMatcherContext( std::size_t numberOfPatterns = 1 );

    void reset();

    MatchedPatterns matchingPatterns;

  private:
    MatchedPatterns matchingPatternsTemplate_;
};

class HsMatcher {
  public:
    HsMatcher() = default;
    HsMatcher( HsDatabase database, HsScratch scratch, std::size_t numberOfPatterns );

    HsMatcher( const HsMatcher& ) = delete;
    HsMatcher& operator=( const HsMatcher& ) = delete;

    HsMatcher( HsMatcher&& other ) = default;
    HsMatcher& operator=( HsMatcher&& other ) = default;

  protected:
    HsDatabase database_;
    HsScratch scratch_;

    mutable HsMatcherContext context_;
};

class HsSingleMatcher : public HsMatcher {
  public:
    HsSingleMatcher() = default;
    HsSingleMatcher( HsDatabase database, HsScratch scratch );

    MatchedPatterns match( const std::string_view& utf8Data ) const;
};

class HsMultiMatcher : public HsMatcher {
  public:
    HsMultiMatcher() = default;
    HsMultiMatcher( HsDatabase database, HsScratch scratch, std::size_t numberOfPatterns );

    MatchedPatterns match( const std::string_view& utf8Data ) const;
};

class HsNoopMatcher {
  public:
    MatchedPatterns match( const std::string_view& utf8Data ) const;
};

class HsPrefilterMatcher {
  public:
    HsPrefilterMatcher(const klogg::vector<RegularExpressionPattern>& patterns, HsMultiMatcher&& hsMatcher);

    MatchedPatterns match( const std::string_view& utf8Data ) const;
  
  private:
    klogg::vector<RegularExpressionPattern> patterns_;
    HsMultiMatcher hsMatcher_;
};

// Buffer scanner for bulk scanning -- scans an entire multi-line buffer in one
// hs_scan() call and maps match positions back to line numbers via binary
// search on endOfLines offsets.
class HsBufferScanner {
  public:
    HsBufferScanner() = default;
    HsBufferScanner( HsDatabase database, HsScratch scratch, std::size_t numberOfPatterns );

    HsBufferScanner( const HsBufferScanner& ) = delete;
    HsBufferScanner& operator=( const HsBufferScanner& ) = delete;

    HsBufferScanner( HsBufferScanner&& other ) = default;
    HsBufferScanner& operator=( HsBufferScanner&& other ) = default;

    bool isValid() const { return database_ != nullptr && scratch_ != nullptr; }

    // Scan the entire buffer and populate matchedLineIndices with local line
    // indices (0-based within the buffer) that matched.  For single-pattern
    // mode, any match marks the line.  For multi-pattern boolean mode,
    // perLinePatterns is populated instead (caller evaluates the boolean).
    void scan( const char* data, unsigned int size,
               const klogg::vector<qint64>& endOfLines,
               klogg::vector<uint64_t>& matchedLineIndices ) const;

    // Multi-pattern variant: sets perLinePatterns[lineIndex][patternId] to
    // a non-zero char (MatchedPatterns is a packed boolean std::string)
    void scanMulti( const char* data, unsigned int size,
                    const klogg::vector<qint64>& endOfLines,
                    std::vector<MatchedPatterns>& perLinePatterns ) const;

    std::size_t numberOfPatterns() const { return numberOfPatterns_; }

  private:
    HsDatabase database_;
    HsScratch scratch_;
    std::size_t numberOfPatterns_ = 0;
};

using MatcherVariant
    = std::variant<DefaultRegularExpressionMatcher, HsNoopMatcher, HsSingleMatcher, HsMultiMatcher, HsPrefilterMatcher>;


class HsRegularExpression {
  public:
    HsRegularExpression() = default;
    explicit HsRegularExpression( const RegularExpressionPattern& includePattern );
    explicit HsRegularExpression( const klogg::vector<RegularExpressionPattern>& patterns );

    HsRegularExpression( const HsRegularExpression& ) = delete;
    HsRegularExpression& operator=( const HsRegularExpression& ) = delete;

    HsRegularExpression( HsRegularExpression&& other ) = default;
    HsRegularExpression& operator=( HsRegularExpression&& other ) = default;

    bool isValid() const;
    QString errorString() const;

    MatcherVariant createMatcher() const;

    // Create a buffer scanner for bulk scanning (no HS_FLAG_SINGLEMATCH).
    // Returns a valid scanner only when Vectorscan is available and the
    // database compiled successfully.
    std::unique_ptr<HsBufferScanner> createBufferScanner() const;

    bool hasBufferScanner() const;

  private:
    bool isHsValid() const;

  private:
    HsDatabase database_;
    HsScratch scratch_;

    // Block database compiled WITHOUT HS_FLAG_SINGLEMATCH for buffer scanning.
    HsDatabase blockDatabase_;
    HsScratch blockScratch_;

    klogg::vector<RegularExpressionPattern> patterns_;

    bool isValid_ = true;
    QString errorMessage_;

    bool isPrefilter_ = false;
};
#else

using MatcherVariant = std::variant<DefaultRegularExpressionMatcher>;

class HsRegularExpression {
  public:
    HsRegularExpression() = default;

    explicit HsRegularExpression( const RegularExpressionPattern& includePattern )
        : HsRegularExpression( klogg::vector<RegularExpressionPattern>{ includePattern } )
    {
    }

    explicit HsRegularExpression( const klogg::vector<RegularExpressionPattern>& patterns )
        : patterns_( patterns )
    {
        for ( const auto& pattern : patterns_ ) {
            const auto& regex = static_cast<QRegularExpression>( pattern );
            if ( !regex.isValid() ) {
                isValid_ = false;
                errorString_ = regex.errorString();
                break;
            }
        }
    }

    bool isValid() const
    {
        return isValid_;
    }

    QString errorString() const
    {
        return errorString_;
    }

    MatcherVariant createMatcher() const
    {
        return MatcherVariant{ DefaultRegularExpressionMatcher( patterns_ ) };
    }

  private:
    bool isValid_ = true;
    QString errorString_;

    klogg::vector<RegularExpressionPattern> patterns_;
};

#endif

#endif
