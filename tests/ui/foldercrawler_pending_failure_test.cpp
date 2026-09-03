#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

#include "foldercrawlerwidget.h"
#include "loadingstatus.h"
#include "logdata.h"

namespace {

QString makeFile( QTemporaryDir& dir, const char* name )
{
    const auto path = dir.filePath( QString::fromUtf8( name ) );
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    for ( int line = 0; line < 12; ++line ) {
        const auto text = line == 2 || line == 8 ? QByteArrayLiteral( "ERROR\n" )
                                                : QByteArrayLiteral( "plain\n" );
        REQUIRE( file.write( text ) == text.size() );
    }
    file.close();
    return path;
}

bool waitFor( const std::function<bool()>& predicate, int timeoutMs = 5000 )
{
    QElapsedTimer timer;
    timer.start();
    while ( !predicate() && timer.elapsed() < timeoutMs ) {
        QTest::qWait( 10 );
    }
    return predicate();
}

} // namespace

TEST_CASE( "FolderCrawlerWidget retries a same-file selection queued behind a failed pending load",
           "[folder][pending][regression]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = makeFile( dir, "a.log" );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ path } );
    widget.searchFor( QStringLiteral( "ERROR" ) );
    REQUIRE( waitFor( [ &widget ] { return !widget.isSearchActive(); } ) );
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );
    const auto secondLine = widget.folderResults()->sourceForLine( 2_lnum ).localLine;
    REQUIRE( secondLine == 8_lnum );

    widget.selectResultRow( 1_lnum );
    auto pending = widget.pendingMainDataForTest();
    REQUIRE( pending != nullptr );
    const std::weak_ptr<LogData> abandonedPending = pending;

    // Queue failure before the second click, but do not process the event loop.
    // The new click must survive cleanup of the terminal pending load.
    REQUIRE( QMetaObject::invokeMethod(
        pending.get(), "loadingFinished", Qt::QueuedConnection,
        Q_ARG( LoadingStatus, LoadingStatus::Interrupted ) ) );
    pending->interruptLoading();
    widget.selectResultRow( 2_lnum );
    pending.reset();

    REQUIRE( waitFor( [ &widget, &path ] { return widget.currentMainFilePath() == path; } ) );
    QTest::qWait( 200 );
    REQUIRE( abandonedPending.expired() );
    REQUIRE( widget.currentMainViewLine() == secondLine );
}
