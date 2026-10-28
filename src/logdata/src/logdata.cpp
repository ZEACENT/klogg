/*
 * Copyright (C) 2009, 2010, 2013, 2014, 2015 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

// This file implements LogData, the content of a log file.

#include <algorithm>
#include <exception>
#include <limits>
#include <numeric>
#include <qregularexpression.h>
#include <qtextcodec.h>
#include <string_view>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QFileInfo>

#include "simdutf_wrapper.h"

#include "configuration.h"
#include "containers.h"
#include "linetypes.h"
#include "log.h"
#include "logfiltereddata.h"

#include "logdata.h"

namespace {
class IndexedReadSnapshot final : public SearchableLogData::ReadSnapshot {
  public:
    IndexedReadSnapshot( IndexedContentSnapshot::Ptr content, QTextCodec* codec,
                         const QString& prefilter, AnsiProcessingMode mode )
        : content_( std::move( content ) ), codec_( codec ),
          prefilter_( prefilter, QRegularExpression::CaseInsensitiveOption ), mode_( mode )
    {}

    LinesCount lineCount() const override { return content_->lineCount(); }
    qint64 byteSize() const override { return content_->byteSize(); }
    std::optional<CacheKey> cacheKey() const override
    {
        return CacheKey{ klogg::LogRowId::Origin::IndexedFile, content_->incarnation(),
                         content_->version(), codec_.codec()->name(), prefilter_, mode_ };
    }
    bool isAppendOf( const ReadSnapshot& previous ) const override
    {
        const auto* old = dynamic_cast<const IndexedReadSnapshot*>( &previous );
        return old && content_->incarnation() == old->content_->incarnation()
               && byteSize() >= old->byteSize() && lineCount() >= old->lineCount()
               && codec_.mibEnum() == old->codec_.mibEnum()
               && prefilter_ == old->prefilter_ && mode_ == old->mode_;
    }

    klogg::SnapshotRow row( LineNumber index ) const override
    {
        if ( index.get() >= lineCount().get() ) {
            throw klogg::SnapshotError{ "Snapshot row is out of bounds" };
        }
        const bool tail = index.get() + 1 == lineCount().get()
                          && content_->lineEnd( index ).get() > byteSize();
        return { { klogg::LogRowId::Origin::IndexedFile, content_->incarnation(),
                   tail ? content_->version() : 0, index.get() },
                 index + 1_lcount, true, false };
    }

    SearchableLogData::RawLines getLinesRaw( LineNumber first, LinesCount count ) const override
    {
        SearchableLogData::RawLines lines;
        lines.startLine = first;
        if ( first.get() > lineCount().get()
             || count.get() > lineCount().get() - first.get() ) {
            throw klogg::SnapshotError{ "Snapshot range is out of bounds" };
        }
        if ( count == 0_lcount ) {
            return lines;
        }
        lines.textDecoder = codec_.makeDecoder();
        lines.prefilterPattern = prefilter_;
        lines.ansiProcessingMode = mode_;
        const auto firstByte = first == 0_lnum ? 0 : content_->lineEnd( first - 1_lcount ).get();
        const auto ends = content_->lineEnds( first, count );
        const bool syntheticEnd = ends.back().get() > byteSize();
        const auto lastByte = std::min<qint64>( ends.back().get(), byteSize() );
        const auto bytes = content_->readBytes( firstByte, lastByte - firstByte );
        lines.buffer.assign( bytes.constData(), bytes.constData() + bytes.size() );
        lines.endOfLines.reserve( ends.size() );
        for ( const auto& end : ends ) {
            lines.endOfLines.push_back( end.get() - firstByte );
        }
        if ( syntheticEnd ) {
            // The index's EOF+1 marker is not a real source byte. Supply a
            // codec-sized delimiter to raw matchers without extending the lease.
            const auto width = lines.textDecoder.encodingParams.lineFeedWidth;
            const auto newline = codec_.codec()->fromUnicode( QStringLiteral( "\n" ) ).right( width );
            lines.buffer.insert( lines.buffer.end(), newline.constData(),
                                 newline.constData() + newline.size() );
            lines.endOfLines.back() = lastByte - firstByte + width;
        }
        return lines;
    }

  private:
    IndexedContentSnapshot::Ptr content_;
    TextCodecHolder codec_;
    QRegularExpression prefilter_;
    AnsiProcessingMode mode_;
};
} // namespace

SearchableLogData::ReadSnapshot::Ptr LogData::searchableSnapshot() const
{
    auto content = indexedContentSnapshot();
    return content ? std::make_shared<IndexedReadSnapshot>( std::move( content ), codec_.codec(),
                                                          prefilterPattern_, ansiProcessingMode_ )
                   : nullptr;
}

klogg::LogSnapshot::Ptr LogData::createReadSnapshot( klogg::SnapshotScope ) const
{
    return searchableSnapshot();
}

OptionalLineNumber LogData::rowForIdentity( const klogg::LogRowId& identity ) const
{
    const auto content = indexedContentSnapshot();
    if ( !content || identity.origin != klogg::LogRowId::Origin::IndexedFile
         || identity.incarnation != content->incarnation()
         || identity.row >= content->lineCount().get() ) {
        return {};
    }
    const LineNumber index{ identity.row };
    const bool tail = identity.row + 1 == content->lineCount().get()
                      && content->lineEnd( index ).get() > content->byteSize();
    if ( identity.member != ( tail ? content->version() : 0 ) ) {
        return {};
    }
    return index;
}

IndexedContentSnapshot::Ptr LogData::indexedContentSnapshot() const
{
    return IndexingData::ConstAccessor{ indexing_data_.get() }.getContentSnapshot();
}

LogData::LogData()
    : SearchableLogData()
    , indexing_data_( std::make_shared<IndexingData>() )
    , operationQueue_( [ this ] { attached_file_->attachReader(); } )
    , codec_( QTextCodec::codecForName( "UTF-8" ) )
{
    // Initialise the file watcher
    fileWatcherConnection_
        = connect( &FileWatcher::getFileWatcher(), &FileWatcher::fileChanged, this,
                   &LogData::fileChangedOnDisk, Qt::QueuedConnection );

    auto worker = std::make_unique<LogDataWorker>( indexing_data_ );

    // Forward the update signal
    workerIndexingProgressConnection_
        = connect( worker.get(), &LogDataWorker::indexingProgressed, this,
                   &LogData::loadingProgressed, Qt::QueuedConnection );
    workerIndexingFinishedConnection_
        = connect( worker.get(), &LogDataWorker::indexingFinished, this, &LogData::indexingFinished,
                   Qt::QueuedConnection );
    workerCheckFileChangesFinishedConnection_
        = connect( worker.get(), &LogDataWorker::checkFileChangesFinished, this,
                   &LogData::checkFileChangesFinished, Qt::QueuedConnection );

    operationQueue_.setWorker( std::move( worker ) );

    const auto& config = Configuration::get();
    keepFileClosed_ = config.keepFileClosed();

    if ( keepFileClosed_ ) {
        LOG_INFO << "Keep file closed option is set";
    }

    const auto defaultEncodingMib = config.defaultEncodingMib();
    if ( defaultEncodingMib >= 0 ) {
        codec_.setCodec( QTextCodec::codecForMib( defaultEncodingMib ) );
    }
}

LogData::~LogData()
{
    LOG_DEBUG << "Destroying log data";
    shuttingDown_ = true;
    QObject::disconnect( fileWatcherConnection_ );

    // Stop worker activity so no more queued worker callbacks are produced while
    // this QObject is tearing down.
    operationQueue_.shutdown();

    // A queued file-changed or worker callback may already be posted for this
    // object. Purge them before this instance is destroyed.
    QCoreApplication::removePostedEvents( this );

    // Deregister from the FileWatcher singleton. The watcher outlives every
    // LogData (it is never destroyed), so without this the watch list would
    // keep every file ever opened for the lifetime of the process; in the
    // serial integration-test binary that means dead temp paths piling up and
    // the polling timer emitting cross-test file-changed traffic.
    //
    // The removal is keyed on actual registration (fileWatcherRegistered_ is
    // only set after a successful indexing) because EfswFileWatcher::removeFile
    // logs a warning for unknown paths. removeFile() itself is safe here: it
    // only posts a task to the watcher's SerialExecutor, which is alive for
    // the whole process and serializes this behind the matching addFile.
    //
    // Known limitation (pre-existing): if two LogData instances watch the same
    // file, addFile dedups to a single watch entry, so the first destructor
    // removes the watch for both. Opening the same file in two tabs therefore
    // loses change notifications on the surviving tab. That trade-off predates
    // this guard and is preferable to leaking an entry per closed tab.
    if ( fileWatcherRegistered_ ) {
        fileWatcherRegistered_ = false;
        try {
            FileWatcher::getFileWatcher().removeFile( indexingFileName_ );
        } catch ( const std::exception& error ) {
            LOG_ERROR << "Failed to deregister file from watcher: " << error.what();
        }
    }
}

void LogData::setPrefilter( const QString& prefilterPattern )
{
    {
        IndexingData::MutateAccessor scopedAccessor{ indexing_data_.get() };
        if ( prefilterPattern_ == prefilterPattern ) {
            return;
        }
        prefilterPattern_ = prefilterPattern;
    }
    notifyInterpretationChanged();
}

void LogData::setAnsiProcessingMode( AnsiProcessingMode mode )
{
    {
        IndexingData::MutateAccessor scopedAccessor{ indexing_data_.get() };
        if ( ansiProcessingMode_ == mode ) {
            return;
        }
        ansiProcessingMode_ = mode;
    }
    notifyInterpretationChanged();
}

void LogData::attachFile( const QString& fileName )
{
    LOG_DEBUG << "LogData::attachFile " << fileName.toStdString();

    if ( attached_file_ ) {
        // We cannot reattach
        throw CantReattachErr();
    }

    indexingFileName_ = fileName;
    attached_file_.reset( new FileHolder( keepFileClosed_ ) );
    attached_file_->open( indexingFileName_ );

    operationQueue_.enqueueOperation<AttachOperation>( fileName );
}

void LogData::interruptLoading()
{
    operationQueue_.interrupt();
}

qint64 LogData::getFileSize() const
{
    return IndexingData::ConstAccessor{ indexing_data_.get() }.getIndexedSize();
}

QDateTime LogData::getLastModifiedDate() const
{
    return lastModifiedDate_;
}

// Return an initialised LogFilteredData. The search is not started.
std::unique_ptr<LogFilteredData> LogData::getNewFilteredData() const
{
    return std::make_unique<LogFilteredData>( this );
}

void LogData::reload( QTextCodec* forcedEncoding )
{
    operationQueue_.interrupt();

    // Re-open the file, useful in case the file has been moved
    attached_file_->reOpenFile();

    operationQueue_.enqueueOperation<FullReindexOperation>( forcedEncoding );
}

void LogData::fileChangedOnDisk( const QString& filename )
{
    if ( shuttingDown_ || !attached_file_ ) {
        return;
    }

    LOG_INFO << "signalFileChanged " << filename << ", indexed file " << indexingFileName_;

    QFileInfo info( indexingFileName_ );
    const auto currentFileId = FileId::getFileId( indexingFileName_ );
    const auto attachedFileId = attached_file_->getFileId();

    const auto indexedHash = IndexingData::ConstAccessor{ indexing_data_.get() }.getHash();

    LOG_INFO << "current indexed fileSize=" << indexedHash.size;
    LOG_INFO << "current indexed hash=" << indexedHash.fullDigest;
    LOG_INFO << "info file_->size()=" << info.size();

    LOG_INFO << "attached_file_->size()=" << attached_file_->size();
    LOG_INFO << "attached_file_id_ index " << attachedFileId.fileIndex;
    LOG_INFO << "currentFileId index " << currentFileId.fileIndex;

    // In absence of any clearer information, we use the following size comparison
    // to determine whether we are following the same file or not (i.e. the file
    // has been moved and the inode we are following is now under a new name, if for
    // instance log has been rotated). We want to follow the name so we have to reopen
    // the file to ensure we are reading the right one.
    // This is a crude heuristic but necessary for notification services that do not
    // give details (e.g. kqueues)

    const bool isFileIdChanged = attachedFileId != currentFileId;

    if ( !isFileIdChanged && filename != indexingFileName_ ) {
        LOG_INFO << "ignore other file update";
        return;
    }

    if ( isFileIdChanged || ( info.size() != attached_file_->size() )
         || ( !attached_file_->isOpen() ) ) {

        LOG_INFO << "Inconsistent size, or file index, the file might have changed, re-opening";

        attached_file_->reOpenFile();
    }

    operationQueue_.enqueueOperation<CheckDataChangesOperation>();
}

void LogData::indexingFinished( LoadingStatus status )
{
    if ( shuttingDown_ || !attached_file_ ) {
        return;
    }

    // The queued completion signal can arrive before the worker std::thread has
    // fully exited. Join first so temp-file tests and reader detach do not race
    // with the tail of the indexing operation on slower/x86 builds.
    operationQueue_.waitForWorkerDone();

    attached_file_->detachReader();

    LOG_INFO << "indexingFinished for: " << indexingFileName_
             << ( status == LoadingStatus::Successful ) << ", found "
             << IndexingData::ConstAccessor{ indexing_data_.get() }.getNbLines() << " lines.";

    if ( status == LoadingStatus::Successful ) {
        FileWatcher::getFileWatcher().addFile( indexingFileName_ );
        // Paired with the guarded removeFile in ~LogData; the flag, not the
        // FileHolder state, is the source of truth for watcher registration.
        fileWatcherRegistered_ = true;

        // Update the modified date/time if the file exists
        lastModifiedDate_ = QDateTime();
        QFileInfo fileInfo( indexingFileName_ );
        if ( fileInfo.exists() )
            lastModifiedDate_ = fileInfo.lastModified();
    }

    fileChangedOnDisk_ = MonitoredFileStatus::Unchanged;

    LOG_DEBUG << "Sending indexingFinished.";
    Q_EMIT loadingFinished( status );

    operationQueue_.finishOperationAndStartNext();
}

void LogData::checkFileChangesFinished( MonitoredFileStatus status )
{
    if ( shuttingDown_ || !attached_file_ ) {
        return;
    }

    // Ensure the worker operation has fully returned before detaching the
    // reader or scheduling follow-up operations.
    operationQueue_.waitForWorkerDone();

    attached_file_->detachReader();

    LOG_INFO << "File " << indexingFileName_ << " status " << static_cast<uint8_t>( status );

    if ( fileChangedOnDisk_ != MonitoredFileStatus::Truncated ) {
        switch ( status ) {
        case MonitoredFileStatus::Truncated:
            fileChangedOnDisk_ = MonitoredFileStatus::Truncated;
            operationQueue_.enqueueOperation<FullReindexOperation>();
            break;
        case MonitoredFileStatus::DataAdded:
            fileChangedOnDisk_ = MonitoredFileStatus::DataAdded;
            operationQueue_.enqueueOperation<PartialReindexOperation>();
            break;
        case MonitoredFileStatus::Unchanged:
            fileChangedOnDisk_ = MonitoredFileStatus::Unchanged;
            break;
        }
    }
    else {
        operationQueue_.enqueueOperation<FullReindexOperation>();
    }

    if ( status != MonitoredFileStatus::Unchanged
         || fileChangedOnDisk_ == MonitoredFileStatus::Truncated ) {
        Q_EMIT fileChanged( fileChangedOnDisk_ );
    }

    operationQueue_.finishOperationAndStartNext();
}

//
// Implementation of virtual functions
//
LinesCount LogData::doGetNbLine() const
{
    return IndexingData::ConstAccessor{ indexing_data_.get() }.getNbLines();
}

LineLength LogData::doGetMaxLength() const
{
    return IndexingData::ConstAccessor{ indexing_data_.get() }.getMaxLength();
}

LineLength LogData::doGetLineLength( LineNumber line ) const
{
    if ( line >= IndexingData::ConstAccessor{ indexing_data_.get() }.getNbLines() ) {
        return 0_length; /* exception? */
    }

    return LineLength{ doGetExpandedLineString( line ).size() };
}

