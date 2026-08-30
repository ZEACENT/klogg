/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "foldersearchresults.h"

#include <QFile>
#include <QTextCodec>

#include <algorithm>

#include "containers.h"

namespace {

// Composite cache key: filePath + null separator + codec name. The codec for a
// marks-only file (nullptr / "UTF-8" default) differs from the scanned codec of
// a later match group (e.g. "Shift-JIS"). Without the codec suffix, a cached
// UTF-8 decode for a marks-only file is reused for a match group that has a
// different actual encoding — producing mojibake for the marked lines.
QString markCacheKey( const QString& filePath, QTextCodec* codec )
{
    return filePath + QChar::Null
           + QString::fromLatin1( codec != nullptr ? codec->name() : QByteArrayLiteral( "UTF-8" ) );
}

} // namespace

#include "linetypes.h"
#include "log.h"

FolderSearchResults::FolderSearchResults()
    : AbstractLogData()
{
}

FolderSearchResults::~FolderSearchResults() = default;

void FolderSearchResults::setResults( std::vector<klogg::folder::FileGroup> groups )
{
    {
        UniqueLock lock( dataMutex_ );
        // Defensive: drop groups with no matches (the engine/caller already filters
        // them, but FolderSearchResults must never emit an empty group's header).
        groups.erase( std::remove_if( groups.begin(), groups.end(),
                                      []( const klogg::folder::FileGroup& group ) {
                                          return group.matches.empty();
                                      } ),
                      groups.end() );
        groups_ = std::move( groups );

        // setResults has no full folder-membership input, so retain its legacy
        // unrestricted marks-only behavior. Folder streaming searches use
        // beginSearch(expectedFileOrder) for membership-aware filtering.
        expectedFilePaths_.clear();
        restrictMarksToExpectedFiles_ = false;
        resetFileCaches();

        // NOTE: setResults resets collapse (it is a full result-set replacement, not
        // the live re-search path -- startSearch uses beginSearch, which preserves
        // collapse via snapshotCollapsedPaths/reapplyCollapseForLastGroup). Callers
        // that need preservation here should snapshot filePaths the same way.
        collapsed_.clear();
        rebuildVisibleRows();
    }
    // Emitted after the lock is released: receivers re-enter the getters.
    Q_EMIT layoutChanged();
}

void FolderSearchResults::snapshotCollapsedPaths()
{
    // Only (re)snapshot while groups_ still holds the current result set. A
    // follow-up beginSearch can fire before the previous one's scan has streamed
    // any groups back (e.g. the context combobox and spinbox each trigger a
    // re-scan); at that point groups_ is empty and re-snapshotting would clobber
    // the snapshot just captured. Keep the existing pendingCollapsePaths_ so it
    // survives until the groups actually stream back in.
    if ( groups_.empty() ) {
        return;
    }
    pendingCollapsePaths_.clear();
    for ( const auto fid : collapsed_ ) {
        if ( fid >= 0 && fid < static_cast<int>( groups_.size() ) ) {
            pendingCollapsePaths_.insert( groups_[ static_cast<size_t>( fid ) ].filePath );
        }
    }
}

void FolderSearchResults::reapplyCollapseForLastGroup()
{
    if ( groups_.empty() ) {
        return;
    }
    const auto& lastPath = groups_.back().filePath;
    const auto fid = static_cast<int>( groups_.size() ) - 1;
    if ( pendingCollapsePaths_.contains( lastPath ) ) {
        collapsed_.insert( fid );
    }
    else {
        // The group just added was NOT in the collapse snapshot. Remove any
        // stale collapsed_ entry a marks-only group may have set at this FileId
        // before the real group arrived (marks-group FileIds shift when later
        // match groups stream in). Without the removal the stale entry leaks
        // to the real group now occupying this index.
        collapsed_.remove( fid );
    }
}

void FolderSearchResults::beginSearch( const QStringList& expectedFileOrder )
{
    {
        UniqueLock lock( dataMutex_ );
        // Preserve collapse across this re-scan: snapshot which filePaths are collapsed
        // BEFORE groups_ is rebuilt and FileIds are reassigned, then re-apply as groups
        // stream back in (addFileGroup/flushPending). Without this, a context-line
        // change (which re-scans via beginSearch) would expand every collapsed group.
        snapshotCollapsedPaths();
        groups_.clear();
        expectedFilePaths_.clear();
        for ( const auto& filePath : expectedFileOrder ) {
            expectedFilePaths_.insert( filePath );
        }
        restrictMarksToExpectedFiles_ = true;
        resetFileCaches();
        collapsed_.clear();
        pendingByIndex_.assign( static_cast<size_t>( expectedFileOrder.size() ),
                                std::nullopt );
        nextExpectedIndex_ = 0;
        rebuildVisibleRows();
    }
    Q_EMIT layoutChanged();
}

void FolderSearchResults::addFileGroup( int fileIndex, klogg::folder::FileGroup group )
{
    bool appended = false;
    {
        UniqueLock lock( dataMutex_ );
        if ( fileIndex < 0 || static_cast<size_t>( fileIndex ) >= pendingByIndex_.size() ) {
            return;
        }
        pendingByIndex_[ static_cast<size_t>( fileIndex ) ] = std::move( group );

        // Drain every consecutive completed predecessor from the cursor. This makes
        // the display order always match enumeration order, no matter which file
        // finished first: file[5] cannot appear until file[0..4] are all committed.
        while ( nextExpectedIndex_ < pendingByIndex_.size()
                && pendingByIndex_[ nextExpectedIndex_ ].has_value() ) {
            auto& opt = pendingByIndex_[ nextExpectedIndex_ ];
            if ( !opt->matches.empty() ) {
                groups_.push_back( std::move( *opt ) );
                reapplyCollapseForLastGroup();
                appended = true;
            }
            ++nextExpectedIndex_;
        }
        if ( appended ) {
            rebuildVisibleRows();
        }
    }
    if ( appended ) {
        Q_EMIT layoutChanged();
    }
}

