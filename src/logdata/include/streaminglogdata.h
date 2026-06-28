#ifndef STREAMINGLOGDATA_H
#define STREAMINGLOGDATA_H

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
    QString boundOutputFile() const;
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
    bool openDisplayOutputFile( const QString& outputPath );
    void closeDisplayOutputFile();
    bool writeDisplayLinesToOutput( LineNumber first, LinesCount count );
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
    RollingFileManager rollingDisplayOutput_;
    qint64 rollingMaxFileSize_ = 0;
    int rollingBackupCount_ = 0;
    LiveLogSaveAnsiMode outputSaveAnsiMode_ = LiveLogSaveAnsiMode::Strip;
    mutable std::mutex cachedRawBatchesMutex_;
    std::deque<CachedRawBatch> cachedRawBatches_;
    qint64 cachedRawBytes_ = 0;
    mutable std::mutex ansiDisplayCacheMutex_;
    mutable std::deque<LineNumber::UnderlyingType> ansiDisplayCacheOrder_;
    mutable std::unordered_map<LineNumber::UnderlyingType, ProcessedAnsiLine> ansiDisplayCache_;
};

#endif
