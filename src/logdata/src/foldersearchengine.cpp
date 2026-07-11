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
 * but WITHOUT ANY WARRANTY; even without implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "foldersearchengine.h"

#include <QFile>

#include <algorithm>
#include <string_view>
#include <utility>

#include "foldersearchtypes.h"
#include "linetypes.h"
#include "log.h"
#include "regularexpression.h"

namespace {

// True if `shouldStop` is set and reports stop. A null predicate never stops.
bool stopped( const std::function<bool()>& shouldStop )
{
    return shouldStop && shouldStop();
}

} // namespace

FolderSearchEngine::FolderSearchEngine( QObject* parent )
    : QObject( parent )
{
    workerThread_ = std::thread( [ this ] { workerLoop(); } );
}

FolderSearchEngine::~FolderSearchEngine()
{
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        shutdown_ = true;
        pendingRequest_.valid = false;
    }
    interruptRequested_ = true;
    requestCv_.notify_all();
    if ( workerThread_.joinable() ) {
        workerThread_.join();
    }
}

quint64 FolderSearchEngine::startSearch( const QStringList& filePaths,
                                         const RegularExpressionPattern& pattern )
{
    const quint64 gen = generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
    interruptRequested_ = false;
    {
        std::lock_guard<std::mutex> lock( requestMutex_ );
        pendingRequest_ = Request{ gen, filePaths, pattern, true };
    }
    requestCv_.notify_one();
    return gen;
}

quint64 FolderSearchEngine::scanSynchronously( const QStringList& filePaths,
                                               const RegularExpressionPattern& pattern )
{
    const quint64 gen = generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
    interruptRequested_ = false;
    runSearch( gen, filePaths, pattern );
    return gen;
}

quint64 FolderSearchEngine::bumpGeneration()
{
    return generation_.fetch_add( 1, std::memory_order_relaxed ) + 1;
}

void FolderSearchEngine::interrupt()
{
    interruptRequested_ = true;
    std::lock_guard<std::mutex> lock( requestMutex_ );
    pendingRequest_.valid = false;
}

std::vector<klogg::folder::FileGroup> FolderSearchEngine::takeResults()
{
    std::lock_guard<std::mutex> lock( resultsMutex_ );
    std::vector<klogg::folder::FileGroup> out;
    out.swap( results_ );
    return out;
}

void FolderSearchEngine::runSearch( quint64 gen, const QStringList& filePaths,
                                    const RegularExpressionPattern& pattern )
{
    // A queued request whose generation is no longer current must do nothing.
    if ( generation_.load( std::memory_order_relaxed ) != gen ) {
        Q_EMIT searchFinished( gen );
        return;
    }

    RegularExpression expression( pattern );
    if ( !expression.isValid() ) {
        LOG_WARNING << "FolderSearchEngine: invalid pattern" << pattern.pattern;
        Q_EMIT searchFinished( gen );
        return;
    }
    auto matcher = expression.createMatcher();

    {
        std::lock_guard<std::mutex> lock( resultsMutex_ );
        results_.clear();
    }
    matchCount_ = 0;

    Q_EMIT searchStarted( gen );

    const int total = static_cast<int>( filePaths.size() );
    for ( int i = 0; i < total; ++i ) {
        if ( generation_.load( std::memory_order_relaxed ) != gen
             || interruptRequested_.load( std::memory_order_relaxed ) ) {
            break;
        }

        const auto shouldStop = [this, gen]() {
            return generation_.load( std::memory_order_relaxed ) != gen
                   || interruptRequested_.load( std::memory_order_relaxed );
        };

        auto group = scanFile( filePaths[ i ], *matcher, DefaultBlockSize, shouldStop );

        if ( !group.matches.empty() ) {
            matchCount_.fetch_add( group.matches.size(), std::memory_order_relaxed );
            std::lock_guard<std::mutex> lock( resultsMutex_ );
            results_.push_back( std::move( group ) );
        }

        const int percent = total > 0 ? static_cast<int>( ( i + 1 ) * 100 / total ) : 100;
        Q_EMIT searchProgressed( matchCount_.load( std::memory_order_relaxed ), percent, gen );
    }

    Q_EMIT searchFinished( gen );
}

