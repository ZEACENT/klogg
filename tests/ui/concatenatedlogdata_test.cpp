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

#include <catch2/catch.hpp>

#include <functional>
#include <stdexcept>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <QTimer>

#include "configuration.h"
#include "test_utils.h"

#include "concatenatedlogdata.h"
#include "logdata.h"

namespace {

class InterpretationTransactionData : public ConcatenatedLogData {
  public:
    using AbstractLogData::notifyInterpretationChanged;
    std::function<void( const char* )> setter;
    QTextCodec* codec = QTextCodec::codecForName( "UTF-8" );
    unsigned setterDepth = 0;
    bool failCodecRead = false;

  protected:
    QTextCodec* doGetDisplayEncoding() const override
    {
        if ( failCodecRead ) {
            throw std::runtime_error{ "Codec observation failed" };
        }
        return codec;
    }
    void doSetDisplayEncoding( const char* encoding ) override
    {
        ++setterDepth;
        try {
            if ( setter ) {
                setter( encoding );
            }
            else {
                codec = QTextCodec::codecForName( encoding );
            }
        } catch ( ... ) {
            --setterDepth;
            throw;
        }
        --setterDepth;
    }
};

QString makeTempFileTemplate( const QString& fileNameTemplate )
{
    const auto tempDir = QDir::cleanPath( QCoreApplication::applicationDirPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) );
    QDir{}.mkpath( tempDir );
    return QDir( tempDir ).filePath( fileNameTemplate );
}

#ifdef Q_OS_WIN
klogg::vector<std::shared_ptr<LogData>>& leakedTestLogData()
{
    static klogg::vector<std::shared_ptr<LogData>> leaked;
    return leaked;
}
#endif

// Helper: create a temp file with specified lines, return (file, shared LogData)
struct TestLogSource {
    struct FileWatchConfigGuard {
        FileWatchConfigGuard()
        {
            auto& config = Configuration::getSynced();
            previousPollingEnabled_ = config.pollingEnabled();
            previousNativeWatchEnabled_ = config.nativeFileWatchEnabled();
            config.setPollingEnabled( false );
            config.setNativeFileWatchEnabled( false );
        }

        ~FileWatchConfigGuard()
        {
            auto& config = Configuration::getSynced();
            config.setPollingEnabled( previousPollingEnabled_ );
            config.setNativeFileWatchEnabled( previousNativeWatchEnabled_ );
        }

        bool previousPollingEnabled_{ false };
        bool previousNativeWatchEnabled_{ false };
    };

    // QTemporaryFile auto-remove can race with background file watcher teardown
    // on Windows, so keep cleanup under test lifecycle control.
    FileWatchConfigGuard fileWatchConfigGuard;
    QTemporaryFile file{ makeTempFileTemplate( QLatin1String( "concat_test_XXXXXX" ) ) };
    QString filePath;
    std::shared_ptr<LogData> logData = std::make_shared<LogData>();
    bool attached = false;

    TestLogSource()
    {
        file.setAutoRemove( false );
    }

    ~TestLogSource()
    {
        shutdown();
    }

    void shutdown()
    {
        if ( logData ) {
            logData->interruptLoading();
            if ( attached ) {
                logData->detachReader();
            }
            QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
#ifdef Q_OS_WIN
            // Windows teardown of LogData/QObject still shows sporadic crashes in
            // repeated-lifecycle UI tests. Keep the instances alive until process
            // exit to avoid the flaky destructor path in test code.
            leakedTestLogData().push_back( std::move( logData ) );
#else
            logData.reset();
#endif
            QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
            attached = false;
        }

        if ( file.isOpen() ) {
            file.close();
        }
    }

