/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <catch2/catch.hpp>

#include <QtGlobal>

#if defined( __has_feature )
#  if __has_feature( address_sanitizer )
#    define KLOGG_TEST_ASAN 1
#  endif
#elif defined( __SANITIZE_ADDRESS__ )
#  define KLOGG_TEST_ASAN 1
#endif

#if defined( Q_OS_LINUX ) && defined( KLOGG_TEST_ASAN )

#  include <efsw/efsw.hpp>

#  include <atomic>
#  include <chrono>
#  include <future>
#  include <memory>

#  include <QDir>
#  include <QFile>
#  include <QFileInfo>
#  include <QTemporaryDir>

namespace {
class RemovingListener final : public efsw::FileWatchListener {
  public:
    explicit RemovingListener( efsw::FileWatcher& watcher ) : watcher_( watcher ) {}

    void handleFileAction( efsw::WatchID watchId, const std::string& dir,
                           const std::string&, efsw::Action action,
                           std::string ) override
    {
        if ( action == efsw::Actions::Add ) {
            watcher_.removeWatch( watchId );
            return;
        }

        if ( action == efsw::Actions::Modified && !completed_.exchange( true ) ) {
            observedDirectory_ = dir;
            completion_.set_value();
        }
    }

    std::future<void> completion()
    {
        return completion_.get_future();
    }

    const std::string& observedDirectory() const
    {
        return observedDirectory_;
    }

  private:
    efsw::FileWatcher& watcher_;
    std::promise<void> completion_;
    std::atomic_bool completed_{ false };
    std::string observedDirectory_;
};

class MoveOutRemovingListener final : public efsw::FileWatchListener {
  public:
    explicit MoveOutRemovingListener( efsw::FileWatcher& watcher ) : watcher_( watcher ) {}

    void handleFileAction( efsw::WatchID watchId, const std::string&, const std::string& filename,
                           efsw::Action action, std::string ) override
    {
        if ( action == efsw::Actions::Delete && !completed_.exchange( true ) ) {
            observedFile_ = filename;
            watcher_.removeWatch( watchId );
            completion_.set_value();
        }
    }

    std::future<void> completion()
    {
        return completion_.get_future();
    }

    const std::string& observedFile() const
    {
        return observedFile_;
    }

  private:
    efsw::FileWatcher& watcher_;
    std::promise<void> completion_;
    std::atomic_bool completed_{ false };
    std::string observedFile_;
};
} // namespace

TEST_CASE( "efsw inotify keeps a removed watcher alive through its in-flight action" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );

    const auto outsidePath = root.filePath( QStringLiteral( "outside-with-a-long-path-name" ) );
    const auto watchedPath = root.filePath( QStringLiteral( "watched-with-a-long-path-name" ) );
    REQUIRE( QDir{}.mkpath( outsidePath ) );
    REQUIRE( QDir{}.mkpath( watchedPath ) );

    const auto sourcePath = QDir( outsidePath ).filePath(
        QStringLiteral( "moved-file-with-a-name-long-enough-to-avoid-small-string-optimization.log" ) );
    QFile source( sourcePath );
    REQUIRE( source.open( QIODevice::WriteOnly ) );
    REQUIRE( source.write( "payload\n" ) > 0 );
    source.close();

    auto watcher = std::make_unique<efsw::FileWatcher>();
    RemovingListener listener( *watcher );
    auto completion = listener.completion();
    const auto watchId = watcher->addWatch( watchedPath.toStdString(), &listener, false );
    REQUIRE( watchId >= 0 );
    watcher->watch();

    const auto destinationPath = QDir( watchedPath ).filePath( QFileInfo( sourcePath ).fileName() );
    REQUIRE( QFile::rename( sourcePath, destinationPath ) );
    REQUIRE( completion.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready );
    CHECK_FALSE( listener.observedDirectory().empty() );

    // Joining efsw's native thread while the listener is still alive also
    // guarantees the callback has returned after publishing the promise.
    watcher.reset();
}

TEST_CASE( "efsw inotify safely removes a watch from a moved-out callback" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );

    const auto outsidePath = root.filePath( QStringLiteral( "outside" ) );
    const auto watchedPath = root.filePath( QStringLiteral( "watched" ) );
    REQUIRE( QDir{}.mkpath( outsidePath ) );
    REQUIRE( QDir{}.mkpath( watchedPath ) );

    const auto sourcePath = QDir( watchedPath ).filePath(
        QStringLiteral( "moved-out-file-with-a-long-name-for-asan.log" ) );
    QFile source( sourcePath );
    REQUIRE( source.open( QIODevice::WriteOnly ) );
    REQUIRE( source.write( "payload\n" ) > 0 );
    source.close();

    auto watcher = std::make_unique<efsw::FileWatcher>();
    MoveOutRemovingListener listener( *watcher );
    auto completion = listener.completion();
    const auto watchId = watcher->addWatch( watchedPath.toStdString(), &listener, false );
    REQUIRE( watchId >= 0 );
    watcher->watch();

    const auto destinationPath = QDir( outsidePath ).filePath( QFileInfo( sourcePath ).fileName() );
    REQUIRE( QFile::rename( sourcePath, destinationPath ) );
    REQUIRE( completion.wait_for( std::chrono::seconds( 5 ) ) == std::future_status::ready );
    CHECK( listener.observedFile() == QFileInfo( sourcePath ).fileName().toStdString() );

    watcher.reset();
}

#endif
