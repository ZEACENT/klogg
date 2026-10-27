#include "folderscanarena.h"

#include "encodingutils.h"
#include "searchablelogdata.h"

#include <QTextCodec>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace klogg::folder {
namespace {
constexpr qint64 BoundaryWidth = static_cast<qint64>( sizeof( qint64 ) );

QTextCodec* requireCodec( const QByteArray& name )
{
    auto* codec = QTextCodec::codecForName( name );
    if ( !codec ) {
        throw SnapshotError( "Folder source encoding is unavailable" );
    }
    return codec;
}

IndexedContentBuilder::Options rowStoreOptions( const IndexedContentBuilder::Options& options )
{
    auto result = options;
    result.byteMemoryBudget = options.boundaryMemoryBudget;
    result.boundaryMemoryBudget = 0;
    return result;
}

qint64 rowBytes( uint64_t rows )
{
    if ( rows > static_cast<uint64_t>( std::numeric_limits<qint64>::max() / BoundaryWidth ) ) {
        throw IndexedContentError( "Folder row boundary extent overflows" );
    }
    return static_cast<qint64>( rows ) * BoundaryWidth;
}

qint64 spanExtent( const std::vector<ScanSpan>& spans )
{
    return spans.empty() ? 0 : spans.back().memberOffset + spans.back().size;
}

// Caller serializes the scan-wide builder. Allocate a new descriptor before
// writing, so allocation failure cannot leave the arena cursor behind its store.
void appendSpans( IndexedContentBuilder& builder, qint64& storeSize,
                  std::vector<ScanSpan>& spans, qint64 memberOffset,
                  const char* bytes, size_t size )
{
    if ( memberOffset != spanExtent( spans ) ) {
        throw IndexedContentError( "Folder member append is not contiguous" );
    }
    if ( size == 0 ) {
        return;
    }
    const bool extend = !spans.empty()
                        && spans.back().arenaOffset + spans.back().size == storeSize;
    if ( !extend ) {
        spans.push_back( { memberOffset, storeSize, 0 } );
    }
    try {
        builder.appendBlock( storeSize, bytes, size, {} );
    }
    catch ( ... ) {
        if ( !extend ) {
            spans.pop_back();
        }
        throw;
    }
    spans.back().size += static_cast<qint64>( size );
    storeSize += static_cast<qint64>( size );
}

QByteArray readSpans( const IndexedContentSnapshot::Ptr& storage,
                      const std::vector<ScanSpan>& spans, qint64 byteSize,
                      qint64 offset, qint64 length, std::atomic<uint64_t>& bytesRead )
{
    if ( offset < 0 || offset > byteSize || length < 0 || length > byteSize - offset
         || length > std::numeric_limits<int>::max() ) {
        throw SnapshotError( "Folder read is outside retained source bytes" );
    }
    bytesRead.fetch_add( static_cast<uint64_t>( length ), std::memory_order_relaxed );
    QByteArray result;
    result.reserve( static_cast<int>( length ) );
    auto span = std::lower_bound( spans.begin(), spans.end(), offset,
                                 []( const ScanSpan& item, qint64 position ) {
                                     return item.memberOffset + item.size <= position;
                                 } );
    try {
        while ( length > 0 && span != spans.end() ) {
            const qint64 within = offset - span->memberOffset;
            if ( within < 0 || within >= span->size ) {
                throw SnapshotError( "Folder source has a missing retained span" );
            }
            const qint64 count = std::min( length, span->size - within );
            result += storage->readBytes( span->arenaOffset + within, count );
            offset += count;
            length -= count;
            ++span;
        }
    }
    catch ( const IndexedContentError& error ) {
        throw SnapshotError( error.what() );
    }
    if ( length != 0 ) {
        throw SnapshotError( "Folder source has incomplete retained bytes" );
    }
    return result;
}
} // namespace

ScanArena::ScanArena( const IndexedContentBuilder::Options& options )
    : builder_( options ), rowBuilder_( rowStoreOptions( options ) )
{
}

void ScanArena::append( std::vector<ScanSpan>& spans, qint64 memberOffset,
                        const QByteArray& bytes )
{
    const std::lock_guard<std::mutex> lock( mutex_ );
    appendSpans( builder_, byteSize_, spans, memberOffset, bytes.constData(),
                 static_cast<size_t>( bytes.size() ) );
}

