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

#include "containers.h"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <qregularexpression.h>
#include <string_view>

#ifdef KLOGG_HAS_VECTORSCAN
#include "hsregularexpression.h"

#include "cpu_info.h"
#include "log.h"
#if defined( KLOGG_MSVC_ASAN )
#include <cstdlib>
#else
#include <mimalloc.h>
#endif

namespace {

// A multi-line block is equivalent to independent records only when every
// consuming atom excludes LF and anchors keep record-local semantics. This is
// a deliberately bounded capability proof, not another regex implementation:
// constructs we cannot prove safe keep the existing per-record matcher.
bool supportsRecordLocalBlockScan( const QString& pattern )
{
    for ( klogg::ContainerIndex i = 0; i < pattern.size(); ) {
        const auto current = pattern.at( i );
        if ( current == QLatin1Char( '\n' ) ) return false;
        if ( current == QLatin1Char( '[' ) ) {
            auto end = i + 1;
            if ( end < pattern.size() && pattern.at( end ) == QLatin1Char( '^' ) ) ++end;
            if ( end < pattern.size() && pattern.at( end ) == QLatin1Char( ']' ) ) ++end;
            for ( ; end < pattern.size(); ++end ) {
                if ( pattern.at( end ) == QLatin1Char( '\\' ) ) {
                    ++end;
                    continue;
                }
                // Nested/POSIX classes need a different lexer; no speculative fast path.
                if ( pattern.at( end ) == QLatin1Char( '[' ) ) return false;
                if ( pattern.at( end ) == QLatin1Char( ']' ) ) break;
            }
            if ( end >= pattern.size() ) return false;
            const QRegularExpression atom{
                QStringLiteral( "\\A(?:" ) + pattern.mid( i, end - i + 1 ) + QStringLiteral( ")\\z" ),
                QRegularExpression::UseUnicodePropertiesOption };
            if ( !atom.isValid() || atom.match( QStringLiteral( "\n" ) ).hasMatch() ) return false;
            i = end + 1;
            continue;
        }
        if ( current == QLatin1Char( '\\' ) ) {
            if ( i + 1 >= pattern.size() ) return false;
            const auto escaped = pattern.at( i + 1 );
            if ( escaped == QLatin1Char( 'Q' ) ) {
                const auto end = pattern.indexOf( QStringLiteral( "\\E" ), i + 2 );
                const auto quotedEnd = end < 0 ? pattern.size() : end;
                if ( pattern.mid( i + 2, quotedEnd - i - 2 ).contains( QLatin1Char( '\n' ) ) ) return false;
                i = end < 0 ? pattern.size() : end + 2;
                continue;
            }
            // These atoms either cannot consume LF or are local zero-width
            // boundaries. Absolute anchors, LF-capable escapes (including \s),
            // backreferences, and other unproven escapes must not scan blocks.
            if ( escaped == QLatin1Char( '\n' ) || escaped.isDigit()
                 || ( escaped.isLetter()
                      && !QStringLiteral( "dwhSbBtrfae" ).contains( escaped ) ) ) return false;
            i += 2;
            continue;
        }
        if ( current == QLatin1Char( '(' ) && i + 1 < pattern.size()
             && pattern.at( i + 1 ) == QLatin1Char( '?' ) ) {
            bool knownHeader = false;
            for ( const auto* header : { "(?:", "(?i)", "(?i:", "(?-i)", "(?-i:", "(?m)", "(?m:" } ) {
                const auto text = QString::fromLatin1( header );
                if ( pattern.mid( i, text.size() ) == text ) {
                    i += text.size();
                    knownHeader = true;
                    break;
                }
            }
            if ( !knownHeader ) return false; // Includes dotall, lookarounds, and absolute-mode changes.
            continue;
        }
        ++i;
    }
    return true;
}

// Wire Vectorscan to use mimalloc for internal allocations (once per process).
struct HsAllocatorInit {
    HsAllocatorInit()
    {
#if defined( KLOGG_MSVC_ASAN )
        // MSVC ASan cannot use mimalloc's GCC/Clang-only MI_TRACK_ASAN mode.
        // Keep Vectorscan allocations visible to the CRT allocator interceptor.
        hs_set_allocator( std::malloc, std::free );
#else
        hs_set_allocator( mi_malloc, mi_free );
#endif
    }
};
static HsAllocatorInit hsAllocatorInit;

int matchSingleCallback( unsigned int id, unsigned long long from, unsigned long long to,
                         unsigned int flags, void* context )
{
    Q_UNUSED( id );
    Q_UNUSED( from );
    Q_UNUSED( to );
    Q_UNUSED( flags );

    auto* matchContext = static_cast<HsMatcherContext*>( context );

    matchContext->matchingPatterns[ 0 ] = true;
    return 1;
}

int matchMultiCallback( unsigned int id, unsigned long long from, unsigned long long to,
                        unsigned int flags, void* context )
{
    Q_UNUSED( from );
    Q_UNUSED( to );
    Q_UNUSED( flags );

    auto* matchContext = static_cast<HsMatcherContext*>( context );

    matchContext->matchingPatterns[ id ] = true;

    return 0;
}

// Callback for buffer scanning -- records which lines contain matches.
// The context carries endOfLines offsets for binary-search mapping.
struct BufferScanContext {
    const klogg::vector<qint64>* endOfLines;
    qint64 bufferSize = 0;
    bool endsWithSeparator = false;
    // For multi-pattern boolean mode:
    std::vector<MatchedPatterns>* perLinePatterns;
    // Occupancy bitset (one byte per line) for O(1) dedup with zero per-match
    // allocation: a line that matches in several positions triggers several
    // callbacks, all mapping to the same line index; only the first sets the
    // bit. The scanner collects set bits in ascending order after the scan, so
    // the output is sorted and deduped by construction. Sized to
    // endOfLines.size() by HsBufferScanner::scan.
    std::vector<unsigned char>* seenLines;
};

int bufferScanSingleCallback( unsigned int id, unsigned long long from, unsigned long long to,
                               unsigned int flags, void* context )
{
    Q_UNUSED( id );
    Q_UNUSED( flags );

    auto* ctx = static_cast<BufferScanContext*>( context );
    // Block compilation requests SOM, so `from` is an actual start offset.
    // Without SOM, Vectorscan reports zero here even for later zero-width
    // matches, incorrectly attributing those matches to the preceding row.
    // An empty match at EOF belongs to an unterminated final record, but not
    // to an imaginary record after the final LF.
    const bool finalRecordEnd = !ctx->endsWithSeparator
        && to == static_cast<unsigned long long>( ctx->bufferSize );
    const auto matchByte = static_cast<qint64>(
        to > 0 && ( from < to || finalRecordEnd ) ? to - 1 : to );
    auto it = std::upper_bound( ctx->endOfLines->cbegin(), ctx->endOfLines->cend(), matchByte );
    const auto lineIndex = static_cast<uint64_t>( std::distance( ctx->endOfLines->cbegin(), it ) );

    // Deduplicate (idempotent bit set) with a bounds guard: endOfLines must be
    // sized so its back() >= scanned size (a sentinel for an unterminated final
    // line), which keeps lineIndex < seenLines->size(); the guard is defensive.
    if ( lineIndex < ctx->seenLines->size() ) {
        ( *ctx->seenLines )[ lineIndex ] = 1;
    }
    return 0; // continue scanning
}

int bufferScanMultiCallback( unsigned int id, unsigned long long from, unsigned long long to,
                              unsigned int flags, void* context )
{
    Q_UNUSED( flags );

    auto* ctx = static_cast<BufferScanContext*>( context );
    const bool finalRecordEnd = !ctx->endsWithSeparator
        && to == static_cast<unsigned long long>( ctx->bufferSize );
    const auto matchByte = static_cast<qint64>(
        to > 0 && ( from < to || finalRecordEnd ) ? to - 1 : to );
    auto it = std::upper_bound( ctx->endOfLines->cbegin(), ctx->endOfLines->cend(), matchByte );
    const auto lineIndex = static_cast<size_t>( std::distance( ctx->endOfLines->cbegin(), it ) );

    if ( lineIndex < ctx->perLinePatterns->size() && id < ( *ctx->perLinePatterns )[ lineIndex ].size() ) {
        ( *ctx->perLinePatterns )[ lineIndex ][ id ] = 1;
    }
    return 0;
}

} // namespace

