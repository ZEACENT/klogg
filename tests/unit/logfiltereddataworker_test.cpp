/*
 * Copyright (C) 2026
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

#include <catch2/catch.hpp>

#include <QDateTime>
#include <QTextCodec>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#ifndef Q_MOC_RUN
#include <roaring.hh>
#include <roaring64map.hh>
#include <tbb/task_group.h>
#endif

#include "abstractlogdata.h"
#include "atomicflag.h"
#include "linetypes.h"
#include "matchercache.h"
#include "regularexpression.h"
#include "searchablelogdata.h"
#include "synchronization.h"

#if defined( __clang__ )
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "logfiltereddataworker.h"
#undef private
#if defined( __clang__ )
#pragma clang diagnostic pop
#endif
#include "logfiltereddata.h"

namespace {
class TestSearchableLogData : public SearchableLogData {
  public:
    void interruptLoading() override {}
    std::unique_ptr<LogFilteredData> getNewFilteredData() const override { return nullptr; }
    qint64 getFileSize() const override { return 0; }
    QDateTime getLastModifiedDate() const override { return {}; }
    void reload( QTextCodec* = nullptr ) override {}
    QTextCodec* getDetectedEncoding() const override { return QTextCodec::codecForName( "UTF-8" ); }
    void setPrefilter( const QString& ) override {}
    void setAnsiProcessingMode( AnsiProcessingMode ) override {}
    RawLines getLinesRaw( LineNumber, LinesCount ) const override { return {}; }
    bool isLiveSource() const override { return true; }

  protected:
    QString doGetLineString( LineNumber ) const override { return {}; }
    QString doGetExpandedLineString( LineNumber ) const override { return {}; }
    klogg::vector<QString> doGetLines( LineNumber, LinesCount ) const override { return {}; }
    klogg::vector<QString> doGetExpandedLines( LineNumber, LinesCount ) const override
    {
        return {};
    }
    LineNumber doGetLineNumber( LineNumber index ) const override { return index; }
    LinesCount doGetNbLine() const override { return 0_lcount; }
    LineLength doGetMaxLength() const override { return 0_length; }
    LineLength doGetLineLength( LineNumber ) const override { return 0_length; }
    void doSetDisplayEncoding( const char* ) override {}
    QTextCodec* doGetDisplayEncoding() const override { return QTextCodec::codecForName( "UTF-8" ); }
    void doAttachReader() const override {}
    void doDetachReader() const override {}
};

LogFilteredDataWorker::SearchRequest makeLiveRequest( quint64 operationId, LineNumber endLine )
{
    return LogFilteredDataWorker::SearchRequest{ LogFilteredDataWorker::SearchRequest::Type::LiveUpdate,
                                                 RegularExpressionPattern{ QStringLiteral( "ERROR" ) },
                                                 0_lnum,
                                                 endLine,
                                                 0_lnum,
                                                 0,
                                                 operationId,
                                                 nullptr };
}
} // namespace

TEST_CASE( "LogFilteredDataWorker clears deferred live update when immediate live dispatch supersedes it" )
{
    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );

    worker.enqueueOrDeferLiveRequest( makeLiveRequest( 1, 10_lnum ) );
    REQUIRE( worker.deferredLiveRequest_.has_value() );

    worker.enqueueRequest( makeLiveRequest( 2, 100_lnum ), false );

    CHECK_FALSE( worker.deferredLiveRequest_.has_value() );

    worker.shutdownAndWait();
}
