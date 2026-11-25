#include <catch2/catch.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTabWidget>
#include "searchtoolbar.h"
#include "test_utils.h"
#include <QUuid>

#include <memory>
#include <thread>

#include "abstractcrawlerwidget.h"
#include "abstractlogview.h"
#include "crawlerwidget.h"
#include "foldercrawlerwidget.h"
#include "foldersearchengine.h"
#include "logfiltereddata.h"
#include "quickfindpattern.h"
#include "regularexpression.h"
#include "savedsearches.h"
#include "searchablelogdata.h"
#include "streaminglogdata.h"

namespace {
using Pane = AbstractCrawlerWidget::Pane;
using Scope = klogg::SnapshotScope;

struct Observations {
    int payloadReads = 0;
    int acquisitions = 0;
    bool ownerThread = false;
    Scope scope = Scope::PresentedRows;
};

class PaneSnapshot final : public klogg::LogSnapshot {
  public:
    QString text = QStringLiteral( "frozen" );
    LinesCount lineCount() const override { return 1_lcount; }
    klogg::SnapshotRow row( LineNumber ) const override
    {
        return { { klogg::LogRowId::Origin::Memory, 17, 4, 9 }, 10_lnum, true, true };
    }
    QString lineString( LineNumber ) const override { return text; }
    OptionalLineNumber presentedRowForIdentity( const klogg::LogRowId& identity ) const override
    {
        // A synthetic folder header precedes the retained row. Snapshot indices
        // and presented ordinals deliberately differ.
        return identity == row( 0_lnum ).identity ? OptionalLineNumber{ 1_lnum }
                                                : OptionalLineNumber{};
    }
};

class PaneData final : public SearchableLogData {
  public:
    void interruptLoading() override {}
    std::unique_ptr<LogFilteredData> getNewFilteredData() const override
    {
        return std::make_unique<LogFilteredData>( this );
    }
    qint64 getFileSize() const override { return 0; }
    QDateTime getLastModifiedDate() const override { return {}; }
    void reload( QTextCodec* = nullptr ) override {}
    QTextCodec* getDetectedEncoding() const override { return QTextCodec::codecForName( "UTF-8" ); }
    void setPrefilter( const QString& ) override {}
    void setAnsiProcessingMode( AnsiProcessingMode ) override {}
    RawLines getLinesRaw( LineNumber, LinesCount ) const override
    {
        ++observations->payloadReads;
        return {};
    }

    LinesCount count = 3_lcount;
    std::shared_ptr<Observations> observations = std::make_shared<Observations>();
    klogg::LogSnapshot::Ptr snapshot = std::make_shared<PaneSnapshot>();
    bool failCapture = false;
    QHash<uint64_t, QString> lineTexts;
    OptionalLineNumber failLine;

    klogg::LogSnapshot::Ptr createReadSnapshot( Scope scope ) const override
    {
        ++observations->acquisitions;
        observations->scope = scope;
        observations->ownerThread = QThread::currentThread() == thread();
        if ( failCapture ) throw klogg::SnapshotError{ "capture failed" };
        return snapshot;
    }

  protected:
    QString doGetLineString( LineNumber line ) const override
    {
        ++observations->payloadReads;
        if ( failLine && *failLine == line ) throw klogg::SnapshotError{ "fixture read failure" };
        return lineTexts.value( line.get(), QStringLiteral( "mutable" ) );
    }
    QString doGetExpandedLineString( LineNumber line ) const override
    {
        return doGetLineString( line );
    }
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount countToRead ) const override
    {
        klogg::vector<QString> result;
        for ( auto line = first; line < first + countToRead; ++line ) {
            result.push_back( doGetLineString( line ) );
        }
        return result;
    }
    klogg::vector<QString> doGetExpandedLines( LineNumber first, LinesCount countToRead ) const override
    {
        return doGetLines( first, countToRead );
    }
    LineNumber doGetLineNumber( LineNumber line ) const override { return line; }
    LinesCount doGetNbLine() const override { return count; }
    LineLength doGetMaxLength() const override { return 20_length; }
    LineLength doGetLineLength( LineNumber line ) const override
    {
        return LineLength{ doGetLineString( line ).size() };
    }
    void doSetDisplayEncoding( const char* ) override {}
    QTextCodec* doGetDisplayEncoding() const override { return QTextCodec::codecForName( "UTF-8" ); }
    void doAttachReader() const override {}
    void doDetachReader() const override {}
};

class PaneView final : public AbstractLogView {
  public:
    using AbstractLogView::AbstractLogView;

  protected:
    AbstractLogData::LineType lineType( LineNumber ) const override
    {
        return AbstractLogData::LineTypeFlags::Plain;
    }
};
} // namespace

TEST_CASE( "Runtime pane identity belongs to the view and needs no source capture",
           "[review-panes][review-pane-inventory]" )
{
    PaneData data;
    QuickFindPattern pattern;
    PaneView first{ &data, &pattern };
    PaneView second{ &data, &pattern };
    const auto id = first.runtimePaneId();
    REQUIRE_FALSE( id.isEmpty() );
    CHECK( id != second.runtimePaneId() );
    data.count = 9_lcount;
    first.textWrapSet( true );
    CHECK( first.runtimePaneId() == id );
    CHECK( data.observations->acquisitions == 0 );
    CHECK( data.observations->payloadReads == 0 );
}

TEST_CASE( "Pane inventory includes unavailable main views and stable live source panes",
           "[review-panes][review-pane-inventory]" )
{
    FolderCrawlerWidget folder;
    const auto folderPanes = folder.paneInventory();
    REQUIRE( folderPanes.size() == 2 );
    CHECK( folderPanes.front().kind == Pane::Main );
    CHECK_FALSE( folderPanes.front().metadata.available );
    CHECK( folderPanes.back().kind == Pane::Results );
    CHECK( folderPanes.back().metadata.available );
    CHECK_FALSE( folder.createPaneSnapshot( folderPanes.front().paneId, Scope::PresentedRows ) );
    CrawlerWidget empty;
    CHECK( empty.paneInventory().empty() );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto data = std::make_shared<StreamingLogData>( QUuid::createUuid().toString(), root.path() );
    data->appendUtf8( "before\n" );
    std::shared_ptr<LogFilteredData> filtered = data->getNewFilteredData();
    SavedSearches searches;
    CrawlerWidget live;
    live.setData( data, filtered );
    live.setQuickFindPattern( std::make_shared<QuickFindPattern>() );
    live.setSavedSearches( &searches );
    const auto panes = live.paneInventory();
    REQUIRE( panes.size() == 2 );
    REQUIRE( panes.front().metadata.available );
    const auto mainId = panes.front().paneId;
    const auto retained = live.createPaneSnapshot( mainId, Scope::PresentedRows );
    REQUIRE( retained );
    data->appendUtf8( "after\n" );
    CHECK( live.paneInventory().front().paneId == mainId );
    CHECK( live.paneMetadata( mainId ).sourceRowCount == 2_lcount );
    CHECK( retained->lineCount() == 1_lcount );
    CHECK_FALSE( live.paneMetadata( "retired-pane" ).available );
    CHECK_FALSE( live.createPaneSnapshot( "retired-pane", Scope::PresentedRows ) );
}

