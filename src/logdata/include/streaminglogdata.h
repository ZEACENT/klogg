#ifndef STREAMINGLOGDATA_H
#define STREAMINGLOGDATA_H

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <QFile>
#include <QRegularExpression>
#include <QTimer>

#include "capturestore.h"
#include "rollingfilemanager.h"
#include "searchablelogdata.h"

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

    void appendUtf8( const QByteArray& data );
    void finishInput();
    void clearCapture();
    void setCaptureLimits( CaptureStore::Limits limits );
    bool bindOutputFile( const QString& outputPath );
    bool bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode );
    bool bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode, OutputBindMode mode );
    QString boundOutputFile() const;
    std::optional<CaptureOutputError> captureOutputError() const;
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
    OutputBindResult writeDisplayLinesToDevice( LineNumber first, LinesCount count,
                                                QIODevice* output );
    OutputBindResult writeDisplayLinesToOutput( LineNumber first, LinesCount count,
                                                bool reportFailures = true,
                                                bool allowRotation = true );
    bool isOutputFileActive() const;
    bool outputRefersToPath( const QString& path ) const;
    void reportCaptureOutputHealthy();
    void reportCaptureOutputFailure( CaptureOutputError error );
    void checkPreservedOutputState();
    // Writes the lines appended in `appendResult` to the Strip-mode display
    // file.  Addresses the appended lines by their current tail position so it
    // is correct even when trimming has shifted line numbers — never by a
    // [previous, current) delta (which underflows when trimming removes more
    // lines than were added).
    void writeAppendedDisplayLines( const CaptureStore::AppendResult& appendResult );
    klogg::vector<QString> getLines( LineNumber first, LinesCount number ) const;
    void rememberAppendedRawLines( const CaptureStore::AppendResult& appendResult );
    std::optional<RawLines> tryBuildCachedRawLines( LineNumber first, LinesCount number ) const;

  private:
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
    LiveLogSaveAnsiMode outputSaveAnsiMode_ = LiveLogSaveAnsiMode::Strip;
    std::optional<CaptureOutputError> captureOutputError_;
    mutable std::mutex cachedRawBatchesMutex_;
    std::deque<CachedRawBatch> cachedRawBatches_;
    qint64 cachedRawBytes_ = 0;
    mutable std::mutex ansiDisplayCacheMutex_;
    mutable std::deque<LineNumber::UnderlyingType> ansiDisplayCacheOrder_;
    mutable std::unordered_map<LineNumber::UnderlyingType, ProcessedAnsiLine> ansiDisplayCache_;
};

#endif
