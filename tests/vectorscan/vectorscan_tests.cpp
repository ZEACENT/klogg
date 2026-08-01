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

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QStringList>

#include <hs.h>
#include <mimalloc.h>

#include <configuration.h>
#include <highlighterset.h>
#include <hsregularexpression.h>
#include <logger.h>
#include <persistentinfo.h>
#include <simdutf_wrapper.h>
#include <test_utils.h>

const bool PersistentInfo::ForcePortable = true;

namespace {

constexpr int kChildTimeoutMs = 60000;

enum class AllocatorMode { Crt, Mimalloc };

enum class ChildCase {
    DirectSinglePrefilter,
    DirectMultiPrefilter,
    DirectMultiNoopScanPrefilter,
    DirectMultiScanPrefilter,
    DirectMultiCloneScanPrefilter,
    DirectMultiCompileSimdutfScanPrefilter,
    SimdutfConvert,
    HighlighterCompilePrefilter,
    HighlighterCollectionRestorePrefilter,
};

struct ChildOptions {
    ChildCase childCase;
    AllocatorMode allocator = AllocatorMode::Crt;
    std::vector<int> indexes;
};

struct ChildRunResult {
    bool started = false;
    bool finished = false;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    int exitCode = -1;
    QString output;
};

void configureTestTempDir()
{
    const auto tempDir = QDir::cleanPath( QCoreApplication::applicationDirPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) );
    QDir{}.mkpath( tempDir );

    const auto tempDirUtf8 = QDir::toNativeSeparators( tempDir ).toUtf8();
    qputenv( "TMP", tempDirUtf8 );
    qputenv( "TEMP", tempDirUtf8 );
    qputenv( "TMPDIR", tempDirUtf8 );
}

void configureProductLikeTestState()
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );
    config.setConfirmTabClose( false );
    config.save();
}

QString joinIndexes( const std::vector<int>& indexes )
{
    QStringList values;
    for ( const auto index : indexes ) {
        values << QString::number( index );
    }
    return values.join( QLatin1Char( ',' ) );
}

std::array<QString, 6> regressionPatterns()
{
    return {
        QStringLiteral( R"(\b(?:ERROR|WARN|INFO|DEBUG|TRACE|FATAL)\b)" ),
        QStringLiteral( R"((?:[A-Za-z_][A-Za-z0-9_]{0,31}\.){2,}[A-Za-z_][A-Za-z0-9_]{0,31})" ),
        QStringLiteral( R"(\b0x[0-9A-Fa-f]{8,16}\b)" ),
        QStringLiteral( R"((?:\d{1,3}\.){3}\d{1,3}(?::\d{2,5})?)" ),
        QStringLiteral( R"([A-Za-z]:\\(?:[^\\/:*?"<>|\r\n]+\\)*[^\\/:*?"<>|\r\n]*)" ),
        QStringLiteral( R"(https?://[A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%-]+)" ),
    };
}

QString regressionSampleLine()
{
    return QStringLiteral(
        "ERROR com.example.service 0xDEADBEEF 10.20.30.40:443 "
        "C:\\logs\\app\\file.txt https://example.com" );
}

klogg::vector<RegularExpressionPattern> makeRegressionPatterns( const std::vector<int>& indexes )
{
    const auto patterns = regressionPatterns();
    klogg::vector<RegularExpressionPattern> selected;
    selected.reserve( indexes.size() );

    for ( const auto index : indexes ) {
        if ( index < 0 || index >= static_cast<int>( patterns.size() ) ) {
            return {};
        }

        RegularExpressionPattern pattern( patterns[ static_cast<size_t>( index ) ], true, false,
                                          false, false );
        pattern.isPrefilter = true;
        selected.emplace_back( std::move( pattern ) );
    }

    return selected;
}

unsigned hsFlags()
{
    return HS_FLAG_UTF8 | HS_FLAG_UCP | HS_FLAG_SINGLEMATCH | HS_FLAG_PREFILTER;
}

void* crtAllocator( size_t size )
{
    return std::malloc( size );
}

void crtFree( void* ptr )
{
    std::free( ptr );
}

hs_error_t configureHsAllocator( AllocatorMode allocator )
{
    if ( allocator == AllocatorMode::Mimalloc ) {
        return hs_set_allocator( mi_malloc, mi_free );
    }

    return hs_set_allocator( crtAllocator, crtFree );
}

QString allocatorName( AllocatorMode allocator )
{
    return allocator == AllocatorMode::Mimalloc ? QStringLiteral( "mimalloc" )
                                                : QStringLiteral( "crt" );
}

