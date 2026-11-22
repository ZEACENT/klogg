#include "indexingsettings.h"

#include "configuration.h"

IndexingSettings IndexingSettings::capture()
{
    const auto& config = Configuration::get();
    return { config.defaultEncodingMib(), static_cast<std::size_t>( config.indexReadBufferSizeMb() ),
             config.useCompressedIndex(), config.fastModificationDetection() };
}
