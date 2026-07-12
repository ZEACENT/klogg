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

// Folder-wide search benchmark: compares klogg's streaming folder search
// (FolderSearchEngine, no byte-offset index) against `grep -EIrn` over the same
// multi-file tree, and asserts match-count parity.
//
// Build:  cmake --build build_root --target folder_search_benchmark
// Run:    ./folder_search_benchmark --files 200 --lines-per-file 4000
//           --pattern 'NEEDLE' --vs-grep

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QTextStream>
#include <QTimer>

#include "foldersearchengine.h"
#include "foldersearchresults.h"
#include "foldersearchtypes.h"
#include "configuration.h"
#include "linetypes.h"
#include "persistentinfo.h"
#include "regularexpressionpattern.h"

const bool PersistentInfo::ForcePortable = true;

namespace {

// Write a deterministic fixture file: `lines` lines, every `matchEvery`-th line
// embeds the pattern so the expected match count is lines/matchEvery.
qint64 writeFixtureFile( const QString& path, qint64 lines, const QString& pattern, qint64 matchEvery )
{
    QFile f( path );
    if ( !f.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        return 0;
    }
    qint64 bytes = 0;
    QByteArray buffer;
    buffer.reserve( 1 << 20 );
    for ( qint64 i = 0; i < lines; ++i ) {
        if ( i % matchEvery == 0 ) {
            buffer += "line " + QByteArray::number( i ) + " hit " + pattern.toUtf8()
                      + " payload abcdefghijklmnop\n";
        }
        else {
            buffer += "line " + QByteArray::number( i ) + " ordinary payload xyz\n";
        }
        if ( buffer.size() >= ( 1 << 20 ) ) {
            bytes += f.write( buffer );
            buffer.clear();
            buffer.reserve( 1 << 20 );
        }
    }
    if ( !buffer.isEmpty() ) {
        bytes += f.write( buffer );
    }
    f.close();
    return bytes;
}

quint64 countMatches( const std::vector<klogg::folder::FileGroup>& groups )
{
    quint64 total = 0;
    for ( const auto& g : groups ) {
        total += static_cast<quint64>( g.matches.size() );
    }
    return total;
}

} // namespace

int main( int argc, char* argv[] )
{
    QCoreApplication app( argc, argv );
    QCoreApplication::setApplicationName( "folder_search_benchmark" );

    // Select the regex engine (PatternMatcher reads the global Configuration).
    auto& config = Configuration::getSynced();
#ifdef KLOGG_HAS_VECTORSCAN
    config.setRegexpEnging( RegexpEngine::Vectorscan );
#else
    config.setRegexpEnging( RegexpEngine::QRegularExpression );
#endif

    const QStringList args = QCoreApplication::arguments();
    auto valueOf = [ &args ]( const QString& key, const QString& fallback ) -> QString {
        const int idx = static_cast<int>( args.indexOf( key ) );
        const int size = static_cast<int>( args.size() );
        return ( idx >= 0 && idx + 1 < size ) ? args[ idx + 1 ] : fallback;
    };
    auto hasFlag = [ &args ]( const QString& key ) -> bool {
        return args.indexOf( key ) >= 0;
    };

    const int files = valueOf( "--files", "100" ).toInt();
    const qint64 linesPerFile = valueOf( "--lines-per-file", "4000" ).toLongLong();
    const QString pattern = valueOf( "--pattern", "NEEDLE" );
    const QString tmpDir = valueOf( "--tmp-dir", QDir::tempPath() );
    const int iterations = valueOf( "--iterations", "3" ).toInt();
    const qint64 matchEvery = valueOf( "--match-every", "1000" ).toLongLong();
    const bool vsGrep = hasFlag( "--vs-grep" );

    // --- fixture ---
    const QString root = QDir( tmpDir ).filePath( "klogg_folder_bench" );
    QDir( root ).removeRecursively();
    QDir{}.mkpath( root );

    std::vector<QString> filePaths;
    filePaths.reserve( static_cast<size_t>( files ) );
    qint64 totalBytes = 0;
    for ( int i = 0; i < files; ++i ) {
        const QString path = QDir( root ).filePath( QString( "file_%1.log" ).arg( i, 4, 10, QChar( '0' ) ) );
        totalBytes += writeFixtureFile( path, linesPerFile, pattern, matchEvery );
        filePaths.push_back( path );
    }
    const quint64 expectedMatches = static_cast<quint64>( files ) * static_cast<quint64>( linesPerFile / matchEvery );
    const double totalMiB = static_cast<double>( totalBytes ) / ( 1024.0 * 1024.0 );

    QTextStream out( stdout );
    out << "Folder benchmark: " << files << " files x " << linesPerFile << " lines = "
        << totalMiB << " MiB, pattern '" << pattern << "'\n";
    out << "Expected matches: " << expectedMatches << "\n\n";

    QStringList filePathsQt;
    filePathsQt.reserve( static_cast<int>( filePaths.size() ) );
    for ( const auto& p : filePaths ) {
        filePathsQt << p;
    }

    // --- klogg streaming folder search ---
    quint64 kloggMatches = 0;
    double kloggSecsBest = 1e9;
    for ( int it = 0; it < iterations; ++it ) {
        FolderSearchEngine engine;
        QElapsedTimer t;
        t.start();
        engine.scanSynchronously( filePathsQt, RegularExpressionPattern{ pattern } );
        const double secs = static_cast<double>( t.elapsed() ) / 1000.0;
        const auto groups = engine.takeResults();
        kloggMatches = countMatches( groups );
        kloggSecsBest = std::min( kloggSecsBest, secs );
        out << "  klogg run " << ( it + 1 ) << ": " << secs << " s, " << kloggMatches << " matches\n";
    }
    out << "\nklogg best: " << kloggSecsBest << " s  (" << ( totalMiB / kloggSecsBest )
        << " MiB/s)\n";

    // --- grep comparison ---
    if ( vsGrep ) {
        double grepSecsBest = 1e9;
        quint64 grepMatches = 0;
        for ( int it = 0; it < iterations; ++it ) {
            QElapsedTimer t;
            t.start();
            QProcess proc;
            proc.setWorkingDirectory( root );
            proc.start( "grep", QStringList{ "-EIrn", pattern, "." } );
            if ( !proc.waitForFinished( 10 * 60 * 1000 ) ) {
                out << "grep timed out\n";
                return 2;
            }
            const double secs = static_cast<double>( t.elapsed() ) / 1000.0;
            const QByteArray out0 = proc.readAllStandardOutput();
            grepMatches = static_cast<quint64>( out0.count( '\n' ) );
            grepSecsBest = std::min( grepSecsBest, secs );
            out << "  grep  run " << ( it + 1 ) << ": " << secs << " s, " << grepMatches << " matches\n";
        }
        out << "\ngrep  best: " << grepSecsBest << " s  (" << ( totalMiB / grepSecsBest )
            << " MiB/s)\n";

        out << "\nSpeedup (klogg / grep): " << ( grepSecsBest / kloggSecsBest ) << "x\n";
        if ( kloggMatches != grepMatches ) {
            out << "\nMATCH COUNT MISMATCH: klogg=" << kloggMatches << " grep=" << grepMatches << "\n";
            return 3;
        }
        out << "Match-count parity: OK (" << kloggMatches << ")\n";
    }

    QDir( root ).removeRecursively();
    return 0;
}
