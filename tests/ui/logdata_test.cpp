/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include <iostream>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QThread>
#include <QFileInfo>

#include "file_write_helper.h"
#include "log.h"
#include "test_utils.h"

#include "logdata.h"

static const qint64 SL_NB_LINES = 500LL;
static const qint64 VBL_NB_LINES = 50000LL;

namespace {

QString makeTempFileTemplate( const QString& fileNameTemplate )
{
    const auto tempDir = QDir::cleanPath( QCoreApplication::applicationDirPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) );
    QDir{}.mkpath( tempDir );
    return QDir( tempDir ).filePath( fileNameTemplate );
}

bool writeFileInline( const QString& fileName, int numberOfLines, WriteFileModification flag )
{
    QFile file{ fileName };
    const auto openedForWrite
        = file.open( QIODevice::Unbuffered | QIODevice::WriteOnly | QIODevice::Append );
    if ( !openedForWrite ) {
        LOG_ERROR << "Inline write helper failed to open file for write "
                  << file.errorString().toStdString();
        return false;
    }

    if ( flag == WriteFileModification::Truncate ) {
        file.resize( 0 );
    }

    if ( flag == WriteFileModification::StartWithPartialLineEnd ) {
        file.write( partial_line_end, static_cast<qint64>( qstrlen( partial_line_end ) ) );
    }

    char newLine[ 90 ];
    for ( int i = 0; i < numberOfLines; ++i ) {
        snprintf( newLine, 89,
                  "LOGDATA is a part of glogg, we are going to test it thoroughly, this is "
                  "line %06d\n",
                  i );
        file.write( newLine, static_cast<qint64>( qstrlen( newLine ) ) );

        if ( flag == WriteFileModification::DelayClosingFile ) {
            QThread::sleep( 2 );
        }
    }

    if ( flag == WriteFileModification::EndWithPartialLineBegin ) {
        file.write( partial_line_begin, static_cast<qint64>( qstrlen( partial_line_begin ) ) );
    }

    file.flush();
    file.close();

    const auto openedForRead
        = file.open( QIODevice::Unbuffered | QIODevice::ReadOnly | QIODevice::Append );
    if ( !openedForRead ) {
        LOG_ERROR << "Inline write helper failed to reopen file for read "
                  << file.errorString().toStdString();
        return false;
    }
    file.close();
    return true;
}

class WriteFileThread : public QThread {
    Q_OBJECT
  public:
    WriteFileThread( QFile* file, int numberOfLines = 200,
                     WriteFileModification flag = WriteFileModification::None )
        : file_{ file }
        , numberOfLines_{ numberOfLines }
        , flag_{ flag }
    {
    }

    bool isSucceeded() const
    {
        return result_ == 0;
    }

  protected:
    void run() override
    {
        QString writeHelper = QCoreApplication::applicationDirPath() + QDir::separator()
                              + QLatin1String( "file_write_helper" );
#ifdef Q_OS_WIN
        writeHelper += QLatin1String( ".exe" );
#endif
        QStringList arguments;
        arguments << file_->fileName() << QString::number( numberOfLines_ )
                  << QString::number( static_cast<uint8_t>( flag_ ) );

        LOG_INFO << "Executing write helper " << writeHelper << " " << arguments;
        QProcess writeHelperProcess;
        writeHelperProcess.start( writeHelper, arguments );
        if ( !writeHelperProcess.waitForStarted() ) {
            LOG_WARNING << "Write helper failed to start, falling back to inline writer: "
                        << writeHelperProcess.errorString().toStdString();
            result_ = writeFileInline( file_->fileName(), numberOfLines_, flag_ ) ? 0 : -1;
            return;
        }
        writeHelperProcess.waitForFinished( -1 );
        result_ = writeHelperProcess.exitCode();
        LOG_INFO << "Write helper result " << result_ << ", exit status "
                 << writeHelperProcess.exitStatus();
    }

  private:
    QFile* file_;
    int numberOfLines_;
    WriteFileModification flag_;

    int result_{};
};

