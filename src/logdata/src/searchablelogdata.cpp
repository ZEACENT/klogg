#include "searchablelogdata.h"

#include <limits>

#include "log.h"
#include "simdutf_wrapper.h"

namespace {
struct Utf8Inspection {
    bool compatible = false;
    bool hasRecordHeader = false;
    bool hasTerminalCr = false;
};

Utf8Inspection inspectUtf8( std::string_view bytes, const EncodingParameters& params,
                            SearchableLogData::RawLines::TrailingCrPolicy policy )
{
    Utf8Inspection result;
    if ( !params.codec || !params.isUtf8Compatible || params.lineFeedWidth != 1 ) return result;
    constexpr int Utf8Mib = 106;
    result.compatible = bytes.empty() || ( params.codec->mibEnum() == Utf8Mib
        ? simdutf::validate_utf8( bytes.data(), bytes.size() )
        : simdutf::validate_ascii( bytes.data(), bytes.size() ) );
    if ( !result.compatible ) return result;
    result.hasTerminalCr = policy == SearchableLogData::RawLines::TrailingCrPolicy::StripFramingCr
        && ( ( !bytes.empty() && bytes.back() == '\r' )
             || bytes.find( "\r\n" ) != std::string_view::npos );
    constexpr std::string_view Bom{ "\xef\xbb\xbf", 3 };
    for ( auto at = bytes.find( Bom ); at != std::string_view::npos;
          at = bytes.find( Bom, at + Bom.size() ) ) {
        if ( at == 0 || bytes[ at - 1 ] == '\n' ) {
            result.hasRecordHeader = true;
            break;
        }
    }
    return result;
}
}

bool SearchableLogData::ReadSnapshot::preservesPresentationOf(
    const klogg::LogSnapshot& previous ) const
{
    if ( this == &previous ) return true;
    const auto* old = dynamic_cast<const ReadSnapshot*>( &previous );
    if ( !old || !isAppendOf( *old ) || lineCount() < old->lineCount() ) return false;
    if ( old->lineCount() == 0_lcount ) return true;
    // isAppendOf permits re-searching an old unterminated final record. Its
    // identity must also survive before presentation positions can be reused.
    const LineNumber last{ old->lineCount().get() - 1 };
    const auto oldRow = old->row( last );
    const auto currentRow = row( last );
    return currentRow.identity == oldRow.identity
        && currentRow.sourceLineNumber == oldRow.sourceLineNumber
        && currentRow.copyable == oldRow.copyable;
}

QString SearchableLogData::ReadSnapshot::lineString( LineNumber index ) const
{
    const auto lines = getLinesRaw( index, 1_lcount ).decodeLinesStrict();
    if ( lines.size() != 1 ) {
        throw klogg::SnapshotError{ "Snapshot did not provide the requested row" };
    }
    return lines.front();
}

klogg::vector<QString> SearchableLogData::RawLines::decodeLinesStrict() const
{
    klogg::vector<QString> lines;
    if ( endOfLines.empty() ) {
        return lines;
    }
    const auto* codec = textDecoder.encodingParams.codec;
    if ( !codec ) {
        throw klogg::SnapshotError{ "Snapshot has no frozen text codec" };
    }
    lines.reserve( endOfLines.size() );
    qint64 start = 0;
    const auto separatorWidth = textDecoder.encodingParams.lineFeedWidth;
    for ( const auto end : endOfLines ) {
        if ( end < start || end - start < separatorWidth ) {
            throw klogg::SnapshotError{ "Snapshot has invalid row boundaries" };
        }
        const auto length = end - start - separatorWidth;
        if ( start > klogg::ssize( buffer ) || length > klogg::ssize( buffer ) - start
             || length >= std::numeric_limits<int>::max() / 2 ) {
            throw klogg::SnapshotError{ "Snapshot row is truncated or exceeds decoder capacity" };
        }
        // A physical record is a complete decoding unit. Stateless conversion
        // flushes incomplete sequences with Qt's replacement policy instead of
        // carrying them across LF into the next row or dropping a final prefix.
        auto text = codec->toUnicode( buffer.data() + start, static_cast<int>( length ) );
        if ( trailingCrPolicy == TrailingCrPolicy::StripFramingCr
             && text.endsWith( QChar::CarriageReturn ) ) {
            text.chop( 1 );
        }
        text = processLogLine( std::move( text ), ansiProcessingMode, prefilterPattern ).text;
        lines.push_back( std::move( text ) );
        start = end;
    }
    return lines;
}

