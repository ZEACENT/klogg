#ifndef SEARCHABLELOGDATA_H
#define SEARCHABLELOGDATA_H

#include <memory>
#include <string_view>

#include <QDateTime>
#include <QObject>
#include <QRegularExpression>
#include <QTextCodec>

#include "abstractlogdata.h"
#include "ansicolorprocessor.h"
#include "encodingdetector.h"
#include "loadingstatus.h"

class LogFilteredData;

// SearchableLogData is the common interface for data sources that can back the
// main log view and the search worker. File-backed and live-stream-backed
// sources both implement this API.
class SearchableLogData : public AbstractLogData {
    Q_OBJECT

  public:
    struct RawLines {
        LineNumber startLine;

        klogg::vector<char> buffer;
        klogg::vector<qint64> endOfLines;

        TextDecoder textDecoder;

        QRegularExpression prefilterPattern;
        AnsiProcessingMode ansiProcessingMode = AnsiProcessingMode::Plain;

      public:
        klogg::vector<QString> decodeLines() const;
        klogg::vector<klogg::vector<AnsiColorSpan>> decodeLineAnsiColors() const;
        klogg::vector<std::string_view> buildUtf8View() const;

      private:
        mutable klogg::vector<char> utf8Data_;
    };

    virtual ~SearchableLogData() = default;

    virtual void interruptLoading() = 0;
    virtual std::unique_ptr<LogFilteredData> getNewFilteredData() const = 0;
    virtual qint64 getFileSize() const = 0;
    virtual QDateTime getLastModifiedDate() const = 0;
    virtual void reload( QTextCodec* forcedEncoding = nullptr ) = 0;
    virtual QTextCodec* getDetectedEncoding() const = 0;
    virtual void setPrefilter( const QString& prefilterPattern ) = 0;
    virtual void setAnsiProcessingMode( AnsiProcessingMode mode ) = 0;
    virtual RawLines getLinesRaw( LineNumber first, LinesCount number ) const = 0;

    virtual bool isLiveSource() const
    {
        return false;
    }

  Q_SIGNALS:
    void loadingProgressed( int percent );
    void loadingFinished( LoadingStatus status );
    void fileChanged( MonitoredFileStatus status );
};

#endif
