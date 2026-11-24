#include "searchpreparation.h"
#include "joinedtbbproducer_p.h"

#include <utility>

SearchPreparation prepareFullSearch( FullSearchPreparationRequest request, AtomicFlag& cancelled )
{
    using Status = SearchPreparation::Status;
    if ( cancelled ) return { Status::Cancelled, {} };
    if ( !request.source || request.settings.readBufferSizeLines <= 0
         || request.settings.matchingThreads == 0 ) {
        return { Status::InvalidInput, {} };
    }
    const auto prepare = [&]() -> SearchPreparation {
        const auto endLine = request.endLine.value_or(
            LineNumber{ request.source->lineCount().get() } );
        if ( request.startLine > endLine ) return { Status::InvalidInput, {} };
        auto compiled = std::make_shared<RegularExpression>( request.pattern, request.settings.backend );
        if ( !compiled->isValid() ) return { Status::CompileFailure, {} };
        if ( cancelled ) return { Status::Cancelled, {} };

        SearchData data;
        FullSearchOperation operation{ request.source, cancelled, request.pattern,
            request.startLine, endLine, request.settings, compiled };
        operation.run( data );
        if ( operation.failed() ) return { Status::Failed, {} };
        if ( cancelled ) return { Status::Cancelled, {} };
        if ( !operation.completed() ) return { Status::Failed, {} };

        auto results = data.copyAllResults();
        auto completed = std::make_shared<const CompletedSearchResult>( CompletedSearchResult{
            std::move( results.newMatches ), results.maxLength, results.processedLines,
            std::move( request.source ), 0, 0, 0, std::move( compiled ) } );
        if ( cancelled ) return { Status::Cancelled, {} };
        return { Status::Ready, std::move( completed ) };
    };
    try {
        return klogg::runOnJoinedTbbProducer( prepare );
    } catch ( ... ) {
        // Native read/matcher failures and allocation failures are terminal, never
        // a successful prefix. All operation-local graph tasks have already drained.
        return { Status::Failed, {} };
    }
}