klogg::vector<QString> SearchableLogData::RawLines::decodeLines() const
{
    if ( this->endOfLines.empty() ) {
        return klogg::vector<QString>();
    }

    klogg::vector<QString> decodedLines;
    decodedLines.reserve( this->endOfLines.size() );

    try {
        qint64 lineStart = 0;
        size_t currentLineIndex = 0;
        const auto lineFeedWidth = textDecoder.encodingParams.lineFeedWidth;
        for ( const auto& lineEnd : this->endOfLines ) {
            const auto length = lineEnd - lineStart - lineFeedWidth;
            LOG_DEBUG << "line " << this->startLine.get() + currentLineIndex << ", length "
                      << length;

            constexpr auto maxlength = std::numeric_limits<int>::max() / 2;
            if ( length >= maxlength ) {
                decodedLines.emplace_back( "KLOGG WARNING: this line is too long" );
                break;
            }

            if ( lineStart + length > klogg::ssize( buffer ) ) {
                decodedLines.emplace_back( "KLOGG WARNING: file read failed" );
                LOG_WARNING << "not enough data in buffer";
                break;
            }

            const auto* codec = textDecoder.encodingParams.codec;
            auto decodedLine = codec
                ? codec->toUnicode( buffer.data() + lineStart, type_safe::narrow_cast<int>( length ) )
                : textDecoder.decoder->toUnicode( buffer.data() + lineStart,
                                                   type_safe::narrow_cast<int>( length ) );

            if ( trailingCrPolicy == TrailingCrPolicy::StripFramingCr
                 && decodedLine.endsWith( QChar::CarriageReturn ) ) {
                decodedLine.chop( 1 );
            }
            decodedLine = processLogLine( std::move( decodedLine ), ansiProcessingMode,
                                          prefilterPattern ).text;

            decodedLines.push_back( std::move( decodedLine ) );

            lineStart = lineEnd;
            ++currentLineIndex;
        }
    } catch ( const std::bad_alloc& ) {
        LOG_ERROR << "not enough memory";
        decodedLines.emplace_back( "KLOGG WARNING: not enough memory" );
    }

    decodedLines.reserve( this->endOfLines.size() - decodedLines.size() );
    while ( decodedLines.size() < this->endOfLines.size() ) {
        decodedLines.emplace_back( "KLOGG WARNING: failed to decode some lines before this one" );
    }

    return decodedLines;
}

klogg::vector<klogg::vector<AnsiColorSpan>> SearchableLogData::RawLines::decodeLineAnsiColors() const
{
    klogg::vector<klogg::vector<AnsiColorSpan>> colors;
    if ( this->endOfLines.empty() || ansiProcessingMode != AnsiProcessingMode::Render ) {
        colors.resize( this->endOfLines.size() );
        return colors;
    }

    colors.reserve( this->endOfLines.size() );

    try {
        qint64 lineStart = 0;
        const auto lineFeedWidth = textDecoder.encodingParams.lineFeedWidth;
        for ( const auto& lineEnd : this->endOfLines ) {
            const auto length = lineEnd - lineStart - lineFeedWidth;

            if ( length < 0 || lineStart + length > klogg::ssize( buffer ) ) {
                break;
            }

            const auto* codec = textDecoder.encodingParams.codec;
            auto decodedLine = codec
                ? codec->toUnicode( buffer.data() + lineStart, type_safe::narrow_cast<int>( length ) )
                : textDecoder.decoder->toUnicode( buffer.data() + lineStart,
                                                   type_safe::narrow_cast<int>( length ) );
            if ( trailingCrPolicy == TrailingCrPolicy::StripFramingCr
                 && decodedLine.endsWith( QChar::CarriageReturn ) ) {
                decodedLine.chop( 1 );
            }
            colors.push_back( processLogLine( std::move( decodedLine ), ansiProcessingMode,
                                               prefilterPattern ).colorSpans );

            lineStart = lineEnd;
        }
    } catch ( const std::bad_alloc& ) {
        LOG_ERROR << "not enough memory";
    }

    colors.resize( this->endOfLines.size() );
    return colors;
}