TEST_CASE( "Kept folder panes retain explicit identity across switching and retire on close",
           "[review-panes][review-pane-inventory]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "kept.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    const QByteArray text{ "foo first\nbar middle\nfoo last\n" };
    REQUIRE( file.write( text ) == text.size() );
    file.close();
    FolderCrawlerWidget widget;
    widget.setFolder( root.path(), QStringList{ path } );
    auto* engine = widget.findChild<FolderSearchEngine*>();
    REQUIRE( engine );
    const auto search = [&]( const QString& pattern ) {
        QObject completion;
        bool finished = false;
        QObject::connect( engine, &FolderSearchEngine::searchFinished, &completion,
                          [&] { finished = true; }, Qt::QueuedConnection );
        widget.searchFor( pattern );
        REQUIRE( waitUiState( [&] { return finished && !widget.isSearchActive(); } ) );
    };
    search( "foo" );
    const auto initial = widget.paneInventory();
    REQUIRE( initial.size() == 2 );
    const auto firstId = initial.back().paneId;
    widget.searchToolbar()->setKeepResultsChecked( true );
    search( "bar" );
    const auto kept = widget.paneInventory();
    REQUIRE( kept.size() == 3 );
    CHECK( kept.at( 1 ).paneId == firstId );
    CHECK_FALSE( kept.at( 1 ).current );
    CHECK( kept.at( 2 ).current );
    CHECK( kept.at( 2 ).paneId != firstId );
    const auto* focus = QApplication::focusWidget();
    const auto retained = widget.createPaneSnapshot( firstId, Scope::Matches );
    REQUIRE( retained );
    CHECK( retained->lineCount() == 2_lcount );
    CHECK( widget.resultsTabs()->currentIndex() == 1 );
    CHECK( QApplication::focusWidget() == focus );
    widget.resultsTabs()->setCurrentIndex( 0 );
    CHECK( widget.paneInventory().at( 1 ).paneId == firstId );
    CHECK( widget.paneInventory().at( 1 ).current );
    REQUIRE( QMetaObject::invokeMethod( widget.resultsTabs(), "tabCloseRequested", Qt::DirectConnection,
                                       Q_ARG( int, 0 ) ) );
    CHECK_FALSE( widget.paneMetadata( firstId ).available );
    CHECK_FALSE( widget.createPaneSnapshot( firstId, Scope::Matches ) );
    CHECK( widget.paneInventory().back().paneId == kept.at( 2 ).paneId );
    CHECK( retained->lineString( 0_lnum ) == QLatin1String( "foo first" ) );
}

TEST_CASE( "Applied-query inventory does not acquire or decode source content",
           "[review-applied-filter]" )
{
    auto source = std::make_shared<PaneData>();
    std::shared_ptr<LogFilteredData> model = source->getNewFilteredData();
    SavedSearches searches;
    CrawlerWidget widget;
    widget.setData( source, model );
    widget.setQuickFindPattern( std::make_shared<QuickFindPattern>() );
    widget.setSavedSearches( &searches );
    const auto reads = source->observations->payloadReads;
    const auto acquisitions = source->observations->acquisitions;
    const auto panes = widget.paneInventory();
    REQUIRE( panes.size() == 2 );
    REQUIRE( panes.back().appliedFilter );
    CHECK( panes.back().appliedFilter->pattern.pattern.isEmpty() );
    CHECK_FALSE( panes.back().appliedFilter->complete );
    CHECK( source->observations->payloadReads == reads );
    CHECK( source->observations->acquisitions == acquisitions );
}

TEST_CASE( "Folder kept panes retain query attribution while drafts and selection change",
           "[review-applied-filter]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "queries.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    const QByteArray text{ "foo first\nbar second\n" };
    REQUIRE( file.write( text ) == text.size() );
    file.close();
    FolderCrawlerWidget folder;
    folder.setFolder( root.path(), { path } );
    folder.setPresentationActive( false );
    auto* engine = folder.findChild<FolderSearchEngine*>();
    REQUIRE( engine );
    const auto search = [&]( const QString& pattern ) {
        QObject completion;
        bool finished = false;
        QObject::connect( engine, &FolderSearchEngine::searchFinished, &completion,
                          [&] { finished = true; }, Qt::QueuedConnection );
        folder.searchFor( pattern );
        REQUIRE( waitUiState( [&] { return finished && !folder.isSearchActive(); } ) );
    };
    search( "foo" );
    folder.searchToolbar()->setKeepResultsChecked( true );
    search( "bar" );
    folder.searchToolbar()->searchLineEdit()->setEditText( QStringLiteral( "unsubmitted" ) );
    const auto panes = folder.paneInventory();
    REQUIRE( panes.size() == 3 );
    CHECK_FALSE( panes.front().appliedFilter );
    REQUIRE( panes.at( 1 ).appliedFilter );
    REQUIRE( panes.at( 2 ).appliedFilter );
    CHECK( panes.at( 1 ).appliedFilter->pattern.pattern == QStringLiteral( "foo" ) );
    CHECK( panes.at( 2 ).appliedFilter->pattern.pattern == QStringLiteral( "bar" ) );
    CHECK( panes.at( 1 ).appliedFilter->complete );
    CHECK( panes.at( 2 ).appliedFilter->complete );
    folder.resultsTabs()->setCurrentIndex( 0 );
    const auto* focus = QApplication::focusWidget();
    const auto switched = folder.paneInventory();
    CHECK( switched.at( 1 ).appliedFilter->pattern.pattern == QStringLiteral( "foo" ) );
    CHECK( switched.at( 2 ).appliedFilter->pattern.pattern == QStringLiteral( "bar" ) );
    CHECK( folder.resultsTabs()->currentIndex() == 0 );
    CHECK( QApplication::focusWidget() == focus );
    SECTION( "reset affects only its owner" ) {
        folder.searchFor( QString{} );
        const auto cleared = folder.paneInventory();
        CHECK( cleared.at( 1 ).appliedFilter->pattern.pattern.isEmpty() );
        CHECK_FALSE( cleared.at( 1 ).appliedFilter->complete );
        CHECK( cleared.at( 2 ).appliedFilter->pattern.pattern == QStringLiteral( "bar" ) );
    }
    SECTION( "synchronous close observers retain surviving view attribution" ) {
        QObject observer;
        int observations = 0;
        bool coherent = true;
        const auto survivorId = panes.at( 2 ).paneId;
        QObject::connect( &folder, &FolderCrawlerWidget::searchablesChanged, &observer, [&] {
            for ( const auto& pane : folder.paneInventory() ) {
                if ( pane.paneId != survivorId ) continue;
                ++observations;
                coherent = coherent && pane.appliedFilter
                    && pane.appliedFilter->pattern.pattern == QStringLiteral( "bar" );
            }
        } );
        REQUIRE( QMetaObject::invokeMethod( folder.resultsTabs(), "tabCloseRequested",
                                           Qt::DirectConnection, Q_ARG( int, 0 ) ) );
        CHECK( observations > 0 );
        CHECK( coherent );
        CHECK( folder.paneInventory().back().paneId == survivorId );
    }
}

