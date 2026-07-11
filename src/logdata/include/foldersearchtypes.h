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

#include "linekind.h"
#include "linetypes.h"

class QTextCodec;

namespace klogg::folder {

// Identifies a source file within a FolderSearchResults set. Stable for the
// lifetime of a result set: it is the index of the file in the (natural-sorted)
// group list, which is also the order results are displayed.
using FileId = int;

// One matched line, as captured by the streaming search engine.
//
// Storage is deliberately offset-based rather than holding the line text: the
// engine streams file bytes and records the byte offset of each match for free,
// so a result set of N matches costs ~N * sizeof(MatchRecord) bytes (not N line
// texts). FolderSearchResults fetches the visible line text on demand by
// seeking to lineStartByte and reading to the next newline.
struct MatchRecord {
    // 0-based line number within the source file (1-based display adds +1).
    LineNumber localLine = 0_lnum;
    // Byte range of the matched line in the source file: [lineStartByte, lineEndByte).
    // lineEndByte sits on the trailing newline (or at EOF). Lets the results
    // model fetch the exact line text by a single bounded seek+read, with no
    // newline scanning at display time.
    OffsetInFile lineStartByte = 0_offset;
    OffsetInFile lineEndByte = 0_offset;
    // Tab-expanded visible length of the whole matched line (for the horizontal
    // scrollbar / column math, without having to re-read the line).
    LineLength lineLength = 0_length;
    // Length of the matched substring within the line (for match highlighting).
    LineLength matchLen = 0_length;
};

// All matches from one source file. Files with zero matches are never emitted
// (they are hidden from results), so every FileGroup has matches.size() >= 1.
struct FileGroup {
    QString filePath;
    std::vector<MatchRecord> matches; // ordered by localLine ascending
    // Source-file codec detected on the first block (mirrors the indexer's
    // guessEncoding). Used by FolderSearchResults::readMatchLine to decode the
    // fetched match bytes with the SAME codec that interpreted the file during
    // the scan, rather than the view-layer display encoding. Nullptr means
    // UTF-8/ASCII (the common fast path). Qt owns codec singletons, so a raw
    // pointer is safe (matches IndexingData in logdataworker.h).
    QTextCodec* sourceCodec = nullptr;
};

// Resolves a visible result row back to its origin (used by the filtered view
// to display the line number and to open the source file in the main view).
struct SourceRef {
    QString filePath;
    LineNumber localLine = 0_lnum;
};

} // namespace klogg::folder

#endif // FOLDERSEARCHTYPES_H
