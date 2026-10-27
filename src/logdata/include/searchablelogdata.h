#ifndef SEARCHABLELOGDATA_H
#define SEARCHABLELOGDATA_H

#include <cstdint>
#include <memory>
#include <optional>
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
        enum class TrailingCrPolicy : std::uint8_t { StripFramingCr, Preserve };
        // Indexed files still include framing CR; capture payloads have already
        // removed it at ingestion and may end in a literal data CR.
        TrailingCrPolicy trailingCrPolicy = TrailingCrPolicy::StripFramingCr;
        LineNumber startLine;

        klogg::vector<char> buffer;
        klogg::vector<qint64> endOfLines;

        TextDecoder textDecoder;

        QRegularExpression prefilterPattern;
        AnsiProcessingMode ansiProcessingMode = AnsiProcessingMode::Plain;

      public:
        klogg::vector<QString> decodeLines() const;
        klogg::vector<QString> decodeLinesStrict() const;
        klogg::vector<klogg::vector<AnsiColorSpan>> decodeLineAnsiColors() const;
        // Both matching backends consume these exact normalized records. The
        // block is LF-framed and boundaries are one-past-LF offsets; views omit
        // separators. Storage is borrowed from this RawLines until its next build.
        struct Utf8Block {
            std::string_view buffer;
            klogg::vector<qint64> endOfLines;
            klogg::vector<std::string_view> lines;
        };
        // Shared eligibility for already-normalized LF-framed bytes (or one
        // unterminated record). ANSI/prefilter eligibility remains caller-owned.
        static bool canBorrowNormalizedUtf8( std::string_view bytes,
                                              const EncodingParameters& params,
                                              TrailingCrPolicy policy = TrailingCrPolicy::StripFramingCr );
        Utf8Block buildUtf8Block() const;
        klogg::vector<std::string_view> buildUtf8View() const;

      private:
        mutable klogg::vector<char> utf8Data_;
    };

    class ReadSnapshot : public klogg::LogSnapshot {
      public:
        using Ptr = std::shared_ptr<const ReadSnapshot>;
        struct CacheKey {
            klogg::LogRowId::Origin origin;
            uint64_t incarnation;
            uint64_t version;
            QByteArray codec;
            QRegularExpression prefilter;
            AnsiProcessingMode ansiMode;

            bool operator==( const CacheKey& other ) const
            {
                return origin == other.origin && incarnation == other.incarnation
                       && version == other.version && codec == other.codec
                       && prefilter == other.prefilter && ansiMode == other.ansiMode;
            }
            bool operator!=( const CacheKey& other ) const { return !( *this == other ); }
        };
        // An absent key permits correct uncached execution, never speculative reuse.
        virtual std::optional<CacheKey> cacheKey() const { return std::nullopt; }
        // True only when bytes/rows preceding the old final row are unchanged,
        // normalization agrees, and the old ordered rows remain a prefix.
        virtual bool isAppendOf( const ReadSnapshot& ) const { return false; }
        bool preservesPresentationOf( const klogg::LogSnapshot& previous ) const override;
        virtual RawLines getLinesRaw( LineNumber first, LinesCount count ) const = 0;
        virtual qint64 byteSize() const = 0;
        QString lineString( LineNumber index ) const override;
    };

    virtual ReadSnapshot::Ptr searchableSnapshot() const { return {}; }
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