    bool create( const QStringList& lines )
    {
        if ( !file.open() ) {
            return false;
        }
        for ( const auto& line : lines ) {
            file.write( line.toUtf8() );
            file.write( "\n" );
        }
        file.flush();
        filePath = QFileInfo{ file }.absoluteFilePath();
        file.close();

        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot( true );

        auto receivedSignal = false;
        auto status = LoadingStatus::Interrupted;

        auto loadingFinishedConnection
            = QObject::connect( logData.get(), &LogData::loadingFinished, &loop,
                                [ & ]( LoadingStatus loadingStatus ) {
                                    status = loadingStatus;
                                    receivedSignal = true;
                                    loop.quit();
                                } );
        auto timeoutConnection
            = QObject::connect( &timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit );

        logData->attachFile( filePath );
        attached = true;

        timeoutTimer.start( 10000 );
        if ( !receivedSignal ) {
            loop.exec();
        }

        // The event loop and timer are stack objects; their destruction will
        // disconnect these temporary lambda connections automatically.
        // Explicit disconnect has been flaky in Windows CI/local runs.

        if ( !receivedSignal || status != LoadingStatus::Successful ) {
            return false;
        }

        const auto expectedLines
            = LinesCount( static_cast<LinesCount::UnderlyingType>( lines.size() ) );
        return waitUiState( [ & ] { return logData->getNbLine() == expectedLines; } );
    }
};

} // namespace

TEST_CASE( "Interpretation transactions defer nested changes until the outer setter exits",
           "[concatenatedlogdata][concatenated-interpretation]" )
{
    InterpretationTransactionData data;
    QSignalSpy changed( &data, &AbstractLogData::interpretationChanged );
    unsigned notificationsInsideSetter = 0;
    QObject::connect( &data, &AbstractLogData::interpretationChanged, &data, [ & ] {
        if ( data.setterDepth != 0 ) {
            ++notificationsInsideSetter;
        }
    } );
    bool throwNested = false;
    SECTION( "nested success" ) {}
    SECTION( "nested failure" ) { throwNested = true; }
    data.setter = [ & ]( const char* encoding ) {
        data.codec = QTextCodec::codecForName( encoding );
        data.notifyInterpretationChanged();
        if ( data.setterDepth == 1 ) {
            data.setDisplayEncoding( "UTF-16LE" );
        }
        else if ( throwNested ) {
            throw std::runtime_error{ "Nested setter failed after changing interpretation" };
        }
    };
    if ( throwNested ) {
        REQUIRE_THROWS_AS( data.setDisplayEncoding( "ISO-8859-1" ), std::runtime_error );
    }
    else {
        data.setDisplayEncoding( "ISO-8859-1" );
    }
    REQUIRE( data.setterDepth == 0 );
    REQUIRE( notificationsInsideSetter == 0 );
    REQUIRE( changed.count() == 1 );
    REQUIRE( data.getDisplayEncoding() == QTextCodec::codecForName( "UTF-16LE" ) );
    changed.clear();
    data.notifyInterpretationChanged();
    REQUIRE( changed.count() == 1 );
}

TEST_CASE( "Interpretation transactions restore notification state after setter exceptions",
           "[concatenatedlogdata][concatenated-interpretation]" )
{
    InterpretationTransactionData data;
    QSignalSpy changed( &data, &AbstractLogData::interpretationChanged );
    unsigned notificationsInsideSetter = 0;
    QObject::connect( &data, &AbstractLogData::interpretationChanged, &data, [ & ] {
        if ( data.setterDepth != 0 ) {
            ++notificationsInsideSetter;
        }
    } );
    bool changeCodec = false;
    bool notifyChildChange = false;
    SECTION( "local codec changed without an explicit notification" ) { changeCodec = true; }
    SECTION( "only a child interpretation changed" ) { notifyChildChange = true; }
    SECTION( "both local codec and child changed" )
    {
        changeCodec = true;
        notifyChildChange = true;
    }
    SECTION( "nothing changed" ) {}
    data.setter = [ & ]( const char* encoding ) {
        if ( changeCodec ) {
            data.codec = QTextCodec::codecForName( encoding );
        }
        if ( notifyChildChange ) {
            data.notifyInterpretationChanged();
        }
        throw std::runtime_error{ "Setter failed" };
    };
    REQUIRE_THROWS_AS( data.setDisplayEncoding( "ISO-8859-1" ), std::runtime_error );
    REQUIRE( data.setterDepth == 0 );
    REQUIRE( notificationsInsideSetter == 0 );
    REQUIRE( changed.count() == ( changeCodec || notifyChildChange ? 1 : 0 ) );
    data.setter = {};
    changed.clear();
    data.setDisplayEncoding( "UTF-16LE" );
    REQUIRE( changed.count() == 1 );
}

