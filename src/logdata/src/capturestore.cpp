#include "capturestore.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>

#include "log.h"
#include "readablesize.h"

namespace {
QString makeSegmentFileName( int id )
{
    return QString( "segment_%1.log" ).arg( id, 6, 10, QLatin1Char( '0' ) );
}

QString decodeUtf8Line( const QByteArray& utf8Line, QTextCodec* codec,
                        const QRegularExpression& prefilterPattern )
{
    auto line = codec ? codec->toUnicode( utf8Line ) : QString::fromUtf8( utf8Line );
    if ( !prefilterPattern.pattern().isEmpty() ) {
        line.remove( prefilterPattern );
    }
    return line;
}
} // namespace

QString CaptureStore::defaultRootPath()
{
    return QDir( QDir::tempPath() ).filePath( "klogg_live" );
}

void CaptureStore::cleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                          const QString& rootPath )
{
    QDir capturesRoot( rootPath.isEmpty() ? defaultRootPath() : rootPath );
    if ( !capturesRoot.exists() ) {
        return;
    }

    const auto entries = capturesRoot.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot );
    for ( const auto& entry : entries ) {
        if ( retainCaptureIds.contains( entry.fileName() ) ) {
            continue;
        }

        QDir orphanCaptureDir( entry.absoluteFilePath() );
        orphanCaptureDir.removeRecursively();
    }
}

CaptureStore::CaptureStore( QString captureId, QString rootPath )
    : CaptureStore( std::move( captureId ), std::move( rootPath ), Limits{} )
{
}

CaptureStore::CaptureStore( QString captureId, QString rootPath, Limits limits )
    : captureId_( std::move( captureId ) )
    , rootPath_( rootPath.isEmpty() ? defaultRootPath() : std::move( rootPath ) )
    , capturePath_( QDir( rootPath_ ).filePath( captureId_ ) )
    , limits_( limits )
{
    ensureCaptureDir();
}

CaptureStore::~CaptureStore()
{
    if ( persistBufferedSegmentsOnDestroy_ ) {
        persistBufferedSegments();
    }
    flush();
}

bool CaptureStore::loadFromDisk()
{
    ensureCaptureDir();

    segments_.clear();
    partialLine_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    nextSegmentId_ = 0;
    lastModified_ = QDateTime{};

    const auto segmentFiles
        = QDir( capturePath_ ).entryList( QStringList{ "segment_*.log" }, QDir::Files,
                                          QDir::Name | QDir::IgnoreCase );
    for ( const auto& fileName : segmentFiles ) {
        Segment segment;
        segment.filePath = QDir( capturePath_ ).filePath( fileName );

        const auto numericId = QFileInfo( fileName ).baseName().mid( QString( "segment_" ).size() );
        segment.id = numericId.toInt();
        scanSegment( segment );
        nextSegmentId_ = qMax( nextSegmentId_, segment.id + 1 );
        segments_.push_back( std::move( segment ) );
    }

    rebuildCumulativeLineCounts();
    enforceMemoryBudget();
    return true;
}

void CaptureStore::appendUtf8( const QByteArray& data )
{
    if ( data.isEmpty() ) {
        return;
    }

    partialLine_.append( data );

    auto newlineIndex = partialLine_.indexOf( '\n' );
    while ( newlineIndex >= 0 ) {
        auto lineBytes = partialLine_.left( newlineIndex );
        partialLine_.remove( 0, newlineIndex + 1 );

        if ( lineBytes.endsWith( '\r' ) ) {
            lineBytes.chop( 1 );
        }

        auto& segment = ensureActiveSegment();
        if ( !segment.memoryData ) {
            segment.memoryData = std::make_shared<QByteArray>();
        }

        const auto offset = segment.memoryData->size();
        segment.memoryData->append( lineBytes );
        segment.memoryData->append( '\n' );
        segment.lineOffsets.push_back( offset );
        segment.lineLengths.push_back( static_cast<int>( lineBytes.size() ) );
        segment.byteSize = segment.memoryData->size();
        segment.spilled = false;

        appendOutputBytes( lineBytes + '\n' );

        fileSize_ += lineBytes.size() + 1;
        totalLines_ += 1;
        maxLineLength_ = qMax( maxLineLength_, static_cast<int>( lineBytes.size() ) );
        lastModified_ = QDateTime::currentDateTime();

        rebuildCumulativeLineCounts();
        rotateSegmentIfNeeded();
        enforceMemoryBudget();

        newlineIndex = partialLine_.indexOf( '\n' );
    }
}

