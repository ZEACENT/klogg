#include <catch2/catch.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <memory>

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

    klogg::LogSnapshot::Ptr createReadSnapshot( Scope scope ) const override
    {
        ++observations->acquisitions;
        observations->scope = scope;
        observations->ownerThread = QThread::currentThread() == thread();
        if ( failCapture ) throw klogg::SnapshotError{ "capture failed" };
        return snapshot;
    }

  protected:
    QString doGetLineString( LineNumber ) const override
    {
        ++observations->payloadReads;
        return QStringLiteral( "mutable" );
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
        widget.followSet( false );
        widget.textWrapSet( false );
        CHECK_FALSE( owner.paneMetadata( Pane::Main ).follow );
        CHECK_FALSE( owner.paneMetadata( Pane::Main ).wrap );
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