void ScanArena::appendLineEnds( std::vector<ScanSpan>& spans, uint64_t firstLine,
                                const qint64* ends, size_t count )
{
    const auto offset = rowBytes( firstLine );
    const auto size = rowBytes( count );
    const std::lock_guard<std::mutex> lock( mutex_ );
    appendSpans( rowBuilder_, rowByteSize_, spans, offset,
                 reinterpret_cast<const char*>( ends ), static_cast<size_t>( size ) );
}

ScannedFileContent::Ptr ScanArena::seal( std::vector<ScanSpan> spans, qint64 byteSize,
                                        uint64_t member, const QByteArray& codecName,
                                        LinesCount lineCount, std::vector<ScanSpan> rowSpans )
{
    if ( spanExtent( spans ) != byteSize || spanExtent( rowSpans ) != rowBytes( lineCount.get() ) ) {
        throw IndexedContentError( "Folder member has incomplete retained payload or boundaries" );
    }
    const std::lock_guard<std::mutex> lock( mutex_ );
    auto source = std::make_shared<ScannedFileContent>();
    source->bytes_ = builder_.snapshot( codecName );
    source->rowEnds_ = rowBuilder_.snapshot( {} );
    source->spans_ = std::move( spans );
    source->rowSpans_ = std::move( rowSpans );
    source->codecName_ = codecName;
    source->framing_ = EncodingParameters( requireCodec( codecName ) );
    source->byteSize_ = byteSize;
    source->lineCount_ = lineCount;
    source->member_ = member;
    return source;
}

LogRowId ScannedFileContent::identity( LineNumber line ) const
{
    return { LogRowId::Origin::IndexedFile, bytes_->incarnation(), member_, line.get() };
}

QByteArray ScannedFileContent::readBytes( qint64 offset, qint64 length ) const
{
    return readSpans( bytes_, spans_, byteSize_, offset, length, bytesRead_ );
}

QStringList ScannedFileContent::spillFileNamesForTesting() const
{
    auto names = bytes_->spillFileNamesForTesting();
    names.append( rowEnds_->spillFileNamesForTesting() );
    return names;
}

QString ScannedFileContent::readRecord( qint64 offset, qint64 length,
                                       const QByteArray& displayCodec ) const
{
    const auto bytes = readBytes( offset, length );
    bool hasSeparator = false;
    if ( bytes.size() >= framing_.lineFeedWidth ) {
        const std::string_view tail( bytes.constData() + bytes.size() - framing_.lineFeedWidth,
                                     static_cast<size_t>( framing_.lineFeedWidth ) );
        hasSeparator = encoding::findNextMultiByteDelimeter( framing_, tail, '\n' )
                       == static_cast<size_t>( framing_.lineFeedIndex );
    }
    // Remove the source's physical LF before applying a display override. The
    // synthetic decoder boundary describes only that record's retained content;
    // shared strict decoding still owns BOM/replacement/single-CR normalization.
    const auto contentSize = bytes.size() - ( hasSeparator ? framing_.lineFeedWidth : 0 );
    const auto* codec = displayCodec == codecName_ ? framing_.codec : requireCodec( displayCodec );
    const auto decoderParams = codec == framing_.codec ? framing_ : EncodingParameters( codec );
    SearchableLogData::RawLines raw;
    raw.startLine = 0_lnum;
    raw.buffer.assign( bytes.constData(), bytes.constData() + contentSize );
    raw.endOfLines.push_back( static_cast<qint64>( contentSize ) + decoderParams.lineFeedWidth );
    raw.textDecoder = { std::make_unique<QTextDecoder>( codec ), decoderParams };
    auto decoded = raw.decodeLinesStrict();
    return std::move( decoded.front() );
}

QString ScannedFileContent::readLine( LineNumber line, const QByteArray& displayCodec ) const
{
    if ( line >= lineCount_ ) {
        throw SnapshotError( "Marked row is outside the accepted folder scan cutoff" );
    }
    const bool first = line == 0_lnum;
    const auto offset = rowBytes( first ? 0 : line.get() - 1 );
    const qint64 length = first ? BoundaryWidth : 2 * BoundaryWidth;
    const auto encoded = readSpans( rowEnds_, rowSpans_, rowBytes( lineCount_.get() ),
                                    offset, length, bytesRead_ );
    std::array<qint64, 2> ends{};
    std::memcpy( ends.data(), encoded.constData(), static_cast<size_t>( length ) );
    const auto start = first ? 0 : ends[ 0 ];
    const auto end = first ? ends[ 0 ] : ends[ 1 ];
    if ( start < 0 || end < start || end > byteSize_ ) {
        throw SnapshotError( "Folder retained row boundaries are invalid" );
    }
    return readRecord( start, end - start, displayCodec );
}

} // namespace klogg::folder
