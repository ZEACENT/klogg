#ifndef CAPTURESTORE_H
#define CAPTURESTORE_H

#include <memory>
#include <mutex>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>

#include "linetypes.h"
#include "searchablelogdata.h"

class CaptureStore {
  public:
    struct Limits {
        qint64 segmentTargetBytes = 1024 * 1024;
        qint64 memoryBudgetBytes = 32 * 1024 * 1024;
    };

    struct Segment {
        int id = 0;
        QString filePath;
        qint64 byteSize = 0;
        qint64 cumulativeEndLine = 0;
        klogg::vector<qint64> lineOffsets;
        klogg::vector<int> lineLengths;
        std::shared_ptr<QByteArray> memoryData;
        bool spilled = false;
    };

    struct Stats {
        qint64 fileSize = 0;
        qint64 memoryBytes = 0;
        qint64 totalLines = 0;
        int maxLineLength = 0;
        QDateTime lastModified;
    };

    explicit CaptureStore( QString captureId, QString rootPath = {} );
    CaptureStore( QString captureId, QString rootPath, Limits limits );
    ~CaptureStore();

    static QString defaultRootPath();
    static void cleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                       const QString& rootPath = {} );

    CaptureStore( const CaptureStore& ) = delete;
    CaptureStore& operator=( const CaptureStore& ) = delete;

    bool loadFromDisk();
    void appendUtf8( const QByteArray& data );
    void finishInput();
    void flush();
    void clear();
    bool bindOutputFile( const QString& outputPath );
    QString boundOutputFile() const;
    QString captureId() const;
    QString capturePath() const;
    QString rootPath() const;
    void deleteCaptureFiles();

    SearchableLogData::RawLines buildRawLines( LineNumber first, LinesCount number,
                                               QTextCodec* codec,
                                               const QRegularExpression& prefilterPattern ) const;
    QString lineAt( LineNumber line, QTextCodec* codec,
                    const QRegularExpression& prefilterPattern ) const;
    LineLength lineLength( LineNumber line ) const;
    LinesCount lineCount() const;
    LineLength maxLineLength() const;
    Stats stats() const;

  private:
    void commitLine( const QByteArray& lineBytes, bool terminated );
    void ensureCaptureDir();
    Segment& ensureActiveSegment();
    void rotateSegmentIfNeeded();
    void rebuildCumulativeLineCounts();
    void enforceMemoryBudget();
    bool spillSegmentToDisk( Segment& segment );
    void persistBufferedSegments();
    void scanSegment( Segment& segment );
    QByteArray readSegmentLine( const Segment& segment, int localLine ) const;
    bool writeSegmentToDevice( const Segment& segment, QIODevice* device ) const;
    bool writeCaptureToDevice( QIODevice* device ) const;
    void appendOutputBytes( const QByteArray& bytes );

  private:
    QString captureId_;
    QString rootPath_;
    QString capturePath_;
    QString boundOutputFile_;
    mutable std::unique_ptr<QFile> boundOutputHandle_;
    Limits limits_;

    klogg::vector<Segment> segments_;
    QByteArray partialLine_;
    qint64 fileSize_ = 0;
    qint64 memoryBytes_ = 0;
    qint64 totalLines_ = 0;
    int maxLineLength_ = 0;
    int nextSegmentId_ = 0;
    QDateTime lastModified_;
    bool persistBufferedSegmentsOnDestroy_ = true;
    mutable std::recursive_mutex mutex_;
};

#endif
