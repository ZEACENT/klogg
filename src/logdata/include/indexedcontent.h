#ifndef INDEXEDCONTENT_H
#define INDEXEDCONTENT_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include "containers.h"
#include "linetypes.h"

class IndexedContentError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IndexedContentStorage;
class IndexedContentBuilder;

// A version of the bytes actually consumed by the indexer, not an atomic view
// of a file being concurrently rewritten. The lease never reopens that file.
class IndexedContentSnapshot {
public:
    using Ptr = std::shared_ptr<const IndexedContentSnapshot>;

    uint64_t version() const { return version_; }
    uint64_t incarnation() const { return incarnation_; }
    qint64 byteSize() const { return byteSize_; }
    LinesCount lineCount() const;
    OffsetInFile lineEnd( LineNumber line ) const;
    klogg::vector<OffsetInFile> lineEnds( LineNumber first, LinesCount count ) const;
    const QByteArray& codecName() const { return codecName_; }
    QByteArray readBytes( qint64 offset, qint64 length ) const;
    size_t residentBytes() const;
    // Explicitly materializes anonymous temporary files for named-file lifetime
    // tests. Normal snapshot reads never request or depend on spill filenames.
    QStringList spillFileNamesForTesting() const;

private:
    friend class IndexedContentBuilder;
    IndexedContentSnapshot() = default;
    std::shared_ptr<IndexedContentStorage> bytes_;
    std::shared_ptr<IndexedContentStorage> boundaries_;
    uint64_t version_{};
    uint64_t incarnation_{};
    qint64 byteSize_{};
    LinesCount completeLines_{};
    bool fakeFinalLF_{};
    QByteArray codecName_;
};

// One indexer writes; published versions may be read concurrently. Both byte
// content and boundary metadata spill to privately owned temporary files.
class IndexedContentBuilder {
public:
    struct Options {
        size_t byteMemoryBudget = 8 * 1024 * 1024;
        size_t boundaryMemoryBudget = 1024 * 1024;
        QString temporaryDirectory;
    };

    IndexedContentBuilder();
    explicit IndexedContentBuilder( const Options& options );
    explicit IndexedContentBuilder( const IndexedContentSnapshot::Ptr& previous );

    void appendBlock( qint64 offset, const char* bytes, size_t size,
                      const klogg::vector<OffsetInFile>& completeLineEnds );
    IndexedContentSnapshot::Ptr snapshot( const QByteArray& codecName,
                                          bool fakeFinalLF = false ) const;

private:
    std::shared_ptr<IndexedContentStorage> bytes_;
    std::shared_ptr<IndexedContentStorage> boundaries_;
    qint64 byteSize_{};
    LinesCount completeLines_{};
    qint64 lastCompleteEnd_{};
};

#endif