QString childCaseName( ChildCase childCase )
{
    switch ( childCase ) {
    case ChildCase::DirectSinglePrefilter:
        return QStringLiteral( "direct_single_prefilter" );
    case ChildCase::DirectMultiPrefilter:
        return QStringLiteral( "direct_multi_prefilter" );
    case ChildCase::DirectMultiNoopScanPrefilter:
        return QStringLiteral( "direct_multi_noop_scan_prefilter" );
    case ChildCase::DirectMultiScanPrefilter:
        return QStringLiteral( "direct_multi_scan_prefilter" );
    case ChildCase::DirectMultiCloneScanPrefilter:
        return QStringLiteral( "direct_multi_clone_scan_prefilter" );
    case ChildCase::DirectMultiCompileSimdutfScanPrefilter:
        return QStringLiteral( "direct_multi_compile_simdutf_scan_prefilter" );
    case ChildCase::SimdutfConvert:
        return QStringLiteral( "simdutf_convert" );
    case ChildCase::HighlighterCompilePrefilter:
        return QStringLiteral( "highlighter_compile_prefilter" );
    case ChildCase::HighlighterCollectionRestorePrefilter:
        return QStringLiteral( "highlighter_collection_restore_prefilter" );
    }

    return QStringLiteral( "unknown" );
}

std::optional<ChildCase> parseChildCase( const QString& value )
{
    if ( value == QStringLiteral( "direct_single_prefilter" ) ) {
        return ChildCase::DirectSinglePrefilter;
    }
    if ( value == QStringLiteral( "direct_multi_prefilter" ) ) {
        return ChildCase::DirectMultiPrefilter;
    }
    if ( value == QStringLiteral( "direct_multi_noop_scan_prefilter" ) ) {
        return ChildCase::DirectMultiNoopScanPrefilter;
    }
    if ( value == QStringLiteral( "direct_multi_scan_prefilter" ) ) {
        return ChildCase::DirectMultiScanPrefilter;
    }
    if ( value == QStringLiteral( "direct_multi_clone_scan_prefilter" ) ) {
        return ChildCase::DirectMultiCloneScanPrefilter;
    }
    if ( value == QStringLiteral( "direct_multi_compile_simdutf_scan_prefilter" ) ) {
        return ChildCase::DirectMultiCompileSimdutfScanPrefilter;
    }
    if ( value == QStringLiteral( "simdutf_convert" ) ) {
        return ChildCase::SimdutfConvert;
    }
    if ( value == QStringLiteral( "highlighter_compile_prefilter" ) ) {
        return ChildCase::HighlighterCompilePrefilter;
    }
    if ( value == QStringLiteral( "highlighter_collection_restore_prefilter" ) ) {
        return ChildCase::HighlighterCollectionRestorePrefilter;
    }

    return std::nullopt;
}

std::optional<AllocatorMode> parseAllocatorMode( const QString& value )
{
    if ( value == QStringLiteral( "crt" ) ) {
        return AllocatorMode::Crt;
    }
    if ( value == QStringLiteral( "mimalloc" ) ) {
        return AllocatorMode::Mimalloc;
    }

    return std::nullopt;
}

std::vector<int> parseIndexes( const QString& value )
{
    std::vector<int> indexes;
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    const auto parts = value.split( QLatin1Char( ',' ), Qt::SkipEmptyParts );
#else
    const auto parts = value.split( QLatin1Char( ',' ), QString::SkipEmptyParts );
#endif

    indexes.reserve( static_cast<size_t>( parts.size() ) );
    for ( const auto& part : parts ) {
        bool ok = false;
        const auto index = part.toInt( &ok );
        if ( ok ) {
            indexes.push_back( index );
        }
    }

    return indexes;
}

std::optional<ChildOptions> parseChildOptions( const QStringList& arguments )
{
    QString childName;
    QString allocatorNameValue = QStringLiteral( "crt" );
    QString indexValue;

    for ( const auto& argument : arguments ) {
        if ( argument.startsWith( QStringLiteral( "--vectorscan-child=" ) ) ) {
            childName = argument.section( QLatin1Char( '=' ), 1 );
        }
        else if ( argument.startsWith( QStringLiteral( "--vectorscan-allocator=" ) ) ) {
            allocatorNameValue = argument.section( QLatin1Char( '=' ), 1 );
        }
        else if ( argument.startsWith( QStringLiteral( "--vectorscan-indexes=" ) ) ) {
            indexValue = argument.section( QLatin1Char( '=' ), 1 );
        }
    }

    if ( childName.isEmpty() ) {
        return std::nullopt;
    }

    const auto parsedCase = parseChildCase( childName );
    const auto parsedAllocator = parseAllocatorMode( allocatorNameValue );
    if ( !parsedCase || !parsedAllocator ) {
        return std::nullopt;
    }

    ChildOptions options;
    options.childCase = *parsedCase;
    options.allocator = *parsedAllocator;
    options.indexes = parseIndexes( indexValue );
    return options;
}

