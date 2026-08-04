#ifndef CAPTURESTORE_H
#define CAPTURESTORE_H

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include "linetypes.h"
#include "rollingfilemanager.h"
#include "searchablelogdata.h"

class CaptureStore {
    struct CapturePathState;
    struct SpilledSegmentFile;

    friend class CaptureStoreTestAccess;

  public:
    struct Limits {
        qint64 segmentTargetBytes = 1024 * 1024;
        qint64 memoryBudgetBytes = 256 * 1024 * 1024;
        qint64 rollingMaxFileSize = 0; // 0 = unlimited
        int rollingBackupCount = 0;
        qint64 maxTotalLines = 0; // 0 = unlimited
    };

    struct Segment {
        qint64 id = 0;
        QString filePath;
        qint64 byteSize = 0;
        qint64 cumulativeEndLine = 0;
        klogg::vector<qint64> lineOffsets;
        klogg::vector<int> lineLengths;
        std::shared_ptr<QByteArray> memoryData;
        std::shared_ptr<SpilledSegmentFile> spilledFile;
        bool spilled = false;
    };

    struct Stats {
        qint64 fileSize = 0;
        qint64 memoryBytes = 0;
        qint64 totalLines = 0;
        int maxLineLength = 0;
        QDateTime lastModified;
    };

    struct AppendResult {
        LineNumber firstLine = 0_lnum;
        LinesCount lineCount = 0_lcount;
        QByteArray rawUtf8Lines;
        klogg::vector<qint64> endOfLines;
    };

    explicit CaptureStore( QString captureId, QString rootPath = {} );
    CaptureStore( QString captureId, QString rootPath, Limits limits );
    ~CaptureStore();

