/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2021 Anton Filimonov and other contributors
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

#include "log.h"
#include <QtGlobal>
#include <qapplication.h>
#include <qthreadpool.h>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif // _WIN32

#ifdef Q_OS_MAC
// macOS uncaught-NSException diagnostic.
//
// Qt's QCocoaEventDispatcher turns an ObjC NSException thrown during event
// dispatch into a C++ exception that propagates through __cxa_rethrow /
// objc_exception_rethrow and aborts (SIGABRT).  The stock macOS crash report
// only shows the terminate path (abort -> -[NSApplication run]), NOT the
// exception's name/reason — so the actual defect is invisible.  This handler
// runs from _objc_terminate before abort() and dumps name + reason to stderr
// and to ~/klogg_nsexception.txt, making the real cause diagnosable.
//
// Pure C / objc-runtime only so this file stays a translation unit of C++.
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

struct NSException; // opaque ObjC object
extern "C" void NSSetUncaughtExceptionHandler( void ( *handler )( NSException* ) );

namespace {
id objcSend( id receiver, SEL sel )
{
    using SendId = id ( * )( id, SEL );
    return reinterpret_cast<SendId>( objc_msgSend )( receiver, sel );
}

const char* objcUtf8( id receiver, const char* selectorName )
{
    if ( receiver == nullptr ) {
        return "(null)";
    }
    const id value = objcSend( receiver, sel_registerName( selectorName ) );
    if ( value == nullptr ) {
        return "(null)";
    }
    using SendCstr = const char* ( * )( id, SEL );
    return reinterpret_cast<SendCstr>( objc_msgSend )( value, sel_registerName( "UTF8String" ) );
}

void writeAll( int fd, const char* text )
{
    if ( text != nullptr && *text != '\0' ) {
        const auto len = static_cast<size_t>( std::strlen( text ) );
        ssize_t written = 0;
        while ( static_cast<size_t>( written ) < len ) {
            const auto n = ::write( fd, text + written, len - static_cast<size_t>( written ) );
            if ( n <= 0 ) {
                break;
            }
            written += n;
        }
    }
}

void dumpException( int fd, NSException* exception )
{
    writeAll( fd, "\n===== KLOGG uncaught NSException =====\nname:   " );
    writeAll( fd, objcUtf8( reinterpret_cast<id>( exception ), "name" ) );
    writeAll( fd, "\nreason: " );
    writeAll( fd, objcUtf8( reinterpret_cast<id>( exception ), "reason" ) );
    writeAll( fd, "\n" );
}

void kloggUncaughtNSExceptionHandler( NSException* exception )
{
    dumpException( STDERR_FILENO, exception );

    if ( const auto home = std::getenv( "HOME" ) ) {
        char path[ 1024 ];
        const auto n = std::snprintf( path, sizeof( path ), "%s/klogg_nsexception.txt", home );
        if ( n > 0 && n < static_cast<int>( sizeof( path ) ) ) {
            const auto fd = ::open( path, O_WRONLY | O_CREAT | O_APPEND, 0644 );
            if ( fd >= 0 ) {
                dumpException( fd, exception );
                ::close( fd );
            }
        }
    }
}
} // namespace
#endif // Q_OS_MAC

#include <mimalloc.h>
#include <roaring.hh>

#ifdef KLOGG_HAS_VECTORSCAN
#include <hs.h>
#endif

#include "tbb/global_control.h"

#include "adblogcatsource.h"
#include "configuration.h"
#include "capturestore.h"
#include "logger.h"
#include "mainwindow.h"
#include "sessioninfo.h"
#include "styles.h"

#include "cli.h"
#include "kloggapp.h"

#ifdef KLOGG_PORTABLE
const bool PersistentInfo::ForcePortable = true;
#else
const bool PersistentInfo::ForcePortable = false;
#endif

void setApplicationAttributes( bool enableQtHdpi, int scaleFactorRounding )
{
    // When QNetworkAccessManager is instantiated it regularly starts polling
    // all network interfaces to see if anything changes and if so, what. This
    // creates a latency spike every 10 seconds on Mac OS 10.12+ and Windows 7 >=
    // when on a wifi connection.
    // So here we disable it for lack of better measure.
    // This will also cause this message: QObject::startTimer: Timers cannot
    // have negative intervals
    // For more info see:
    // - https://bugreports.qt.io/browse/QTBUG-40332
    // - https://bugreports.qt.io/browse/QTBUG-46015
    qputenv( "QT_BEARER_POLL_TIMEOUT", QByteArray::number( std::numeric_limits<int>::max() ) );

#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
#ifdef Q_OS_WIN
    QCoreApplication::setAttribute( Qt::AA_DisableWindowContextHelpButton );
#endif

    if ( !enableQtHdpi ) {
        QCoreApplication::setAttribute( Qt::AA_DisableHighDpiScaling );
    }
    else {

#if QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 )
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
            static_cast<Qt::HighDpiScaleFactorRoundingPolicy>( scaleFactorRounding ) );
#else
        Q_UNUSED( scaleFactorRounding );
#endif

        // This attribute must be set before QGuiApplication is constructed:
        QCoreApplication::setAttribute( Qt::AA_EnableHighDpiScaling );
        // We support high-dpi (aka Retina) displays
        QCoreApplication::setAttribute( Qt::AA_UseHighDpiPixmaps );
    }