TEST_CASE( "Interpretation transactions restore state when codec observation fails",
           "[concatenatedlogdata][concatenated-interpretation]" )
{
    InterpretationTransactionData data;
    QSignalSpy changed( &data, &AbstractLogData::interpretationChanged );
    bool failSetter = false;
    SECTION( "observation failure is reported" ) {}
    SECTION( "the original setter failure is preserved" ) { failSetter = true; }
    data.setter = [ & ]( const char* encoding ) {
        data.codec = QTextCodec::codecForName( encoding );
        data.failCodecRead = true;
        if ( failSetter ) {
            throw std::logic_error{ "Original setter failure" };
        }
    };
    if ( failSetter ) {
        REQUIRE_THROWS_AS( data.setDisplayEncoding( "ISO-8859-1" ), std::logic_error );
    }
    else {
        REQUIRE_THROWS_AS( data.setDisplayEncoding( "ISO-8859-1" ), std::runtime_error );
    }
    REQUIRE( data.setterDepth == 0 );
    REQUIRE( changed.count() == 1 );
    data.failCodecRead = false;
    data.setter = {};
    changed.clear();
    data.setDisplayEncoding( "UTF-8" );
    REQUIRE( changed.count() == 1 );
}

TEST_CASE( "Concatenated data forwards interpretation changes from every child",
           "[concatenatedlogdata][concatenated-interpretation]" )
{
    TestLogSource first, second;
    REQUIRE( first.create( { "alpha" } ) );
    REQUIRE( second.create( { "beta" } ) );
    first.logData->setDisplayEncoding( "UTF-8" );
    second.logData->setDisplayEncoding( "UTF-8" );
    auto concat = std::make_unique<ConcatenatedLogData>();
    concat->addSource( first.logData );
    concat->addSource( second.logData );
    QSignalSpy childChanged( first.logData.get(), &AbstractLogData::interpretationChanged );
    QSignalSpy changed( concat.get(), &AbstractLogData::interpretationChanged );
    REQUIRE( childChanged.isValid() );
    REQUIRE( changed.isValid() );
    first.logData->setDisplayEncoding( "ISO-8859-1" );
    REQUIRE( childChanged.count() == 1 );
    REQUIRE( changed.count() == 1 );
    second.logData->setDisplayEncoding( "ISO-8859-1" );
    REQUIRE( changed.count() == 2 );
    first.logData->setDisplayEncoding( "ISO-8859-1" );
    REQUIRE( changed.count() == 2 );
    concat.reset();
    first.logData->setDisplayEncoding( "UTF-8" );
    REQUIRE( childChanged.count() == 2 );
    REQUIRE( changed.count() == 2 );
}

TEST_CASE( "Setting a concatenated display encoding emits one interpretation change",
           "[concatenatedlogdata][concatenated-interpretation]" )
{
    TestLogSource first, second;
    REQUIRE( first.create( { "alpha" } ) );
    REQUIRE( second.create( { "beta" } ) );
    first.logData->setDisplayEncoding( "UTF-8" );
    second.logData->setDisplayEncoding( "UTF-8" );
    ConcatenatedLogData concat;
    concat.addSource( first.logData );
    concat.addSource( second.logData );
    SECTION( "both children change encoding" )
    {
        QSignalSpy changed( &concat, &AbstractLogData::interpretationChanged );
        REQUIRE( changed.isValid() );
        concat.setDisplayEncoding( "ISO-8859-1" );
        REQUIRE( changed.count() == 1 );
    }
    SECTION( "only a later child changes encoding" )
    {
        second.logData->setDisplayEncoding( "ISO-8859-1" );
        QSignalSpy changed( &concat, &AbstractLogData::interpretationChanged );
        REQUIRE( changed.isValid() );
        concat.setDisplayEncoding( "UTF-8" );
        REQUIRE( changed.count() == 1 );
    }
}

