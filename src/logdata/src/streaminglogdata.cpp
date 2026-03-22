#include "streaminglogdata.h"

#include <QMetaObject>

#include "logfiltereddata.h"

StreamingLogData::StreamingLogData( QString captureId, QString captureRoot )
    : SearchableLogData()
    , captureStore_( std::move( captureId ), std::move( captureRoot ) )
    , codec_( QTextCodec::codecForName( "UTF-8" ) )
{
    captureStore_.loadFromDisk();
    scheduleLoadingFinished();

    outputFlushTimer_.setInterval( 1000 );
    connect( &outputFlushTimer_, &QTimer::timeout, this, [this] {
        captureStore_.flush();
    } );

    captureStore_.setOutputFlushedCallback( [this] {
        // Restart timer so the 1-second countdown begins after each threshold flush.
        // Use QMetaObject::invokeMethod to ensure QTimer is accessed from its owning thread.
        QMetaObject::invokeMethod( &outputFlushTimer_, [this] {
            if ( outputFlushTimer_.isActive() ) {
                outputFlushTimer_.start();
            }
        }, Qt::QueuedConnection );
    } );
}

StreamingLogData::~StreamingLogData()
{
    // Clear callback before members are destroyed to prevent use-after-free.
    // captureStore_ is declared before outputFlushTimer_, so the timer is
    // destroyed first; without this, CaptureStore's destructor could fire
    // the callback against a dead timer.
    captureStore_.setOutputFlushedCallback( nullptr );
}

void StreamingLogData::appendUtf8( const QByteArray& data )
{
    // Restart the flush timer if new data arrives while an output file is bound
    // but the timer is stopped (e.g. after finishInput from a reconnect cycle).
    if ( !outputFlushTimer_.isActive() && !captureStore_.boundOutputFile().isEmpty() ) {
        startOutputFlushTimer();
    }

    const auto previousLineCount = captureStore_.lineCount();
    captureStore_.appendUtf8( data );
    if ( captureStore_.lineCount() != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }
}

void StreamingLogData::finishInput()
{
    stopOutputFlushTimer();
    const auto previousLineCount = captureStore_.lineCount();
    captureStore_.finishInput();
    if ( captureStore_.lineCount() != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }
}

void StreamingLogData::clearCapture()
{
    const auto timerWasActive = outputFlushTimer_.isActive();
    stopOutputFlushTimer();
    captureStore_.clear();

    // clear() internally rebinds the output file if one was bound.
    // Only restart the timer if it was running before the clear,
    // so a clearCapture after finishInput does not revive the timer.
    if ( timerWasActive && !captureStore_.boundOutputFile().isEmpty() ) {
        startOutputFlushTimer();
    }

    Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    scheduleLoadingFinished();
}

bool StreamingLogData::bindOutputFile( const QString& outputPath )
{
    stopOutputFlushTimer();
    const auto result = captureStore_.bindOutputFile( outputPath );
    if ( !outputPath.isEmpty() && result ) {
        startOutputFlushTimer();
    }
    return result;
}

QString StreamingLogData::boundOutputFile() const
{
    return captureStore_.boundOutputFile();
}

QString StreamingLogData::captureId() const
{
    return captureStore_.captureId();
}

QString StreamingLogData::capturePath() const
{
    return captureStore_.capturePath();
}

void StreamingLogData::deleteCaptureFiles()
{
    captureStore_.deleteCaptureFiles();
}

void StreamingLogData::interruptLoading()
{
}

std::unique_ptr<LogFilteredData> StreamingLogData::getNewFilteredData() const
{
    return std::make_unique<LogFilteredData>( this );
}

qint64 StreamingLogData::getFileSize() const
{
    return captureStore_.stats().fileSize;
}

QDateTime StreamingLogData::getLastModifiedDate() const
{
    return captureStore_.stats().lastModified;
}

void StreamingLogData::reload( QTextCodec* forcedEncoding )
{
    if ( forcedEncoding ) {
        codec_.setCodec( forcedEncoding );
    }
    scheduleLoadingFinished();
}

QTextCodec* StreamingLogData::getDetectedEncoding() const
{
    return codec_.codec();
}

void StreamingLogData::setPrefilter( const QString& prefilterPattern )
{
    prefilterPattern_.setPattern( prefilterPattern );
}

SearchableLogData::RawLines StreamingLogData::getLinesRaw( LineNumber first, LinesCount number ) const
{
    return captureStore_.buildRawLines( first, number, codec_.codec(), prefilterPattern_ );
}

bool StreamingLogData::isLiveSource() const
{
    return true;
}

QString StreamingLogData::doGetLineString( LineNumber line ) const
{
    return captureStore_.lineAt( line, codec_.codec(), prefilterPattern_ );
}

QString StreamingLogData::doGetExpandedLineString( LineNumber line ) const
{
    return doGetLineString( line );
}

klogg::vector<QString> StreamingLogData::doGetLines( LineNumber first, LinesCount number ) const
{
    return getLines( first, number );
}

klogg::vector<QString> StreamingLogData::doGetExpandedLines( LineNumber first,
                                                             LinesCount number ) const
{
    return getLines( first, number );
}

LineNumber StreamingLogData::doGetLineNumber( LineNumber index ) const
{
    return index;
}

LinesCount StreamingLogData::doGetNbLine() const
{
    return captureStore_.lineCount();
}

LineLength StreamingLogData::doGetMaxLength() const
{
    return captureStore_.maxLineLength();
}

LineLength StreamingLogData::doGetLineLength( LineNumber line ) const
{
    return captureStore_.lineLength( line );
}

void StreamingLogData::doSetDisplayEncoding( const char* encoding )
{
    codec_.setCodec( QTextCodec::codecForName( encoding ) );
}

QTextCodec* StreamingLogData::doGetDisplayEncoding() const
{
    return codec_.codec();
}

void StreamingLogData::doAttachReader() const
{
}

void StreamingLogData::doDetachReader() const
{
}

void StreamingLogData::scheduleLoadingFinished()
{
    if ( loadingFinishedQueued_ ) {
        return;
    }

    loadingFinishedQueued_ = true;
    QMetaObject::invokeMethod(
        this,
        [ this ] {
            loadingFinishedQueued_ = false;
            Q_EMIT loadingFinished( LoadingStatus::Successful );
        },
        Qt::QueuedConnection );
}

void StreamingLogData::startOutputFlushTimer()
{
    if ( !outputFlushTimer_.isActive() ) {
        outputFlushTimer_.start();
    }
}

void StreamingLogData::stopOutputFlushTimer()
{
    outputFlushTimer_.stop();
}

klogg::vector<QString> StreamingLogData::getLines( LineNumber first, LinesCount number ) const
{
    klogg::vector<QString> lines;
    const auto lastLine = qMin( first.get() + number.get(), doGetNbLine().get() );
    lines.reserve( qMax<LinesCount::UnderlyingType>( 0, lastLine - first.get() ) );
    for ( auto line = first.get(); line < lastLine; ++line ) {
        lines.push_back( doGetLineString( LineNumber( line ) ) );
    }
    return lines;
}
