/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

namespace {
constexpr int StartTimeoutMs = 5000;
constexpr int RunTimeoutMs = 60000;
constexpr int CleanupTimeoutMs = 2000;
constexpr int UsageFailure = 64;
constexpr int StartFailure = 70;
constexpr int ChildCrash = 71;
constexpr int ChildTimeout = 72;
constexpr int CleanupFailure = 73;
} // namespace

int main( int argc, char* argv[] )
{
    QCoreApplication application( argc, argv );
    const auto arguments = QCoreApplication::arguments();
    if ( arguments.size() != 3 ) {
        return UsageFailure;
    }

    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert( QStringLiteral( "KLOGG_TEST_TRACE_SEARCH_STARTS" ), QStringLiteral( "" ) );
    QProcess child;
    child.setProcessEnvironment( environment );
    child.setWorkingDirectory( QDir::currentPath() );
    child.setProcessChannelMode( QProcess::ForwardedChannels );
    child.start( arguments.at( 1 ), { QStringLiteral( "-platform" ), QStringLiteral( "offscreen" ),
                                      arguments.at( 2 ) } );
    if ( !child.waitForStarted( StartTimeoutMs ) ) {
        if ( child.state() != QProcess::NotRunning ) {
            child.kill();
            if ( !child.waitForFinished( CleanupTimeoutMs )
                 && child.state() != QProcess::NotRunning ) {
                return CleanupFailure;
            }
        }
        return StartFailure;
    }

    // These waits also leave room for QProcess's bounded destructor fallback
    // inside the outer 120-second CTest deadline. Only this owned child is stopped.
    if ( child.state() != QProcess::NotRunning && !child.waitForFinished( RunTimeoutMs )
         && child.state() != QProcess::NotRunning ) {
        child.terminate();
        if ( !child.waitForFinished( CleanupTimeoutMs ) && child.state() != QProcess::NotRunning ) {
            child.kill();
            if ( !child.waitForFinished( CleanupTimeoutMs )
                 && child.state() != QProcess::NotRunning ) {
                return CleanupFailure;
            }
        }
        return ChildTimeout;
    }
    if ( child.exitStatus() != QProcess::NormalExit ) {
        return ChildCrash;
    }
    return child.exitCode();
}
