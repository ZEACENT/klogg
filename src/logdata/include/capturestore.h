#ifndef CAPTURESTORE_H
#define CAPTURESTORE_H

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

// Directory that gates capture-path ownership across klogg processes
// (active-store markers + QLockFile stems). Per-user so a different local user
// cannot pre-create or redirect it; see captureCoordinationRoot().
QString captureCoordinationRoot();

class CaptureStore {
    struct CapturePathState;
    struct SpilledSegmentFile;

    friend class CaptureStoreTestAccess;
    friend struct StreamingLogDataTimerTestAccess;
    friend struct LiveSourceStreamingLogDataTestAccess;

public:
    struct Limits {
        qint64 segmentTargetBytes = 1024 * 1024;
        qint64 memoryBudgetBytes = 256 * 1024 * 1024;
        qint64 rollingMaxFileSize = 0; // 0 = unlimited
        int rollingBackupCount = 0;
        qint64 maxTotalLines = 0; // 0 = unlimited
        // In-flight normalization allowance, in addition to 2x resident target.
        // Payload only: indexes, caches, allocator overhead and RSS are separate.
        qint64 ingressBudgetBytes = 16LL * 1024 * 1024;
    };

    struct Segment {
        qint64 id = 0;
        QString filePath;
        qint64 byteSize = 0;
        qint64 cumulativeEndLine = 0;
        klogg::vector<qint64> lineOffsets;
        klogg::vector<int> lineLengths;
        int maxLineLength = 0;
        std::shared_ptr<QByteArray> memoryData;
        std::shared_ptr<SpilledSegmentFile> spilledFile;
        bool spilled = false;
        bool publicationPending = false; // Published file awaits identity lease, never replay it.
        bool unterminated = false;       // A finalized record seals its segment.
    };

    struct Stats {
        qint64 fileSize = 0;
        qint64 memoryBytes = 0;
        qint64 totalLines = 0;
        int maxLineLength = 0;
        QDateTime lastModified;
    };

    // Fixed record sequence: COW pins resident payload, leases pin disk files.
    // Metadata is O(segments), not O(lines); history payload is never read eagerly.
    // Export orchestration should hold one snapshot at a time and release it on cancel.
    class Snapshot {
        friend class CaptureStore;
        struct Part {
            QByteArray memory;
            std::shared_ptr<SpilledSegmentFile> file;
            qint64 bytes = 0;
            bool unterminated = false;
        };
        std::vector<Part> parts_;

    public:
        struct Cursor {
            size_t part = 0;
            qint64 offset = 0;
            bool separatorPending = false;
        };
        struct Chunk {
            QByteArray bytes;
            bool complete = false;
            bool readFailed = false;
        };
        // Caller owns the cursor. No store locks or mutable capture state needed.
        // maxBytes is clamped to 64 KiB. Bytes include separators BETWEEN sealed
        // records, never a synthetic newline at final EOF. readFailed is terminal.
        Chunk readChunk( Cursor& cursor, int maxBytes = 64 * 1024 ) const;
    };
    Snapshot snapshot() const;

    // PartialKnown permits only the unaccepted suffix to be retried. PartialUnknown
    // carries a confirmed lower-bound prefix; NEVER replay the whole input batch.
    enum class AppendDisposition : std::uint8_t {
        Complete,
        RejectedUnchanged,
        PartialKnown,
        PartialUnknown
    };
    enum class CaptureFailure : std::uint8_t {
        Capacity,
        SegmentIds,
        Directory,
        Allocation,
        Unexpected
    };
    enum class PersistenceFailure : std::uint8_t {
        Directory,
        Gate,
        TemporaryCreate,
        Write,
        Flush,
        Publish,
        Unexpected
    };
    struct PersistenceResult {
        qint64 pendingBytes = 0;
        qint64 pendingSegments = 0;
        qint64 pendingPartialBytes = 0;
        std::optional<PersistenceFailure> failure;
        // Remaining monotonic backoff, for a caller-owned precise timer/scheduler.
        std::optional<qint64> retryAfterMs;
        bool complete() const
        {
            return pendingSegments == 0 && pendingPartialBytes == 0 && !failure;
        }
    };

    enum class OutputFailure : std::uint8_t { Open, Write, Flush };