#ifdef _WIN32
WriteFileThread* writeDataToFileBackground( QFile& file, int numberOfLines = 200,
                                            WriteFileModification flag
                                            = WriteFileModification::None )
{
    auto thread = new WriteFileThread( &file, numberOfLines, flag );
    thread->start();
    return thread;
}
#endif
void writeDataToFile( QFile& file, int numberOfLines = 200,
                      WriteFileModification flag = WriteFileModification::None )
{
    auto thread = new WriteFileThread( &file, numberOfLines, flag );
    thread->start();
    thread->wait();
    REQUIRE( thread->isSucceeded() );
    thread->deleteLater();
}
} // namespace

TEST_CASE( "Logdata decoding lines", "[logdata]" )
{
    QTemporaryFile file{ makeTempFileTemplate( QLatin1String( "testdecode_XXXXXX" ) ) };
    if ( file.open() ) {
        writeDataToFile( file );
    }

    writeDataToFile( file, 199, WriteFileModification::EndWithPartialLineBegin );

    LogData logData;

    auto finishedSpy
        = std::make_unique<SafeQSignalSpy>( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
    QThread* loadingFinishedThread = nullptr;
    QObject::connect(
        &logData, &LogData::loadingFinished, &logData,
        [ &loadingFinishedThread ]( LoadingStatus ) {
            loadingFinishedThread = QThread::currentThread();
        },
        Qt::DirectConnection );

    logData.attachFile( QFileInfo{ file }.absoluteFilePath() );

    REQUIRE( finishedSpy->safeWait() );
    REQUIRE( finishedSpy->count() == 1 );
    REQUIRE( loadingFinishedThread == logData.thread() );
    REQUIRE( logData.getNbLine() == 400_lcount );

    const auto rawLines = logData.getLinesRaw( 200_lnum, 200_lcount );
    REQUIRE( rawLines.startLine == 200_lnum );
    REQUIRE( rawLines.endOfLines.size() == 200 );

    const auto utf8View = rawLines.buildUtf8View();

    REQUIRE( rawLines.endOfLines.size() == utf8View.size() );
}

TEST_CASE( "Logdata reading changing file", "[logdata]" )
{

    LogData logData;

    SafeQSignalSpy changedSpy( &logData, SIGNAL( fileChanged( MonitoredFileStatus ) ) );

    // Generate a small file
    QTemporaryFile file{ makeTempFileTemplate( QLatin1String( "testdecode_XXXXXX" ) ) };
    if ( file.open() ) {
        writeDataToFile( file );
    }

    SafeQSignalSpy finishedSpy( &logData, SIGNAL( loadingFinished( LoadingStatus ) ) );
#ifdef Q_OS_WIN
    WriteFileThread* backgroundWriteThread = nullptr;
#endif
    // Start loading it
    logData.attachFile( QFileInfo{ file }.absoluteFilePath() );
    waitUiState( [ &logData ] { return logData.getNbLine() == 200_lcount; } );
    REQUIRE( finishedSpy.safeWait() );
    REQUIRE( finishedSpy.count() == 1 );

    // Check we have the small file
    REQUIRE( logData.getNbLine() == 200_lcount );
    REQUIRE( logData.getMaxLength() == LineLength( SL_LINE_LENGTH ) );
    REQUIRE( logData.getFileSize() == 200 * ( SL_LINE_LENGTH + 1LL ) );

    // Add some data to it
    if ( file.isOpen() ) {
        // To test the edge case when the final line is not complete
#ifdef Q_OS_WIN
        backgroundWriteThread
            = writeDataToFileBackground( file, 200, WriteFileModification::EndWithPartialLineBegin );
#else
        writeDataToFile( file, 200, WriteFileModification::EndWithPartialLineBegin );
#endif
    }

    waitUiState( [ &logData ] { return logData.getNbLine() == 401_lcount; } );
#ifdef Q_OS_WIN
    REQUIRE( backgroundWriteThread != nullptr );
    backgroundWriteThread->wait();
    REQUIRE( backgroundWriteThread->isSucceeded() );
    delete backgroundWriteThread;
    backgroundWriteThread = nullptr;
#endif

    // Check we have a bigger file
    REQUIRE( changedSpy.count() >= 1 );
    REQUIRE( logData.getNbLine() == 401_lcount );
    REQUIRE( logData.getMaxLength() == LineLength( SL_LINE_LENGTH ) );
    REQUIRE( logData.getFileSize()
             == (qint64)( 400 * ( SL_LINE_LENGTH + 1LL ) + strlen( partial_line_begin ) ) );

    {
        // Add a couple more lines, including the end of the unfinished one.
        if ( file.isOpen() ) {
#ifdef Q_OS_WIN
            backgroundWriteThread = writeDataToFileBackground(
                file, 20, WriteFileModification::StartWithPartialLineEnd );
#else
            writeDataToFile( file, 20, WriteFileModification::StartWithPartialLineEnd );
#endif
        }

        waitUiState( [ &logData ] { return logData.getNbLine() == 421_lcount; } );
#ifdef Q_OS_WIN
        REQUIRE( backgroundWriteThread != nullptr );
        backgroundWriteThread->wait();
        REQUIRE( backgroundWriteThread->isSucceeded() );
        delete backgroundWriteThread;
        backgroundWriteThread = nullptr;
#endif

        // Check we have a bigger file
        REQUIRE( changedSpy.count() >= 2 );
        REQUIRE( logData.getNbLine() == 421_lcount );
        REQUIRE( logData.getMaxLength() == LineLength( SL_LINE_LENGTH ) );
        REQUIRE( logData.getFileSize()
                 == (qint64)( 420 * ( SL_LINE_LENGTH + 1LL ) + strlen( partial_line_begin )
                              + strlen( partial_line_end ) ) );
    }

    {
        // Truncate the file
        writeDataToFile( file, 0, WriteFileModification::Truncate );

        waitUiState( [ &logData ] { return logData.getNbLine() == 0_lcount; } );

        // Check we have an empty file
        REQUIRE( changedSpy.count() >= 3 );
        REQUIRE( logData.getNbLine() == 0_lcount );
        REQUIRE( logData.getMaxLength().get() == 0 );
        REQUIRE( logData.getFileSize() == 0LL );
    }
}

SCENARIO( "Attaching log data to files", "[logdata]" )
{

    GIVEN( "Small and big files" )
    {

        QTemporaryFile smallFile{
            makeTempFileTemplate( QLatin1String( "logdata_test_small_XXXXXX" ) ) };
        QTemporaryFile bigFile{
            makeTempFileTemplate( QLatin1String( "logdata_test_big_XXXXXX" ) ) };

        if ( smallFile.open() ) {
            writeDataToFile( smallFile, SL_NB_LINES );
        }

        if ( bigFile.open() ) {
            writeDataToFile( bigFile, VBL_NB_LINES );
        }

        WHEN( "Interrupt loading" )
        {
            LogData log_data;
            SafeQSignalSpy endSpy( &log_data, SIGNAL( loadingFinished( LoadingStatus ) ) );

            // Start loading the VBL
            log_data.attachFile( QFileInfo{ bigFile }.absoluteFilePath() );

            // Immediately interrupt the loading
            log_data.interruptLoading();

            REQUIRE( endSpy.safeWait( 10000 ) );

            THEN( "No file is attached" )
            {
                // Check we have an empty file
                REQUIRE( endSpy.count() == 1 );
                QList<QVariant> arguments = endSpy.takeFirst();
                REQUIRE( arguments.at( 0 ).toInt()
                         == static_cast<int>( LoadingStatus::Interrupted ) );

                REQUIRE( log_data.getNbLine() == 0_lcount );
                REQUIRE( log_data.getMaxLength().get() == 0 );
                REQUIRE( log_data.getFileSize() == 0LL );
            }
        }

        WHEN( "Try to reattach" )
        {
            LogData log_data;
            SafeQSignalSpy endSpy( &log_data, SIGNAL( loadingFinished( LoadingStatus ) ) );

            log_data.attachFile( QFileInfo{ smallFile }.absoluteFilePath() );
            endSpy.safeWait( 10000 );

            THEN( "Throws" )
            {
                CHECK_THROWS_AS( log_data.attachFile( QFileInfo{ bigFile }.absoluteFilePath() ), CantReattachErr );
            }
        }
    }
}

#include "logdata_test.moc"