void FolderSearchResults::flushPending()
{
    bool appended = false;
    {
        UniqueLock lock( dataMutex_ );
        // Commit every remaining present group, skipping missing/empty slots (an
        // interrupted scan may leave a predecessor that will never arrive).
        while ( nextExpectedIndex_ < pendingByIndex_.size() ) {
            auto& opt = pendingByIndex_[ nextExpectedIndex_ ];
            if ( opt.has_value() && !opt->matches.empty() ) {
                groups_.push_back( std::move( *opt ) );
                reapplyCollapseForLastGroup();
                appended = true;
            }
            ++nextExpectedIndex_;
        }

        pendingCollapsePaths_.clear(); // the snapshot lives exactly one search cycle

        if ( appended ) {
            rebuildVisibleRows();
        }
    }
    if ( appended ) {
        Q_EMIT layoutChanged();
    }
}

LineKind FolderSearchResults::lineKind( LineNumber visibleIndex ) const
{
    SharedLock lock( dataMutex_ );
    const auto* row = visibleRowAt( visibleIndex );
    return row ? row->kind : LineKind::Data;
}

bool FolderSearchResults::isMatchRow( LineNumber visibleIndex ) const
{
    SharedLock lock( dataMutex_ );
    const auto* row = visibleRowAt( visibleIndex );
    if ( row == nullptr || row->kind != LineKind::Data || row->isMarkRow ) {
        return false;
    }
    const auto& group = groups_[ static_cast<size_t>( row->fileId ) ];
    return group.matches[ row->matchIndex ].role == klogg::folder::RecordRole::Match;
}

QString FolderSearchResults::unavailableMarkLineText()
{
    return QStringLiteral( "<marked line unavailable: file too large for this encoding>" );
}

FolderSearchResults::MarkLineTextStatus
FolderSearchResults::markLineTextStatus( LineNumber visibleIndex ) const
{
    QString filePath;
    QTextCodec* codec = nullptr;
    {
        SharedLock lock( dataMutex_ );
        const auto* row = visibleRowAt( visibleIndex );
        // Non-mark rows read text from MatchRecord byte offsets -- never capped.
        if ( row == nullptr || !row->isMarkRow ) {
            return MarkLineTextStatus::Available;
        }
        filePath = row->markFilePath;
        codec = codecForFile( filePath );
    }
    return markLineTextStatusFor( filePath, codec );
}

FolderSearchResults::MarkLineTextStatus
FolderSearchResults::markLineTextStatusFor( const QString& filePath, QTextCodec* codec ) const
{
    // Byte-newline-safe files read by seek: text is always fetchable, any size.
    if ( codecIsByteNewlineSafe( codec ) ) {
        return MarkLineTextStatus::Available;
    }
    // Stateful codec: text comes from the whole-file cache. Populate it (a
    // transient open failure caches nothing and would otherwise flip the status
    // to Unavailable spuriously), then report Unavailable only when the cached
    // entry is present-but-empty -- the over-cap signature ensureMarkLines logs.
    ensureMarkLines( filePath, codec );
    std::lock_guard<std::mutex> ioLock( fileIoMutex_ );
    const auto cacheIt = markLineCache_.constFind( markCacheKey( filePath, codec ) );
    if ( cacheIt != markLineCache_.cend() && cacheIt->empty() ) {
        return MarkLineTextStatus::Unavailable;
    }
    return MarkLineTextStatus::Available;
}

klogg::folder::FileId FolderSearchResults::fileIdForLine( LineNumber visibleIndex ) const
{
    SharedLock lock( dataMutex_ );
    const auto* row = visibleRowAt( visibleIndex );
    return row ? row->fileId : klogg::folder::FileId{ -1 };
}

klogg::folder::SourceRef FolderSearchResults::sourceForLine( LineNumber visibleIndex ) const
{
    SharedLock lock( dataMutex_ );
    const auto* row = visibleRowAt( visibleIndex );
    if ( row == nullptr ) {
        return {};
    }
    // Mark rows carry their own (filePath, localLine) -- they may belong to a
    // marks-only file with no entry in groups_.
    if ( row->isMarkRow ) {
        return klogg::folder::SourceRef{ row->markFilePath, row->markLocalLine };
    }
    if ( row->fileId < 0 || row->fileId >= static_cast<int>( groups_.size() ) ) {
        return {};
    }

    klogg::folder::SourceRef ref;
    ref.filePath = groups_[ static_cast<size_t>( row->fileId ) ].filePath;
    if ( row->kind == LineKind::Data ) {
        const auto& matches = groups_[ static_cast<size_t>( row->fileId ) ].matches;
        if ( row->matchIndex < matches.size() ) {
            ref.localLine = matches[ row->matchIndex ].localLine;
        }
    }
    return ref;
}

std::vector<LineNumber> FolderSearchResults::matchLinesForFile( const QString& filePath ) const
{
    SharedLock lock( dataMutex_ );
    std::vector<LineNumber> out;
    // Linear scan of groups_ (small: number of files with matches). groups are
    // unique per file, so we stop at the first match. Reads groups_, not
    // visibleRows_, so collapse state has no effect.
    for ( const auto& g : groups_ ) {
        if ( g.filePath == filePath ) {
            out.reserve( g.matches.size() );
            for ( const auto& m : g.matches ) {
                // Context rows (grep -A/-B/-C) are not matches: keep them out of
                // the main-view overview marks.
                if ( m.role == klogg::folder::RecordRole::Match ) {
                    out.push_back( m.localLine );
                }
            }
            break;
        }
    }
    return out;
}

klogg::folder::FileId FolderSearchResults::groupCount() const
{
    SharedLock lock( dataMutex_ );
    return static_cast<klogg::folder::FileId>( groups_.size() );
}

LineNumber FolderSearchResults::maxLocalLine() const
{
    SharedLock lock( dataMutex_ );
    return maxLocalLine_;
}

void FolderSearchResults::toggleCollapse( klogg::folder::FileId fileId )
{
    {
        UniqueLock lock( dataMutex_ );
        // Marks-only groups use FileId = groups_.size() + idx, so the valid range
        // covers both real and marks-only groups.
        const int totalGroups = static_cast<int>( groups_.size() + marksGroups_.size() );
        if ( fileId < 0 || fileId >= totalGroups ) {
            return;
        }
        if ( collapsed_.contains( fileId ) ) {
            collapsed_.remove( fileId );
        }
        else {
            collapsed_.insert( fileId );
        }
        rebuildVisibleRows();
    }
    Q_EMIT layoutChanged();
}