void LogData::doSetDisplayEncoding( const char* encoding )
{
    LOG_DEBUG << "AbstractLogData::setDisplayEncoding: " << encoding;
    codec_.setCodec( QTextCodec::codecForName( encoding ) );
    auto needReload = false;
    auto useGuessedCodec = false;

    {
        IndexingData::ConstAccessor scopedAccessor{ indexing_data_.get() };

        const QTextCodec* currentIndexCodec = scopedAccessor.getForcedEncoding();
        if ( !currentIndexCodec ) {
            currentIndexCodec = scopedAccessor.getEncodingGuess();
        }

        if ( currentIndexCodec && codec_.mibEnum() != currentIndexCodec->mibEnum() ) {
            if ( codec_.encodingParameters() != EncodingParameters( currentIndexCodec ) ) {
                needReload = true;
                useGuessedCodec = codec_.mibEnum() == scopedAccessor.getEncodingGuess()->mibEnum();
            }
        }
    }

    if ( needReload ) {
        reload( useGuessedCodec ? nullptr : codec_.codec() );
    }
}

QTextCodec* LogData::doGetDisplayEncoding() const
{
    return codec_.codec();
}

QString LogData::doGetLineString( LineNumber line ) const
{
    const auto lines = doGetLines( line, 1_lcount );
    return lines.empty() ? QString{} : lines.front();
}

