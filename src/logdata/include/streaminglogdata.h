#ifndef STREAMINGLOGDATA_H
#define STREAMINGLOGDATA_H

#include <memory>

#include <QRegularExpression>
#include <QTimer>

#include "capturestore.h"
#include "searchablelogdata.h"

class StreamingLogData : public SearchableLogData {
    Q_OBJECT

  public:
    explicit StreamingLogData( QString captureId, QString captureRoot = {} );
    ~StreamingLogData() override;

    void appendUtf8( const QByteArray& data );
    void finishInput();
    void clearCapture();
    bool bindOutputFile( const QString& outputPath );
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
    void scheduleLoadingFinished();
    void startOutputFlushTimer();
    void stopOutputFlushTimer();
    klogg::vector<QString> getLines( LineNumber first, LinesCount number ) const;

  private:
    CaptureStore captureStore_;
    TextCodecHolder codec_;
    QRegularExpression prefilterPattern_;
    AnsiProcessingMode ansiProcessingMode_ = AnsiProcessingMode::Plain;
    bool loadingFinishedQueued_ = false;
    QTimer outputFlushTimer_;
};

#endif
