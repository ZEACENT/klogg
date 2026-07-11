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

    collapsed_.clear();
    rebuildVisibleRows();
    Q_EMIT layoutChanged();
}

LineKind FolderSearchResults::lineKind( LineNumber visibleIndex ) const
{
    const auto* row = visibleRowAt( visibleIndex );
    return row ? row->kind : LineKind::Data;
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

    for ( klogg::folder::FileId fid = 0; fid < static_cast<int>( groups_.size() ); ++fid ) {
        const auto& group = groups_[ static_cast<size_t>( fid ) ];

        // Every group always shows its header (collapsed or not).
        visibleRows_.push_back( VisibleRow{ LineKind::Header, fid, 0 } );
        maxLength_ = std::max( maxLength_, LineLength( headerText( fid ).size() ) );

        if ( collapsed_.contains( fid ) ) {
            continue;
        }
        for ( size_t mi = 0; mi < group.matches.size(); ++mi ) {
            visibleRows_.push_back( VisibleRow{ LineKind::Data, fid, mi } );
            maxLength_ = std::max( maxLength_, group.matches[ mi ].lineLength );
            maxLocalLine_ = std::max( maxLocalLine_, group.matches[ mi ].localLine );
        }
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
    text += QString::number( static_cast<int>( group.matches.size() ) );
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
    auto* codec = QTextCodec::codecForName( displayEncodingName_ );
    QString text = codec != nullptr ? codec->toUnicode( bytes ) : QString::fromUtf8( bytes );

    // Strip the trailing newline (and a preceding CR for CRLF files).
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