void FolderSearchResults::setCollapsed( klogg::folder::FileId fileId, bool collapsed )
{
    {
        UniqueLock lock( dataMutex_ );
        const int totalGroups = static_cast<int>( groups_.size() + marksGroups_.size() );
        if ( fileId < 0 || fileId >= totalGroups ) {
            return;
        }
        if ( collapsed ) {
            collapsed_.insert( fileId );
        }
        else {
            collapsed_.remove( fileId );
        }
        rebuildVisibleRows();
    }
    Q_EMIT layoutChanged();
}

void FolderSearchResults::collapseAll()
{
    {
        UniqueLock lock( dataMutex_ );
        collapsed_.clear();
        const int totalGroups = static_cast<int>( groups_.size() + marksGroups_.size() );
        for ( klogg::folder::FileId i = 0; i < totalGroups; ++i ) {
            collapsed_.insert( i );
        }
        rebuildVisibleRows();
    }
    Q_EMIT layoutChanged();
}

void FolderSearchResults::expandAll()
{
    {
        UniqueLock lock( dataMutex_ );
        collapsed_.clear();
        rebuildVisibleRows();
    }
    Q_EMIT layoutChanged();
}

bool FolderSearchResults::isCollapsed( klogg::folder::FileId fileId ) const
{
    SharedLock lock( dataMutex_ );
    return collapsed_.contains( fileId );
}

// --- AbstractLogData ---

QString FolderSearchResults::doGetLineString( LineNumber line ) const
{
    SharedLock lock( dataMutex_ );
    const auto* row = visibleRowAt( line );
    if ( row == nullptr ) {
        return {};
    }
    if ( row->isMarkRow ) {
        // Release dataMutex_ before the (capped) file read+decode so mutators
        // (streaming search commits) are not blocked; codec is resolved under
        // the lock. readMarkLine takes only fileIoMutex_.
        const QString filePath = row->markFilePath;
        const LineNumber localLine = row->markLocalLine;
        QTextCodec* const codec = codecForFile( filePath );
        lock.unlock();
        // A mark row whose text is unavailable BY DESIGN (over-cap stateful
        // codec) renders an explicit placeholder, not a silent blank line -- so
        // the user can tell "text could not be loaded" apart from "the line is
        // empty" (the original 16 MiB defect showed an unexplained blank row).
        // The status is evaluated on the identity captured under the lock
        // above: re-resolving the visible row here could race a streaming
        // commit and attribute the placeholder to the wrong row.
        if ( markLineTextStatusFor( filePath, codec ) == MarkLineTextStatus::Unavailable ) {
            return unavailableMarkLineText();
        }
        return readMarkLine( filePath, localLine, codec );
    }
    if ( row->kind == LineKind::Header ) {
        if ( row->fileId >= 0 && row->fileId < static_cast<int>( groups_.size() ) ) {
            return headerText( row->fileId );
        }
        // Marks-only group header (fileId = groups_.size() + marksGroupIndex).
        const auto mgi = static_cast<size_t>( row->fileId - static_cast<int>( groups_.size() ) );
        if ( mgi < marksGroups_.size() ) {
            return marksGroupHeader( mgi );
        }
        return {};
    }
    return readMatchLine( row->fileId, row->matchIndex );
}

QString FolderSearchResults::doGetExpandedLineString( LineNumber line ) const
{
    // doGetLineString takes the shared lock itself.
    return untabify( doGetLineString( line ) );
}

klogg::vector<QString> FolderSearchResults::doGetLines( LineNumber first, LinesCount number ) const
{
    klogg::vector<QString> result;
    result.reserve( number.get() );
    for ( LinesCount::UnderlyingType i = 0; i < number.get(); ++i ) {
        result.push_back( doGetLineString( first + LinesCount( i ) ) );
    }
    return result;
}

klogg::vector<QString> FolderSearchResults::doGetExpandedLines( LineNumber first,
                                                                LinesCount number ) const
{
    klogg::vector<QString> result;
    result.reserve( number.get() );
    for ( LinesCount::UnderlyingType i = 0; i < number.get(); ++i ) {
        result.push_back( doGetExpandedLineString( first + LinesCount( i ) ) );
    }
    return result;
}

LineNumber FolderSearchResults::doGetLineNumber( LineNumber index ) const
{
    // sourceForLine takes the shared lock itself; do not double-lock here --
    // recursive shared_lock on std::shared_mutex is UB and deadlocks on Linux
    // when a mutation thread is waiting between the two acquisitions.
    // The results view is a cross-file listing: the "line number" of a row is
    // the 0-based local line in its SOURCE file (single-file filtered views
    // map to the underlying file's line, so copy-with-line-numbers and the
    // other getLineNumber consumers see real source lines). Header rows have
    // no source line; sourceForLine returns localLine 0 for them.
    return sourceForLine( index ).localLine;
}

bool FolderSearchResults::doIsLineCopyable( LineNumber index ) const
{
    // lineKind takes the shared lock itself; do not double-lock here.
    // Group headers are UI chrome (path + count), not source lines: exclude
    // them from the clipboard / search-composition text.
    return lineKind( index ) != LineKind::Header;
}

void FolderSearchResults::setEncodingOverrideForFile( const QString& filePath,
                                                      const QByteArray& encoding )
{
    if ( filePath.isEmpty() || encoding.isEmpty() ) {
        return;
    }
    {
        UniqueLock lock( dataMutex_ );
        encodingOverrides_.insert( filePath, encoding );
        // Cached whole-file decoded mark lines (only multi-byte/stateful codecs
        // use that path) and cached seek-path mark text were decoded with the
        // old codec; drop every entry for this file (keys are filePath + NUL +
        // ...) so the next fetch re-decodes with the override. Lock order
        // dataMutex_ -> fileIoMutex_.
        std::lock_guard<std::mutex> io( fileIoMutex_ );
        const QString prefix = filePath + QChar::Null;
        for ( auto it = markLineCache_.begin(); it != markLineCache_.end(); ) {
            it = it.key().startsWith( prefix ) ? markLineCache_.erase( it ) : std::next( it );
        }
        clearMarkTextCacheForFile( filePath );
    }
    Q_EMIT layoutChanged();
}

