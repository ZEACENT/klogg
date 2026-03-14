#include "searchablelogdata.h"

#include <limits>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#include <simdutf.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "log.h"

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

            auto decodedLine = textDecoder.decoder->toUnicode(
                buffer.data() + lineStart, type_safe::narrow_cast<int>( length ) );

            if ( !prefilterPattern.pattern().isEmpty() ) {
                decodedLine.remove( prefilterPattern );
            }

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

klogg::vector<std::string_view> SearchableLogData::RawLines::buildUtf8View() const
{
    klogg::vector<std::string_view> lines;
    if ( this->endOfLines.empty() || textDecoder.decoder == nullptr ) {
        return lines;
    }

    try {
        lines.reserve( endOfLines.size() );

        std::string_view wholeString;

        if ( prefilterPattern.pattern().isEmpty() && textDecoder.encodingParams.isUtf8Compatible ) {
            wholeString = std::string_view( buffer.data(), buffer.size() );
        }
        else {
            QString utf16Data;
            if ( prefilterPattern.pattern().isEmpty() && textDecoder.encodingParams.isUtf16LE ) {
                utf16Data = QString::fromRawData( reinterpret_cast<const QChar*>( buffer.data() ),
                                                  klogg::isize( buffer ) / 2 );
            }
            else {
                utf16Data = textDecoder.decoder->toUnicode( buffer.data(), klogg::isize( buffer ) );
            }

            if ( !prefilterPattern.pattern().isEmpty() ) {
                utf16Data.remove( prefilterPattern );
            }

            utf8Data_.resize( buffer.size() * 4 );
            const auto resultSize = simdutf::convert_utf16_to_utf8(
                reinterpret_cast<const char16_t*>( utf16Data.utf16() ),
                static_cast<size_t>( utf16Data.size() ), utf8Data_.data() );

            wholeString = { utf8Data_.data(), resultSize };
        }

        auto nextLineFeed = wholeString.find( '\n' );
        while ( nextLineFeed != std::string_view::npos ) {
            lines.push_back( wholeString.substr( 0, nextLineFeed ) );
            wholeString.remove_prefix( nextLineFeed + 1 );
            nextLineFeed = wholeString.find( '\n' );
        }

        if ( !wholeString.empty() ) {
            lines.push_back( wholeString );
        }
    } catch ( const std::exception& e ) {
        LOG_ERROR << "failed to transform lines to utf8 " << e.what();
        const auto lastLineOffset = utf8Data_.size();
        lines.reserve( this->endOfLines.size() - lines.size() );
        while ( lines.size() < this->endOfLines.size() ) {
            lines.emplace_back( utf8Data_.data() + lastLineOffset,
                                utf8Data_.size() - lastLineOffset );
        }
    }

    return lines;
}
