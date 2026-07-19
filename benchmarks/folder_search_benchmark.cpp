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
// (FolderSearchEngine, no byte-offset index) against `grep -EIrn` and `rg`
// (ripgrep) over the same multi-file tree, and asserts match-count parity.
//
// Build:  cmake --build build_root --target folder_search_benchmark
// Run:    ./folder_search_benchmark --files 200 --lines-per-file 4000
//           --pattern 'NEEDLE' --vs-grep --vs-rg

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
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
    // Explicitly enable the block-scan fast path: this benchmark measures it, so
    // do not let a persisted perf.useBlockScan=false (e.g. from the main app's
    // QSettings, which getSynced() re-reads) silently switch to per-line.
    // --block-scan off reproduces that persisted-off state for comparison.
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
    const QString scanRoot = valueOf( "--root", QString{} );
    const bool ignoreCase = hasFlag( "--ignore-case" );
    const int iterations = valueOf( "--iterations", "3" ).toInt();
    const qint64 matchEvery = valueOf( "--match-every", "1000" ).toLongLong();
    const bool vsGrep = hasFlag( "--vs-grep" );
    const bool vsRg = hasFlag( "--vs-rg" );
    config.setUseBlockScan( valueOf( "--block-scan", "on" ) != QLatin1String( "off" ) );

    // --root <dir>: scan an existing tree recursively (grep -r semantics: all
    // regular files, no symlink following) instead of generating a fixture.
    if ( !scanRoot.isEmpty() ) {
        QStringList filePathsQt;
        qint64 totalBytes = 0;
        QDirIterator it( scanRoot, QDir::Files, QDirIterator::Subdirectories );
        while ( it.hasNext() ) {
            const QString path = it.next();
            filePathsQt << path;
            totalBytes += QFileInfo( path ).size();
        }
        const double totalMiB = static_cast<double>( totalBytes ) / ( 1024.0 * 1024.0 );

        QTextStream out( stdout );
        out << "Folder benchmark (real tree): " << filePathsQt.size() << " files = " << totalMiB
            << " MiB, pattern '" << pattern << "'"
            << ( ignoreCase ? " [ignore-case]" : "" )
            << ( config.useBlockScan() ? " [block-scan]" : " [per-line]" ) << "\n";

        quint64 kloggMatches = 0;
        double kloggSecsBest = 1e9;
        for ( int it2 = 0; it2 < iterations; ++it2 ) {
            FolderSearchEngine engine;
            QElapsedTimer t;
            t.start();
            engine.scanSynchronously(
                filePathsQt,
                RegularExpressionPattern{ pattern, !ignoreCase, false, false, false } );
            const double secs = static_cast<double>( t.elapsed() ) / 1000.0;
            const auto groups = engine.takeResults();
            kloggMatches = countMatches( groups );
            kloggSecsBest = std::min( kloggSecsBest, secs );
            out << "  klogg run " << ( it2 + 1 ) << ": " << secs << " s, " << kloggMatches
                << " matches\n";
        }
        out << "\nklogg best: " << kloggSecsBest << " s  (" << ( totalMiB / kloggSecsBest )
            << " MiB/s)\n";
        return 0;
    }

    if ( files <= 0 || linesPerFile <= 0 || iterations <= 0 || matchEvery <= 0 ) {
        std::fprintf( stderr,
                      "--files, --lines-per-file, --iterations, and --match-every must be positive\n" );
        return 1;
    }

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
    // The fixture embeds the pattern on line 0 and every matchEvery-th line
    // thereafter, so each file has floor((lines-1)/matchEvery)+1 matches (the
    // naive lines/matchEvery undercounts when the division has a remainder).
    const quint64 matchesPerFile = static_cast<quint64>( ( linesPerFile - 1 ) / matchEvery ) + 1;
    const quint64 expectedMatches = static_cast<quint64>( files ) * matchesPerFile;
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

    // --- external grep-like tool comparison (grep / ripgrep) ---
    struct ToolResult {
        bool ran = false;
        bool found = true; // false if the tool binary is not on PATH
        double bestSecs = 1e9;
        quint64 matches = 0;
    };
    auto runTool = [ & ]( const QString& program, const QStringList& toolArgs ) -> ToolResult {
        ToolResult r;
        for ( int it = 0; it < iterations; ++it ) {
            QElapsedTimer t;
            t.start();
            QProcess proc;
            proc.setWorkingDirectory( root );
            proc.start( program, toolArgs );
            if ( !proc.waitForStarted( 30000 ) ) {
                out << program << " not available (" << proc.errorString() << ")\n";
                r.found = false;
                return r;
            }
            if ( !proc.waitForFinished( 10 * 60 * 1000 ) ) {
                out << program << " timed out\n";
                // Terminate the runaway child so it is not left running after
                // the benchmark returns (waitForFinished timed out -> the
                // QProcess destructor would otherwise kill it only on teardown).
                proc.kill();
                proc.waitForFinished( 5000 );
                return r;
            }
            const double secs = static_cast<double>( t.elapsed() ) / 1000.0;
            const QByteArray out0 = proc.readAllStandardOutput();
            r.matches = static_cast<quint64>( out0.count( '\n' ) );
            r.bestSecs = std::min( r.bestSecs, secs );
            out << "  " << program << " run " << ( it + 1 ) << ": " << secs << " s, " << r.matches
                << " matches\n";
        }
        r.ran = true;
        return r;
    };

    ToolResult grepRes;
    if ( vsGrep ) {
        out << "\n";
        grepRes = runTool( QStringLiteral( "grep" ),
                           QStringList{ "-EIrn", "--", pattern, "." } );
        if ( grepRes.ran ) {
            out << "grep  best: " << grepRes.bestSecs << " s  ("
                << ( totalMiB / grepRes.bestSecs ) << " MiB/s)\n";
        }
    }

    // ripgrep: recursive by default; --no-ignore/--hidden match grep's unconditional
    // walk; --color never keeps stdout parseable. Each printed line is one match.
    ToolResult rgRes;
    if ( vsRg ) {
        out << "\n";
        rgRes = runTool( QStringLiteral( "rg" ),
                         QStringList{ "--no-ignore", "--hidden", "--color", "never", "--", pattern,
                                      "." } );
        if ( rgRes.ran ) {
            out << "rg    best: " << rgRes.bestSecs << " s  (" << ( totalMiB / rgRes.bestSecs )
                << " MiB/s)\n";
        }
    }

    // --- summary + parity ---
    out << "\n--- summary ---\n";
    out << "klogg: " << kloggSecsBest << " s  (" << ( totalMiB / kloggSecsBest ) << " MiB/s)\n";
    if ( vsGrep && grepRes.ran ) {
        out << "grep:  " << grepRes.bestSecs << " s  (" << ( totalMiB / grepRes.bestSecs )
            << " MiB/s)   klogg/grep = " << ( grepRes.bestSecs / kloggSecsBest ) << "x\n";
        if ( kloggMatches != grepRes.matches ) {
            out << "MATCH MISMATCH: klogg=" << kloggMatches << " grep=" << grepRes.matches << "\n";
            return 3;
        }
    }
    if ( vsRg && rgRes.ran ) {
        out << "rg:    " << rgRes.bestSecs << " s  (" << ( totalMiB / rgRes.bestSecs )
            << " MiB/s)   klogg/rg = " << ( rgRes.bestSecs / kloggSecsBest ) << "x\n";
        if ( kloggMatches != rgRes.matches ) {
            out << "MATCH MISMATCH: klogg=" << kloggMatches << " rg=" << rgRes.matches << "\n";
            return 3;
        }
    }
    out << "Match-count parity: OK (" << kloggMatches << ")\n";

    QDir( root ).removeRecursively();
    return 0;
}