TEST_CASE( "Mark membership edits defer decoding until explicit width projection",
           "[review-mark-projection]" )
{
    PaneData data;
    LogFilteredData filtered{ &data };
    const auto reads = data.observations->payloadReads;
    filtered.addMark( 0_lnum );
    CHECK( data.observations->payloadReads == reads );
    CHECK( filtered.getNbMarks() == 1_lcount );
    CHECK( filtered.getNbLine() == 1_lcount );
    CHECK( data.observations->payloadReads == reads );
    CHECK( filtered.getMaxLength() == 20_length );
    const auto measured = data.observations->payloadReads;
    CHECK( measured == reads + 1 );
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == measured );
    filtered.addMark( 1_lnum );
    CHECK( data.observations->payloadReads == measured );
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == measured + 1 );
    const auto bothMeasured = data.observations->payloadReads;
    filtered.addMark( 1_lnum );
    filtered.deleteMark( 2_lnum );
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == bothMeasured );
    filtered.deleteMark( 0_lnum );
    CHECK( data.observations->payloadReads == bothMeasured );
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == bothMeasured + 1 );
    filtered.clearMarks();
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == bothMeasured + 1 );
}

TEST_CASE( "Mark width projection measures only surviving additions in a batch",
           "[review-mark-projection]" )
{
    PaneData data;
    LogFilteredData filtered{ &data };
    filtered.addMark( 0_lnum );
    filtered.addMark( 1_lnum );
    filtered.toggleMark( 2_lnum );
    filtered.toggleMark( 1_lnum );
    filtered.deleteMark( 0_lnum );
    CHECK( filtered.getMarks() == QList<LineNumber>{ 2_lnum } );
    CHECK( data.observations->payloadReads == 0 );
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == 1 );
}

TEST_CASE( "Lazy mark widths preserve long rows and recover after measurement failure",
           "[review-mark-projection]" )
{
    PaneData data;
    data.lineTexts.insert( 0, QString( 100, 'x' ) );
    LogFilteredData filtered{ &data };
    filtered.addMark( 0_lnum );
    filtered.addMark( 1_lnum );
    SECTION( "failed measurements never publish a partial cache" ) {
        data.failLine = 1_lnum;
        CHECK_THROWS_AS( filtered.getMaxLength(), klogg::SnapshotError );
        const auto reads = data.observations->payloadReads;
        data.failLine.reset();
        CHECK( filtered.getMaxLength() == 100_length );
        CHECK( data.observations->payloadReads == reads + 2 );
    }
    SECTION( "removing a nonmaximum row keeps the measured maximum" ) {
        CHECK( filtered.getMaxLength() == 100_length );
        const auto reads = data.observations->payloadReads;
        filtered.deleteMark( 1_lnum );
        CHECK( filtered.getMaxLength() == 100_length );
        CHECK( data.observations->payloadReads == reads );
        filtered.deleteMark( 0_lnum );
        CHECK( filtered.getMaxLength() == 20_length );
        CHECK( data.observations->payloadReads == reads );
    }
}

TEST_CASE( "Source-model marks expose a bounded detached snapshot without reading rows",
           "[review-marks-owner]" )
{
    PaneData data;
    LogFilteredData filtered{ &data };
    filtered.addMark( 2_lnum );
    filtered.addMark( 0_lnum );
    const klogg::ReviewMarksOwner& owner = filtered;
    const auto state = owner.reviewMarksSnapshot();
    REQUIRE( state );
    CHECK( state->scope == klogg::ReviewMarksSnapshot::Scope::SourceModel );
    CHECK( state->revision == owner.reviewMarksRevision() );
    REQUIRE( state->groups.size() == 1 );
    CHECK( state->groups.front().memberPath.isEmpty() );
    CHECK( state->groups.front().rows == QList<LineNumber>{ 0_lnum, 2_lnum } );
    filtered.deleteMark( 0_lnum );
    CHECK( owner.reviewMarksRevision() != state->revision );
    CHECK( state->groups.front().rows == QList<LineNumber>{ 0_lnum, 2_lnum } );
    CHECK( data.observations->payloadReads == 0 );
}

TEST_CASE( "Source mark observation rejects oversized stores before copying rows",
           "[review-marks-owner]" )
{
    PaneData data;
    data.count = LinesCount( klogg::ReviewMarksOwner::MaxMarks + 1 );
    LogFilteredData filtered{ &data };
    for ( uint64_t row = 0; row < data.count.get(); ++row ) filtered.addMark( LineNumber( row ) );
    CHECK_FALSE( filtered.reviewMarksSnapshot() );
    CHECK( data.observations->payloadReads == 0 );
}

