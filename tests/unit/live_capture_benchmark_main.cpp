/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <QCoreApplication>

#include "capturestore.h"

int main( int argc, char* argv[] )
{
    QCoreApplication application( argc, argv );
    const auto result = Catch::Session().run( argc, argv );
    CaptureStore::shutdownBackgroundWorkers();
    return result;
}
