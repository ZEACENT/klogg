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

#include <QSet>
#include <QString>
#include <QTextCodec>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "abstractlogdata.h"
#include "foldersearchtypes.h"

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

    // The source group (fileId) a visible row belongs to (Header or Match).
    klogg::folder::FileId fileIdForLine( LineNumber visibleIndex ) const;

    // Returns the source file + local line for a visible Match row. For a
    // Header row returns the group's filePath with localLine == 0.
    klogg::folder::SourceRef sourceForLine( LineNumber visibleIndex ) const;

    // All match local-line numbers for filePath (ascending, as stored), empty if
    // the file is not in the result set. Used by the folder main-view overview.
    // Reads groups_ (not visibleRows_), so it is unaffected by collapse state.
    std::vector<LineNumber> matchLinesForFile( const QString& filePath ) const;

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
    // Inject the mark query the Marks filter consults (filePath, localLine) ->
    // marked. The widget owns the mark store; this keeps FolderSearchResults free
    // of UI-side mark ownership. Call before/with setVisibility(Marks).
    void setMarkedLineQuery( std::function<bool( const QString&, LineNumber )> query );
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
    LinesCount doGetNbLine() const override;
    LineLength doGetMaxLength() const override;
    LineLength doGetLineLength( LineNumber line ) const override;
    void doSetDisplayEncoding( const char* encoding ) override;
    QTextCodec* doGetDisplayEncoding() const override;
    void doAttachReader() const override;
    void doDetachReader() const override;

  private:
    struct VisibleRow {
        LineKind kind;
        klogg::folder::FileId fileId = 0;   // index into groups_
        size_t matchIndex = 0;              // index into groups_[fileId].matches (Match rows)
    };

    void rebuildVisibleRows();
    bool isLineMarked( const QString& filePath, LineNumber localLine ) const;
    QString headerText( klogg::folder::FileId fileId ) const;
    QString readMatchLine( klogg::folder::FileId fileId, size_t matchIndex ) const;
    QFile* fileForGroup( klogg::folder::FileId fileId ) const;
    const VisibleRow* visibleRowAt( LineNumber line ) const;

    std::vector<klogg::folder::FileGroup> groups_;
    std::vector<VisibleRow> visibleRows_;
    QSet<klogg::folder::FileId> collapsed_;

    Visibility visibility_ = Visibility::MarksAndMatches;
    std::function<bool( const QString&, LineNumber )> isMarkedLine_;

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