TEST_CASE( "Folder mark observation includes absent members in stable order without opening files",
           "[review-marks-owner]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    FolderCrawlerWidget folder;
    folder.setFolder( root.path(), {} );
    const QJsonObject marks{ { "z.log", QJsonArray{ 2, 0, 2 } }, { "a.log", QJsonArray{ 1 } } };
    folder.setViewContext( QString::fromUtf8( QJsonDocument( QJsonObject{ { "M", marks } } ).toJson() ) );
    const auto state = folder.reviewMarksSnapshot();
    REQUIRE( state );
    CHECK( state->scope == klogg::ReviewMarksSnapshot::Scope::FolderTab );
    CHECK( state->revision == folder.reviewMarksRevision() );
    REQUIRE( state->groups.size() == 2 );
    CHECK( state->groups.at( 0 ).memberPath == root.filePath( "a.log" ) );
    CHECK( state->groups.at( 0 ).rows == QList<LineNumber>{ 1_lnum } );
    CHECK( state->groups.at( 1 ).memberPath == root.filePath( "z.log" ) );
    CHECK( state->groups.at( 1 ).rows == QList<LineNumber>{ 0_lnum, 2_lnum } );
    folder.setViewContext( QStringLiteral( "{}" ) );
    CHECK( folder.reviewMarksRevision() != state->revision );
    CHECK( state->groups.size() == 2 );
    CHECK( folder.reviewMarksSnapshot()->groups.empty() );
}

TEST_CASE( "Folder mark snapshots bound member count row count and member text",
           "[review-marks-owner]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    FolderCrawlerWidget folder;
    folder.setFolder( root.path(), {} );
    QJsonObject marks;
    SECTION( "member count" ) {
        for ( size_t i = 0; i <= klogg::ReviewMarksOwner::MaxMembers; ++i )
            marks.insert( QString::number( i ), QJsonArray{ 0 } );
    }
    SECTION( "row count" ) {
        QJsonArray rows;
        for ( size_t i = 0; i <= klogg::ReviewMarksOwner::MaxMarks; ++i ) rows.append( static_cast<int>( i ) );
        marks.insert( "many.log", rows );
    }
    SECTION( "member text" ) {
        marks.insert( QString( static_cast<int>( klogg::ReviewMarksOwner::MaxTextUnits ), 'x' ), QJsonArray{ 0 } );
    }
    folder.setViewContext( QString::fromUtf8( QJsonDocument( QJsonObject{ { "M", marks } } ).toJson() ) );
    CHECK_FALSE( folder.reviewMarksSnapshot() );
}

TEST_CASE( "Folder mark owner prepares every kept pane and publishes after all guards release",
           "[review-mark-owner-publication]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "kept.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    const QByteArray text{ "foo first\nbar middle\nfoo last\n" };
    REQUIRE( file.write( text ) == text.size() );
    file.close();
    FolderCrawlerWidget folder;
    folder.setFolder( root.path(), { path } );
    auto* engine = folder.findChild<FolderSearchEngine*>();
    REQUIRE( engine );
    const auto search = [&]( const QString& pattern ) {
        QObject completion;
        bool finished = false;
        QObject::connect( engine, &FolderSearchEngine::searchFinished, &completion,
                          [&] { finished = true; }, Qt::QueuedConnection );
        folder.searchFor( pattern );
        REQUIRE( waitUiState( [&] { return finished && !folder.isSearchActive(); } ) );
    };
    search( "foo" );
    auto* kept = folder.folderResults();
    folder.searchToolbar()->setKeepResultsChecked( true );
    search( "bar" );
    auto* active = folder.folderResults();
    REQUIRE( active != kept );
    auto candidate = *folder.reviewMarksSnapshot();
    candidate.groups.push_back( { path, { 1_lnum } } );
    klogg::ReviewMarksBudget budget;
    const auto prepare = folder.prepareReviewMarks( candidate, budget );
    REQUIRE( prepare );
    std::atomic<bool> cancelled{ false };
    std::shared_ptr<klogg::PreparedReviewMarks> prepared;
    std::thread worker( [&] { prepared = prepare( cancelled ); } );
    worker.join();
    REQUIRE( prepared );
    CHECK( folder.reviewMarksSnapshot()->groups.empty() );
    CHECK( kept->getNbLine() == 3_lcount );
    SECTION( "publish every pane before the first observer" ) {
        QSignalSpy keptChanged( kept, &FolderSearchResults::layoutChanged );
        QSignalSpy activeChanged( active, &FolderSearchResults::layoutChanged );
        bool allInstalled = false;
        QObject::connect( kept, &FolderSearchResults::layoutChanged, &folder, [&] {
            allInstalled = kept->getNbLine() == 4_lcount && active->getNbLine() == 2_lcount
                && folder.reviewMarksRevision() == candidate.revision + 1;
        } );
        const auto* focus = QApplication::focusWidget();
        auto publication = prepared->acquire();
        REQUIRE( publication );
        publication->install();
        CHECK( keptChanged.count() == 0 );
        CHECK( activeChanged.count() == 0 );
        publication->release();
        publication->notify();
        publication->notify();
        CHECK( allInstalled );
        CHECK( keptChanged.count() == 1 );
        CHECK( activeChanged.count() == 1 );
        CHECK( folder.resultsTabs()->currentIndex() == 1 );
        CHECK( QApplication::focusWidget() == focus );
        CHECK_FALSE( prepared->acquire() );
    }
    SECTION( "later pane conflict releases an earlier acquired guard" ) {
        active->collapseAll();
        CHECK_FALSE( prepared->acquire() );
        active->expandAll();
        REQUIRE( prepared->acquire() );
    }
    SECTION( "new kept pane invalidates the captured participant set" ) {
        folder.searchToolbar()->setKeepResultsChecked( true );
        search( "last" );
        CHECK_FALSE( prepared->acquire() );
    }
    SECTION( "closed kept pane invalidates the captured participant set" ) {
        REQUIRE( QMetaObject::invokeMethod( folder.resultsTabs(), "tabCloseRequested", Qt::DirectConnection,
                                           Q_ARG( int, 0 ) ) );
        CHECK_FALSE( prepared->acquire() );
    }
    SECTION( "aggregate projection budget is shared across panes" ) {
        klogg::ReviewMarksBudget exhausted;
        exhausted.remainingBytes = 0;
        CHECK_FALSE( folder.prepareReviewMarks( candidate, exhausted ) );
    }
}

