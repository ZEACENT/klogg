#include <catch2/catch.hpp>
#include <future>

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>

#include "crawlerwidget.h"
#include "logdata.h"
#include "preparedfilesource.h"
#include "session.h"
#include "test_utils.h"

namespace {
struct ReadyDocumentFixture {
    QTemporaryDir root;
    QString path;
    std::shared_ptr<Session> session = std::make_shared<Session>();
    WindowSession window{ session, QStringLiteral( "ReadySources" ), 0 };
    FileSourcePreparation result;
    ReadyDocumentFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "input.log" );
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "one\ntwo\n" ) == 8 );
        file.close();
        const auto settings = IndexingSettings::capture();
        AtomicFlag cancelled;
        result = std::async( std::launch::async, [&] {
            return prepareFileSource( path, settings, cancelled );
        } ).get();
        REQUIRE( result.source );
    }
    static ViewInterface* makeView()
    {
        return new CrawlerWidget( nullptr, CrawlerWidget::InitialFocusPolicy::Preserve );
    }
};
}

TEST_CASE( "Session binds a prepared source through the native private view pipeline",
           "[session-preparation][session-prepared-source]" )
{
    ReadyDocumentFixture fixture;
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    REQUIRE( QFile::rename( fixture.path, fixture.root.filePath( "original.log" ) ) );
    QFile replacement{ fixture.path };
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( "replacement\n" ) == 12 );
    replacement.close();

    auto prepared = fixture.window.prepareFile( fixture.result.source, ReadyDocumentFixture::makeView );
    REQUIRE( prepared );
    CHECK_FALSE( fixture.result.source );
    auto* view = dynamic_cast<CrawlerWidget*>( prepared->view() );
    REQUIRE( view );
    QPointer<LogData> source{ dynamic_cast<LogData*>( prepared->source() ) };
    REQUIRE( source );
    CHECK( source->getNbLine() == 2_lcount );
    CHECK( source->getLineString( 0_lnum ) == QStringLiteral( "one" ) );
    CHECK( prepared->info().documentId == fixture.path );
    SafeQSignalSpy loaded{ view, &CrawlerWidget::loadingFinished };
    CHECK_FALSE( fixture.window.adoptPrepared( prepared ) );
    REQUIRE( prepared->startLoading() );
    CHECK( loaded.count() == 1 ); // Already indexed: only native presentation setup remains.
    CHECK_FALSE( prepared->startLoading() );
    CHECK( view->isFirstLoadDone() );
    CHECK_FALSE( view->isVisible() );
    CHECK( QApplication::focusWidget() == &editor );
    CHECK( fixture.session->openedDocuments().empty() );
    CHECK( fixture.window.openedDocuments().empty() );

    std::unique_ptr<ViewInterface> published{ fixture.window.adoptPrepared( prepared ) };
    REQUIRE( published.get() == view );
    CHECK( fixture.window.openedDocuments().size() == 1 );
    REQUIRE( source->activatePrepared() );
    fixture.window.close( published.get() );
    published.reset();
    CHECK_FALSE( source );
}

TEST_CASE( "Prepared source graphs use session retirement and reject absent worker results",
           "[session-preparation][session-prepared-source]" )
{
    ReadyDocumentFixture fixture;
    auto prepared = fixture.window.prepareFile( fixture.result.source, ReadyDocumentFixture::makeView );
    REQUIRE( prepared );
    QPointer<QObject> view{ dynamic_cast<QObject*>( prepared->view() ) };
    QPointer<LogData> source{ dynamic_cast<LogData*>( prepared->source() ) };
    REQUIRE( prepared->startLoading() );
    REQUIRE( fixture.session->retirePrepared( prepared ) );
    CHECK_FALSE( prepared );
    REQUIRE( waitUiState( [&] { return view.isNull(); } ) );
    CHECK_FALSE( source );
    CHECK( fixture.window.openedDocuments().empty() );
    CHECK( fixture.session->openedDocuments().empty() );
    bool called = false;
    CHECK_FALSE( fixture.window.prepareFile( fixture.result.source, [&]() -> ViewInterface* {
        called = true;
        return ReadyDocumentFixture::makeView();
    } ) );
    CHECK_FALSE( called );
}