void FolderSearchResults::clearEncodingOverrideForFile( const QString& filePath )
{
    // QHash::remove() returns a bool on Qt 6.9 / MSVC, and `bool > 0` trips
    // C4804 ("unsafe use of type 'bool'") which /WX turns into a hard error
    // on the Windows x64-qt6 build. `!= 0` is unambiguous for both bool and
    // integral return types.
    bool removed = false;
    {
        UniqueLock lock( dataMutex_ );
        removed = encodingOverrides_.remove( filePath ) != 0;
        if ( removed ) {
            // Same composite-key sweep as setEncodingOverrideForFile (both the
            // whole-file line cache and the seek-path text cache).
            std::lock_guard<std::mutex> io( fileIoMutex_ );
            const QString prefix = filePath + QChar::Null;
            for ( auto it = markLineCache_.begin(); it != markLineCache_.end(); ) {
                it = it.key().startsWith( prefix ) ? markLineCache_.erase( it )
                                                   : std::next( it );
            }
            clearMarkTextCacheForFile( filePath );
        }
    }
    if ( removed ) {
        Q_EMIT layoutChanged();
    }
}

LinesCount FolderSearchResults::doGetNbLine() const
{
    SharedLock lock( dataMutex_ );
    return LinesCount( visibleRows_.size() );
}

LineLength FolderSearchResults::doGetMaxLength() const
{
    SharedLock lock( dataMutex_ );
    return maxLength_;
}

LineLength FolderSearchResults::doGetLineLength( LineNumber line ) const
{
    SharedLock lock( dataMutex_ );
    const auto* row = visibleRowAt( line );
    if ( row == nullptr ) {
        return 0_length;
    }
    if ( row->isMarkRow ) {
        const QString filePath = row->markFilePath;
        const LineNumber localLine = row->markLocalLine;
        QTextCodec* const codec = codecForFile( filePath );
        lock.unlock();
        return LineLength( untabify( readMarkLine( filePath, localLine, codec ) ).size() );
    }
    if ( row->kind == LineKind::Header ) {
        if ( row->fileId >= 0 && row->fileId < static_cast<int>( groups_.size() ) ) {
            return LineLength( headerText( row->fileId ).size() );
        }
        const auto mgi = static_cast<size_t>( row->fileId - static_cast<int>( groups_.size() ) );
        if ( mgi < marksGroups_.size() ) {
            return LineLength( marksGroupHeader( mgi ).size() );
        }
        return 0_length;
    }
    if ( row->fileId >= 0 && row->fileId < static_cast<int>( groups_.size() ) ) {
        const auto& matches = groups_[ static_cast<size_t>( row->fileId ) ].matches;
        if ( row->matchIndex < matches.size() ) {
            return matches[ row->matchIndex ].lineLength;
        }
    }
    return 0_length;
}

void FolderSearchResults::doSetDisplayEncoding( const char* encoding )
{
    if ( encoding != nullptr ) {
        UniqueLock lock( dataMutex_ );
        displayEncodingName_ = encoding;
    }
}

QTextCodec* FolderSearchResults::doGetDisplayEncoding() const
{
    SharedLock lock( dataMutex_ );
    return QTextCodec::codecForName( displayEncodingName_ );
}

void FolderSearchResults::doAttachReader() const
{
    // File handles are opened lazily by fileForGroup() on first text fetch.
}

void FolderSearchResults::doDetachReader() const
{
    // Release cached file handles; they are re-opened lazily if needed again.
    UniqueLock lock( dataMutex_ );
    std::lock_guard<std::mutex> io( fileIoMutex_ );
    for ( auto& slot : openFiles_ ) {
        if ( slot ) {
            slot->close();
        }
    }
}

// --- private ---

void FolderSearchResults::resetFileCaches()
{
    // Full replacement boundary. The caller holds dataMutex_ uniquely, so the
    // documented lock order remains dataMutex_ -> fileIoMutex_. Encoding choices
    // and the external marks store are pane state, not file-content caches, and
    // deliberately survive this reset. File-handle slots grow lazily on reads so
    // streaming commits never wait behind potentially slow marked-line I/O.
    std::lock_guard<std::mutex> ioLock( fileIoMutex_ );
    openFiles_.clear();
    markLineCache_.clear();
    markTextCache_.clear();
    markTextCacheBytes_ = 0;
}

const FolderSearchResults::VisibleRow* FolderSearchResults::visibleRowAt( LineNumber line ) const
{
    if ( line.get() >= visibleRows_.size() ) {
        return nullptr;
    }
    return &visibleRows_[ line.get() ];
}

