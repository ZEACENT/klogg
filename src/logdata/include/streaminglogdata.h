#ifndef STREAMINGLOGDATA_H
#define STREAMINGLOGDATA_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QFile>
#include <QRegularExpression>
#include <QTimer>

#include "capturestore.h"
#include "platform/platform_files.h"
#include "rollingfilemanager.h"
#include "searchablelogdata.h"

class QSaveFile;

enum class LiveLogSaveAnsiMode {
    Strip,
    Preserve,
};

enum class CaptureOutputError : std::uint8_t {
    Open,
    Write,
    Flush,
    Reopen,
};

// How bindOutputFile treats an existing destination file.
enum class OutputBindMode {
    // User-initiated "Save Live Log As": truncate the destination and rewrite
    // it from the current capture. The Save dialog has already confirmed any
    // overwrite, so destroying prior content is the user's explicit intent.
    FreshSave,
    // Session restore: the destination already holds previously streamed
    // content. Preserve it and only append data that arrives after the
    // restore. The capture store is volatile (it lives in the OS temp dir),
    // so rewriting from it on restart can silently empty the file when the
    // temp dir has been cleared (computer restart, logout, crash, cleanup).
    Restore,
};

class StreamingLogData : public SearchableLogData {
    Q_OBJECT

  public:
    explicit StreamingLogData( QString captureId, QString captureRoot = {} );
    ~StreamingLogData() override;

    enum class OutputExportFailure : std::uint8_t {
        Busy,
        Cancelled,
        TailOverflow,
        PartialUnknown,
        SnapshotRead,
        Write,
        Publish,
        PublishedReopen,
        PublishedCutover,
    };

    struct OutputExportBatch {
        std::uint64_t sequence = 0;
        QByteArray rawUtf8Lines;
        klogg::vector<qint64> endOfLines;
        bool finalRecordUnterminated = false;
    };

    struct OutputExportCandidate {
        std::uint64_t id = 0;
        std::uint64_t firstTailSequence = 0;
        CaptureStore::Snapshot snapshot;
        bool snapshotFinalRecordUnterminated = false;
        LiveLogSaveAnsiMode ansiMode = LiveLogSaveAnsiMode::Strip;
        QByteArray codecName;
        QString prefilterPattern;
    };

    struct OutputExportTail {
        std::vector<OutputExportBatch> batches;
        std::optional<OutputExportFailure> failure;
    };

    struct OutputExportEncodingState {
        QByteArray partialRecord;
        std::optional<std::uint64_t> nextTailSequence;
        bool needsSeparator = false;
    };

    struct OutputExportActivation {
        bool success = false;
        std::optional<OutputExportFailure> failure;
    };

    using OutputExportWrite = std::function<qint64( const QByteArray& )>;
    using OutputExportCancelled = std::function<bool()>;

    // Registers the cutover journal before fixing the immutable snapshot boundary.
    // Only one candidate may be pending for a capture.
    std::optional<OutputExportCandidate>
    beginOutputExport( LiveLogSaveAnsiMode ansiMode, qint64 maximumTailBytes );
    OutputExportTail takeOutputExportTail( std::uint64_t candidateId );
    void cancelOutputExport( std::uint64_t candidateId );
    // Runs on the data owner thread. Final-tail validation, atomic publication and
    // output cutover share the append-ordering critical section.
    OutputExportActivation publishStagedOutputExport(
        std::uint64_t candidateId, const QString& outputPath,
        OutputExportEncodingState encodingState, QSaveFile& stagedOutput,
        const std::function<void()>& afterPublish = {} );
    static bool writeOutputExportSnapshot( const OutputExportCandidate& candidate,
                                           OutputExportEncodingState& state,
                                           const OutputExportWrite& write,
                                           const OutputExportCancelled& cancelled = {} );
    static bool writeOutputExportBatches( const OutputExportCandidate& candidate,
                                          const std::vector<OutputExportBatch>& batches,
                                          OutputExportEncodingState& state,
                                          const OutputExportWrite& write );
    bool hasPendingOutputExport() const;

