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

#ifndef FOLDERSEARCHRESULTS_H
#define FOLDERSEARCHRESULTS_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QTextCodec>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "abstractlogdata.h"
#include "foldersearchtypes.h"
#include "synchronization.h"

class QFile;

// Presents folder-wide search matches to the filtered view as an
// AbstractLogData, with one Header row per source file followed by that file's
// match rows (VS-Code-style grouping). Header rows are ordinary "virtual
// lines" on the same uniform grid as match rows, so AbstractLogView's
// one-row-per-charHeight geometry stays intact.
//
// Collapsed file groups contribute only their Header row; their match rows are
// hidden from getNbLine/getLineString until expanded again. The Header text
// always reports the TOTAL match count for the group so the user can see what
// is hidden, regardless of collapse state.
//
// Line text for match rows is fetched on demand by seeking the source file to
// the recorded byte offset (see MatchRecord); it is never stored. Result sets
// are therefore cheap regardless of how many lines matched.
//
// THREAD SAFETY: all mutation still happens on the main thread (streaming
// commits, collapse, visibility), but READERS also run on the QuickFind
// worker (quickfind.cpp searches this model via QtConcurrent). Every public
// entry point therefore locks: mutators take the unique lock and emit
// layoutChanged only AFTER releasing it (receivers may re-enter getters),
// getters take the shared lock, and readMatchLine additionally serializes the
// shared per-group QFile cursors on fileIoMutex_ (lock order: dataMutex_ ->
// fileIoMutex_, never the reverse). Private helpers are lock-free; callers
// must already hold the appropriate lock.
class FolderSearchResults : public AbstractLogData {
    Q_OBJECT

  public:
    FolderSearchResults();
    ~FolderSearchResults() override;

    FolderSearchResults( const FolderSearchResults& ) = delete;
    FolderSearchResults& operator=( const FolderSearchResults& ) = delete;

    // Replace the full result set. Files with zero matches must already be
    // filtered out by the caller. Resets collapse state. Emits layoutChanged().
    void setResults( std::vector<klogg::folder::FileGroup> groups );

    // --- Streaming API (stable incremental display) ---
    // beginSearch resets for a streaming search and sizes the internal pending
    // buffer to the number of files in `expectedFileOrder`. Call once at the
    // start of a search. Emits layoutChanged().
    void beginSearch( const QStringList& expectedFileOrder );
    // Buffer `group` by its file index, then commit every consecutive completed
    // predecessor in enumeration order (advancing an internal cursor). This
    // guarantees display order always matches the natural-sorted enumeration,
    // regardless of which file finishes first. Empty groups advance the cursor
    // without creating a header (grep-style "no header for zero-match files").
    // Emits a single batched layoutChanged() if any groups were appended.
    void addFileGroup( int fileIndex, klogg::folder::FileGroup group );
    // Commit any remaining buffered groups, skipping gaps from files that never
    // arrived (interrupted/incomplete scan). Called at searchFinished so the
    // display never stalls. Emits layoutChanged() if anything was committed.
    void flushPending();

    // --- Folder-specific API (not part of AbstractLogData) ---

    LineKind lineKind( LineNumber visibleIndex ) const;

    // True iff the visible row is a real Match (not a grep -A/-B/-C Context row
    // and not a Header). Exposed so FolderFilteredView can suppress the Match
    // bullet on context rows without touching the records vector.
    bool isMatchRow( LineNumber visibleIndex ) const;

    // The source group (fileId) a visible row belongs to (Header or Match).
    klogg::folder::FileId fileIdForLine( LineNumber visibleIndex ) const;

    // Returns the source file + local line for a visible Match row. For a
    // Header row returns the group's filePath with localLine == 0.
    klogg::folder::SourceRef sourceForLine( LineNumber visibleIndex ) const;

    // All match local-line numbers for filePath (ascending, as stored), empty if
    // the file is not in the result set. Used by the folder main-view overview.
    // Reads groups_ (not visibleRows_), so it is unaffected by collapse state.
    std::vector<LineNumber> matchLinesForFile( const QString& filePath ) const;

    // Per-file display-encoding override (the user's Encoding-menu pick for the
    // file open in the main view): rows of that file decode with it instead of
    // the scan-time detected codec (single-file setEncoding parity). Emits
    // layoutChanged so the view re-renders.
    void setEncodingOverrideForFile( const QString& filePath, const QByteArray& encoding );
    void clearEncodingOverrideForFile( const QString& filePath );