[[maybe_unused]] std::vector<std::vector<int>> buildSearchSpace()
{
    std::vector<std::vector<int>> searchSpace;
    std::vector<int> values;
    values.reserve( regressionPatterns().size() );
    for ( auto i = 0; i < static_cast<int>( regressionPatterns().size() ); ++i ) {
        values.push_back( i );
        searchSpace.push_back( { i } );
    }

    auto addCombinations = [ &searchSpace, &values ]( int choose ) {
        std::vector<int> combination;

        const auto visit = [ & ]( const auto& self, int offset ) -> void {
            if ( combination.size() == static_cast<size_t>( choose ) ) {
                searchSpace.push_back( combination );
                return;
            }

            for ( auto i = offset; i < static_cast<int>( values.size() ); ++i ) {
                combination.push_back( values[ static_cast<size_t>( i ) ] );
                self( self, i + 1 );
                combination.pop_back();
            }
        };

        visit( visit, 0 );
    };

    addCombinations( 2 );
    addCombinations( 3 );
    searchSpace.push_back( values );

    return searchSpace;
}

void writeHighlighterSet( QSettings& settings,
                          const klogg::vector<RegularExpressionPattern>& patterns )
{
    settings.beginGroup( QStringLiteral( "HighlighterSet" ) );
    settings.setValue( QStringLiteral( "version" ), 3 );
    settings.setValue( QStringLiteral( "name" ), QStringLiteral( "Vectorscan Regression" ) );
    settings.setValue( QStringLiteral( "id" ), QStringLiteral( "vectorscan-regression" ) );
    settings.beginWriteArray( QStringLiteral( "highlighters" ) );

    int arrayIndex = 0;
    for ( const auto& pattern : patterns ) {
        settings.setArrayIndex( arrayIndex++ );
        settings.setValue( QStringLiteral( "regexp" ), pattern.pattern );
        settings.setValue( QStringLiteral( "ignore_case" ), !pattern.isCaseSensitive );
        settings.setValue( QStringLiteral( "match_only" ), true );
        settings.setValue( QStringLiteral( "use_regex" ), true );
        settings.setValue( QStringLiteral( "variate_colors" ), false );
        settings.setValue( QStringLiteral( "color_variance" ), 15 );
        settings.setValue( QStringLiteral( "fore_colour" ),
                           QColor( Qt::black ).name( QColor::HexArgb ) );
        settings.setValue( QStringLiteral( "back_colour" ),
                           QColor( Qt::yellow ).name( QColor::HexArgb ) );
    }

    settings.endArray();
    settings.endGroup();
}

HighlighterSet loadHighlighterSet( const QString& settingsPath,
                                   const std::vector<int>& indexes )
{
    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.clear();
    writeHighlighterSet( settings, makeRegressionPatterns( indexes ) );
    settings.sync();

    HighlighterSet set;
    set.retrieveFromStorage( settings );
    return set;
}

HighlighterSetCollection loadHighlighterCollection( const QString& settingsPath,
                                                    const std::vector<int>& indexes )
{
    auto set = loadHighlighterSet( settingsPath + QStringLiteral( ".set.ini" ), indexes );

    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.clear();
    settings.beginGroup( QStringLiteral( "HighlighterSetCollection" ) );
    settings.setValue( QStringLiteral( "version" ), 2 );
    settings.setValue( QStringLiteral( "active_sets" ),
                       QStringList{ QStringLiteral( "vectorscan-regression" ) } );
    settings.beginWriteArray( QStringLiteral( "sets" ) );
    settings.setArrayIndex( 0 );
    set.saveToStorage( settings );
    settings.endArray();
    settings.endGroup();
    settings.sync();

    HighlighterSetCollection collection;
    collection.retrieveFromStorage( settings );
    return collection;
}