TEST_CASE( "Concatenated snapshots freeze content labels and global row mappings",
           "[concatenatedlogdata][concatenated-snapshot]" )
{
    TestLogSource first, second;
    REQUIRE( first.create( { "first old", "first tail" } ) );
    REQUIRE( second.create( { "second old", "second tail" } ) );
    ConcatenatedLogData concat;
    concat.addSource( first.logData );
    concat.addSource( second.logData );
    const auto snapshot = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot );
    REQUIRE( snapshot->lineCount() == 4_lcount );
    REQUIRE( snapshot->lineString( 0_lnum ) == "first old" );
    REQUIRE( snapshot->lineString( 2_lnum ) == "second old" );
    REQUIRE( snapshot->row( 0_lnum ).sourceLineNumber == 1_lnum );
    REQUIRE( snapshot->row( 2_lnum ).sourceLineNumber == 1_lnum );
    REQUIRE( snapshot->row( 3_lnum ).sourceLineNumber == 2_lnum );
    for ( uint64_t index = 0; index < 4; ++index ) {
        const LineNumber line{ index };
        const auto row = snapshot->row( line );
        REQUIRE( row.copyable );
        REQUIRE_FALSE( row.match );
        REQUIRE( row.identity.origin == klogg::LogRowId::Origin::Concatenated );
        REQUIRE( snapshot->rowForIdentity( row.identity ) == OptionalLineNumber{ line } );
        REQUIRE( snapshot->presentedRowForIdentity( row.identity ) == OptionalLineNumber{ line } );
        REQUIRE( concat.rowForIdentity( row.identity ) == OptionalLineNumber{ line } );
    }
    const auto oldFirst = snapshot->row( 0_lnum ).identity;
    const auto oldSecond = snapshot->row( 2_lnum ).identity;
    {
        QFile file( first.filePath );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( file.write( "replacement\nextra\nlast\n" ) == 23 );
    }
    SafeQSignalSpy finished( first.logData.get(), SIGNAL( loadingFinished( LoadingStatus ) ) );
    first.logData->reload();
    REQUIRE( finished.safeWait( 10000 ) );
    concat.rebuildIndex();
    REQUIRE( concat.getNbLine() == 5_lcount );
    REQUIRE( concat.getLineString( 0_lnum ) == "replacement" );
    REQUIRE( snapshot->lineCount() == 4_lcount );
    REQUIRE( snapshot->lineString( 0_lnum ) == "first old" );
    REQUIRE( snapshot->lineString( 2_lnum ) == "second old" );
    REQUIRE_FALSE( concat.rowForIdentity( oldFirst ) );
    REQUIRE( concat.rowForIdentity( oldSecond ) == OptionalLineNumber{ 3_lnum } );
    REQUIRE( snapshot->rowForIdentity( oldSecond ) == OptionalLineNumber{ 2_lnum } );
    const auto latest = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( latest );
    REQUIRE( latest->row( 0_lnum ).identity != oldFirst );
    REQUIRE( latest->row( 3_lnum ).identity == oldSecond );
    REQUIRE( latest->rowForIdentity( oldSecond ) == OptionalLineNumber{ 3_lnum } );
}

TEST_CASE( "Concatenated snapshot identities distinguish repeated child occurrences",
           "[concatenatedlogdata][concatenated-snapshot]" )
{
    TestLogSource source;
    REQUIRE( source.create( { "alpha", "beta" } ) );
    ConcatenatedLogData concat;
    concat.addSource( source.logData );
    concat.addSource( source.logData );
    const auto first = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( first );
    REQUIRE( first->lineCount() == 4_lcount );
    const auto firstId = first->row( 0_lnum ).identity;
    const auto repeatedId = first->row( 2_lnum ).identity;
    REQUIRE( firstId != repeatedId );
    REQUIRE( first->row( 0_lnum ).sourceLineNumber == 1_lnum );
    REQUIRE( first->row( 2_lnum ).sourceLineNumber == 1_lnum );
    REQUIRE( first->rowForIdentity( firstId ) == OptionalLineNumber{ 0_lnum } );
    REQUIRE( first->rowForIdentity( repeatedId ) == OptionalLineNumber{ 2_lnum } );
    REQUIRE( concat.rowForIdentity( repeatedId ) == OptionalLineNumber{ 2_lnum } );
    concat.addSource( source.logData );
    const auto next = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( next );
    REQUIRE( next->lineCount() == 6_lcount );
    REQUIRE( next->row( 0_lnum ).identity == firstId );
    REQUIRE( next->row( 2_lnum ).identity == repeatedId );
    REQUIRE( next->row( 4_lnum ).identity != repeatedId );
    REQUIRE( first->lineCount() == 4_lcount );
    REQUIRE_THROWS_AS( first->row( 4_lnum ), klogg::SnapshotError );
    REQUIRE_THROWS_AS( first->lineString( 4_lnum ), klogg::SnapshotError );
    ConcatenatedLogData other;
    other.addSource( source.logData );
    const auto otherSnapshot = other.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( otherSnapshot );
    REQUIRE( otherSnapshot->row( 0_lnum ).identity != firstId );
    REQUIRE_FALSE( other.rowForIdentity( firstId ) );
    REQUIRE_FALSE( otherSnapshot->rowForIdentity( firstId ) );
}