void CaptureStore::flush()
{
    if ( boundOutputHandle_ ) {
        boundOutputHandle_->flush();
    }
}

void CaptureStore::clear()
{
    flush();

    partialLine_.clear();
    segments_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    nextSegmentId_ = 0;
    lastModified_ = QDateTime::currentDateTime();

    const auto files = QDir( capturePath_ ).entryList( QDir::Files | QDir::NoDotAndDotDot );
    for ( const auto& fileName : files ) {
        QFile::remove( QDir( capturePath_ ).filePath( fileName ) );
    }

    if ( !boundOutputFile_.isEmpty() ) {
        QFile outputFile( boundOutputFile_ );
        if ( outputFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
            outputFile.close();
        }
        boundOutputHandle_.reset();
        bindOutputFile( boundOutputFile_ );
    }
}

bool CaptureStore::bindOutputFile( const QString& outputPath )
{
    boundOutputHandle_.reset();
    boundOutputFile_ = outputPath;
    if ( boundOutputFile_.isEmpty() ) {
        return true;
    }

    QDir().mkpath( QFileInfo( boundOutputFile_ ).absolutePath() );
    auto outputFile = std::make_unique<QFile>( boundOutputFile_ );
    if ( !outputFile->open( QIODevice::WriteOnly | QIODevice::Append ) ) {
        boundOutputFile_.clear();
        return false;
    }

    if ( outputFile->size() == 0 ) {
        const auto rawLines = buildRawLines( 0_lnum, lineCount(), QTextCodec::codecForName( "UTF-8" ),
                                             QRegularExpression{} );
        outputFile->write( rawLines.buffer.data(),
                           type_safe::narrow_cast<qint64>( rawLines.buffer.size() ) );
        outputFile->flush();
    }

    boundOutputHandle_ = std::move( outputFile );
    return true;
}

QString CaptureStore::boundOutputFile() const
{
    return boundOutputFile_;
}

QString CaptureStore::captureId() const
{
    return captureId_;
}

QString CaptureStore::capturePath() const
{
    return capturePath_;
}

QString CaptureStore::rootPath() const
{
    return rootPath_;
}

void CaptureStore::deleteCaptureFiles()
{
    flush();
    boundOutputHandle_.reset();
    persistBufferedSegmentsOnDestroy_ = false;
    partialLine_.clear();
    segments_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    nextSegmentId_ = 0;
    lastModified_ = QDateTime{};
    QDir captureDir( capturePath_ );
    if ( captureDir.exists() ) {
        captureDir.removeRecursively();
    }
}

SearchableLogData::RawLines CaptureStore::buildRawLines( LineNumber first, LinesCount number,
                                                         QTextCodec* codec,
                                                         const QRegularExpression& prefilterPattern ) const
{
    SearchableLogData::RawLines rawLines;
    rawLines.startLine = first;
    const auto effectiveCodec = codec ? codec : QTextCodec::codecForName( "UTF-8" );
    rawLines.textDecoder.decoder.reset( effectiveCodec->makeDecoder() );
    rawLines.textDecoder.encodingParams = EncodingParameters( effectiveCodec );
    rawLines.textDecoder.encodingParams.isUtf8Compatible = true;
    rawLines.textDecoder.encodingParams.lineFeedWidth = 1;
    rawLines.prefilterPattern = prefilterPattern;

    const auto availableLines
        = qMax<LineNumber::UnderlyingType>( 0, lineCount().get() - qMin( first.get(), lineCount().get() ) );
    const auto requestedLines = qMin( number.get(), static_cast<LinesCount::UnderlyingType>( availableLines ) );
    for ( LinesCount::UnderlyingType lineOffset = 0; lineOffset < requestedLines; ++lineOffset ) {
        const auto lineData = lineAt( first + LinesCount( lineOffset ), codec, prefilterPattern );
        const auto utf8Line = lineData.toUtf8();
        rawLines.buffer.insert( rawLines.buffer.end(), utf8Line.begin(), utf8Line.end() );
        rawLines.buffer.push_back( '\n' );
        rawLines.endOfLines.push_back( klogg::ssize( rawLines.buffer ) );
    }

    return rawLines;
}