int runDirectSingleChild( const std::vector<int>& indexes, AllocatorMode allocator )
{
    const auto allocatorResult = configureHsAllocator( allocator );
    if ( allocatorResult != HS_SUCCESS ) {
        std::cerr << "hs_set_allocator failed rc=" << allocatorResult << std::endl;
        return 9;
    }
    const auto patterns = regressionPatterns();

    for ( size_t iteration = 0; iteration < indexes.size(); ++iteration ) {
        const auto index = indexes[ iteration ];
        const auto utf8Pattern = patterns[ static_cast<size_t>( index ) ].toUtf8();

        std::cout << "[iter " << ( iteration + 1 ) << "] compile start mode=single indexes="
                  << index << " allocator=" << allocatorName( allocator ).toStdString()
                  << " prefilter=on" << std::endl;

        hs_database_t* database = nullptr;
        hs_compile_error_t* error = nullptr;
        const auto compileResult = hs_compile( utf8Pattern.constData(), hsFlags(), HS_MODE_BLOCK,
                                               nullptr, &database, &error );
        if ( compileResult != HS_SUCCESS ) {
            if ( error != nullptr ) {
                std::cerr << "hs_compile failed: " << error->message << std::endl;
                hs_free_compile_error( error );
            }
            return 10;
        }

        hs_scratch_t* scratch = nullptr;
        const auto scratchResult = hs_alloc_scratch( database, &scratch );
        if ( scratchResult != HS_SUCCESS ) {
            hs_free_database( database );
            std::cerr << "hs_alloc_scratch failed" << std::endl;
            return 11;
        }

        std::cout << "[iter " << ( iteration + 1 ) << "] single compile ok index=" << index
                  << " scratch_rc=" << scratchResult << std::endl;

        hs_free_scratch( scratch );
        hs_free_database( database );

        std::cout << "[iter " << ( iteration + 1 ) << "] single free complete index=" << index
                  << std::endl;
    }

    std::cout << "Completed " << indexes.size() << " iterations without process crash"
              << std::endl;
    return 0;
}

int runDirectMultiChild( const std::vector<int>& indexes, AllocatorMode allocator )
{
    const auto allocatorResult = configureHsAllocator( allocator );
    if ( allocatorResult != HS_SUCCESS ) {
        std::cerr << "hs_set_allocator failed rc=" << allocatorResult << std::endl;
        return 19;
    }
    const auto patterns = makeRegressionPatterns( indexes );

    std::vector<QByteArray> utf8Patterns;
    std::vector<const char*> patternPointers;
    std::vector<unsigned> flags;
    std::vector<unsigned> ids;

    utf8Patterns.reserve( patterns.size() );
    patternPointers.reserve( patterns.size() );
    flags.reserve( patterns.size() );
    ids.reserve( patterns.size() );

    for ( size_t i = 0; i < patterns.size(); ++i ) {
        utf8Patterns.emplace_back( patterns[ i ].pattern.toUtf8() );
        patternPointers.push_back( utf8Patterns.back().constData() );
        flags.push_back( hsFlags() );
        ids.push_back( static_cast<unsigned>( i ) );
    }

    std::cout << "compile start mode=multi indexes=" << joinIndexes( indexes ).toStdString()
              << " allocator=" << allocatorName( allocator ).toStdString() << " prefilter=on"
              << std::endl;

    hs_database_t* database = nullptr;
    hs_compile_error_t* error = nullptr;
    const auto compileResult
        = hs_compile_multi( patternPointers.data(), flags.data(), ids.data(),
                            static_cast<unsigned>( patternPointers.size() ), HS_MODE_BLOCK, nullptr,
                            &database, &error );
    if ( compileResult != HS_SUCCESS ) {
        if ( error != nullptr ) {
            std::cerr << "hs_compile_multi failed: " << error->message << std::endl;
            hs_free_compile_error( error );
        }
        return 20;
    }

    hs_scratch_t* scratch = nullptr;
    const auto scratchResult = hs_alloc_scratch( database, &scratch );
    if ( scratchResult != HS_SUCCESS ) {
        hs_free_database( database );
        std::cerr << "hs_alloc_scratch failed" << std::endl;
        return 21;
    }

    std::cout << "multi compile ok indexes=" << joinIndexes( indexes ).toStdString()
              << " scratch_rc=" << scratchResult << std::endl;

    hs_free_scratch( scratch );
    hs_free_database( database );

    std::cout << "multi free complete indexes=" << joinIndexes( indexes ).toStdString()
              << std::endl;
    return 0;
}

struct DirectScanContext {
    explicit DirectScanContext( size_t patternCount )
        : matches( patternCount, 0 )
    {
    }

    std::vector<unsigned char> matches;
    bool invalidId = false;
};

struct DirectScanPlan {
    bool cloneScratch = false;
    bool convertWithSimdutfAfterCompile = false;
    bool recordMatches = true;
};

int ignoreDirectScanMatch( unsigned int id, unsigned long long from, unsigned long long to,
                           unsigned int flags, void* context )
{
    Q_UNUSED( id );
    Q_UNUSED( from );
    Q_UNUSED( to );
    Q_UNUSED( flags );
    Q_UNUSED( context );
    return 0;
}

int recordDirectScanMatch( unsigned int id, unsigned long long from, unsigned long long to,
                           unsigned int flags, void* context )
{
    Q_UNUSED( from );
    Q_UNUSED( to );
    Q_UNUSED( flags );

    auto* scanContext = static_cast<DirectScanContext*>( context );
    if ( id >= scanContext->matches.size() ) {
        scanContext->invalidId = true;
        return 1;
    }

    scanContext->matches[ id ] = 1;
    return 0;
}

