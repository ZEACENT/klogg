#include "indexedcontent.h"

#include <QDir>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace {
std::atomic<uint64_t> nextContentIdentity{ 1 };
constexpr qint64 BoundaryWidth = sizeof( qint64 );

void writeAll( QFile& file, const char* bytes, qint64 size )
{
    qint64 written = 0;
    while ( written < size ) {
        const auto count = file.write( bytes + written, size - written );
        if ( count <= 0 ) {
            throw IndexedContentError( "Cannot write indexed content spill: "
                                       + file.errorString().toStdString() );
        }
        written += count;
    }
    if ( !file.flush() ) {
        throw IndexedContentError( "Cannot flush indexed content spill: "
                                   + file.errorString().toStdString() );
    }
}
}

// Logical content is append-only. Migration and seek-based file IO are guarded
// here, never by IndexingData's mutex. A snapshot freezes the readable prefix.
class IndexedContentStorage {
public:
    IndexedContentStorage( size_t memoryBudget, const QString& temporaryDirectory )
        : memoryBudget_( memoryBudget )
        , temporaryDirectory_( temporaryDirectory )
        , identity_( nextContentIdentity.fetch_add( 1, std::memory_order_relaxed ) )
    {
    }

    uint64_t identity() const { return identity_; }

    void append( qint64 expectedOffset, const char* bytes, qint64 size )
    {
        const std::lock_guard<std::mutex> lock( mutex_ );
        if ( expectedOffset != size_ || size < 0
             || size > std::numeric_limits<qint64>::max() - size_ ) {
            throw IndexedContentError( "Indexed content append is not contiguous" );
        }
        if ( size == 0 ) {
            return;
        }
        const auto nextSize = size_ + size;
        if ( !file_ && static_cast<uint64_t>( nextSize ) <= memoryBudget_ ) {
            const auto required = static_cast<size_t>( nextSize );
            if ( required > memory_.capacity() ) {
                // Explicit growth keeps allocated capacity within the budget.
                const auto growth = memory_.capacity() > memoryBudget_ / 2
                                        ? memoryBudget_ : memory_.capacity() * 2;
                memory_.reserve( std::max( required, growth ) );
            }
            memory_.insert( memory_.end(), bytes, bytes + size );
        }
        else if ( !file_ ) {
            const auto directory = temporaryDirectory_.isEmpty() ? QDir::tempPath()
                                                                  : temporaryDirectory_;
            auto spill = std::make_unique<QTemporaryFile>(
                QDir( directory ).filePath( "klogg-indexed-XXXXXX" ) );
            // QTemporaryFile creates an owner-only file and owns its deletion.
            // Keep resident bytes until the migration and new append succeed.
            if ( !spill->open() ) {
                throw IndexedContentError( "Cannot create indexed content spill: "
                                           + spill->errorString().toStdString() );
            }
            writeAll( *spill, memory_.data(), size_ );
            writeAll( *spill, bytes, size );
            file_ = std::move( spill );
            std::vector<char>{}.swap( memory_ );
        }
        else {
            if ( !file_->seek( size_ ) ) {
                throw IndexedContentError( "Cannot seek indexed content spill for append" );
            }
            writeAll( *file_, bytes, size );
        }
        size_ = nextSize;
    }

    QByteArray read( qint64 offset, qint64 length ) const
    {
        const std::lock_guard<std::mutex> lock( mutex_ );
        if ( offset < 0 || offset > size_ || length < 0 || length > size_ - offset
             || length > std::numeric_limits<int>::max() ) {
            throw IndexedContentError( "Indexed content read is outside retained storage" );
        }
        if ( length == 0 ) {
            return {};
        }
        if ( !file_ ) {
            return { memory_.data() + offset, static_cast<int>( length ) };
        }
        if ( !file_->seek( offset ) ) {
            throw IndexedContentError( "Cannot seek indexed content spill for reading" );
        }
        QByteArray result( static_cast<int>( length ), Qt::Uninitialized );
        qint64 total = 0;
        while ( total < length ) {
            const auto count = file_->read( result.data() + total, length - total );
            if ( count <= 0 ) {
                throw IndexedContentError( "Cannot read retained indexed content: "
                                           + file_->errorString().toStdString() );
            }
            total += count;
        }
        return result;
    }