void FolderSearchResults::rebuildVisibleRows()
{
    visibleRows_.clear();
    maxLength_ = 0_length;
    maxLocalLine_ = 0_lnum;

    // maxLocalLine_ sizes the line-number gutter; it must cover ALL records
    // (independent of collapse/visibility) so the gutter is wide enough when a
    // collapsed group is later expanded.
    for ( const auto& group : groups_ ) {
        for ( const auto& rec : group.matches ) {
            maxLocalLine_ = std::max( maxLocalLine_, rec.localLine );
        }
    }

    const bool marksOnly = ( visibility_ == Visibility::Marks );
    // Mark rows (marked non-match lines) are injected under Marks and
    // Marks-and-matches -- the single-file marks_ parity: a bookmark on a source
    // line stays visible across filter changes. Matches stays grep-pure (no marks).
    const bool injectMarks
        = ( visibility_ == Visibility::Marks || visibility_ == Visibility::MarksAndMatches )
        && marksStore_ != nullptr;

    // Build marks-only groups: files with marks that have no real match group.
    marksGroups_.clear();
    if ( injectMarks ) {
        QSet<QString> groupFiles;
        groupFiles.reserve( static_cast<int>( groups_.size() ) );
        for ( const auto& g : groups_ ) {
            groupFiles.insert( g.filePath );
        }
        for ( auto it = marksStore_->constBegin(); it != marksStore_->constEnd(); ++it ) {
            if ( it.value().empty() || groupFiles.contains( it.key() )
                 || ( restrictMarksToExpectedFiles_
                      && !expectedFilePaths_.contains( it.key() ) ) ) {
                continue;
            }
            MarksGroup mg;
            mg.filePath = it.key();
            mg.localLines.reserve( it.value().size() );
            for ( const auto& ln : it.value() ) {
                mg.localLines.push_back( LineNumber( ln ) );
            }
            // std::set iterates ascending, so localLines is already sorted.
            marksGroups_.push_back( std::move( mg ) );
        }
        // marksStore_ is a QHash (unspecified, per-process-salted iteration
        // order); sort so marks-only groups -- and their derived FileIds / row
        // indices -- appear in a stable, natural order across rebuilds and runs.
        std::sort( marksGroups_.begin(), marksGroups_.end(),
                   []( const MarksGroup& l, const MarksGroup& r ) {
                       return l.filePath < r.filePath;
                   } );
    }

    // Real groups: header + match rows + injected Mark rows.
    for ( klogg::folder::FileId fid = 0; fid < static_cast<int>( groups_.size() ); ++fid ) {
        const auto& group = groups_[ static_cast<size_t>( fid ) ];

        std::vector<LineNumber> markRows;
        if ( injectMarks ) {
            const auto mIt = marksStore_->constFind( group.filePath );
            if ( mIt != marksStore_->cend() ) {
                // Lines already shown as a visible record (don't inject as a mark
                // row -> avoids duplicates). Under Marks, context records and
                // unmarked matches are hidden, so only marked Match records are
                // visible -- a marked context line is therefore NOT excluded and
                // is injected as a mark row (its context record is hidden).
                QSet<uint64_t> visibleRecordLines;
                visibleRecordLines.reserve( static_cast<int>( group.matches.size() ) );
                for ( const auto& rec : group.matches ) {
                    if ( marksOnly ) {
                        if ( rec.role != klogg::folder::RecordRole::Match ) {
                            continue;
                        }
                        if ( !isLineMarked( group.filePath, rec.localLine ) ) {
                            continue;
                        }
                    }
                    visibleRecordLines.insert( rec.localLine.get() );
                }
                for ( const auto& ln : mIt.value() ) {
                    if ( !visibleRecordLines.contains( ln ) ) {
                        markRows.push_back( LineNumber( ln ) );
                    }
                }
            }
        }

        // Does this group have any visible match row?
        bool hasVisibleMatch = false;
        for ( const auto& rec : group.matches ) {
            if ( rec.role != klogg::folder::RecordRole::Match ) {
                continue;
            }
            if ( marksOnly && !isLineMarked( group.filePath, rec.localLine ) ) {
                continue;
            }
            hasVisibleMatch = true;
            break;
        }
        if ( !hasVisibleMatch && markRows.empty() ) {
            continue;
        }

        // Header (always shown for a visible group).
        visibleRows_.push_back( VisibleRow{ LineKind::Header, false, fid, 0, 0_lnum, {} } );
        maxLength_ = std::max( maxLength_, LineLength( headerText( fid ).size() ) );

        if ( collapsed_.contains( fid ) ) {
            continue;
        }

        // Merge match rows (sorted, include context) with mark rows (sorted) by
        // localLine so the listing reads in source order. Under Marks, context
        // rows and unmarked matches are hidden (only marked rows remain).
        size_t mi = 0;
        size_t mri = 0;
        while ( mi < group.matches.size() || mri < markRows.size() ) {
            const bool canMatch = mi < group.matches.size();
            const bool canMark = mri < markRows.size();
            const bool takeMatch = !canMatch
                ? false
                : ( !canMark || group.matches[ mi ].localLine < markRows[ mri ] );

            if ( takeMatch ) {
                const auto& rec = group.matches[ mi ];
                const auto useIndex = mi;
                ++mi;
                if ( marksOnly ) {
                    if ( rec.role != klogg::folder::RecordRole::Match ) {
                        continue; // hide context under Marks
                    }
                    if ( !isLineMarked( group.filePath, rec.localLine ) ) {
                        continue; // hide unmarked matches under Marks
                    }
                }
                visibleRows_.push_back(
                    VisibleRow{ LineKind::Data, false, fid, useIndex, 0_lnum, {} } );
                maxLength_ = std::max( maxLength_, rec.lineLength );
                maxLocalLine_ = std::max( maxLocalLine_, rec.localLine );
            }
            else {
                const auto ln = markRows[ mri ];
                ++mri;
                visibleRows_.push_back(
                    VisibleRow{ LineKind::Data, true, fid, 0, ln, group.filePath } );
                maxLocalLine_ = std::max( maxLocalLine_, ln );
                // maxLength_ for a mark row needs the fetched text; deferred (the
                // horizontal scrollbar may be slightly narrow for long marked
                // lines -- acceptable; doGetLineLength fetches the exact value).
            }
        }
    }

    // Marks-only groups (files with marks but no match group).
    if ( injectMarks ) {
        for ( size_t mgi = 0; mgi < marksGroups_.size(); ++mgi ) {
            const auto fid = static_cast<klogg::folder::FileId>( groups_.size() + mgi );
            const auto& mg = marksGroups_[ mgi ];
            visibleRows_.push_back( VisibleRow{ LineKind::Header, false, fid, 0, 0_lnum, {} } );
            maxLength_ = std::max( maxLength_, LineLength( marksGroupHeader( mgi ).size() ) );
            if ( collapsed_.contains( fid ) ) {
                continue;
            }
            for ( const auto& ln : mg.localLines ) {
                visibleRows_.push_back(
                    VisibleRow{ LineKind::Data, true, fid, 0, ln, mg.filePath } );
                maxLocalLine_ = std::max( maxLocalLine_, ln );
            }
        }
    }
}

bool FolderSearchResults::isLineMarked( const QString& filePath, LineNumber localLine ) const
{
    if ( marksStore_ == nullptr ) {
        return false;
    }
    const auto it = marksStore_->constFind( filePath );
    return it != marksStore_->cend() && it->count( localLine.get() ) > 0;
}

void FolderSearchResults::setVisibility( Visibility visibility )
{
    {
        UniqueLock lock( dataMutex_ );
        visibility_ = visibility;
        rebuildVisibleRows();
    }
    Q_EMIT layoutChanged();
}

