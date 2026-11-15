#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>
#include <QCoreApplication>

int main( int argc, char** argv )
{
    QCoreApplication application( argc, argv );
    return Catch::Session().run( argc, argv );
}