    struct AppendResult {
        AppendDisposition disposition = AppendDisposition::Complete;
        std::optional<CaptureFailure> failure;
        // Ingress prefix from THIS call, not normalized bytes or prior partial.
        qint64 acceptedBytes = 0;
        // Normalized bytes actually published to capture; may include an earlier
        // call's partial. Excludes the synthetic cache LF on a finalized record.
        qint64 committedBytes = 0;
        bool finalRecordUnterminated = false;
        LinesCount committedLines = 0_lcount;
        qint64 pendingPartialBytes = 0;
        bool outputAttempted = false;
        bool notificationFailed = false; // Observer/cache error AFTER capture commit.
        std::optional<OutputFailure> outputFailure;
        // Actual written prefix (including a resumed separator), not durability.
        // nullopt means output progress is unknown, not zero and not rollback.
        std::optional<qint64> outputBytes = qint64{ 0 };
        PersistenceResult persistence;
        LineNumber firstLine = 0_lnum;
        LinesCount lineCount = 0_lcount; // Alias of committedLines, NOT retained count.
        // Complete confirmed batch, independent of retention. The final record
        // has a synthetic LF for search/cache; finalRecordUnterminated marks it.
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
    void flush(); // Bound output only; does not persist capture segments.
    // Explicit user retry bypasses backoff; bounded work, may return pending.
    // Does NOT finalize partial input: call finishInput and inspect its outcome first.
    PersistenceResult persistCapture( int maxSegments = 32 );
    // Automatic continuation honors backoff. Neither API promises fsync durability.
    PersistenceResult retryPersistence( int maxSegments = 8 );
    PersistenceResult persistenceState() const;
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
    bool adoptPublishedOutputFile( RollingFileManager output, const QString& outputPath,
                                   bool needsSeparator );
    void setLimits( Limits limits );
    QString boundOutputFile() const;
    bool outputRefersToPath( const QString& path ) const;
    std::optional<OutputFailure> outputFailure() const;
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
    bool finalRecordUnterminated() const;
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
    void failNextOutputReplayWriteForTesting();
    void setAfterCaptureFilesRetiredCallbackForTesting(
        std::function<void()> callback );
    bool contendForCapturePathAfterGateForTesting(
        std::function<void()> gateAcquired );
    bool holdCapturePathGateForTesting( std::function<void()> gateAcquired,
                                        std::function<void()> waitForRelease );
    static int setCapturePathGateTimeoutForTesting( int timeoutMs );
    // Operation counts stay outside business Stats and are scoped to one path state.
    struct MaintenanceOperationsForTesting {
        std::uint64_t markerScans = 0;
        std::uint64_t retryGateAttempts = 0;
    };
    MaintenanceOperationsForTesting maintenanceOperationsForTesting() const;
    struct MaintenanceRetryForTesting {
        std::uint64_t requested = 0;
        std::uint64_t completed = 0;
        bool scheduled = false;
    };
    MaintenanceRetryForTesting maintenanceRetryForTesting() const;
    void setBeforeRetryHandoffCallbackForTesting( std::function<void()> callback );
    bool hasCapturePathCoordinationOwnershipForTesting() const;
    QString capturePathActiveMarkerPathForTesting() const;
    QString capturePathIdentity() const;
    void commitLines( AppendResult& appendResult );
    struct ActiveCapturePath {
        std::shared_ptr<CapturePathState> state;
        QByteArray activationToken;
        QSet<QString> inheritedCaptureFiles;
    };
    // Boundedly acquire and activate the capture path state, retrying while a
    // competing cleanup is still tearing down the previous generation. Throws
    // if activation cannot complete within CaptureActivationMaxAttempts so a
    // wedged teardown cannot hang the streaming worker thread.
    ActiveCapturePath activateCapturePathState();
    void ensureCaptureDir( bool startsReplacement = true );
    bool needsNewSegment( qint64 incomingBytes = 0 ) const;
    void ensureSegmentIdsAvailable( const AppendResult& appendResult,
                                    qint64 pendingPartialBytes );
    Segment& ensureActiveSegment( qint64 incomingBytes );
    void rebuildCumulativeLineCounts( bool onlyLast = false );
    void enforceMemoryBudget();
    bool spillSegmentToDisk( Segment& segment );
    void persistBufferedSegments();
    PersistenceResult persistPending( int maxSegments, bool force, bool toBudget );
    qint64 spillNowMs() const;
    bool scanSegment( Segment& segment );
    qint64 takeNextSegmentId();
    QByteArray readSegmentLine( const Segment& segment, int localLine ) const;
    bool writeCaptureToDevice( QIODevice* device );
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
    qint64 pendingPersistenceBytes_ = 0;
    qint64 pendingPersistenceSegments_ = 0;
    size_t firstResidentSegment_ = 0;
    qint64 totalLines_ = 0;
    int maxLineLength_ = 0;
    std::deque<qint64> reservedSegmentIds_;
    QSet<QString> inheritedCaptureFiles_;
    QDateTime lastModified_;
    bool persistBufferedSegmentsOnDestroy_ = true;
    bool preserveTailDuringTrim_ = false;
    bool failNextSegmentWriteForTesting_ = false;
    bool failNextOutputReplayWriteForTesting_ = false;
    mutable std::recursive_mutex mutex_;

    std::optional<OutputFailure> outputFailure_;
    bool outputNeedsSeparator_ = false;
    qint64 outputWrittenBytes_ = 0;
    std::function<qint64( const QByteArray& )> outputWriteForTesting_;
    qint64 unflushedOutputBytes_ = 0;
    int unflushedOutputLines_ = 0;
    mutable std::function<void()> beforeRawSnapshotCopyCallbackForTesting_;
    mutable std::function<void()> beforeSpilledSegmentReadCallbackForTesting_;
    std::function<void()> afterCaptureFilesRetiredCallbackForTesting_;
    std::function<void()> beforeSegmentMutationForTesting_;
    std::function<void()> afterSpillPublishForTesting_;
    std::function<void()> beforeAppendMaintenanceForTesting_;
    qint64 segmentByteLimitForTesting_ = std::numeric_limits<int>::max();
    std::function<qint64()> spillClockForTesting_;
    std::function<std::optional<PersistenceFailure>()> spillFailureForTesting_;
    std::uint64_t spillAttemptsForTesting_ = 0;
    mutable std::uint64_t persistenceVisitsForTesting_ = 0;
    mutable std::uint64_t maxLineLengthLineVisitsForTesting_ = 0;
    std::uint64_t commitIndexReservationsForTesting_ = 0;
    std::uint64_t trimChecksForTesting_ = 0;
    std::uint64_t ingressBoundaryCopiesForTesting_ = 0;
    std::optional<PersistenceFailure> persistenceFailure_;
    TrimResult lastTrimResult_;

    // Spill throttling: avoid frequent small spills
    static constexpr int SpillThrottleMs = 5000;
    std::optional<qint64> nextSpillRetryMs_;
};

#endif