bool convertRegressionSampleWithSimdutf()
{
    const auto sample = regressionSampleLine();
    const auto utf16Sample = sample.toStdU16String();
    klogg::vector<char> utf8Data( utf16Sample.size() * 4 );

    std::cout << "phase=simdutf_convert start utf16_units=" << utf16Sample.size() << std::endl;
    const auto resultSize = simdutf::convert_utf16_to_utf8(
        utf16Sample.data(), utf16Sample.size(), utf8Data.data() );
    std::cout << "phase=simdutf_convert complete bytes=" << resultSize << std::endl;

    const auto converted
        = QString::fromUtf8( utf8Data.data(), static_cast<int>( resultSize ) );
    return converted == sample;
}

hs_error_t freeScratchWithMarker( hs_scratch_t*& scratch, const char* phase )
{
    std::cout << "phase=" << phase << " start" << std::endl;
    const auto result = hs_free_scratch( scratch );
    scratch = nullptr;
    std::cout << "phase=" << phase << " complete rc=" << result << std::endl;
    return result;
}

hs_error_t freeDatabaseWithMarker( hs_database_t*& database, const char* phase )
{
    std::cout << "phase=" << phase << " start" << std::endl;
    const auto result = hs_free_database( database );
    database = nullptr;
    std::cout << "phase=" << phase << " complete rc=" << result << std::endl;
    return result;
}

int runDirectMultiScanChild( const std::vector<int>& indexes, AllocatorMode allocator,
                             DirectScanPlan plan )
{
    const auto allocatorResult = configureHsAllocator( allocator );
    if ( allocatorResult != HS_SUCCESS ) {
        std::cerr << "phase=direct_allocator failed rc=" << allocatorResult << std::endl;
        return 49;
    }
    std::cout << "phase=direct_allocator complete rc=" << allocatorResult << std::endl;
    const auto patterns = makeRegressionPatterns( indexes );

    std::vector<QByteArray> utf8Patterns;
    std::vector<const char*> patternPointers;
    std::vector<unsigned> flags;
    std::vector<unsigned> ids;

    utf8Patterns.reserve( patterns.size() );
    patternPointers.reserve( patterns.size() );
    flags.reserve( patterns.size() );
    ids.reserve( patterns.size() );

    for ( size_t i = 0; i < patterns.size(); ++i ) {
        utf8Patterns.emplace_back( patterns[ i ].pattern.toUtf8() );
        patternPointers.push_back( utf8Patterns.back().constData() );
        flags.push_back( hsFlags() );
        ids.push_back( static_cast<unsigned>( i ) );
    }

    std::cout << "phase=direct_multi_compile start indexes="
              << joinIndexes( indexes ).toStdString() << " clone_scratch=" << plan.cloneScratch
              << std::endl;

    hs_database_t* database = nullptr;
    hs_compile_error_t* error = nullptr;
    const auto compileResult
        = hs_compile_multi( patternPointers.data(), flags.data(), ids.data(),
                            static_cast<unsigned>( patternPointers.size() ), HS_MODE_BLOCK, nullptr,
                            &database, &error );
    if ( compileResult != HS_SUCCESS ) {
        if ( error != nullptr ) {
            std::cerr << "hs_compile_multi failed: " << error->message << std::endl;
            hs_free_compile_error( error );
        }
        return 50;
    }

    std::cout << "phase=direct_multi_compile complete" << std::endl;

    if ( plan.convertWithSimdutfAfterCompile && !convertRegressionSampleWithSimdutf() ) {
        std::cerr << "phase=simdutf_convert validation_failed" << std::endl;
        freeDatabaseWithMarker( database, "direct_database_free_after_simdutf_failure" );
        return 54;
    }

    std::cout << "phase=direct_scratch_allocate start" << std::endl;
    hs_scratch_t* prototypeScratch = nullptr;
    const auto scratchResult = hs_alloc_scratch( database, &prototypeScratch );
    if ( scratchResult != HS_SUCCESS ) {
        std::cerr << "phase=direct_scratch_allocate failed rc=" << scratchResult << std::endl;
        freeDatabaseWithMarker( database, "direct_database_free_after_scratch_failure" );
        return 51;
    }

    std::cout << "phase=direct_scratch_allocate complete rc=" << scratchResult << std::endl;

    hs_scratch_t* scanScratch = prototypeScratch;
    hs_scratch_t* clonedScratch = nullptr;
    if ( plan.cloneScratch ) {
        std::cout << "phase=direct_scratch_clone start" << std::endl;
        const auto cloneResult = hs_clone_scratch( prototypeScratch, &clonedScratch );
        if ( cloneResult != HS_SUCCESS ) {
            std::cerr << "phase=direct_scratch_clone failed rc=" << cloneResult << std::endl;
            freeScratchWithMarker( prototypeScratch,
                                   "direct_prototype_free_after_clone_failure" );
            freeDatabaseWithMarker( database, "direct_database_free_after_clone_failure" );
            return 52;
        }
        scanScratch = clonedScratch;
        std::cout << "phase=direct_scratch_clone complete rc=" << cloneResult << std::endl;
    }

    const auto sample = regressionSampleLine().toUtf8();
    DirectScanContext context( patterns.size() );
    const auto callback
        = plan.recordMatches ? recordDirectScanMatch : ignoreDirectScanMatch;
    auto* callbackContext = plan.recordMatches ? static_cast<void*>( &context ) : nullptr;
    std::cout << "phase=direct_scan start bytes=" << sample.size()
              << " callback=" << ( plan.recordMatches ? "record" : "noop" ) << std::endl;
    const auto scanResult
        = hs_scan( database, sample.constData(), static_cast<unsigned>( sample.size() ), 0,
                   scanScratch, callback, callbackContext );
    std::cout << "phase=direct_scan complete rc=" << scanResult << std::endl;

    auto teardownResult = HS_SUCCESS;
    if ( clonedScratch != nullptr ) {
        teardownResult
            = freeScratchWithMarker( clonedScratch, "direct_cloned_scratch_free" );
    }
    const auto prototypeFreeResult
        = freeScratchWithMarker( prototypeScratch, "direct_prototype_scratch_free" );
    const auto databaseFreeResult
        = freeDatabaseWithMarker( database, "direct_database_free" );
    if ( teardownResult != HS_SUCCESS || prototypeFreeResult != HS_SUCCESS
         || databaseFreeResult != HS_SUCCESS ) {
        return 55;
    }

    const auto allExpectedPatternsMatched
        = !plan.recordMatches
          || std::all_of( context.matches.cbegin(), context.matches.cend(),
                          []( unsigned char matched ) { return matched != 0; } );
    if ( scanResult != HS_SUCCESS || context.invalidId || !allExpectedPatternsMatched ) {
        return 53;
    }

    return 0;
}

