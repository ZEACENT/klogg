#include "searchsettings.h"

#include <algorithm>
#include <tbb/info.h>

#include "configuration.h"

SearchSettings SearchSettings::capture()
{
    const auto& config = Configuration::get();
    const auto configuredThreads = config.searchThreadPoolSize();
    const auto threads = config.useParallelSearch()
        ? std::max( 1, configuredThreads == 0 ? tbb::info::default_concurrency() : configuredThreads )
        : 1;
    return { std::max( 1, config.searchReadBufferSizeLines() ),
             static_cast<uint32_t>( threads ), config.useBlockScan(), config.regexpEngine() };
}
