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

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <QApplication>
#include <QDir>

#include <logger.h>

#include "capturestore.h"
#include "configuration.h"
#include <persistentinfo.h>
#include "test_utils.h"

const bool PersistentInfo::ForcePortable = true;

namespace {
void configureTestTempDir()
{
    const auto tempDir = QDir::cleanPath( QDir::currentPath() + QDir::separator()
                                          + QLatin1String( "test_tmp" ) );

    QDir tempDirectory{ tempDir };
    const auto preservesParentTempDir
        = qEnvironmentVariableIsSet( "KLOGG_TEST_PRESERVE_TEMP_DIR" );
    if ( tempDirectory.exists() && !preservesParentTempDir ) {
        tempDirectory.removeRecursively();
    }

    QDir{}.mkpath( tempDir );

    const auto tempDirUtf8 = QDir::toNativeSeparators( tempDir ).toUtf8();
    qputenv( "TMP", tempDirUtf8 );
    qputenv( "TEMP", tempDirUtf8 );
    qputenv( "TMPDIR", tempDirUtf8 );
}
} // namespace

int main( int argc, char* argv[] )
{
    QApplication a( argc, argv );

    logging::enableLogging( true, logging::LogLevel::Warning );
    configureTestTempDir();

    auto& config = Configuration::getSynced();
    Q_UNUSED( config );
    configureProductLikeRegexpEngine( config );

    const auto result = Catch::Session().run( argc, argv );
    CaptureStore::shutdownBackgroundWorkers();
    return result;
}