int runSimdutfChild()
{
    return convertRegressionSampleWithSimdutf() ? 0 : 60;
}

int runHighlighterCompileChild( const std::vector<int>& indexes, AllocatorMode allocator )
{
    std::cout << "phase=highlighter_allocator start" << std::endl;
    const auto allocatorResult = configureHsAllocator( allocator );
    if ( allocatorResult != HS_SUCCESS ) {
        std::cerr << "phase=highlighter_allocator failed rc=" << allocatorResult << std::endl;
        return 29;
    }
    std::cout << "phase=highlighter_allocator complete rc=" << allocatorResult << std::endl;

    std::cout << "phase=highlighter_config start" << std::endl;
    configureProductLikeTestState();
    std::cout << "phase=highlighter_config complete" << std::endl;

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-child-XXXXXX" ) );
    if ( !tempDir.isValid() ) {
        return 30;
    }
    std::cout << "phase=highlighter_tempdir complete" << std::endl;

    {
        std::cout << "phase=highlighter_storage_load start" << std::endl;
        auto set = loadHighlighterSet( tempDir.filePath( QStringLiteral( "highlighter.ini" ) ),
                                       indexes );
        std::cout << "phase=highlighter_storage_load complete" << std::endl;

        std::cout << "phase=highlighter_compile start" << std::endl;
        set.compile();
        std::cout << "phase=highlighter_compile complete" << std::endl;

        HighlightedMatchRanges matches;
        std::cout << "phase=highlighter_match start" << std::endl;
        const auto matchType = set.matchLine( regressionSampleLine(), matches );
        std::cout << "phase=highlighter_match complete type=" << static_cast<int>( matchType )
                  << " empty=" << matches.empty() << std::endl;
        if ( matchType == HighlighterMatchType::NoMatch || matches.empty() ) {
            return 31;
        }

        std::cout << "phase=highlighter_teardown start" << std::endl;
    }
    std::cout << "phase=highlighter_teardown complete" << std::endl;

    std::cout << "highlighter compile ok indexes=" << joinIndexes( indexes ).toStdString()
              << std::endl;
    return 0;
}