HsMatcherContext::HsMatcherContext( std::size_t numberOfPatterns )
    : matchingPatterns( numberOfPatterns, 0 )
    , matchingPatternsTemplate_( numberOfPatterns, 0 )
{
}

void HsMatcherContext::reset()
{
    matchingPatterns = matchingPatternsTemplate_;
}

HsMatcher::HsMatcher( HsDatabase db, HsScratch scratch, std::size_t numberOfPatterns )
    : database_{ std::move( db ) }
    , scratch_{ std::move( scratch ) }
    , context_( numberOfPatterns )
{
}

HsSingleMatcher::HsSingleMatcher( HsDatabase db, HsScratch scratch )
    : HsMatcher( db, std::move( scratch ), 1 )
{
}

MatchedPatterns HsSingleMatcher::match( const std::string_view& utf8Data ) const
{
    context_.reset();

    hs_scan( database_.get(), utf8Data.data(), static_cast<unsigned int>( utf8Data.size() ), 0,
             scratch_.get(), matchSingleCallback, static_cast<void*>( &context_ ) );

    return std::move( context_.matchingPatterns );
}

HsMultiMatcher::HsMultiMatcher( HsDatabase db, HsScratch scratch, std::size_t numberOfPatterns )
    : HsMatcher( db, std::move( scratch ), numberOfPatterns )
{
}