QString LogData::doGetExpandedLineString( LineNumber line ) const
{
    return untabify( doGetLineString( line ) );
}

klogg::vector<AnsiColorSpan> LogData::doGetLineAnsiColors( LineNumber line ) const
{
    const auto rawLines = getLinesRaw( line, 1_lcount );
    const auto colors = rawLines.decodeLineAnsiColors();
    return colors.empty() ? klogg::vector<AnsiColorSpan>{} : colors.front();
}

// Note this function is also called from the LogFilteredDataWorker thread, so
// data must be protected because they are changed in the main thread (by
// indexingFinished).
klogg::vector<QString> LogData::doGetLines( LineNumber first_line, LinesCount number ) const
{
    return getLinesFromFile( first_line, number,
                             []( QString&& lineData ) { return std::move( lineData ); } );
}

klogg::vector<QString> LogData::doGetExpandedLines( LineNumber first_line, LinesCount number ) const
{
    return getLinesFromFile( first_line, number, []( QString&& lineData ) {
        return untabify( std::move( lineData ) );
    } );
}

LineNumber LogData::doGetLineNumber( LineNumber index ) const
{
    return index;
}

LogData::RawLines LogData::getLinesRaw( LineNumber firstLine, LinesCount number ) const
{
    try {
        const auto snapshot = searchableSnapshot();
        if ( !snapshot || number == 0_lcount ) {
            return {};
        }
        const auto count = snapshot->lineCount().get();
        if ( firstLine.get() > count || number.get() > count - firstLine.get() ) {
            LOG_WARNING << "Lines out of bound asked for";
            return {};
        }
        return snapshot->getLinesRaw( firstLine, number );
    }
    catch ( const std::exception& error ) {
        // Ordinary presentation retains its warning-row policy. Strict search
        // and extraction use the snapshot directly and propagate read failures.
        LOG_ERROR << "Cannot read indexed presentation: " << error.what();
        return {};
    }
}