TEST_CASE( "Source mark owner separates worker preparation installation and notification",
           "[review-mark-owner-publication]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\nc\n" );
    auto filtered = data.getNewFilteredData();
    auto candidate = *filtered->reviewMarksSnapshot();
    candidate.groups.push_back( { {}, { 1_lnum } } );
    klogg::ReviewMarksBudget budget;
    const auto prepare = filtered->prepareReviewMarks( candidate, budget );
    REQUIRE( prepare );
    CHECK( budget.remainingBytes < klogg::ReviewMarksBudget::MaxBytes );
    std::atomic<bool> cancelled{ false };
    std::shared_ptr<klogg::PreparedReviewMarks> prepared;
    std::thread worker( [&] { prepared = prepare( cancelled ); } );
    worker.join();
    REQUIRE( prepared );
    CHECK( filtered->getNbMarks() == 0_lcount );
    QSignalSpy changed( filtered.get(), &LogFilteredData::marksChanged );
    auto publication = prepared->acquire();
    REQUIRE( publication );
    publication->install();
    CHECK( filtered->getMarks() == QList<LineNumber>{ 1_lnum } );
    CHECK( changed.count() == 0 );
    publication->release();
    publication->notify();
    publication->notify();
    CHECK( changed.count() == 1 );
    CHECK_FALSE( prepared->acquire() );
}

TEST_CASE( "Source mark owner admission cancellation and closure never publish a prefix",
           "[review-mark-owner-publication]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\n" );
    auto filtered = data.getNewFilteredData();
    auto candidate = *filtered->reviewMarksSnapshot();
    candidate.groups.push_back( { {}, { 1_lnum } } );
    klogg::ReviewMarksBudget budget;
    const auto prepare = filtered->prepareReviewMarks( candidate, budget );
    REQUIRE( prepare );
    std::atomic<bool> cancelled{ false };
    SECTION( "wrong scope" ) {
        candidate.scope = klogg::ReviewMarksSnapshot::Scope::FolderTab;
        CHECK_FALSE( filtered->prepareReviewMarks( candidate, budget ) );
    }
    SECTION( "aggregate budget exhausted" ) {
        budget.remainingBytes = 0;
        CHECK_FALSE( filtered->prepareReviewMarks( candidate, budget ) );
    }
    SECTION( "cancel before work" ) { cancelled = true; CHECK_FALSE( prepare( cancelled ) ); }
    SECTION( "owner closed before worker completion" ) {
        filtered.reset();
        const auto prepared = prepare( cancelled );
        REQUIRE( prepared );
        CHECK_FALSE( prepared->acquire() );
    }
}

TEST_CASE( "Source marks prepare context privately and publish one native revision",
           "[review-mark-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\nc\nd\ne\n" );
    auto filtered = data.getNewFilteredData();
    filtered->setVisibility( LogFilteredData::VisibilityFlags::Marks );
    filtered->setContextLines( 1, 1 );
    filtered->addMark( 0_lnum );
    const auto revision = filtered->reviewMarksRevision();
    const auto captured = filtered->captureMarksUpdate( { 2_lnum, 4_lnum } );
    REQUIRE( captured );
    std::shared_ptr<LogFilteredData::PreparedMarksUpdate> prepared;
    std::thread worker( [&] { prepared = LogFilteredData::prepareMarksUpdate( captured ); } );
    worker.join();
    REQUIRE( prepared );
    CHECK( filtered->getMarks() == QList<LineNumber>{ 0_lnum } );
    CHECK( filtered->getNbLine() == 2_lcount );
    auto publication = filtered->lockMarksUpdate( prepared );
    REQUIRE( publication );
    CHECK( publication->install() );
    CHECK_FALSE( publication->install() );
    publication.reset();
    CHECK( filtered->reviewMarksRevision() == revision + 1 );
    CHECK( filtered->getMarks() == QList<LineNumber>{ 2_lnum, 4_lnum } );
    CHECK( filtered->getNbLine() == 4_lcount );
    CHECK( filtered->getMatchingLineNumber( 0_lnum ) == 1_lnum );
    CHECK( filtered->getMatchingLineNumber( 3_lnum ) == 4_lnum );
    CHECK_FALSE( filtered->lockMarksUpdate( prepared ) );
}

TEST_CASE( "Source mark preparation rejects concurrent edits and replacement but accepts safe append",
           "[review-mark-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\nc\n" );
    auto filtered = data.getNewFilteredData();
    filtered->setVisibility( LogFilteredData::VisibilityFlags::Marks );
    const auto captured = filtered->captureMarksUpdate( { 1_lnum } );
    REQUIRE( captured );
    const auto prepared = LogFilteredData::prepareMarksUpdate( captured );
    REQUIRE( prepared );
    bool valid = false;
    SECTION( "human mark ABA" ) { filtered->addMark( 0_lnum ); filtered->deleteMark( 0_lnum ); }
    SECTION( "human context" ) { filtered->setContextLines( 1, 1 ); }
    SECTION( "human visibility" ) { filtered->setVisibility( LogFilteredData::VisibilityFlags::Matches ); }
    SECTION( "source replacement" ) { data.clearCapture(); data.appendUtf8( "x\ny\nz\n" ); }
    SECTION( "sealed append preserves targeted rows" ) { data.appendUtf8( "d\n" ); valid = true; }
    SECTION( "another owner" ) {
        auto other = data.getNewFilteredData();
        other->setVisibility( LogFilteredData::VisibilityFlags::Marks );
        CHECK_FALSE( other->lockMarksUpdate( prepared ) );
        return;
    }
    auto publication = filtered->lockMarksUpdate( prepared );
    CHECK( static_cast<bool>( publication ) == valid );
    CHECK( filtered->getNbMarks() == 0_lcount );
    if ( publication ) CHECK( publication->install() );
}

