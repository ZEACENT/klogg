#ifndef KLOGG_PREPAREDFILESOURCE_H
#define KLOGG_PREPAREDFILESOURCE_H

#include <cstdint>
#include <memory>
#include <optional>

#include <QFile>
#include <QString>

#include "atomicflag.h"
#include "indexedcontent.h"
#include "indexingsettings.h"
#include "platform/platform_files.h"

class IndexingData;
class LogData;
class QTextCodec;
struct FileSourcePreparation;

// A complete private index and its original handle. Ready values have no QObject
// thread affinity, so discarding a worker result needs no GUI event-loop turn.
class PreparedFileSource {
public:
    ~PreparedFileSource();
    PreparedFileSource( const PreparedFileSource& ) = delete;
    PreparedFileSource& operator=( const PreparedFileSource& ) = delete;

    IndexedContentSnapshot::Ptr content() const;
    const klogg::platform::AcquiredFileMetadata& metadata() const { return metadata_; }
    const QString& path() const { return path_; }

private:
    PreparedFileSource();
    QString path_;
    std::unique_ptr<QFile> file_;
    std::shared_ptr<IndexingData> index_;
    klogg::platform::AcquiredFileMetadata metadata_;

    friend class LogData;
    friend FileSourcePreparation prepareFileSource(
        const QString&, const IndexingSettings&, AtomicFlag&, QTextCodec*,
        std::optional<LineLength> );
};

struct FileSourcePreparation {
    enum class Status : std::uint8_t {
        Ready, Cancelled, InvalidInput, Missing, NotRegular, OpenFailure,
        ReadFailure, ResourceLimit
    };
    Status status = Status::InvalidInput;
    QString error;
    std::unique_ptr<PreparedFileSource> source;
};

// Worker-only. Capture settings and an absolute logical path on the owner thread.
// Successful return means indexing has fully quiesced, not just emitted progress.
FileSourcePreparation prepareFileSource(
    const QString& path, const IndexingSettings& settings, AtomicFlag& cancelled,
    QTextCodec* forcedEncoding = nullptr,
    std::optional<LineLength> maximumLineLength = std::nullopt );

#endif