TEST_CASE( "Concatenated snapshots preserve child tail epochs across append",
           "[concatenatedlogdata][concatenated-snapshot]" )
{
    TestLogSource source;
    REQUIRE( source.create( { "head", "tail" } ) );
    {
        QFile file( source.filePath );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( file.write( "head\ntail" ) == 9 );
    }
    SafeQSignalSpy reloaded( source.logData.get(), SIGNAL( loadingFinished( LoadingStatus ) ) );
    source.logData->reload();
    REQUIRE( reloaded.safeWait( 10000 ) );
    ConcatenatedLogData concat;
    concat.addSource( source.logData );
    const auto childBefore = source.logData->searchableSnapshot();
    REQUIRE( childBefore );
    const auto snapshot = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( snapshot );
    const auto headId = snapshot->row( 0_lnum ).identity;
    const auto tailId = snapshot->row( 1_lnum ).identity;
    REQUIRE( childBefore->row( 1_lnum ).identity.member != 0 );
    REQUIRE( tailId.member == childBefore->row( 1_lnum ).identity.member );
    REQUIRE( concat.rowForIdentity( tailId ) == OptionalLineNumber{ 1_lnum } );
    {
        QFile file( source.filePath );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Append ) );
        REQUIRE( file.write( "\nnext\n" ) == 6 );
    }
    SafeQSignalSpy appended( source.logData.get(), SIGNAL( loadingFinished( LoadingStatus ) ) );
    // Drive the actual change-check/partial-index path without watcher timing.
    REQUIRE( QMetaObject::invokeMethod( source.logData.get(), "fileChangedOnDisk",
                                       Qt::DirectConnection, Q_ARG( QString, source.filePath ) ) );
    REQUIRE( appended.safeWait( 10000 ) );
    concat.rebuildIndex();
    const auto childAfter = source.logData->searchableSnapshot();
    REQUIRE( childAfter );
    REQUIRE( childAfter->row( 0_lnum ).identity.incarnation
             == childBefore->row( 0_lnum ).identity.incarnation );
    const auto latest = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( latest );
    REQUIRE( latest->lineCount() == 3_lcount );
    REQUIRE( latest->row( 0_lnum ).identity == headId );
    REQUIRE( latest->row( 1_lnum ).identity.incarnation == tailId.incarnation );
    REQUIRE( latest->row( 1_lnum ).identity.member == childAfter->row( 1_lnum ).identity.member );
    REQUIRE( latest->row( 1_lnum ).identity.member != tailId.member );
    REQUIRE( concat.rowForIdentity( headId ) == OptionalLineNumber{ 0_lnum } );
    REQUIRE_FALSE( concat.rowForIdentity( tailId ) );
    REQUIRE( snapshot->lineCount() == 2_lcount );
    REQUIRE( snapshot->lineString( 1_lnum ) == "tail" );
    REQUIRE( snapshot->rowForIdentity( tailId ) == OptionalLineNumber{ 1_lnum } );
}