    qint64 size() const
    {
        const std::lock_guard<std::mutex> lock( mutex_ );
        return size_;
    }

    size_t residentBytes() const
    {
        const std::lock_guard<std::mutex> lock( mutex_ );
        return memory_.capacity();
    }

    QString spillFileNameForTesting() const
    {
        const std::lock_guard<std::mutex> lock( mutex_ );
        if ( !file_ ) {
            return {};
        }
        // On Linux, Qt may use O_TMPFILE until fileName() explicitly requests
        // a directory entry. Keep that side effect exclusive to test observation.
        auto name = file_->fileName();
        if ( name.isEmpty() ) {
            throw IndexedContentError( "Cannot materialize indexed content spill name: "
                                       + file_->errorString().toStdString() );
        }
        return name;
    }

private:
    mutable std::mutex mutex_;
    size_t memoryBudget_;
    QString temporaryDirectory_;
    uint64_t identity_;
    std::vector<char> memory_;
    std::unique_ptr<QTemporaryFile> file_;
    qint64 size_{};
};

LinesCount IndexedContentSnapshot::lineCount() const
{
    // NOLINTNEXTLINE(modernize-return-braced-init-list) -- The strong typedef constructor is explicit.
    return LinesCount( completeLines_.get() + ( fakeFinalLF_ ? 1u : 0u ) );
}

OffsetInFile IndexedContentSnapshot::lineEnd( LineNumber line ) const
{
    if ( line.get() >= lineCount().get() ) {
        throw IndexedContentError( "Line is outside indexed content snapshot" );
    }
    if ( line.get() == completeLines_.get() ) {
        // NOLINTNEXTLINE(modernize-return-braced-init-list) -- The strong typedef constructor is explicit.
        return OffsetInFile( byteSize_ + 1 );
    }
    const auto bytes = boundaries_->read( static_cast<qint64>( line.get() ) * BoundaryWidth,
                                          BoundaryWidth );
    qint64 offset{};
    std::memcpy( &offset, bytes.constData(), sizeof( offset ) );
    // NOLINTNEXTLINE(modernize-return-braced-init-list) -- The strong typedef constructor is explicit.
    return OffsetInFile( offset );
}

klogg::vector<OffsetInFile> IndexedContentSnapshot::lineEnds( LineNumber first,
                                                            LinesCount count ) const
{
    if ( first.get() > lineCount().get() || count.get() > lineCount().get() - first.get()
         || count.get() > static_cast<uint64_t>( std::numeric_limits<int>::max() / BoundaryWidth ) ) {
        throw IndexedContentError( "Line range is outside indexed content snapshot" );
    }
    klogg::vector<OffsetInFile> result;
    result.reserve( static_cast<size_t>( count.get() ) );
    const auto completeCount = first.get() < completeLines_.get()
                                  ? std::min( count.get(), completeLines_.get() - first.get() ) : 0;
    if ( completeCount > 0 ) {
        const auto bytes = boundaries_->read( static_cast<qint64>( first.get() ) * BoundaryWidth,
                                              static_cast<qint64>( completeCount ) * BoundaryWidth );
        for ( uint64_t index = 0; index < completeCount; ++index ) {
            qint64 offset{};
            std::memcpy( &offset, bytes.constData() + index * sizeof( offset ), sizeof( offset ) );
            result.emplace_back( offset );
        }
    }
    if ( completeCount < count.get() ) {
        result.emplace_back( byteSize_ + 1 );
    }
    return result;
}

QByteArray IndexedContentSnapshot::readBytes( qint64 offset, qint64 length ) const
{
    if ( offset < 0 || offset > byteSize_ || length < 0 || length > byteSize_ - offset ) {
        throw IndexedContentError( "Read is outside indexed content snapshot" );
    }
    return bytes_->read( offset, length );
}

size_t IndexedContentSnapshot::residentBytes() const
{
    return bytes_->residentBytes() + boundaries_->residentBytes();
}

QStringList IndexedContentSnapshot::spillFileNamesForTesting() const
{
    QStringList names;
    for ( const auto& storage : { bytes_, boundaries_ } ) {
        const auto name = storage->spillFileNameForTesting();
        if ( !name.isEmpty() ) {
            names.push_back( name );
        }
    }
    return names;
}

