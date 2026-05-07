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

ConcatenatedLogData::ConcatenatedLogData()
    : AbstractLogData()
{
}

ConcatenatedLogData::~ConcatenatedLogData()
{
    for ( auto& info : sources_ ) {
        QObject::disconnect( info.loadingFinishedConnection );
        QObject::disconnect( info.fileChangedConnection );
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