TEST_CASE( "Source mark no-op remains revision neutral and bounded admission rejects excessive context",
           "[review-mark-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\nc\n" );
    auto filtered = data.getNewFilteredData();
    filtered->setVisibility( LogFilteredData::VisibilityFlags::Marks );
    filtered->addMark( 1_lnum );
    const auto revision = filtered->reviewMarksRevision();
    const auto captured = filtered->captureMarksUpdate( { 1_lnum, 1_lnum } );
    REQUIRE( captured );
    auto publication = filtered->lockMarksUpdate( LogFilteredData::prepareMarksUpdate( captured ) );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication.reset();
    CHECK( filtered->reviewMarksRevision() == revision );
    filtered->setContextLines( static_cast<int>( LogFilteredData::MaxPreparedContextRows ), 0 );
    CHECK_FALSE( filtered->captureMarksUpdate( { 2_lnum } ) );
    QList<LineNumber> excessive;
    for ( size_t index = 0; index <= klogg::ReviewMarksOwner::MaxMarks; ++index ) excessive.append( 0_lnum );
    CHECK_FALSE( filtered->captureMarksUpdate( excessive ) );
}

TEST_CASE( "Prepared source context permits append only beyond its completed ranges",
           "[review-mark-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\nc\n" );
    auto filtered = data.getNewFilteredData();
    filtered->setVisibility( LogFilteredData::VisibilityFlags::Marks );
    filtered->setContextLines( 0, 1 );
    const bool clipped = GENERATE( false, true );
    const auto captured = filtered->captureMarksUpdate( { clipped ? 2_lnum : 1_lnum } );
    REQUIRE( captured );
    const auto prepared = LogFilteredData::prepareMarksUpdate( captured );
    REQUIRE( prepared );
    data.appendUtf8( "d\n" );
    auto publication = filtered->lockMarksUpdate( prepared );
    REQUIRE( static_cast<bool>( publication ) == !clipped );
    if ( publication ) {
        REQUIRE( publication->install() );
        publication.reset();
        CHECK( filtered->getNbLine() == 2_lcount );
        CHECK( filtered->getMatchingLineNumber( 1_lnum ) == 2_lnum );
    }
    else CHECK( filtered->getNbMarks() == 0_lcount );
}

TEST_CASE( "Source private mark work survives owner deletion and rejects unobserved rows",
           "[review-mark-preparation]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    data.appendUtf8( "a\nb\n" );
    REQUIRE( data.getNbLine() == 2_lcount );
    auto filtered = data.getNewFilteredData();
    const auto captured = filtered->captureMarksUpdate( { 1_lnum } );
    REQUIRE( captured );
    SECTION( "worker uses only detached input" ) {
        filtered.reset();
        std::shared_ptr<LogFilteredData::PreparedMarksUpdate> prepared;
        std::thread worker( [&] { prepared = LogFilteredData::prepareMarksUpdate( captured ); } );
        worker.join();
        CHECK( prepared );
    }
    SECTION( "future row has not been observed" ) {
        CHECK_FALSE( filtered->captureMarksUpdate( { 2_lnum } ) );
    }
}

TEST_CASE( "Mark source identities resolve current live rows rather than segment-local ordinals",
           "[review-marks-source]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    StreamingLogData data( QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 2;
    limits.maxTotalLines = 2;
    data.setCaptureLimits( limits );
    data.appendUtf8( "a\nb\n" );
    auto filtered = data.getNewFilteredData();
    const auto snapshot = data.createReadSnapshot( Scope::PresentedRows );
    REQUIRE( snapshot );
    const auto evicted = snapshot->row( 0_lnum ).identity;
    const auto survivor = snapshot->row( 1_lnum ).identity;
    const auto before = filtered->resolveReviewMark( survivor );
    REQUIRE( before );
    CHECK( before->memberPath.isEmpty() );
    CHECK( before->row == 1_lnum );
    data.appendUtf8( "c\n" );
    CHECK_FALSE( filtered->resolveReviewMark( evicted ) );
    const auto after = filtered->resolveReviewMark( survivor );
    REQUIRE( after );
    CHECK( after->row == 0_lnum );
    CHECK( *before != *after );
    data.clearCapture();
    data.appendUtf8( "replacement\n" );
    CHECK_FALSE( filtered->resolveReviewMark( survivor ) );
    CHECK( filtered->getNbMarks() == 0_lcount );
}

TEST_CASE( "Folder mark sources resolve hidden retained rows without reopening their paths",
           "[review-marks-source]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "retained.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    const QByteArray text{ "before\nhit\nafter\n" };
    REQUIRE( file.write( text ) == text.size() );
    file.close();
    RegularExpression expression{ RegularExpressionPattern{ "hit" } };
    const auto matcher = expression.createMatcher();
    auto group = FolderSearchEngine::scanFile( path, *matcher );
    REQUIRE( group.content );
    const auto content = group.content;
    const auto identity = content->identity( 0_lnum ); // Nonmatching but retained.
    FolderCrawlerWidget folder;
    folder.folderResults()->setResults( { std::move( group ) } );
    folder.folderResults()->setCollapsed( 0, true );
    REQUIRE_FALSE( folder.folderResults()->rowForIdentity( identity ) );
    REQUIRE( QFile::remove( path ) );
    const auto reads = content->bytesReadForTesting();
    const auto location = folder.resolveReviewMark( identity );
    REQUIRE( location );
    CHECK( location->memberPath == path );
    CHECK( location->row == 0_lnum );
    CHECK( content->bytesReadForTesting() == reads );
    auto foreign = identity;
    foreign.origin = klogg::LogRowId::Origin::Capture;
    CHECK_FALSE( folder.resolveReviewMark( foreign ) );
    folder.folderResults()->beginSearch( QStringList{ path } );
    CHECK_FALSE( folder.resolveReviewMark( identity ) );
    CHECK( folder.marksSnapshot().marks.isEmpty() );
}

