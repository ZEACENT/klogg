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
}

void StreamingLogData::appendUtf8( const QByteArray& data )
{
    const auto previousLineCount = captureStore_.lineCount();
    captureStore_.appendUtf8( data );
    if ( captureStore_.lineCount() != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }
}

void StreamingLogData::finishInput()
{
    const auto previousLineCount = captureStore_.lineCount();
    captureStore_.finishInput();
    if ( captureStore_.lineCount() != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }
}

void StreamingLogData::clearCapture()
{
    captureStore_.clear();
    Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    scheduleLoadingFinished();
}

bool StreamingLogData::bindOutputFile( const QString& outputPath )
{
    return captureStore_.bindOutputFile( outputPath );
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