int runHighlighterCollectionChild( const std::vector<int>& indexes, AllocatorMode allocator )
{
    const auto allocatorResult = configureHsAllocator( allocator );
    if ( allocatorResult != HS_SUCCESS ) {
        std::cerr << "hs_set_allocator failed rc=" << allocatorResult << std::endl;
        return 39;
    }
    configureProductLikeTestState();

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-collection-XXXXXX" ) );
    if ( !tempDir.isValid() ) {
        return 40;
    }

    auto collection = loadHighlighterCollection(
        tempDir.filePath( QStringLiteral( "collection.ini" ) ), indexes );
    HighlightedMatchRanges matches;
    const auto matchType = collection.currentActiveSet().matchLine( regressionSampleLine(), matches );
    if ( matchType == HighlighterMatchType::NoMatch || matches.empty() ) {
        return 41;
    }

    std::cout << "highlighter collection restore ok indexes="
              << joinIndexes( indexes ).toStdString() << std::endl;
    return 0;
}

int runVectorscanChild( const ChildOptions& options )
{
    switch ( options.childCase ) {
    case ChildCase::DirectSinglePrefilter:
        return runDirectSingleChild( options.indexes, options.allocator );
    case ChildCase::DirectMultiPrefilter:
        return runDirectMultiChild( options.indexes, options.allocator );
    case ChildCase::DirectMultiNoopScanPrefilter:
        return runDirectMultiScanChild( options.indexes, options.allocator,
                                        DirectScanPlan{ false, false, false } );
    case ChildCase::DirectMultiScanPrefilter:
        return runDirectMultiScanChild( options.indexes, options.allocator,
                                        DirectScanPlan{ false, false, true } );
    case ChildCase::DirectMultiCloneScanPrefilter:
        return runDirectMultiScanChild( options.indexes, options.allocator,
                                        DirectScanPlan{ true, false, true } );
    case ChildCase::DirectMultiCompileSimdutfScanPrefilter:
        return runDirectMultiScanChild( options.indexes, options.allocator,
                                        DirectScanPlan{ false, true, true } );
    case ChildCase::SimdutfConvert:
        return runSimdutfChild();
    case ChildCase::HighlighterCompilePrefilter:
        return runHighlighterCompileChild( options.indexes, options.allocator );
    case ChildCase::HighlighterCollectionRestorePrefilter:
        return runHighlighterCollectionChild( options.indexes, options.allocator );
    }

    return 99;
}

ChildRunResult runChildProcess( ChildCase childCase, AllocatorMode allocator,
                                const std::vector<int>& indexes )
{
    ChildRunResult result;

    QProcess process;
    process.setProgram( QCoreApplication::applicationFilePath() );
    process.setProcessChannelMode( QProcess::MergedChannels );
    process.setArguments( { QStringLiteral( "-platform" ),
                            QStringLiteral( "offscreen" ),
                            QStringLiteral( "--vectorscan-child=" ) + childCaseName( childCase ),
                            QStringLiteral( "--vectorscan-allocator=" )
                                + allocatorName( allocator ),
                            QStringLiteral( "--vectorscan-indexes=" ) + joinIndexes( indexes ) } );
    process.start();

    result.started = process.waitForStarted();
    if ( !result.started ) {
        result.output = process.errorString();
        return result;
    }

    result.finished = process.waitForFinished( kChildTimeoutMs );
    result.output = QString::fromUtf8( process.readAllStandardOutput() );
    result.exitStatus = process.exitStatus();
    result.exitCode = process.exitCode();
    return result;
}

void requireSuccessfulChildRun( ChildCase childCase, AllocatorMode allocator,
                                const std::vector<int>& indexes )
{
    const auto result = runChildProcess( childCase, allocator, indexes );

    INFO( "child case=" << childCaseName( childCase ).toStdString() << " allocator="
                        << allocatorName( allocator ).toStdString() << " indexes="
                        << joinIndexes( indexes ).toStdString() );
    INFO( "child output:\n" << result.output.toStdString() );

    REQUIRE( result.started );
    REQUIRE( result.finished );
    REQUIRE( result.exitStatus == QProcess::NormalExit );
    REQUIRE( result.exitCode == 0 );
}

} // namespace

TEST_CASE( "Product-like backend is active in vectorscan test binary", "[vectorscan][backend]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

#ifdef KLOGG_HAS_VECTORSCAN
    REQUIRE( config.regexpEngine() == RegexpEngine::Vectorscan );
#else
    REQUIRE( config.regexpEngine() == RegexpEngine::QRegularExpression );
#endif
}

TEST_CASE( "Multi-pattern prefilters do not build an unused buffer scanner",
           "[vectorscan][buffer]" )
{
    HsRegularExpression expression( makeRegressionPatterns( { 1, 3 } ) );

    REQUIRE( expression.isValid() );
    REQUIRE_FALSE( expression.hasBufferScanner() );
}