TEST_CASE( "Folder mark identity resolution retains indexed members across human navigation",
           "[review-marks-source]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto firstPath = root.filePath( "a.log" );
    const auto secondPath = root.filePath( "b.log" );
    for ( const auto& path : { firstPath, secondPath } ) {
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "hit\n" ) == 4 );
    }
    RegularExpression expression{ RegularExpressionPattern{ "hit" } };
    const auto matcher = expression.createMatcher();
    FolderCrawlerWidget folder;
    folder.setFolder( root.path(), QStringList{ firstPath, secondPath } );
    folder.folderResults()->setResults( { FolderSearchEngine::scanFile( firstPath, *matcher ),
                                         FolderSearchEngine::scanFile( secondPath, *matcher ) } );
    const auto open = [&]( LineNumber resultRow, const QString& path ) {
        const auto ready = [&] {
            const auto info = folder.currentMainViewInfo();
            return info && info->path == path && info->nbLines == 1;
        };
        folder.selectResultRow( resultRow );
        REQUIRE( waitUiState( ready ) );
        const auto snapshot = folder.createPaneSnapshot( Pane::Main, Scope::PresentedRows );
        REQUIRE( snapshot );
        return snapshot->row( 0_lnum ).identity;
    };
    const auto firstId = open( 1_lnum, firstPath );
    const auto secondId = open( 3_lnum, secondPath );
    CHECK( firstId != secondId );
    const auto* focus = QApplication::focusWidget();
    const auto first = folder.resolveReviewMark( firstId );
    const auto second = folder.resolveReviewMark( secondId );
    REQUIRE( first );
    REQUIRE( second );
    CHECK( first->memberPath == firstPath );
    CHECK( second->memberPath == secondPath );
    CHECK( folder.currentMainViewInfo()->path == secondPath );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( folder.marksSnapshot().marks.isEmpty() );
}

TEST_CASE( "Review pane metadata reads widget state without payload or presentation work",
           "[review-panes]" )
{
    PaneData data;
    QuickFindPattern pattern;
    PaneView view{ &data, &pattern };
    view.textWrapSet( false );
    view.selectAndDisplayLine( 1_lnum );
    const auto selected = view.getSelectedText();
    const auto top = view.getTopLine();
    const auto geometry = view.geometry();
    const auto* focus = QApplication::focusWidget();
    const auto mapBuilds = view.visibleLineMapBuildCount();
    const auto reads = data.observations->payloadReads;
    QSignalSpy selectionChanged{ &view, &AbstractLogView::newSelection };

    for ( int query = 0; query < 3; ++query ) {
        const auto state = view.paneMetadata();
        CHECK( state.available );
        CHECK( state.sourceRowCount == 3_lcount );
        CHECK( QLatin1String( state.selectionKind ) == QLatin1String( "line" ) );
        CHECK( state.selectedRow == 1_lnum );
        REQUIRE( state.topPresentedRow );
        CHECK( *state.topPresentedRow == top );
        CHECK( state.follow == view.isFollowEnabled() );
        CHECK( state.wrap == view.isTextWrapEnabled() );
        CHECK( state.visible == view.isVisible() );
        CHECK( state.focused == view.hasFocus() );
    }
    CHECK( data.observations->acquisitions == 0 );
    CHECK( data.observations->payloadReads == reads );
    CHECK( view.visibleLineMapBuildCount() == mapBuilds );
    CHECK( view.geometry() == geometry );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( selectionChanged.count() == 0 );
    CHECK( view.getSelectedText() == selected );
}

TEST_CASE( "Review pane metadata observes current row counts and empty panes have no top row",
           "[review-panes]" )
{
    PaneData data;
    QuickFindPattern pattern;
    PaneView view{ &data, &pattern };
    data.count = 0_lcount;
    const auto empty = view.paneMetadata();
    CHECK( empty.available );
    CHECK( empty.sourceRowCount == 0_lcount );
    CHECK_FALSE( empty.topPresentedRow );
    data.count = 7_lcount;
    CHECK( view.paneMetadata().sourceRowCount == 7_lcount );
    CHECK( data.observations->payloadReads == 0 );
    CHECK( data.observations->acquisitions == 0 );
}

TEST_CASE( "Review pane capture forwards scope and retains provider mapping beyond view lifetime",
           "[review-panes]" )
{
    const auto scope = GENERATE( Scope::PresentedRows, Scope::Matches );
    klogg::LogSnapshot::Ptr retained;
    std::shared_ptr<Observations> observations;
    {
        PaneData data;
        QuickFindPattern pattern;
        PaneView view{ &data, &pattern };
        observations = data.observations;
        retained = view.createPaneSnapshot( scope );
        REQUIRE( retained );
        CHECK( retained == data.snapshot );
        CHECK( observations->scope == scope );
        CHECK( observations->ownerThread );
        CHECK( observations->acquisitions == 1 );
        CHECK( observations->payloadReads == 0 );
        data.snapshot.reset();
        data.count = 0_lcount;
    }
    REQUIRE( retained );
    CHECK( retained->lineCount() == 1_lcount );
    CHECK( retained->lineString( 0_lnum ) == QStringLiteral( "frozen" ) );
    const auto presented = retained->presentedRowForIdentity( retained->row( 0_lnum ).identity );
    REQUIRE( presented );
    CHECK( *presented == 1_lnum );
    CHECK( observations->payloadReads == 0 );
}

TEST_CASE( "Review pane capture preserves unsupported and failure results without mutable fallback",
           "[review-panes]" )
{
    PaneData data;
    QuickFindPattern pattern;
    PaneView view{ &data, &pattern };
    data.snapshot.reset();
    CHECK_FALSE( view.createPaneSnapshot( Scope::PresentedRows ) );
    CHECK( data.observations->acquisitions == 1 );
    data.failCapture = true;
    CHECK_THROWS_AS( view.createPaneSnapshot( Scope::Matches ), klogg::SnapshotError );
    CHECK( data.observations->acquisitions == 2 );
    CHECK( data.observations->payloadReads == 0 );
}

TEST_CASE( "Review pane metadata does not decode dirty filtered context rows",
           "[review-panes][review-panes-context]" )
{
    PaneData data;
    LogFilteredData filtered{ &data };
    QuickFindPattern pattern;
    PaneView view{ &filtered, &pattern };
    filtered.addMark( 1_lnum );
    filtered.setContextLines( 1, 1 );
    const auto reads = data.observations->payloadReads;
    const auto captures = data.observations->acquisitions;

    const auto state = view.paneMetadata();
    CHECK( state.available );
    CHECK( state.sourceRowCount == 3_lcount );
    CHECK( data.observations->payloadReads == reads );
    CHECK( data.observations->acquisitions == captures );
    // Layout may explicitly request lengths later, after metadata observation.
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads > reads );
    const auto layoutReads = data.observations->payloadReads;
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == layoutReads );

    // A membership change invalidates the lazy width, without making the next
    // observation decode the new membership ahead of an explicit layout query.
    filtered.setContextLines( 0, 1 );
    CHECK( view.paneMetadata().sourceRowCount == 2_lcount );
    CHECK( data.observations->payloadReads == layoutReads );
    CHECK( filtered.getMaxLength() == 20_length );
    CHECK( data.observations->payloadReads == layoutReads + 2 );
}

