#ifndef KLOGG_FOLDERSCANARENA_H
#define KLOGG_FOLDERSCANARENA_H

#include "encodingdetector.h"
#include "indexedcontent.h"
#include "logsnapshot.h"

#include <QStringList>

#include <atomic>
#include <mutex>
#include <vector>

namespace klogg::folder {

// One scan-wide pair of packed stores: 8 MiB payload plus 1 MiB row boundaries
// by default, and at most two spill handles. Never one store per folder member.
// Span metadata costs three qint64 values per noncontiguous scanner/index batch,
// not per source line; consecutive writes from the same member merge.
struct ScanSpan {
    qint64 memberOffset = 0;
    qint64 arenaOffset = 0;
    qint64 size = 0;
};

class ScanArena;

class ScannedFileContent {
  public:
    using Ptr = std::shared_ptr<const ScannedFileContent>;

    LogRowId identity( LineNumber line ) const;
    QByteArray readBytes( qint64 offset, qint64 length ) const;
    // Framing always belongs to the scanned source. The supplied codec changes
    // interpretation of record content, never its retained byte boundaries.
    QString readRecord( qint64 offset, qint64 length, const QByteArray& displayCodec ) const;
    QString readLine( LineNumber line, const QByteArray& displayCodec ) const;
    const QByteArray& codecName() const { return codecName_; }
    qint64 byteSize() const { return byteSize_; }
    LinesCount lineCount() const { return lineCount_; }
    size_t residentBytes() const { return bytes_->residentBytes() + rowEnds_->residentBytes(); }
    uint64_t bytesReadForTesting() const { return bytesRead_.load( std::memory_order_relaxed ); }
    // Explicit test observation may materialize otherwise anonymous spill names.
    QStringList spillFileNamesForTesting() const;

  private:
    friend class ScanArena;
    IndexedContentSnapshot::Ptr bytes_;
    IndexedContentSnapshot::Ptr rowEnds_;
    std::vector<ScanSpan> spans_;
    std::vector<ScanSpan> rowSpans_;
    QByteArray codecName_;
    EncodingParameters framing_;
    qint64 byteSize_ = 0;
    LinesCount lineCount_ = 0_lcount;
    uint64_t member_ = 0;
    mutable std::atomic<uint64_t> bytesRead_{ 0 };
};

class ScanArena {
  public:
    static constexpr size_t BoundaryBatchLines = 8192;

    explicit ScanArena( const IndexedContentBuilder::Options& options = {} );
    uint64_t nextMember() { return nextMember_.fetch_add( 1, std::memory_order_relaxed ); }
    void append( std::vector<ScanSpan>& spans, qint64 memberOffset, const QByteArray& bytes );
    void appendLineEnds( std::vector<ScanSpan>& spans, uint64_t firstLine,
                         const qint64* ends, size_t count );
    ScannedFileContent::Ptr seal( std::vector<ScanSpan> spans, qint64 byteSize,
                                 uint64_t member, const QByteArray& codecName,
                                 LinesCount lineCount, std::vector<ScanSpan> rowSpans );

  private:
    std::mutex mutex_;
    IndexedContentBuilder builder_;
    IndexedContentBuilder rowBuilder_;
    qint64 byteSize_ = 0;
    qint64 rowByteSize_ = 0;
    std::atomic<uint64_t> nextMember_{ 1 };
};

} // namespace klogg::folder

#endif