void FolderSearchEngine::workerLoop()
{
    while ( true ) {
        Request req;
        {
            std::unique_lock<std::mutex> lock( requestMutex_ );
            requestCv_.wait( lock, [ this ]() {
                return shutdown_.load( std::memory_order_relaxed ) || pendingRequest_.valid;
            } );
            if ( shutdown_.load( std::memory_order_relaxed ) ) {
                return;
            }
            req = std::move( pendingRequest_ );
            pendingRequest_.valid = false;
        }
        runSearch( req.generation, req.filePaths, req.pattern );
    }
}

klogg::folder::FileGroup FolderSearchEngine::scanFile( const QString& path,
                                                       const PatternMatcher& matcher,
                                                       qint64 blockSize,
                                                       std::function<bool()> shouldStop )
{
    klogg::folder::FileGroup group;
    group.filePath = path;

    if ( blockSize <= 0 ) {
        blockSize = DefaultBlockSize;
    }

    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly | QIODevice::Unbuffered ) ) {
        return group; // unreadable -> no matches
    }

    OffsetInFile fileOffset = 0_offset; // absolute byte offset of block[0]
    OffsetInFile lineStartAbs = 0_offset; // absolute byte offset of the current line's start
    LineNumber localLine = 0_lnum; // 0-based line counter within the file
    QByteArray carry; // bytes of the current line carried across a block boundary
    bool firstBlock = true;

    QByteArray block = file.read( blockSize );
    while ( !block.isEmpty() ) {
        if ( firstBlock ) {
            firstBlock = false;
            // grep -I: a NUL byte in the opening window means "binary" -> skip.
            const int window
                = std::min<int>( static_cast<int>( block.size() ), BinaryDetectionWindow );
            if ( block.left( window ).contains( '\0' ) ) {
                return group;
            }
        }

        const char* const data = block.constData();
        const int size = static_cast<int>( block.size() );
        int scanFrom = 0;
        while ( true ) {
            const int nl = static_cast<int>( block.indexOf( '\n', scanFrom ) );
            if ( nl < 0 ) {
                // No more newlines in this block: the remainder is a continuation
                // of the still-incomplete current line. Append it to `carry` and
                // read the next block. Do NOT evaluate here -- the line is not
                // complete; it is evaluated when a newline completes it, or at
                // EOF if the file ends without a trailing newline.
                carry += block.mid( scanFrom, size - scanFrom );
                break;
            }

            // Complete line found: its content is carry + block[scanFrom, nl).
            // For the common case (line fits in one block) `carry` is empty and
            // `lineView` is a zero-copy view into `block`; only boundary-spanning
            // lines need the temporary `joined` buffer.
            std::string_view lineView;
            QByteArray joined;
            if ( carry.isEmpty() ) {
                lineView = std::string_view( data + scanFrom, static_cast<size_t>( nl - scanFrom ) );
            }
            else {
                joined = carry + QByteArray::fromRawData( data + scanFrom, nl - scanFrom );
                lineView = std::string_view( joined.constData(), static_cast<size_t>( joined.size() ) );
            }

            if ( matcher.hasMatch( lineView ) ) {
                klogg::folder::MatchRecord m;
                m.localLine = localLine;
                m.lineStartByte = lineStartAbs;
                m.lineEndByte = OffsetInFile( fileOffset.get() + nl + 1 );
                m.lineLength = getUntabifiedLength( lineView );
                m.matchLen = 0_length;
                group.matches.push_back( std::move( m ) );
            }

            ++localLine;
            carry.clear();
            lineStartAbs = OffsetInFile( fileOffset.get() + nl + 1 );
            scanFrom = nl + 1;

            if ( stopped( shouldStop ) ) {
                return group;
            }
        }

        fileOffset = OffsetInFile( fileOffset.get() + size );

        if ( stopped( shouldStop ) ) {
            return group;
        }
        block = file.read( blockSize );
    }

    // Trailing line without a final newline (left over in `carry`).
    if ( !carry.isEmpty() ) {
        const std::string_view lineView( carry.constData(), static_cast<size_t>( carry.size() ) );
        if ( matcher.hasMatch( lineView ) ) {
            klogg::folder::MatchRecord m;
            m.localLine = localLine;
            m.lineStartByte = lineStartAbs;
            m.lineEndByte = fileOffset; // EOF
            m.lineLength = getUntabifiedLength( lineView );
            m.matchLen = 0_length;
            group.matches.push_back( std::move( m ) );
        }
    }

    return group;
}