QString CaptureStore::lineAt( LineNumber line, QTextCodec* codec,
                              const QRegularExpression& prefilterPattern ) const
{
    if ( line < 0_lnum || line >= lineCount() ) {
        return {};
    }

    const auto segmentIt
        = std::lower_bound( segments_.cbegin(), segments_.cend(), line.get(),
                            []( const Segment& segment, qint64 value ) {
                                return segment.cumulativeEndLine <= value;
                            } );
    if ( segmentIt == segments_.cend() ) {
        return {};
    }

    const auto segmentIndex = static_cast<size_t>( std::distance( segments_.cbegin(), segmentIt ) );
    const qint64 previousEndLine
        = segmentIndex == 0 ? 0LL : segments_[ segmentIndex - 1 ].cumulativeEndLine;
    const auto localLine = static_cast<int>( line.get<qint64>() - previousEndLine );

    const auto utf8Line = readSegmentLine( *segmentIt, localLine );
    return decodeUtf8Line( utf8Line, codec, prefilterPattern );
}

LineLength CaptureStore::lineLength( LineNumber line ) const
{
    if ( line < 0_lnum || line >= lineCount() ) {
        return 0_length;
    }

    const auto segmentIt
        = std::lower_bound( segments_.cbegin(), segments_.cend(), line.get(),
                            []( const Segment& segment, qint64 value ) {
                                return segment.cumulativeEndLine <= value;
                            } );
    if ( segmentIt == segments_.cend() ) {
        return 0_length;
    }

    const auto segmentIndex = static_cast<size_t>( std::distance( segments_.cbegin(), segmentIt ) );
    const qint64 previousEndLine
        = segmentIndex == 0 ? 0LL : segments_[ segmentIndex - 1 ].cumulativeEndLine;
    const auto localLine = static_cast<int>( line.get<qint64>() - previousEndLine );
    if ( localLine < 0 || localLine >= klogg::isize( segmentIt->lineLengths ) ) {
        return 0_length;
    }
    return LineLength( segmentIt->lineLengths[ static_cast<size_t>( localLine ) ] );
}

LinesCount CaptureStore::lineCount() const
{
    return LinesCount( static_cast<LinesCount::UnderlyingType>( totalLines_ ) );
}

LineLength CaptureStore::maxLineLength() const
{
    return LineLength( maxLineLength_ );
}

CaptureStore::Stats CaptureStore::stats() const
{
    return Stats{ fileSize_, memoryBytes_, totalLines_, maxLineLength_, lastModified_ };
}

void CaptureStore::ensureCaptureDir()
{
    QDir().mkpath( capturePath_ );
}

CaptureStore::Segment& CaptureStore::ensureActiveSegment()
{
    if ( segments_.empty() || segments_.back().byteSize >= limits_.segmentTargetBytes
         || segments_.back().spilled || !segments_.back().memoryData ) {
        Segment segment;
        segment.id = nextSegmentId_++;
        segment.filePath = QDir( capturePath_ ).filePath( makeSegmentFileName( segment.id ) );
        segment.memoryData = std::make_shared<QByteArray>();
        segments_.push_back( std::move( segment ) );
    }

    return segments_.back();
}