klogg::vector<QString> LogData::getLinesFromFile( LineNumber firstLine, LinesCount number,
                                                  QString ( *processLine )( QString&& ) ) const
{
    LOG_DEBUG << "firstLine:" << firstLine << " nb:" << number;

    if ( number.get() == 0 ) {
        return klogg::vector<QString>();
    }

    klogg::vector<QString> processedLines;
    try {
        const auto rawLines = getLinesRaw( firstLine, number );
        auto decodedLines = rawLines.decodeLines();

        processedLines.reserve( decodedLines.size() );

        for ( auto&& line : decodedLines ) {
            processedLines.push_back( processLine( std::move( line ) ) );
        }

    } catch ( const std::bad_alloc& e ) {
        LOG_ERROR << "not enough memory " << e.what();
        processedLines.emplace_back( "KLOGG WARNING: not enough memory" );
    }

    processedLines.reserve( number.get() - processedLines.size() );
    while ( processedLines.size() < number.get() ) {
        processedLines.emplace_back( "KLOGG WARNING: failed to read some lines before this one" );
    }

    return processedLines;
}

QTextCodec* LogData::getDetectedEncoding() const
{
    return IndexingData::ConstAccessor{ indexing_data_.get() }.getEncodingGuess();
}

void LogData::doAttachReader() const
{
    attached_file_->attachReader();
}

void LogData::doDetachReader() const
{
    attached_file_->detachReader();
}