IndexedContentBuilder::IndexedContentBuilder()
    : IndexedContentBuilder( Options{} )
{
}

IndexedContentBuilder::IndexedContentBuilder( const Options& options )
    : bytes_( std::make_shared<IndexedContentStorage>( options.byteMemoryBudget,
                                                      options.temporaryDirectory ) )
    , boundaries_( std::make_shared<IndexedContentStorage>( options.boundaryMemoryBudget,
                                                           options.temporaryDirectory ) )
{
}

IndexedContentBuilder::IndexedContentBuilder( const IndexedContentSnapshot::Ptr& previous )
{
    if ( !previous ) {
        *this = IndexedContentBuilder{};
        return;
    }
    bytes_ = previous->bytes_;
    boundaries_ = previous->boundaries_;
    byteSize_ = previous->byteSize_;
    completeLines_ = previous->completeLines_;
    if ( completeLines_.get() > 0 ) {
        lastCompleteEnd_ = previous->lineEnd( LineNumber( completeLines_.get() - 1 ) ).get();
    }
}

void IndexedContentBuilder::appendBlock( qint64 offset, const char* bytes, size_t size,
                                         const klogg::vector<OffsetInFile>& completeLineEnds )
{
    if ( offset != byteSize_ || size > static_cast<uint64_t>( std::numeric_limits<qint64>::max() - byteSize_ )
         || ( size != 0 && !bytes ) || bytes_->size() != byteSize_
         || boundaries_->size() != static_cast<qint64>( completeLines_.get() ) * BoundaryWidth ) {
        throw IndexedContentError( "Indexed content append does not extend the current version" );
    }
    const auto nextSize = byteSize_ + static_cast<qint64>( size );
    const auto maxBoundaries = static_cast<uint64_t>( std::numeric_limits<qint64>::max() / BoundaryWidth );
    if ( completeLineEnds.size() > maxBoundaries - completeLines_.get() ) {
        throw IndexedContentError( "Indexed content has too many line boundaries" );
    }
    auto previousEnd = lastCompleteEnd_;
    for ( const auto end : completeLineEnds ) {
        if ( end.get() <= previousEnd || end.get() > nextSize ) {
            throw IndexedContentError( "Indexed content line boundaries are not ordered within its extent" );
        }
        previousEnd = end.get();
    }

    // Bound staging memory even for a block consisting entirely of newlines.
    std::array<qint64, 8192> encoded{};
    size_t copied = 0;
    while ( copied < completeLineEnds.size() ) {
        const auto count = std::min( encoded.size(), completeLineEnds.size() - copied );
        for ( size_t index = 0; index < count; ++index ) {
            encoded[ index ] = completeLineEnds[ copied + index ].get();
        }
        const auto boundaryOffset = static_cast<qint64>( completeLines_.get() + copied ) * BoundaryWidth;
        boundaries_->append( boundaryOffset, reinterpret_cast<const char*>( encoded.data() ),
                              static_cast<qint64>( count ) * BoundaryWidth );
        copied += count;
    }
    bytes_->append( offset, bytes, static_cast<qint64>( size ) );
    // Publish counters only when both stores have accepted the complete block.
    byteSize_ = nextSize;
    completeLines_ = LinesCount( completeLines_.get() + completeLineEnds.size() );
    lastCompleteEnd_ = previousEnd;
}

IndexedContentSnapshot::Ptr IndexedContentBuilder::snapshot( const QByteArray& codecName,
                                                             bool fakeFinalLF ) const
{
    const auto hasTail = fakeFinalLF && lastCompleteEnd_ < byteSize_;
    if ( hasTail && byteSize_ == std::numeric_limits<qint64>::max() ) {
        throw IndexedContentError( "Indexed content synthetic boundary overflows" );
    }
    auto result = std::shared_ptr<IndexedContentSnapshot>( new IndexedContentSnapshot );
    result->bytes_ = bytes_;
    result->boundaries_ = boundaries_;
    result->version_ = nextContentIdentity.fetch_add( 1, std::memory_order_relaxed );
    result->incarnation_ = bytes_->identity();
    result->byteSize_ = byteSize_;
    result->completeLines_ = completeLines_;
    result->fakeFinalLF_ = hasTail;
    result->codecName_ = codecName;
    return result;
}