void FolderSearchResults::setMarksStore( const QHash<QString, std::set<uint64_t>>* store )
{
    bool rebuilt = false;
    {
        UniqueLock lock( dataMutex_ );
        marksStore_ = store;
        // The visible set depends on marks under Marks AND Marks-and-matches now
        // (marked non-match rows are injected under both); rebuild there. Under
        // Matches no marks are shown, so a rebuild is unnecessary.
        if ( visibility_ == Visibility::Marks || visibility_ == Visibility::MarksAndMatches ) {
            rebuildVisibleRows();
            rebuilt = true;
        }
    }
    if ( rebuilt ) {
        Q_EMIT layoutChanged();
    }
}

void FolderSearchResults::refreshForMarksChange()
{
    // The visible set depends on marks under Marks AND Marks-and-matches (both
    // inject marked non-match rows); Matches shows no marks.
    bool rebuilt = false;
    {
        UniqueLock lock( dataMutex_ );
        if ( visibility_ == Visibility::Marks || visibility_ == Visibility::MarksAndMatches ) {
            rebuildVisibleRows();
            rebuilt = true;
        }
    }
    if ( rebuilt ) {
        Q_EMIT layoutChanged();
    }
}

QString FolderSearchResults::headerText( klogg::folder::FileId fileId ) const
{
    if ( fileId < 0 || fileId >= static_cast<int>( groups_.size() ) ) {
        return {};
    }
    const auto& group = groups_[ static_cast<size_t>( fileId ) ];

    // ▸ (U+25B8) when collapsed, ▾ (U+25BE) when expanded — VS-Code style.
    const QChar arrow = collapsed_.contains( fileId ) ? QChar( 0x25B8 ) : QChar( 0x25BE );
    QString text;
    text.reserve( group.filePath.size() + 24 );
    text += arrow;
    text += QLatin1Char( ' ' );
    text += group.filePath;
    text += QLatin1Char( ' ' );
    text += QChar( 0x2014 ); // em dash —
    text += QLatin1Char( ' ' );
    // Count only real Match rows: with -A/-B/-C the group also holds Context
    // records that must not inflate the per-file result count.
    int matchCount = 0;
    for ( const auto& r : group.matches ) {
        if ( r.role == klogg::folder::RecordRole::Match ) {
            ++matchCount;
        }
    }
    text += QString::number( matchCount );
    text += QLatin1String( " results" );
    return text;
}

QString FolderSearchResults::readMatchLine( klogg::folder::FileId fileId, size_t matchIndex ) const
{
    // Caller (a public getter) already holds dataMutex_ shared; the container
    // references below are stable for the whole call. Only the shared per-group
    // QFile cursors need extra serialization (a second reader, e.g. QuickFind
    // on its worker, would otherwise interleave seek/read with the painter).
    if ( fileId < 0 || fileId >= static_cast<int>( groups_.size() ) ) {
        return {};
    }
    const auto& matches = groups_[ static_cast<size_t>( fileId ) ].matches;
    if ( matchIndex >= matches.size() ) {
        return {};
    }
    const auto& record = matches[ matchIndex ];

    const auto start = record.lineStartByte.get();
    const auto end = record.lineEndByte.get();
    if ( end <= start ) {
        return {};
    }

    QByteArray bytes;
    {
        std::lock_guard<std::mutex> io( fileIoMutex_ );
        QFile* file = fileForGroup( fileId );
        if ( file == nullptr ) {
            return {};
        }
        if ( !file->seek( start ) ) {
            LOG_WARNING << "FolderSearchResults: seek failed on" << groups_[ static_cast<size_t>( fileId ) ].filePath;
            return {};
        }
        bytes = file->read( end - start );
    }
    // Decode with the SOURCE file's codec: a per-file user override (the
    // Encoding-menu pick for the file open in the main view) wins over the
    // codec detected during the scan and stored on the FileGroup -- otherwise
    // a misdetection would keep the results view in mojibake even after the
    // user corrected the encoding (single-file setEncoding parity).
    // displayEncodingName_ stays for the view layer and must not override
    // source interpretation.
    const auto& group = groups_[ static_cast<size_t>( fileId ) ];
    QTextCodec* src = nullptr;
    const auto overrideIt = encodingOverrides_.constFind( group.filePath );
    if ( overrideIt != encodingOverrides_.cend() ) {
        src = QTextCodec::codecForName( overrideIt.value() );
    }
    if ( src == nullptr ) {
        src = group.sourceCodec;
    }
    QString text = src != nullptr ? src->toUnicode( bytes ) : QString::fromUtf8( bytes );

    // Strip the trailing newline (and a preceding CR for CRLF files). For
    // multi-byte encodings codec->toUnicode of the multi-byte LF yields '\n',
    // so this strip works uniformly.
    while ( text.endsWith( QLatin1Char( '\n' ) ) || text.endsWith( QLatin1Char( '\r' ) ) ) {
        text.chop( 1 );
    }
    return text;
}

QFile* FolderSearchResults::fileForGroup( klogg::folder::FileId fileId ) const
{
    if ( fileId < 0 || fileId >= static_cast<int>( groups_.size() ) ) {
        return nullptr;
    }
    if ( openFiles_.size() < groups_.size() ) {
        openFiles_.resize( groups_.size() );
    }

    auto& slot = openFiles_[ static_cast<size_t>( fileId ) ];
    if ( slot != nullptr && slot->isOpen() ) {
        return slot.get();
    }

    auto file = std::make_unique<QFile>( groups_[ static_cast<size_t>( fileId ) ].filePath );
    if ( !file->open( QIODevice::ReadOnly ) ) {
        LOG_WARNING << "FolderSearchResults: cannot open" << groups_[ static_cast<size_t>( fileId ) ].filePath;
        return nullptr;
    }

    auto* raw = file.get();
    slot = std::move( file );
    return raw;
}

QString FolderSearchResults::marksGroupHeader( size_t marksGroupIndex ) const
{
    if ( marksGroupIndex >= marksGroups_.size() ) {
        return {};
    }
    const auto& mg = marksGroups_[ marksGroupIndex ];
    const auto fid = static_cast<klogg::folder::FileId>( groups_.size() + marksGroupIndex );
    const QChar arrow = collapsed_.contains( fid ) ? QChar( 0x25B8 ) : QChar( 0x25BE );
    QString text;
    text.reserve( mg.filePath.size() + 24 );
    text += arrow;
    text += QLatin1Char( ' ' );
    text += mg.filePath;
    text += QLatin1Char( ' ' );
    text += QChar( 0x2014 ); // em dash
    text += QLatin1Char( ' ' );
    text += QString::number( static_cast<int>( mg.localLines.size() ) );
    text += QLatin1String( " marks" );
    return text;
}

