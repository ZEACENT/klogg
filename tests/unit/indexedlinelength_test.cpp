#include <catch2/catch.hpp>
#include <cstdint>
#include <limits>

#include "indexedlinelength.h"

TEMPLATE_TEST_CASE( "Indexed line lengths saturate before narrowing or tab arithmetic overflows",
                    "[indexed-line-length]", std::int32_t, std::int64_t )
{
    using klogg::indexing::lineLength;
    using klogg::indexing::tabSpaces;
    constexpr auto maximum = std::numeric_limits<TestType>::max();
    if constexpr ( sizeof( TestType ) < sizeof( std::int64_t ) ) {
        const auto wider = static_cast<std::int64_t>( maximum ) + 1;
        CHECK( lineLength<TestType>( wider, 0, 1 ) == maximum );
        // Byte width is reduced before checking the native character limit.
        CHECK( lineLength<TestType>( wider, 0, 2 ) == wider / 2 );
    }
    CHECK( lineLength<TestType>( maximum, 1, 1 ) == maximum );
    CHECK( lineLength<TestType>( maximum, 0, 2 ) == maximum / 2 );
    CHECK( lineLength<TestType>( 4, 3, 1 ) == 7 );
    CHECK( tabSpaces<TestType>( maximum - 2, 7, 8 ) == maximum );
    CHECK( tabSpaces<TestType>( 0, 0, 8 ) == 7 );
    CHECK( tabSpaces<TestType>( 0, -1, 8 ) == 8 );
}
