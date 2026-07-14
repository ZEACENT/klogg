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

#include "linetypes.h"
#include "log.h"

FolderSearchResults::FolderSearchResults()
    : AbstractLogData()
{
}

FolderSearchResults::~FolderSearchResults() = default;

void FolderSearchResults::setResults( std::vector<klogg::folder::FileGroup> groups )
{
    groups_ = std::move( groups );
    // A new result set invalidates any cached file handles; size to the new
    // group count so fileForGroup() can index by fileId directly.
    openFiles_.clear();
    openFiles_.resize( groups_.size() );
    // Defensive: drop groups with no matches (the engine/caller already filters
    // them, but FolderSearchResults must never emit an empty group's header).
    groups_.erase( std::remove_if( groups_.begin(), groups_.end(),
                                   []( const klogg::folder::FileGroup& g ) { return g.matches.empty(); } ),
                   groups_.end() );

    // NOTE: setResults resets collapse (it is a full result-set replacement, not
    // the live re-search path -- startSearch uses beginSearch, which preserves
    // collapse via snapshotCollapsedPaths/reapplyCollapseForLastGroup). Callers
    // that need preservation here should snapshot filePaths the same way.
    collapsed_.clear();
    rebuildVisibleRows();
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
    if ( pendingCollapsePaths_.contains( lastPath ) ) {
        collapsed_.insert( static_cast<int>( groups_.size() ) - 1 );
    }
}