void FolderSearchResults::clearMarkTextCacheForFile( const QString& filePath ) const
{
    // Caller holds fileIoMutex_. Subtract each removed entry's decoded bytes so
    // the aggregate budget stays accurate.
    const QString prefix = filePath + QChar::Null;
    for ( auto it = markTextCache_.begin(); it != markTextCache_.end(); ) {
        if ( it.key().startsWith( prefix ) ) {
            markTextCacheBytes_
                -= static_cast<qint64>( it.value().size() ) * qint64{ sizeof( QChar ) };
            it = markTextCache_.erase( it );
        }
        else {
            ++it;
        }
    }
}

QTextCodec* FolderSearchResults::codecForFile( const QString& filePath ) const
{
    // Caller holds dataMutex_ shared; reads encodingOverrides_ + groups_.
    const auto overrideIt = encodingOverrides_.constFind( filePath );
    if ( overrideIt != encodingOverrides_.cend() ) {
        QTextCodec* const c = QTextCodec::codecForName( overrideIt.value() );
        if ( c != nullptr ) {
            return c;
        }
    }
    for ( const auto& g : groups_ ) {
        if ( g.filePath == filePath ) {
            return g.sourceCodec;
        }
    }
    return nullptr; // UTF-8 default for marks-only files (no scanned codec)
}

void FolderSearchResults::ensureMarkLines( const QString& filePath, QTextCodec* codec ) const
{
    // Whole-file decoded line cache. This is the CORRECTNESS fallback for
    // multi-byte / stateful encodings (UTF-16, UTF-32, Shift-JIS, ...) where a
    // byte-level '\n' scan cannot locate line boundaries, and for small files
    // where one decode is cheapest. Files over kMarkLineCacheCap with such a
    // codec insert an EMPTY cache (marked-line text unavailable -- a deliberate
    // trade-off to avoid a main-thread O(file) decode). Single-byte / UTF-8
    // files NEVER reach this path for text fetch: readMarkLine seeks to the
    // line directly instead (see readMarkLineSeek), which scales to any file
    // size. Caller does NOT hold fileIoMutex_.
    std::lock_guard<std::mutex> io( fileIoMutex_ );
    const QString cacheKey = markCacheKey( filePath, codec );
    if ( markLineCache_.contains( cacheKey ) ) {
        return;
    }

    std::vector<QString> lines;
    QFile file( filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        // Transient open failure: don't cache an empty entry so a later fetch
        // (e.g. after the file becomes readable again) can retry instead of
        // being permanently recorded as "no mark text".
        return;
    }
    // Over-cap with a stateful codec: cache an EMPTY list and LOG it. The empty
    // entry is what markLineTextStatus keys on to report Unavailable -- callers
    // must never treat "cached but empty" as "the line is empty" (that silent
    // conflation was the original blank-marked-row defect).
    if ( file.size() > kMarkLineCacheCap ) {
        LOG_WARNING << "FolderSearchResults: mark-line text unavailable for" << filePath
                    << "-- file size" << file.size() << "exceeds kMarkLineCacheCap ("
                    << kMarkLineCacheCap << ") with a stateful codec";
    }
    if ( file.size() <= kMarkLineCacheCap ) {
        const QByteArray bytes = file.readAll();
        const QString text
            = codec != nullptr ? codec->toUnicode( bytes ) : QString::fromUtf8( bytes );
        // File size is capped at kMarkLineCacheCap (16 MiB) above, so all line
        // indices fit in int. Use int + explicit casts for Qt 5 / Qt 6
        // portability: Qt 5's QString::indexOf/mid take int, Qt 6's take
        // qsizetype, and a raw qsizetype here triggers -Werror=conversion on
        // the Qt 5 Linux CI builds.
        const int n = static_cast<int>( text.size() );
        int start = 0;
        while ( start <= n ) {
            int idx = static_cast<int>( text.indexOf( QLatin1Char( '\n' ), start ) );
            if ( idx < 0 ) {
                idx = n;
            }
            QString line = text.mid( start, idx - start );
            if ( line.endsWith( QLatin1Char( '\r' ) ) ) {
                line.chop( 1 );
            }
            lines.push_back( std::move( line ) );
            if ( idx >= n ) {
                break;
            }
            start = idx + 1;
        }
    }
    markLineCache_.insert( cacheKey, std::move( lines ) );
}

bool FolderSearchResults::codecIsByteNewlineSafe( QTextCodec* codec )
{
    // ALLOWLIST: true only for encodings where the raw byte 0x0A unambiguously
    // marks a line boundary, so a seek-based byte scan finds the same lines the
    // codec would. Anything not enumerated returns false and stays on the
    // whole-file decode path -- a denylist would let an unlisted codec with a
    // non-ASCII newline encoding (or a stateful one we forgot) silently
    // mis-seek, so the safe default is "not byte-newline-safe".
    //
    // Qualifies: nullptr (UTF-8 default), UTF-8 (0x0A never appears inside a
    // multi-byte sequence), and the stateless single-byte / ASCII-superset
    // codecs (each byte is one code unit and 0x0A is LF).
    if ( codec == nullptr ) {
        return true;
    }
    if ( codec->mibEnum() == 106 ) { // UTF-8
        return true;
    }
    const QByteArray name = codec->name().toUpper();
    static const QByteArrayList kAllowed = {
        // ISO-8859 single-byte family.
        QByteArrayLiteral( "ISO-8859-1" ),  QByteArrayLiteral( "ISO-8859-2" ),
        QByteArrayLiteral( "ISO-8859-3" ),  QByteArrayLiteral( "ISO-8859-4" ),
        QByteArrayLiteral( "ISO-8859-5" ),  QByteArrayLiteral( "ISO-8859-6" ),
        QByteArrayLiteral( "ISO-8859-7" ),  QByteArrayLiteral( "ISO-8859-8" ),
        QByteArrayLiteral( "ISO-8859-9" ),  QByteArrayLiteral( "ISO-8859-10" ),
        QByteArrayLiteral( "ISO-8859-13" ), QByteArrayLiteral( "ISO-8859-14" ),
        QByteArrayLiteral( "ISO-8859-15" ), QByteArrayLiteral( "ISO-8859-16" ),
        QByteArrayLiteral( "ISO 8859-1" ), // Qt's spaced alias for Latin-1.
        // Windows single-byte code pages.
        QByteArrayLiteral( "WINDOWS-1250" ), QByteArrayLiteral( "WINDOWS-1251" ),
        QByteArrayLiteral( "WINDOWS-1252" ), QByteArrayLiteral( "WINDOWS-1253" ),
        QByteArrayLiteral( "WINDOWS-1254" ), QByteArrayLiteral( "WINDOWS-1255" ),
        QByteArrayLiteral( "WINDOWS-1256" ), QByteArrayLiteral( "WINDOWS-1257" ),
        QByteArrayLiteral( "WINDOWS-1258" ),
        // Other common single-byte encodings.
        QByteArrayLiteral( "KOI8-R" ),  QByteArrayLiteral( "KOI8-U" ),
        QByteArrayLiteral( "CP866" ),   QByteArrayLiteral( "IBM866" ),
        QByteArrayLiteral( "APPLE ROMAN" ), QByteArrayLiteral( "MACINTOSH" ),
        QByteArrayLiteral( "US-ASCII" ), QByteArrayLiteral( "ASCII" ),
        QByteArrayLiteral( "LATIN1" ), QByteArrayLiteral( "LATIN-1" ),
    };
    return kAllowed.contains( name );
}