#else
    Q_UNUSED( enableQtHdpi );
    Q_UNUSED( scaleFactorRounding );
#endif

#ifdef Q_OS_MAC
    QCoreApplication::setAttribute( Qt::AA_MacDontSwapCtrlAndMeta );
#endif

    QCoreApplication::setAttribute( Qt::AA_DontShowIconsInMenus );
}

QSet<QString> retainedAdbCaptureIds( const SessionInfo& sessionInfo )
{
    QSet<QString> captureIds;

    const auto windows = sessionInfo.windows();
    for ( const auto& windowId : windows ) {
        const auto openFiles = sessionInfo.openFiles( windowId );
        for ( const auto& openFile : openFiles ) {
            if ( !AdbLogcatSessionData::isPersistedSourceType( openFile.sourceType ) ) {
                continue;
            }

            const auto sessionData = AdbLogcatSessionData::fromJson( openFile.sourceSpec );
            if ( !sessionData.captureId.isEmpty() ) {
                captureIds.insert( sessionData.captureId );
            }
        }
    }

    return captureIds;
}

int main( int argc, char* argv[] )
{
#ifdef Q_OS_MAC
    // Install as early as possible: NSExceptions can fly during QApplication
    // construction and the very first event-loop spin.
    NSSetUncaughtExceptionHandler( &kloggUncaughtNSExceptionHandler );
#endif

#ifdef KLOGG_USE_MIMALLOC
    mi_process_init();
#endif

    const auto& config = Configuration::getSynced();
    setApplicationAttributes( config.enableQtHighDpi(), config.scaleFactorRounding() );

    KloggApp app( argc, argv );


    MainWindow::installLanguage( config.language() );
    CliParameters parameters( app );

    const auto logLevel
        = static_cast<logging::LogLevel>( std::max( parameters.log_level, config.loggingLevel() ) );
    logging::enableLogging( parameters.enable_logging || config.enableLogging(), logLevel );
    logging::enableFileLogging( parameters.log_to_file || config.enableLogging(), logLevel );

    app.initCrashHandler();

    auto maxConcurrency
        = tbb::global_control::active_value( tbb::global_control::max_allowed_parallelism );

    LOG_INFO << "Klogg instance"
             << ", mimalloc v" << mi_version()
             << ", default concurrency " << maxConcurrency;


    roaring_memory_t roaring_memory_allocators;
    roaring_memory_allocators.malloc = mi_malloc;
    roaring_memory_allocators.realloc = mi_realloc;
    roaring_memory_allocators.calloc = mi_calloc;
    roaring_memory_allocators.free = mi_free;
    roaring_memory_allocators.aligned_malloc = mi_aligned_alloc;
    roaring_memory_allocators.aligned_free = mi_free;
    roaring_init_memory_hook(roaring_memory_allocators);

#ifdef KLOGG_HAS_VECTORSCAN
    hs_set_allocator(mi_malloc, mi_free);
#endif

    if ( maxConcurrency < 2 ) {
        maxConcurrency = 2;
        LOG_INFO << "Overriding default concurrency to " << maxConcurrency;
        tbb::global_control concurrencyControl( tbb::global_control::max_allowed_parallelism,
                                                maxConcurrency );
        QThreadPool::globalInstance()->setMaxThreadCount( static_cast<int>( maxConcurrency ) );
    }

    bool trackSessionDirtyState = false;
    bool previousRunCrashed = false;

    if ( !parameters.multi_instance && app.isSecondary() ) {
        LOG_INFO << "Found another klogg, pid " << app.primaryPid();
        app.sendFilesToPrimaryInstance( parameters.filenames );
    }
    else {
        auto& sessionInfo = SessionInfo::getSynced();
        previousRunCrashed = sessionInfo.hadUncleanShutdown();
        sessionInfo.setDirtyShutdown( true );
        sessionInfo.save();
        trackSessionDirtyState = true;

        const auto shouldReloadSession
            = parameters.load_session
              || ( !parameters.new_session
                   && ( previousRunCrashed
                        || ( parameters.filenames.empty() && config.loadLastSession() ) ) );
        CaptureStore::cleanupUnusedCapturesAsync( retainedAdbCaptureIds( sessionInfo ) );

        // Apply theme based on theme mode
        StyleManager::applyStyle( config.style() );

        auto startNewSession = true;
        MainWindow* mw = nullptr;
        if ( shouldReloadSession ) {
            mw = app.reloadSession();
            startNewSession = false;
        }
        else {
            mw = app.newWindow();
            mw->reloadGeometry();
            mw->show();
        }

        if ( parameters.window_width > 0 && parameters.window_height > 0 ) {
            mw->resize( parameters.window_width, parameters.window_height );
        }

        for ( const auto& filename : parameters.filenames ) {
            mw->loadInitialFile( filename, parameters.follow_file );
        }

        if ( startNewSession ) {
            app.clearInactiveSessions();
        }

        app.startBackgroundTasks();
    }

    const auto exitCode = app.exec();

    if ( trackSessionDirtyState ) {
        auto& sessionInfo = SessionInfo::getSynced();
        sessionInfo.setDirtyShutdown( false );
        sessionInfo.save();
    }

    return exitCode;
}