void CaptureStore::rotateSegmentIfNeeded()
{
    if ( segments_.empty() || segments_.back().byteSize < limits_.segmentTargetBytes ) {
        return;
    }

    Segment segment;
    segment.id = nextSegmentId_++;
    segment.filePath = QDir( capturePath_ ).filePath( makeSegmentFileName( segment.id ) );
    segment.memoryData = std::make_shared<QByteArray>();
    segments_.push_back( std::move( segment ) );
}

void CaptureStore::rebuildCumulativeLineCounts()
{
    qint64 cumulative = 0;
    memoryBytes_ = 0;
    for ( auto& segment : segments_ ) {
        cumulative += klogg::ssize( segment.lineOffsets );
        segment.cumulativeEndLine = cumulative;
        if ( segment.memoryData ) {
            memoryBytes_ += segment.memoryData->size();
        }
    }
}

void CaptureStore::enforceMemoryBudget()
{
    for ( auto& segment : segments_ ) {
        if ( memoryBytes_ <= limits_.memoryBudgetBytes ) {
            break;
        }
        if ( &segment == &segments_.back() ) {
            break;
        }
        if ( segment.memoryData && spillSegmentToDisk( segment ) ) {
            memoryBytes_ -= segment.memoryData->size();
            segment.memoryData.reset();
        }
    }
}

void CaptureStore::scanSegment( Segment& segment )
{
    QFile file( segment.filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return;
    }

    segment.byteSize = file.size();
    segment.spilled = true;
    lastModified_ = QFileInfo( segment.filePath ).lastModified();

    qint64 offset = 0;
    while ( !file.atEnd() ) {
        const auto lineBytes = file.readLine();
        if ( lineBytes.isEmpty() ) {
            break;
        }
        auto lineLength = lineBytes.endsWith( '\n' ) ? lineBytes.size() - 1 : lineBytes.size();
        if ( lineLength > 0 && lineBytes[ lineLength - 1 ] == '\r' ) {
            --lineLength;
        }
        segment.lineOffsets.push_back( offset );
        segment.lineLengths.push_back( type_safe::narrow_cast<int>( lineLength ) );
        fileSize_ += lineLength + 1;
        maxLineLength_ = qMax( maxLineLength_, type_safe::narrow_cast<int>( lineLength ) );
        offset += lineBytes.size();
    }
    totalLines_ += klogg::ssize( segment.lineOffsets );
}

bool CaptureStore::spillSegmentToDisk( Segment& segment )
{
    if ( segment.spilled || !segment.memoryData ) {
        return true;
    }
    if ( segment.memoryData->isEmpty() ) {
        return true;
    }

    ensureCaptureDir();

    QFile file( segment.filePath );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        LOG_WARNING << "Failed to spill capture segment " << segment.filePath;
        return false;
    }

    if ( file.write( *segment.memoryData ) != segment.memoryData->size() ) {
        LOG_WARNING << "Failed to write capture segment " << segment.filePath;
        return false;
    }

    segment.spilled = true;
    return true;
}

void CaptureStore::persistBufferedSegments()
{
    for ( auto& segment : segments_ ) {
        spillSegmentToDisk( segment );
    }
}

QByteArray CaptureStore::readSegmentLine( const Segment& segment, int localLine ) const
{
    if ( localLine < 0 || localLine >= klogg::isize( segment.lineOffsets ) ) {
        return {};
    }

    if ( segment.memoryData ) {
        const auto lineOffset = segment.lineOffsets[ static_cast<size_t>( localLine ) ];
        const auto lineLength = segment.lineLengths[ static_cast<size_t>( localLine ) ];
        return segment.memoryData->mid( type_safe::narrow_cast<int>( lineOffset ), lineLength );
    }

    QFile file( segment.filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    const auto lineOffset = segment.lineOffsets[ static_cast<size_t>( localLine ) ];
    const auto lineLength = segment.lineLengths[ static_cast<size_t>( localLine ) ];
    file.seek( lineOffset );
    return file.read( lineLength );
}

void CaptureStore::appendOutputBytes( const QByteArray& bytes )
{
    if ( !boundOutputHandle_ ) {
        return;
    }

    boundOutputHandle_->write( bytes );
    boundOutputHandle_->flush();
}