MatchedPatterns HsMultiMatcher::match( const std::string_view& utf8Data ) const
{
    context_.reset();

    hs_scan( database_.get(), utf8Data.data(), static_cast<unsigned int>( utf8Data.size() ), 0,
             scratch_.get(), matchMultiCallback, static_cast<void*>( &context_ ) );

    return std::move( context_.matchingPatterns );
}

MatchedPatterns HsNoopMatcher::match( const std::string_view& ) const
{
    return {};
}

HsPrefilterMatcher::HsPrefilterMatcher( const klogg::vector<RegularExpressionPattern>& patterns,
                                        HsMultiMatcher&& hsMatcher )
    : patterns_( patterns )
    , hsMatcher_( std::move( hsMatcher ) )

{
}

MatchedPatterns HsPrefilterMatcher::match( const std::string_view& utf8Data ) const
{
    MatchedPatterns matchingPatterns = hsMatcher_.match( utf8Data );

    for ( size_t i = 0u; i < matchingPatterns.size(); ++i ) {
        if ( matchingPatterns[ i ] ) {
            matchingPatterns[ i ]
                = static_cast<QRegularExpression>( patterns_[ i ] )
                      .match( QString::fromUtf8( utf8Data.data(), klogg::isize( utf8Data ) ) )
                      .hasMatch();
        }
    }

    return matchingPatterns;
}

HsBufferScanner::HsBufferScanner( HsDatabase database, HsScratch scratch,
                                   std::size_t numberOfPatterns )
    : database_( std::move( database ) )
    , scratch_( std::move( scratch ) )
    , numberOfPatterns_( numberOfPatterns )
{
}

void HsBufferScanner::scan( const char* data, unsigned int size,
                             const klogg::vector<qint64>& endOfLines,
                             klogg::vector<uint64_t>& matchedLineIndices ) const
{
    if ( !isValid() || size == 0 ) {
        return;
    }

    // One byte per line (not a packed bitset): direct indexing in the callback
    // with no proxy/reference cost, at a negligible memory cost (chunk line
    // count). This replaces a std::set dedup whose per-distinct-line node
    // allocation was the dominant per-match overhead.
    std::vector<unsigned char> seenLines( endOfLines.size(), 0 );
    BufferScanContext ctx{};
    ctx.endOfLines = &endOfLines;
    ctx.bufferSize = static_cast<qint64>( size );
    ctx.endsWithSeparator = data[ size - 1 ] == '\n';
    ctx.perLinePatterns = nullptr;
    ctx.seenLines = &seenLines;

    hs_scan( database_.get(), data, size, 0, scratch_.get(), bufferScanSingleCallback,
             static_cast<void*>( &ctx ) );

    // Collect matched line indices in ascending order: sorted and deduped by
    // construction (one pass over the occupancy vector). Cheaper and simpler
    // than the old std::set, and gives callers a guaranteed-ordered result.
    matchedLineIndices.clear();
    for ( size_t i = 0; i < seenLines.size(); ++i ) {
        if ( seenLines[ i ] ) {
            matchedLineIndices.push_back( static_cast<uint64_t>( i ) );
        }
    }
}