    CaptureStore::AppendResult appendUtf8( const QByteArray& data );
    CaptureStore::AppendResult finishInput();
    CaptureStore::PersistenceResult persistCapture( int maxSegments = 32 );
    CaptureStore::PersistenceResult retryPersistence( int maxSegments = 8 );
    CaptureStore::PersistenceResult persistenceState() const;
    CaptureStore::Snapshot captureSnapshot() const;
    void clearCapture();
    void setCaptureLimits( CaptureStore::Limits limits );
    bool bindOutputFile( const QString& outputPath );
    bool bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode );
    bool bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode, OutputBindMode mode );
    QString boundOutputFile() const;
    bool hasActiveOutputBinding( const QString& outputPath,
                                 LiveLogSaveAnsiMode ansiMode ) const;
    std::optional<CaptureOutputError> captureOutputError() const;
    std::optional<CaptureOutputError> flushOutputForClose();
    QString captureId() const;
    QString capturePath() const;
    void deleteCaptureFiles();

    void interruptLoading() override;
    std::unique_ptr<LogFilteredData> getNewFilteredData() const override;
    qint64 getFileSize() const override;
    QDateTime getLastModifiedDate() const override;
    void reload( QTextCodec* forcedEncoding = nullptr ) override;
    QTextCodec* getDetectedEncoding() const override;
    void setPrefilter( const QString& prefilterPattern ) override;
    void setAnsiProcessingMode( AnsiProcessingMode mode ) override;
    RawLines getLinesRaw( LineNumber first, LinesCount number ) const override;
    bool isLiveSource() const override;

  Q_SIGNALS:
      void captureOutputChanged( bool healthy, CaptureOutputError error );
      // Current spool health, not a claim that pending bytes are durable.
      void capturePersistenceChanged( bool healthy, CaptureStore::PersistenceFailure error );

  protected:
    QString doGetLineString( LineNumber line ) const override;
    QString doGetExpandedLineString( LineNumber line ) const override;
    klogg::vector<AnsiColorSpan> doGetLineAnsiColors( LineNumber line ) const override;
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount number ) const override;
    klogg::vector<QString> doGetExpandedLines( LineNumber first, LinesCount number ) const override;
    LineNumber doGetLineNumber( LineNumber index ) const override;
    LinesCount doGetNbLine() const override;
    LineLength doGetMaxLength() const override;
    LineLength doGetLineLength( LineNumber line ) const override;
    void doSetDisplayEncoding( const char* encoding ) override;
    QTextCodec* doGetDisplayEncoding() const override;
    void doAttachReader() const override;
    void doDetachReader() const override;

  private:
    // Tests deliver the existing single-shot timer, never a synthetic completion signal.
    friend struct StreamingLogDataTimerTestAccess;
    friend struct LiveSourceStreamingLogDataTestAccess;

      struct OutputBindResult {
          bool success = false;
          CaptureOutputError error = CaptureOutputError::Open;
      };

    struct CachedRawBatch {
        LineNumber firstLine = 0_lnum;
        LinesCount lineCount = 0_lcount;
        QByteArray rawUtf8Lines;
        klogg::vector<qint64> endOfLines;
    };

    struct SuspendedOutputBinding {
        LiveLogSaveAnsiMode ansiMode = LiveLogSaveAnsiMode::Strip;
        klogg::platform::FileIdentity identity;
    };

    struct PendingOutputExport {
        std::uint64_t id = 0;
        std::uint64_t firstTailSequence = 0;
        qint64 maximumTailBytes = 0;
        qint64 tailBytes = 0;
        bool snapshotFinalRecordUnterminated = false;
        LiveLogSaveAnsiMode ansiMode = LiveLogSaveAnsiMode::Strip;
        QByteArray codecName;
        QString prefilterPattern;
        std::deque<OutputExportBatch> tail;
        std::optional<OutputExportFailure> failure;
    };

    void scheduleLoadingFinished( int delayMs = 0 );
    ProcessedAnsiLine processedAnsiLine( LineNumber line ) const;
    void clearAnsiDisplayCache();
    // Reads CaptureStore's pending trim result; if nonzero, clears it and
    // invalidates the line-keyed raw/ANSI caches (their absolute line numbers
    // shifted). Returns the consumed result so the caller can emit Truncated.
    CaptureStore::TrimResult consumeTrimResult();
    void startOutputFlushTimer();
    void stopOutputFlushTimer();
    OutputBindResult openDisplayOutputFile( const QString& outputPath,
                                            bool preserveExisting = false );
    void closeDisplayOutputFile( bool clearBinding = true );
    static CaptureOutputError
    captureStoreOutputError( std::optional<CaptureStore::OutputFailure> failure );
    OutputBindResult writeDisplayLinesToDevice( QIODevice* output );
    QByteArray displayOutputRecord( const QByteArray& bytes, bool terminated ) const;
    bool isOutputFileActive() const;
    bool outputRefersToPath( const QString& path ) const;
    bool suspendOutputForReplacement(
        const QString& outputPath,
        std::optional<SuspendedOutputBinding>& suspended );
    bool restoreOutputAfterFailedReplacement(
        const std::optional<SuspendedOutputBinding>& suspended );
    void abandonOutputAfterFailedReplacement(
        const std::optional<SuspendedOutputBinding>& suspended,
        CaptureStore::OutputFailure failure );
    void reportCaptureOutputHealthy();
    void reportCaptureOutputFailure( CaptureOutputError error );
    void checkPreservedOutputState();
    void reportPersistenceState( const CaptureStore::PersistenceResult& state );
    void journalOutputExport( const CaptureStore::AppendResult& appendResult ) noexcept;
    static QByteArray transformOutputRecord( const OutputExportCandidate& candidate,
                                             const QByteArray& bytes, bool terminated );
    static bool writeAllOutputBytes( const QByteArray& bytes, const OutputExportWrite& write );
    // Transform only the confirmed accepted normalized batch, never the retained tail.
    void writeAppendedDisplayLines( CaptureStore::AppendResult& appendResult );
    klogg::vector<QString> getLines( LineNumber first, LinesCount number ) const;
    static qint64 cachedRawBatchMetadataBytes( const CachedRawBatch& batch );
    void rememberAppendedRawLines( const CaptureStore::AppendResult& appendResult );
    std::optional<RawLines> tryBuildCachedRawLines( LineNumber first, LinesCount number ) const;

  private:
    // Serializes capture mutation, candidate registration and final cutover.
    mutable std::recursive_mutex appendOrderingMutex_;
    CaptureStore captureStore_;
    TextCodecHolder codec_;
    QRegularExpression prefilterPattern_;
    AnsiProcessingMode ansiProcessingMode_ = AnsiProcessingMode::Plain;
    bool loadingFinishedQueued_ = false;
    QTimer loadingFinishedTimer_;
    QTimer outputFlushTimer_;
    QString boundOutputFile_;
    // getFileSize() reads the bound path from search worker threads while the
    // main thread rebinds it; guard the QString (implicitly shared, not
    // thread-safe against concurrent writes).
    mutable std::mutex boundOutputFileMutex_;
    RollingFileManager rollingDisplayOutput_;
    qint64 rollingMaxFileSize_ = 0;
    int rollingBackupCount_ = 0;
    bool displayOutputNeedsSeparator_ = false;
    std::function<qint64( const QByteArray& )> outputWriteForTesting_;
    qint64 replayPeakBufferForTesting_ = 0;
    LiveLogSaveAnsiMode outputSaveAnsiMode_ = LiveLogSaveAnsiMode::Strip;
    std::optional<CaptureOutputError> captureOutputError_;
    std::optional<CaptureStore::PersistenceFailure> persistenceFailure_;
    std::optional<PendingOutputExport> pendingOutputExport_;
    std::function<void()> beforeOutputExportJournalForTesting_;
    std::uint64_t nextOutputExportId_ = 0;
    std::uint64_t nextOutputDeliverySequence_ = 0;
    static constexpr std::size_t CachedRawBatchCountLimit = 65536u;
    static constexpr qint64 CachedRawBatchMetadataBytesLimit = 32LL * 1024 * 1024;
    static constexpr qint64 CachedRawBatchTargetBytes = 64LL * 1024;
    static constexpr LinesCount::UnderlyingType CachedRawBatchLineLimit = 4096;

    mutable std::mutex cachedRawBatchesMutex_;
    // Queries retain immutable shared batches after releasing this lock. New
    // appends may coalesce only into a uniquely-owned tail batch.
    std::deque<std::shared_ptr<CachedRawBatch>> cachedRawBatches_;
    qint64 cachedRawBytes_ = 0;
    qint64 cachedRawMetadataBytes_ = 0;
    std::size_t cachedRawBatchCountLimit_ = CachedRawBatchCountLimit;
    qint64 cachedRawMetadataBytesLimit_ = CachedRawBatchMetadataBytesLimit;
    mutable std::uint64_t cachedRawLookupBatchVisitsForTesting_ = 0;
    mutable std::mutex ansiDisplayCacheMutex_;
    mutable std::deque<LineNumber::UnderlyingType> ansiDisplayCacheOrder_;
    mutable std::unordered_map<LineNumber::UnderlyingType, ProcessedAnsiLine> ansiDisplayCache_;
};

#endif