    // The total number of matches in a group (always shown on the header, even
    // when collapsed). Returns 0 for an out-of-range fileId.
    klogg::folder::FileId groupCount() const;

    // The largest source-file local line number across all matches (0 if none).
    // Used by the folder filtered view to size the line-number gutter.
    LineNumber maxLocalLine() const;

    // --- Collapse management ---
    void toggleCollapse( klogg::folder::FileId fileId );
    void setCollapsed( klogg::folder::FileId fileId, bool collapsed );
    void collapseAll();
    void expandAll();
    bool isCollapsed( klogg::folder::FileId fileId ) const;

    // --- Visibility filter (parity with LogFilteredData::Visibility) ---
    // Every folder Data row is a match, so MarksAndMatches and Matches both show
    // all rows; Marks shows only the marked match rows (plus a header for each
    // group that has at least one marked row).
    enum class Visibility { MarksAndMatches, Marks, Matches };
    void setVisibility( Visibility visibility );
    Visibility visibility() const { return visibility_; }
    // Inject the per-file marks store (widget-owned, read LIVE during rebuild)
    // so the Marks / Marks-and-matches visibility can show marked lines that do
    // NOT match the current filter -- single-file LogFilteredData::marks_ parity.
    // Marks are (filePath, localLine) bookmarks independent of the transient
    // match set; the store is ENUMERATED (not just predicate-tested) so marked
    // non-match rows can be injected alongside match rows. The widget must
    // outlive the results object. Call before/with setVisibility(Marks).
    void setMarksStore( const QHash<QString, std::set<uint64_t>>* store );
    // Rebuild + emit layoutChanged. Called by the widget when marks change while
    // the Marks filter is active (the visible set depends on marks).
    void refreshForMarksChange();

  Q_SIGNALS:
    // Emitted whenever the visible-row layout changes (new results, collapse
    // toggle, collapse/expand all). The view responds with updateData() +
    // forceRefresh().
    void layoutChanged();

