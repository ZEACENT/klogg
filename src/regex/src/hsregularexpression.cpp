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
#include <set>
#include <qregularexpression.h>
#include <string_view>

#ifdef KLOGG_HAS_VECTORSCAN
#include "hsregularexpression.h"

#include "cpu_info.h"
#include "log.h"

namespace {

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

// Callback for buffer scanning — records which lines contain matches.
// The context carries endOfLines offsets for binary-search mapping.
struct BufferScanContext {
    const klogg::vector<qint64>* endOfLines;
    klogg::vector<uint64_t>* matchedLineIndices;
    // For multi-pattern boolean mode:
    std::vector<MatchedPatterns>* perLinePatterns;
    // Robust dedup: Vectorscan reports matches in byte-offset order for block
    // mode, but we use a set to be safe against any future pattern type that
    // could produce out-of-order callbacks.
    std::set<uint64_t>* seenLines;
};

int bufferScanSingleCallback( unsigned int id, unsigned long long from, unsigned long long to,
                               unsigned int flags, void* context )
{
    Q_UNUSED( id );
    Q_UNUSED( from );
    Q_UNUSED( flags );

    auto* ctx = static_cast<BufferScanContext*>( context );
    // Binary search: find the line that contains byte offset 'to - 1'
    const auto matchByte = static_cast<qint64>( to > 0 ? to - 1 : to );
    auto it = std::upper_bound( ctx->endOfLines->cbegin(), ctx->endOfLines->cend(), matchByte );
    const auto lineIndex = static_cast<uint64_t>( std::distance( ctx->endOfLines->cbegin(), it ) );

    // Deduplicate: multiple matches in the same line produce multiple callbacks
    if ( ctx->seenLines->insert( lineIndex ).second ) {
        ctx->matchedLineIndices->push_back( lineIndex );
    }
    return 0; // continue scanning
}

int bufferScanMultiCallback( unsigned int id, unsigned long long from, unsigned long long to,
                              unsigned int flags, void* context )
{
    Q_UNUSED( from );
    Q_UNUSED( flags );

    auto* ctx = static_cast<BufferScanContext*>( context );
    const auto matchByte = static_cast<qint64>( to > 0 ? to - 1 : to );
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

    std::set<uint64_t> seenLines;
    BufferScanContext ctx{};
    ctx.endOfLines = &endOfLines;
    ctx.matchedLineIndices = &matchedLineIndices;
    ctx.perLinePatterns = nullptr;
    ctx.seenLines = &seenLines;

    hs_scan( database_.get(), data, size, 0, scratch_.get(), bufferScanSingleCallback,
             static_cast<void*>( &ctx ) );
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
    ctx.matchedLineIndices = nullptr;
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

    auto allocScratchForDb = []( hs_database_t* db ) -> hs_scratch_t* {
        hs_scratch_t* scratch = nullptr;
        const auto scratchResult = hs_alloc_scratch( db, &scratch );
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

    // Compile a second database WITHOUT HS_FLAG_SINGLEMATCH for bulk buffer scanning.
    // This allows hs_scan to report all match occurrences across the buffer
    // (one per line that matches), not just the first match overall.
    if ( !isPrefilter_ ) {
        auto compileBlockDatabase
            = []( const klogg::vector<RegularExpressionPattern>& expressions,
                  QString& errorMessage ) -> hs_database_t* {
            hs_database_t* db = nullptr;
            hs_compile_error_t* error = nullptr;

            klogg::vector<unsigned> flags( expressions.size() );
            std::transform( expressions.cbegin(), expressions.cend(), flags.begin(),
                            []( const auto& expression ) {
                                auto expressionFlags = HS_FLAG_UTF8 | HS_FLAG_UCP;
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

    if ( !isHsValid() ) {
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
        []( hs_scratch_t* prototype ) -> hs_scratch_t* {
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
        []( hs_scratch_t* prototype ) -> hs_scratch_t* {
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