TEST_CASE( "Concatenated snapshots outlive their owner and all child log objects",
           "[concatenatedlogdata][concatenated-snapshot]" )
{
    TestLogSource source;
    REQUIRE( source.create( { "owned bytes", "still readable" } ) );
    std::weak_ptr<LogData> child = source.logData;
    klogg::LogSnapshot::Ptr snapshot;
    {
        ConcatenatedLogData concat;
        concat.addSource( source.logData );
        snapshot = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
        REQUIRE( snapshot );
    }
    // This contract deliberately destroys the child even on Windows, rather
    // than using the legacy fixture's process-lifetime retention workaround.
    source.logData->interruptLoading();
    source.logData->detachReader();
    source.attached = false;
    source.logData.reset();
    REQUIRE( child.expired() );
    // QTemporaryFile::close() retains its native handle, without delete sharing
    // on Windows. Remove through the owning object to release that handle first.
    REQUIRE( source.file.remove() );
    REQUIRE_FALSE( QFileInfo::exists( source.filePath ) );
    REQUIRE( snapshot->lineString( 0_lnum ) == "owned bytes" );
    REQUIRE( snapshot->lineString( 1_lnum ) == "still readable" );
    const auto identity = snapshot->row( 1_lnum ).identity;
    REQUIRE( snapshot->rowForIdentity( identity ) == OptionalLineNumber{ 1_lnum } );
}

TEST_CASE( "Concatenated snapshot acquisition rejects unavailable children without partial rows",
           "[concatenatedlogdata][concatenated-snapshot]" )
{
    ConcatenatedLogData concat;
    const auto empty = concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( empty );
    REQUIRE( empty->lineCount() == 0_lcount );
    REQUIRE_THROWS_AS( empty->row( 0_lnum ), klogg::SnapshotError );
    TestLogSource source;
    REQUIRE( source.create( { "ready" } ) );
    concat.addSource( source.logData );
    concat.addSource( std::make_shared<LogData>() );
    REQUIRE_FALSE( concat.createReadSnapshot( klogg::SnapshotScope::PresentedRows ) );
}

SCENARIO( "empty concatenated log data", "[concatenatedlogdata]" )
{
    GIVEN( "a default-constructed ConcatenatedLogData" )
    {
        ConcatenatedLogData concat;

        THEN( "it has zero lines" )
        {
            REQUIRE( concat.getNbLine() == 0_lcount );
        }

        THEN( "it has zero max length" )
        {
            REQUIRE( concat.getMaxLength() == 0_length );
        }

        THEN( "sourceCount is zero" )
        {
            REQUIRE( concat.sourceCount() == 0 );
        }

        THEN( "mapToSource returns default for any line" )
        {
            auto [idx, localLine] = concat.mapToSource( 0_lnum );
            REQUIRE( idx == 0 );
            REQUIRE( localLine == 0_lnum );
        }
    }
}

SCENARIO( "single source concatenated log data", "[concatenatedlogdata]" )
{
    GIVEN( "a ConcatenatedLogData with one source" )
    {
        TestLogSource src;
        REQUIRE( src.create( { "alpha", "bravo", "charlie" } ) );

        ConcatenatedLogData concat;
        concat.addSource( src.logData );

        THEN( "sourceCount is 1" )
        {
            REQUIRE( concat.sourceCount() == 1 );
        }

        THEN( "total lines match source" )
        {
            REQUIRE( concat.getNbLine() == 3_lcount );
        }

        THEN( "maxLength matches source" )
        {
            REQUIRE( concat.getMaxLength() == src.logData->getMaxLength() );
        }

        THEN( "getLineString returns correct content" )
        {
            REQUIRE( concat.getLineString( 0_lnum ) == "alpha" );
            REQUIRE( concat.getLineString( 1_lnum ) == "bravo" );
            REQUIRE( concat.getLineString( 2_lnum ) == "charlie" );
        }

        THEN( "getExpandedLineString returns correct content" )
        {
            REQUIRE( concat.getExpandedLineString( 0_lnum ) == "alpha" );
        }

        THEN( "getLineLength returns correct lengths" )
        {
            REQUIRE( concat.getLineLength( 0_lnum ) == LineLength( 5 ) );
            REQUIRE( concat.getLineLength( 2_lnum ) == LineLength( 7 ) );
        }

        THEN( "mapToSource maps correctly" )
        {
            auto [idx, localLine] = concat.mapToSource( 0_lnum );
            REQUIRE( idx == 0 );
            REQUIRE( localLine == 0_lnum );

            auto [idx2, localLine2] = concat.mapToSource( 2_lnum );
            REQUIRE( idx2 == 0 );
            REQUIRE( localLine2 == 2_lnum );
        }

        THEN( "sourceAt returns the correct source" )
        {
            REQUIRE( concat.sourceAt( 0 ) == src.logData );
        }

        THEN( "getLines returns multiple lines" )
        {
            auto lines = concat.getLines( 0_lnum, 3_lcount );
            REQUIRE( lines.size() == 3 );
            REQUIRE( lines[ 0 ] == "alpha" );
            REQUIRE( lines[ 1 ] == "bravo" );
            REQUIRE( lines[ 2 ] == "charlie" );
        }

        THEN( "getExpandedLines returns multiple lines" )
        {
            auto lines = concat.getExpandedLines( 1_lnum, 2_lcount );
            REQUIRE( lines.size() == 2 );
            REQUIRE( lines[ 0 ] == "bravo" );
            REQUIRE( lines[ 1 ] == "charlie" );
        }
    }
}

