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
 * but WITHOUT ANY WARRANTY; even without the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FOLDERSEARCHTYPES_H
#define FOLDERSEARCHTYPES_H

#include <QString>
#include <vector>

#include "folderscanarena.h"
#include "linekind.h"
#include "linetypes.h"

class QTextCodec;

namespace klogg::folder {

// Identifies a source file within a FolderSearchResults set. Stable for the
// lifetime of a result set: it is the index of the file in the (natural-sorted)
// group list, which is also the order results are displayed.
using FileId = int;

// Distinguishes a real match row from a grep -A/-B/-C context row. Both live in
// FileGroup::matches (one flat, localLine-sorted, deduplicated vector); the role
// drives rendering (context rows have no Match bullet) and counting (only Match
// rows count toward the header total, matchLinesForFile, and the engine match
// count). Default Match keeps every legacy emission/test unchanged.
enum class RecordRole : unsigned char {
    Match,
    Context,
};

// grep -A/-B/-C context-window selection, captured at scan time. before = -B
// (lines before each match), after = -A (lines after). Both default 0 (no
// context, the legacy behavior). Context is a SCAN-TIME property of folder
// search (there is no line-number index, so a context line's text can only be
// fetched later by the byte offset recorded while streaming); changing the
// window requires a full re-scan via startSearch. Context never crosses a file
// boundary (each file is an independent scanFile stream).
struct ContextOptions {
    int before = 0;
    int after = 0;
};

// One matched line, as captured by the streaming search engine.
//
// Records remain offset-based rather than retaining decoded strings. The source
// lease owns the scan's immutable raw bytes in a scan-wide bounded/spilled arena;
// visible text is decoded on demand from these exact byte ranges.
struct MatchRecord {
    // 0-based line number within the source file (1-based display adds +1).
    LineNumber localLine = 0_lnum;
    // Byte range of the matched line in the source file: [lineStartByte, lineEndByte).
    // lineEndByte sits after the trailing newline (or at EOF). Lets the results
    // model fetch the exact line text by a single bounded seek+read, with no
    // newline scanning at display time.
    OffsetInFile lineStartByte = 0_offset;
    OffsetInFile lineEndByte = 0_offset;
    // Tab-expanded visible length of the whole matched line (for the horizontal
    // scrollbar / column math, without having to re-read the line).
    LineLength lineLength = 0_length;
    // Length of the matched substring within the line (for match highlighting).
    LineLength matchLen = 0_length;
    // Match vs Context (grep -A/-B/-C). LAST field so aggregate init order is
    // {localLine, lineStartByte, lineEndByte, lineLength, matchLen, role}.
    RecordRole role = RecordRole::Match;
};

// A scanned source member. Zero-match groups have no result header, but their
// immutable content is retained by streaming consumers for marked source rows.
struct FileGroup {
    QString filePath;
    // Flat, sorted by localLine ascending, deduplicated; holds Match AND Context
    // records (grep -A/-B/-C). A line that is both is emitted once as Match.
    std::vector<MatchRecord> matches;
    // Source-file codec detected on the first block (mirrors the indexer's
    // guessEncoding). Used by FolderSearchResults::readMatchLine to decode the
    // fetched match bytes with the SAME codec that interpreted the file during
    // the scan, rather than the view-layer display encoding. Nullptr means
    // UTF-8/ASCII (the common fast path). Qt owns codec singletons, so a raw
    // pointer is safe (matches IndexingData in logdataworker.h).
    QTextCodec* sourceCodec = nullptr;
    // Immutable bytes consumed by this scan, including zero-match members used
    // by marks. FileId is presentation-only; this lease owns the stable identity.
    ScannedFileContent::Ptr content;
    // A storage failure must not turn a scanned member into a successful empty
    // result. Consumers reject snapshot acquisition even if no match was found.
    QString retentionError;
};

// Resolves a visible result row back to its origin (used by the filtered view
// to display the line number and to open the source file in the main view).
struct SourceRef {
    QString filePath;
    LineNumber localLine = 0_lnum;
};

} // namespace klogg::folder

#endif // FOLDERSEARCHTYPES_H
