#include "preparedfilesource.h"
#include "joinedtbbproducer_p.h"
#include "logdataworker.h"

#include <QDir>
#include <exception>
#include <new>

PreparedFileSource::PreparedFileSource() = default;
PreparedFileSource::~PreparedFileSource() = default;

IndexedContentSnapshot::Ptr PreparedFileSource::content() const
{
    return IndexingData::ConstAccessor{ index_.get() }.getContentSnapshot();
}

FileSourcePreparation prepareFileSource(
    const QString& path, const IndexingSettings& settings, AtomicFlag& cancelled,
    QTextCodec* forcedEncoding, std::optional<LineLength> maximumLineLength )
{
    using Status = FileSourcePreparation::Status;
    if ( cancelled ) return { Status::Cancelled, {}, {} };
    if ( path.isEmpty() || !QDir::isAbsolutePath( path ) ) {
        return { Status::InvalidInput, QStringLiteral( "An absolute source path is required" ), {} };
    }
    const auto prepare = [&]() -> FileSourcePreparation {
        auto source = std::unique_ptr<PreparedFileSource>( new PreparedFileSource );
        source->path_ = path;
        source->file_ = std::make_unique<QFile>( path );
        const auto acquired = klogg::platform::openRegularFileReadOnly( *source->file_ );
        if ( cancelled ) return { Status::Cancelled, {}, {} };
        using OpenStatus = klogg::platform::ReadOnlyFileOpenStatus;
        switch ( acquired.status ) {
        case OpenStatus::Missing: return { Status::Missing, acquired.error, {} };
        case OpenStatus::NotRegular: return { Status::NotRegular, acquired.error, {} };
        case OpenStatus::Failed: return { Status::OpenFailure, acquired.error, {} };
        case OpenStatus::Opened: break;
        }
        if ( !acquired.metadata ) {
            return { Status::OpenFailure, QStringLiteral( "Acquired metadata is unavailable" ), {} };
        }
        source->metadata_ = *acquired.metadata;
        source->index_ = std::make_shared<IndexingData>();
        FullIndexOperation operation{ path, source->index_, cancelled, forcedEncoding, settings };
        const auto indexed = operation.prepare( *source->file_, source->metadata_.size,
                                                maximumLineLength );
        using IndexStatus = PreparedIndexResult::Status;
        switch ( indexed.status ) {
        case IndexStatus::Cancelled: return { Status::Cancelled, {}, {} };
        case IndexStatus::InvalidInput: return { Status::InvalidInput, indexed.error, {} };
        case IndexStatus::ReadFailure: return { Status::ReadFailure, indexed.error, {} };
        case IndexStatus::ResourceLimit: return { Status::ResourceLimit, indexed.error, {} };
        case IndexStatus::Ready: break;
        }
        if ( cancelled ) return { Status::Cancelled, {}, {} };
        source->file_->moveToThread( nullptr );
        if ( source->file_->thread() ) {
            return { Status::ReadFailure, QStringLiteral( "Cannot transfer the prepared handle" ), {} };
        }
        return { Status::Ready, {}, std::move( source ) };
    };
    try {
        return klogg::runOnJoinedTbbProducer( prepare );
    } catch ( const std::bad_alloc& ) {
        return { Status::ResourceLimit, QStringLiteral( "Insufficient memory for source preparation" ), {} };
    } catch ( const std::exception& error ) {
        return { Status::ReadFailure, QString::fromUtf8( error.what() ), {} };
    }
}