SCENARIO( "multi-source concatenated log data", "[concatenatedlogdata]" )
{
    GIVEN( "a ConcatenatedLogData with three sources" )
    {
        TestLogSource src1, src2, src3;
        REQUIRE( src1.create( { "aaa", "bbb" } ) );
        REQUIRE( src2.create( { "ccc", "ddd", "eee" } ) );
        REQUIRE( src3.create( { "fff" } ) );

        ConcatenatedLogData concat;
        concat.addSource( src1.logData );
        concat.addSource( src2.logData );
        concat.addSource( src3.logData );

        THEN( "sourceCount is 3" )
        {
            REQUIRE( concat.sourceCount() == 3 );
        }

        THEN( "total lines is sum of all sources" )
        {
            REQUIRE( concat.getNbLine() == 6_lcount );
        }

        WHEN( "reading lines across source boundaries" )
        {
            THEN( "lines from first source are correct" )
            {
                REQUIRE( concat.getLineString( 0_lnum ) == "aaa" );
                REQUIRE( concat.getLineString( 1_lnum ) == "bbb" );
            }

            THEN( "lines from second source are correct" )
            {
                REQUIRE( concat.getLineString( 2_lnum ) == "ccc" );
                REQUIRE( concat.getLineString( 3_lnum ) == "ddd" );
                REQUIRE( concat.getLineString( 4_lnum ) == "eee" );
            }

            THEN( "lines from third source are correct" )
            {
                REQUIRE( concat.getLineString( 5_lnum ) == "fff" );
            }
        }

        WHEN( "mapping lines to sources" )
        {
            THEN( "first source lines map correctly" )
            {
                auto [idx, local] = concat.mapToSource( 0_lnum );
                REQUIRE( idx == 0 );
                REQUIRE( local == 0_lnum );

                auto [idx2, local2] = concat.mapToSource( 1_lnum );
                REQUIRE( idx2 == 0 );
                REQUIRE( local2 == 1_lnum );
            }

            THEN( "second source lines map correctly" )
            {
                auto [idx, local] = concat.mapToSource( 2_lnum );
                REQUIRE( idx == 1 );
                REQUIRE( local == 0_lnum );

                auto [idx2, local2] = concat.mapToSource( 4_lnum );
                REQUIRE( idx2 == 1 );
                REQUIRE( local2 == 2_lnum );
            }

            THEN( "third source lines map correctly" )
            {
                auto [idx, local] = concat.mapToSource( 5_lnum );
                REQUIRE( idx == 2 );
                REQUIRE( local == 0_lnum );
            }
        }

        WHEN( "reading lines spanning source boundaries" )
        {
            auto lines = concat.getLines( 1_lnum, 3_lcount );

            THEN( "returns lines from multiple sources" )
            {
                REQUIRE( lines.size() == 3 );
                REQUIRE( lines[ 0 ] == "bbb" );
                REQUIRE( lines[ 1 ] == "ccc" );
                REQUIRE( lines[ 2 ] == "ddd" );
            }
        }

        WHEN( "checking maxLength" )
        {
            THEN( "returns max across all sources" )
            {
                // All lines are 3 chars, so maxLength should be 3
                REQUIRE( concat.getMaxLength() == LineLength( 3 ) );
            }
        }

        WHEN( "checking line lengths" )
        {
            THEN( "returns correct length for each line" )
            {
                for ( LineNumber::UnderlyingType i = 0; i < 6; ++i ) {
                    REQUIRE( concat.getLineLength( LineNumber( i ) ) == LineLength( 3 ) );
                }
            }
        }

        WHEN( "sourceAt returns correct sources" )
        {
            REQUIRE( concat.sourceAt( 0 ) == src1.logData );
            REQUIRE( concat.sourceAt( 1 ) == src2.logData );
            REQUIRE( concat.sourceAt( 2 ) == src3.logData );
        }
    }
}

