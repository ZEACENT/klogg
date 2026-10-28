/*
 * Copyright (C) 2024 -- 2026 Anton Filimonov and other contributors
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

#include "concatenatedlogdata.h"
#include "logdata.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace {
std::atomic<uint64_t> nextConcatenatedIncarnation{ 1 };

class ConcatenatedReadSnapshot final : public klogg::LogSnapshot {
  public:
    struct Source {
        klogg::LogSnapshot::Ptr snapshot;
        uint64_t incarnation = 0;
        klogg::LogRowId childIdentity;
        LinesCount cumulativeLines = 0_lcount;
    };

    explicit ConcatenatedReadSnapshot( std::vector<Source> sources )
        : sources_( std::move( sources ) )
    {
        for ( size_t index = 0; index < sources_.size(); ++index ) {
            auto& source = sources_[ index ];
            const auto count = source.snapshot->lineCount();
            if ( count.get() > std::numeric_limits<uint64_t>::max() - totalLines_.get() ) {
                throw klogg::SnapshotError{ "Concatenated snapshot line count overflows" };
            }
            totalLines_ += count;
            source.cumulativeLines = totalLines_;
            if ( count != 0_lcount ) {
                sourceByIncarnation_.emplace( source.incarnation, index );
            }
        }
    }

    LinesCount lineCount() const override { return totalLines_; }

    klogg::SnapshotRow row( LineNumber index ) const override
    {
        const auto& source = sourceForRow( index );
        auto result = source.snapshot->row( localRow( source, index ) );
        result.identity.origin = klogg::LogRowId::Origin::Concatenated;
        result.identity.incarnation = source.incarnation;
        return result;
    }

    QString lineString( LineNumber index ) const override
    {
        const auto& source = sourceForRow( index );
        return source.snapshot->lineString( localRow( source, index ) );
    }

    OptionalLineNumber rowForIdentity( const klogg::LogRowId& identity ) const override
    {
        if ( identity.origin != klogg::LogRowId::Origin::Concatenated ) {
            return {};
        }
        const auto found = sourceByIncarnation_.find( identity.incarnation );
        if ( found == sourceByIncarnation_.end() ) {
            return {};
        }
        const auto& source = sources_[ found->second ];
        auto childIdentity = identity;
        childIdentity.origin = source.childIdentity.origin;
        childIdentity.incarnation = source.childIdentity.incarnation;
        const auto local = source.snapshot->rowForIdentity( childIdentity );
        return local ? OptionalLineNumber{ LineNumber( sourceStart( source ) + local->get() ) }
                     : OptionalLineNumber{};
    }

  private:
    static uint64_t sourceStart( const Source& source )
    {
        return source.cumulativeLines.get() - source.snapshot->lineCount().get();
    }

    static LineNumber localRow( const Source& source, LineNumber index )
    {
        // NOLINTNEXTLINE(modernize-return-braced-init-list) -- The strong typedef constructor is explicit.
        return LineNumber( index.get() - sourceStart( source ) );
    }

    const Source& sourceForRow( LineNumber index ) const
    {
        if ( index.get() >= totalLines_.get() ) {
            throw klogg::SnapshotError{ "Concatenated snapshot row is out of bounds" };
        }
        const auto found = std::upper_bound(
            sources_.begin(), sources_.end(), index.get(),
            []( uint64_t line, const Source& source ) { return line < source.cumulativeLines.get(); } );
        return *found;
    }

    std::vector<Source> sources_;
    std::unordered_map<uint64_t, size_t> sourceByIncarnation_;
    LinesCount totalLines_ = 0_lcount;
};
}

ConcatenatedLogData::ConcatenatedLogData()
    : AbstractLogData()
{
}

ConcatenatedLogData::~ConcatenatedLogData()
{
    for ( auto& info : sources_ ) {
        QObject::disconnect( info.loadingFinishedConnection );
        QObject::disconnect( info.fileChangedConnection );
        QObject::disconnect( info.interpretationChangedConnection );
    }
    sources_.clear();
    totalLines_ = 0_lcount;
    maxLength_ = 0_length;
}

void ConcatenatedLogData::addSource( std::shared_ptr<LogData> source )
{
    if ( !source ) {
        LOG_WARNING << "Ignoring null source in ConcatenatedLogData::addSource";
        return;
    }

    SourceInfo sourceInfo;
    sourceInfo.logData = std::move( source );

    // Connect to source signals for live updates.
    sourceInfo.loadingFinishedConnection
        = connect( sourceInfo.logData.get(), &LogData::loadingFinished, this,
                   [ this ]( LoadingStatus status ) {
                       rebuildIndex();
                       Q_EMIT loadingFinished( status );
                   } );
    sourceInfo.fileChangedConnection
        = connect( sourceInfo.logData.get(), &LogData::fileChanged, this,
                   [ this ]( MonitoredFileStatus status ) {
                       rebuildIndex();
                       Q_EMIT fileChanged( status );
                   } );

    sourceInfo.interpretationChangedConnection
        = connect( sourceInfo.logData.get(), &AbstractLogData::interpretationChanged,
                   this, [ this ] { notifyInterpretationChanged(); } );

    sources_.push_back( std::move( sourceInfo ) );
    rebuildIndex();
}

void ConcatenatedLogData::rebuildIndex()
{
    totalLines_ = 0_lcount;
    maxLength_ = 0_length;

    for ( auto& info : sources_ ) {
        if ( !info.logData ) {
            info.cumulativeLines = totalLines_;
            continue;
        }

        totalLines_ += info.logData->getNbLine();
        info.cumulativeLines = totalLines_;
        maxLength_ = qMax( maxLength_, info.logData->getMaxLength() );
    }
}

int ConcatenatedLogData::sourceCount() const
{
    return static_cast<int>( sources_.size() );
}

std::pair<int, LineNumber> ConcatenatedLogData::mapToSource( LineNumber globalLine ) const
{
    LineNumber::UnderlyingType offset = 0;
    for ( size_t i = 0; i < sources_.size(); ++i ) {
        const auto sourceLines = sources_[ i ].cumulativeLines.get();
        if ( globalLine.get() < sourceLines ) {
            return { static_cast<int>( i ), LineNumber( globalLine.get() - offset ) };
        }
        offset = sourceLines;
    }
    // Should not happen, but return last source's last line
    if ( !sources_.empty() ) {
        const auto lastIdx = sources_.size() - 1;
        const auto prevCum = lastIdx > 0 ? sources_[ lastIdx - 1 ].cumulativeLines.get() : 0;
        return { static_cast<int>( lastIdx ), LineNumber( globalLine.get() - prevCum ) };
    }
    return { 0, 0_lnum };
}

std::shared_ptr<LogData> ConcatenatedLogData::sourceAt( int index ) const
{
    if ( index < 0 || static_cast<size_t>( index ) >= sources_.size() ) {
        return {};
    }

    return sources_[ static_cast<size_t>( index ) ].logData;
}

klogg::LogSnapshot::Ptr
ConcatenatedLogData::createReadSnapshot( klogg::SnapshotScope scope ) const
{
    std::vector<ConcatenatedReadSnapshot::Source> frozen;
    frozen.reserve( sources_.size() );
    LinesCount previousEnd = 0_lcount;
    for ( const auto& source : sources_ ) {
        auto snapshot = source.logData->createReadSnapshot( scope );
        // Do not silently omit unready sources or substitute a newer row layout
        // before rebuildIndex has made it the presented concatenation.
        if ( !snapshot || snapshot->lineCount() != source.cumulativeLines - previousEnd ) {
            return {};
        }
        previousEnd = source.cumulativeLines;
        frozen.push_back( { std::move( snapshot ), 0, {}, 0_lcount } );
    }

    for ( size_t index = 0; index < frozen.size(); ++index ) {
        auto& target = frozen[ index ];
        if ( target.snapshot->lineCount() == 0_lcount ) {
            continue;
        }
        const auto identity = target.snapshot->row( 0_lnum ).identity;
        const auto& source = sources_[ index ];
        if ( source.snapshotIncarnation == 0
             || source.snapshotSourceIdentity.origin != identity.origin
             || source.snapshotSourceIdentity.incarnation != identity.incarnation ) {
            const auto nextIncarnation
                = nextConcatenatedIncarnation.fetch_add( 1, std::memory_order_relaxed );
            // Allocate the inverse entry before committing the cached identity.
            // An allocation failure must leave the previous mapping usable.
            snapshotSources_.emplace( nextIncarnation, index );
            snapshotSources_.erase( source.snapshotIncarnation );
            source.snapshotIncarnation = nextIncarnation;
            source.snapshotSourceIdentity = identity;
        }
        target.incarnation = source.snapshotIncarnation;
        target.childIdentity = identity;
    }
    return std::make_shared<ConcatenatedReadSnapshot>( std::move( frozen ) );
}

OptionalLineNumber ConcatenatedLogData::rowForIdentity( const klogg::LogRowId& identity ) const
{
    if ( identity.origin != klogg::LogRowId::Origin::Concatenated ) {
        return {};
    }
    const auto found = snapshotSources_.find( identity.incarnation );
    if ( found == snapshotSources_.end() ) {
        return {};
    }
    const auto index = found->second;
    const auto& source = sources_[ index ];
    auto childIdentity = identity;
    childIdentity.origin = source.snapshotSourceIdentity.origin;
    childIdentity.incarnation = source.snapshotSourceIdentity.incarnation;
    const auto local = source.logData->rowForIdentity( childIdentity );
    const auto start = index > 0 ? sources_[ index - 1 ].cumulativeLines.get() : 0;
    if ( !local || local->get() >= source.cumulativeLines.get() - start ) {
        return {};
    }
    return LineNumber( start + local->get() );
}

QString ConcatenatedLogData::doGetLineString( LineNumber line ) const
{
    if ( sources_.empty() || line.get() >= totalLines_.get() ) {
        return {};
    }

    const auto [ srcIdx, localLine ] = mapToSource( line );
    const auto source = sourceAt( srcIdx );
    return source ? source->getLineString( localLine ) : QString{};
}

QString ConcatenatedLogData::doGetExpandedLineString( LineNumber line ) const
{
    if ( sources_.empty() || line.get() >= totalLines_.get() ) {
        return {};
    }

    const auto [ srcIdx, localLine ] = mapToSource( line );
    const auto source = sourceAt( srcIdx );
    return source ? source->getExpandedLineString( localLine ) : QString{};
}

klogg::vector<AnsiColorSpan> ConcatenatedLogData::doGetLineAnsiColors( LineNumber line ) const
{
    if ( sources_.empty() || line.get() >= totalLines_.get() ) {
        return {};
    }

    const auto [ srcIdx, localLine ] = mapToSource( line );
    const auto source = sourceAt( srcIdx );
    return source ? source->getLineAnsiColors( localLine ) : klogg::vector<AnsiColorSpan>{};
}

klogg::vector<QString> ConcatenatedLogData::doGetLines( LineNumber first, LinesCount number ) const
{
    klogg::vector<QString> result;
    result.reserve( number.get() );

    for ( LinesCount::UnderlyingType i = 0; i < number.get(); ++i ) {
        result.push_back( doGetLineString( first + LinesCount( i ) ) );
    }
    return result;
}

klogg::vector<QString> ConcatenatedLogData::doGetExpandedLines( LineNumber first,
                                                                 LinesCount number ) const
{
    klogg::vector<QString> result;
    result.reserve( number.get() );

    for ( LinesCount::UnderlyingType i = 0; i < number.get(); ++i ) {
        result.push_back( doGetExpandedLineString( first + LinesCount( i ) ) );
    }
    return result;
}

LineNumber ConcatenatedLogData::doGetLineNumber( LineNumber index ) const
{
    return index;
}

LinesCount ConcatenatedLogData::doGetNbLine() const
{
    return totalLines_;
}

LineLength ConcatenatedLogData::doGetMaxLength() const
{
    return maxLength_;
}

LineLength ConcatenatedLogData::doGetLineLength( LineNumber line ) const
{
    if ( sources_.empty() || line.get() >= totalLines_.get() ) {
        return 0_length;
    }

    const auto [ srcIdx, localLine ] = mapToSource( line );
    const auto source = sourceAt( srcIdx );
    return source ? source->getLineLength( localLine ) : 0_length;
}

void ConcatenatedLogData::doSetDisplayEncoding( const char* encoding )
{
    for ( auto& info : sources_ ) {
        if ( info.logData ) {
            info.logData->setDisplayEncoding( encoding );
        }
    }
}

QTextCodec* ConcatenatedLogData::doGetDisplayEncoding() const
{
    for ( const auto& info : sources_ ) {
        if ( info.logData ) {
            return info.logData->getDisplayEncoding();
        }
    }

    return nullptr;
}

void ConcatenatedLogData::doAttachReader() const
{
    for ( const auto& info : sources_ ) {
        if ( info.logData ) {
            info.logData->attachReader();
        }
    }
}

void ConcatenatedLogData::doDetachReader() const
{
    for ( const auto& info : sources_ ) {
        if ( info.logData ) {
            info.logData->detachReader();
        }
    }
}