void HsBufferScanner::scanMulti( const char* data, unsigned int size,
                                  const klogg::vector<qint64>& endOfLines,
                                  std::vector<MatchedPatterns>& perLinePatterns ) const
{
    if ( !isValid() || size == 0 ) {
        return;
    }

    BufferScanContext ctx{};
    ctx.endOfLines = &endOfLines;
    ctx.bufferSize = static_cast<qint64>( size );
    ctx.endsWithSeparator = data[ size - 1 ] == '\n';
    ctx.perLinePatterns = &perLinePatterns;
    ctx.seenLines = nullptr; // multi callback doesn't need dedup (idempotent set)

    hs_scan( database_.get(), data, size, 0, scratch_.get(), bufferScanMultiCallback,
             static_cast<void*>( &ctx ) );
}

HsRegularExpression::HsRegularExpression( const RegularExpressionPattern& pattern )
    : HsRegularExpression( klogg::vector<RegularExpressionPattern>{ pattern } )
{
}

HsRegularExpression::HsRegularExpression( const klogg::vector<RegularExpressionPattern>& patterns )
    : patterns_( patterns )
{
    if ( patterns_.empty() ) {
        isValid_ = true;
        return;
    }

    auto requiredInstructuins = CpuInstructions::SSE2;
    requiredInstructuins |= CpuInstructions::SSSE3;

    if ( hasRequiredInstructions( supportedCpuInstructions(), requiredInstructuins ) ) {
        auto compileHsDatabase = []( const klogg::vector<RegularExpressionPattern>& expressions,
                                     QString& errorMessage, bool isPrefilter ) -> hs_database_t* {
            hs_database_t* db = nullptr;
            hs_compile_error_t* error = nullptr;

            klogg::vector<unsigned> flags( expressions.size() );
            std::transform( expressions.cbegin(), expressions.cend(), flags.begin(),
                            [ isPrefilter ]( const auto& expression ) {
                                auto expressionFlags
                                    = HS_FLAG_UTF8 | HS_FLAG_UCP | HS_FLAG_SINGLEMATCH;
                                if ( !expression.isCaseSensitive ) {
                                    expressionFlags |= HS_FLAG_CASELESS;
                                }
                                if ( isPrefilter || expression.isPrefilter ) {
                                    expressionFlags |= HS_FLAG_PREFILTER;
                                }
                                return expressionFlags;
                            } );

            klogg::vector<QByteArray> utf8Patterns( expressions.size() );
            std::transform( expressions.cbegin(), expressions.cend(), utf8Patterns.begin(),
                            []( const auto& expression ) {
                                auto p = expression.pattern;
                                if ( expression.isPlainText ) {
                                    p = QRegularExpression::escape( expression.pattern );
                                }
                                return p.toUtf8();
                            } );

            klogg::vector<const char*> patternPointers( utf8Patterns.size() );
            std::transform( utf8Patterns.cbegin(), utf8Patterns.cend(), patternPointers.begin(),
                            []( const auto& utf8Pattern ) { return utf8Pattern.data(); } );

            klogg::vector<unsigned> expressionIds( expressions.size() );
            std::iota( expressionIds.begin(), expressionIds.end(), 0u );

            const auto compileResult = hs_compile_multi(
                patternPointers.data(), flags.data(), expressionIds.data(),
                static_cast<unsigned>( expressions.size() ), HS_MODE_BLOCK, nullptr, &db, &error );

            if ( compileResult != HS_SUCCESS ) {
                LOG_ERROR << "Failed to compile pattern " << error->message;
                errorMessage = error->message;
                hs_free_compile_error( error );
                return nullptr;
            }

            return db;
        };

        database_ = HsDatabase{ makeUniqueResource<hs_database_t, hs_free_database>(
            [ &compileHsDatabase ]( const klogg::vector<RegularExpressionPattern>& expressions,
                                    QString& errorMessage ) -> hs_database_t* {
                return compileHsDatabase( expressions, errorMessage, false );
            },
            patterns, errorMessage_ ) };

        if ( !database_ ) {
            QString preFilterErrorMessage;
            isPrefilter_ = true;
            database_ = HsDatabase{ makeUniqueResource<hs_database_t, hs_free_database>(
                [ &compileHsDatabase ]( const klogg::vector<RegularExpressionPattern>& expressions,
                                        QString& errorMessage ) -> hs_database_t* {
                    return compileHsDatabase( expressions, errorMessage, true );
                },
                patterns, preFilterErrorMessage ) };
        }
    }
    else {
        LOG_WARNING << "Cpu doesn't have sse2 or ssse3, use qt regex engine";
    }

    auto allocScratchForDb
        = []( const hs_database_t* database ) -> hs_scratch_t* {
        hs_scratch_t* scratch = nullptr;
        const auto scratchResult = hs_alloc_scratch( database, &scratch );
        if ( scratchResult != HS_SUCCESS ) {
            LOG_ERROR << "Failed to allocate scratch";
            return nullptr;
        }
        return scratch;
    };

    if ( database_ ) {
        scratch_ = makeUniqueResource<hs_scratch_t, hs_free_scratch>( allocScratchForDb,
                                                                       database_.get() );
    }

    // Compile a second database WITHOUT HS_FLAG_SINGLEMATCH only for the
    // single exact pattern path that can create a bulk buffer scanner.
    // Multi-pattern and explicit-prefilter expressions are consumed per-line;
    // compiling an unused exact database for them is both wasteful and unsafe
    // with MSVC ASan in Vectorscan's multi-pattern compiler.
    const bool canBuildBufferScanner
        = !isPrefilter_ && patterns_.size() == 1 && !patterns_.front().isPrefilter;
    if ( canBuildBufferScanner ) {
        auto compileBlockDatabase
            = []( const klogg::vector<RegularExpressionPattern>& expressions,
                  QString& errorMessage ) -> hs_database_t* {
            for ( const auto& expression : expressions ) {
                const auto pattern = expression.isPlainText
                    ? QRegularExpression::escape( expression.pattern ) : expression.pattern;
                if ( !supportsRecordLocalBlockScan( pattern ) ) {
                    errorMessage = QStringLiteral( "Pattern requires independent record matching" );
                    LOG_DEBUG << "Block scan fallback: " << errorMessage;
                    return nullptr;
                }
            }
            hs_database_t* db = nullptr;
            hs_compile_error_t* error = nullptr;

            klogg::vector<unsigned> flags( expressions.size() );
            std::transform( expressions.cbegin(), expressions.cend(), flags.begin(),
                            []( const auto& expression ) {
                                // HS_FLAG_MULTILINE: ^ / $ match at each line
                                // boundary (after/before every '\n') plus the buffer
                                // edges, so a whole-buffer scan agrees with the
                                // per-line scan for line-anchored patterns. Without
                                // it, ^ERROR would match only at the buffer start and
                                // silently undercount. (Does not affect '.' matching
                                // '\n' -- that is HS_FLAG_DOTALL, intentionally off,
                                // so .* still stops at line ends.)
                                // Accurate zero-width attribution requires start
                                // offsets. SOM-incompatible patterns intentionally
                                // retain their existing per-record matcher; do not
                                // combine ALLOWEMPTY with SOM (unsupported by HS).
                                auto expressionFlags
                                    = HS_FLAG_UTF8 | HS_FLAG_UCP | HS_FLAG_MULTILINE
                                      | HS_FLAG_SOM_LEFTMOST;
                                if ( !expression.isCaseSensitive ) {
                                    expressionFlags |= HS_FLAG_CASELESS;
                                }
                                return expressionFlags;
                            } );

            klogg::vector<QByteArray> utf8Patterns( expressions.size() );
            std::transform( expressions.cbegin(), expressions.cend(), utf8Patterns.begin(),
                            []( const auto& expression ) {
                                auto p = expression.pattern;
                                if ( expression.isPlainText ) {
                                    p = QRegularExpression::escape( expression.pattern );
                                }
                                return p.toUtf8();
                            } );

            klogg::vector<const char*> patternPointers( utf8Patterns.size() );
            std::transform( utf8Patterns.cbegin(), utf8Patterns.cend(), patternPointers.begin(),
                            []( const auto& utf8Pattern ) { return utf8Pattern.data(); } );

            klogg::vector<unsigned> expressionIds( expressions.size() );
            std::iota( expressionIds.begin(), expressionIds.end(), 0u );

            const auto compileResult = hs_compile_multi(
                patternPointers.data(), flags.data(), expressionIds.data(),
                static_cast<unsigned>( expressions.size() ), HS_MODE_BLOCK, nullptr, &db,
                &error );

            if ( compileResult != HS_SUCCESS ) {
                LOG_WARNING << "Failed to compile block database: " << error->message;
                errorMessage = error->message;
                hs_free_compile_error( error );
                return nullptr;
            }
            return db;
        };

        QString blockErrorMessage;
        blockDatabase_ = HsDatabase{ makeUniqueResource<hs_database_t, hs_free_database>(
            compileBlockDatabase, patterns, blockErrorMessage ) };

        if ( blockDatabase_ ) {
            blockScratch_ = makeUniqueResource<hs_scratch_t, hs_free_scratch>(
                allocScratchForDb, blockDatabase_.get() );
        }
    }

    // When Vectorscan compilation fails entirely, or when it succeeds only in
    // prefilter mode, we must validate patterns against QRegularExpression.
    // The prefilter matcher (HsPrefilterMatcher) uses QRegularExpression to
    // confirm Vectorscan's approximate matches — if QRegularExpression rejects
    // the pattern, every confirmation fails silently and the expression never
    // matches, even though isHsValid() reports true.
    if ( !isHsValid() || isPrefilter_ ) {
        for ( const auto& pattern : patterns_ ) {
            const auto regex = static_cast<QRegularExpression>( pattern );
            if ( !regex.isValid() ) {
                isValid_ = false;
                errorMessage_ = regex.errorString();
                break;
            }
        }
    }

    LOG_DEBUG << "Finished creating pattern database, patterns: " << patterns_.size()
             << ", is db valid: " << isValid_ << ", is prefilter: " << isPrefilter_;
}