QString FolderSearchResults::readMarkLineSeek( const QString& filePath, LineNumber localLine,
                                               QTextCodec* codec ) const
{
    // Seek-based per-line read for byte-newline-safe encodings: scan forward to
    // the start of localLine, then read to the next '\n'. Only the target line's
    // bytes are touched, so this scales to any file size (the whole-file cache
    // silently returned empty text over its 16 MiB cap -- the blank-marked-row
    // bug this replaces). Decoded with the resolved codec (or UTF-8 default),
    // trailing CR/LF stripped. Caller has released dataMutex_; takes only
    // fileIoMutex_.
    std::lock_guard<std::mutex> ioLock( fileIoMutex_ );

    // Resolved-text cache: the view re-fetches every visible mark row on each
    // repaint, so without caching a mark near the end of a large file would
    // rescan from byte 0 every frame. Keyed by file+codec+line.
    const QString textKey
        = markCacheKey( filePath, codec ) + QChar::Null + QString::number( localLine.get() );
    const auto cached = markTextCache_.constFind( textKey );
    if ( cached != markTextCache_.cend() ) {
        return cached.value();
    }

    QFile file( filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }

    const quint64 target = localLine.get();
    if ( target > 0 ) {
        // Stream past `target` newlines. Chunked reads bound memory; after the
        // target newline is seen INSIDE a chunk, seek back to just past it --
        // the buffered read already consumed the whole chunk, so without the
        // reposition readLine() would start at the chunk end (mid-file).
        constexpr qint64 kScanChunk = 1LL << 16; // 64 KiB
        quint64 seen = 0;
        bool found = false;
        while ( !found ) {
            const qint64 chunkStart = file.pos();
            const QByteArray chunk = file.read( kScanChunk );
            if ( chunk.isEmpty() ) {
                return {}; // EOF before the target line: no such line.
            }
            // int (not qsizetype) index: Qt 5's QByteArray::at takes int and the
            // build is -Werror=conversion, so a qsizetype index fails the Qt 5
            // Linux CI legs. kScanChunk (64 KiB) always fits in int.
            const int chunkLen = klogg::isize( chunk );
            for ( int i = 0; i < chunkLen; ++i ) {
                if ( chunk.at( i ) == '\n' ) {
                    ++seen;
                    if ( seen == target ) {
                        if ( !file.seek( chunkStart + i + 1 ) ) {
                            return {};
                        }
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    QByteArray lineBytes = file.readLine();
    if ( lineBytes.isEmpty() && file.atEnd() ) {
        return {}; // target line index is past the last line.
    }
    while ( lineBytes.endsWith( '\n' ) || lineBytes.endsWith( '\r' ) ) {
        lineBytes.chop( 1 );
    }
    const QString text = codec != nullptr ? codec->toUnicode( lineBytes )
                                          : QString::fromUtf8( lineBytes );
    // Cache only a successfully-read line; a read past EOF returns above without
    // caching so a file that later grows can be retried. Honor the aggregate
    // byte budget AND the entry-count cap: empty/short lines cost ~0 payload
    // bytes, so the byte budget alone would let the QString keys and QHash
    // nodes grow with the mark count. Over the limit the line is returned
    // without caching it (the row still renders; it simply rescans on the next
    // repaint) rather than growing without bound.
    const qint64 textBytes = static_cast<qint64>( text.size() ) * qint64{ sizeof( QChar ) };
    if ( markTextCache_.size() < kMarkTextCacheMaxEntries
         && markTextCacheBytes_ + textBytes <= kMarkTextCacheBudget ) {
        markTextCache_.insert( textKey, text );
        markTextCacheBytes_ += textBytes;
    }
    return text;
}

QString FolderSearchResults::readMarkLine( const QString& filePath, LineNumber localLine,
                                           QTextCodec* codec ) const
{
    // Fetch the marked source line by line number (marked non-match lines have
    // no MatchRecord offset). Byte-newline-safe encodings (UTF-8 / single-byte,
    // the common case) use a bounded seek read that scales to any file size;
    // multi-byte/stateful codecs fall back to the (capped) whole-file decoded
    // cache. Caller has released dataMutex_; only fileIoMutex_ is taken here.
    if ( codecIsByteNewlineSafe( codec ) ) {
        return readMarkLineSeek( filePath, localLine, codec );
    }
    ensureMarkLines( filePath, codec );
    std::lock_guard<std::mutex> io( fileIoMutex_ );
    const auto it = markLineCache_.constFind( markCacheKey( filePath, codec ) );
    const auto lineIdx = static_cast<size_t>( localLine.get() );
    if ( it != markLineCache_.cend() && lineIdx < it->size() ) {
        return it->at( lineIdx );
    }
    return {};
}