TEST_CASE( "Review pane owners distinguish absent file panes from an empty folder results pane",
           "[review-panes]" )
{
    CrawlerWidget file;
    const AbstractCrawlerWidget& fileOwner = file;
    CHECK_FALSE( fileOwner.paneMetadata( Pane::Main ).available );
    CHECK_FALSE( fileOwner.paneMetadata( Pane::Results ).available );
    CHECK_FALSE( fileOwner.createPaneSnapshot( Pane::Main, Scope::PresentedRows ) );

    FolderCrawlerWidget folder;
    const AbstractCrawlerWidget& folderOwner = folder;
    CHECK_FALSE( folderOwner.paneMetadata( Pane::Main ).available );
    CHECK_FALSE( folderOwner.createPaneSnapshot( Pane::Main, Scope::PresentedRows ) );
    const auto results = folderOwner.paneMetadata( Pane::Results );
    CHECK( results.available );
    CHECK( results.sourceRowCount == 0_lcount );
    CHECK_FALSE( results.topPresentedRow );
    const auto snapshot = folderOwner.createPaneSnapshot( Pane::Results, Scope::PresentedRows );
    REQUIRE( snapshot );
    CHECK( snapshot->lineCount() == 0_lcount );
}

TEST_CASE( "Review panes expose live owner state while captured rows stay frozen",
           "[review-panes]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    klogg::LogSnapshot::Ptr retained;
    {
        auto data = std::make_shared<StreamingLogData>(
            QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
        data->appendUtf8( "before\n" );
        std::shared_ptr<LogFilteredData> filtered = data->getNewFilteredData();
        SavedSearches searches;
        CrawlerWidget widget;
        widget.setData( data, filtered );
        widget.setQuickFindPattern( std::make_shared<QuickFindPattern>() );
        widget.setSavedSearches( &searches );
        const AbstractCrawlerWidget& owner = widget;
        CHECK( owner.paneMetadata( Pane::Main ).available );
        CHECK( owner.paneMetadata( Pane::Main ).sourceRowCount == 1_lcount );
        CHECK( owner.paneMetadata( Pane::Results ).available );
        CHECK( owner.paneMetadata( Pane::Results ).sourceRowCount == filtered->getNbLine() );
        retained = owner.createPaneSnapshot( Pane::Main, Scope::PresentedRows );
        REQUIRE( retained );
        data->appendUtf8( "after\n" );
        CHECK( owner.paneMetadata( Pane::Main ).sourceRowCount == 2_lcount );
        CHECK( retained->lineCount() == 1_lcount );
        widget.followSet( true );
        widget.textWrapSet( true );
        CHECK( owner.paneMetadata( Pane::Main ).follow );
        CHECK( owner.paneMetadata( Pane::Main ).wrap );
        const auto mainId = owner.paneInventory().front().paneId;
        const auto identity = retained->row( 0_lnum ).identity;
        const auto following = widget.navigateReviewPane( mainId, identity );
        CHECK( std::get<AbstractCrawlerWidget::NavigationError>( following )
               == AbstractCrawlerWidget::NavigationError::NotAvailable );
        CHECK( owner.paneMetadata( Pane::Main ).follow );
        widget.followSet( false );
        widget.textWrapSet( false );
        CHECK_FALSE( owner.paneMetadata( Pane::Main ).follow );
        CHECK_FALSE( owner.paneMetadata( Pane::Main ).wrap );
        CHECK( std::get<LineNumber>( widget.navigateReviewPane( mainId, identity ) ) == 0_lnum );
        CHECK( owner.paneMetadata( Pane::Main ).selectedRow == 0_lnum );
    }
    CHECK( retained->lineString( 0_lnum ) == QStringLiteral( "before" ) );
}

TEST_CASE( "Review folder pane counts headers while snapshots retain scoped source mappings",
           "[review-panes]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = QDir( root.path() ).filePath( QStringLiteral( "source.log" ) );
    {
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "error first\nerror second\n" ) == 25 );
    }
    klogg::LogSnapshot::Ptr retained;
    {
        RegularExpression expression{ RegularExpressionPattern{ "error" } };
        const auto matcher = expression.createMatcher();
        FolderCrawlerWidget widget;
        widget.folderResults()->setResults( { FolderSearchEngine::scanFile( path, *matcher ) } );
        const AbstractCrawlerWidget& owner = widget;
        const auto state = owner.paneMetadata( Pane::Results );
        REQUIRE( state.available );
        CHECK( state.sourceRowCount == 3_lcount ); // Header plus two data rows.
        REQUIRE( state.topPresentedRow );
        CHECK( *state.topPresentedRow == 0_lnum );
        retained = owner.createPaneSnapshot( Pane::Results, Scope::PresentedRows );
        REQUIRE( retained );
        CHECK( retained->lineCount() == 2_lcount );
        widget.folderResults()->setCollapsed( 0, true );
        CHECK( owner.paneMetadata( Pane::Results ).sourceRowCount == 1_lcount );
        const auto hidden = widget.navigateReviewPane( owner.paneInventory().back().paneId,
                                                        retained->row( 0_lnum ).identity );
        CHECK( std::get<AbstractCrawlerWidget::NavigationError>( hidden )
               == AbstractCrawlerWidget::NavigationError::RowNotPresent );
        CHECK_FALSE( owner.paneMetadata( Pane::Results ).selectedRow );
        const auto collapsed = owner.createPaneSnapshot( Pane::Results, Scope::PresentedRows );
        const auto matches = owner.createPaneSnapshot( Pane::Results, Scope::Matches );
        REQUIRE( collapsed );
        REQUIRE( matches );
        CHECK( collapsed->lineCount() == 0_lcount );
        CHECK( matches->lineCount() == 2_lcount );
    }
    REQUIRE( QFile::remove( path ) );
    CHECK( retained->lineString( 0_lnum ) == QStringLiteral( "error first" ) );
    const auto presented = retained->presentedRowForIdentity( retained->row( 0_lnum ).identity );
    REQUIRE( presented );
    CHECK( *presented == 1_lnum );
}