  protected:
    QString doGetLineString( LineNumber line ) const override;
    QString doGetExpandedLineString( LineNumber line ) const override;
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount number ) const override;
    klogg::vector<QString> doGetExpandedLines( LineNumber first, LinesCount number ) const override;
    LineNumber doGetLineNumber( LineNumber index ) const override;
    bool doIsLineCopyable( LineNumber index ) const override;
    LinesCount doGetNbLine() const override;
    LineLength doGetMaxLength() const override;
    LineLength doGetLineLength( LineNumber line ) const override;
    void doSetDisplayEncoding( const char* encoding ) override;
    QTextCodec* doGetDisplayEncoding() const override;
    void doAttachReader() const override;
    void doDetachReader() const override;

  private:
    struct VisibleRow {
        LineKind kind = LineKind::Data;
        // Injected marked non-match row: a bookmark on a source line that does
        // not match the current filter. Renders as a normal Data row but is not
        // a match (isMatchRow == false) and fetches text by (filePath, localLine).
        bool isMarkRow = false;
        klogg::folder::FileId fileId = 0;   // Header/Match: index into groups_; Mark: real group or marks-only (groups_.size() + idx)
        size_t matchIndex = 0;              // Match rows: index into groups_[fileId].matches
        LineNumber markLocalLine = 0_lnum;  // Mark rows: marked source line
        QString markFilePath;               // Mark rows: source file path
    };

    // A file that has marks but no match group (marks-only). Rebuilt each
    // rebuildVisibleRows from the injected marks store; shown under Marks /
    // Marks-and-matches so marked lines stay visible across filter changes.
    struct MarksGroup {
        QString filePath;
        std::vector<LineNumber> localLines; // sorted ascending
    };

    void rebuildVisibleRows();
    // Snapshot the filePaths of currently-collapsed groups (called at the top of
    // beginSearch, before groups_ is rebuilt) and re-apply collapse to a group as
    // it streams back in (called after each push_back). Keyed by filePath because
    // FileId is reassigned when groups_ is rebuilt (foldersearchtypes.h:33-36).
    void snapshotCollapsedPaths();
    void reapplyCollapseForLastGroup();
    bool isLineMarked( const QString& filePath, LineNumber localLine ) const;
    QString headerText( klogg::folder::FileId fileId ) const;
    QString marksGroupHeader( size_t marksGroupIndex ) const;
    QString readMatchLine( klogg::folder::FileId fileId, size_t matchIndex ) const;
    // Fetch the text of a marked source line by (filePath, localLine) using a
    // cached per-file line-offset index (marked non-match lines have no
    // MatchRecord offset). Results are cached per (filePath, localLine).
    QString readMarkLine( const QString& filePath, LineNumber localLine, QTextCodec* codec ) const;
    void ensureMarkLines( const QString& filePath, QTextCodec* codec ) const;
    QTextCodec* codecForFile( const QString& filePath ) const;
    QFile* fileForGroup( klogg::folder::FileId fileId ) const;
    const VisibleRow* visibleRowAt( LineNumber line ) const;

    // Guards every data member below EXCEPT openFiles_ (see fileIoMutex_):
    // unique for mutation (streaming commits, collapse, visibility, encoding
    // overrides), shared for all getters incl. the QuickFind worker's reads.
    mutable SharedMutex dataMutex_;
    // Guards openFiles_ and every seek/read on the shared per-group QFile
    // handles (two concurrent readMatchLine callers would otherwise tear the
    // file cursor). Always taken AFTER dataMutex_, never the reverse.
    mutable std::mutex fileIoMutex_;

    std::vector<klogg::folder::FileGroup> groups_;
    std::vector<VisibleRow> visibleRows_;
    QSet<klogg::folder::FileId> collapsed_;
    // Snapshot of collapsed groups' filePaths captured in beginSearch and
    // re-applied as each group streams back in, so a re-scan (e.g. a context-line
    // change) preserves the user's collapse state. Lives one search cycle.
    QSet<QString> pendingCollapsePaths_;

    Visibility visibility_ = Visibility::MarksAndMatches;
    // Widget-owned per-file marks store, read LIVE during rebuildVisibleRows
    // (main thread). Lets the Marks / Marks-and-matches filter enumerate marked
    // lines and inject marked non-match rows (single-file marks_ parity).
    const QHash<QString, std::set<uint64_t>>* marksStore_ = nullptr;
    // Marks-only groups (files with marks but no match group), rebuilt each
    // rebuildVisibleRows. Indexed by FileId = groups_.size() + index.
    std::vector<MarksGroup> marksGroups_;
    // Per-file decoded line text for on-demand marked-line fetch (marked
    // non-match lines have no MatchRecord offset). Built lazily by reading the
    // whole file, decoding with codecForFile and splitting on '\n' (correct for
    // all encodings incl. UTF-16), cached. Key is filePath + null + codec name;
    // a marks-only file (nullptr codec / UTF-8 default) and a later match-group
    // file (scanned codec, e.g. Shift-JIS) get separate cache entries to avoid
    // mojibake from a stale encoding mismatch. kMarkLineCacheCap is a PER-FILE
    // cap (huge files skip -> empty mark text, no main-thread stall); there is
    // no cache-wide memory budget. Persists across searches (file-intrinsic);
    // invalidated on encoding-override change. A transient open failure is NOT
    // cached so a later fetch can retry. Guarded by fileIoMutex_.
    mutable QHash<QString, std::vector<QString>> markLineCache_;
    // Per-file display-encoding overrides (Encoding-menu picks), consulted by
    // readMatchLine before the scan-time detected sourceCodec.
    QHash<QString, QByteArray> encodingOverrides_;

    // Streaming commit state. pendingByIndex_ buffers out-of-order groups keyed
    // by file enumeration index; nextExpectedIndex_ is the in-order commit
    // cursor (the next index that must be committed before any later index can
    // appear). All FolderSearchResults mutation happens on the main thread.
    std::vector<std::optional<klogg::folder::FileGroup>> pendingByIndex_;
    size_t nextExpectedIndex_ = 0;
    LineLength maxLength_ = 0_length;
    LineNumber maxLocalLine_ = 0_lnum;
    mutable LineLength headerMaxLength_ = 0_length;

    // Lazily-opened per-group file handles for on-demand match-line text fetch.
    // Indexed by fileId (== group index), so a dense vector rather than a map.
    mutable std::vector<std::unique_ptr<QFile>> openFiles_;
    // View-layer display encoding only. readMatchLine decodes match bytes with
    // each file's source codec (FileGroup::sourceCodec), NOT this value; this
    // is retained so the inherited doSetDisplayEncoding/doGetDisplayEncoding
    // API still works for the view layer and must not override source
    // interpretation.
    QByteArray displayEncodingName_ = "UTF-8";
};

#endif // FOLDERSEARCHRESULTS_H