    static QString defaultRootPath();
    static bool isValidCaptureId( const QString& captureId );
    static void cleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                       const QString& rootPath = {},
                                       const QDateTime& preserveModifiedAfter = {} );
    static void cleanupUnusedCapturesAsync(
        const QSet<QString>& retainCaptureIds, const QString& rootPath = {},
        const QDateTime& preserveModifiedAfter = {} );
    static void shutdownBackgroundWorkers();

    CaptureStore( const CaptureStore& ) = delete;
    CaptureStore& operator=( const CaptureStore& ) = delete;

    bool loadFromDisk();
    AppendResult appendUtf8( const QByteArray& data );
    AppendResult finishInput();
    void flush();
    void clear();

    struct TrimResult {
        LinesCount trimmedLines = 0_lcount;
        qint64 trimmedBytes = 0;
    };
    TrimResult trimToLimits();
    TrimResult lastTrimResult() const;
    void clearTrimResult();
    // Reopen the bound output file. When preserveExisting is true the file is
    // opened append-only and the current capture is NOT replayed into it, so
    // previously streamed content already on disk is kept (session restore).
    bool bindOutputFile( const QString& outputPath, bool preserveExisting = false );
    void setOutputFlushedCallback( std::function<void()> callback );
    void setLimits( Limits limits );
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
    struct CleanupCandidate {
        QString capturePath;
        std::shared_ptr<CapturePathState> capturePathState;
        qint64 activityEpoch = 0;
        QByteArray processGeneration;
    };

    static QStringList collectUnusedCapturePaths( const QSet<QString>& retainCaptureIds,
                                                  const QString& rootPath );
    static std::vector<CleanupCandidate> collectUnusedCaptureCandidates(
        const QSet<QString>& retainCaptureIds, const QString& rootPath,
        int gateTimeoutMs = -1,
        const std::function<bool()>& shouldStop = {} );
    static void cleanupCapturePaths( const QStringList& capturePaths,
                                     const QDateTime& preserveModifiedAfter );
    static void cleanupCaptureCandidates(
        const std::vector<CleanupCandidate>& candidates,
        const QDateTime& preserveModifiedAfter,
        const std::function<void( const QString& )>& beforeRemoval = {},
        int gateTimeoutMs = -1,
        const std::function<bool()>& shouldStop = {} );
    static void scheduleCleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                               const QString& rootPath,
                                               const QDateTime& preserveModifiedAfter );

    std::shared_ptr<SpilledSegmentFile> spilledFileLease( const QString& filePath ) const;
    void retireSpilledSegment( Segment& segment );
    std::vector<std::shared_ptr<SpilledSegmentFile>> retireCaptureFiles();
    void synchronizeSegmentIdsWithDisk();
    void failNextRetiredFileRemovalForTesting();
    void failNextCaptureDirectoryRemovalForTesting();
    static void failNextCandidateRecursiveRemovalForTesting(
        const CleanupCandidate& candidate );
    static void setBeforeCandidateActivationCallbackForTesting(
        const CleanupCandidate& candidate, std::function<void()> callback );
    static void setAfterCandidateRecursiveRemovalQuarantineCallbackForTesting(
        const CleanupCandidate& candidate, std::function<void()> callback );
    void failNextSegmentWriteForTesting();
    void setAfterCaptureFilesRetiredCallbackForTesting(
        std::function<void()> callback );
    bool contendForCapturePathAfterGateForTesting(
        std::function<void()> gateAcquired );
    bool holdCapturePathGateForTesting( std::function<void()> gateAcquired,
                                        std::function<void()> waitForRelease );
    static int setCapturePathGateTimeoutForTesting( int timeoutMs );
    bool hasCapturePathCoordinationOwnershipForTesting() const;
    QString capturePathActiveMarkerPathForTesting() const;
    QString capturePathIdentity() const;
    void commitLine( const QByteArray& lineBytes, bool terminated );
    void commitLines( const AppendResult& appendResult );
    void ensureCaptureDir( bool startsReplacement = true );
    bool needsNewSegment() const;
    void ensureSegmentIdsAvailable( const AppendResult& appendResult,
                                    qint64 pendingPartialBytes );
    Segment& ensureActiveSegment();
    void rebuildCumulativeLineCounts( bool onlyLast = false );
    void enforceMemoryBudget();
    bool spillSegmentToDisk( Segment& segment );
    void persistBufferedSegments();
    bool scanSegment( Segment& segment );
    qint64 takeNextSegmentId();
    QByteArray readSegmentLine( const Segment& segment, int localLine ) const;
    bool writeSegmentToDevice( const Segment& segment, QIODevice* device ) const;
    void appendOutputBytes( const QByteArray& bytes, int lineCount = 1 );
    void flushOutputIfNeeded();
    void resetOutputFlushCounters();
    void trimToWindowSize();

    static constexpr qint64 OutputFlushBytesThreshold = 1024 * 1024;
    static constexpr int OutputFlushLinesThreshold = 1000;

  private:
    QString captureId_;
    QString rootPath_;
    QString capturePath_;
    std::shared_ptr<CapturePathState> capturePathState_;
    QByteArray capturePathActivationToken_;
    QString boundOutputFile_;
    RollingFileManager rollingOutput_;
    Limits limits_;

    klogg::vector<Segment> segments_;
    QByteArray partialLine_;
    qint64 fileSize_ = 0;
    qint64 memoryBytes_ = 0;
    qint64 totalLines_ = 0;
    int maxLineLength_ = 0;
    std::deque<qint64> reservedSegmentIds_;
    QSet<QString> inheritedCaptureFiles_;
    QDateTime lastModified_;
    bool persistBufferedSegmentsOnDestroy_ = true;
    bool preserveTailDuringTrim_ = false;
    bool failNextSegmentWriteForTesting_ = false;
    mutable std::recursive_mutex mutex_;

    qint64 unflushedOutputBytes_ = 0;
    int unflushedOutputLines_ = 0;
    std::function<void()> outputFlushedCallback_;
    mutable std::function<void()> beforeRawSnapshotCopyCallbackForTesting_;
    mutable std::function<void()> beforeSpilledSegmentReadCallbackForTesting_;
    std::function<void()> afterCaptureFilesRetiredCallbackForTesting_;
    TrimResult lastTrimResult_;

    // Spill throttling: avoid frequent small spills
    static constexpr int SpillThrottleMs = 5000;
    qint64 lastSpillTimeMs_ = 0;
};

#endif