bool HsRegularExpression::isValid() const
{
    return isValid_;
}

bool HsRegularExpression::isHsValid() const
{
    return database_ != nullptr && scratch_ != nullptr;
}

bool HsRegularExpression::hasBufferScanner() const
{
    return blockDatabase_ != nullptr && blockScratch_ != nullptr;
}

std::unique_ptr<HsBufferScanner> HsRegularExpression::createBufferScanner() const
{
    if ( !hasBufferScanner() ) {
        return nullptr;
    }

    auto scannerScratch = makeUniqueResource<hs_scratch_t, hs_free_scratch>(
        []( const hs_scratch_t* prototype ) -> hs_scratch_t* {
            hs_scratch_t* scratch = nullptr;
            const auto err = hs_clone_scratch( prototype, &scratch );
            if ( err != HS_SUCCESS ) {
                LOG_ERROR << "hs_clone_scratch failed for buffer scanner";
                return nullptr;
            }
            return scratch;
        },
        blockScratch_.get() );

    if ( !scannerScratch ) {
        return nullptr;
    }

    return std::make_unique<HsBufferScanner>( blockDatabase_, std::move( scannerScratch ),
                                               patterns_.size() );
}

QString HsRegularExpression::errorString() const
{
    return errorMessage_;
}

MatcherVariant HsRegularExpression::createMatcher() const
{
    if ( !isHsValid() ) {
        return MatcherVariant{ DefaultRegularExpressionMatcher( patterns_ ) };
    }

    if ( !database_ || !scratch_ ) {
        return HsNoopMatcher();
    }

    auto matcherScratch = makeUniqueResource<hs_scratch_t, hs_free_scratch>(
        []( const hs_scratch_t* prototype ) -> hs_scratch_t* {
            hs_scratch_t* scratch = nullptr;

            const auto err = hs_clone_scratch( prototype, &scratch );
            if ( err != HS_SUCCESS ) {
                LOG_ERROR << "hs_clone_scratch failed";
                return nullptr;
            }

            return scratch;
        },
        scratch_.get() );

    if ( !isPrefilter_ ) {
        if ( patterns_.size() == 1 ) {
            return HsSingleMatcher{ database_, std::move( matcherScratch ) };
        }
        else {
            return HsMultiMatcher{ database_, std::move( matcherScratch ), patterns_.size() };
        }
    }
    else {
        return HsPrefilterMatcher(
            patterns_, HsMultiMatcher{ database_, std::move( matcherScratch ), patterns_.size() } );
    }
}
#endif