bool SearchableLogData::RawLines::canBorrowNormalizedUtf8(
    std::string_view bytes, const EncodingParameters& params, TrailingCrPolicy policy )
{
    const auto inspected = inspectUtf8( bytes, params, policy );
    return inspected.compatible && !inspected.hasRecordHeader && !inspected.hasTerminalCr;
}

SearchableLogData::RawLines::Utf8Block SearchableLogData::RawLines::buildUtf8Block() const
{
    Utf8Block result;
    if ( endOfLines.empty() ) return result;
    result.lines.reserve( endOfLines.size() );
    result.endOfLines.reserve( endOfLines.size() );

    const auto* rawData = buffer.empty() ? "" : buffer.data();
    const auto inspected = inspectUtf8( std::string_view{ rawData, buffer.size() },
                                        textDecoder.encodingParams, trailingCrPolicy );
    const bool plainUtf8 = ansiProcessingMode == AnsiProcessingMode::Plain
        && prefilterPattern.pattern().isEmpty() && inspected.compatible;
    bool directlyFramed = plainUtf8 && !inspected.hasRecordHeader && !inspected.hasTerminalCr;
    if ( plainUtf8 ) {
        qint64 start = 0;
        for ( const auto end : endOfLines ) {
            const auto length = end - start - 1;
            if ( length < 0 || start > klogg::ssize( buffer )
                 || length > klogg::ssize( buffer ) - start ) {
                throw klogg::SnapshotError{ "UTF8 snapshot has invalid row boundaries" };
            }
            std::string_view line{ rawData + start, static_cast<size_t>( length ) };
            if ( trailingCrPolicy == TrailingCrPolicy::StripFramingCr
                 && !line.empty() && line.back() == '\r' ) {
                line.remove_suffix( 1 );
                directlyFramed = false;
            }
            if ( end > klogg::ssize( buffer ) || buffer[ static_cast<size_t>( end - 1 ) ] != '\n' ) {
                directlyFramed = false;
            }
            result.lines.push_back( line );
            start = end;
        }
        if ( directlyFramed ) {
            result.buffer = std::string_view{ buffer.data(), static_cast<size_t>( endOfLines.back() ) };
            result.endOfLines = endOfLines;
            return result;
        }
    }

    utf8Data_.clear();
    const auto append = [ this, &result ]( std::string_view line ) {
        utf8Data_.insert( utf8Data_.end(), line.begin(), line.end() );
        utf8Data_.push_back( '\n' );
        result.endOfLines.push_back( static_cast<qint64>( utf8Data_.size() ) );
    };
    if ( plainUtf8 && !inspected.hasRecordHeader ) {
        // Only framing/CR normalization changed; retain already valid UTF8 bytes.
        for ( const auto line : result.lines ) append( line );
    }
    else {
        // The display decoder is authoritative for malformed input, codec
        // conversion, ANSI processing, and per-record anchored prefilters.
        for ( const auto& line : decodeLinesStrict() ) {
            const auto encoded = line.toUtf8();
            append( std::string_view{ encoded.constData(), static_cast<size_t>( encoded.size() ) } );
        }
    }
    result.buffer = std::string_view{ utf8Data_.data(), utf8Data_.size() };
    result.lines.clear();
    size_t start = 0;
    for ( const auto end : result.endOfLines ) {
        const auto boundary = static_cast<size_t>( end );
        result.lines.emplace_back( utf8Data_.data() + start, boundary - start - 1 );
        start = boundary;
    }
    return result;
}

klogg::vector<std::string_view> SearchableLogData::RawLines::buildUtf8View() const
{
    return buildUtf8Block().lines;
}
