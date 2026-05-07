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

#ifndef CONCATENATEDLOGDATA_H
#define CONCATENATEDLOGDATA_H

#include <memory>
#include <vector>

#include <QMetaObject>
#include <QObject>
#include <QString>

#include "abstractlogdata.h"
#include "linetypes.h"
#include "loadingstatus.h"

class LogData;

// ConcatenatedLogData presents multiple LogData objects as a single
// AbstractLogData, with lines from each source appearing consecutively.
// This is used for the "Merge Tabs" feature.
//
// Note: Search (LogFilteredDataWorker) requires LogData& for getLinesRaw().
// Since ConcatenatedLogData is not a LogData, a new LogFilteredData must be
// created that uses AbstractLogData for display but still delegates search
// to individual LogData sources. For now, search works through the
// AbstractLogData interface (getExpandedLines), which is slower but correct.
class ConcatenatedLogData : public AbstractLogData {
    Q_OBJECT

  public:
    ConcatenatedLogData();
    ~ConcatenatedLogData() override;

    // Add a source LogData. Lines from this source will appear after all
    // previously added sources. The caller retains ownership.
    void addSource( std::shared_ptr<LogData> source );

    // Rebuild cumulative line counts after a source changes.
    // Should be called when any source emits loadingFinished or fileChanged.
    void rebuildIndex();

    // Get the number of sources
    int sourceCount() const;

    // Map a global line number to (source_index, local_line_number)
    std::pair<int, LineNumber> mapToSource( LineNumber globalLine ) const;

    // Get a specific source LogData
    std::shared_ptr<LogData> sourceAt( int index ) const;

  Q_SIGNALS:
    // Emitted after rebuildIndex() completes, to notify views
    void loadingFinished( LoadingStatus status );
    void fileChanged( MonitoredFileStatus status );

  protected:
    QString doGetLineString( LineNumber line ) const override;
    QString doGetExpandedLineString( LineNumber line ) const override;
    klogg::vector<AnsiColorSpan> doGetLineAnsiColors( LineNumber line ) const override;
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
    struct SourceInfo {
        std::shared_ptr<LogData> logData;
        LinesCount cumulativeLines = 0_lcount; // total lines from this source and all before it
        QMetaObject::Connection loadingFinishedConnection;
        QMetaObject::Connection fileChangedConnection;
    };

    std::vector<SourceInfo> sources_;
    LinesCount totalLines_ = 0_lcount;
    LineLength maxLength_ = 0_length;
};

#endif