void FolderSearchResults::beginSearch( const QStringList& expectedFileOrder )
{
    // Preserve collapse across this re-scan: snapshot which filePaths are collapsed
    // BEFORE groups_ is rebuilt and FileIds are reassigned, then re-apply as groups
    // stream back in (addFileGroup/flushPending). Without this, a context-line
    // change (which re-scans via beginSearch) would expand every collapsed group.
    snapshotCollapsedPaths();
    groups_.clear();
    openFiles_.clear();
    collapsed_.clear();
    pendingByIndex_.assign( static_cast<size_t>( expectedFileOrder.size() ), std::nullopt );
    nextExpectedIndex_ = 0;
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

void FolderSearchResults::addFileGroup( int fileIndex, klogg::folder::FileGroup group )
{
    if ( fileIndex < 0 || static_cast<size_t>( fileIndex ) >= pendingByIndex_.size() ) {
        return;
    }
    pendingByIndex_[ static_cast<size_t>( fileIndex ) ] = std::move( group );

    bool appended = false;
    // Drain every consecutive completed predecessor from the cursor. This makes
    // the display order always match enumeration order, no matter which file
    // finished first: file[5] cannot appear until file[0..4] are all committed.
    while ( nextExpectedIndex_ < pendingByIndex_.size()
            && pendingByIndex_[ nextExpectedIndex_ ].has_value() ) {
        auto& opt = pendingByIndex_[ nextExpectedIndex_ ];
        if ( !opt->matches.empty() ) {
            groups_.push_back( std::move( *opt ) );
            reapplyCollapseForLastGroup();
            // Keep file-handle slots aligned to fileId (== group index), matching
            // the setResults contract so fileForGroup() can index directly.
            openFiles_.resize( groups_.size() );
            appended = true;
        }
        ++nextExpectedIndex_;
    }

    if ( appended ) {
        rebuildVisibleRows();
        Q_EMIT layoutChanged();
    }
}

void FolderSearchResults::flushPending()
{
    bool appended = false;
    // Commit every remaining present group, skipping missing/empty slots (an
    // interrupted scan may leave a predecessor that will never arrive).
    while ( nextExpectedIndex_ < pendingByIndex_.size() ) {
        auto& opt = pendingByIndex_[ nextExpectedIndex_ ];
        if ( opt.has_value() && !opt->matches.empty() ) {
            groups_.push_back( std::move( *opt ) );
            reapplyCollapseForLastGroup();
            openFiles_.resize( groups_.size() );
            appended = true;
        }
        ++nextExpectedIndex_;
    }

    pendingCollapsePaths_.clear(); // the snapshot lives exactly one search cycle

    if ( appended ) {
        rebuildVisibleRows();
        Q_EMIT layoutChanged();
    }
}

LineKind FolderSearchResults::lineKind( LineNumber visibleIndex ) const
{
    const auto* row = visibleRowAt( visibleIndex );
    return row ? row->kind : LineKind::Data;
}

bool FolderSearchResults::isMatchRow( LineNumber visibleIndex ) const
{
    const auto* row = visibleRowAt( visibleIndex );
    if ( row == nullptr || row->kind != LineKind::Data ) {
        return false;
    }
    const auto& group = groups_[ static_cast<size_t>( row->fileId ) ];
    return group.matches[ row->matchIndex ].role == klogg::folder::RecordRole::Match;
}

klogg::folder::FileId FolderSearchResults::fileIdForLine( LineNumber visibleIndex ) const
{
    const auto* row = visibleRowAt( visibleIndex );
    return row ? row->fileId : klogg::folder::FileId{ -1 };
}

klogg::folder::SourceRef FolderSearchResults::sourceForLine( LineNumber visibleIndex ) const
{
    const auto* row = visibleRowAt( visibleIndex );
    if ( row == nullptr || row->fileId < 0 || row->fileId >= static_cast<int>( groups_.size() ) ) {
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
    return static_cast<klogg::folder::FileId>( groups_.size() );
}

LineNumber FolderSearchResults::maxLocalLine() const
{
    return maxLocalLine_;
}

void FolderSearchResults::toggleCollapse( klogg::folder::FileId fileId )
{
    if ( fileId < 0 || fileId >= static_cast<int>( groups_.size() ) ) {
        return;
    }
    if ( collapsed_.contains( fileId ) ) {
        collapsed_.remove( fileId );
    }
    else {
        collapsed_.insert( fileId );
    }
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

void FolderSearchResults::setCollapsed( klogg::folder::FileId fileId, bool collapsed )
{
    if ( fileId < 0 || fileId >= static_cast<int>( groups_.size() ) ) {
        return;
    }
    if ( collapsed ) {
        collapsed_.insert( fileId );
    }
    else {
        collapsed_.remove( fileId );
    }
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

void FolderSearchResults::collapseAll()
{
    collapsed_.clear();
    for ( klogg::folder::FileId i = 0; i < static_cast<int>( groups_.size() ); ++i ) {
        collapsed_.insert( i );
    }
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

void FolderSearchResults::expandAll()
{
    collapsed_.clear();
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

bool FolderSearchResults::isCollapsed( klogg::folder::FileId fileId ) const
{
    return collapsed_.contains( fileId );
}

// --- AbstractLogData ---

QString FolderSearchResults::doGetLineString( LineNumber line ) const
{
    const auto* row = visibleRowAt( line );
    if ( row == nullptr ) {
        return {};
    }
    if ( row->kind == LineKind::Header ) {
        return headerText( row->fileId );
    }
    return readMatchLine( row->fileId, row->matchIndex );
}

QString FolderSearchResults::doGetExpandedLineString( LineNumber line ) const
{
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
    return index;
}

LinesCount FolderSearchResults::doGetNbLine() const
{
    return LinesCount( visibleRows_.size() );
}

LineLength FolderSearchResults::doGetMaxLength() const
{
    return maxLength_;
}

LineLength FolderSearchResults::doGetLineLength( LineNumber line ) const
{
    const auto* row = visibleRowAt( line );
    if ( row == nullptr ) {
        return 0_length;
    }
    if ( row->kind == LineKind::Header ) {
        return LineLength( headerText( row->fileId ).size() );
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
        displayEncodingName_ = encoding;
    }
}

QTextCodec* FolderSearchResults::doGetDisplayEncoding() const
{
    return QTextCodec::codecForName( displayEncodingName_ );
}

void FolderSearchResults::doAttachReader() const
{
    // File handles are opened lazily by fileForGroup() on first text fetch.
}

void FolderSearchResults::doDetachReader() const
{
    // Release cached file handles; they are re-opened lazily if needed again.
    for ( auto& slot : openFiles_ ) {
        if ( slot ) {
            slot->close();
        }
    }
}

// --- private ---

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

    for ( klogg::folder::FileId fid = 0; fid < static_cast<int>( groups_.size() ); ++fid ) {
        const auto& group = groups_[ static_cast<size_t>( fid ) ];

        if ( marksOnly ) {
            // Under the Marks filter a group with no marked MATCH rows is hidden
            // entirely (context rows are never marks). Matches single-file Marks
            // view omitting unmarked lines.
            bool anyMarked = false;
            for ( size_t mi = 0; mi < group.matches.size(); ++mi ) {
                const auto& rec = group.matches[ mi ];
                if ( rec.role == klogg::folder::RecordRole::Match
                     && isLineMarked( group.filePath, rec.localLine ) ) {
                    anyMarked = true;
                    break;
                }
            }
            if ( !anyMarked ) {
                continue;
            }
        }

        // Every shown group always shows its header (collapsed or not).
        visibleRows_.push_back( VisibleRow{ LineKind::Header, fid, 0 } );
        maxLength_ = std::max( maxLength_, LineLength( headerText( fid ).size() ) );

        if ( collapsed_.contains( fid ) ) {
            continue;
        }
        // group.matches holds Match AND Context records (grep -A/-B/-C), already
        // sorted by localLine ascending and deduplicated by the engine, so the
        // linear walk produces grep grouping (context interleaved with matches)
        // for free. Under Marks, context rows are hidden (not marks) and unmarked
        // matches are hidden.
        for ( size_t mi = 0; mi < group.matches.size(); ++mi ) {
            const auto& rec = group.matches[ mi ];
            if ( marksOnly ) {
                if ( rec.role != klogg::folder::RecordRole::Match ) {
                    continue;
                }
                if ( !isLineMarked( group.filePath, rec.localLine ) ) {
                    continue;
                }
            }
            visibleRows_.push_back( VisibleRow{ LineKind::Data, fid, mi } );
            maxLength_ = std::max( maxLength_, rec.lineLength );
            maxLocalLine_ = std::max( maxLocalLine_, rec.localLine );
        }
    }
}

bool FolderSearchResults::isLineMarked( const QString& filePath, LineNumber localLine ) const
{
    return isMarkedLine_ != nullptr && isMarkedLine_( filePath, localLine );
}

void FolderSearchResults::setVisibility( Visibility visibility )
{
    visibility_ = visibility;
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

void FolderSearchResults::setMarkedLineQuery( std::function<bool( const QString&, LineNumber )> query )
{
    isMarkedLine_ = std::move( query );
    if ( visibility_ == Visibility::Marks ) {
        rebuildVisibleRows();
        Q_EMIT layoutChanged();
    }
}

void FolderSearchResults::refreshForMarksChange()
{
    // The visible set depends on marks only under the Marks filter.
    if ( visibility_ == Visibility::Marks ) {
        rebuildVisibleRows();
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
    if ( fileId < 0 || fileId >= static_cast<int>( groups_.size() ) ) {
        return {};
    }
    const auto& matches = groups_[ static_cast<size_t>( fileId ) ].matches;
    if ( matchIndex >= matches.size() ) {
        return {};
    }
    const auto& record = matches[ matchIndex ];

    QFile* file = fileForGroup( fileId );
    if ( file == nullptr ) {
        return {};
    }

    const auto start = record.lineStartByte.get();
    const auto end = record.lineEndByte.get();
    if ( end <= start ) {
        return {};
    }
    if ( !file->seek( start ) ) {
        LOG_WARNING << "FolderSearchResults: seek failed on" << groups_[ static_cast<size_t>( fileId ) ].filePath;
        return {};
    }

    const QByteArray bytes = file->read( end - start );
    // Decode with the SOURCE file's codec (detected during the scan and stored
    // on the FileGroup), not displayEncodingName_. This mirrors LogData, which
    // reads raw line bytes and decodes with codec_.makeDecoder() (logdata.cpp)
    // -- the source codec interprets the bytes. displayEncodingName_ stays for
    // the view layer and must not override source interpretation.
    auto* src = groups_[ static_cast<size_t>( fileId ) ].sourceCodec;
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
    if ( fileId < 0 || fileId >= static_cast<int>( openFiles_.size() ) ) {
        return nullptr;
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