SCENARIO( "concatenated log data with varying line lengths", "[concatenatedlogdata]" )
{
    GIVEN( "sources with different line lengths" )
    {
        TestLogSource src1, src2;
        REQUIRE( src1.create( { "short", "a]" } ) );
        REQUIRE( src2.create(
            { "this is a much longer line for testing max length calculation" } ) );

        ConcatenatedLogData concat;
        concat.addSource( src1.logData );
        concat.addSource( src2.logData );

        THEN( "maxLength reflects the longest line across all sources" )
        {
            auto longestLen = src2.logData->getMaxLength();
            REQUIRE( concat.getMaxLength() == longestLen );
        }

        THEN( "individual line lengths are correct" )
        {
            REQUIRE( concat.getLineLength( 0_lnum ) == LineLength( 5 ) );   // "short"
            REQUIRE( concat.getLineLength( 1_lnum ) == LineLength( 2 ) );   // "a]"
            REQUIRE( concat.getLineLength( 2_lnum ) == src2.logData->getLineLength( 0_lnum ) );
        }
    }
}

SCENARIO( "concatenated log data encoding delegation", "[concatenatedlogdata]" )
{
    GIVEN( "a ConcatenatedLogData with sources" )
    {
        TestLogSource src1, src2;
        REQUIRE( src1.create( { "line1" } ) );
        REQUIRE( src2.create( { "line2" } ) );

        ConcatenatedLogData concat;
        concat.addSource( src1.logData );
        concat.addSource( src2.logData );

        THEN( "getDisplayEncoding returns first source encoding" )
        {
            auto encoding = concat.getDisplayEncoding();
            REQUIRE( encoding != nullptr );
            REQUIRE( encoding == src1.logData->getDisplayEncoding() );
        }
    }
}

SCENARIO( "concatenated log data attach/detach reader", "[concatenatedlogdata]" )
{
    GIVEN( "a ConcatenatedLogData with sources" )
    {
        TestLogSource src1, src2;
        REQUIRE( src1.create( { "line1" } ) );
        REQUIRE( src2.create( { "line2" } ) );

        ConcatenatedLogData concat;
        concat.addSource( src1.logData );
        concat.addSource( src2.logData );

        WHEN( "attaching and detaching readers" )
        {
            THEN( "does not crash" )
            {
                concat.attachReader();
                concat.detachReader();
                // If we get here without crashing, the test passes
                REQUIRE( true );
            }
        }
    }
}

SCENARIO( "concatenated log data repeated lifecycle", "[concatenatedlogdata][stability]" )
{
    constexpr int Iterations = 20;

    for ( int iteration = 0; iteration < Iterations; ++iteration ) {
        TestLogSource src1, src2, src3;
        REQUIRE( src1.create( { "aaa", "bbb" } ) );
        REQUIRE( src2.create( { "ccc", "ddd", "eee" } ) );
        REQUIRE( src3.create( { "fff" } ) );

        {
            ConcatenatedLogData concat;
            concat.addSource( src1.logData );
            concat.addSource( src2.logData );
            concat.addSource( src3.logData );

            REQUIRE( concat.sourceCount() == 3 );
            REQUIRE( concat.getNbLine() == 6_lcount );
            REQUIRE( concat.getLineString( 0_lnum ) == "aaa" );
            REQUIRE( concat.getLineString( 5_lnum ) == "fff" );

            concat.attachReader();
            concat.detachReader();
        }

        QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
        QTest::qWait( 1 );
    }
}