TEST_CASE( "Block scan accepts zero-length Vectorscan history", "[vectorscan][buffer]" )
{
    HsRegularExpression expression(
        RegularExpressionPattern{ QStringLiteral( R"(\d{3}9$)" ) } );
    auto scanner = expression.createBufferScanner();
    REQUIRE( scanner );

    const QByteArray data{ "1239\n" };
    const klogg::vector<qint64> endOfLines{ static_cast<qint64>( data.size() ) };
    klogg::vector<uint64_t> matchedLines;
    scanner->scan( data.constData(), static_cast<unsigned int>( data.size() ), endOfLines,
                   matchedLines );

    REQUIRE( matchedLines.size() == 1 );
    CHECK( matchedLines.front() == 0 );
}

TEST_CASE( "Direct multi compile/free succeeds for representative patterns",
           "[vectorscan][smoke]" )
{
    configureProductLikeTestState();

    REQUIRE( runDirectMultiChild( { 0, 1, 5 }, AllocatorMode::Crt ) == 0 );
}

TEST_CASE( "Highlighter-backed compile path succeeds for representative patterns",
           "[vectorscan][smoke]" )
{
    configureProductLikeTestState();

    REQUIRE( runHighlighterCompileChild( { 0, 5 }, AllocatorMode::Crt ) == 0 );
    REQUIRE( runHighlighterCollectionChild( { 0, 5 }, AllocatorMode::Crt ) == 0 );
}

TEST_CASE( "Multi-pattern prefilters scan with a no-op callback and CRT allocation",
           "[vectorscan][regression]" )
{
    requireSuccessfulChildRun( ChildCase::DirectMultiNoopScanPrefilter, AllocatorMode::Crt,
                               { 1, 3 } );
}

TEST_CASE( "Multi-pattern prefilters scan directly with CRT allocation",
           "[vectorscan][regression]" )
{
    requireSuccessfulChildRun( ChildCase::DirectMultiScanPrefilter, AllocatorMode::Crt,
                               { 1, 3 } );
}

TEST_CASE( "Multi-pattern prefilters scan with cloned scratch and CRT allocation",
           "[vectorscan][regression]" )
{
    requireSuccessfulChildRun( ChildCase::DirectMultiCloneScanPrefilter, AllocatorMode::Crt,
                               { 1, 3 } );
}

TEST_CASE( "Multi-pattern compile followed by simdutf and scan exits cleanly",
           "[vectorscan][regression]" )
{
    requireSuccessfulChildRun( ChildCase::DirectMultiCompileSimdutfScanPrefilter,
                               AllocatorMode::Crt, { 1, 3 } );
}

TEST_CASE( "Highlighter sample converts through simdutf in isolation",
           "[vectorscan][regression]" )
{
    requireSuccessfulChildRun( ChildCase::SimdutfConvert, AllocatorMode::Crt, { 1, 3 } );
}

TEST_CASE( "Multi-pattern highlighter prefilters exit cleanly with CRT allocation",
           "[vectorscan][regression]" )
{
    requireSuccessfulChildRun( ChildCase::HighlighterCompilePrefilter, AllocatorMode::Crt,
                               { 1, 3 } );
}

TEST_CASE( "Representative child cases succeed for both allocators", "[vectorscan][child]" )
{
    const std::array childCases = {
        ChildCase::DirectSinglePrefilter,
        ChildCase::DirectMultiPrefilter,
        ChildCase::HighlighterCompilePrefilter,
        ChildCase::HighlighterCollectionRestorePrefilter,
    };

    for ( const auto allocator : { AllocatorMode::Crt, AllocatorMode::Mimalloc } ) {
        for ( const auto childCase : childCases ) {
            requireSuccessfulChildRun( childCase, allocator, { 0, 5 } );
        }
    }
}

TEST_CASE( "Windows VectorScan regression search space exits cleanly",
           "[vectorscan][regression]" )
{
#if defined(Q_OS_WIN) && defined(_MSC_VER)
    const std::array childCases = {
        ChildCase::DirectSinglePrefilter,
        ChildCase::DirectMultiPrefilter,
        ChildCase::HighlighterCompilePrefilter,
        ChildCase::HighlighterCollectionRestorePrefilter,
    };

    const auto searchSpace = buildSearchSpace();
    for ( const auto allocator : { AllocatorMode::Crt, AllocatorMode::Mimalloc } ) {
        for ( const auto childCase : childCases ) {
            for ( const auto& indexes : searchSpace ) {
                requireSuccessfulChildRun( childCase, allocator, indexes );
            }
        }
    }
#else
    SUCCEED( "Full allocator regression search is only required on Windows/MSVC." );
#endif
}

int main( int argc, char* argv[] )
{
    QApplication app( argc, argv );

    logging::enableLogging( true, logging::LogLevel::Warning );
    configureTestTempDir();

    if ( const auto childOptions = parseChildOptions( QCoreApplication::arguments() ) ) {
        return runVectorscanChild( *childOptions );
    }

    configureProductLikeTestState();
    return Catch::Session().run( argc, argv );
}
